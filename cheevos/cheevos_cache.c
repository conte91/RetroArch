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
 * Path Utilities                                                       *
 ************************************************************************/

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

static bool rcheevos_cache_ensure_directory(const char *path, const char *base)
{
   if (path_is_directory(path))
      return true;
   if (!path_is_directory(base))
   {
      CHEEVOS_ERR(RCHEEVOS_TAG "Refusing to create cache directory `%s`. Base directory `%s` does not exist or is not a directory.\n", path, base);
      return false;
   }

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

   if (fill_pathname_join_special(user_base, cache_dir, RCHEEVOS_CACHE_USER_DIR, sizeof(user_base)) == 0)
      return 0;
   return fill_pathname_join_special(buffer, user_base, username, buffer_size);
}

static size_t rcheevos_cache_get_user_game_dir(const char *username, uint32_t game_id,
                                               char *buffer, size_t buffer_size)
{
   char user_dir[PATH_MAX_LENGTH];
   char game_id_str[64];
   int ret;

   if (rcheevos_cache_get_user_dir(username, user_dir, sizeof(user_dir)) == 0)
      return 0;

   ret = snprintf(game_id_str, sizeof(game_id_str), "%u", game_id);
   if (ret < 0 || ret >= (int) sizeof(game_id_str))
      return 0;

   return fill_pathname_join_special(buffer, user_dir, game_id_str, buffer_size);
}

static size_t rcheevos_cache_get_unlocks_path(const char *username, uint32_t game_id,
                                              bool hardcore, char *buffer, size_t buffer_size)
{
   char user_game_dir[PATH_MAX_LENGTH];
   const char *filename;

   if (rcheevos_cache_get_user_game_dir(username, game_id, user_game_dir, sizeof(user_game_dir)) == 0)
      return 0;

   filename = hardcore ? "unlocks_hardcore.json" : "unlocks_softcore.json";
   return fill_pathname_join_special(buffer, user_game_dir, filename, buffer_size);
}

static size_t rcheevos_cache_get_pending_path(const char *username, uint32_t game_id,
                                              char *buffer, size_t buffer_size)
{
   char user_game_dir[PATH_MAX_LENGTH];
   if (rcheevos_cache_get_user_game_dir(username, game_id, user_game_dir, sizeof(user_game_dir)) == 0)
      return 0;
   return fill_pathname_join_special(buffer, user_game_dir, "pending.json", buffer_size);
}

bool rcheevos_cache_save_hash_mapping(const char *restrict hash, rcheevos_cache_hash_t *data)
{
   char path[PATH_MAX_LENGTH];
   char hashes_dir[PATH_MAX_LENGTH];
   char filename[PATH_MAX_LENGTH];
   char *json = NULL;
   int ret;
   bool result = false;
   settings_t *settings = config_get_ptr();
   const char *dir_thumbnails;

   if (!hash || !data || data->game_id == 0)
      return false;

   /* Get thumbnails directory as base */
   dir_thumbnails = settings->paths.directory_thumbnails;
   if (string_is_empty(dir_thumbnails))
      return false;

   /* Get hashes directory */
   if (rcheevos_cache_get_hashes_dir(hashes_dir, sizeof(hashes_dir)) == 0)
      return false;

   /* Ensure hashes directory exists */
   if (!rcheevos_cache_ensure_directory(hashes_dir, dir_thumbnails))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to create hashes directory: %s\n", hashes_dir);
      return false;
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
   char filename[PATH_MAX_LENGTH];
   char *content = NULL;
   int ret;
   int64_t bytes_read;
   bool result = false;
   settings_t *settings = config_get_ptr();

   if (!hash || !out)
      return false;

   if (!settings || !settings->bools.cheevos_cache_enabled)
      return false;

   /* Get hashes directory */
   if (rcheevos_cache_get_hashes_dir(hashes_dir, sizeof(hashes_dir)) == 0)
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
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to deserialize hash data from: %s\n", path);

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
   char filename[64];
   char *content = NULL;
   int ret;
   int64_t bytes_read;
   bool result = false;

   if (game_id == 0)
      return false;

   /* Get games directory */
   if (rcheevos_cache_get_games_dir(games_dir, sizeof(games_dir)) == 0)
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
   if (!filestream_read_file(path, (void **) &content, &bytes_read))
      goto cleanup;
   if (!content)
   {
      CHEEVOS_ERR(RCHEEVOS_TAG "Successfully read `filestream_read_file`, but no content buffer was allocated.\n");
      goto cleanup;
   }

   /* Deserialize the JSON */
   result = rcheevos_cache_game_deserialize(content, out);

   if (result)
      CHEEVOS_LOG(RCHEEVOS_TAG "Loaded game data from cache for game %u\n", game_id);
   else
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to deserialize game data from: %s\n", path);

cleanup:
   if (content)
      free(content);

   return result;
}

