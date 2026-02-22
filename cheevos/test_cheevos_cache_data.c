/* Copyright  (C) 2024 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (test_cheevos_cache_data.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <check.h>
#include <stdlib.h>
#include <string.h>

#include "cheevos_cache_data.h"

#define SUITE_NAME "cheevos_cache_data"

/************************************************************************
 * Pending Unlock Queue                                                  *
 *                                                                       *
 * Trajectories modelled: achievement unlocked while offline, leaderboard*
 * submitted while offline, multiple events queued, nothing queued yet.  *
 ************************************************************************/

/* Smallest trajectory: one achievement unlocked while offline.
 * All fields must survive the serialize → deserialize round-trip. */
START_TEST(test_pending_one_achievement_roundtrip)
{
   rcheevos_cache_pending_t entry;
   rcheevos_cache_pending_list_t src;
   rcheevos_cache_pending_list_t dst;
   char *json = NULL;

   memset(&entry, 0, sizeof(entry));
   entry.id            = 99001;
   entry.timestamp     = 1700000001;
   entry.retries       = 3;
   entry.hardcore      = true;
   entry.is_leaderboard = false;

   src.entries     = &entry;
   src.num_entries = 1;

   ck_assert(rcheevos_cache_pending_serialize(&src, &json));
   ck_assert_ptr_nonnull(json);

   memset(&dst, 0, sizeof(dst));
   ck_assert(rcheevos_cache_pending_deserialize(json, &dst));

   ck_assert_uint_eq(dst.num_entries, 1);
   ck_assert_uint_eq(dst.entries[0].id, 99001);
   ck_assert_int_eq((long long)dst.entries[0].timestamp, 1700000001LL);
   ck_assert_uint_eq(dst.entries[0].retries, 3);
   ck_assert_int_eq(dst.entries[0].hardcore, true);
   ck_assert_int_eq(dst.entries[0].is_leaderboard, false);

   free(json);
   rcheevos_cache_pending_free(&dst);
}
END_TEST

/* Same trajectory but with hardcore = false, to cover both boolean paths. */
START_TEST(test_pending_achievement_softcore_roundtrip)
{
   rcheevos_cache_pending_t entry;
   rcheevos_cache_pending_list_t src;
   rcheevos_cache_pending_list_t dst;
   char *json = NULL;

   memset(&entry, 0, sizeof(entry));
   entry.id            = 99002;
   entry.timestamp     = 1700000002;
   entry.hardcore      = false;
   entry.is_leaderboard = false;

   src.entries     = &entry;
   src.num_entries = 1;

   ck_assert(rcheevos_cache_pending_serialize(&src, &json));
   ck_assert_ptr_nonnull(json);

   memset(&dst, 0, sizeof(dst));
   ck_assert(rcheevos_cache_pending_deserialize(json, &dst));

   ck_assert_uint_eq(dst.num_entries, 1);
   ck_assert_int_eq(dst.entries[0].hardcore, false);

   free(json);
   rcheevos_cache_pending_free(&dst);
}
END_TEST

/* Leaderboard submitted offline: score must survive the round-trip. */
START_TEST(test_pending_leaderboard_with_score_roundtrip)
{
   rcheevos_cache_pending_t entry;
   rcheevos_cache_pending_list_t src;
   rcheevos_cache_pending_list_t dst;
   char *json = NULL;

   memset(&entry, 0, sizeof(entry));
   entry.id            = 55001;
   entry.timestamp     = 1700000003;
   entry.hardcore      = true;
   entry.is_leaderboard = true;
   entry.score         = -42000;   /* negative scores are valid */

   src.entries     = &entry;
   src.num_entries = 1;

   ck_assert(rcheevos_cache_pending_serialize(&src, &json));
   ck_assert_ptr_nonnull(json);

   memset(&dst, 0, sizeof(dst));
   ck_assert(rcheevos_cache_pending_deserialize(json, &dst));

   ck_assert_uint_eq(dst.num_entries, 1);
   ck_assert_uint_eq(dst.entries[0].id, 55001);
   ck_assert_int_eq(dst.entries[0].is_leaderboard, true);
   ck_assert_int_eq(dst.entries[0].score, -42000);

   free(json);
   rcheevos_cache_pending_free(&dst);
}
END_TEST

