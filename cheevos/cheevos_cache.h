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

#ifndef __RARCH_CHEEVOS_CACHE_H
#define __RARCH_CHEEVOS_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include <boolean.h>
#include <retro_common_api.h>

#include "cheevos_cache_data.h"

RETRO_BEGIN_DECLS

/************************************************************************
 * Hash to Game ID Mapping                                              *
 ************************************************************************/

/* Get game ID for a content hash from cache
 * Returns true if found, false if not cached */
bool rcheevos_cache_get_game_id_for_hash(const char* restrict hash, rcheevos_cache_hash_t* restrict out);

/* Save hash to game ID mapping */
bool rcheevos_cache_save_hash_mapping(const char* hash, rcheevos_cache_hash_t* restrict out);

////////////////////////////////////////////////////////////////////////////////
///                              JUNK JUNK JUNK                              ///
///                              JUNK JUNK JUNK                              ///
///                              JUNK JUNK JUNK                              ///
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
///                              JUNK JUNK JUNK                              ///
///                              JUNK JUNK JUNK                              ///
///                              JUNK JUNK JUNK                              ///
////////////////////////////////////////////////////////////////////////////////

/************************************************************************
 * Cache Directory Structure                                            *
 ************************************************************************
 * <cache_dir>/cheevos/
 * ├── hashes/
 * │   └── <hash>.json              # One file per ROM hash -> game_id
 * ├── games/
 * │   └── <game_id>/
 * │       └── game.json            # Achievement + leaderboard definitions
 * └── users/
 *     └── <username>/
 *         ├── pending.json         # Queued unlocks for sync (all games)
 *         └── <game_id>/
 *             ├── unlocks_softcore.json
 *             └── unlocks_hardcore.json
 ************************************************************************/


/* Initialize the cache system - creates directories if needed */
bool rcheevos_cache_init(void);

/* Shutdown the cache system */
void rcheevos_cache_deinit(void);

/* Clear all cached data */
bool rcheevos_cache_clear(void);


/************************************************************************
 * Game Data (Achievement Definitions)                                  *
 ************************************************************************/

/* Get cached game data for a game
 * Returns true if found and valid, false otherwise */
bool rcheevos_cache_get_game_data(uint32_t game_id, rcheevos_cache_game_t *out);

/* Save game data to cache
 * Returns true on success */
bool rcheevos_cache_save_game_data(const rcheevos_cache_game_t *data);

/* Check if cached game data is stale (older than max age)
 * Returns true if stale or not present */
bool rcheevos_cache_is_game_data_stale(uint32_t game_id);

/************************************************************************
 * User Unlock State                                                    *
 ************************************************************************/

/* Get cached user unlock state for a game
 * Returns true if found and valid, false otherwise */
bool rcheevos_cache_get_user_unlocks(
   const char* username,
   uint32_t game_id,
   bool hardcore,
   rcheevos_cache_user_unlocks_t *out);

/* Save user unlock state for a game
 * Returns true on success */
bool rcheevos_cache_save_user_unlocks(
   const char* username,
   uint32_t game_id,
   bool hardcore,
   const rcheevos_cache_user_unlocks_t *data);

/************************************************************************
 * Pending Unlock Queue                                                 *
 ************************************************************************/

/* Queue an achievement unlock for later sync */
bool rcheevos_cache_queue_achievement_unlock(
   const char* username,
   uint32_t game_id,
   uint32_t achievement_id,
   bool hardcore,
   time_t timestamp);

/* Queue a leaderboard submission for later sync */
bool rcheevos_cache_queue_leaderboard_submit(
   const char* username,
   uint32_t game_id,
   uint32_t leaderboard_id,
   int32_t score,
   time_t timestamp);

/* Get pending unlocks for a user
 * Caller must free the returned array
 * Returns count of pending items, or -1 on error */
bool rcheevos_cache_get_pending_unlocks(
   const char* username,
   rcheevos_cache_pending_list_t* out);

bool rcheevos_cache_save_pending_unlocks(
   const char* username,
   const rcheevos_cache_pending_list_t* data);

/* Remove a pending unlock after successful sync */
bool rcheevos_cache_remove_pending_unlock(
   const char* username,
   uint32_t id,
   bool is_leaderboard);

/* Update retry count for a pending unlock */
bool rcheevos_cache_update_pending_retry(
   const char* username,
   uint32_t id,
   bool is_leaderboard,
   uint32_t retries);

/************************************************************************
 * Cache Path Utilities                                                 *
 ************************************************************************/

/* Get the base cache directory path
 * Returns length written, or 0 on error */
size_t rcheevos_cache_get_directory(char* buffer, size_t buffer_size);

RETRO_END_DECLS

#endif /* __RARCH_CHEEVOS_CACHE_H */
