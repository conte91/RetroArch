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

#ifndef __RARCH_CHEEVOS_CACHE_DATA_H
#define __RARCH_CHEEVOS_CACHE_DATA_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/************************************************************************
 * Achievement Definition                                               *
 ************************************************************************/

typedef struct rcheevos_cache_achievement_t
{
   uint32_t id;              /* Achievement ID */
   uint32_t points;          /* Points value */
   uint32_t category;        /* 3 = core (official), 5 = unofficial */
   char* title;              /* Achievement name */
   char* description;        /* Achievement description */
   char* definition;         /* Trigger logic string (memaddr) */
   char* author;             /* Author username */
   char* badge_name;         /* Badge image filename */
   time_t created;           /* Server creation timestamp */
   time_t updated;           /* Server last modified timestamp */
} rcheevos_cache_achievement_t;

/************************************************************************
 * Leaderboard Definition                                               *
 ************************************************************************/

typedef struct rcheevos_cache_leaderboard_t
{
   uint32_t id;              /* Leaderboard ID */
   int32_t format;           /* Score format type */
   char* title;              /* Leaderboard name */
   char* description;        /* Leaderboard description */
   char* definition;         /* Trigger/submit logic string */
   int lower_is_better;      /* Non-zero if lower scores are better */
   int hidden;               /* Non-zero if hidden from list */
} rcheevos_cache_leaderboard_t;

/************************************************************************
 * Game Data                                                            *
 ************************************************************************/

typedef struct rcheevos_cache_game_t
{
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
} rcheevos_cache_game_t;

/************************************************************************
 * User Unlock State                                                    *
 ************************************************************************/

typedef struct rcheevos_cache_unlock_t
{
   uint32_t achievement_id;  /* Achievement ID */
   time_t unlock_time;       /* When it was unlocked */
} rcheevos_cache_unlock_t;

typedef struct rcheevos_cache_user_unlocks_t
{
   char* username;                       /* Username */
   rcheevos_cache_unlock_t* unlocks;     /* Array of unlocks */
   uint32_t num_unlocks;                 /* Count */
   time_t last_updated;                  /* Last update timestamp */
} rcheevos_cache_user_unlocks_t;

/************************************************************************
 * Pending Sync Queue                                                   *
 ************************************************************************/

typedef struct rcheevos_cache_pending_t
{
   uint32_t game_id;         /* Game ID */
   uint32_t id;              /* Achievement or leaderboard ID */
   time_t timestamp;         /* When the unlock/submission occurred */
   uint32_t retries;         /* Number of retry attempts */
   bool hardcore;            /* Hardcore mode flag */
   bool is_leaderboard;      /* true = leaderboard, false = achievement */
   int32_t score;            /* Leaderboard score (if is_leaderboard) */
} rcheevos_cache_pending_t;

typedef struct rcheevos_cache_pending_list_t
{
   rcheevos_cache_pending_t* entries;
   uint32_t num_entries;
} rcheevos_cache_pending_list_t;

/************************************************************************
 * Hash Mapping                                                         *
 ************************************************************************/

typedef struct rcheevos_cache_hash_t
{
   uint32_t game_id;         /* Game ID for this hash */
} rcheevos_cache_hash_t;

/************************************************************************
 * Serialization Functions                                              *
 ************************************************************************/

/* Hash mapping */
bool rcheevos_cache_hash_serialize(const rcheevos_cache_hash_t* data, char** json_out);
bool rcheevos_cache_hash_deserialize(const char* json, rcheevos_cache_hash_t* data_out);

/* Game data */
bool rcheevos_cache_game_serialize(const rcheevos_cache_game_t* data, char** json_out);
bool rcheevos_cache_game_deserialize(const char* json, rcheevos_cache_game_t* data_out);
void rcheevos_cache_game_free(rcheevos_cache_game_t* data);

/* User unlocks */
bool rcheevos_cache_unlocks_serialize(const rcheevos_cache_user_unlocks_t* data, char** json_out);
bool rcheevos_cache_unlocks_deserialize(const char* json, rcheevos_cache_user_unlocks_t* data_out);
void rcheevos_cache_unlocks_free(rcheevos_cache_user_unlocks_t* data);

/* Pending queue */
bool rcheevos_cache_pending_serialize(const rcheevos_cache_pending_list_t* data, char** json_out);
bool rcheevos_cache_pending_deserialize(const char* json, rcheevos_cache_pending_list_t* data_out);
void rcheevos_cache_pending_free(rcheevos_cache_pending_list_t* data);

RETRO_END_DECLS

#endif /* __RARCH_CHEEVOS_CACHE_DATA_H */
