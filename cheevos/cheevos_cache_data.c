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

#include "cheevos_cache_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <formats/rjson.h>
#include <string/stdstring.h>

/************************************************************************
 * Helper Macros                                                        *
 ************************************************************************/

#define CHEEVOS_JSON_KEY(writer, key)   \
   rjsonwriter_add_string(writer, key); \
   rjsonwriter_raw(writer, ": ", 2)

#define CHEEVOS_JSON_KEY_STR(writer, key, value) \
   CHEEVOS_JSON_KEY(writer, key);                \
   if (value)                                    \
      rjsonwriter_add_string(writer, value);     \
   else                                          \
      rjsonwriter_raw(writer, "null", 4)

#define CHEEVOS_FREE(p) \
   do                   \
   {                    \
      if (p)            \
      {                 \
         free(p);       \
         p = NULL;      \
      }                 \
   } while (0)

/************************************************************************
 * JSON Parser Context                                                  *
 ************************************************************************/

typedef struct json_parse_ctx
{
   char *current_key;
   unsigned depth;
   unsigned array_depth;
   void *user_data;
} json_parse_ctx_t;

static char *json_strdup(const char *s)
{
   return s ? strdup(s) : NULL;
}

/************************************************************************
 * Hash Mapping Serialization                                           *
 ************************************************************************/

bool rcheevos_cache_hash_serialize(const rcheevos_cache_hash_t *data, char **json_out)
{
   rjsonwriter_t *writer;
   char *json;
   int len;

   if (!data || !json_out)
      return false;

   writer = rjsonwriter_open_memory();
   if (!writer)
      return false;

   rjsonwriter_raw(writer, "{\n  ", 4);
   CHEEVOS_JSON_KEY(writer, "game_id");
   rjsonwriter_rawf(writer, "%u", data->game_id);
   rjsonwriter_raw(writer, "\n}", 2);

   json = rjsonwriter_get_memory_buffer(writer, &len);
   if (json && len > 0)
   {
      *json_out = strdup(json);
      rjsonwriter_free(writer);
      return *json_out != NULL;
   }

   rjsonwriter_free(writer);
   return false;
}

bool rcheevos_cache_hash_deserialize(const char *json, rcheevos_cache_hash_t *data_out)
{
   rjson_t *parser;
   enum rjson_type type;

   if (!json || !data_out)
      return false;

   memset(data_out, 0, sizeof(*data_out));

   parser = rjson_open_string(json, strlen(json));
   if (!parser)
      return false;

   while ((type = rjson_next(parser)) != RJSON_DONE && type != RJSON_ERROR)
   {
      if (type == RJSON_STRING && rjson_get_context_type(parser) == RJSON_OBJECT)
      {
         const char *key = rjson_get_string(parser, NULL);
         type = rjson_next(parser);

         if (string_is_equal(key, "game_id") && type == RJSON_NUMBER)
            data_out->game_id = (uint32_t) rjson_get_int(parser);
      }
   }

   rjson_free(parser);
   return data_out->game_id != 0;
}

/************************************************************************
 * Game Data Serialization                                              *
 ************************************************************************/

static void serialize_achievement(rjsonwriter_t *writer, const rcheevos_cache_achievement_t *ach)
{
   rjsonwriter_raw(writer, "{\n", 2);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY(writer, "id");
   rjsonwriter_rawf(writer, "%u,\n", ach->id);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY(writer, "points");
   rjsonwriter_rawf(writer, "%u,\n", ach->points);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY(writer, "category");
   rjsonwriter_rawf(writer, "%u,\n", ach->category);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY_STR(writer, "title", ach->title);
   rjsonwriter_raw(writer, ",\n", 2);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY_STR(writer, "description", ach->description);
   rjsonwriter_raw(writer, ",\n", 2);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY_STR(writer, "definition", ach->definition);
   rjsonwriter_raw(writer, ",\n", 2);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY_STR(writer, "author", ach->author);
   rjsonwriter_raw(writer, ",\n", 2);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY_STR(writer, "badge_name", ach->badge_name);
   rjsonwriter_raw(writer, ",\n", 2);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY(writer, "created");
   rjsonwriter_rawf(writer, "%lld,\n", (long long) ach->created);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY(writer, "updated");
   rjsonwriter_rawf(writer, "%lld\n", (long long) ach->updated);

   rjsonwriter_raw(writer, "      }", 7);
}

