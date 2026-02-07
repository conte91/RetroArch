/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2019-2024 - Brian Weiss
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "cheevos_cache.h"
#include "cheevos_locals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <formats/rjson.h>
#include <retro_miscellaneous.h>
#include <compat/strl.h>
#include <time/rtime.h>

#include "../configuration.h"
#include "../file_path_special.h"
#include "../verbosity.h"

/* Maximum pending unlock entries to prevent unbounded growth */
#define RCHEEVOS_CACHE_MAX_PENDING 1000

/* Cache subdirectory names */
#define RCHEEVOS_CACHE_DIR "cache"
#define RCHEEVOS_CACHE_GAMES_DIR "games"
#define RCHEEVOS_CACHE_HASHES_DIR "hashes"
#define RCHEEVOS_CACHE_USER_DIR "user"
#define RCHEEVOS_CACHE_HASH_MAP_FILE "hash_map.json"

/************************************************************************
 * JSON Parse Context Structures                                        *
 ************************************************************************/

typedef struct hash_map_parse_ctx
{
   const char *target_hash;
   uint32_t result_game_id;
   bool found;
   char *current_key;
} hash_map_parse_ctx_t;

typedef struct unlock_parse_ctx
{
   uint32_t *achievement_ids;
   time_t *unlock_times;
   size_t count;
   size_t capacity;
   char *current_key;
   uint32_t current_id;
   time_t current_time;
   bool in_unlock_array;
   bool in_unlock_object;
   unsigned object_depth;
} unlock_parse_ctx_t;

typedef struct pending_parse_ctx
{
   rcheevos_pending_unlock_t *pending;
   size_t count;
   size_t capacity;
   char *current_key;
   bool in_unlocks_array;
   bool in_leaderboards_array;
   bool in_item_object;
   rcheevos_pending_unlock_t current_item;
   unsigned object_depth;
} pending_parse_ctx_t;

/************************************************************************
 * Path Utilities                                                       *
 ************************************************************************/

#if 0
size_t rcheevos_cache_get_directory(char *buffer, size_t buffer_size)
{
   char cheevos_dir[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();
   const char *dir_thumbnails;

   if (!settings || !buffer || buffer_size == 0)
      return 0;

   dir_thumbnails = settings->paths.directory_thumbnails;
   if (string_is_empty(dir_thumbnails))
      return 0;

   /* Build path: <thumbnails>/cheevos/cache */
   fill_pathname_join_special(cheevos_dir, dir_thumbnails, "cheevos", sizeof(cheevos_dir));
   return fill_pathname_join_special(buffer, cheevos_dir, RCHEEVOS_CACHE_DIR, buffer_size);
}

static bool rcheevos_cache_ensure_directory(const char *path)
{
   if (path_is_directory(path))
      return true;

   CHEEVOS_LOG(RCHEEVOS_TAG "Creating cache directory: %s\n", path);
   return path_mkdir(path);
}

static size_t rcheevos_cache_get_games_dir(char *buffer, size_t buffer_size)
{
   char cache_dir[PATH_MAX_LENGTH];
   if (rcheevos_cache_get_directory(cache_dir, sizeof(cache_dir)) == 0)
      return 0;
   return fill_pathname_join_special(buffer, cache_dir, RCHEEVOS_CACHE_GAMES_DIR, buffer_size);
}

static size_t rcheevos_cache_get_hashes_dir(char *buffer, size_t buffer_size)
{
   char cache_dir[PATH_MAX_LENGTH];
   if (rcheevos_cache_get_directory(cache_dir, sizeof(cache_dir)) == 0)
      return 0;
   return fill_pathname_join_special(buffer, cache_dir, RCHEEVOS_CACHE_HASHES_DIR, buffer_size);
}

static size_t rcheevos_cache_get_user_dir(const char *username, char *buffer, size_t buffer_size)
{
   char cache_dir[PATH_MAX_LENGTH];
   char user_base[PATH_MAX_LENGTH];

   if (string_is_empty(username))
      return 0;

   if (rcheevos_cache_get_directory(cache_dir, sizeof(cache_dir)) == 0)
      return 0;

   fill_pathname_join_special(user_base, cache_dir, RCHEEVOS_CACHE_USER_DIR, sizeof(user_base));
   return fill_pathname_join_special(buffer, user_base, username, buffer_size);
}

#endif

bool rcheevos_cache_save_hash_mapping(const char *restrict hash, rcheevos_cache_hash_t *data)
{
   char path[PATH_MAX_LENGTH];
   char hashes_dir[PATH_MAX_LENGTH];
   char cheevos_dir[PATH_MAX_LENGTH];
   char cache_dir[PATH_MAX_LENGTH];
   char filename[PATH_MAX_LENGTH];
   char *json = NULL;
   int ret;
   bool result = false;
   settings_t *settings = config_get_ptr();
   const char *dir_thumbnails;

   if (!hash || !data || data->game_id == 0)
      return false;

   if (!settings || !settings->bools.cheevos_cache_enabled)
      return false;

   /* Get thumbnails directory */
   dir_thumbnails = settings->paths.directory_thumbnails;
   if (string_is_empty(dir_thumbnails))
      return false;

   /* Build path: <thumbnails>/cheevos/cache/hashes */
   if (fill_pathname_join_special(cheevos_dir, dir_thumbnails, "cheevos", sizeof(cheevos_dir)) == 0)
      return false;

   if (fill_pathname_join_special(cache_dir, cheevos_dir, RCHEEVOS_CACHE_DIR, sizeof(cache_dir)) == 0)
      return false;

   if (fill_pathname_join_special(hashes_dir, cache_dir, RCHEEVOS_CACHE_HASHES_DIR, sizeof(hashes_dir)) == 0)
      return false;

   /* Ensure hashes directory exists */
   if (!path_is_directory(hashes_dir))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Creating hashes directory: %s\n", hashes_dir);
      if (!path_mkdir(hashes_dir))
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Failed to create hashes directory\n");
         return false;
      }
   }

   /* Serialize the hash data to JSON */
   if (!rcheevos_cache_hash_serialize(data, &json))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to serialize hash data\n");
      return false;
   }

   /* Build full file path: <hashes>/<hash>.json */
   ret = snprintf(filename, sizeof(filename), "%s.json", hash);
   if (ret < 0 || ret >= (int) sizeof(filename))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to format filename for hash: %s\n", hash);
      goto cleanup;
   }

   if (fill_pathname_join_special(path, hashes_dir, filename, sizeof(path)) == 0)
      goto cleanup;

   /* Write the JSON file */
   result = filestream_write_file(path, json, strlen(json));

   if (result)
      CHEEVOS_LOG(RCHEEVOS_TAG "Cached hash mapping: %s -> %u\n", hash, data->game_id);
   else
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to write hash file: %s\n", path);

