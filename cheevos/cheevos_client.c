/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2019-2021 - Brian Weiss
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Found- ation, either version 3 of the License, or (at your option) any later
 * version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with
 * RetroArch. If not, see <http://www.gnu.org/licenses/>.
 */

#include "cheevos_client.h"

#include "cheevos.h"
#include "cheevos_cache.h"
#include "cheevos_cache_data.h"

#include "../configuration.h"
#include "../file_path_special.h"
#include "../paths.h"
#include "../retroarch.h"
#include "../version.h"

#include <features/features_cpu.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <time.h>

#include "../frontend/frontend_driver.h"
#include "../network/net_http_special.h"
#include "../tasks/tasks_internal.h"

#include "../configuration.h"

#ifdef HAVE_PRESENCE
#include "../network/presence.h"
#endif

#include "../deps/rcheevos/include/rc_api_runtime.h"
#include "../deps/rcheevos/include/rc_api_user.h"

/* Define this macro to log URLs. */
#undef CHEEVOS_LOG_URLS

/* Define this macro to have the password and token logged.
 * THIS WILL DISCLOSE THE USER'S PASSWORD, TAKE CARE! */
#undef CHEEVOS_LOG_PASSWORD

/* Define this macro to load a JSON file from disk instead of downloading
 * from retroachievements.org. */
#undef CHEEVOS_JSON_OVERRIDE

/* Define this macro with a string to save the JSON file to disk with
 * that name. */
#undef CHEEVOS_SAVE_JSON

/* Define this macro to log downloaded badge images. */
#undef CHEEVOS_LOG_BADGES

/* Number of usecs to wait between posting rich presence to the site. */
/* Keep consistent with SERVER_PING_FREQUENCY from RAIntegration. */
#define CHEEVOS_PING_FREQUENCY 2 * 60 * 1000000

/* Number of usecs to wait between posting rich presence to the site. */
/* Keep consistent with SERVER_PING_FREQUENCY from RAIntegration. */
#define CHEEVOS_NETWORK_POLL_FREQUENCY 60 * 1000000

#define CHEEVOS_CACHE_EXPIRATION_SEC (60 * 100000)
#define CHEEVOS_UNLOCKS_CACHE_EXPIRATION_SEC (60 * 5)

/****************************
 * data types               *
 ****************************/

enum rcheevos_async_io_type
{
   CHEEVOS_ASYNC_RICHPRESENCE = 0,
   CHEEVOS_ASYNC_AWARD_ACHIEVEMENT,
   CHEEVOS_ASYNC_SUBMIT_LBOARD,
   CHEEVOS_ASYNC_LOGIN,
   CHEEVOS_ASYNC_RESOLVE_HASH,
   CHEEVOS_ASYNC_FETCH_GAME_DATA,
   CHEEVOS_ASYNC_FETCH_USER_UNLOCKS,
   CHEEVOS_ASYNC_FETCH_HARDCORE_USER_UNLOCKS,
   CHEEVOS_ASYNC_START_SESSION,
   CHEEVOS_ASYNC_FETCH_BADGE,
   CHEEVOS_ASYNC_NETWORK_POLL,
};

struct rcheevos_async_io_request;

typedef void (*rcheevos_async_handler)(struct rcheevos_async_io_request *request,
                                       http_transfer_data_t *data, char buffer[],
                                       size_t buffer_size,
                                       void *handler_data);

typedef struct rcheevos_async_io_request
{
   rc_api_request_t request;
   // This is the function that processes the HTTP response
   rcheevos_async_handler handler;
   void *handler_data;
   int id;
   // This is called at the very end of the handler.
   // (in rcheevos_async_end_request)
   rcheevos_client_callback callback;
   void *callback_data;
   int attempt_count;
   const char *success_message;
   const char *failure_message;
   const char *user_agent;
   char type;
} rcheevos_async_io_request;

#ifdef HAVE_THREADS
#define RCHEEVOS_CONCURRENT_BADGE_DOWNLOADS 2
#else
#define RCHEEVOS_CONCURRENT_BADGE_DOWNLOADS 1
#endif

typedef struct rcheevos_fetch_badge_state
{
   unsigned badge_fetch_index;
   unsigned locked_badge_fetch_index;
   const char *badge_directory;
   rcheevos_client_callback callback;
   void *callback_data;
   char requested_badges[RCHEEVOS_CONCURRENT_BADGE_DOWNLOADS][32];
} rcheevos_fetch_badge_state;

typedef struct rcheevos_fetch_badge_data
{
   rcheevos_fetch_badge_state *state;
   int request_index;
   void *data;
   size_t data_len;
   rcheevos_client_callback callback;
} rcheevos_fetch_badge_data;

/****************************
 * forward declarations     *
 ****************************/

static retro_time_t rcheevos_client_prepare_ping(rcheevos_async_io_request *request);

static void rcheevos_async_http_task_callback(retro_task_t *task, void *task_data, void *user_data,
                                              const char *error);

static void rcheevos_async_end_request(rcheevos_async_io_request *request);

static void rcheevos_async_fetch_badge_callback(struct rcheevos_async_io_request *request,
                                                http_transfer_data_t *data, char buffer[],
                                                size_t buffer_size, void *handler_data);

/****************************
 * user agent construction  *
 ****************************/

static int append_no_spaces(char *buffer, char *stop, const char *text)
{
   char *ptr = buffer;

   while (ptr < stop && *text)
   {
      if (*text == ' ')
      {
         *ptr++ = '_';
         ++text;
      }
      else
         *ptr++ = *text++;
   }

   *ptr = '\0';
   return (int) (ptr - buffer);
}

void rcheevos_get_user_agent(rcheevos_locals_t *locals, char *buffer, size_t len)
{
   char *ptr;
   struct retro_system_info *system = &runloop_state_get_ptr()->system.info;

   /* if we haven't calculated the non-changing portion yet, do so now
   * [retroarch version + os version] */
   if (!locals->user_agent_prefix[0])
   {
      const frontend_ctx_driver_t *frontend = frontend_get_ptr();
      int major, minor;
      char tmp[64];

      if (frontend && frontend->get_os)
      {
         frontend->get_os(tmp, sizeof(tmp), &major, &minor);
         snprintf(locals->user_agent_prefix, sizeof(locals->user_agent_prefix),
                  "RetroArch/%s (%s %d.%d)", PACKAGE_VERSION, tmp, major, minor);
      }
      else
         snprintf(locals->user_agent_prefix, sizeof(locals->user_agent_prefix), "RetroArch/%s",
                  PACKAGE_VERSION);
   }

   /* append the non-changing portion */
   ptr = buffer + strlcpy(buffer, locals->user_agent_prefix, len);

   /* if a core is loaded, append its information */
   if (system && !string_is_empty(system->library_name))
   {
      char *stop = buffer + len - 1;
      const char *path = path_get(RARCH_PATH_CORE);
      *ptr++ = ' ';

      if (!string_is_empty(path))
      {
         append_no_spaces(ptr, stop, path_basename(path));
         path_remove_extension(ptr);
         ptr += strlen(ptr);
      }
      else
         ptr += append_no_spaces(ptr, stop, system->library_name);

      if (system->library_version)
      {
         *ptr++ = '/';
         ptr += append_no_spaces(ptr, stop, system->library_version);
      }
   }

   *ptr = '\0';
}

#ifdef CHEEVOS_LOG_URLS
#ifndef CHEEVOS_LOG_PASSWORD
static void rcheevos_filter_url_param(char *url, char *param)
{
   char *next;
   size_t param_len = strlen(param);
   char *start = strchr(url, '?');
   if (!start)
      start = url;
   else
      ++start;

   do
   {
      next = strchr(start, '&');

      if (start[param_len] == '=' && memcmp(start, param, param_len) == 0)
      {
         if (next)
            strcpy_literal(start, next + 1);
         else if (start > url)
            start[-1] = '\0';
         else
            *start = '\0';

         return;
      }

      if (!next)
         return;

      start = next + 1;
   } while (1);
}
#endif
#endif

void rcheevos_log_url(const char *api, const char *url)
{
#ifdef CHEEVOS_LOG_URLS
#ifdef CHEEVOS_LOG_PASSWORD
   CHEEVOS_LOG(RCHEEVOS_TAG "GET %s\n", url);
#else
   char copy[256];
   strlcpy(copy, url, sizeof(copy));
   rcheevos_filter_url_param(copy, "p");
   rcheevos_filter_url_param(copy, "t");
   CHEEVOS_LOG(RCHEEVOS_TAG "GET %s\n", copy);
#endif
#else
   (void) api;
   (void) url;
#endif
}

static void rcheevos_log_post_url(const char *url, const char *post)
{
#ifdef CHEEVOS_LOG_URLS
#ifdef CHEEVOS_LOG_PASSWORD
   if (post && post[0])
      CHEEVOS_LOG(RCHEEVOS_TAG "POST %s %s\n", url, post);
   else
      CHEEVOS_LOG(RCHEEVOS_TAG "POST %s\n", url);
#else
   if (post && post[0])
   {
      char post_copy[2048];
      strlcpy(post_copy, post, sizeof(post_copy));
      rcheevos_filter_url_param(post_copy, "p");
      rcheevos_filter_url_param(post_copy, "t");

      if (post_copy[0])
         CHEEVOS_LOG(RCHEEVOS_TAG "POST %s %s\n", url, post_copy);
      else
         CHEEVOS_LOG(RCHEEVOS_TAG "POST %s\n", url);
   }
   else
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "POST %s\n", url);
   }
#endif
#else
   (void) url;
   (void) post;
#endif
}

/****************************
 * dispatch                 *
 ****************************/

static void rcheevos_async_begin_http_request(rcheevos_async_io_request *request)
{
   if (request->request.post_data)
      task_push_http_post_transfer_with_user_agent(request->request.url, request->request.post_data,
                                                   true, "POST", request->user_agent,
                                                   rcheevos_async_http_task_callback, request);
   else
      task_push_http_transfer_with_user_agent(request->request.url, true, "GET",
                                              request->user_agent,
                                              rcheevos_async_http_task_callback, request);
}

static void rcheevos_async_retry_request(retro_task_t *task)
{
   rcheevos_async_io_request *request = (rcheevos_async_io_request *) task->user_data;

   /* the timer task has done its job. let it dispose itself */
   task_set_finished(task, 1);

   /* start a new task for the HTTP call */
   rcheevos_async_begin_http_request(request);
}

static void rcheevos_async_retry_request_after_delay(rcheevos_async_io_request *request,
                                                     const char *error)
{
   retro_task_t *task = task_init();
   /* Double the wait between each attempt until we hit
   * a maximum delay of two minutes.
   * 250ms -> 500ms -> 1s -> 2s -> 4s -> 8s -> 16s -> 32s -> 64s -> 120s ->
   * 120s... */
   retro_time_t retry_delay =
      (request->attempt_count > 8) ? (120 * 1000 * 1000) : ((250 * 1000) << request->attempt_count);

   CHEEVOS_ERR(RCHEEVOS_TAG "%s %u: %s (automatic retry in %dms)\n", request->failure_message,
               request->id, error, (int) retry_delay / 1000);

   task->when = cpu_features_get_time_usec() + retry_delay;
   task->handler = rcheevos_async_retry_request;
   task->user_data = request;
   task->progress = -1;

   ++request->attempt_count;
   task_queue_push(task);
}

