/* Copyright (C) 2024 The RetroArch team
 *
 * Contract tests for cheevos_client.c. Uses public entry points only.
 */

#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "../../configuration.h"
#include <compat/strl.h>

#include <rc_api_runtime.h>
#include <rc_api_user.h>
#include <rc_error.h>

#include "../cheevos_cache.h"
#include "../cheevos_client.h"

#define SUITE_NAME "cheevos_client"

static settings_t s_settings;
static char s_tmpdir[PATH_MAX_LENGTH];
static rcheevos_locals_t s_locals;
static runloop_state_t s_runloop;
static bool s_mastery_placard_shown;
static retro_task_t *s_task_queue[32];
static unsigned s_task_queue_count;

typedef struct
{
   int init_result;
   int process_result;
   bool response_succeeded;
   const char *error_message;
} mock_api_result_t;

typedef struct
{
   mock_api_result_t login;
   mock_api_result_t resolve_hash;
   mock_api_result_t fetch_game_data;
   mock_api_result_t award_achievement;
   mock_api_result_t submit_lboard;
   mock_api_result_t start_session;
   mock_api_result_t fetch_image;
   mock_api_result_t ping;
   mock_api_result_t fetch_user_unlocks[2];

   struct { unsigned count; char username[64]; char password[64]; char token[64]; } login_init;
   struct { unsigned count; char hash[96]; } resolve_hash_init;
   struct { unsigned count; char username[64]; char token[64]; unsigned game_id; } fetch_game_data_init;
   struct { unsigned count[2]; char username[64]; char token[64]; unsigned game_id; } fetch_user_unlocks_init;
   struct { unsigned count; unsigned achievement_id; int hardcore; } award_init;

   struct {
      unsigned total_count;
      unsigned by_url_login;
      unsigned by_url_resolve_hash;
      unsigned by_url_fetch_game_data;
      unsigned by_url_fetch_unlocks_softcore;
      unsigned by_url_fetch_unlocks_hardcore;
      unsigned by_url_award;
      unsigned by_url_ping;
   } http;

   bool defer_http_callbacks;
   struct {
      retro_task_callback_t cb;
      void *userdata;
      char *body;
      int status;
   } pending_http[16];
   unsigned pending_http_count;

   int ping_http_statuses[8];
   unsigned ping_http_status_count;
   unsigned ping_http_status_index;

   struct {
      bool enabled;
      bool succeeded;
      const char *display_name;
      const char *username;
      const char *api_token;
      const char *error_message;
   } login_response;

   struct {
      bool enabled;
      bool succeeded;
      unsigned game_id;
      const char *error_message;
   } resolve_hash_response;

   struct {
      bool enabled;
      bool succeeded;
      unsigned id;
      unsigned console_id;
      const char *title;
      const char *image_name;
      const char *rich_presence_script;
      rc_api_achievement_definition_t *achievements;
      unsigned num_achievements;
      rc_api_leaderboard_definition_t *leaderboards;
      unsigned num_leaderboards;
      const char *error_message;
   } fetch_game_data_response;

   struct {
      bool enabled;
      bool succeeded;
      const unsigned *ids;
      unsigned num_ids;
      const char *error_message;
   } fetch_unlocks_response[2];

   struct {
      bool enabled;
      bool succeeded;
      unsigned awarded_id;
      int achievements_remaining;
      const char *error_message;
   } award_response;
} test_http_mock_state_t;

static test_http_mock_state_t s_http_mock;

static void mock_reset_defaults(void)
{
   memset(&s_http_mock, 0, sizeof(s_http_mock));
   s_http_mock.login.init_result = RC_MISSING_VALUE;
   s_http_mock.login.process_result = RC_MISSING_VALUE;
   s_http_mock.resolve_hash.init_result = RC_MISSING_VALUE;
   s_http_mock.resolve_hash.process_result = RC_MISSING_VALUE;
   s_http_mock.fetch_game_data.init_result = RC_MISSING_VALUE;
   s_http_mock.fetch_game_data.process_result = RC_MISSING_VALUE;
   s_http_mock.fetch_user_unlocks[0].init_result = RC_MISSING_VALUE;
   s_http_mock.fetch_user_unlocks[0].process_result = RC_MISSING_VALUE;
   s_http_mock.fetch_user_unlocks[1].init_result = RC_MISSING_VALUE;
   s_http_mock.fetch_user_unlocks[1].process_result = RC_MISSING_VALUE;
   s_http_mock.award_achievement.init_result = RC_MISSING_VALUE;
   s_http_mock.award_achievement.process_result = RC_MISSING_VALUE;
   s_http_mock.submit_lboard.init_result = RC_MISSING_VALUE;
   s_http_mock.submit_lboard.process_result = RC_MISSING_VALUE;
   s_http_mock.start_session.init_result = RC_MISSING_VALUE;
   s_http_mock.start_session.process_result = RC_MISSING_VALUE;
   s_http_mock.fetch_image.init_result = RC_MISSING_VALUE;
   s_http_mock.ping.init_result = RC_MISSING_VALUE;
}

static void mock_fill_request(rc_api_request_t *r, const char *url, const char *post_data)
{
   memset(r, 0, sizeof(*r));
   if (url) r->url = strdup(url);
   if (post_data) r->post_data = strdup(post_data);
}

static bool mock_str_contains(const char *s, const char *needle)
{
   return s && needle && strstr(s, needle) != NULL;
}