/* Two events queued offline: both must be present after round-trip,
 * regardless of their order in the output. */
START_TEST(test_pending_multiple_entries_roundtrip)
{
   rcheevos_cache_pending_t entries[2];
   rcheevos_cache_pending_list_t src;
   rcheevos_cache_pending_list_t dst;
   char *json = NULL;
   uint32_t ids[2];
   uint32_t i;

   memset(entries, 0, sizeof(entries));
   entries[0].id = 11111; entries[0].timestamp = 1700000010; entries[0].hardcore = true;
   entries[1].id = 22222; entries[1].timestamp = 1700000020; entries[1].hardcore = false;

   src.entries     = entries;
   src.num_entries = 2;

   ck_assert(rcheevos_cache_pending_serialize(&src, &json));
   ck_assert_ptr_nonnull(json);

   memset(&dst, 0, sizeof(dst));
   ck_assert(rcheevos_cache_pending_deserialize(json, &dst));
   ck_assert_uint_eq(dst.num_entries, 2);

   /* Collect IDs without assuming order */
   for (i = 0; i < dst.num_entries; i++)
      ids[i] = dst.entries[i].id;
   ck_assert((ids[0] == 11111 && ids[1] == 22222) ||
             (ids[0] == 22222 && ids[1] == 11111));

   free(json);
   rcheevos_cache_pending_free(&dst);
}
END_TEST

/* Nothing queued yet: empty list must serialize and deserialize cleanly. */
START_TEST(test_pending_empty_list_roundtrip)
{
   rcheevos_cache_pending_list_t src;
   rcheevos_cache_pending_list_t dst;
   char *json = NULL;

   src.entries     = NULL;
   src.num_entries = 0;

   ck_assert(rcheevos_cache_pending_serialize(&src, &json));
   ck_assert_ptr_nonnull(json);

   memset(&dst, 0, sizeof(dst));
   ck_assert(rcheevos_cache_pending_deserialize(json, &dst));
   ck_assert_uint_eq(dst.num_entries, 0);

   free(json);
   rcheevos_cache_pending_free(&dst);
}
END_TEST

/* NULL inputs must return false and not crash. */
START_TEST(test_pending_null_inputs_are_safe)
{
   rcheevos_cache_pending_list_t list;
   char *json = NULL;

   memset(&list, 0, sizeof(list));

   ck_assert(!rcheevos_cache_pending_serialize(NULL, &json));
   ck_assert(!rcheevos_cache_pending_serialize(&list, NULL));
   ck_assert(!rcheevos_cache_pending_deserialize(NULL, &list));
   ck_assert(!rcheevos_cache_pending_deserialize("[]", NULL));

   rcheevos_cache_pending_free(NULL); /* must not crash */
}
END_TEST

/* Malformed JSON must not crash and must not produce entries. */
START_TEST(test_pending_malformed_json_does_not_crash)
{
   rcheevos_cache_pending_list_t dst;

   memset(&dst, 0, sizeof(dst));
   /* Return value is unspecified for garbage input, but it must not crash */
   rcheevos_cache_pending_deserialize("{not valid json!!!", &dst);
   rcheevos_cache_pending_free(&dst);

   memset(&dst, 0, sizeof(dst));
   rcheevos_cache_pending_deserialize("", &dst);
   rcheevos_cache_pending_free(&dst);
}
END_TEST

/************************************************************************
 * Hash Mapping                                                          *
 *                                                                       *
 * Trajectory: a ROM is identified by its content hash; the mapping from *
 * hash → game_id must survive the round-trip.                           *
 ************************************************************************/