static bool rcheevos_async_request_failed(rcheevos_async_io_request *request, const char *error)
{
   /* always retry any request once (attempt_count==0) in case of network hiccup
   */
   if (request->attempt_count > 0)
   {
      /* retry failed, don't retry these requests */
      switch (request->type)
      {
         /* timer will ping again */
         case CHEEVOS_ASYNC_RICHPRESENCE:
         /* fallback to the placeholder image */
         case CHEEVOS_ASYNC_FETCH_BADGE:
            return false;

         case CHEEVOS_ASYNC_RESOLVE_HASH:
         case CHEEVOS_ASYNC_LOGIN:
            /* make a maximum of four attempts
         (0ms -> 250ms -> 500ms -> 1s) */
            if (request->attempt_count == 3)
               return false;
            break;

         default:
            break;
      }
   }

   /* automatically retry the request */
   rcheevos_async_retry_request_after_delay(request, error);
   return true;
}

static void rcheevos_async_http_task_callback(retro_task_t *task, void *task_data, void *user_data,
                                              const char *error)
{
   rcheevos_async_io_request *request = (rcheevos_async_io_request *) user_data;
   http_transfer_data_t *data = (http_transfer_data_t *) task_data;
   const bool aborted = rcheevos_load_aborted();
   char buffer[224];

   if (aborted)
   {
      /* load was aborted. don't process the response */
      strlcpy(buffer, "Load aborted", sizeof(buffer));
   }
   else if (error)
   {
      /* there was a communication error */
      /* if automatically requeued, don't process any further */
      if (rcheevos_async_request_failed(request, error))
         return;

      strlcpy(buffer, error, sizeof(buffer));
   }
   else if (!data)
   {
      /* Server did not return HTTP headers */
      strlcpy(buffer, "Server communication error", sizeof(buffer));
   }
   else if (!data->data || !data->len)
   {
      if (data->status <= 0)
      {
         /* something occurred which prevented the response from being processed.
       * assume the server request hasn't happened and try again. */
         snprintf(buffer, sizeof(buffer), "task status code %d", data->status);
         rcheevos_async_request_failed(request, buffer);
         return;
      }

      if (data->status != 200) /* Server returned error via status code. */
      {
         snprintf(buffer, sizeof(buffer), "HTTP error code %d", data->status);

         if (request->type == CHEEVOS_ASYNC_FETCH_BADGE)
         {
            /* This isn't a JSON request. An empty response with a status code is a
         * valid response. */
            if (request->handler)
               request->handler(request, data, buffer, sizeof(buffer), request->handler_data);
         }
      }
      else /* Server sent empty response without error status code */
         strlcpy(buffer, "No response from server", sizeof(buffer));
   }
   else
   {
      /* indicate success unless handler provides error */
      buffer[0] = '\0';

      /* Call appropriate handler to process the response */
      /* NOTE: data->data is not null-terminated. Most handlers assume the
     * response is properly formatted or will encounter a parse failure
     * before reading past the end of the data */
      if (request->handler)
         request->handler(request, data, buffer, sizeof(buffer), request->handler_data);
   }

   if (!buffer[0])
   {
      /* success */
      if (request->success_message)
      {
         if (request->id)
            CHEEVOS_LOG(RCHEEVOS_TAG "%s %u\n", request->success_message, request->id);
         else
            CHEEVOS_LOG(RCHEEVOS_TAG "%s\n", request->success_message);
      }
   }
   else
   {
      /* encountered an error */
      char errbuf[256];
      if (request->id)
         snprintf(errbuf, sizeof(errbuf), "%s %u: %s", request->failure_message, request->id,
                  buffer);
      else
         snprintf(errbuf, sizeof(errbuf), "%s: %s", request->failure_message, buffer);

      if (!aborted)
      {
         switch (request->type)
         {
            case CHEEVOS_ASYNC_RICHPRESENCE:
            case CHEEVOS_ASYNC_FETCH_BADGE:
               /* Don't bother informing user when these fail */
               break;

            case CHEEVOS_ASYNC_LOGIN:
            case CHEEVOS_ASYNC_RESOLVE_HASH:
               if (error)
               {
                  rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
                  size_t len = 0;
                  char *ptr;

                  if (rcheevos_locals->load_info.state == RCHEEVOS_LOAD_STATE_NETWORK_ERROR)
                     break;

                  rcheevos_locals->load_info.state = RCHEEVOS_LOAD_STATE_NETWORK_ERROR;

                  while (
                     /* find the first single slash */
                     request->request.url[len] != '/' || request->request.url[len + 1] == '/' ||
                     request->request.url[len - 1] == '/')
                     ++len;

                  ptr = errbuf + snprintf(errbuf, sizeof(errbuf), "Could not communicate with ");
                  memcpy(ptr, request->request.url, len);
                  ptr[len] = '\0';
               }
               /* fallthrough to default */

            default:
               runloop_msg_queue_push(errbuf, 0, 5 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT,
                                      MESSAGE_QUEUE_CATEGORY_ERROR);
               break;
         }
      }

      CHEEVOS_LOG(RCHEEVOS_TAG "%s\n", errbuf);
   }

   rcheevos_async_end_request(request);
}

static void rcheevos_async_end_request(rcheevos_async_io_request *request)
{
   CHEEVOS_LOG(RCHEEVOS_TAG "rcheevos_async_end_request\n");
   rc_api_destroy_request(&request->request);

   if (request->callback && !rcheevos_load_aborted())
      request->callback(request->callback_data);

   /* rich presence request will be reused on next ping - reset the attempt
   * counter. for all other request types, free the request object */
   if (request->type == CHEEVOS_ASYNC_RICHPRESENCE)
      request->attempt_count = 0;
   else
      free(request);
}

static void rcheevos_async_begin_request(rcheevos_async_io_request *request, int init_result,
                                         rcheevos_async_handler handler, void *handler_data, char type, int id,
                                         const char *success_message, const char *failure_message)
{
   if (init_result != RC_OK)
   {
      char errbuf[256];
      if (id)
         snprintf(errbuf, sizeof(errbuf), "%s %u: %s", failure_message, id,
                  rc_error_str(init_result));
      else
         snprintf(errbuf, sizeof(errbuf), "%s: %s", failure_message, rc_error_str(init_result));

      CHEEVOS_LOG(RCHEEVOS_TAG "%s\n", errbuf);
      runloop_msg_queue_push(errbuf, 0, 5 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT,
                             MESSAGE_QUEUE_CATEGORY_ERROR);

      rcheevos_async_end_request(request);
      return;
   }

   request->handler = handler;
   request->handler_data = handler_data;
   request->type = type;
   request->id = id;
   request->success_message = success_message;
   request->failure_message = failure_message;
   request->attempt_count = 0;

   if (!request->user_agent)
      request->user_agent = get_rcheevos_locals()->user_agent_core;

   rcheevos_log_post_url(request->request.url, request->request.post_data);
   rcheevos_async_begin_http_request(request);
}

static bool rcheevos_async_succeeded(int result, const rc_api_response_t *response, char buffer[],
                                     size_t buffer_size)
{
   if (result != RC_OK)
   {
      strlcpy(buffer, rc_error_str(result), buffer_size);
      return false;
   }

   if (!response->succeeded)
   {
      strlcpy(buffer, response->error_message, buffer_size);
      return false;
   }

   return true;
}

void rcheevos_client_initialize(void)
{
   const settings_t *settings = config_get_ptr();
   const char *host = settings->arrays.cheevos_custom_host;
   if (!host[0])
   {
#ifdef HAVE_SSL
      //host = "https://peppino.usuraio.org";
      host = "https://retroachievements.org";
#else
      host = "http://peppino.usuraio.org";
      //host = "http://retroachievements.org";
#endif
   }

   CHEEVOS_LOG(RCHEEVOS_TAG "Using host: %s\n", host);
   if (!string_is_equal(host, "https://retroachievements.org"))
   {
      rc_api_set_host(host);

      if (!string_is_equal(host, "http://retroachievements.org"))
         rc_api_set_image_host(host);
   }
}

/****************************
 * login                    *
 ****************************/
typedef struct rcheevos_client_login_callback_data_t
{
   rcheevos_client_callback callback;
   void *cb_data;
   char *username;
   bool login_success;
} rcheevos_client_login_callback_data_t;


static void rcheevos_async_login_callback(struct rcheevos_async_io_request *request,
                                          http_transfer_data_t *data, char buffer[],
                                          size_t buffer_size, void *handler_data)
{
   rcheevos_client_login_callback_data_t *cb_data = (rcheevos_client_login_callback_data_t *) handler_data;
   rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
   rc_api_login_response_t api_response;

   int result = rc_api_process_login_response(&api_response, data->data);
   if (rcheevos_async_succeeded(result, &api_response.response, buffer, buffer_size))
   {
      /* save the token to the config and clear the password on success */
      settings_t *settings = config_get_ptr();
      strlcpy(settings->arrays.cheevos_token, api_response.api_token,
              sizeof(settings->arrays.cheevos_token));
      settings->arrays.cheevos_password[0] = '\0';

      CHEEVOS_LOG(RCHEEVOS_TAG "%s logged in successfully\n", api_response.display_name);
      strlcpy(rcheevos_locals->displayname, api_response.display_name,
              sizeof(rcheevos_locals->displayname));
      strlcpy(rcheevos_locals->username, api_response.username, sizeof(rcheevos_locals->username));
      strlcpy(rcheevos_locals->token, api_response.api_token, sizeof(rcheevos_locals->token));
      cb_data->login_success = true;
   }
   else
   {
      cb_data->login_success = false;
   }
   rc_api_destroy_login_response(&api_response);
}

static void rcheevos_client_login_callback(void *userdata)
{
   rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
   rcheevos_client_login_callback_data_t *data = (rcheevos_client_login_callback_data_t *) userdata;
   if (!data->login_success)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed login for `%s`. Achievements won't be synced with Retroachievements.\n", data->username);
      strlcpy(rcheevos_locals->displayname, data->username,
              sizeof(rcheevos_locals->displayname));
      strlcpy(rcheevos_locals->username, data->username, sizeof(rcheevos_locals->username));
      rcheevos_locals->token[0] = '\0';
      rcheevos_locals->logged_in = false;
   }
   else
   {
      rcheevos_locals->logged_in = true;
   }
   if (data->callback)
   {
      data->callback(data->cb_data);
   }
   free(userdata);
}

static void rcheevos_client_login(const char *username, const char *password, const char *token,
                                  rcheevos_client_callback callback, void *userdata)
{
   rcheevos_async_io_request *request =
      (rcheevos_async_io_request *) calloc(1, sizeof(rcheevos_async_io_request));
   rcheevos_client_login_callback_data_t *cb_data =
      (rcheevos_client_login_callback_data_t *) calloc(1, sizeof(*cb_data));
   cb_data->callback = callback;
   cb_data->cb_data = userdata;
   cb_data->username = strdup(username);
   if (!request)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to allocate login request\n");
   }
   else
   {
      rc_api_login_request_t api_params;
      int result;

      memset(&api_params, 0, sizeof(api_params));
      api_params.username = username;
      api_params.password = password;
      api_params.api_token = token;

      result = rc_api_init_login_request(&request->request, &api_params);

      request->callback = rcheevos_client_login_callback;
      request->callback_data = cb_data;

      rcheevos_async_begin_request(request, result, rcheevos_async_login_callback, cb_data,
                                   CHEEVOS_ASYNC_LOGIN, 0, NULL, "Error logging in");
   }
}