static void mock_http_dispatch(const char *url, retro_task_callback_t cb, void *userdata)
{
   static char body_login[] = "mock:login";
   static char body_resolve_hash[] = "mock:resolve_hash";
   static char body_fetch_game_data[] = "mock:fetch_game_data";
   static char body_fetch_unlocks_soft[] = "mock:fetch_user_unlocks:0";
   static char body_fetch_unlocks_hard[] = "mock:fetch_user_unlocks:1";
   static char body_award[] = "mock:award";
   static char body_other[] = "mock:other";
   http_transfer_data_t transfer;
   char *body = body_other;

   memset(&transfer, 0, sizeof(transfer));
   transfer.status = 200;

   s_http_mock.http.total_count++;
   if (mock_str_contains(url, "mock://login"))
   {
      s_http_mock.http.by_url_login++;
      body = body_login;
   }
   else if (mock_str_contains(url, "mock://resolve_hash"))
   {
      s_http_mock.http.by_url_resolve_hash++;
      body = body_resolve_hash;
   }
   else if (mock_str_contains(url, "mock://fetch_game_data"))
   {
      s_http_mock.http.by_url_fetch_game_data++;
      body = body_fetch_game_data;
   }
   else if (mock_str_contains(url, "mock://fetch_user_unlocks?hardcore=0"))
   {
      s_http_mock.http.by_url_fetch_unlocks_softcore++;
      body = body_fetch_unlocks_soft;
   }
   else if (mock_str_contains(url, "mock://fetch_user_unlocks?hardcore=1"))
   {
      s_http_mock.http.by_url_fetch_unlocks_hardcore++;
      body = body_fetch_unlocks_hard;
   }
   else if (mock_str_contains(url, "mock://award_achievement"))
   {
      s_http_mock.http.by_url_award++;
      body = body_award;
   }
   else if (mock_str_contains(url, "mock://ping"))
   {
      s_http_mock.http.by_url_ping++;
   }

   if (mock_str_contains(url, "mock://ping") && s_http_mock.ping_http_status_count > 0)
   {
      unsigned idx = s_http_mock.ping_http_status_index;
      if (idx >= s_http_mock.ping_http_status_count)
         idx = s_http_mock.ping_http_status_count - 1;
      transfer.status = s_http_mock.ping_http_statuses[idx];
      if (s_http_mock.ping_http_status_index < s_http_mock.ping_http_status_count)
         s_http_mock.ping_http_status_index++;
   }

   if (s_http_mock.defer_http_callbacks)
   {
      ck_assert_uint_lt(s_http_mock.pending_http_count, 16);
      s_http_mock.pending_http[s_http_mock.pending_http_count].cb = cb;
      s_http_mock.pending_http[s_http_mock.pending_http_count].userdata = userdata;
      s_http_mock.pending_http[s_http_mock.pending_http_count].body = body;
      s_http_mock.pending_http[s_http_mock.pending_http_count].status = transfer.status;
      s_http_mock.pending_http_count++;
      return;
   }

   transfer.data = body;
   transfer.len = strlen(body);
   cb(NULL, &transfer, userdata, NULL);
}

static void mock_http_flush_all(void)
{
   while (s_http_mock.pending_http_count > 0)
   {
      http_transfer_data_t transfer;
      retro_task_callback_t cb = s_http_mock.pending_http[0].cb;
      void *userdata = s_http_mock.pending_http[0].userdata;
      char *body = s_http_mock.pending_http[0].body;
      int status = s_http_mock.pending_http[0].status;
      unsigned i;

      for (i = 1; i < s_http_mock.pending_http_count; i++)
         s_http_mock.pending_http[i - 1] = s_http_mock.pending_http[i];
      s_http_mock.pending_http_count--;

      memset(&transfer, 0, sizeof(transfer));
      transfer.status = status;
      transfer.data = body;
      transfer.len = strlen(body);
      cb(NULL, &transfer, userdata, NULL);
   }
}

static void mock_configure_login_success(const char *username, const char *display_name, const char *token)
{
   s_http_mock.login.init_result = RC_OK;
   s_http_mock.login.process_result = RC_OK;
   s_http_mock.login_response.enabled = true;
   s_http_mock.login_response.succeeded = true;
   s_http_mock.login_response.username = username;
   s_http_mock.login_response.display_name = display_name;
   s_http_mock.login_response.api_token = token;
}

static void mock_configure_login_failure(const char *error_message)
{
   s_http_mock.login.init_result = RC_OK;
   s_http_mock.login.process_result = RC_OK;
   s_http_mock.login_response.enabled = true;
   s_http_mock.login_response.succeeded = false;
   s_http_mock.login_response.error_message = error_message ? error_message : "login failed";
}

static void mock_configure_resolve_hash_success(unsigned game_id)
{
   s_http_mock.resolve_hash.init_result = RC_OK;
   s_http_mock.resolve_hash.process_result = RC_OK;
   s_http_mock.resolve_hash_response.enabled = true;
   s_http_mock.resolve_hash_response.succeeded = true;
   s_http_mock.resolve_hash_response.game_id = game_id;
}

static void mock_configure_fetch_game_data_success(
   unsigned id, unsigned console_id, const char *title, const char *image_name,
   const char *rich_presence_script, rc_api_achievement_definition_t *achievements,
   unsigned num_achievements, rc_api_leaderboard_definition_t *leaderboards,
   unsigned num_leaderboards)
{
   s_http_mock.fetch_game_data.init_result = RC_OK;
   s_http_mock.fetch_game_data.process_result = RC_OK;
   s_http_mock.fetch_game_data_response.enabled = true;
   s_http_mock.fetch_game_data_response.succeeded = true;
   s_http_mock.fetch_game_data_response.id = id;
   s_http_mock.fetch_game_data_response.console_id = console_id;
   s_http_mock.fetch_game_data_response.title = title;
   s_http_mock.fetch_game_data_response.image_name = image_name;
   s_http_mock.fetch_game_data_response.rich_presence_script = rich_presence_script;
   s_http_mock.fetch_game_data_response.achievements = achievements;
   s_http_mock.fetch_game_data_response.num_achievements = num_achievements;
   s_http_mock.fetch_game_data_response.leaderboards = leaderboards;
   s_http_mock.fetch_game_data_response.num_leaderboards = num_leaderboards;
}

static void mock_configure_fetch_game_data_failure(const char *error_message)
{
   s_http_mock.fetch_game_data.init_result = RC_OK;
   s_http_mock.fetch_game_data.process_result = RC_OK;
   s_http_mock.fetch_game_data_response.enabled = true;
   s_http_mock.fetch_game_data_response.succeeded = false;
   s_http_mock.fetch_game_data_response.error_message = error_message ? error_message : "fetch failed";
}