cleanup:
   if (json)
      free(json);

   return result;
}

bool rcheevos_cache_get_game_id_for_hash(const char *restrict hash, rcheevos_cache_hash_t *out)
{
   char path[PATH_MAX_LENGTH];
   char hashes_dir[PATH_MAX_LENGTH];
   char cheevos_dir[PATH_MAX_LENGTH];
   char cache_dir[PATH_MAX_LENGTH];
   char filename[PATH_MAX_LENGTH];
   char *content = NULL;
   int ret;
   int64_t bytes_read;
   bool result = false;
   settings_t *settings = config_get_ptr();
   const char *dir_thumbnails;

   if (!hash || !out)
      return false;

   if (!settings || !settings->bools.cheevos_cache_enabled)
      return false;

   /* Get thumbnails directory */
   dir_thumbnails = settings->paths.directory_thumbnails;
   if (string_is_empty(dir_thumbnails))
      return false;

   /* Build path: <thumbnails>/cheevos/cache/hashes */
   if (fill_pathname_join_special(cheevos_dir, dir_thumbnails, "cheevos", sizeof(cheevos_dir)) == 0)
      return false;

   if (fill_pathname_join_special(cache_dir, cheevos_dir, RCHEEVOS_CACHE_DIR, sizeof(cache_dir)) == 0)
      return false;

   if (fill_pathname_join_special(hashes_dir, cache_dir, RCHEEVOS_CACHE_HASHES_DIR, sizeof(hashes_dir)) == 0)
      return false;

   /* Build full file path: <hashes>/<hash>.json */
   ret = snprintf(filename, sizeof(filename), "%s.json", hash);
   if (ret < 0 || ret >= (int) sizeof(filename))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to format filename for hash: %s\n", hash);
      return false;
   }

   if (fill_pathname_join_special(path, hashes_dir, filename, sizeof(path)) == 0)
      return false;

   /* Check if file exists */
   if (!filestream_exists(path))
      return false;

   /* Read the JSON file */
   bytes_read = filestream_read_file(path, (void **) &content, NULL);
   if (bytes_read <= 0 || !content)
      return false;

   /* Deserialize the JSON */
   result = rcheevos_cache_hash_deserialize(content, out);

   if (result)
      CHEEVOS_LOG(RCHEEVOS_TAG "Cache hit: hash %s -> game %u\n", hash, out->game_id);
   else
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to deserialize hash data for: %s\n", hash);

   /* Free allocated content */
   if (content)
      free(content);

   return result;
}

/************************************************************************
 * Game Data Caching                                                    *
 ************************************************************************/

bool rcheevos_cache_get_game_data(uint32_t game_id, rcheevos_cache_game_t *out)
{
   char path[PATH_MAX_LENGTH];
   char games_dir[PATH_MAX_LENGTH];
   char cheevos_dir[PATH_MAX_LENGTH];
   char cache_dir[PATH_MAX_LENGTH];
   char filename[64];
   char *content = NULL;
   int ret;
   int64_t bytes_read;
   bool result = false;
   settings_t *settings = config_get_ptr();
   const char *dir_thumbnails;

   if (!out || game_id == 0)
      return false;

   if (!settings || !settings->bools.cheevos_cache_enabled)
      return false;

   /* Get thumbnails directory */
   dir_thumbnails = settings->paths.directory_thumbnails;
   if (string_is_empty(dir_thumbnails))
      return false;

   /* Build path: <thumbnails>/cheevos/cache/games */
   if (fill_pathname_join_special(cheevos_dir, dir_thumbnails, "cheevos", sizeof(cheevos_dir)) == 0)
      return false;

   if (fill_pathname_join_special(cache_dir, cheevos_dir, RCHEEVOS_CACHE_DIR, sizeof(cache_dir)) == 0)
      return false;

   if (fill_pathname_join_special(games_dir, cache_dir, RCHEEVOS_CACHE_GAMES_DIR, sizeof(games_dir)) == 0)
      return false;

   /* Build full file path: <games>/<game_id>.json */
   ret = snprintf(filename, sizeof(filename), "%u.json", game_id);
   if (ret < 0 || ret >= (int) sizeof(filename))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to format filename for game %u\n", game_id);
      return false;
   }

   if (fill_pathname_join_special(path, games_dir, filename, sizeof(path)) == 0)
      return false;

   /* Check if file exists */
   if (!filestream_exists(path))
      return false;

   /* Read the JSON file */
   bytes_read = filestream_read_file(path, (void **) &content, NULL);
   if (bytes_read <= 0 || !content)
      goto cleanup;

   /* Deserialize the JSON */
   result = rcheevos_cache_game_deserialize(content, out);

   if (result)
      CHEEVOS_LOG(RCHEEVOS_TAG "Loaded game data from cache for game %u\n", game_id);
   else
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to deserialize game data for game %u\n", game_id);