void rcheevos_client_login_with_password(const char *username, const char *password,
                                         rcheevos_client_callback callback, void *userdata)
{
   rcheevos_client_login(username, password, NULL, callback, userdata);
}

void rcheevos_client_login_with_token(const char *username, const char *token,
                                      rcheevos_client_callback callback, void *userdata)
{
   rcheevos_client_login(username, NULL, token, callback, userdata);
}

/****************************
 * identify game            *
 ****************************/

static void rcheevos_resolve_hash_succeeded(unsigned int game_id)
{
   rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
   rcheevos_locals->game.id = game_id;
}

static void rcheevos_async_resolve_hash_callback(struct rcheevos_async_io_request *request,
                                                 http_transfer_data_t *data, char buffer[],
                                                 size_t buffer_size, void *handler_data)
{
   rc_api_resolve_hash_response_t api_response;
   int result = rc_api_process_resolve_hash_response(&api_response, data->data);

   if (rcheevos_async_succeeded(result, &api_response.response, buffer, buffer_size))
   {
      rcheevos_cache_hash_t cache_data;
      char *hash = (char *) handler_data;
      cache_data.game_id = api_response.game_id;
      if (rcheevos_cache_save_hash_mapping(hash, &cache_data))
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Cached hash `%s` -> game id mapping for game: %u\n", hash, api_response.game_id);
      }
      else
      {
         CHEEVOS_ERR(RCHEEVOS_TAG "Failed to save hash `%s` -> game id mapping for game: %u\n", hash, api_response.game_id);
      }
      rcheevos_resolve_hash_succeeded(api_response.game_id);
   }

   free(handler_data);
   rc_api_destroy_resolve_hash_response(&api_response);
}

static void rcheevos_client_identify_game_remote(const char *hash,
                                                 rcheevos_client_callback callback, void *userdata)
{
   rcheevos_async_io_request *request =
      (rcheevos_async_io_request *) calloc(1, sizeof(rcheevos_async_io_request));
   if (!request)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to allocate game identification request\n");
   }
   else
   {
      rc_api_resolve_hash_request_t api_params;
      int result;

      memset(&api_params, 0, sizeof(api_params));
      api_params.game_hash = hash;

      result = rc_api_init_resolve_hash_request(&request->request, &api_params);

      request->callback = callback;
      request->callback_data = userdata;

      rcheevos_async_begin_request(request, result, rcheevos_async_resolve_hash_callback, strdup(hash),
                                   CHEEVOS_ASYNC_RESOLVE_HASH, 0, NULL, "Error resolving hash");
   }
}

static bool rcheevos_client_identify_game_cached(const char *hash,
                                                 rcheevos_client_callback callback, void *userdata)
{
   rcheevos_cache_hash_t cached;
   if (!rcheevos_cache_get_game_id_for_hash(hash, &cached))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Game hash `%s` not found in game ID cache.\n", hash);
      return false;
   }
   CHEEVOS_LOG(RCHEEVOS_TAG "Game hash `%s` was cached: game ID %u.\n", hash, cached.game_id);
   rcheevos_resolve_hash_succeeded(cached.game_id);
   if (callback)
   {
      callback(userdata);
   }
   return true;
}

void rcheevos_client_identify_game(const char *hash, rcheevos_client_callback callback,
                                   void *userdata)
{
   if (rcheevos_client_identify_game_cached(hash, callback, userdata))
   {
      return;
   }
   rcheevos_client_identify_game_remote(hash, callback, userdata);
}


/****************************
 * initialize runtime       *
 ****************************/
enum rcheevos_async_fetch_status_t
{
   CHEEVOS_ASYNC_STATUS_PENDING = 0,
   CHEEVOS_ASYNC_STATUS_SUCCESS = 1,
   CHEEVOS_ASYNC_STATUS_FAILED = 1,
};


typedef struct rcheevos_async_initialize_runtime_data_t
{
   rc_api_fetch_game_data_response_t game_data;
   rc_api_fetch_user_unlocks_response_t hardcore_unlocks;
   rc_api_fetch_user_unlocks_response_t non_hardcore_unlocks;

   time_t game_data_cache_time;
   time_t unlocks_cache_time[2];

   rcheevos_client_callback callback;
   void *callback_data;

   enum rcheevos_async_fetch_status_t game_data_fetch_status;
   enum rcheevos_async_fetch_status_t unlock_fetch_status[2];
   bool badge_fetch_done;
   char *username;
} rcheevos_async_initialize_runtime_data_t;

static void rcheevos_async_initialize_runtime_data_free(rcheevos_async_initialize_runtime_data_t *data)
{
   if (!data)
   {
      return;
   }
   free(data->username);
}

static void
rcheevos_client_copy_achievements(rcheevos_async_initialize_runtime_data_t *runtime_data)
{
   unsigned i, j;
   const rc_api_achievement_definition_t *definition;
   rcheevos_racheevo_t *achievement;
   rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
   const settings_t *settings = config_get_ptr();

   rcheevos_locals->game.achievements = (rcheevos_racheevo_t *) calloc(
      runtime_data->game_data.num_achievements, sizeof(rcheevos_racheevo_t));

   achievement = rcheevos_locals->game.achievements;
   if (!achievement)
   {
      CHEEVOS_ERR(RCHEEVOS_TAG "Could not allocate achievements\n");
      return;
   }

   definition = runtime_data->game_data.achievements;
   CHEEVOS_LOG(RCHEEVOS_TAG "Copying %d achievements\n", runtime_data->game_data.num_achievements);
   for (i = 0; i < runtime_data->game_data.num_achievements; ++i, ++definition)
   {
      /* invalid definition, ignore */
      if (definition->category == 0 || !definition->definition || !definition->definition[0] ||
          !definition->title || !definition->title[0] || !definition->description ||
          !definition->description[0])
         continue;

      if (definition->category != 3)
      {
         if (!settings->bools.cheevos_test_unofficial)
            continue;

         achievement->active =
            RCHEEVOS_ACTIVE_UNOFFICIAL | RCHEEVOS_ACTIVE_SOFTCORE | RCHEEVOS_ACTIVE_HARDCORE;
      }
      else
      {
         achievement->active = RCHEEVOS_ACTIVE_SOFTCORE | RCHEEVOS_ACTIVE_HARDCORE;

         for (j = 0; j < runtime_data->hardcore_unlocks.num_achievement_ids; ++j)
         {
            if (runtime_data->hardcore_unlocks.achievement_ids[j] == definition->id)
            {
               achievement->active &= ~(RCHEEVOS_ACTIVE_HARDCORE | RCHEEVOS_ACTIVE_SOFTCORE);
               break;
            }
         }

         if ((achievement->active & RCHEEVOS_ACTIVE_SOFTCORE) != 0)
         {
            for (j = 0; j < runtime_data->non_hardcore_unlocks.num_achievement_ids; ++j)
            {
               if (runtime_data->non_hardcore_unlocks.achievement_ids[j] == definition->id)
               {
                  achievement->active &= ~RCHEEVOS_ACTIVE_SOFTCORE;
                  break;
               }
            }
         }
      }

      achievement->id = definition->id;
      achievement->title = strdup(definition->title);
      achievement->description = strdup(definition->description);
      achievement->badge = strdup(definition->badge_name);
      achievement->points = definition->points;

      /* If an achievement has been fully unlocked,
     * we don't need to keep the definition around
     * as it won't be reactivated. Otherwise,
     * we do have to keep a copy of it. */
      if ((achievement->active & (RCHEEVOS_ACTIVE_HARDCORE | RCHEEVOS_ACTIVE_SOFTCORE)) != 0)
      {
         //CHEEVOS_LOG(RCHEEVOS_TAG "Adding achievement: %s (definition: `%s`)\n", definition->title, definition->definition);
         achievement->memaddr = strdup(definition->definition);
      }

      ++achievement;
   }

   rcheevos_locals->game.achievement_count = achievement - rcheevos_locals->game.achievements;
   CHEEVOS_LOG(RCHEEVOS_TAG "Total achievements: %d\n", rcheevos_locals->game.achievement_count);
}

static void
rcheevos_client_copy_leaderboards(rcheevos_async_initialize_runtime_data_t *runtime_data)
{
   unsigned i;
   rcheevos_ralboard_t *leaderboard;
   const rc_api_leaderboard_definition_t *definition;
   rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();

   rcheevos_locals->game.leaderboards = (rcheevos_ralboard_t *) calloc(
      runtime_data->game_data.num_leaderboards, sizeof(rcheevos_ralboard_t));
   rcheevos_locals->game.leaderboard_count = runtime_data->game_data.num_leaderboards;

   leaderboard = rcheevos_locals->game.leaderboards;
   if (!leaderboard)
   {
      CHEEVOS_ERR(RCHEEVOS_TAG "Could not allocate leaderboards\n");
      return;
   }

   definition = runtime_data->game_data.leaderboards;
   CHEEVOS_LOG(RCHEEVOS_TAG "Copying %d leaderboards\n", runtime_data->game_data.num_leaderboards);
   for (i = 0; i < runtime_data->game_data.num_leaderboards; ++i, ++definition, ++leaderboard)
   {
      leaderboard->id = definition->id;
      leaderboard->title = strdup(definition->title);
      leaderboard->description = strdup(definition->description);
      leaderboard->mem = strdup(definition->definition);
      leaderboard->format = definition->format;
   }
}

static void rcheevos_client_initialize_runtime_rich_presence(
   rcheevos_async_initialize_runtime_data_t *runtime_data)
{
   if (runtime_data->game_data.rich_presence_script &&
       *runtime_data->game_data.rich_presence_script)
   {
      rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();

      /* Just activate the rich presence script now.
     * It can't be toggled on or off,
     * so there's no reason to keep the unparsed version
     * around any longer than necessary, and we can avoid
     * making a copy in the process. */
      int result = rc_runtime_activate_richpresence(
         &rcheevos_locals->runtime, runtime_data->game_data.rich_presence_script, NULL, 0);

      if (result != RC_OK)
      {
         const settings_t *settings = config_get_ptr();
         char buffer[256];
         snprintf(buffer, sizeof(buffer), "Could not activate rich presence: %s",
                  rc_error_str(result));

         if (settings->bools.cheevos_verbose_enable)
            runloop_msg_queue_push(buffer, 0, 4 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT,
                                   MESSAGE_QUEUE_CATEGORY_INFO);

         CHEEVOS_ERR(RCHEEVOS_TAG "%s\n", buffer);
      }
   }
}