static void mock_configure_fetch_unlocks_success(int hardcore, const unsigned *ids, unsigned num_ids)
{
   s_http_mock.fetch_user_unlocks[hardcore].init_result = RC_OK;
   s_http_mock.fetch_user_unlocks[hardcore].process_result = RC_OK;
   s_http_mock.fetch_unlocks_response[hardcore].enabled = true;
   s_http_mock.fetch_unlocks_response[hardcore].succeeded = true;
   s_http_mock.fetch_unlocks_response[hardcore].ids = ids;
   s_http_mock.fetch_unlocks_response[hardcore].num_ids = num_ids;
}

static void mock_configure_fetch_unlocks_failure(int hardcore, const char *error_message)
{
   s_http_mock.fetch_user_unlocks[hardcore].init_result = RC_OK;
   s_http_mock.fetch_user_unlocks[hardcore].process_result = RC_OK;
   s_http_mock.fetch_unlocks_response[hardcore].enabled = true;
   s_http_mock.fetch_unlocks_response[hardcore].succeeded = false;
   s_http_mock.fetch_unlocks_response[hardcore].error_message =
      error_message ? error_message : "fetch unlocks failed";
}

static void mock_configure_ping_status_sequence(const int *statuses, unsigned count)
{
   unsigned i;
   ck_assert_uint_le(count, 8);
   s_http_mock.ping_http_status_count = count;
   s_http_mock.ping_http_status_index = 0;
   for (i = 0; i < count; i++)
      s_http_mock.ping_http_statuses[i] = statuses[i];
}

/* ---- RetroArch/global stubs ---- */
frontend_ctx_driver_t *frontend_get_ptr(void) { return NULL; }
settings_t *config_get_ptr(void) { return &s_settings; }
rcheevos_locals_t *get_rcheevos_locals(void) { return &s_locals; }
void rcheevos_show_mastery_placard(void) { s_mastery_placard_shown = true; }
runloop_state_t *runloop_state_get_ptr(void) { return &s_runloop; }
void runloop_msg_queue_push(const char *msg, unsigned prio, unsigned duration,
                            bool flush, char *title, enum message_queue_icon icon,
                            enum message_queue_category category)
{ (void)msg; (void)prio; (void)duration; (void)flush; (void)title; (void)icon; (void)category; }

/* load-state stubs */
void rcheevos_begin_load_state(enum rcheevos_load_state state) { (void)state; }
int rcheevos_end_load_state(void) { return 0; }
bool rcheevos_load_aborted(void) { return false; }

/* cheevos.h function stubs */
bool rcheevos_unload(void) { return true; }
bool rcheevos_hardcore_active(void) { return false; }
int rcheevos_get_richpresence(char *s, size_t len) { (void)s; (void)len; return 0; }

/* task system stubs */
void task_set_finished(retro_task_t *task, bool finished) { task->finished = finished; }
void *task_push_http_transfer_with_user_agent(const char *url, bool mute, const char *type,
      const char *user_agent, retro_task_callback_t cb, void *userdata)
{ (void)mute; (void)type; (void)user_agent; ck_assert_ptr_nonnull(cb); mock_http_dispatch(url, cb, userdata); return NULL; }
void *task_push_http_post_transfer_with_user_agent(const char *url, const char *post_data, bool mute, const char *type,
      const char *user_agent, retro_task_callback_t cb, void *userdata)
{ (void)post_data; (void)mute; (void)type; (void)user_agent; ck_assert_ptr_nonnull(cb); mock_http_dispatch(url, cb, userdata); return NULL; }
retro_task_t *task_init(void) { return (retro_task_t*)calloc(1, sizeof(retro_task_t)); }
bool task_queue_push(retro_task_t *task) { ck_assert_uint_lt(s_task_queue_count, 32); s_task_queue[s_task_queue_count++] = task; return true; }

/* misc */
retro_time_t cpu_features_get_time_usec(void) { return 0; }
const char *path_get(enum rarch_path_type type) { (void)type; return ""; }
void presence_update(enum presence p) { (void)p; }

/* rcheevos API stubs */
const char *rc_error_str(int err) { (void)err; return "stub"; }
void rc_api_set_host(const char *h) { (void)h; }
void rc_api_set_image_host(const char *h) { (void)h; }
void rc_api_destroy_request(rc_api_request_t *r)
{
   if (!r) return;
   free((void*)r->url);
   free((void*)r->post_data);
   r->url = NULL;
   r->post_data = NULL;
}

int rc_api_init_login_request(rc_api_request_t *r, const rc_api_login_request_t *p)
{
   s_http_mock.login_init.count++;
   if (p)
   {
      strlcpy(s_http_mock.login_init.username, p->username ? p->username : "", sizeof(s_http_mock.login_init.username));
      strlcpy(s_http_mock.login_init.password, p->password ? p->password : "", sizeof(s_http_mock.login_init.password));
      strlcpy(s_http_mock.login_init.token, p->api_token ? p->api_token : "", sizeof(s_http_mock.login_init.token));
   }
   if (s_http_mock.login.init_result == RC_OK) mock_fill_request(r, "mock://login", "u=1");
   return s_http_mock.login.init_result;
}
int rc_api_process_login_response(rc_api_login_response_t *r, const char *s)
{
   (void)s;
   memset(r, 0, sizeof(*r));
   if (!s_http_mock.login_response.enabled) return s_http_mock.login.process_result;
   r->response.succeeded = s_http_mock.login_response.succeeded;
   r->response.error_message = s_http_mock.login_response.error_message;
   r->display_name = s_http_mock.login_response.display_name ? s_http_mock.login_response.display_name : "";
   r->username = s_http_mock.login_response.username ? s_http_mock.login_response.username : "";
   r->api_token = s_http_mock.login_response.api_token ? s_http_mock.login_response.api_token : "";
   return s_http_mock.login.process_result;
}
void rc_api_destroy_login_response(rc_api_login_response_t *r) { (void)r; }