bool rcheevos_cache_save_game_data(const rcheevos_cache_game_t *data)
{
   char path[PATH_MAX_LENGTH];
   char games_dir[PATH_MAX_LENGTH];
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

   /* Get thumbnails directory as base */
   dir_thumbnails = settings->paths.directory_thumbnails;
   if (string_is_empty(dir_thumbnails))
      return false;

   /* Get games directory */
   if (rcheevos_cache_get_games_dir(games_dir, sizeof(games_dir)) == 0)
      return false;

   /* Ensure games directory exists */
   if (!rcheevos_cache_ensure_directory(games_dir, dir_thumbnails))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to create games directory: %s\n", games_dir);
      return false;
   }

   /* Serialize the game data to JSON */
   if (!rcheevos_cache_game_serialize(data, &json))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to serialize game data for game %u\n", data->id);
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

/************************************************************************
 * User Unlock State Caching                                           *
 ************************************************************************/

bool rcheevos_cache_get_user_unlocks(
   const char *username,
   uint32_t game_id,
   bool hardcore,
   rcheevos_cache_user_unlocks_t *out)
{
   char path[PATH_MAX_LENGTH];
   char *content = NULL;
   int64_t bytes_read;
   bool result = false;
   settings_t *settings = config_get_ptr();

   if (!username || !out || game_id == 0)
      return false;

   if (!settings || !settings->bools.cheevos_cache_enabled)
      return false;

   /* Get full file path */
   if (rcheevos_cache_get_unlocks_path(username, game_id, hardcore, path, sizeof(path)) == 0)
      return false;

   /* Check if file exists */
   if (!filestream_exists(path))
      return false;

   /* Read the JSON file */
   bytes_read = filestream_read_file(path, (void **) &content, NULL);
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
   const char *username,
   uint32_t game_id,
   bool hardcore,
   const rcheevos_cache_user_unlocks_t *data)
{
   char path[PATH_MAX_LENGTH];
   char user_game_dir[PATH_MAX_LENGTH];
   char user_dir[PATH_MAX_LENGTH];
   char cache_dir[PATH_MAX_LENGTH];
   char user_base[PATH_MAX_LENGTH];
   char *json = NULL;
   bool result = false;
   settings_t *settings = config_get_ptr();
   const char *dir_thumbnails;

   if (!username || !data || game_id == 0)
      return false;

   if (!settings || !settings->bools.cheevos_cache_enabled)
      return false;

   /* Get thumbnails directory as base */
   dir_thumbnails = settings->paths.directory_thumbnails;
   if (string_is_empty(dir_thumbnails))
      return false;

   /* Get user directory */
   if (rcheevos_cache_get_user_dir(username, user_dir, sizeof(user_dir)) == 0)
      return false;

   /* Get user game directory */
   if (rcheevos_cache_get_user_game_dir(username, game_id, user_game_dir, sizeof(user_game_dir)) == 0)
      return false;

   /* Get cache directory for base path check */
   if (rcheevos_cache_get_directory(cache_dir, sizeof(cache_dir)) == 0)
      return false;

   if (fill_pathname_join_special(user_base, cache_dir, RCHEEVOS_CACHE_USER_DIR, sizeof(user_base)) == 0)
      return false;

   /* Ensure user base directory exists */
   if (!rcheevos_cache_ensure_directory(user_base, dir_thumbnails))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to create users base directory: %s\n", user_base);
      return false;
   }

   /* Ensure user directory exists */
   if (!rcheevos_cache_ensure_directory(user_dir, dir_thumbnails))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to create user directory: %s\n", user_dir);
      return false;
   }

   /* Ensure user game directory exists */
   if (!rcheevos_cache_ensure_directory(user_game_dir, dir_thumbnails))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to create user game directory: %s\n", user_game_dir);
      return false;
   }

   /* Serialize the unlocks data to JSON */
   if (!rcheevos_cache_unlocks_serialize(data, &json))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to serialize user unlocks\n");
      return false;
   }

   /* Get full file path */
   if (rcheevos_cache_get_unlocks_path(username, game_id, hardcore, path, sizeof(path)) == 0)
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