static void rcheevos_client_game_data_to_cache(
   const rc_api_fetch_game_data_response_t *runtime_data,
   rcheevos_cache_game_t *cached_game_data)
{
   rcheevos_cache_achievement_t *achievements;
   rcheevos_cache_leaderboard_t *leaderboards;

   /* Convert achievements */
   achievements = calloc(runtime_data->num_achievements, sizeof(*achievements));
   for (int i = 0; i < runtime_data->num_achievements; ++i)
   {
      achievements[i].author = strdup(runtime_data->achievements[i].author);
      achievements[i].badge_name = strdup(runtime_data->achievements[i].badge_name);
      achievements[i].category = runtime_data->achievements[i].category;
      achievements[i].created = runtime_data->achievements[i].created;
      achievements[i].definition = strdup(runtime_data->achievements[i].definition);
      achievements[i].id = runtime_data->achievements[i].id;
      achievements[i].points = runtime_data->achievements[i].points;
      achievements[i].description = strdup(runtime_data->achievements[i].description);
      achievements[i].title = strdup(runtime_data->achievements[i].title);
      achievements[i].updated = runtime_data->achievements[i].updated;
   }

   /* Convert leaderboards */
   leaderboards = calloc(runtime_data->num_leaderboards, sizeof(*leaderboards));
   for (int i = 0; i < runtime_data->num_leaderboards; ++i)
   {
      leaderboards[i].format = runtime_data->leaderboards[i].format;
      leaderboards[i].hidden = runtime_data->leaderboards[i].hidden;
      leaderboards[i].definition = strdup(runtime_data->leaderboards[i].definition);
      leaderboards[i].id = runtime_data->leaderboards[i].id;
      leaderboards[i].description = strdup(runtime_data->leaderboards[i].description);
      leaderboards[i].title = strdup(runtime_data->leaderboards[i].title);
      leaderboards[i].lower_is_better = runtime_data->leaderboards[i].lower_is_better;
   }

   /* Populate cache structure */
   cached_game_data->id = runtime_data->id;
   cached_game_data->console_id = runtime_data->console_id;
   cached_game_data->title = strdup(runtime_data->title);
   cached_game_data->image_name = strdup(runtime_data->image_name);
   cached_game_data->rich_presence_script = strdup(runtime_data->rich_presence_script);
   cached_game_data->cached_at = time(NULL);
   cached_game_data->achievements = achievements;
   cached_game_data->num_achievements = runtime_data->num_achievements;
   cached_game_data->leaderboards = leaderboards;
   cached_game_data->num_leaderboards = runtime_data->num_leaderboards;
}

static void rcheevos_client_unlocks_to_cache(
   const rc_api_fetch_user_unlocks_response_t *runtime_data,
   const char *username,
   rcheevos_cache_user_unlocks_t *cached_unlocks)
{
   rcheevos_cache_unlock_t *unlocks;
   time_t current_time = time(NULL);
   unsigned i;

   /* Convert achievement IDs to unlock entries */
   unlocks = calloc(runtime_data->num_achievement_ids, sizeof(*unlocks));
   for (i = 0; i < runtime_data->num_achievement_ids; ++i)
   {
      unlocks[i].achievement_id = runtime_data->achievement_ids[i];
      unlocks[i].unlock_time = current_time;
   }

   /* Populate cache structure */
   cached_unlocks->username = strdup(username);
   cached_unlocks->unlocks = unlocks;
   cached_unlocks->num_unlocks = runtime_data->num_achievement_ids;
   cached_unlocks->last_updated = current_time;
}

static void rcheevos_client_cache_to_unlocks(
   const rcheevos_cache_user_unlocks_t *cached_unlocks,
   rc_api_fetch_user_unlocks_response_t *runtime_data)
{
   unsigned *achievement_ids;
   unsigned i;

   /* Convert unlocks to achievement IDs */
   achievement_ids = calloc(cached_unlocks->num_unlocks, sizeof(*achievement_ids));
   for (i = 0; i < cached_unlocks->num_unlocks; ++i)
   {
      achievement_ids[i] = cached_unlocks->unlocks[i].achievement_id;
   }

   /* Populate runtime structure */
   runtime_data->achievement_ids = achievement_ids;
   runtime_data->num_achievement_ids = cached_unlocks->num_unlocks;
}

static void rcheevos_client_cache_to_game_data(
   const rcheevos_cache_game_t *cached_game_data,
   rc_api_fetch_game_data_response_t *runtime_data)
{
   rc_api_achievement_definition_t *achievements;
   rc_api_leaderboard_definition_t *leaderboards;

   /* Convert achievements */
   achievements = calloc(cached_game_data->num_achievements, sizeof(*achievements));
   for (int i = 0; i < cached_game_data->num_achievements; ++i)
   {
      achievements[i].author = strdup(cached_game_data->achievements[i].author);
      achievements[i].badge_name = strdup(cached_game_data->achievements[i].badge_name);
      achievements[i].category = cached_game_data->achievements[i].category;
      achievements[i].created = cached_game_data->achievements[i].created;
      achievements[i].definition = strdup(cached_game_data->achievements[i].definition);
      achievements[i].id = cached_game_data->achievements[i].id;
      achievements[i].points = cached_game_data->achievements[i].points;
      achievements[i].description = strdup(cached_game_data->achievements[i].description);
      achievements[i].title = strdup(cached_game_data->achievements[i].title);
      achievements[i].updated = cached_game_data->achievements[i].updated;
   }

   /* Convert leaderboards */
   leaderboards = calloc(cached_game_data->num_leaderboards, sizeof(*leaderboards));
   for (int i = 0; i < cached_game_data->num_leaderboards; ++i)
   {
      leaderboards[i].format = cached_game_data->leaderboards[i].format;
      leaderboards[i].hidden = cached_game_data->leaderboards[i].hidden;
      leaderboards[i].definition = strdup(cached_game_data->leaderboards[i].definition);
      leaderboards[i].id = cached_game_data->leaderboards[i].id;
      leaderboards[i].description = strdup(cached_game_data->leaderboards[i].description);
      leaderboards[i].title = strdup(cached_game_data->leaderboards[i].title);
      leaderboards[i].lower_is_better = cached_game_data->leaderboards[i].lower_is_better;
   }

   /* Populate runtime structure */
   runtime_data->id = cached_game_data->id;
   runtime_data->console_id = cached_game_data->console_id;
   runtime_data->title = strdup(cached_game_data->title);
   runtime_data->image_name = strdup(cached_game_data->image_name);
   runtime_data->rich_presence_script = strdup(cached_game_data->rich_presence_script);
   runtime_data->achievements = achievements;
   runtime_data->num_achievements = cached_game_data->num_achievements;
   runtime_data->leaderboards = leaderboards;
   runtime_data->num_leaderboards = cached_game_data->num_leaderboards;
}

/* Backfills queued achievements, if any. Then completes runtime initialization. */
static void rcheevos_client_finish_initialize_runtime(rcheevos_async_initialize_runtime_data_t *runtime_data)
{
   rcheevos_cache_pending_list_t pending;
   if (rcheevos_cache_get_pending_unlocks(runtime_data->username, &pending))
   {
      for (int i = 0; i < pending.num_entries; ++i)
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Has pending achievement: %d.\n");
      }
   }


   if (runtime_data->callback)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Hell yeah, starting callback.\n");
      runtime_data->callback(runtime_data->callback_data);
   }

   rcheevos_async_initialize_runtime_data_free(runtime_data);
   free(runtime_data);
}

static void rcheevos_client_initialize_runtime_callback(void *userdata)
{
   CHEEVOS_LOG(RCHEEVOS_TAG "Welcome to rcheevos_client_initialize_runtime_callback()\n");
   rcheevos_async_initialize_runtime_data_t *runtime_data =
      (rcheevos_async_initialize_runtime_data_t *) userdata;

   if (rcheevos_end_load_state() > 0)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Still loading. No init.\n");
      return;
   }
   if (runtime_data->game_data_fetch_status == CHEEVOS_ASYNC_STATUS_PENDING)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Game data still loading. No init.\n");
      return;
   }
   for (int i = 0; i < 2; ++i)
   {
      if (runtime_data->unlock_fetch_status[i] == CHEEVOS_ASYNC_STATUS_PENDING)
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Unlock data for %s still loading. No init.\n", i ? "softcore" : "hardcore");
         return;
      }
   }
   if (!runtime_data->badge_fetch_done)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Badge fetch still loading. No init.\n");
      return;
   }

   if (runtime_data->game_data_fetch_status == CHEEVOS_ASYNC_STATUS_SUCCESS && runtime_data->unlock_fetch_status[0] == CHEEVOS_ASYNC_STATUS_SUCCESS && runtime_data->unlock_fetch_status[1] == CHEEVOS_ASYNC_STATUS_SUCCESS && runtime_data->badge_fetch_done)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "rcheevos_client_initialize_runtime_callback doing stuff!!\n");
      rcheevos_client_copy_achievements(runtime_data);
      rcheevos_client_copy_leaderboards(runtime_data);
      rcheevos_client_initialize_runtime_rich_presence(runtime_data);
#if 0
   uint32_t id;                              /* Game ID */
   uint32_t console_id;                      /* Console type ID */
   char* title;                              /* Game title */
   char* image_name;                         /* Game badge image filename */
   char* rich_presence_script;               /* Rich presence script (optional) */
   time_t cached_at;                         /* When this was cached */

   rcheevos_cache_achievement_t* achievements;
   uint32_t num_achievements;

   rcheevos_cache_leaderboard_t* leaderboards;
   uint32_t num_leaderboards;
      };
      bool rcheevos_cache_save_game_data(const rcheevos_cache_game_t *data)
#endif
   }
   else
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "rcheevos_client_initialize_runtime_callback exiting because stuff isn't there; sad.\n");
   }

   rc_api_destroy_fetch_user_unlocks_response(&runtime_data->hardcore_unlocks);
   rc_api_destroy_fetch_user_unlocks_response(&runtime_data->non_hardcore_unlocks);
   rc_api_destroy_fetch_game_data_response(&runtime_data->game_data);
   rcheevos_client_finish_initialize_runtime(runtime_data);
}

static void rcheevos_client_fetch_game_badge_callback(void *userdata)
{
   CHEEVOS_LOG("FETCH GAME BADGE CALLBACK!\n");
   rcheevos_fetch_badge_data *data = (rcheevos_fetch_badge_data *) userdata;
   rcheevos_async_initialize_runtime_data_t *runtime_data =
      (rcheevos_async_initialize_runtime_data_t *) data->state->callback_data;

   free((void *) data->state->badge_directory);
   free(data->state);
   free(data);

   CHEEVOS_LOG("Now calling init cb \n");
   runtime_data->badge_fetch_done = true;
   rcheevos_client_initialize_runtime_callback(runtime_data);
}