int rc_api_init_resolve_hash_request(rc_api_request_t *r, const rc_api_resolve_hash_request_t *p)
{
   s_http_mock.resolve_hash_init.count++;
   if (p) strlcpy(s_http_mock.resolve_hash_init.hash, p->game_hash ? p->game_hash : "", sizeof(s_http_mock.resolve_hash_init.hash));
   if (s_http_mock.resolve_hash.init_result == RC_OK) mock_fill_request(r, "mock://resolve_hash", "h=1");
   return s_http_mock.resolve_hash.init_result;
}
int rc_api_process_resolve_hash_response(rc_api_resolve_hash_response_t *r, const char *s)
{
   (void)s;
   memset(r, 0, sizeof(*r));
   if (!s_http_mock.resolve_hash_response.enabled) return s_http_mock.resolve_hash.process_result;
   r->response.succeeded = s_http_mock.resolve_hash_response.succeeded;
   r->response.error_message = s_http_mock.resolve_hash_response.error_message;
   r->game_id = s_http_mock.resolve_hash_response.game_id;
   return s_http_mock.resolve_hash.process_result;
}
void rc_api_destroy_resolve_hash_response(rc_api_resolve_hash_response_t *r) { (void)r; }

int rc_api_init_fetch_game_data_request(rc_api_request_t *r, const rc_api_fetch_game_data_request_t *p)
{
   s_http_mock.fetch_game_data_init.count++;
   if (p)
   {
      strlcpy(s_http_mock.fetch_game_data_init.username, p->username ? p->username : "", sizeof(s_http_mock.fetch_game_data_init.username));
      strlcpy(s_http_mock.fetch_game_data_init.token, p->api_token ? p->api_token : "", sizeof(s_http_mock.fetch_game_data_init.token));
      s_http_mock.fetch_game_data_init.game_id = p->game_id;
   }
   if (s_http_mock.fetch_game_data.init_result == RC_OK) mock_fill_request(r, "mock://fetch_game_data", "g=1");
   return s_http_mock.fetch_game_data.init_result;
}
int rc_api_process_fetch_game_data_response(rc_api_fetch_game_data_response_t *r, const char *s)
{
   (void)s;
   memset(r, 0, sizeof(*r));
   if (!s_http_mock.fetch_game_data_response.enabled) return s_http_mock.fetch_game_data.process_result;
   r->response.succeeded = s_http_mock.fetch_game_data_response.succeeded;
   r->response.error_message = s_http_mock.fetch_game_data_response.error_message;
   r->id = s_http_mock.fetch_game_data_response.id;
   r->console_id = s_http_mock.fetch_game_data_response.console_id;
   r->title = s_http_mock.fetch_game_data_response.title;
   r->image_name = s_http_mock.fetch_game_data_response.image_name;
   r->rich_presence_script = s_http_mock.fetch_game_data_response.rich_presence_script;
   r->achievements = s_http_mock.fetch_game_data_response.achievements;
   r->num_achievements = s_http_mock.fetch_game_data_response.num_achievements;
   r->leaderboards = s_http_mock.fetch_game_data_response.leaderboards;
   r->num_leaderboards = s_http_mock.fetch_game_data_response.num_leaderboards;
   return s_http_mock.fetch_game_data.process_result;
}
void rc_api_destroy_fetch_game_data_response(rc_api_fetch_game_data_response_t *r)
{
   unsigned i;
   if (!r)
      return;

   if (r->achievements && r->achievements != s_http_mock.fetch_game_data_response.achievements)
   {
      for (i = 0; i < r->num_achievements; i++)
      {
         free((void*)r->achievements[i].author);
         free((void*)r->achievements[i].badge_name);
         free((void*)r->achievements[i].definition);
         free((void*)r->achievements[i].description);
         free((void*)r->achievements[i].title);
      }
      free(r->achievements);
   }

   if (r->leaderboards && r->leaderboards != s_http_mock.fetch_game_data_response.leaderboards)
   {
      for (i = 0; i < r->num_leaderboards; i++)
      {
         free((void*)r->leaderboards[i].definition);
         free((void*)r->leaderboards[i].description);
         free((void*)r->leaderboards[i].title);
      }
      free(r->leaderboards);
   }

   if (r->title && r->title != s_http_mock.fetch_game_data_response.title)
      free((void*)r->title);
   if (r->image_name && r->image_name != s_http_mock.fetch_game_data_response.image_name)
      free((void*)r->image_name);
   if (r->rich_presence_script && r->rich_presence_script != s_http_mock.fetch_game_data_response.rich_presence_script)
      free((void*)r->rich_presence_script);

   memset(r, 0, sizeof(*r));
}

int rc_api_init_fetch_user_unlocks_request(rc_api_request_t *r, const rc_api_fetch_user_unlocks_request_t *p)
{
   int hardcore = (p && p->hardcore) ? 1 : 0;
   s_http_mock.fetch_user_unlocks_init.count[hardcore]++;
   if (p)
   {
      strlcpy(s_http_mock.fetch_user_unlocks_init.username, p->username ? p->username : "", sizeof(s_http_mock.fetch_user_unlocks_init.username));
      strlcpy(s_http_mock.fetch_user_unlocks_init.token, p->api_token ? p->api_token : "", sizeof(s_http_mock.fetch_user_unlocks_init.token));
      s_http_mock.fetch_user_unlocks_init.game_id = p->game_id;
   }
   if (s_http_mock.fetch_user_unlocks[hardcore].init_result == RC_OK)
      mock_fill_request(r, hardcore ? "mock://fetch_user_unlocks?hardcore=1" : "mock://fetch_user_unlocks?hardcore=0", "u=1");
   return s_http_mock.fetch_user_unlocks[hardcore].init_result;
}
int rc_api_process_fetch_user_unlocks_response(rc_api_fetch_user_unlocks_response_t *r, const char *s)
{
   int hardcore = (s && strstr(s, ":1")) ? 1 : 0;
   memset(r, 0, sizeof(*r));
   if (!s_http_mock.fetch_unlocks_response[hardcore].enabled) return s_http_mock.fetch_user_unlocks[hardcore].process_result;
   r->response.succeeded = s_http_mock.fetch_unlocks_response[hardcore].succeeded;
   r->response.error_message = s_http_mock.fetch_unlocks_response[hardcore].error_message;
   r->achievement_ids = (unsigned*)s_http_mock.fetch_unlocks_response[hardcore].ids;
   r->num_achievement_ids = s_http_mock.fetch_unlocks_response[hardcore].num_ids;
   return s_http_mock.fetch_user_unlocks[hardcore].process_result;
}
void rc_api_destroy_fetch_user_unlocks_response(rc_api_fetch_user_unlocks_response_t *r)
{
   if (!r)
      return;

   if (r->achievement_ids &&
       r->achievement_ids != s_http_mock.fetch_unlocks_response[0].ids &&
       r->achievement_ids != s_http_mock.fetch_unlocks_response[1].ids)
      free(r->achievement_ids);

   r->achievement_ids = NULL;
   r->num_achievement_ids = 0;
}