static void serialize_leaderboard(rjsonwriter_t *writer, const rcheevos_cache_leaderboard_t *lb)
{
   rjsonwriter_raw(writer, "{\n", 2);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY(writer, "id");
   rjsonwriter_rawf(writer, "%u,\n", lb->id);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY(writer, "format");
   rjsonwriter_rawf(writer, "%d,\n", lb->format);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY_STR(writer, "title", lb->title);
   rjsonwriter_raw(writer, ",\n", 2);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY_STR(writer, "description", lb->description);
   rjsonwriter_raw(writer, ",\n", 2);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY_STR(writer, "definition", lb->definition);
   rjsonwriter_raw(writer, ",\n", 2);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY(writer, "lower_is_better");
   rjsonwriter_rawf(writer, "%d,\n", lb->lower_is_better);

   rjsonwriter_raw(writer, "        ", 8);
   CHEEVOS_JSON_KEY(writer, "hidden");
   rjsonwriter_rawf(writer, "%d\n", lb->hidden);

   rjsonwriter_raw(writer, "      }", 7);
}

bool rcheevos_cache_game_serialize(const rcheevos_cache_game_t *data, char **json_out)
{
   rjsonwriter_t *writer;
   char *json;
   int len;
   uint32_t i;

   if (!data || !json_out)
      return false;

   writer = rjsonwriter_open_memory();
   if (!writer)
      return false;

   rjsonwriter_raw(writer, "{\n", 2);

   /* Game metadata */
   rjsonwriter_raw(writer, "  ", 2);
   CHEEVOS_JSON_KEY(writer, "id");
   rjsonwriter_rawf(writer, "%u,\n", data->id);

   rjsonwriter_raw(writer, "  ", 2);
   CHEEVOS_JSON_KEY(writer, "console_id");
   rjsonwriter_rawf(writer, "%u,\n", data->console_id);

   rjsonwriter_raw(writer, "  ", 2);
   CHEEVOS_JSON_KEY_STR(writer, "title", data->title);
   rjsonwriter_raw(writer, ",\n", 2);

   rjsonwriter_raw(writer, "  ", 2);
   CHEEVOS_JSON_KEY_STR(writer, "image_name", data->image_name);
   rjsonwriter_raw(writer, ",\n", 2);

   rjsonwriter_raw(writer, "  ", 2);
   CHEEVOS_JSON_KEY_STR(writer, "rich_presence_script", data->rich_presence_script);
   rjsonwriter_raw(writer, ",\n", 2);

   rjsonwriter_raw(writer, "  ", 2);
   CHEEVOS_JSON_KEY(writer, "cached_at");
   rjsonwriter_rawf(writer, "%lld,\n", (long long) data->cached_at);

   /* Achievements array */
   rjsonwriter_raw(writer, "  ", 2);
   CHEEVOS_JSON_KEY(writer, "achievements");
   rjsonwriter_raw(writer, "[\n", 2);

   for (i = 0; i < data->num_achievements; i++)
   {
      rjsonwriter_raw(writer, "      ", 6);
      serialize_achievement(writer, &data->achievements[i]);
      if (i < data->num_achievements - 1)
         rjsonwriter_raw(writer, ",", 1);
      rjsonwriter_raw(writer, "\n", 1);
   }
   rjsonwriter_raw(writer, "  ],\n", 5);

   /* Leaderboards array */
   rjsonwriter_raw(writer, "  ", 2);
   CHEEVOS_JSON_KEY(writer, "leaderboards");
   rjsonwriter_raw(writer, "[\n", 2);

   for (i = 0; i < data->num_leaderboards; i++)
   {
      rjsonwriter_raw(writer, "      ", 6);
      serialize_leaderboard(writer, &data->leaderboards[i]);
      if (i < data->num_leaderboards - 1)
         rjsonwriter_raw(writer, ",", 1);
      rjsonwriter_raw(writer, "\n", 1);
   }
   rjsonwriter_raw(writer, "  ]\n}", 5);

   json = rjsonwriter_get_memory_buffer(writer, &len);
   if (json && len > 0)
   {
      *json_out = strdup(json);
      rjsonwriter_free(writer);
      return *json_out != NULL;
   }

   rjsonwriter_free(writer);
   return false;
}