static void rcheevos_client_fetch_game_badge(const char *badge_name,
                                             rcheevos_async_initialize_runtime_data_t *runtime_data)
{
#if defined(HAVE_GFX_WIDGETS) /* don't need game badge unless widgets are      \
                                 enabled */
   char badge_fullpath[PATH_MAX_LENGTH] = "";
   char *badge_fullname = NULL;
   size_t badge_fullname_size = 0;

   /* make sure the directory exists */
   fill_pathname_application_special(badge_fullpath, sizeof(badge_fullpath),
                                     APPLICATION_SPECIAL_DIRECTORY_THUMBNAILS_CHEEVOS_BADGES);

   if (!path_is_directory(badge_fullpath))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Creating %s\n", badge_fullpath);
      path_mkdir(badge_fullpath);
   }

   fill_pathname_slash(badge_fullpath, sizeof(badge_fullpath));
   badge_fullname = badge_fullpath + strlen(badge_fullpath);
   badge_fullname_size = sizeof(badge_fullpath) - (badge_fullname - badge_fullpath);

   snprintf(badge_fullname, badge_fullname_size, "i%s" FILE_PATH_PNG_EXTENSION, badge_name);

   /* check if it's already available */
   if (path_is_valid(badge_fullpath))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Badge at `%s` already available.\n", badge_fullpath);
      runtime_data->badge_fetch_done = true;
      return;
   }

#ifdef CHEEVOS_LOG_BADGES
   CHEEVOS_LOG(RCHEEVOS_TAG "Downloading game badge %s\n", badge_name);
#endif

   {
      rcheevos_async_io_request *request =
         (rcheevos_async_io_request *) calloc(1, sizeof(rcheevos_async_io_request));
      rcheevos_fetch_badge_data *data =
         (rcheevos_fetch_badge_data *) calloc(1, sizeof(rcheevos_fetch_badge_data));
      rcheevos_fetch_badge_state *state =
         (rcheevos_fetch_badge_state *) calloc(1, sizeof(rcheevos_fetch_badge_state));

      if (!request || !data || !state)
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Failed to allocate fetch badge request\n");
         runtime_data->badge_fetch_done = true;
      }
      else
      {
         rc_api_fetch_image_request_t api_params;
         int result;

         memset(&api_params, 0, sizeof(api_params));
         api_params.image_name = badge_name;
         api_params.image_type = RC_IMAGE_TYPE_GAME;

         result = rc_api_init_fetch_image_request(&request->request, &api_params);

         strlcpy(state->requested_badges[0], badge_fullname, sizeof(state->requested_badges[0]));
         *badge_fullname = '\0';
         state->badge_directory = strdup(badge_fullpath);
         state->callback_data = runtime_data;

         data->state = state;
         data->request_index = 0;
         data->callback = rcheevos_client_fetch_game_badge_callback;

         request->callback_data = data;

         rcheevos_begin_load_state(RCHEEVOS_LOAD_STATE_FETCHING_GAME_DATA);
         CHEEVOS_LOG(RCHEEVOS_TAG "Starting fetch badge request\n");
         rcheevos_async_begin_request(request, result, rcheevos_async_fetch_badge_callback, NULL,
                                      CHEEVOS_ASYNC_FETCH_BADGE, atoi(badge_name), NULL,
                                      "Error fetching game badge");
      }
   }
#endif
}

static void rcheevos_async_fetch_user_unlocks_callback(struct rcheevos_async_io_request *request,
                                                       http_transfer_data_t *data, char buffer[],
                                                       size_t buffer_size, void *handler_data)
{
   rcheevos_async_initialize_runtime_data_t *runtime_data =
      (rcheevos_async_initialize_runtime_data_t *) request->callback_data;
   const rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
   int result;
   int hardcore;
   rc_api_fetch_user_unlocks_response_t *target_unlocks;

   if (request->type == CHEEVOS_ASYNC_FETCH_HARDCORE_USER_UNLOCKS)
   {
      hardcore = 1;
      target_unlocks = &runtime_data->hardcore_unlocks;
   }
   else
   {
      hardcore = 0;
      target_unlocks = &runtime_data->non_hardcore_unlocks;
   }
   CHEEVOS_LOG(RCHEEVOS_TAG "rcheevos_async_fetch_user_unlocks_callback[%d] start\n", hardcore);

   result = rc_api_process_fetch_user_unlocks_response(target_unlocks, data->data);
   if (rcheevos_async_succeeded(result, &target_unlocks->response, buffer, buffer_size))
   {
      rcheevos_cache_user_unlocks_t cached_unlocks;
      rcheevos_client_unlocks_to_cache(target_unlocks, rcheevos_locals->username, &cached_unlocks);

      if (rcheevos_cache_save_user_unlocks(rcheevos_locals->username, rcheevos_locals->game.id, hardcore, &cached_unlocks))
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Cached %s unlocks for game %u\n",
                     hardcore ? "hardcore" : "softcore", rcheevos_locals->game.id);
      }
      else
      {
         CHEEVOS_ERR(RCHEEVOS_TAG "Failed to cache %s unlocks for game %u\n",
                     hardcore ? "hardcore" : "softcore", rcheevos_locals->game.id);
      }
      rcheevos_cache_unlocks_free(&cached_unlocks);
      runtime_data->unlock_fetch_status[hardcore] = CHEEVOS_ASYNC_STATUS_SUCCESS;
   }
   else if (runtime_data->unlocks_cache_time[hardcore] != 0)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Remote request failed, falling back to cached game data available for %u\n", runtime_data->game_data.id);
      runtime_data->unlock_fetch_status[hardcore] = CHEEVOS_ASYNC_STATUS_SUCCESS;
   }
   else
   {
      runtime_data->unlock_fetch_status[hardcore] = CHEEVOS_ASYNC_STATUS_FAILED;
   }
   CHEEVOS_LOG(RCHEEVOS_TAG "rcheevos_async_fetch_user_unlocks_callback[%d] finish\n", hardcore);
}

static void rcheevos_fetch_game_data_done(rcheevos_async_initialize_runtime_data_t *runtime_data, bool success)
{
   runtime_data->game_data_fetch_status = success ? CHEEVOS_ASYNC_STATUS_SUCCESS : CHEEVOS_ASYNC_STATUS_FAILED;
   if (success)
   {
      rcheevos_client_fetch_game_badge(runtime_data->game_data.image_name, runtime_data);
   }
   else
   {
      rcheevos_unload();
   }
}

static void rcheevos_async_fetch_game_data_callback(struct rcheevos_async_io_request *request,
                                                    http_transfer_data_t *data, char buffer[],
                                                    size_t buffer_size, void *handler_data)
{
   rcheevos_async_initialize_runtime_data_t *runtime_data =
      (rcheevos_async_initialize_runtime_data_t *) request->callback_data;

#ifdef CHEEVOS_SAVE_JSON
   filestream_write_file(CHEEVOS_SAVE_JSON, data->data, data->len);
#endif

   bool proceed = true;
   int result = rc_api_process_fetch_game_data_response(&runtime_data->game_data, data->data);
   if (rcheevos_async_succeeded(result, &runtime_data->game_data.response, buffer, buffer_size))
   {
      rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
      rcheevos_locals->game.title = strdup(runtime_data->game_data.title);
      rcheevos_locals->game.console_id = runtime_data->game_data.console_id;

      rcheevos_cache_game_t cached_game_data;
      rcheevos_client_game_data_to_cache(&runtime_data->game_data, &cached_game_data);

      if (rcheevos_cache_save_game_data(&cached_game_data))
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Cached game data for %u\n", cached_game_data.id);
      }
      else
      {
         CHEEVOS_ERR(RCHEEVOS_TAG "Failed to cache game data for %u\n", cached_game_data.id);
      }
      rcheevos_cache_game_free(&cached_game_data);

      snprintf(rcheevos_locals->game.badge_name, sizeof(rcheevos_locals->game.badge_name), "i%s",
               runtime_data->game_data.image_name);
      rcheevos_fetch_game_data_done(runtime_data, true);
   }
   else if (runtime_data->game_data_cache_time != 0)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Remote request failed, falling back to cached game data available for %u\n", runtime_data->game_data.id);
      rcheevos_fetch_game_data_done(runtime_data, true);
   }
   else
   {
      rcheevos_fetch_game_data_done(runtime_data, false);
   }
}

static bool rcheevos_client_get_cached_game_data(uint64_t game_id, rcheevos_async_initialize_runtime_data_t *data)
{
   rcheevos_cache_game_t game_data;
   if (!rcheevos_cache_get_game_data(game_id, &game_data))
   {
      return false;
   }

   rcheevos_client_cache_to_game_data(&game_data, &data->game_data);
   rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
   rcheevos_locals->game.title = strdup(game_data.title);
   rcheevos_locals->game.console_id = game_data.console_id;

   data->game_data_cache_time = game_data.cached_at;
   CHEEVOS_LOG(RCHEEVOS_TAG "Game data cache hit for %u\n", game_id);
   rcheevos_cache_game_free(&game_data);
   return true;
}

static void rcheevos_client_start_fetch_game_data(rcheevos_async_initialize_runtime_data_t *data)
{
   const rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
   if (rcheevos_client_get_cached_game_data(rcheevos_locals->game.id, data))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Game data cache hit for %u\n", rcheevos_locals->game.id);
      if ((data->game_data_cache_time + CHEEVOS_CACHE_EXPIRATION_SEC) > time(NULL))
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Game data is valid; bypassing remote fetch.\n");
         rcheevos_fetch_game_data_done(data, true);
         return;
      }
      else if (rcheevos_locals->logged_in)
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "No network available; falling back to cached data.\n");
         rcheevos_fetch_game_data_done(data, true);
         return;
      }
      else
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Game data expired; fetching remote game data.\n");
      }
   }

   rcheevos_async_io_request *request = (rcheevos_async_io_request *) calloc(1, sizeof(rcheevos_async_io_request));
   if (!request)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to allocate game data fetch request\n");
      return;
   }
#ifdef CHEEVOS_JSON_OVERRIDE
   char buffer[128];
   size_t size = 0;
   char *contents;
   http_transfer_data_t transfer_data;
   FILE *file = fopen(CHEEVOS_JSON_OVERRIDE, "rb");

   fseek(file, 0, SEEK_END);
   size = ftell(file);
   fseek(file, 0, SEEK_SET);

   contents = (char *) malloc(size + 1);
   fread((void *) contents, 1, size, file);
   fclose(file);

   contents[size] = 0;

   transfer_data.data = contents;
   transfer_data.len = size;
   transfer_data.status = 200;

   request->callback_data = data;
   rcheevos_async_fetch_game_data_callback(request, &transfer_data, buffer, sizeof(buffer));

   free(contents);
#else
   rc_api_fetch_game_data_request_t api_params;
   int result;

   memset(&api_params, 0, sizeof(api_params));
   api_params.username = rcheevos_locals->username;
   api_params.api_token = rcheevos_locals->token;
   api_params.game_id = rcheevos_locals->game.id;

   result = rc_api_init_fetch_game_data_request(&request->request, &api_params);

   request->callback = rcheevos_client_initialize_runtime_callback;
   request->callback_data = data;

   rcheevos_begin_load_state(RCHEEVOS_LOAD_STATE_FETCHING_GAME_DATA);
   rcheevos_async_begin_request(request, result, rcheevos_async_fetch_game_data_callback, NULL,
                                CHEEVOS_ASYNC_FETCH_GAME_DATA, rcheevos_locals->game.id,
                                "Fetched game data", "Error fetching game data");
#endif
}