cleanup:
   if (content)
      free(content);

   return result;
}

bool rcheevos_cache_save_game_data(const rcheevos_cache_game_t *data)
{
   char path[PATH_MAX_LENGTH];
   char games_dir[PATH_MAX_LENGTH];
   char cheevos_dir[PATH_MAX_LENGTH];
   char cache_dir[PATH_MAX_LENGTH];
   char filename[64];
   char *json = NULL;
   int ret;
   bool result = false;
   settings_t *settings = config_get_ptr();
   const char *dir_thumbnails;

   if (!data || data->id == 0)
      return false;

   if (!settings || !settings->bools.cheevos_cache_enabled)
      return false;

   /* Get thumbnails directory */
   dir_thumbnails = settings->paths.directory_thumbnails;
   if (string_is_empty(dir_thumbnails))
      return false;

   /* Build path: <thumbnails>/cheevos/cache/games */
   if (fill_pathname_join_special(cheevos_dir, dir_thumbnails, "cheevos", sizeof(cheevos_dir)) == 0)
      return false;

   if (fill_pathname_join_special(cache_dir, cheevos_dir, RCHEEVOS_CACHE_DIR, sizeof(cache_dir)) == 0)
      return false;

   if (fill_pathname_join_special(games_dir, cache_dir, RCHEEVOS_CACHE_GAMES_DIR, sizeof(games_dir)) == 0)
      return false;

   /* Ensure games directory exists */
   if (!path_is_directory(games_dir))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Creating games directory: %s\n", games_dir);
      if (!path_mkdir(games_dir))
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Failed to create games directory\n");
         return false;
      }
   }

   /* Serialize the game data to JSON */
   if (!rcheevos_cache_game_serialize(data, &json))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to serialize game data\n");
      return false;
   }

   /* Build full file path: <games>/<game_id>.json */
   ret = snprintf(filename, sizeof(filename), "%u.json", data->id);
   if (ret < 0 || ret >= (int) sizeof(filename))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to format filename for game %u\n", data->id);
      goto cleanup;
   }

   if (fill_pathname_join_special(path, games_dir, filename, sizeof(path)) == 0)
      goto cleanup;

   /* Write the JSON file */
   result = filestream_write_file(path, json, strlen(json));

   if (result)
      CHEEVOS_LOG(RCHEEVOS_TAG "Saved game data to cache for game %u\n", data->id);
   else
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to write game data file: %s\n", path);

cleanup:
   if (json)
      free(json);

   return result;
}


#if 0
static size_t rcheevos_cache_get_game_data_path(uint32_t game_id, char* buffer, size_t buffer_size)
{
   char games_dir[PATH_MAX_LENGTH];
   char filename[64];

   if (rcheevos_cache_get_games_dir(games_dir, sizeof(games_dir)) == 0)
      return 0;

   snprintf(filename, sizeof(filename), "%u.json", game_id);
   return fill_pathname_join_special(buffer, games_dir, filename, buffer_size);
}

static size_t rcheevos_cache_get_hash_map_path(char* buffer, size_t buffer_size)
{
   char hashes_dir[PATH_MAX_LENGTH];
   if (rcheevos_cache_get_hashes_dir(hashes_dir, sizeof(hashes_dir)) == 0)
      return 0;
   return fill_pathname_join_special(buffer, hashes_dir, RCHEEVOS_CACHE_HASH_MAP_FILE, buffer_size);
}

static size_t rcheevos_cache_get_unlocks_path(
   const char* username, uint32_t game_id, bool hardcore, char* buffer, size_t buffer_size)
{
   char user_dir[PATH_MAX_LENGTH];
   char filename[64];

   if (rcheevos_cache_get_user_dir(username, user_dir, sizeof(user_dir)) == 0)
      return 0;

   snprintf(filename, sizeof(filename), "unlocks_%u%s.json",
      game_id, hardcore ? "_hardcore" : "");
   return fill_pathname_join_special(buffer, user_dir, filename, buffer_size);
}

static size_t rcheevos_cache_get_pending_path(const char* username, char* buffer, size_t buffer_size)
{
   char user_dir[PATH_MAX_LENGTH];
   if (rcheevos_cache_get_user_dir(username, user_dir, sizeof(user_dir)) == 0)
      return 0;
   return fill_pathname_join_special(buffer, user_dir, "pending.json", buffer_size);
}

/************************************************************************
 * File I/O Utilities                                                   *
 ************************************************************************/

static bool rcheevos_cache_read_file(const char* path, char** content_out, size_t* size_out)
{
   int64_t file_size;
   char* content;
   RFILE* file;

   if (!path || !content_out)
      return false;

   *content_out = NULL;
   if (size_out)
      *size_out = 0;

   if (!path_is_valid(path))
      return false;

   file = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!file)
      return false;

   file_size = filestream_get_size(file);
   if (file_size <= 0)
   {
      filestream_close(file);
      return false;
   }

   content = (char*)malloc((size_t)file_size + 1);
   if (!content)
   {
      filestream_close(file);
      return false;
   }

   if (filestream_read(file, content, (size_t)file_size) != file_size)
   {
      free(content);
      filestream_close(file);
      return false;
   }

   content[file_size] = '\0';
   filestream_close(file);

   *content_out = content;
   if (size_out)
      *size_out = (size_t)file_size;

   return true;
}