START_TEST(test_hash_mapping_roundtrip)
{
   rcheevos_cache_hash_t src;
   rcheevos_cache_hash_t dst;
   char *json = NULL;

   src.game_id = 12345;

   ck_assert(rcheevos_cache_hash_serialize(&src, &json));
   ck_assert_ptr_nonnull(json);

   memset(&dst, 0, sizeof(dst));
   ck_assert(rcheevos_cache_hash_deserialize(json, &dst));
   ck_assert_uint_eq(dst.game_id, 12345);

   free(json);
}
END_TEST

START_TEST(test_hash_null_inputs_are_safe)
{
   rcheevos_cache_hash_t data;
   char *json = NULL;

   data.game_id = 1;

   ck_assert(!rcheevos_cache_hash_serialize(NULL, &json));
   ck_assert(!rcheevos_cache_hash_serialize(&data, NULL));
   ck_assert(!rcheevos_cache_hash_deserialize(NULL, &data));
   ck_assert(!rcheevos_cache_hash_deserialize("{}", NULL));
}
END_TEST

/************************************************************************
 * Game Data                                                             *
 *                                                                       *
 * Trajectory: game data (including achievements and leaderboards) is    *
 * loaded from cache for an offline session. All metadata must survive   *
 * the round-trip.                                                       *
 ************************************************************************/

START_TEST(test_game_data_with_content_roundtrip)
{
   rcheevos_cache_achievement_t ach;
   rcheevos_cache_leaderboard_t lb;
   rcheevos_cache_game_t src;
   rcheevos_cache_game_t dst;
   char *json = NULL;

   memset(&ach, 0, sizeof(ach));
   ach.id          = 1001;
   ach.points      = 10;
   ach.category    = 3;
   ach.title       = "First Blood";
   ach.description = "Kill your first enemy";
   ach.definition  = "0xH0001=1";
   ach.author      = "devuser";
   ach.badge_name  = "badge_01";
   ach.created     = 1600000000;
   ach.updated     = 1600000001;

   memset(&lb, 0, sizeof(lb));
   lb.id              = 2001;
   lb.format          = 1;
   lb.title           = "Fastest Run";
   lb.description     = "Complete the level as fast as possible";
   lb.definition      = "STA:0xH0010=1::SUB:0xH0010=0::VAL:0xV0020";
   lb.lower_is_better = 1;
   lb.hidden          = 0;

   memset(&src, 0, sizeof(src));
   src.id                  = 777;
   src.console_id          = 7;
   src.title               = "Test Game";
   src.image_name          = "game_badge.png";
   src.rich_presence_script = "Display:\nPlaying";
   src.cached_at           = 1700000000;
   src.achievements        = &ach;
   src.num_achievements    = 1;
   src.leaderboards        = &lb;
   src.num_leaderboards    = 1;

   ck_assert(rcheevos_cache_game_serialize(&src, &json));
   ck_assert_ptr_nonnull(json);

   memset(&dst, 0, sizeof(dst));
   ck_assert(rcheevos_cache_game_deserialize(json, &dst));

   ck_assert_uint_eq(dst.id, 777);
   ck_assert_uint_eq(dst.console_id, 7);
   ck_assert_str_eq(dst.title, "Test Game");
   ck_assert_str_eq(dst.image_name, "game_badge.png");
   ck_assert_str_eq(dst.rich_presence_script, "Display:\nPlaying");
   ck_assert_int_eq((long long)dst.cached_at, 1700000000LL);

   ck_assert_uint_eq(dst.num_achievements, 1);
   ck_assert_uint_eq(dst.achievements[0].id, 1001);
   ck_assert_uint_eq(dst.achievements[0].points, 10);
   ck_assert_uint_eq(dst.achievements[0].category, 3);
   ck_assert_str_eq(dst.achievements[0].title, "First Blood");
   ck_assert_str_eq(dst.achievements[0].description, "Kill your first enemy");
   ck_assert_str_eq(dst.achievements[0].definition, "0xH0001=1");
   ck_assert_str_eq(dst.achievements[0].author, "devuser");
   ck_assert_str_eq(dst.achievements[0].badge_name, "badge_01");
   ck_assert_int_eq((long long)dst.achievements[0].created, 1600000000LL);
   ck_assert_int_eq((long long)dst.achievements[0].updated, 1600000001LL);

   ck_assert_uint_eq(dst.num_leaderboards, 1);
   ck_assert_uint_eq(dst.leaderboards[0].id, 2001);
   ck_assert_int_eq(dst.leaderboards[0].format, 1);
   ck_assert_str_eq(dst.leaderboards[0].title, "Fastest Run");
   ck_assert_str_eq(dst.leaderboards[0].description, "Complete the level as fast as possible");
   ck_assert_str_eq(dst.leaderboards[0].definition, "STA:0xH0010=1::SUB:0xH0010=0::VAL:0xV0020");
   ck_assert_int_eq(dst.leaderboards[0].lower_is_better, 1);
   ck_assert_int_eq(dst.leaderboards[0].hidden, 0);

   free(json);
   rcheevos_cache_game_free(&dst);
}
END_TEST