static bool rcheevos_client_get_cached_unlocks_data(int hardcore, uint32_t game_id, rcheevos_async_initialize_runtime_data_t *data)
{
   rcheevos_cache_user_unlocks_t cached_unlocks;
   const rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
   rc_api_fetch_user_unlocks_response_t *target_unlocks;

   if (!rcheevos_cache_get_user_unlocks(rcheevos_locals->username, game_id, hardcore, &cached_unlocks))
   {
      return false;
   }

   /* Select which unlock structure to populate based on hardcore flag */
   target_unlocks = hardcore ? &data->hardcore_unlocks : &data->non_hardcore_unlocks;

   /* Convert cached unlock data to runtime format */
   rcheevos_client_cache_to_unlocks(&cached_unlocks, target_unlocks);

   /* Store cache timestamp */
   data->unlocks_cache_time[hardcore] = cached_unlocks.last_updated;

   CHEEVOS_LOG(RCHEEVOS_TAG "%s unlocks cache hit for game %u (%u unlocks)\n",
               hardcore ? "Hardcore" : "Softcore", game_id, cached_unlocks.num_unlocks);

   rcheevos_cache_unlocks_free(&cached_unlocks);
   return true;
}

static void rcheevos_client_start_fetch_user_unlocks(int hardcore, rcheevos_async_initialize_runtime_data_t *data)
{
   const rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();

   if (rcheevos_client_get_cached_unlocks_data(hardcore, rcheevos_locals->game.id, data))
   {
      time_t cache_time = data->unlocks_cache_time[hardcore];
      CHEEVOS_LOG(RCHEEVOS_TAG "User unlocks cache hit for game %u (hardcore=%d)\n", rcheevos_locals->game.id, hardcore);
      /* Cache data is valid, check if it's fresh enough */
      if ((cache_time + CHEEVOS_UNLOCKS_CACHE_EXPIRATION_SEC) > time(NULL))
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "User unlocks data is valid; bypassing remote fetch.\n");
         data->unlock_fetch_status[hardcore] = CHEEVOS_ASYNC_STATUS_SUCCESS;
         return;
      }
      else if (!rcheevos_locals->logged_in)
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "No network available; falling back to cached user unlock data.\n");
         data->unlock_fetch_status[hardcore] = CHEEVOS_ASYNC_STATUS_SUCCESS;
         return;
      }
      else
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "User unlock cache expired; refetching.\n");
      }
   }
   else
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "No user unlocks cache hit for hardcore=%d, user=`%s`. Fetching.\n", hardcore, rcheevos_locals->username);
   }

   rcheevos_async_io_request *request = (rcheevos_async_io_request *) calloc(1, sizeof(rcheevos_async_io_request));
   if (!request)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to allocate user unlock request\n");
      return;
   }
   rc_api_fetch_user_unlocks_request_t api_params;
   int result;

   memset(&api_params, 0, sizeof(api_params));
   api_params.username = rcheevos_locals->username;
   api_params.api_token = rcheevos_locals->token;
   api_params.game_id = rcheevos_locals->game.id;
   api_params.hardcore = hardcore;

   result = rc_api_init_fetch_user_unlocks_request(&request->request, &api_params);

   request->callback = rcheevos_client_initialize_runtime_callback;
   request->callback_data = data;

   rcheevos_begin_load_state(RCHEEVOS_LOAD_STATE_FETCHING_GAME_DATA);
   if (hardcore == 0)
   {
      rcheevos_async_begin_request(
         request, result, rcheevos_async_fetch_user_unlocks_callback, NULL,
         CHEEVOS_ASYNC_FETCH_USER_UNLOCKS, rcheevos_locals->game.id,
         "Fetched user unlocks", "Error fetching user unlocks");
   }
   else
   {
      rcheevos_async_begin_request(
         request, result, rcheevos_async_fetch_user_unlocks_callback, NULL,
         CHEEVOS_ASYNC_FETCH_HARDCORE_USER_UNLOCKS, rcheevos_locals->game.id,
         "Fetched hardcore user unlocks", "Error fetching hardcore user unlocks");
   }
}

void rcheevos_client_initialize_runtime(unsigned game_id, rcheevos_client_callback callback,
                                        void *userdata)
{
   rcheevos_async_io_request *request;
   const settings_t *settings = config_get_ptr();
   const rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
   rcheevos_async_initialize_runtime_data_t *data =
      (rcheevos_async_initialize_runtime_data_t *) calloc(1,
                                                          sizeof(rcheevos_async_initialize_runtime_data_t));

   if (!data)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to allocate runtime initalization data\n");
      return;
   }

   data->callback = callback;
   data->callback_data = userdata;
   data->username = strdup(rcheevos_locals->username);
   rcheevos_client_start_fetch_game_data(data);
   if (settings->bools.cheevos_start_active)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "cheevos_start_active -> no unlock fetch\n");
      memset(&data->hardcore_unlocks, 0, sizeof(data->hardcore_unlocks));
      memset(&data->non_hardcore_unlocks, 0, sizeof(data->non_hardcore_unlocks));

      data->hardcore_unlocks.num_achievement_ids = 0;
      data->non_hardcore_unlocks.num_achievement_ids = 0;
      data->unlock_fetch_status[0] = CHEEVOS_ASYNC_STATUS_SUCCESS;
      data->unlock_fetch_status[1] = CHEEVOS_ASYNC_STATUS_SUCCESS;
   }
   else
   {
      int i;
      for (i = 0; i < 2; ++i)
      {
         rcheevos_client_start_fetch_user_unlocks(i, data);
      }
   }
   // Trigger end state for initialization. If any request is pending from the code above,
   // this is a noop (this will then be called by one of the other HTTP callbacks).
   rcheevos_client_initialize_runtime_callback(data);
}

/****************************
 * ping                     *
 ****************************/

static retro_time_t rcheevos_client_prepare_ping(rcheevos_async_io_request *request)
{
   rc_api_ping_request_t api_params;
   const rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
   const settings_t *settings = config_get_ptr();
   const bool cheevos_richpresence_enable =
      rcheevos_hardcore_active() || settings->bools.cheevos_richpresence_enable;
   char buffer[256] = "";

   memset(&api_params, 0, sizeof(api_params));
   api_params.username = rcheevos_locals->username;
   api_params.api_token = rcheevos_locals->token;
   api_params.game_id = request->id;

   if (cheevos_richpresence_enable)
   {
      rcheevos_get_richpresence(buffer, sizeof(buffer));
      api_params.rich_presence = buffer;
   }

   rc_api_init_ping_request(&request->request, &api_params);

   rcheevos_log_post_url(request->request.url, request->request.post_data);

#ifdef HAVE_PRESENCE
   presence_update(PRESENCE_RETROACHIEVEMENTS);
#endif

   /* Update rich presence every two minutes */
   if (cheevos_richpresence_enable)
      return cpu_features_get_time_usec() + CHEEVOS_PING_FREQUENCY;

   /* Send ping every four minutes */
   return cpu_features_get_time_usec() + CHEEVOS_PING_FREQUENCY * 2;
}

/** Callback data for rcheevos_async_network_state_poll_handler */
typedef struct
{
   bool online;
   rcheevos_async_io_request *net_poll_request; // NULL when not online, non-null and has valid user agent when online.
   rcheevos_async_io_request *ping_request;     // This is initialized once if the initial login is successful. Otherwise it's always NULL.
   unsigned game_id;

   // TODO(future): these should be locked with a mutex
   bool pending_sync_request;
   unsigned int *pending_achievement_queue;
   int pending_achievement_queue_size;
} rcheevos_async_network_state_poll_state_t;

static void rcheevos_poll_dispatch_pending_achievements(rcheevos_async_network_state_poll_state_t *state)
{
   /// TODO ///
   /// @claude
   if (state->pending_sync_request)
   {
      return; // Don't queue more pending requests if some are still ongoing
   }
   /// Here, we check pending_achievement_queue.
   /// - If empty: we refresh the list of pending achievements into pending_achievement_queue.
   ///    - Then continue to next step
   /// - If non empty, we set pending_sync_request and spawn a task that "awards" the next achievement in the list.
   ///   - The pending achievement callback will, on success, move on to the next achievement. on failure or no achievement left, reset pending_sync_request.
}

static void rcheevos_async_network_state_poll_handler(retro_task_t *task)
{
   rcheevos_async_network_state_poll_state_t *state = (rcheevos_async_network_state_poll_state_t *) task->user_data;

   const rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
   if (state->game_id != rcheevos_locals->game.id)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Stopping periodic rich presence update task for game %u\n",
                  state->net_poll_request->id);
      /* game changed; stop the recurring task - a new one will
     * be scheduled if a new game is loaded */
      task_set_finished(task, 1);
      /* request->request was destroyed
     * in rcheevos_async_http_task_callback */
      free(state->net_poll_request);
      state->net_poll_request = NULL;
      return;
   }
   if (state->online)
   {
      rcheevos_poll_dispatch_pending_achievements(state);
   }

   /* Set the task to fire again */
   task->when = cpu_features_get_time_usec() + CHEEVOS_NETWORK_POLL_FREQUENCY;

   /* Start the HTTP request */
   rcheevos_async_begin_http_request(state->net_poll_request);
}

static void rcheevos_async_ping_callback(struct rcheevos_async_io_request *request,
                                         http_transfer_data_t *data, char buffer[],
                                         size_t buffer_size,
                                         void *handler_data)
{
   rcheevos_async_network_state_poll_state_t *state = (rcheevos_async_network_state_poll_state_t *) handler_data;
   if (data->status != 200)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Ping succeeded for game %u :)\n", request->id);
      state->online = true;
      return;
   }

   // Network temporarily down. Will retry.
   CHEEVOS_LOG(RCHEEVOS_TAG "Ping failed for game %u :( code: %d\n", data->status);
   state->online = false;
}

/** Periodic callback that sends keepalive pings to the server. */
static void rcheevos_async_ping_handler(retro_task_t *task)
{
   rcheevos_async_network_state_poll_state_t *state = (rcheevos_async_network_state_poll_state_t *) task->user_data;
   rcheevos_async_io_request *request = state->ping_request;

   const rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
   if (request->id != (int) rcheevos_locals->game.id)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Stopping periodic rich presence update task for game %u\n",
                  request->id);
      /* game changed; stop the recurring task - a new one will
     * be scheduled if a new game is loaded */
      task_set_finished(task, 1);
      /* request->request was destroyed
     * in rcheevos_async_http_task_callback */
      free(request);
      return;
   }

   /* update the request and set the task to fire again in
   * two minutes */
   task->when = rcheevos_client_prepare_ping(request);

   /* start the HTTP request */
   rcheevos_async_begin_http_request(request);
}

/****************************
 * start session            *
 ****************************/

static void rcheevos_async_start_session_callback(struct rcheevos_async_io_request *request,
                                                  http_transfer_data_t *data, char buffer[],
                                                  size_t buffer_size, void *handler_data)
{
   rc_api_start_session_response_t api_response;

   int result = rc_api_process_start_session_response(&api_response, data->data);
   rcheevos_async_succeeded(result, &api_response.response, buffer, buffer_size);
   rc_api_destroy_start_session_response(&api_response);
}