/* Game data parse context */
typedef struct game_parse_ctx
{
   rcheevos_cache_game_t *game;
   rcheevos_cache_achievement_t current_ach;
   rcheevos_cache_leaderboard_t current_lb;
   char *current_key;
   unsigned depth;
   bool in_achievements;
   bool in_leaderboards;
   bool in_item;
} game_parse_ctx_t;

static bool game_parse_string(void *ctx, const char *str, size_t len)
{
   game_parse_ctx_t *pctx = (game_parse_ctx_t *) ctx;

   if (!pctx->current_key)
      return true;

   if (pctx->in_item && pctx->in_achievements)
   {
      if (string_is_equal(pctx->current_key, "title"))
         pctx->current_ach.title = json_strdup(str);
      else if (string_is_equal(pctx->current_key, "description"))
         pctx->current_ach.description = json_strdup(str);
      else if (string_is_equal(pctx->current_key, "definition"))
         pctx->current_ach.definition = json_strdup(str);
      else if (string_is_equal(pctx->current_key, "author"))
         pctx->current_ach.author = json_strdup(str);
      else if (string_is_equal(pctx->current_key, "badge_name"))
         pctx->current_ach.badge_name = json_strdup(str);
   }
   else if (pctx->in_item && pctx->in_leaderboards)
   {
      if (string_is_equal(pctx->current_key, "title"))
         pctx->current_lb.title = json_strdup(str);
      else if (string_is_equal(pctx->current_key, "description"))
         pctx->current_lb.description = json_strdup(str);
      else if (string_is_equal(pctx->current_key, "definition"))
         pctx->current_lb.definition = json_strdup(str);
   }
   else if (pctx->depth == 1)
   {
      if (string_is_equal(pctx->current_key, "title"))
         pctx->game->title = json_strdup(str);
      else if (string_is_equal(pctx->current_key, "image_name"))
         pctx->game->image_name = json_strdup(str);
      else if (string_is_equal(pctx->current_key, "rich_presence_script"))
         pctx->game->rich_presence_script = json_strdup(str);
   }

   return true;
}

static bool game_parse_number(void *ctx, const char *str, size_t len)
{
   game_parse_ctx_t *pctx = (game_parse_ctx_t *) ctx;

   if (!pctx->current_key)
      return true;

   if (pctx->in_item && pctx->in_achievements)
   {
      if (string_is_equal(pctx->current_key, "id"))
         pctx->current_ach.id = (uint32_t) strtoul(str, NULL, 10);
      else if (string_is_equal(pctx->current_key, "points"))
         pctx->current_ach.points = (uint32_t) strtoul(str, NULL, 10);
      else if (string_is_equal(pctx->current_key, "category"))
         pctx->current_ach.category = (uint32_t) strtoul(str, NULL, 10);
      else if (string_is_equal(pctx->current_key, "created"))
         pctx->current_ach.created = (time_t) strtoll(str, NULL, 10);
      else if (string_is_equal(pctx->current_key, "updated"))
         pctx->current_ach.updated = (time_t) strtoll(str, NULL, 10);
   }
   else if (pctx->in_item && pctx->in_leaderboards)
   {
      if (string_is_equal(pctx->current_key, "id"))
         pctx->current_lb.id = (uint32_t) strtoul(str, NULL, 10);
      else if (string_is_equal(pctx->current_key, "format"))
         pctx->current_lb.format = (int32_t) strtol(str, NULL, 10);
      else if (string_is_equal(pctx->current_key, "lower_is_better"))
         pctx->current_lb.lower_is_better = (int) strtol(str, NULL, 10);
      else if (string_is_equal(pctx->current_key, "hidden"))
         pctx->current_lb.hidden = (int) strtol(str, NULL, 10);
   }
   else if (pctx->depth == 1)
   {
      if (string_is_equal(pctx->current_key, "id"))
         pctx->game->id = (uint32_t) strtoul(str, NULL, 10);
      else if (string_is_equal(pctx->current_key, "console_id"))
         pctx->game->console_id = (uint32_t) strtoul(str, NULL, 10);
      else if (string_is_equal(pctx->current_key, "cached_at"))
         pctx->game->cached_at = (time_t) strtoll(str, NULL, 10);
   }

   return true;
}