static bool rcheevos_cache_write_file(const char* path, const char* content, size_t size)
{
   char temp_path[PATH_MAX_LENGTH];
   RFILE* file;
   bool success;

   if (!path || !content)
      return false;

   /* Write to temp file first for atomic operation */
   snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

   file = filestream_open(temp_path, RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!file)
      return false;

   success = (filestream_write(file, content, size) == (int64_t)size);
   filestream_close(file);

   if (!success)
   {
      filestream_delete(temp_path);
      return false;
   }

   /* Rename temp file to target */
   if (path_is_valid(path))
      filestream_delete(path);

   if (filestream_rename(temp_path, path) != 0)
   {
      filestream_delete(temp_path);
      return false;
   }

   return true;
}

/************************************************************************
 * Cache Init/Deinit                                                    *
 ************************************************************************/

bool rcheevos_cache_init(void)
{
   char cache_dir[PATH_MAX_LENGTH];
   char subdir[PATH_MAX_LENGTH];
   settings_t* settings = config_get_ptr();

   if (!settings || !settings->bools.cheevos_cache_enabled)
      return false;

   if (rcheevos_cache_get_directory(cache_dir, sizeof(cache_dir)) == 0)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to get cache directory path\n");
      return false;
   }

   /* Create main cache directory and subdirectories */
   if (!rcheevos_cache_ensure_directory(cache_dir))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to create cache directory: %s\n", cache_dir);
      return false;
   }

   if (rcheevos_cache_get_games_dir(subdir, sizeof(subdir)) > 0)
      rcheevos_cache_ensure_directory(subdir);

   if (rcheevos_cache_get_hashes_dir(subdir, sizeof(subdir)) > 0)
      rcheevos_cache_ensure_directory(subdir);

   /* User directory is created on-demand when needed */

   CHEEVOS_LOG(RCHEEVOS_TAG "Cache initialized at %s\n", cache_dir);
   return true;
}

void rcheevos_cache_deinit(void)
{
   /* Nothing to clean up currently */
}

bool rcheevos_cache_clear(void)
{
   char cache_dir[PATH_MAX_LENGTH];
   char hash_map_path[PATH_MAX_LENGTH];

   if (rcheevos_cache_get_directory(cache_dir, sizeof(cache_dir)) == 0)
      return false;

   /* Delete the hash map file to invalidate all hash lookups */
   if (rcheevos_cache_get_hash_map_path(hash_map_path, sizeof(hash_map_path)) > 0)
   {
      if (path_is_valid(hash_map_path))
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Clearing cache hash map at %s\n", hash_map_path);
         filestream_delete(hash_map_path);
      }
   }

   /* Note: Individual game data and user files remain but will be
    * refreshed as stale data when max_age is exceeded.
    * For complete cleanup, users should delete the cache directory manually. */
   CHEEVOS_LOG(RCHEEVOS_TAG "Cache cleared (hash map removed)\n");

   return true;
}

/************************************************************************
 * Hash Map JSON Parsing                                                *
 ************************************************************************/

static bool hash_map_object_member(void* ctx, const char* str, size_t len)
{
   hash_map_parse_ctx_t* parse_ctx = (hash_map_parse_ctx_t*)ctx;
   if (parse_ctx->current_key)
      free(parse_ctx->current_key);
   parse_ctx->current_key = strdup(str);
   return true;
}

static bool hash_map_number(void* ctx, const char* str, size_t len)
{
   hash_map_parse_ctx_t* parse_ctx = (hash_map_parse_ctx_t*)ctx;

   if (parse_ctx->current_key && parse_ctx->target_hash)
   {
      if (string_is_equal_case_insensitive(parse_ctx->current_key, parse_ctx->target_hash))
      {
         parse_ctx->result_game_id = (uint32_t)strtoul(str, NULL, 10);
         parse_ctx->found = true;
      }
   }
   return true;
}

bool rcheevos_cache_is_game_data_stale(uint32_t game_id)
{
   char path[PATH_MAX_LENGTH];
   struct stat file_stat;
   time_t now;
   time_t max_age_seconds;
   settings_t* settings = config_get_ptr();

   if (game_id == 0)
      return true;

   if (!settings || !settings->bools.cheevos_cache_enabled)
      return true;

   if (rcheevos_cache_get_game_data_path(game_id, path, sizeof(path)) == 0)
      return true;

   if (!path_is_valid(path))
      return true;

   if (stat(path, &file_stat) != 0)
      return true;

   time(&now);
   max_age_seconds = (time_t)settings->uints.cheevos_cache_max_age_days * 24 * 60 * 60;

   return (now - file_stat.st_mtime) > max_age_seconds;
}

/************************************************************************
 * User Unlock State Parsing                                            *
 ************************************************************************/

static bool unlock_object_member(void* ctx, const char* str, size_t len)
{
   unlock_parse_ctx_t* parse_ctx = (unlock_parse_ctx_t*)ctx;
   if (parse_ctx->current_key)
      free(parse_ctx->current_key);
   parse_ctx->current_key = strdup(str);
   return true;
}

static bool unlock_number(void* ctx, const char* str, size_t len)
{
   unlock_parse_ctx_t* parse_ctx = (unlock_parse_ctx_t*)ctx;

   if (parse_ctx->in_unlock_object && parse_ctx->current_key)
   {
      if (string_is_equal(parse_ctx->current_key, "id"))
         parse_ctx->current_id = (uint32_t)strtoul(str, NULL, 10);
      else if (string_is_equal(parse_ctx->current_key, "time"))
         parse_ctx->current_time = (time_t)strtoll(str, NULL, 10);
   }
   return true;
}

static bool unlock_start_object(void* ctx)
{
   unlock_parse_ctx_t* parse_ctx = (unlock_parse_ctx_t*)ctx;
   parse_ctx->object_depth++;

   if (parse_ctx->in_unlock_array && parse_ctx->object_depth == 2)
   {
      parse_ctx->in_unlock_object = true;
      parse_ctx->current_id = 0;
      parse_ctx->current_time = 0;
   }
   return true;
}