/* Game with no achievements or leaderboards must still round-trip. */
START_TEST(test_game_data_empty_arrays_roundtrip)
{
   rcheevos_cache_game_t src;
   rcheevos_cache_game_t dst;
   char *json = NULL;

   memset(&src, 0, sizeof(src));
   src.id         = 888;
   src.console_id = 1;
   src.title      = "Empty Game";

   ck_assert(rcheevos_cache_game_serialize(&src, &json));
   ck_assert_ptr_nonnull(json);

   memset(&dst, 0, sizeof(dst));
   ck_assert(rcheevos_cache_game_deserialize(json, &dst));

   ck_assert_uint_eq(dst.id, 888);
   ck_assert_uint_eq(dst.num_achievements, 0);
   ck_assert_uint_eq(dst.num_leaderboards, 0);

   free(json);
   rcheevos_cache_game_free(&dst);
}
END_TEST

START_TEST(test_game_null_inputs_are_safe)
{
   rcheevos_cache_game_t data;
   char *json = NULL;

   memset(&data, 0, sizeof(data));
   data.id = 1;

   ck_assert(!rcheevos_cache_game_serialize(NULL, &json));
   ck_assert(!rcheevos_cache_game_serialize(&data, NULL));
   ck_assert(!rcheevos_cache_game_deserialize(NULL, &data));
   ck_assert(!rcheevos_cache_game_deserialize("{}", NULL));

   rcheevos_cache_game_free(NULL); /* must not crash */
}
END_TEST

/************************************************************************
 * User Unlocks                                                          *
 *                                                                       *
 * Trajectory: server-confirmed unlocks for a user are cached; the       *
 * achievement IDs must survive the round-trip.                          *
 ************************************************************************/

START_TEST(test_user_unlocks_roundtrip)
{
   rcheevos_cache_unlock_t unlocks[3];
   rcheevos_cache_user_unlocks_t src;
   rcheevos_cache_user_unlocks_t dst;
   char *json = NULL;
   uint32_t i, found;

   memset(unlocks, 0, sizeof(unlocks));
   unlocks[0].achievement_id = 1001;
   unlocks[1].achievement_id = 1002;
   unlocks[2].achievement_id = 1003;

   memset(&src, 0, sizeof(src));
   src.username     = "testuser";
   src.unlocks      = unlocks;
   src.num_unlocks  = 3;
   src.last_updated = 1700000000;

   ck_assert(rcheevos_cache_unlocks_serialize(&src, &json));
   ck_assert_ptr_nonnull(json);

   memset(&dst, 0, sizeof(dst));
   ck_assert(rcheevos_cache_unlocks_deserialize(json, &dst));

   ck_assert_uint_eq(dst.num_unlocks, 3);

   /* All three IDs must be present (order not guaranteed) */
   found = 0;
   for (i = 0; i < dst.num_unlocks; i++)
   {
      if (dst.unlocks[i].achievement_id == 1001) found |= 1;
      if (dst.unlocks[i].achievement_id == 1002) found |= 2;
      if (dst.unlocks[i].achievement_id == 1003) found |= 4;
   }
   ck_assert_uint_eq(found, 7);

   free(json);
   rcheevos_cache_unlocks_free(&dst);
}
END_TEST

