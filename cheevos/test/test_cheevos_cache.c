/* Copyright  (C) 2024 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (test_cheevos_cache.c).
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
#include <stdio.h>

/* Pull in the real settings_t so we can provide a stub config_get_ptr(). */
#include "../../configuration.h"
#include <compat/strl.h>

#include "../cheevos_cache.h"

#define SUITE_NAME "cheevos_cache"

/************************************************************************
 * Per-test fixture: isolated tmpdir + stub settings                    *
 *                                                                       *
 * Each test gets its own mkdtemp directory, making tests hermetic and   *
 * safe to run in parallel.                                              *
 ************************************************************************/

static settings_t s_settings;
static char       s_tmpdir[PATH_MAX_LENGTH];

/* Stub that cheevos_cache.c calls to find the thumbnail/cache directory. */
settings_t *config_get_ptr(void)
{
   return &s_settings;
}

static void fixture_setup(void)
{
   char tmpl[] = "/tmp/test_cheevos_XXXXXX";
   char *dir   = mkdtemp(tmpl);
   ck_assert_ptr_nonnull(dir);

   memset(&s_settings, 0, sizeof(s_settings));
   strlcpy(s_tmpdir, dir, sizeof(s_tmpdir));
   strlcpy(s_settings.paths.directory_thumbnails, dir,
         sizeof(s_settings.paths.directory_thumbnails));
   s_settings.bools.cheevos_cache_enabled = true;
}

static void fixture_teardown(void)
{
   char cmd[PATH_MAX_LENGTH + 16];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", s_tmpdir);
   system(cmd);
}

/************************************************************************
 * Pending unlock queue — file I/O layer                                 *
 *                                                                       *
 * Trajectory: offline unlock recorded → survives to next session        *
 * (save → load), then removed when sync succeeds (remove → gone).      *
 ************************************************************************/

/* Save one pending achievement; it must be readable back with all fields. */
START_TEST(test_pending_save_and_load_one_entry)
{
   rcheevos_cache_pending_t    entry;
   rcheevos_cache_pending_list_t src;
   rcheevos_cache_pending_list_t dst;

   memset(&entry, 0, sizeof(entry));
   entry.id            = 99001;
   entry.timestamp     = 1700000001;
   entry.hardcore      = true;
   entry.is_leaderboard = false;

   src.entries     = &entry;
   src.num_entries = 1;

   ck_assert(rcheevos_cache_save_pending_unlocks("player1", 777, &src));

   memset(&dst, 0, sizeof(dst));
   ck_assert(rcheevos_cache_get_pending_unlocks("player1", 777, &dst));
   ck_assert_uint_eq(dst.num_entries, 1);
   ck_assert_uint_eq(dst.entries[0].id, 99001);
   ck_assert_int_eq(dst.entries[0].hardcore, true);
   ck_assert_int_eq(dst.entries[0].is_leaderboard, false);

   rcheevos_cache_pending_list_free(&dst);
}
END_TEST

/* Save two entries; both must be present on next load. */
START_TEST(test_pending_save_and_load_two_entries)
{
   rcheevos_cache_pending_t    entries[2];
   rcheevos_cache_pending_list_t src;
   rcheevos_cache_pending_list_t dst;
   uint32_t ids = 0;
   uint32_t i;

   memset(entries, 0, sizeof(entries));
   entries[0].id = 11111; entries[0].hardcore = true;
   entries[1].id = 22222; entries[1].hardcore = false;

   src.entries     = entries;
   src.num_entries = 2;

   ck_assert(rcheevos_cache_save_pending_unlocks("player1", 777, &src));

   memset(&dst, 0, sizeof(dst));
   ck_assert(rcheevos_cache_get_pending_unlocks("player1", 777, &dst));
   ck_assert_uint_eq(dst.num_entries, 2);

   for (i = 0; i < dst.num_entries; i++)
   {
      if (dst.entries[i].id == 11111) ids |= 1;
      if (dst.entries[i].id == 22222) ids |= 2;
   }
   ck_assert_uint_eq(ids, 3);

   rcheevos_cache_pending_list_free(&dst);
}
END_TEST

/* After a successful sync, the entry is removed and the queue is empty. */
START_TEST(test_pending_remove_after_sync)
{
   rcheevos_cache_pending_t    entry;
   rcheevos_cache_pending_list_t src;
   rcheevos_cache_pending_list_t dst;

   memset(&entry, 0, sizeof(entry));
   entry.id = 99001;

   src.entries     = &entry;
   src.num_entries = 1;

   ck_assert(rcheevos_cache_save_pending_unlocks("player1", 777, &src));
   ck_assert(rcheevos_cache_remove_pending_unlock("player1", 777, 99001, false));

   memset(&dst, 0, sizeof(dst));
   rcheevos_cache_get_pending_unlocks("player1", 777, &dst);
   ck_assert_uint_eq(dst.num_entries, 0);
   rcheevos_cache_pending_list_free(&dst);
}
END_TEST