static bool unlock_end_object(void* ctx)
{
   unlock_parse_ctx_t* parse_ctx = (unlock_parse_ctx_t*)ctx;

   if (parse_ctx->in_unlock_object && parse_ctx->object_depth == 2)
   {
      /* Store the parsed unlock */
      if (parse_ctx->current_id != 0)
      {
         if (parse_ctx->count >= parse_ctx->capacity)
         {
            size_t new_capacity = parse_ctx->capacity == 0 ? 64 : parse_ctx->capacity * 2;
            uint32_t* new_ids = (uint32_t*)realloc(parse_ctx->achievement_ids, new_capacity * sizeof(uint32_t));
            time_t* new_times = (time_t*)realloc(parse_ctx->unlock_times, new_capacity * sizeof(time_t));

            if (!new_ids || !new_times)
            {
               if (new_ids) parse_ctx->achievement_ids = new_ids;
               if (new_times) parse_ctx->unlock_times = new_times;
               return false;
            }

            parse_ctx->achievement_ids = new_ids;
            parse_ctx->unlock_times = new_times;
            parse_ctx->capacity = new_capacity;
         }

         parse_ctx->achievement_ids[parse_ctx->count] = parse_ctx->current_id;
         parse_ctx->unlock_times[parse_ctx->count] = parse_ctx->current_time;
         parse_ctx->count++;
      }
      parse_ctx->in_unlock_object = false;
   }

   parse_ctx->object_depth--;
   return true;
}

static bool unlock_start_array(void* ctx)
{
   unlock_parse_ctx_t* parse_ctx = (unlock_parse_ctx_t*)ctx;
   if (parse_ctx->current_key && string_is_equal(parse_ctx->current_key, "unlocks"))
      parse_ctx->in_unlock_array = true;
   return true;
}

static bool unlock_end_array(void* ctx)
{
   unlock_parse_ctx_t* parse_ctx = (unlock_parse_ctx_t*)ctx;
   parse_ctx->in_unlock_array = false;
   return true;
}

bool rcheevos_cache_get_user_unlocks(
   const char* username,
   uint32_t game_id,
   bool hardcore,
   rcheevos_cache_user_unlocks_t *out)
{
   char path[PATH_MAX_LENGTH];
   char user_game_dir[PATH_MAX_LENGTH];
   char user_dir[PATH_MAX_LENGTH];
   char user_base[PATH_MAX_LENGTH];
   char cheevos_dir[PATH_MAX_LENGTH];
   char cache_dir[PATH_MAX_LENGTH];
   char game_id_str[64];
   char *content            = NULL;
   int ret;
   int64_t bytes_read;
   bool result              = false;
   settings_t *settings     = config_get_ptr();
   const char *dir_thumbnails;
   const char *filename;

   if (!username || !out || game_id == 0)
      return false;

   if (!settings || !settings->bools.cheevos_cache_enabled)
      return false;

   /* Get thumbnails directory */
   dir_thumbnails = settings->paths.directory_thumbnails;
   if (string_is_empty(dir_thumbnails))
      return false;

   /* Build path: <thumbnails>/cheevos/cache/users/<username>/<game_id> */
   if (fill_pathname_join_special(cheevos_dir, dir_thumbnails, "cheevos", sizeof(cheevos_dir)) == 0)
      return false;

   if (fill_pathname_join_special(cache_dir, cheevos_dir, RCHEEVOS_CACHE_DIR, sizeof(cache_dir)) == 0)
      return false;

   if (fill_pathname_join_special(user_base, cache_dir, RCHEEVOS_CACHE_USER_DIR, sizeof(user_base)) == 0)
      return false;

   if (fill_pathname_join_special(user_dir, user_base, username, sizeof(user_dir)) == 0)
      return false;

   ret = snprintf(game_id_str, sizeof(game_id_str), "%u", game_id);
   if (ret < 0 || ret >= (int)sizeof(game_id_str))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to format game_id for user unlocks\n");
      return false;
   }

   if (fill_pathname_join_special(user_game_dir, user_dir, game_id_str, sizeof(user_game_dir)) == 0)
      return false;

   /* Build full file path: <user_game_dir>/unlocks_{hardcore|softcore}.json */
   filename = hardcore ? "unlocks_hardcore.json" : "unlocks_softcore.json";
   if (fill_pathname_join_special(path, user_game_dir, filename, sizeof(path)) == 0)
      return false;

   /* Check if file exists */
   if (!filestream_exists(path))
      return false;

   /* Read the JSON file */
   bytes_read = filestream_read_file(path, (void**)&content, NULL);
   if (bytes_read <= 0 || !content)
      goto cleanup;

   /* Deserialize the JSON */
   result = rcheevos_cache_unlocks_deserialize(content, out);

   if (result)
      CHEEVOS_LOG(RCHEEVOS_TAG "Loaded user unlocks for game %u (%s)\n",
                  game_id, hardcore ? "hardcore" : "softcore");
   else
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to deserialize user unlocks for game %u\n", game_id);

cleanup:
   if (content)
      free(content);

   return result;
}