int rc_api_init_award_achievement_request(rc_api_request_t *r, const rc_api_award_achievement_request_t *p)
{
   s_http_mock.award_init.count++;
   if (p)
   {
      s_http_mock.award_init.achievement_id = p->achievement_id;
      s_http_mock.award_init.hardcore = p->hardcore;
   }
   if (s_http_mock.award_achievement.init_result == RC_OK) mock_fill_request(r, "mock://award_achievement", "a=1");
   return s_http_mock.award_achievement.init_result;
}
int rc_api_process_award_achievement_response(rc_api_award_achievement_response_t *r, const char *s)
{
   (void)s;
   memset(r, 0, sizeof(*r));
   if (!s_http_mock.award_response.enabled) return s_http_mock.award_achievement.process_result;
   r->response.succeeded = s_http_mock.award_response.succeeded;
   r->response.error_message = s_http_mock.award_response.error_message;
   r->awarded_achievement_id = s_http_mock.award_response.awarded_id ? s_http_mock.award_response.awarded_id : s_http_mock.award_init.achievement_id;
   r->achievements_remaining = s_http_mock.award_response.achievements_remaining;
   return s_http_mock.award_achievement.process_result;
}
void rc_api_destroy_award_achievement_response(rc_api_award_achievement_response_t *r) { (void)r; }

int rc_api_init_submit_lboard_entry_request(rc_api_request_t *r, const rc_api_submit_lboard_entry_request_t *p) { (void)r; (void)p; return s_http_mock.submit_lboard.init_result; }
int rc_api_process_submit_lboard_entry_response(rc_api_submit_lboard_entry_response_t *r, const char *s) { (void)r; (void)s; return s_http_mock.submit_lboard.process_result; }
void rc_api_destroy_submit_lboard_entry_response(rc_api_submit_lboard_entry_response_t *r) { (void)r; }

int rc_api_init_start_session_request(rc_api_request_t *r, const rc_api_start_session_request_t *p)
{
   (void)p;
   if (s_http_mock.start_session.init_result == RC_OK) mock_fill_request(r, "mock://start_session", "s=1");
   return s_http_mock.start_session.init_result;
}
int rc_api_process_start_session_response(rc_api_start_session_response_t *r, const char *s)
{
   (void)s;
   memset(r, 0, sizeof(*r));
   if (s_http_mock.start_session.process_result == RC_OK) r->response.succeeded = true;
   return s_http_mock.start_session.process_result;
}
void rc_api_destroy_start_session_response(rc_api_start_session_response_t *r) { (void)r; }

int rc_api_init_fetch_image_request(rc_api_request_t *r, const rc_api_fetch_image_request_t *p) { (void)r; (void)p; return s_http_mock.fetch_image.init_result; }
int rc_api_init_ping_request(rc_api_request_t *r, const rc_api_ping_request_t *p)
{
   (void)p;
   if (s_http_mock.ping.init_result == RC_OK) mock_fill_request(r, "mock://ping", "p=1");
   return s_http_mock.ping.init_result;
}
int rc_runtime_activate_richpresence(rc_runtime_t *r, const char *s, lua_State *L, int idx) { (void)r; (void)s; (void)L; (void)idx; return RC_MISSING_VALUE; }

/* ---- Fixture & helpers ---- */
static void fixture_setup(void)
{
   char tmpl[] = "/tmp/test_cheevos_client_XXXXXX";
   char *dir = mkdtemp(tmpl);
   ck_assert_ptr_nonnull(dir);

   memset(&s_settings, 0, sizeof(s_settings));
   memset(&s_locals, 0, sizeof(s_locals));
   memset(&s_runloop, 0, sizeof(s_runloop));
   memset(s_task_queue, 0, sizeof(s_task_queue));
   s_task_queue_count = 0;
   mock_reset_defaults();
   s_mastery_placard_shown = false;

   strlcpy(s_tmpdir, dir, sizeof(s_tmpdir));
   strlcpy(s_settings.paths.directory_thumbnails, dir, sizeof(s_settings.paths.directory_thumbnails));
   s_settings.bools.cheevos_cache_enabled = true;

   strlcpy(s_locals.username, "player1", sizeof(s_locals.username));
   s_locals.game.id = 777;
}

static void fixture_teardown(void)
{
   unsigned i;
   char cmd[PATH_MAX_LENGTH + 16];
   for (i = 0; i < s_task_queue_count; i++)
      free(s_task_queue[i]);
   s_task_queue_count = 0;
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", s_tmpdir);
   system(cmd);
}

static void counted_callback(void *data) { (*(int *)data)++; }