START_TEST(test_user_unlocks_empty_roundtrip)
{
   rcheevos_cache_user_unlocks_t src;
   rcheevos_cache_user_unlocks_t dst;
   char *json = NULL;

   memset(&src, 0, sizeof(src));
   src.username    = "testuser";
   src.unlocks     = NULL;
   src.num_unlocks = 0;

   ck_assert(rcheevos_cache_unlocks_serialize(&src, &json));
   ck_assert_ptr_nonnull(json);

   memset(&dst, 0, sizeof(dst));
   ck_assert(rcheevos_cache_unlocks_deserialize(json, &dst));
   ck_assert_uint_eq(dst.num_unlocks, 0);

   free(json);
   rcheevos_cache_unlocks_free(&dst);
}
END_TEST

START_TEST(test_unlocks_null_inputs_are_safe)
{
   rcheevos_cache_user_unlocks_t data;
   char *json = NULL;

   memset(&data, 0, sizeof(data));

   ck_assert(!rcheevos_cache_unlocks_serialize(NULL, &json));
   ck_assert(!rcheevos_cache_unlocks_serialize(&data, NULL));
   ck_assert(!rcheevos_cache_unlocks_deserialize(NULL, &data));

   rcheevos_cache_unlocks_free(NULL); /* must not crash */
}
END_TEST

/************************************************************************
 * Suite assembly                                                        *
 ************************************************************************/

Suite *create_suite(void)
{
   Suite *s          = suite_create(SUITE_NAME);
   TCase *tc_pending = tcase_create("Pending");
   TCase *tc_hash    = tcase_create("Hash");
   TCase *tc_game    = tcase_create("Game");
   TCase *tc_unlocks = tcase_create("Unlocks");

   tcase_add_test(tc_pending, test_pending_one_achievement_roundtrip);
   tcase_add_test(tc_pending, test_pending_achievement_softcore_roundtrip);
   tcase_add_test(tc_pending, test_pending_leaderboard_with_score_roundtrip);
   tcase_add_test(tc_pending, test_pending_multiple_entries_roundtrip);
   tcase_add_test(tc_pending, test_pending_empty_list_roundtrip);
   tcase_add_test(tc_pending, test_pending_null_inputs_are_safe);
   tcase_add_test(tc_pending, test_pending_malformed_json_does_not_crash);
   suite_add_tcase(s, tc_pending);

   tcase_add_test(tc_hash, test_hash_mapping_roundtrip);
   tcase_add_test(tc_hash, test_hash_null_inputs_are_safe);
   suite_add_tcase(s, tc_hash);

   tcase_add_test(tc_game, test_game_data_with_content_roundtrip);
   tcase_add_test(tc_game, test_game_data_empty_arrays_roundtrip);
   tcase_add_test(tc_game, test_game_null_inputs_are_safe);
   suite_add_tcase(s, tc_game);

   tcase_add_test(tc_unlocks, test_user_unlocks_roundtrip);
   tcase_add_test(tc_unlocks, test_user_unlocks_empty_roundtrip);
   tcase_add_test(tc_unlocks, test_unlocks_null_inputs_are_safe);
   suite_add_tcase(s, tc_unlocks);

   return s;
}

int main(void)
{
   int num_fail;
   Suite *s   = create_suite();
   SRunner *sr = srunner_create(s);
   srunner_run_all(sr, CK_NORMAL);
   num_fail = srunner_ntests_failed(sr);
   srunner_free(sr);
   return (num_fail == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