/* Remove one of two entries: the other must still be there. */
START_TEST(test_pending_remove_one_of_two)
{
   rcheevos_cache_pending_t    entries[2];
   rcheevos_cache_pending_list_t src;
   rcheevos_cache_pending_list_t dst;
   bool found_22222 = false;
   uint32_t i;

   memset(entries, 0, sizeof(entries));
   entries[0].id = 11111;
   entries[1].id = 22222;

   src.entries     = entries;
   src.num_entries = 2;

   ck_assert(rcheevos_cache_save_pending_unlocks("player1", 777, &src));
   ck_assert(rcheevos_cache_remove_pending_unlock("player1", 777, 11111, false));

   memset(&dst, 0, sizeof(dst));
   ck_assert(rcheevos_cache_get_pending_unlocks("player1", 777, &dst));
   ck_assert_uint_eq(dst.num_entries, 1);

   for (i = 0; i < dst.num_entries; i++)
      if (dst.entries[i].id == 22222) found_22222 = true;
   ck_assert(found_22222);

   rcheevos_cache_pending_list_free(&dst);
}
END_TEST

/* Pending queues are per-game: game A's queue must not affect game B's. */
START_TEST(test_pending_queues_are_per_game)
{
   rcheevos_cache_pending_t    e_a;
   rcheevos_cache_pending_t    e_b;
   rcheevos_cache_pending_list_t src_a;
   rcheevos_cache_pending_list_t src_b;
   rcheevos_cache_pending_list_t dst_a;
   rcheevos_cache_pending_list_t dst_b;

   memset(&e_a, 0, sizeof(e_a)); e_a.id = 1001;
   memset(&e_b, 0, sizeof(e_b)); e_b.id = 2001;

   src_a.entries = &e_a; src_a.num_entries = 1;
   src_b.entries = &e_b; src_b.num_entries = 1;

   ck_assert(rcheevos_cache_save_pending_unlocks("player1", 111, &src_a));
   ck_assert(rcheevos_cache_save_pending_unlocks("player1", 222, &src_b));

   memset(&dst_a, 0, sizeof(dst_a));
   memset(&dst_b, 0, sizeof(dst_b));
   ck_assert(rcheevos_cache_get_pending_unlocks("player1", 111, &dst_a));
   ck_assert(rcheevos_cache_get_pending_unlocks("player1", 222, &dst_b));

   ck_assert_uint_eq(dst_a.num_entries, 1);
   ck_assert_uint_eq(dst_b.num_entries, 1);
   ck_assert_uint_eq(dst_a.entries[0].id, 1001);
   ck_assert_uint_eq(dst_b.entries[0].id, 2001);

   rcheevos_cache_pending_list_free(&dst_a);
   rcheevos_cache_pending_list_free(&dst_b);
}
END_TEST

/* A leaderboard entry (score included) must round-trip through disk. */
START_TEST(test_pending_leaderboard_score_survives_disk)
{
   rcheevos_cache_pending_t    entry;
   rcheevos_cache_pending_list_t src;
   rcheevos_cache_pending_list_t dst;
   bool found = false;
   uint32_t i;

   memset(&entry, 0, sizeof(entry));
   entry.id            = 55001;
   entry.is_leaderboard = true;
   entry.score         = 98765;

   src.entries     = &entry;
   src.num_entries = 1;

   ck_assert(rcheevos_cache_save_pending_unlocks("player1", 777, &src));

   memset(&dst, 0, sizeof(dst));
   ck_assert(rcheevos_cache_get_pending_unlocks("player1", 777, &dst));

   for (i = 0; i < dst.num_entries; i++)
   {
      if (dst.entries[i].id == 55001 &&
          dst.entries[i].is_leaderboard &&
          dst.entries[i].score == 98765)
         found = true;
   }
   ck_assert(found);

   rcheevos_cache_pending_list_free(&dst);
}
END_TEST

/************************************************************************
 * User unlock cache — file I/O layer                                    *
 *                                                                       *
 * Trajectory: server-confirmed unlocks saved at end of session, loaded  *
 * at start of next session (including offline).                         *
 ************************************************************************/