static void rcheevos_client_start_network_state_poll(unsigned game_id, rcheevos_async_network_state_poll_state_t *state)
{
   rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
   retro_task_t *task = task_init();

   task->handler = rcheevos_async_network_state_poll_handler;
   task->user_data = state;
   task->progress = -1;
   task->when = cpu_features_get_time_usec();

   CHEEVOS_LOG(RCHEEVOS_TAG "Starting network state poll for %u\n", game_id);
   task_queue_push(task);
}

void rcheevos_client_start_session(unsigned game_id)
{
   rcheevos_async_network_state_poll_state_t *state =
      (rcheevos_async_network_state_poll_state_t *) calloc(1, sizeof(*state));
   if (!state)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to allocate network poll state\n");
      return;
   }
   rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();

   state->online = rcheevos_locals->logged_in;
   state->game_id = game_id;
   state->net_poll_request = NULL;
   state->ping_request = NULL;

   if (!rcheevos_locals->logged_in)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Not logged in to retroachievements: will not start session.\n");
      return;
   }

   rcheevos_client_start_network_state_poll(game_id, state);

   /* the core won't change while a session is active, so only
   * calculate the user agent once */
   rcheevos_get_user_agent(rcheevos_locals, rcheevos_locals->user_agent_core,
                           sizeof(rcheevos_locals->user_agent_core));

   /* schedule the first rich presence call in 30 seconds */
   {
      state->ping_request =
         (rcheevos_async_io_request *) calloc(1, sizeof(rcheevos_async_io_request));
      if (!state->ping_request)
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Failed to allocate rich presence request\n");
      }
      else
      {
         retro_task_t *task = task_init();

         state->ping_request->id = game_id;
         state->ping_request->type = CHEEVOS_ASYNC_RICHPRESENCE;
         state->ping_request->user_agent = rcheevos_locals->user_agent_core;
         state->ping_request->failure_message = "Error sending ping";

         state->ping_request->handler = rcheevos_async_ping_callback;
         state->ping_request->handler_data = state;
         task->handler = rcheevos_async_ping_handler;
         task->user_data = state;
         task->progress = -1;
         task->when = cpu_features_get_time_usec() + CHEEVOS_PING_FREQUENCY / 4;

         CHEEVOS_LOG(RCHEEVOS_TAG "Starting periodic rich presence update task for game %u\n",
                     game_id);
         task_queue_push(task);
      }
   }

   /* send the new session request */
   {
      rcheevos_async_io_request *request =
         (rcheevos_async_io_request *) calloc(1, sizeof(rcheevos_async_io_request));
      if (!request)
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Failed to allocate new session request\n");
      }
      else
      {
         rc_api_start_session_request_t api_params;
         int result;

         memset(&api_params, 0, sizeof(api_params));
         api_params.username = rcheevos_locals->username;
         api_params.api_token = rcheevos_locals->token;
         api_params.game_id = game_id;

         result = rc_api_init_start_session_request(&request->request, &api_params);

         rcheevos_async_begin_request(
            request, result, rcheevos_async_start_session_callback, NULL,
            CHEEVOS_ASYNC_START_SESSION,
            game_id, "Started session for game", "Error starting session for game");
      }
   }
}

/****************************
 * fetch badge              *
 ****************************/

static bool rcheevos_fetch_next_badge(rcheevos_fetch_badge_state *state);

static void rcheevos_end_fetch_badges(rcheevos_fetch_badge_state *state)
{
   if (state->callback)
      state->callback(state->callback_data);

   free((void *) state->badge_directory);
   free(state);
}

static void rcheevos_async_download_next_badge(void *userdata)
{
   rcheevos_fetch_badge_data *badge_data = (rcheevos_fetch_badge_data *) userdata;
   rcheevos_fetch_next_badge(badge_data->state);

   if (rcheevos_end_load_state() == 0)
      rcheevos_end_fetch_badges(badge_data->state);

   free(badge_data);
}

static void rcheevos_async_fetch_badge_complete(rcheevos_fetch_badge_data *badge_data)
{
#ifdef HAVE_THREADS
   const rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();

   slock_lock(rcheevos_locals->load_info.request_lock);
#endif
   badge_data->state->requested_badges[badge_data->request_index][0] = '\0';
#ifdef HAVE_THREADS
   slock_unlock(rcheevos_locals->load_info.request_lock);
#endif

   if (badge_data->callback)
      badge_data->callback(badge_data);
}

static void rcheevos_async_write_badge(retro_task_t *task)
{
   char badge_fullpath[PATH_MAX_LENGTH];
   rcheevos_fetch_badge_data *badge_data = (rcheevos_fetch_badge_data *) task->user_data;

   fill_pathname_join_special(badge_fullpath, badge_data->state->badge_directory,
                              badge_data->state->requested_badges[badge_data->request_index],
                              sizeof(badge_fullpath));

   if (!filestream_write_file(badge_fullpath, badge_data->data, badge_data->data_len))
   {
      CHEEVOS_ERR(RCHEEVOS_TAG "Error writing badge %s\n", badge_fullpath);
   }

   free(badge_data->data);
   badge_data->data = NULL;
   badge_data->data_len = 0;

   task_set_finished(task, true);

   rcheevos_async_fetch_badge_complete(badge_data);
}

static void rcheevos_async_fetch_badge_callback(struct rcheevos_async_io_request *request,
                                                http_transfer_data_t *data, char buffer[],
                                                size_t buffer_size, void *handler_data)
{
   CHEEVOS_LOG(RCHEEVOS_TAG "rcheevos_async_fetch_badge_complete()\n");
   rcheevos_fetch_badge_data *badge_data = (rcheevos_fetch_badge_data *) request->callback_data;
   retro_task_t *task;

   if (!data->data)
   {
      /* error retrieving badge, nothing to write. just call the callback */
      CHEEVOS_LOG(RCHEEVOS_TAG "rcheevos_async_fetch_badge_complete() bailing out\n");
      rcheevos_async_fetch_badge_complete(badge_data);
      return;
   }

   /* take ownership of the file data */
   badge_data->data = data->data;
   badge_data->data_len = data->len;
   data->data = NULL;
   data->len = 0;

   /* this is called on the primary thread. use a task to write the file from a
   * background thread */
   task = task_init();
   task->handler = rcheevos_async_write_badge;
   task->user_data = badge_data;
   task_queue_push(task);
   CHEEVOS_LOG(RCHEEVOS_TAG "End rcheevos_async_fetch_badge_complete()\n");
}

static bool rcheevos_client_fetch_badge(const char *badge_name, int locked,
                                        rcheevos_fetch_badge_state *state)
{
   char badge_fullpath[PATH_MAX_LENGTH];
   char *badge_fullname = NULL;
   size_t badge_fullname_size = 0;
   int request_index = -1;

   if (!badge_name || !badge_name[0])
      return false;

   strlcpy(badge_fullpath, state->badge_directory, sizeof(badge_fullpath));
   fill_pathname_slash(badge_fullpath, sizeof(badge_fullpath));
   badge_fullname = badge_fullpath + strlen(badge_fullpath);
   badge_fullname_size = sizeof(badge_fullpath) - (badge_fullname - badge_fullpath);

   snprintf(badge_fullname, badge_fullname_size, "%s%s" FILE_PATH_PNG_EXTENSION, badge_name,
            locked ? "_lock" : "");

   /* check if it's already available */
   if (path_is_valid(badge_fullpath))
      return false;

   /* check if it's already requested */
   {
      int i;
      int found_index = -1;
#ifdef HAVE_THREADS
      const rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
      slock_lock(rcheevos_locals->load_info.request_lock);
#endif
      for (i = RCHEEVOS_CONCURRENT_BADGE_DOWNLOADS - 1; i >= 0; --i)
      {
         if (!state->requested_badges[i][0])
            request_index = i;
         else if (string_is_equal(badge_fullname, state->requested_badges[i]))
         {
            found_index = i;
            break;
         }
      }

      if (found_index == -1)
      {
         /* unexpected - but if it happens,
       * the queue is full. Pretend we found
       * a match to prevent an exception */
         if (request_index == -1)
            found_index = 0;
         else
            strlcpy(state->requested_badges[request_index], badge_fullname,
                    sizeof(state->requested_badges[request_index]));
      }
#ifdef HAVE_THREADS
      slock_unlock(rcheevos_locals->load_info.request_lock);
#endif
      if (found_index != -1)
         return false;
   }

   /* request the new badge */
#ifdef CHEEVOS_LOG_BADGES
   CHEEVOS_LOG(RCHEEVOS_TAG "Downloading badge %s\n", badge_name);
#endif

   {
      rcheevos_async_io_request *request =
         (rcheevos_async_io_request *) calloc(1, sizeof(rcheevos_async_io_request));
      rcheevos_fetch_badge_data *data =
         (rcheevos_fetch_badge_data *) calloc(1, sizeof(rcheevos_fetch_badge_data));

      if (!request || !data)
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Failed to allocate fetch badge request\n");
      }
      else
      {
         rc_api_fetch_image_request_t api_params;
         int result;

         memset(&api_params, 0, sizeof(api_params));
         api_params.image_name = badge_name;
         api_params.image_type =
            locked ? RC_IMAGE_TYPE_ACHIEVEMENT_LOCKED : RC_IMAGE_TYPE_ACHIEVEMENT;

         result = rc_api_init_fetch_image_request(&request->request, &api_params);

         data->state = state;
         data->request_index = request_index;
         data->callback = rcheevos_async_download_next_badge;

         request->callback_data = data;

         rcheevos_begin_load_state(RCHEEVOS_LOAD_STATE_FETCHING_BADGES);
         rcheevos_async_begin_request(request, result, rcheevos_async_fetch_badge_callback, NULL,
                                      CHEEVOS_ASYNC_FETCH_BADGE, atoi(badge_name), NULL,
                                      "Error fetching badge");
      }
   }

   return true;
}

static bool rcheevos_fetch_next_badge(rcheevos_fetch_badge_state *state)
{
   if (rcheevos_load_aborted())
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Load aborted while fetching badges\n");
   }
   else
   {
      int active = 0;
      const rcheevos_racheevo_t *cheevo = NULL;
      const rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();

      /* fetch badges for current state of achievements first */
      do
      {
#ifdef HAVE_THREADS
         slock_lock(rcheevos_locals->load_info.request_lock);
#endif
         if (state->locked_badge_fetch_index < rcheevos_locals->game.achievement_count)
            cheevo = &rcheevos_locals->game.achievements[state->locked_badge_fetch_index++];
         else
            cheevo = NULL;
#ifdef HAVE_THREADS
         slock_unlock(rcheevos_locals->load_info.request_lock);
#endif

         if (!cheevo)
            break;

         active = (cheevo->active & (RCHEEVOS_ACTIVE_HARDCORE | RCHEEVOS_ACTIVE_SOFTCORE));
         if (rcheevos_client_fetch_badge(cheevo->badge, active, state))
            return true;

      } while (true);

      /* then fetch badges for unlocked state so they're ready when the user
     * unlocks something */
      do
      {
#ifdef HAVE_THREADS
         slock_lock(rcheevos_locals->load_info.request_lock);
#endif
         if (state->badge_fetch_index < rcheevos_locals->game.achievement_count)
            cheevo = &rcheevos_locals->game.achievements[state->badge_fetch_index++];
         else
            cheevo = NULL;

#ifdef HAVE_THREADS
         slock_unlock(rcheevos_locals->load_info.request_lock);
#endif

         if (!cheevo)
            break;

         if (rcheevos_client_fetch_badge(cheevo->badge, 0, state))
            return true;

      } while (true);
   }

   return false;
}