static bool game_parse_object_member(void *ctx, const char *str, size_t len)
{
   game_parse_ctx_t *pctx = (game_parse_ctx_t *) ctx;
   CHEEVOS_FREE(pctx->current_key);
   pctx->current_key = json_strdup(str);
   return true;
}

static bool game_parse_start_object(void *ctx)
{
   game_parse_ctx_t *pctx = (game_parse_ctx_t *) ctx;
   pctx->depth++;

   if ((pctx->in_achievements || pctx->in_leaderboards) && pctx->depth == 3)
   {
      pctx->in_item = true;
      if (pctx->in_achievements)
         memset(&pctx->current_ach, 0, sizeof(pctx->current_ach));
      else
         memset(&pctx->current_lb, 0, sizeof(pctx->current_lb));
   }

   return true;
}

static bool game_parse_end_object(void *ctx)
{
   game_parse_ctx_t *pctx = (game_parse_ctx_t *) ctx;

   if (pctx->in_item && pctx->depth == 3)
   {
      if (pctx->in_achievements && pctx->current_ach.id != 0)
      {
         /* Add achievement to array */
         rcheevos_cache_achievement_t *new_arr = (rcheevos_cache_achievement_t *) realloc(
            pctx->game->achievements,
            (pctx->game->num_achievements + 1) * sizeof(rcheevos_cache_achievement_t));
         if (new_arr)
         {
            pctx->game->achievements = new_arr;
            pctx->game->achievements[pctx->game->num_achievements++] = pctx->current_ach;
            memset(&pctx->current_ach, 0, sizeof(pctx->current_ach));
         }
      }
      else if (pctx->in_leaderboards && pctx->current_lb.id != 0)
      {
         /* Add leaderboard to array */
         rcheevos_cache_leaderboard_t *new_arr = (rcheevos_cache_leaderboard_t *) realloc(
            pctx->game->leaderboards,
            (pctx->game->num_leaderboards + 1) * sizeof(rcheevos_cache_leaderboard_t));
         if (new_arr)
         {
            pctx->game->leaderboards = new_arr;
            pctx->game->leaderboards[pctx->game->num_leaderboards++] = pctx->current_lb;
            memset(&pctx->current_lb, 0, sizeof(pctx->current_lb));
         }
      }
      pctx->in_item = false;
   }

   pctx->depth--;
   return true;
}

static bool game_parse_start_array(void *ctx)
{
   game_parse_ctx_t *pctx = (game_parse_ctx_t *) ctx;
   pctx->depth++;

   if (pctx->current_key)
   {
      if (string_is_equal(pctx->current_key, "achievements"))
         pctx->in_achievements = true;
      else if (string_is_equal(pctx->current_key, "leaderboards"))
         pctx->in_leaderboards = true;
   }

   return true;
}

static bool game_parse_end_array(void *ctx)
{
   game_parse_ctx_t *pctx = (game_parse_ctx_t *) ctx;
   pctx->in_achievements = false;
   pctx->in_leaderboards = false;
   return true;
}

bool rcheevos_cache_game_deserialize(const char *json, rcheevos_cache_game_t *data_out)
{
   game_parse_ctx_t ctx;

   if (!json || !data_out)
      return false;

   memset(data_out, 0, sizeof(*data_out));
   memset(&ctx, 0, sizeof(ctx));
   ctx.game = data_out;

   rjson_parse_quick(json, strlen(json), &ctx, 0,
                     game_parse_object_member, game_parse_string, game_parse_number,
                     game_parse_start_object, game_parse_end_object,
                     game_parse_start_array, game_parse_end_array,
                     NULL, NULL, NULL);

   CHEEVOS_FREE(ctx.current_key);

   return data_out->id != 0;
}

