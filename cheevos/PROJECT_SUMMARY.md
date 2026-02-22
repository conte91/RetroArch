# RetroAchievements Offline Cache - Project Summary

**Last Updated**: Session 3
**Branch**: `achievement_cache`
**Status**: Core offline cache flow working end-to-end. Duplicate-queue bug fixed.

---

## What's Working Now

### Offline unlock flow
1. Achievement triggered → `rcheevos_client_award_achievement()` → HTTP request
2. If request fails (offline / bad host): `rcheevos_client_award_achievement_callback` calls `rcheevos_queue_achievement_sync()` → saves to `pending.json`
3. On next game load: `rcheevos_sync_pending_state()` reads `pending.json` into `rcheevos_locals->pending_achievement_queue`
4. `rcheevos_client_apply_pending_unlocks()` clears `active` bits for pending achievements → rc_runtime won't re-trigger them → **no duplicates**
5. `synced_active` stays set (= server's locked view) → menu shows NEED_SYNC bucket

### Online sync flow (when connectivity restored)
1. `rcheevos_client_start_session()` starts the network poll task
2. Every 60s: `rcheevos_poll_dispatch_pending_achievements()` dispatches one entry at a time
3. On success: removes from `pending.json`, updates unlock cache, sets `synced_active = active`, chains next dispatch
4. On failure: keeps entry in queue, retries next poll cycle
5. When in-memory queue exhausted: re-reads `pending.json` in case new entries arrived

### State tracking
- `synced_active = active` initialized at load time from server-reported unlock state
- `synced_active = active` updated on confirmed server sync (both normal and pending callbacks)
- `active != synced_active` → `RCHEEVOS_MENUITEM_BUCKET_NEED_SYNC_ENTRY` in menu
- `game.needs_sync` set when pending queue is non-empty

### Testing
- Set `cheevos_custom_host = "http://peppino.usuraio"` in `testconfig/retroarch.cfg` to simulate offline / unreachable server without rebuilding

---

## Key Design Decisions Made

### Pending queue lives in `rcheevos_locals_t`
Fields: `rcheevos_cache_pending_t *pending_achievement_queue` + `int pending_achievement_queue_size`

Read from disk **once** at end of fetch-unlocks stage (before `rcheevos_client_copy_achievements`). Shared between the backfill pass and the network poll dispatch — no repeated disk reads.

### Dispatch is one-at-a-time, tail-popping
`rcheevos_poll_dispatch_pending_achievements` dispatches the last entry, decrements size on success. Only re-reads disk when size hits 0.

### Shared HTTP response parser
`rcheevos_parse_award_achievement_response()` is used by both the normal award callback and the pending sync callback. Extracts `awarded_achievement_id` + `achievements_remaining`, handles error logging.

### Unlock cache vs pending queue
- **Unlock cache** (`user/<user>/unlocks/<game_id>_[hc].json`): server-confirmed unlocks only
- **Pending queue** (`user/<user>/pending.json`): locally unlocked, not yet server-confirmed
- They are mutually exclusive — `rcheevos_register_achievement_unlocked` writes to unlock cache and `rcheevos_cache_remove_pending_unlock` removes from pending queue in the same success callback

---

## Key Code Locations

| What | Where |
|------|-------|
| Achievement triggered | `cheevos.c` → `rcheevos_award_achievement()` |
| HTTP award request | `cheevos_client.c` → `rcheevos_client_award_achievement()` |
| Normal award HTTP cb | `rcheevos_async_award_achievement_callback` |
| Normal award task cb | `rcheevos_client_award_achievement_callback` |
| Queue on failure | `rcheevos_queue_achievement_sync()` |
| Shared response parser | `rcheevos_parse_award_achievement_response()` |
| Load pending from disk | `rcheevos_sync_pending_state(username)` |
| Backfill active bits | `rcheevos_client_apply_pending_unlocks()` |
| Find cheevo by id | `rcheevos_find_achievement_by_id(id)` |
| Update unlock cache | `rcheevos_register_achievement_unlocked(...)` |
| Remove from pending | `rcheevos_cache_remove_pending_unlock(...)` |
| Network poll task | `rcheevos_async_network_state_poll_handler()` |
| Dispatch one pending | `rcheevos_poll_dispatch_pending_achievements(state)` |
| Pending HTTP cb | `rcheevos_async_award_pending_callback` |
| Pending task cb | `rcheevos_client_award_pending_callback` |
| Menu bucket logic | `cheevos_menu.c` → `rcheevos_menu_update_bucket()` |
| Needs sync flag check | `cheevos_menu.c` → checks `locals->game.needs_sync` |

---

## Design Logic for synced_active

| State | `active` | `synced_active` | Effect |
|-------|----------|-----------------|--------|
| Locked (server agrees) | `SOFT\|HARD` | `SOFT\|HARD` | Normal locked, rc_runtime active |
| Unlocked + synced | `0` | `0` | Normal unlocked |
| **Pending** (offline unlock) | **`0`** | **`SOFT\|HARD`** | rc_runtime inactive (no retrigger), menu shows NEED_SYNC |
| Just synced successfully | `0` | `0` | NEED_SYNC clears |

---

## Remaining Work / Known Gaps

- **Leaderboard pending sync**: skipped with a log message in `rcheevos_client_dispatch_pending_entry` — not yet implemented
- **`rcheevos_cache_process_pending_queue()`**: declared in `cheevos_cache.h` but never implemented (was superseded by the poll-based approach — may be safe to remove the declaration)
- **Unlock cache not written on offline unlock**: when an achievement is queued offline, nothing is written to the unlock cache yet. It's written when the pending sync succeeds. This means if the game is re-loaded offline again, the unlock cache won't have the entry — but `apply_pending_unlocks` handles this correctly by clearing `active` from the pending queue data.
- **Retry cap**: pending entries have a `retries` field in `rcheevos_cache_pending_t` but it's never incremented or checked — no max retry limit enforced yet
- **Mutex**: `pending_sync_request` flag in `rcheevos_async_network_state_poll_state_t` has a TODO comment for locking — currently safe because all callbacks run on the main thread

---

## Architecture Notes

- All async/callback-driven — nothing blocks the main thread
- HTTP requests via `task_push_http_*`, callbacks always run on main thread via `task_queue_check()`
- `rcheevos_async_end_request` always calls `request->callback` even on init failure (synchronous path) — so flag ordering matters (set `pending_sync_request = true` BEFORE calling `rcheevos_async_begin_request`)
- `CHEEVOS_NETWORK_POLL_FREQUENCY` = 60s, `CHEEVOS_PING_FREQUENCY` = 2min