static void cleanup_installed_game_data(void)
{
   unsigned i;
   for (i = 0; i < s_locals.game.achievement_count; i++)
   {
      free((void*)s_locals.game.achievements[i].title);
      free((void*)s_locals.game.achievements[i].description);
      free((void*)s_locals.game.achievements[i].badge);
      free((void*)s_locals.game.achievements[i].memaddr);
   }
   free(s_locals.game.achievements);
   s_locals.game.achievements = NULL;
   s_locals.game.achievement_count = 0;

   for (i = 0; i < s_locals.game.leaderboard_count; i++)
   {
      free((void*)s_locals.game.leaderboards[i].title);
      free((void*)s_locals.game.leaderboards[i].description);
      free((void*)s_locals.game.leaderboards[i].mem);
   }
   free(s_locals.game.leaderboards);
   s_locals.game.leaderboards = NULL;
   s_locals.game.leaderboard_count = 0;

   free(s_locals.pending_achievement_queue);
   s_locals.pending_achievement_queue = NULL;
   s_locals.pending_achievement_queue_size = 0;
}

static void seed_cached_game_and_unlocks(uint32_t game_id, const char *username)
{
   rcheevos_cache_achievement_t cache_ach;
   rcheevos_cache_game_t cache_game;
   rcheevos_cache_user_unlocks_t unlocks;

   memset(&cache_ach, 0, sizeof(cache_ach));
   cache_ach.id = 9001;
   cache_ach.points = 5;
   cache_ach.category = 3;
   cache_ach.title = "Cached Achievement";
   cache_ach.description = "From disk cache";
   cache_ach.definition = "0xH1234=1";
   cache_ach.author = "tester";
   cache_ach.badge_name = "00001";

   memset(&cache_game, 0, sizeof(cache_game));
   cache_game.id = game_id;
   cache_game.console_id = 4;
   cache_game.title = "Cached Game";
   cache_game.image_name = "00000";
   cache_game.rich_presence_script = "";
   cache_game.cached_at = time(NULL);
   cache_game.achievements = &cache_ach;
   cache_game.num_achievements = 1;
   ck_assert(rcheevos_cache_save_game_data(&cache_game));

   memset(&unlocks, 0, sizeof(unlocks));
   unlocks.username = (char*)username;
   unlocks.last_updated = time(NULL);
   ck_assert(rcheevos_cache_save_user_unlocks(username, game_id, false, &unlocks));
   ck_assert(rcheevos_cache_save_user_unlocks(username, game_id, true, &unlocks));
}

static void run_initialize_runtime_and_flush(unsigned game_id, int *callback_count)
{
   s_http_mock.defer_http_callbacks = true;
   rcheevos_client_initialize_runtime(game_id, counted_callback, callback_count);
   mock_http_flush_all();
   s_http_mock.defer_http_callbacks = false;
}

static void run_task_handlers_once(void)
{
   unsigned i;
   for (i = 0; i < s_task_queue_count; i++)
      if (s_task_queue[i] && !s_task_queue[i]->finished && s_task_queue[i]->handler)
         s_task_queue[i]->handler(s_task_queue[i]);
}

static void compact_finished_tasks(void)
{
   unsigned i, out = 0;
   for (i = 0; i < s_task_queue_count; i++)
   {
      retro_task_t *task = s_task_queue[i];
      if (!task) continue;
      if (task->finished) { free(task); continue; }
      s_task_queue[out++] = task;
   }
   s_task_queue_count = out;
}

static void assert_single_installed_achievement(unsigned id, const char *memaddr)
{
   ck_assert_uint_eq(s_locals.game.achievement_count, 1);
   ck_assert_ptr_nonnull(s_locals.game.achievements);
   ck_assert_uint_eq(s_locals.game.achievements[0].id, id);
   ck_assert_ptr_nonnull(s_locals.game.achievements[0].memaddr);
   ck_assert_str_eq(s_locals.game.achievements[0].memaddr, memaddr);
}

static bool unlock_cache_contains_achievement(const char *username, uint32_t game_id, bool hardcore, uint32_t achievement_id)
{
   rcheevos_cache_user_unlocks_t unlocks;
   uint32_t i;
   bool found = false;
   memset(&unlocks, 0, sizeof(unlocks));
   if (!rcheevos_cache_get_user_unlocks(username, game_id, hardcore, &unlocks))
      return false;
   for (i = 0; i < unlocks.num_unlocks; i++)
      if (unlocks.unlocks[i].achievement_id == achievement_id) found = true;
   rcheevos_cache_unlocks_free(&unlocks);
   return found;
}

/* ---- Tests ---- */
START_TEST(test_contract_login_success_dispatches_request_and_fires_callback)
{
   int count = 0;
   mock_configure_login_success("player1", "Player One", "tok123");
   rcheevos_client_login_with_password("player1", "pw", counted_callback, &count);
   ck_assert_int_eq(count, 1);
   ck_assert_uint_eq(s_http_mock.login_init.count, 1);
   ck_assert_str_eq(s_http_mock.login_init.username, "player1");
   ck_assert_str_eq(s_http_mock.login_init.password, "pw");
   ck_assert_uint_eq(s_http_mock.http.by_url_login, 1);
}
END_TEST

START_TEST(test_contract_login_failure_still_fires_callback)
{
   int count = 0;
   mock_configure_login_failure("bad credentials");
   rcheevos_client_login_with_password("player1", "badpw", counted_callback, &count);
   ck_assert_int_eq(count, 1);
   ck_assert_uint_eq(s_http_mock.login_init.count, 1);
   ck_assert_uint_eq(s_http_mock.http.by_url_login, 1);
}
END_TEST

START_TEST(test_contract_identify_game_remote_saves_hash_mapping)
{
   int count = 0;
   rcheevos_cache_hash_t cached;
   mock_configure_resolve_hash_success(42001);
   rcheevos_client_identify_game("feedfacecafebeef", counted_callback, &count);
   ck_assert_int_eq(count, 1);
   ck_assert_uint_eq(s_http_mock.resolve_hash_init.count, 1);
   ck_assert_str_eq(s_http_mock.resolve_hash_init.hash, "feedfacecafebeef");
   ck_assert_uint_eq(s_http_mock.http.by_url_resolve_hash, 1);
   ck_assert(rcheevos_cache_get_game_id_for_hash("feedfacecafebeef", &cached));
   ck_assert_uint_eq(cached.game_id, 42001);
}
END_TEST