void rcheevos_client_fetch_badges(rcheevos_client_callback callback, void *userdata)
{
#if defined(HAVE_MENU) || \
   defined(HAVE_GFX_WIDGETS) /* don't need badges unless menu or widgets are  \
                                 enabled */
   rcheevos_fetch_badge_state *state = NULL;
   char badge_fullpath[PATH_MAX_LENGTH] = "";
#if !defined(HAVE_GFX_WIDGETS) /* we always want badges if widgets are enabled */
   settings_t *settings = config_get_ptr();
   /* User has explicitly disabled badges */
   if (!settings->bools.cheevos_badges_enable)
      return;

   /* badges are only needed for xmb and ozone menus */
   if (!string_is_equal(settings->arrays.menu_driver, "xmb") &&
       !string_is_equal(settings->arrays.menu_driver, "ozone"))
      return;
#endif /* !defined(HAVE_GFX_WIDGETS) */

   /* make sure the directory exists */
   fill_pathname_application_special(badge_fullpath, sizeof(badge_fullpath),
                                     APPLICATION_SPECIAL_DIRECTORY_THUMBNAILS_CHEEVOS_BADGES);

   if (!path_is_directory(badge_fullpath))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Creating %s\n", badge_fullpath);
      path_mkdir(badge_fullpath);
   }

   /* start the download task */
   state = (rcheevos_fetch_badge_state *) calloc(1, sizeof(rcheevos_fetch_badge_state));

   if (!state)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to allocate fetch badge state\n");
   }
   else
   {
      int num_concurrent = RCHEEVOS_CONCURRENT_BADGE_DOWNLOADS;

      state->badge_directory = strdup(badge_fullpath);
      state->locked_badge_fetch_index = 0;
      state->badge_fetch_index = 0;
      state->callback = callback;
      state->callback_data = userdata;

      rcheevos_begin_load_state(RCHEEVOS_LOAD_STATE_FETCHING_BADGES);

      /* fetch the placeholder image */
      if (rcheevos_client_fetch_badge("00000", 0, state))
         num_concurrent--;

      /* queue up additional requests so up to {num_concurrent} downloads are
     * queued */
      while (num_concurrent--)
      {
         if (!rcheevos_fetch_next_badge(state))
            break;
      }

      if (rcheevos_end_load_state() == 0)
         rcheevos_end_fetch_badges(state);
   }
#endif /* defined(HAVE_MENU) || defined(HAVE_GFX_WIDGETS) */
}

#undef RCHEEVOS_CONCURRENT_BADGE_DOWNLOADS

/****************************
 * award achievement        *
 ****************************/

static void rcheevos_register_achievement_unlocked(const char *username, uint64_t game_id, bool hardcore, unsigned int awarded_achievement, unsigned int achievements_remaining)
{
   /* Update cache if available */
   if (achievements_remaining == 0)
   {
      rcheevos_show_mastery_placard();
   }
   rcheevos_cache_user_unlocks_t cached_unlocks;
   if (rcheevos_cache_get_user_unlocks(username, game_id, hardcore, &cached_unlocks))
   {
      // Update cache accordingly
      cached_unlocks.unlocks = reallocarray(cached_unlocks.unlocks, cached_unlocks.num_unlocks + 1, sizeof(*cached_unlocks.unlocks));
      cached_unlocks.num_unlocks++;
      rcheevos_cache_save_user_unlocks(username, game_id, hardcore, &cached_unlocks);
   }
}

static void rcheevos_queue_achievement_sync(const char *username, uint64_t game_id, unsigned int achievement_id, bool hardcore, bool is_leaderboard, unsigned int score, time_t timestamp)
{
   rcheevos_cache_pending_list_t pending;
   if (!rcheevos_cache_get_pending_unlocks(username, &pending))
   {
      pending.entries = NULL;
      pending.num_entries = 0;
   }
   pending.entries = reallocarray(pending.entries, pending.num_entries + 1, sizeof(*pending.entries));
   pending.entries[pending.num_entries].game_id = game_id;
   pending.entries[pending.num_entries].hardcore = hardcore;
   pending.entries[pending.num_entries].id = achievement_id;
   pending.entries[pending.num_entries].is_leaderboard = is_leaderboard;
   pending.entries[pending.num_entries].retries = 0;
   pending.entries[pending.num_entries].score = score;
   pending.entries[pending.num_entries].timestamp = timestamp;
   pending.num_entries++;
   if (rcheevos_cache_save_pending_unlocks(username, &pending))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Achievement %d of game ID %d put in pending queue for `%s`.\n", achievement_id, game_id, username);
   }
   rcheevos_cache_pending_free(&pending);
}


typedef struct rcheevos_async_award_achievement_callback_data_t
{
   unsigned int achievement_id;
   char *username;
   uint64_t game_id;
   bool hardcore;
   bool is_leaderboard;
   unsigned int score;
   time_t timestamp;
   bool award_success;
} rcheevos_client_award_achievement_callback_data_t;

void rcheevos_client_award_achievement_callback_data_free(rcheevos_client_award_achievement_callback_data_t *data)
{
   if (!data)
   {
      return;
   }
   free(data->username);
}

static void rcheevos_async_award_achievement_callback(struct rcheevos_async_io_request *request,
                                                      http_transfer_data_t *data, char buffer[],
                                                      size_t buffer_size, void *handler_data)
{
   rc_api_award_achievement_response_t api_response;
   rcheevos_client_award_achievement_callback_data_t *cb_data = (rcheevos_client_award_achievement_callback_data_t *) handler_data;

   int result = rc_api_process_award_achievement_response(&api_response, data->data);
   if (rcheevos_async_succeeded(result, &api_response.response, buffer, buffer_size))
   {
      if (api_response.awarded_achievement_id != request->id)
         snprintf(buffer, buffer_size, "Achievement %u awarded instead",
                  api_response.awarded_achievement_id);
      else if (api_response.response.error_message)
      {
         /* previously unlocked achievements are returned as a "successful" error
       */
         CHEEVOS_LOG(RCHEEVOS_TAG "Achievement %u: %s\n", request->id,
                     api_response.response.error_message);
      }
      rcheevos_register_achievement_unlocked(cb_data->username, cb_data->game_id, cb_data->hardcore, api_response.awarded_achievement_id, api_response.achievements_remaining);
      cb_data->award_success = true;
   }

   rc_api_destroy_award_achievement_response(&api_response);
}

void rcheevos_client_award_achievement_callback(void *userdata)
{
   rcheevos_client_award_achievement_callback_data_t *cb_data = (rcheevos_client_award_achievement_callback_data_t *) userdata;
   if (!cb_data->award_success)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Queueing achievement %u in unlock cache.\n", cb_data->achievement_id);
      rcheevos_queue_achievement_sync(cb_data->username, cb_data->game_id, cb_data->achievement_id, cb_data->hardcore, cb_data->is_leaderboard, 0, cb_data->timestamp);
      rcheevos_register_achievement_unlocked(cb_data->username, cb_data->game_id, cb_data->hardcore, cb_data->achievement_id, 42 /* Achievements remaining: how to compute? */);
   }
   rcheevos_client_award_achievement_callback_data_free(cb_data);
   free(userdata);
}

void rcheevos_client_award_achievement(unsigned achievement_id)
{
   rcheevos_async_io_request *request =
      (rcheevos_async_io_request *) calloc(1, sizeof(rcheevos_async_io_request));
   if (!request)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to allocate unlock request for achievement %u\n",
                  achievement_id);
      return;
   }
   const rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
   rc_api_award_achievement_request_t api_params;
   int result;

   memset(&api_params, 0, sizeof(api_params));
   api_params.username = rcheevos_locals->username;
   api_params.api_token = rcheevos_locals->token;
   api_params.achievement_id = achievement_id;
   api_params.achievement_id = achievement_id;
   api_params.hardcore = rcheevos_locals->hardcore_active ? 1 : 0;
   api_params.game_hash = rcheevos_locals->game.hash;

   result = rc_api_init_award_achievement_request(&request->request, &api_params);

   rcheevos_client_award_achievement_callback_data_t *cb_data = calloc(1, sizeof(*cb_data));
   if (!cb_data)
   {
      CHEEVOS_ERR(RCHEEVOS_TAG "Failed to allocate callback context for rcheevos_async_award_achievement_callback\n.");
      free(request);
      return;
   }
   cb_data->achievement_id = achievement_id;
   cb_data->username = strdup(rcheevos_locals->username);
   cb_data->game_id = rcheevos_locals->game.id;
   cb_data->hardcore = rcheevos_locals->hardcore_active ? 1 : 0;
   cb_data->timestamp = time(NULL);
   request->callback = rcheevos_client_award_achievement_callback;
   request->callback_data = cb_data;

   rcheevos_async_begin_request(request, result, rcheevos_async_award_achievement_callback, cb_data,
                                CHEEVOS_ASYNC_AWARD_ACHIEVEMENT, achievement_id,
                                "Awarded achievement", "Error awarding achievement");
}

/****************************
 * submit leaderboard       *
 ****************************/

static void rcheevos_async_submit_lboard_entry_callback(struct rcheevos_async_io_request *request,
                                                        http_transfer_data_t *data, char buffer[],
                                                        size_t buffer_size, void *handler_data)
{
   rc_api_submit_lboard_entry_response_t api_response;
   int result = rc_api_process_submit_lboard_entry_response(&api_response, data->data);

   /* not currently doing anything with the response */
   if (rcheevos_async_succeeded(result, &api_response.response, buffer, buffer_size))
   {
   }

   rc_api_destroy_submit_lboard_entry_response(&api_response);
}

void rcheevos_client_submit_lboard_entry(unsigned leaderboard_id, int value)
{
   rcheevos_async_io_request *request =
      (rcheevos_async_io_request *) calloc(1, sizeof(rcheevos_async_io_request));
   if (!request)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to allocate request for lboard %u submit\n", leaderboard_id);
   }
   else
   {
      const rcheevos_locals_t *rcheevos_locals = get_rcheevos_locals();
      rc_api_submit_lboard_entry_request_t api_params;
      int result;

      memset(&api_params, 0, sizeof(api_params));
      api_params.username = rcheevos_locals->username;
      api_params.api_token = rcheevos_locals->token;
      api_params.leaderboard_id = leaderboard_id;
      api_params.score = value;
      api_params.game_hash = rcheevos_locals->game.hash;

      result = rc_api_init_submit_lboard_entry_request(&request->request, &api_params);

      rcheevos_async_begin_request(request, result, rcheevos_async_submit_lboard_entry_callback, NULL,
                                   CHEEVOS_ASYNC_SUBMIT_LBOARD, leaderboard_id,
                                   "Submitted leaderboard", "Error submitting leaderboard");
   }
}