void rcheevos_cache_game_free(rcheevos_cache_game_t *data)
{
   uint32_t i;

   if (!data)
      return;

   CHEEVOS_FREE(data->title);
   CHEEVOS_FREE(data->image_name);
   CHEEVOS_FREE(data->rich_presence_script);

   if (data->achievements)
   {
      for (i = 0; i < data->num_achievements; i++)
      {
         CHEEVOS_FREE(data->achievements[i].title);
         CHEEVOS_FREE(data->achievements[i].description);
         CHEEVOS_FREE(data->achievements[i].definition);
         CHEEVOS_FREE(data->achievements[i].author);
         CHEEVOS_FREE(data->achievements[i].badge_name);
      }
      free(data->achievements);
      data->achievements = NULL;
   }

   if (data->leaderboards)
   {
      for (i = 0; i < data->num_leaderboards; i++)
      {
         CHEEVOS_FREE(data->leaderboards[i].title);
         CHEEVOS_FREE(data->leaderboards[i].description);
         CHEEVOS_FREE(data->leaderboards[i].definition);
      }
      free(data->leaderboards);
      data->leaderboards = NULL;
   }

   data->num_achievements = 0;
   data->num_leaderboards = 0;
}

/************************************************************************
 * User Unlocks Serialization                                           *
 ************************************************************************/

bool rcheevos_cache_unlocks_serialize(const rcheevos_cache_user_unlocks_t *data, char **json_out)
{
   rjsonwriter_t *writer;
   char *json;
   int len;
   uint32_t i;

   if (!data || !json_out)
      return false;

   writer = rjsonwriter_open_memory();
   if (!writer)
      return false;

   rjsonwriter_raw(writer, "{\n", 2);

   rjsonwriter_raw(writer, "  ", 2);
   CHEEVOS_JSON_KEY_STR(writer, "username", data->username);
   rjsonwriter_raw(writer, ",\n", 2);

   rjsonwriter_raw(writer, "  ", 2);
   CHEEVOS_JSON_KEY(writer, "last_updated");
   rjsonwriter_rawf(writer, "%lld,\n", (long long) data->last_updated);

   rjsonwriter_raw(writer, "  ", 2);
   CHEEVOS_JSON_KEY(writer, "achievement_ids");
   rjsonwriter_raw(writer, "[", 1);

   for (i = 0; i < data->num_unlocks; i++)
   {
      if (i > 0)
         rjsonwriter_raw(writer, ", ", 2);
      rjsonwriter_rawf(writer, "%u", data->unlocks[i].achievement_id);
   }
   rjsonwriter_raw(writer, "]\n}", 3);

   json = rjsonwriter_get_memory_buffer(writer, &len);
   if (json && len > 0)
   {
      *json_out = strdup(json);
      rjsonwriter_free(writer);
      return *json_out != NULL;
   }

   rjsonwriter_free(writer);
   return false;
}

/* Unlocks parse context */
typedef struct unlocks_parse_ctx
{
   rcheevos_cache_user_unlocks_t *unlocks;
   char *current_key;
   bool in_array;
} unlocks_parse_ctx_t;

static bool unlocks_parse_string(void *ctx, const char *str, size_t len)
{
   unlocks_parse_ctx_t *pctx = (unlocks_parse_ctx_t *) ctx;

   if (pctx->current_key && string_is_equal(pctx->current_key, "username"))
      pctx->unlocks->username = json_strdup(str);

   return true;
}

static bool unlocks_parse_number(void *ctx, const char *str, size_t len)
{
   unlocks_parse_ctx_t *pctx = (unlocks_parse_ctx_t *) ctx;

   if (pctx->in_array)
   {
      /* Add unlock to array */
      rcheevos_cache_unlock_t *new_arr = (rcheevos_cache_unlock_t *) realloc(
         pctx->unlocks->unlocks,
         (pctx->unlocks->num_unlocks + 1) * sizeof(rcheevos_cache_unlock_t));
      if (new_arr)
      {
         pctx->unlocks->unlocks = new_arr;
         pctx->unlocks->unlocks[pctx->unlocks->num_unlocks].achievement_id =
            (uint32_t) strtoul(str, NULL, 10);
         pctx->unlocks->unlocks[pctx->unlocks->num_unlocks].unlock_time = 0;
         pctx->unlocks->num_unlocks++;
      }
   }
   else if (pctx->current_key && string_is_equal(pctx->current_key, "last_updated"))
   {
      pctx->unlocks->last_updated = (time_t) strtoll(str, NULL, 10);
   }

   return true;
}