START_TEST(test_contract_award_achievement_success_updates_unlock_cache)
{
   rcheevos_cache_user_unlocks_t seed, result;
   uint32_t i;
   bool found = false;

   memset(&seed, 0, sizeof(seed));
   seed.username = "player1";
   ck_assert(rcheevos_cache_save_user_unlocks("player1", 777, false, &seed));

   s_locals.game.id = 777;
   s_locals.hardcore_active = false;
   s_http_mock.award_achievement.init_result = RC_OK;
   s_http_mock.award_achievement.process_result = RC_OK;
   s_http_mock.award_response.enabled = true;
   s_http_mock.award_response.succeeded = true;
   s_http_mock.award_response.awarded_id = 31337;
   s_http_mock.award_response.achievements_remaining = 9;

   rcheevos_client_award_achievement(31337);

   ck_assert_uint_eq(s_http_mock.award_init.count, 1);
   ck_assert_uint_eq(s_http_mock.award_init.achievement_id, 31337);
   ck_assert_uint_eq(s_http_mock.http.by_url_award, 1);

   memset(&result, 0, sizeof(result));
   ck_assert(rcheevos_cache_get_user_unlocks("player1", 777, false, &result));
   for (i = 0; i < result.num_unlocks; i++)
      if (result.unlocks[i].achievement_id == 31337) found = true;
   ck_assert(found);
   rcheevos_cache_unlocks_free(&result);
}
END_TEST

START_TEST(test_contract_award_achievement_failure_queues_pending)
{
   rcheevos_cache_pending_list_t pending;
   s_locals.game.id = 777;
   s_locals.hardcore_active = true;
   s_http_mock.award_achievement.init_result = RC_OK;
   s_http_mock.award_achievement.process_result = RC_OK;
   s_http_mock.award_response.enabled = true;
   s_http_mock.award_response.succeeded = false;
   s_http_mock.award_response.error_message = "offline";

   rcheevos_client_award_achievement(404);

   ck_assert_uint_eq(s_http_mock.award_init.count, 1);
   ck_assert_uint_eq(s_http_mock.http.by_url_award, 1);
   memset(&pending, 0, sizeof(pending));
   ck_assert(rcheevos_cache_get_pending_unlocks("player1", 777, &pending));
   ck_assert_uint_eq(pending.num_entries, 1);
   ck_assert_uint_eq(pending.entries[0].id, 404);
   ck_assert_int_eq(pending.entries[0].hardcore, true);
   rcheevos_cache_pending_list_free(&pending);
}
END_TEST

START_TEST(test_contract_initialize_runtime_login_success_installs_definition)
{
   int login_cb_count = 0, identify_cb_count = 0, init_cb_count = 0;
   rc_api_achievement_definition_t ach_def;

   mock_configure_login_success("player1", "Player One", "tok123");
   mock_configure_resolve_hash_success(777);
   memset(&ach_def, 0, sizeof(ach_def));
   ach_def.id = 901; ach_def.category = 3; ach_def.points = 10;
   ach_def.title = "Remote Achievement"; ach_def.description = "Fetched from server";
   ach_def.definition = "0xHCAFE=1"; ach_def.badge_name = "12345"; ach_def.author = "author";
   mock_configure_fetch_game_data_success(777, 4, "Remote Game", "00000", "", &ach_def, 1, NULL, 0);
   mock_configure_fetch_unlocks_success(0, NULL, 0);
   mock_configure_fetch_unlocks_success(1, NULL, 0);

   rcheevos_client_login_with_password("player1", "pw", counted_callback, &login_cb_count);
   rcheevos_client_identify_game("abc123", counted_callback, &identify_cb_count);
   run_initialize_runtime_and_flush(777, &init_cb_count);

   ck_assert_int_eq(login_cb_count, 1);
   ck_assert_int_eq(identify_cb_count, 1);
   ck_assert_int_eq(init_cb_count, 1);
   ck_assert(s_locals.logged_in);
   ck_assert_uint_eq(s_locals.game.id, 777);
   assert_single_installed_achievement(901, "0xHCAFE=1");
   ck_assert_uint_eq(s_http_mock.http.by_url_login, 1);
   ck_assert_uint_eq(s_http_mock.http.by_url_resolve_hash, 1);
   ck_assert_uint_eq(s_http_mock.http.by_url_fetch_game_data, 1);
   ck_assert_uint_eq(s_http_mock.http.by_url_fetch_unlocks_softcore, 1);
   ck_assert_uint_eq(s_http_mock.http.by_url_fetch_unlocks_hardcore, 1);
   cleanup_installed_game_data();
}
END_TEST

START_TEST(test_contract_initialize_runtime_login_failed_with_network_does_not_install_definition)
{
   int login_cb_count = 0, identify_cb_count = 0, init_cb_count = 0;
   mock_configure_login_failure("unauthorized");
   mock_configure_resolve_hash_success(777);
   mock_configure_fetch_game_data_failure("unauthorized");
   mock_configure_fetch_unlocks_failure(0, "unauthorized");
   mock_configure_fetch_unlocks_failure(1, "unauthorized");

   rcheevos_client_login_with_password("player1", "badpw", counted_callback, &login_cb_count);
   rcheevos_client_identify_game("abc124", counted_callback, &identify_cb_count);
   run_initialize_runtime_and_flush(777, &init_cb_count);

   ck_assert_int_eq(login_cb_count, 1);
   ck_assert_int_eq(identify_cb_count, 1);
   ck_assert_int_eq(init_cb_count, 1);
   ck_assert(!s_locals.logged_in);
   ck_assert_uint_eq(s_locals.game.id, 777);
   ck_assert_uint_eq(s_locals.game.achievement_count, 0);
   ck_assert_uint_eq(s_http_mock.http.by_url_login, 1);
   ck_assert_uint_eq(s_http_mock.http.by_url_resolve_hash, 1);
   ck_assert_uint_eq(s_http_mock.http.by_url_fetch_game_data, 1);
   ck_assert_uint_eq(s_http_mock.http.by_url_fetch_unlocks_softcore, 1);
   ck_assert_uint_eq(s_http_mock.http.by_url_fetch_unlocks_hardcore, 1);
   cleanup_installed_game_data();
}
END_TEST