bool rcheevos_cache_save_user_unlocks(
   const char* username,
   uint32_t game_id,
   bool hardcore,
   const rcheevos_cache_user_unlocks_t *data)
{
   char path[PATH_MAX_LENGTH];
   char user_game_dir[PATH_MAX_LENGTH];
   char user_dir[PATH_MAX_LENGTH];
   char user_base[PATH_MAX_LENGTH];
   char cheevos_dir[PATH_MAX_LENGTH];
   char cache_dir[PATH_MAX_LENGTH];
   char game_id_str[64];
   char *json               = NULL;
   int ret;
   bool result              = false;
   settings_t *settings     = config_get_ptr();
   const char *dir_thumbnails;
   const char *filename;

   if (!username || !data || game_id == 0)
      return false;

   if (!settings || !settings->bools.cheevos_cache_enabled)
      return false;

   /* Get thumbnails directory */
   dir_thumbnails = settings->paths.directory_thumbnails;
   if (string_is_empty(dir_thumbnails))
      return false;

   /* Build path: <thumbnails>/cheevos/cache/users/<username>/<game_id> */
   if (fill_pathname_join_special(cheevos_dir, dir_thumbnails, "cheevos", sizeof(cheevos_dir)) == 0)
      return false;

   if (fill_pathname_join_special(cache_dir, cheevos_dir, RCHEEVOS_CACHE_DIR, sizeof(cache_dir)) == 0)
      return false;

   if (fill_pathname_join_special(user_base, cache_dir, RCHEEVOS_CACHE_USER_DIR, sizeof(user_base)) == 0)
      return false;

   /* Ensure users base directory exists */
   if (!path_is_directory(user_base))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Creating users directory: %s\n", user_base);
      if (!path_mkdir(user_base))
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Failed to create users directory\n");
         return false;
      }
   }

   if (fill_pathname_join_special(user_dir, user_base, username, sizeof(user_dir)) == 0)
      return false;

   /* Ensure user directory exists */
   if (!path_is_directory(user_dir))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Creating user directory: %s\n", user_dir);
      if (!path_mkdir(user_dir))
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Failed to create user directory\n");
         return false;
      }
   }

   ret = snprintf(game_id_str, sizeof(game_id_str), "%u", game_id);
   if (ret < 0 || ret >= (int)sizeof(game_id_str))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to format game_id for user unlocks\n");
      return false;
   }

   if (fill_pathname_join_special(user_game_dir, user_dir, game_id_str, sizeof(user_game_dir)) == 0)
      return false;

   /* Ensure user game directory exists */
   if (!path_is_directory(user_game_dir))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Creating user game directory: %s\n", user_game_dir);
      if (!path_mkdir(user_game_dir))
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Failed to create user game directory\n");
         return false;
      }
   }

   /* Serialize the unlocks data to JSON */
   if (!rcheevos_cache_unlocks_serialize(data, &json))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to serialize user unlocks\n");
      return false;
   }

   /* Build full file path: <user_game_dir>/unlocks_{hardcore|softcore}.json */
   filename = hardcore ? "unlocks_hardcore.json" : "unlocks_softcore.json";
   if (fill_pathname_join_special(path, user_game_dir, filename, sizeof(path)) == 0)
      goto cleanup;

   /* Write the JSON file */
   result = filestream_write_file(path, json, strlen(json));

   if (result)
      CHEEVOS_LOG(RCHEEVOS_TAG "Saved user unlocks for game %u (%s)\n",
                  game_id, hardcore ? "hardcore" : "softcore");
   else
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to write user unlocks file: %s\n", path);

cleanup:
   if (json)
      free(json);

   return result;
}


/************************************************************************
 * Pending Queue Parsing                                                *
 ************************************************************************/

static bool pending_object_member(void* ctx, const char* str, size_t len)
{
   pending_parse_ctx_t* parse_ctx = (pending_parse_ctx_t*)ctx;
   if (parse_ctx->current_key)
      free(parse_ctx->current_key);
   parse_ctx->current_key = strdup(str);
   return true;
}

static bool pending_number(void* ctx, const char* str, size_t len)
{
   pending_parse_ctx_t* parse_ctx = (pending_parse_ctx_t*)ctx;

   if (parse_ctx->in_item_object && parse_ctx->current_key)
   {
      if (string_is_equal(parse_ctx->current_key, "id"))
         parse_ctx->current_item.id = (uint32_t)strtoul(str, NULL, 10);
      else if (string_is_equal(parse_ctx->current_key, "time"))
         parse_ctx->current_item.timestamp = (time_t)strtoll(str, NULL, 10);
      else if (string_is_equal(parse_ctx->current_key, "retries"))
         parse_ctx->current_item.retries = (uint32_t)strtoul(str, NULL, 10);
      else if (string_is_equal(parse_ctx->current_key, "score"))
         parse_ctx->current_item.score = (int32_t)strtol(str, NULL, 10);
   }
   return true;
}

static bool pending_boolean(void* ctx, bool value)
{
   pending_parse_ctx_t* parse_ctx = (pending_parse_ctx_t*)ctx;

   if (parse_ctx->in_item_object && parse_ctx->current_key)
   {
      if (string_is_equal(parse_ctx->current_key, "hardcore"))
         parse_ctx->current_item.hardcore = value;
   }
   return true;
}

static bool pending_start_object(void* ctx)
{
   pending_parse_ctx_t* parse_ctx = (pending_parse_ctx_t*)ctx;
   parse_ctx->object_depth++;

   if ((parse_ctx->in_unlocks_array || parse_ctx->in_leaderboards_array)
       && parse_ctx->object_depth == 2)
   {
      parse_ctx->in_item_object = true;
      memset(&parse_ctx->current_item, 0, sizeof(parse_ctx->current_item));
      parse_ctx->current_item.is_leaderboard = parse_ctx->in_leaderboards_array;
   }
   return true;
}

static bool pending_end_object(void* ctx)
{
   pending_parse_ctx_t* parse_ctx = (pending_parse_ctx_t*)ctx;

   if (parse_ctx->in_item_object && parse_ctx->object_depth == 2)
   {
      if (parse_ctx->current_item.id != 0)
      {
         if (parse_ctx->count >= parse_ctx->capacity)
         {
            size_t new_capacity = parse_ctx->capacity == 0 ? 32 : parse_ctx->capacity * 2;
            rcheevos_pending_unlock_t* new_pending = (rcheevos_pending_unlock_t*)realloc(
               parse_ctx->pending, new_capacity * sizeof(rcheevos_pending_unlock_t));

            if (!new_pending)
               return false;

            parse_ctx->pending = new_pending;
            parse_ctx->capacity = new_capacity;
         }

         parse_ctx->pending[parse_ctx->count++] = parse_ctx->current_item;
      }
      parse_ctx->in_item_object = false;
   }

   parse_ctx->object_depth--;
   return true;
}