static bool unlocks_parse_object_member(void *ctx, const char *str, size_t len)
{
   unlocks_parse_ctx_t *pctx = (unlocks_parse_ctx_t *) ctx;
   CHEEVOS_FREE(pctx->current_key);
   pctx->current_key = json_strdup(str);
   return true;
}

static bool unlocks_parse_start_array(void *ctx)
{
   unlocks_parse_ctx_t *pctx = (unlocks_parse_ctx_t *) ctx;
   if (pctx->current_key && string_is_equal(pctx->current_key, "achievement_ids"))
      pctx->in_array = true;
   return true;
}

static bool unlocks_parse_end_array(void *ctx)
{
   unlocks_parse_ctx_t *pctx = (unlocks_parse_ctx_t *) ctx;
   pctx->in_array = false;
   return true;
}

bool rcheevos_cache_unlocks_deserialize(const char *json, rcheevos_cache_user_unlocks_t *data_out)
{
   unlocks_parse_ctx_t ctx;

   if (!json || !data_out)
      return false;

   memset(data_out, 0, sizeof(*data_out));
   memset(&ctx, 0, sizeof(ctx));
   ctx.unlocks = data_out;

   rjson_parse_quick(json, strlen(json), &ctx, 0,
                     unlocks_parse_object_member, unlocks_parse_string, unlocks_parse_number,
                     NULL, NULL,
                     unlocks_parse_start_array, unlocks_parse_end_array,
                     NULL, NULL, NULL);

   CHEEVOS_FREE(ctx.current_key);

   return true;
}

void rcheevos_cache_unlocks_free(rcheevos_cache_user_unlocks_t *data)
{
   if (!data)
      return;

   CHEEVOS_FREE(data->username);
   CHEEVOS_FREE(data->unlocks);
   data->num_unlocks = 0;
}

/************************************************************************
 * Pending Queue Serialization                                          *
 ************************************************************************/

bool rcheevos_cache_pending_serialize(const rcheevos_cache_pending_list_t *data, char **json_out)
{
   rjsonwriter_t *writer;
   char *json;
   int len;
   uint32_t i;

   if (!data || !json_out)
      return false;

   writer = rjsonwriter_open_memory();
   if (!writer)
      return false;

   rjsonwriter_raw(writer, "[\n", 2);

   for (i = 0; i < data->num_entries; i++)
   {
      const rcheevos_cache_pending_t *entry = &data->entries[i];

      if (i > 0)
         rjsonwriter_raw(writer, ",\n", 2);

      rjsonwriter_raw(writer, "  {\n", 4);

      rjsonwriter_raw(writer, "    ", 4);
      CHEEVOS_JSON_KEY(writer, "game_id");
      rjsonwriter_rawf(writer, "%u,\n", entry->game_id);

      rjsonwriter_raw(writer, "    ", 4);
      CHEEVOS_JSON_KEY(writer, "id");
      rjsonwriter_rawf(writer, "%u,\n", entry->id);

      rjsonwriter_raw(writer, "    ", 4);
      CHEEVOS_JSON_KEY(writer, "timestamp");
      rjsonwriter_rawf(writer, "%lld,\n", (long long) entry->timestamp);

      rjsonwriter_raw(writer, "    ", 4);
      CHEEVOS_JSON_KEY(writer, "retries");
      rjsonwriter_rawf(writer, "%u,\n", entry->retries);

      rjsonwriter_raw(writer, "    ", 4);
      CHEEVOS_JSON_KEY(writer, "hardcore");
      rjsonwriter_raw(writer, entry->hardcore ? "true" : "false", entry->hardcore ? 4 : 5);
      rjsonwriter_raw(writer, ",\n", 2);

      rjsonwriter_raw(writer, "    ", 4);
      CHEEVOS_JSON_KEY(writer, "is_leaderboard");
      rjsonwriter_raw(writer, entry->is_leaderboard ? "true" : "false", entry->is_leaderboard ? 4 : 5);

      if (entry->is_leaderboard)
      {
         rjsonwriter_raw(writer, ",\n    ", 6);
         CHEEVOS_JSON_KEY(writer, "score");
         rjsonwriter_rawf(writer, "%d", entry->score);
      }

      rjsonwriter_raw(writer, "\n  }", 4);
   }

   rjsonwriter_raw(writer, "\n]", 2);

   json = rjsonwriter_get_memory_buffer(writer, &len);
   if (json && len > 0)
   {
      *json_out = strdup(json);
      rjsonwriter_free(writer);
      return *json_out != NULL;
   }

   rjsonwriter_free(writer);
   return false;
}