START_TEST(test_contract_initialize_runtime_no_network_uses_cache_and_installs_definition)
{
   int login_cb_count = 0, identify_cb_count = 0, init_cb_count = 0;
   rcheevos_cache_hash_t hash_map;

   seed_cached_game_and_unlocks(777, "player1");
   hash_map.game_id = 777;
   ck_assert(rcheevos_cache_save_hash_mapping("cachedhash", &hash_map));

   rcheevos_client_login_with_password("player1", "pw", counted_callback, &login_cb_count);
   rcheevos_client_identify_game("cachedhash", counted_callback, &identify_cb_count);
   rcheevos_client_initialize_runtime(777, counted_callback, &init_cb_count);

   ck_assert_int_eq(login_cb_count, 1);
   ck_assert_int_eq(identify_cb_count, 1);
   ck_assert_int_eq(init_cb_count, 1);
   ck_assert(!s_locals.logged_in);
   ck_assert_uint_eq(s_http_mock.http.by_url_login, 0);
   ck_assert_uint_eq(s_http_mock.http.by_url_resolve_hash, 0);
   ck_assert_uint_eq(s_http_mock.http.by_url_fetch_game_data, 0);
   ck_assert_uint_eq(s_http_mock.http.by_url_fetch_unlocks_softcore, 0);
   ck_assert_uint_eq(s_http_mock.http.by_url_fetch_unlocks_hardcore, 0);
   assert_single_installed_achievement(9001, "0xH1234=1");
   cleanup_installed_game_data();
}
END_TEST

START_TEST(test_contract_pending_awards_sync_after_ping_recovers)
{
   rcheevos_cache_user_unlocks_t seed;
   rcheevos_cache_pending_list_t pending;
   int ping_statuses[2] = {500, 200};

   s_locals.logged_in = true;
   strlcpy(s_locals.username, "player1", sizeof(s_locals.username));
   strlcpy(s_locals.token, "tok123", sizeof(s_locals.token));
   s_locals.game.id = 777;
   s_locals.hardcore_active = false;
   memset(&seed, 0, sizeof(seed));
   seed.username = "player1";
   ck_assert(rcheevos_cache_save_user_unlocks("player1", 777, false, &seed));

   s_http_mock.ping.init_result = RC_OK;
   mock_configure_ping_status_sequence(ping_statuses, 2);
   s_http_mock.start_session.init_result = RC_OK;
   s_http_mock.start_session.process_result = RC_OK;
   rcheevos_client_start_session(777);

   run_task_handlers_once(); /* net poll */
   run_task_handlers_once(); /* ping -> fail */
   compact_finished_tasks();

   s_http_mock.award_achievement.init_result = RC_OK;
   s_http_mock.award_achievement.process_result = RC_OK;
   s_http_mock.award_response.enabled = true;
   s_http_mock.award_response.succeeded = false;
   s_http_mock.award_response.error_message = "offline";
   rcheevos_client_award_achievement(1001);
   rcheevos_client_award_achievement(1002);

   memset(&pending, 0, sizeof(pending));
   ck_assert(rcheevos_cache_get_pending_unlocks("player1", 777, &pending));
   ck_assert_uint_eq(pending.num_entries, 2);
   rcheevos_cache_pending_list_free(&pending);

   run_task_handlers_once(); /* poll while offline */
   run_task_handlers_once(); /* ping -> success */

   s_http_mock.award_response.succeeded = true;
   s_http_mock.award_response.error_message = NULL;
   s_http_mock.award_response.awarded_id = 0;
   s_http_mock.award_response.achievements_remaining = 5;
   run_task_handlers_once(); /* poll dispatch and chained syncs */

   memset(&pending, 0, sizeof(pending));
   ck_assert(rcheevos_cache_get_pending_unlocks("player1", 777, &pending));
   ck_assert_uint_eq(pending.num_entries, 0);
   rcheevos_cache_pending_list_free(&pending);

   ck_assert(unlock_cache_contains_achievement("player1", 777, false, 1001));
   ck_assert(unlock_cache_contains_achievement("player1", 777, false, 1002));
   ck_assert_uint_ge(s_http_mock.http.by_url_ping, 2);

   /* Stop recurring session tasks so LSAN can observe clean shutdown in test process. */
   s_locals.game.id = 0;
   run_task_handlers_once();
   run_task_handlers_once();
   compact_finished_tasks();
}
END_TEST

/* ---- Suite ---- */
Suite *create_suite(void)
{
   Suite *s = suite_create(SUITE_NAME);
   TCase *tc = tcase_create("ContractHttpMocks");

   tcase_add_checked_fixture(tc, fixture_setup, fixture_teardown);
   tcase_add_test(tc, test_contract_login_success_dispatches_request_and_fires_callback);
   tcase_add_test(tc, test_contract_login_failure_still_fires_callback);
   tcase_add_test(tc, test_contract_identify_game_remote_saves_hash_mapping);
   tcase_add_test(tc, test_contract_award_achievement_success_updates_unlock_cache);
   tcase_add_test(tc, test_contract_award_achievement_failure_queues_pending);
   tcase_add_test(tc, test_contract_initialize_runtime_login_success_installs_definition);
   tcase_add_test(tc, test_contract_initialize_runtime_login_failed_with_network_does_not_install_definition);
   tcase_add_test(tc, test_contract_initialize_runtime_no_network_uses_cache_and_installs_definition);
   tcase_add_test(tc, test_contract_pending_awards_sync_after_ping_recovers);
   suite_add_tcase(s, tc);
   return s;
}

int main(void)
{
   int num_fail;
   Suite *s = create_suite();
   SRunner *sr = srunner_create(s);
   srunner_run_all(sr, CK_NORMAL);
   num_fail = srunner_ntests_failed(sr);
   srunner_free(sr);
   return (num_fail == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