static bool pending_start_array(void* ctx)
{
   pending_parse_ctx_t* parse_ctx = (pending_parse_ctx_t*)ctx;

   if (parse_ctx->current_key)
   {
      if (string_is_equal(parse_ctx->current_key, "unlocks"))
         parse_ctx->in_unlocks_array = true;
      else if (string_is_equal(parse_ctx->current_key, "leaderboards"))
         parse_ctx->in_leaderboards_array = true;
   }
   return true;
}

static bool pending_end_array(void* ctx)
{
   pending_parse_ctx_t* parse_ctx = (pending_parse_ctx_t*)ctx;
   parse_ctx->in_unlocks_array = false;
   parse_ctx->in_leaderboards_array = false;
   return true;
}

int rcheevos_cache_get_pending_unlocks(
   const char* username,
   rcheevos_pending_unlock_t** pending_out)
{
   char path[PATH_MAX_LENGTH];
   char* content = NULL;
   pending_parse_ctx_t ctx;
   int result;
   settings_t* settings = config_get_ptr();

   if (!username || !pending_out)
      return -1;

   if (!settings || !settings->bools.cheevos_cache_enabled)
      return -1;

   *pending_out = NULL;

   if (rcheevos_cache_get_pending_path(username, path, sizeof(path)) == 0)
      return -1;

   if (!rcheevos_cache_read_file(path, &content, NULL))
      return 0; /* No pending file is not an error */

   memset(&ctx, 0, sizeof(ctx));

   rjson_parse_quick(content, strlen(content), &ctx, 0,
      pending_object_member, NULL, pending_number,
      pending_start_object, pending_end_object,
      pending_start_array, pending_end_array,
      pending_boolean, NULL, NULL);

   if (ctx.current_key)
      free(ctx.current_key);

   free(content);

   if (ctx.count > 0)
   {
      *pending_out = ctx.pending;
      result = (int)ctx.count;
      CHEEVOS_LOG(RCHEEVOS_TAG "Loaded %d pending unlock(s) for user %s\n", result, username);
   }
   else
   {
      if (ctx.pending)
         free(ctx.pending);
      result = 0;
   }

   return result;
}

static bool rcheevos_cache_save_pending_internal(
   const char* username,
   const rcheevos_pending_unlock_t* pending,
   size_t count)
{
   char path[PATH_MAX_LENGTH];
   char user_dir[PATH_MAX_LENGTH];
   char cache_dir[PATH_MAX_LENGTH];
   char user_base[PATH_MAX_LENGTH];
   rjsonwriter_t* writer;
   char* json;
   int json_len;
   size_t i;
   bool first_unlock = true;
   bool first_lb = true;
   bool result = false;

   if (rcheevos_cache_get_directory(cache_dir, sizeof(cache_dir)) == 0)
      return false;

   fill_pathname_join_special(user_base, cache_dir, RCHEEVOS_CACHE_USER_DIR, sizeof(user_base));
   rcheevos_cache_ensure_directory(user_base);

   if (rcheevos_cache_get_user_dir(username, user_dir, sizeof(user_dir)) == 0)
      return false;

   rcheevos_cache_ensure_directory(user_dir);

   if (rcheevos_cache_get_pending_path(username, path, sizeof(path)) == 0)
      return false;

   /* If no pending items, delete the file */
   if (count == 0 || !pending)
   {
      if (path_is_valid(path))
         filestream_delete(path);
      return true;
   }

   writer = rjsonwriter_open_memory();
   if (!writer)
      return false;

   rjsonwriter_raw(writer, "{\n  \"unlocks\": [\n", 17);

   /* Write achievement unlocks */
   for (i = 0; i < count; i++)
   {
      if (!pending[i].is_leaderboard)
      {
         if (!first_unlock)
            rjsonwriter_raw(writer, ",\n", 2);
         first_unlock = false;

         rjsonwriter_rawf(writer,
            "    {\"id\": %u, \"hardcore\": %s, \"time\": %lld, \"retries\": %u}",
            pending[i].id,
            pending[i].hardcore ? "true" : "false",
            (long long)pending[i].timestamp,
            pending[i].retries);
      }
   }

   rjsonwriter_raw(writer, "\n  ],\n  \"leaderboards\": [\n", 25);

   /* Write leaderboard submissions */
   for (i = 0; i < count; i++)
   {
      if (pending[i].is_leaderboard)
      {
         if (!first_lb)
            rjsonwriter_raw(writer, ",\n", 2);
         first_lb = false;

         rjsonwriter_rawf(writer,
            "    {\"id\": %u, \"score\": %d, \"time\": %lld, \"retries\": %u}",
            pending[i].id,
            pending[i].score,
            (long long)pending[i].timestamp,
            pending[i].retries);
      }
   }

   rjsonwriter_raw(writer, "\n  ]\n}", 6);

   json = rjsonwriter_get_memory_buffer(writer, &json_len);
   if (json && json_len > 0)
      result = rcheevos_cache_write_file(path, json, (size_t)json_len);

   rjsonwriter_free(writer);
   return result;
}