/* Pending parse context */
typedef struct pending_parse_ctx
{
   rcheevos_cache_pending_list_t *list;
   rcheevos_cache_pending_t current;
   char *current_key;
   unsigned depth;
   bool in_object;
} pending_parse_ctx_t;

static bool pending_parse_number(void *ctx, const char *str, size_t len)
{
   pending_parse_ctx_t *pctx = (pending_parse_ctx_t *) ctx;

   if (!pctx->in_object || !pctx->current_key)
      return true;

   if (string_is_equal(pctx->current_key, "game_id"))
      pctx->current.game_id = (uint32_t) strtoul(str, NULL, 10);
   else if (string_is_equal(pctx->current_key, "id"))
      pctx->current.id = (uint32_t) strtoul(str, NULL, 10);
   else if (string_is_equal(pctx->current_key, "timestamp"))
      pctx->current.timestamp = (time_t) strtoll(str, NULL, 10);
   else if (string_is_equal(pctx->current_key, "retries"))
      pctx->current.retries = (uint32_t) strtoul(str, NULL, 10);
   else if (string_is_equal(pctx->current_key, "score"))
      pctx->current.score = (int32_t) strtol(str, NULL, 10);

   return true;
}

static bool pending_parse_boolean(void *ctx, bool value)
{
   pending_parse_ctx_t *pctx = (pending_parse_ctx_t *) ctx;

   if (!pctx->in_object || !pctx->current_key)
      return true;

   if (string_is_equal(pctx->current_key, "hardcore"))
      pctx->current.hardcore = value;
   else if (string_is_equal(pctx->current_key, "is_leaderboard"))
      pctx->current.is_leaderboard = value;

   return true;
}

static bool pending_parse_object_member(void *ctx, const char *str, size_t len)
{
   pending_parse_ctx_t *pctx = (pending_parse_ctx_t *) ctx;
   CHEEVOS_FREE(pctx->current_key);
   pctx->current_key = json_strdup(str);
   return true;
}

static bool pending_parse_start_object(void *ctx)
{
   pending_parse_ctx_t *pctx = (pending_parse_ctx_t *) ctx;
   pctx->depth++;

   /* Top-level container is an array (no array callbacks), so entry
    * objects land at depth 1, not 2. */
   if (pctx->depth == 1)
   {
      pctx->in_object = true;
      memset(&pctx->current, 0, sizeof(pctx->current));
   }

   return true;
}

static bool pending_parse_end_object(void *ctx)
{
   pending_parse_ctx_t *pctx = (pending_parse_ctx_t *) ctx;

   if (pctx->in_object && pctx->depth == 1)
   {
      /* Add entry to list */
      rcheevos_cache_pending_t *new_arr = (rcheevos_cache_pending_t *) realloc(
         pctx->list->entries,
         (pctx->list->num_entries + 1) * sizeof(rcheevos_cache_pending_t));
      if (new_arr)
      {
         pctx->list->entries = new_arr;
         pctx->list->entries[pctx->list->num_entries++] = pctx->current;
      }
      pctx->in_object = false;
   }

   pctx->depth--;
   return true;
}

bool rcheevos_cache_pending_deserialize(const char *json, rcheevos_cache_pending_list_t *data_out)
{
   pending_parse_ctx_t ctx;

   if (!json || !data_out)
      return false;

   memset(data_out, 0, sizeof(*data_out));
   memset(&ctx, 0, sizeof(ctx));
   ctx.list = data_out;

   rjson_parse_quick(json, strlen(json), &ctx, 0,
                     pending_parse_object_member, NULL, pending_parse_number,
                     pending_parse_start_object, pending_parse_end_object,
                     NULL, NULL,
                     pending_parse_boolean, NULL, NULL);

   CHEEVOS_FREE(ctx.current_key);

   return true;
}

void rcheevos_cache_pending_free(rcheevos_cache_pending_list_t *data)
{
   if (!data)
      return;

   CHEEVOS_FREE(data->entries);
   data->num_entries = 0;
}