bool rcheevos_cache_get_pending_unlocks(
   const char *username,
   uint32_t game_id,
   rcheevos_cache_pending_list_t *out)
{
   char path[PATH_MAX_LENGTH];
   char *content = NULL;
   int64_t bytes_read;
   bool result = false;

   /* Get full file path */
   if (rcheevos_cache_get_pending_path(username, game_id, path, sizeof(path)) == 0)
      return false;

   /* Check if file exists */
   if (!filestream_exists(path))
      return false;

   /* Read the JSON file */
   bytes_read = filestream_read_file(path, (void **) &content, NULL);
   if (bytes_read <= 0 || !content)
      goto cleanup;

   /* Deserialize the JSON */
   result = rcheevos_cache_pending_deserialize(content, out);

   if (result)
      CHEEVOS_LOG(RCHEEVOS_TAG "Loaded %d pending unlocks for game %u (%s)\n", out->num_entries, game_id, username);
   else
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to deserialize pending unlocks for game %u\n", game_id);

cleanup:
   if (content)
      free(content);

   return result;
}

bool rcheevos_cache_save_pending_unlocks(
   const char *username,
   uint32_t game_id,
   const rcheevos_cache_pending_list_t *data)
{
   char path[PATH_MAX_LENGTH];
   char user_dir[PATH_MAX_LENGTH];
   char user_game_dir[PATH_MAX_LENGTH];
   char cache_dir[PATH_MAX_LENGTH];
   char user_base[PATH_MAX_LENGTH];
   char *json = NULL;
   bool result = false;
   settings_t *settings = config_get_ptr();
   const char *dir_thumbnails;

   /* Get thumbnails directory as base */
   dir_thumbnails = settings->paths.directory_thumbnails;
   if (string_is_empty(dir_thumbnails))
      return false;

   if (rcheevos_cache_get_directory(cache_dir, sizeof(cache_dir)) == 0)
      return false;

   if (fill_pathname_join_special(user_base, cache_dir, RCHEEVOS_CACHE_USER_DIR, sizeof(user_base)) == 0)
      return false;

   if (rcheevos_cache_get_user_dir(username, user_dir, sizeof(user_dir)) == 0)
      return false;

   if (rcheevos_cache_get_user_game_dir(username, game_id, user_game_dir, sizeof(user_game_dir)) == 0)
      return false;

   if (rcheevos_cache_get_pending_path(username, game_id, path, sizeof(path)) == 0)
      return false;

   /* Ensure directory chain exists */
   if (!rcheevos_cache_ensure_directory(user_base, dir_thumbnails) ||
       !rcheevos_cache_ensure_directory(user_dir, dir_thumbnails) ||
       !rcheevos_cache_ensure_directory(user_game_dir, dir_thumbnails))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to create directory for pending.json (game %u)\n", game_id);
      return false;
   }

   /* Serialize the pending list to JSON */
   if (!rcheevos_cache_pending_serialize(data, &json))
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to serialize pending unlocks for game %u\n", game_id);
      return false;
   }

   /* Write the JSON file */
   result = filestream_write_file(path, json, strlen(json));

   if (result)
      CHEEVOS_LOG(RCHEEVOS_TAG "Saved %d pending unlocks for game %u (%s)\n", data->num_entries, game_id, username);
   else
      CHEEVOS_LOG(RCHEEVOS_TAG "Failed to write pending unlocks file: %s\n", path);

   if (json)
      free(json);

   return result;
}

bool rcheevos_cache_remove_pending_unlock(
   const char *username,
   uint32_t game_id,
   uint32_t id,
   bool is_leaderboard)
{
   rcheevos_cache_pending_list_t pending;
   uint32_t i;
   bool found = false;

   if (!rcheevos_cache_get_pending_unlocks(username, game_id, &pending))
      return false;

   for (i = 0; i < pending.num_entries; i++)
   {
      if (pending.entries[i].id == id &&
          pending.entries[i].is_leaderboard == is_leaderboard)
      {
         /* Overwrite with last entry, then shrink */
         pending.entries[i] = pending.entries[pending.num_entries - 1];
         pending.num_entries--;
         found = true;
         break;
      }
   }

   if (found)
      rcheevos_cache_save_pending_unlocks(username, game_id, &pending);

   rcheevos_cache_pending_list_free(&pending);
   return found;
}