bool rcheevos_cache_queue_achievement_unlock(
   const char* username,
   uint32_t achievement_id,
   bool hardcore,
   time_t timestamp)
{
   rcheevos_pending_unlock_t* pending = NULL;
   rcheevos_pending_unlock_t* new_pending;
   int count;
   bool result;
   size_t i;
   settings_t* settings = config_get_ptr();

   if (!username || achievement_id == 0)
      return false;

   if (!settings || !settings->bools.cheevos_cache_enabled)
      return false;

   count = rcheevos_cache_get_pending_unlocks(username, &pending);
   if (count < 0)
      count = 0;

   /* Check for duplicates */
   for (i = 0; i < (size_t)count; i++)
   {
      if (!pending[i].is_leaderboard && pending[i].id == achievement_id)
      {
         /* Already queued */
         if (pending)
            free(pending);
         return true;
      }
   }

   /* Check limit */
   if ((size_t)count >= RCHEEVOS_CACHE_MAX_PENDING)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Pending queue full, cannot add achievement %u\n", achievement_id);
      if (pending)
         free(pending);
      return false;
   }

   /* Add new entry */
   new_pending = (rcheevos_pending_unlock_t*)realloc(pending, ((size_t)count + 1) * sizeof(rcheevos_pending_unlock_t));
   if (!new_pending)
   {
      if (pending)
         free(pending);
      return false;
   }

   new_pending[count].id = achievement_id;
   new_pending[count].timestamp = timestamp;
   new_pending[count].retries = 0;
   new_pending[count].hardcore = hardcore;
   new_pending[count].is_leaderboard = false;
   new_pending[count].score = 0;

   result = rcheevos_cache_save_pending_internal(username, new_pending, (size_t)count + 1);

   if (result)
      CHEEVOS_LOG(RCHEEVOS_TAG "Queued achievement %u for later sync\n", achievement_id);

   free(new_pending);
   return result;
}

bool rcheevos_cache_queue_leaderboard_submit(
   const char* username,
   uint32_t leaderboard_id,
   int32_t score,
   time_t timestamp)
{
   rcheevos_pending_unlock_t* pending = NULL;
   rcheevos_pending_unlock_t* new_pending;
   int count;
   bool result;
   settings_t* settings = config_get_ptr();

   if (!username || leaderboard_id == 0)
      return false;

   if (!settings || !settings->bools.cheevos_cache_enabled)
      return false;

   count = rcheevos_cache_get_pending_unlocks(username, &pending);
   if (count < 0)
      count = 0;

   /* Check limit - don't check duplicates for leaderboards since scores can differ */
   if ((size_t)count >= RCHEEVOS_CACHE_MAX_PENDING)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Pending queue full, cannot add leaderboard %u\n", leaderboard_id);
      if (pending)
         free(pending);
      return false;
   }

   /* Add new entry */
   new_pending = (rcheevos_pending_unlock_t*)realloc(pending, ((size_t)count + 1) * sizeof(rcheevos_pending_unlock_t));
   if (!new_pending)
   {
      if (pending)
         free(pending);
      return false;
   }

   new_pending[count].id = leaderboard_id;
   new_pending[count].timestamp = timestamp;
   new_pending[count].retries = 0;
   new_pending[count].hardcore = false;
   new_pending[count].is_leaderboard = true;
   new_pending[count].score = score;

   result = rcheevos_cache_save_pending_internal(username, new_pending, (size_t)count + 1);

   if (result)
      CHEEVOS_LOG(RCHEEVOS_TAG "Queued leaderboard %u (score: %d) for later sync\n", leaderboard_id, score);

   free(new_pending);
   return result;
}

bool rcheevos_cache_remove_pending_unlock(
   const char* username,
   uint32_t id,
   bool is_leaderboard)
{
   rcheevos_pending_unlock_t* pending = NULL;
   int count;
   size_t i, j;
   bool found = false;
   bool result;
   settings_t* settings = config_get_ptr();

   if (!username || id == 0)
      return false;

   if (!settings || !settings->bools.cheevos_cache_enabled)
      return false;

   count = rcheevos_cache_get_pending_unlocks(username, &pending);
   if (count <= 0)
   {
      if (pending)
         free(pending);
      return true; /* Nothing to remove */
   }

   /* Find and remove the entry */
   for (i = 0; i < (size_t)count; i++)
   {
      if (pending[i].id == id && pending[i].is_leaderboard == is_leaderboard)
      {
         /* Shift remaining entries */
         for (j = i; j < (size_t)count - 1; j++)
            pending[j] = pending[j + 1];
         count--;
         found = true;
         break;
      }
   }

   if (!found)
   {
      free(pending);
      return true;
   }

   result = rcheevos_cache_save_pending_internal(username, pending, (size_t)count);

   if (result)
      CHEEVOS_LOG(RCHEEVOS_TAG "Removed pending %s %u from queue\n",
         is_leaderboard ? "leaderboard" : "achievement", id);

   free(pending);
   return result;
}

bool rcheevos_cache_update_pending_retry(
   const char* username,
   uint32_t id,
   bool is_leaderboard,
   uint32_t retries)
{
   rcheevos_pending_unlock_t* pending = NULL;
   int count;
   size_t i;
   bool result;
   settings_t* settings = config_get_ptr();

   if (!username || id == 0)
      return false;

   if (!settings || !settings->bools.cheevos_cache_enabled)
      return false;

   count = rcheevos_cache_get_pending_unlocks(username, &pending);
   if (count <= 0)
   {
      if (pending)
         free(pending);
      return false;
   }

   /* Find and update the entry */
   for (i = 0; i < (size_t)count; i++)
   {
      if (pending[i].id == id && pending[i].is_leaderboard == is_leaderboard)
      {
         pending[i].retries = retries;
         break;
      }
   }

   result = rcheevos_cache_save_pending_internal(username, pending, (size_t)count);
   free(pending);
   return result;
}

int rcheevos_cache_process_pending_queue(const char* username)
{
   /* This function is a stub - actual implementation requires
    * integration with the rc_client API which will be done in
    * cheevos_client.c by attempting to submit each pending item
    * and removing it from the queue on success */
   return 0;
}
#endif