START_TEST(test_user_unlocks_save_and_load)
{
   rcheevos_cache_unlock_t  unlocks[2];
   rcheevos_cache_user_unlocks_t src;
   rcheevos_cache_user_unlocks_t dst;
   uint32_t ids = 0;
   uint32_t i;

   memset(unlocks, 0, sizeof(unlocks));
   unlocks[0].achievement_id = 1001;
   unlocks[1].achievement_id = 1002;

   memset(&src, 0, sizeof(src));
   src.username    = "player1";
   src.unlocks     = unlocks;
   src.num_unlocks = 2;

   ck_assert(rcheevos_cache_save_user_unlocks("player1", 777, false, &src));

   memset(&dst, 0, sizeof(dst));
   ck_assert(rcheevos_cache_get_user_unlocks("player1", 777, false, &dst));
   ck_assert_uint_eq(dst.num_unlocks, 2);

   for (i = 0; i < dst.num_unlocks; i++)
   {
      if (dst.unlocks[i].achievement_id == 1001) ids |= 1;
      if (dst.unlocks[i].achievement_id == 1002) ids |= 2;
   }
   ck_assert_uint_eq(ids, 3);

   rcheevos_cache_unlocks_free(&dst);
}
END_TEST

/* Hardcore and softcore unlock files must be independent. */
START_TEST(test_user_unlocks_hardcore_softcore_independent)
{
   rcheevos_cache_unlock_t sc_unlock;
   rcheevos_cache_unlock_t hc_unlock;
   rcheevos_cache_user_unlocks_t sc_src;
   rcheevos_cache_user_unlocks_t hc_src;
   rcheevos_cache_user_unlocks_t sc_dst;
   rcheevos_cache_user_unlocks_t hc_dst;

   memset(&sc_unlock, 0, sizeof(sc_unlock)); sc_unlock.achievement_id = 1001;
   memset(&hc_unlock, 0, sizeof(hc_unlock)); hc_unlock.achievement_id = 2001;

   memset(&sc_src, 0, sizeof(sc_src));
   sc_src.username = "player1"; sc_src.unlocks = &sc_unlock; sc_src.num_unlocks = 1;
   memset(&hc_src, 0, sizeof(hc_src));
   hc_src.username = "player1"; hc_src.unlocks = &hc_unlock; hc_src.num_unlocks = 1;

   ck_assert(rcheevos_cache_save_user_unlocks("player1", 777, false, &sc_src));
   ck_assert(rcheevos_cache_save_user_unlocks("player1", 777, true,  &hc_src));

   memset(&sc_dst, 0, sizeof(sc_dst));
   memset(&hc_dst, 0, sizeof(hc_dst));
   ck_assert(rcheevos_cache_get_user_unlocks("player1", 777, false, &sc_dst));
   ck_assert(rcheevos_cache_get_user_unlocks("player1", 777, true,  &hc_dst));

   ck_assert_uint_eq(sc_dst.num_unlocks, 1);
   ck_assert_uint_eq(hc_dst.num_unlocks, 1);
   ck_assert_uint_eq(sc_dst.unlocks[0].achievement_id, 1001);
   ck_assert_uint_eq(hc_dst.unlocks[0].achievement_id, 2001);

   rcheevos_cache_unlocks_free(&sc_dst);
   rcheevos_cache_unlocks_free(&hc_dst);
}
END_TEST

/************************************************************************
 * Suite assembly                                                        *
 ************************************************************************/

Suite *create_suite(void)
{
   Suite *s          = suite_create(SUITE_NAME);
   TCase *tc_pending = tcase_create("Pending");
   TCase *tc_unlocks = tcase_create("Unlocks");

   tcase_add_checked_fixture(tc_pending, fixture_setup, fixture_teardown);
   tcase_add_test(tc_pending, test_pending_save_and_load_one_entry);
   tcase_add_test(tc_pending, test_pending_save_and_load_two_entries);
   tcase_add_test(tc_pending, test_pending_remove_after_sync);
   tcase_add_test(tc_pending, test_pending_remove_one_of_two);
   tcase_add_test(tc_pending, test_pending_queues_are_per_game);
   tcase_add_test(tc_pending, test_pending_leaderboard_score_survives_disk);
   suite_add_tcase(s, tc_pending);

   tcase_add_checked_fixture(tc_unlocks, fixture_setup, fixture_teardown);
   tcase_add_test(tc_unlocks, test_user_unlocks_save_and_load);
   tcase_add_test(tc_unlocks, test_user_unlocks_hardcore_softcore_independent);
   suite_add_tcase(s, tc_unlocks);

   return s;
}

int main(void)
{
   int num_fail;
   Suite  *s  = create_suite();
   SRunner *sr = srunner_create(s);
   srunner_run_all(sr, CK_NORMAL);
   num_fail = srunner_ntests_failed(sr);
   srunner_free(sr);
   return (num_fail == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
