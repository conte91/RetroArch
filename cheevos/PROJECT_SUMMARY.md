# RetroAchievements Offline Cache - Project Summary

> **Purpose of this document**: At any point in time, Claude should be able to read this file
> and immediately resume work on this feature without needing any other context. Keep it up to date!

**Branch**: `achievement_cache`
**Build command**: `make DEBUG=1 MIYOO354=1 NATIVE=1`
**Test config**: `testconfig/retroarch.cfg` — set `cheevos_custom_host = "http://peppino.usuraio"` to simulate offline/unreachable RA server without rebuilding. (File is gitignored, lives locally.)

---

## What This Feature Does

Adds an offline achievement cache so that achievements unlocked without internet connectivity are:
1. Stored locally in a `pending.json` queue
2. Automatically synced to the RetroAchievements server when connectivity is restored
3. Correctly reflected in the menu (NEED_SYNC bucket) while pending
4. Not re-triggered by the rc_runtime on subsequent game loads (dedup)

---

## RetroArch Architecture (important context)

- **All network I/O is async/callback-driven** — nothing blocks the main thread
- HTTP requests go through `task_push_http_*` (see `tasks/tasks_internal.h`)
- Callbacks **always run on the main thread** via `task_queue_check()` — safe for state updates
- `rcheevos_async_io_request` carries: `handler` (HTTP-level cb), `callback` (task-level cb), `handler_data`, `callback_data`
- `rcheevos_async_end_request` always calls `request->callback` even on init failure (synchronous path) — so set any "in-flight" flags BEFORE calling `rcheevos_async_begin_request`
- `CHEEVOS_NETWORK_POLL_FREQUENCY` = 60s, `CHEEVOS_PING_FREQUENCY` = 2min

---

## Cache File Layout

All cache files live under RetroArch's thumbnail dir, e.g.:
```
~/.config/retroarch/thumbnails/cheevos/cache/
  hash/<md5>.json                         — hash → game_id mapping
  game/<game_id>.json                     — game data (achievements, leaderboards)
  user/<username>/unlocks/<game_id>.json      — server-confirmed softcore unlocks
  user/<username>/unlocks/<game_id>_hc.json  — server-confirmed hardcore unlocks
  user/<username>/pending.json            — locally unlocked, not yet synced to server
```

**Unlock cache** and **pending queue** are mutually exclusive:
- Unlock cache = server confirmed it
- Pending queue = locally unlocked, server doesn't know yet

---

## Key Structs

### `rcheevos_racheevo_t` (cheevos_locals.h)
```c
uint8_t active;        // bits set = still locked. 0 = unlocked locally
uint8_t synced_active; // mirrors active but only updated on server confirmation
```
`active != synced_active` → menu shows `RCHEEVOS_MENUITEM_BUCKET_NEED_SYNC_ENTRY`

| State | `active` | `synced_active` |
|-------|----------|-----------------|
| Locked (server agrees) | `SOFT\|HARD` | `SOFT\|HARD` |
| Unlocked + synced | `0` | `0` |
| **Pending** (offline unlock) | **`0`** | **`SOFT\|HARD`** |

### `rcheevos_locals_t` (cheevos_locals.h)
```c
rcheevos_cache_pending_t *pending_achievement_queue; // in-memory snapshot of pending.json
int pending_achievement_queue_size;
bool logged_in;           // false = offline/not logged in
rcheevos_game_info_t game;
  .needs_sync             // true when pending queue is non-empty
```

### `rcheevos_cache_pending_t` (cheevos_cache_data.h)
```c
uint32_t game_id;
uint32_t id;          // achievement or leaderboard ID
time_t   timestamp;   // when unlock occurred
uint32_t retries;     // retry count (not yet enforced)
bool     hardcore;
bool     is_leaderboard;
int32_t  score;       // leaderboard score if is_leaderboard
```

### `rcheevos_async_network_state_poll_state_t` (cheevos_client.c, local)
```c
bool online;
bool pending_sync_request; // true while an HTTP sync is in-flight
unsigned game_id;
rcheevos_async_io_request *net_poll_request;
rcheevos_async_io_request *ping_request;
```

---

## Full Achievement Award Flow

### Online path (normal)
```
cheevos.c: rcheevos_award_achievement()
  → cheevo->active &= ~RCHEEVOS_ACTIVE_SOFTCORE  (clears active locally)
  → rcheevos_client_award_achievement(cheevo->id)
      → builds HTTP request
      → rcheevos_async_award_achievement_callback  [HTTP level]
          → rcheevos_parse_award_achievement_response()  [shared parser]
          → sets cb_data->award_success, awarded_achievement_id, achievements_remaining
      → rcheevos_client_award_achievement_callback  [task level]
          → if success:
              rcheevos_register_achievement_unlocked(...)  → writes unlock cache
              rcheevos_find_achievement_by_id() → cheevo->synced_active = cheevo->active
          → if failure:
              rcheevos_queue_achievement_sync(...)  → appends to pending.json
```

### Offline path (queued)
```
Same start, but HTTP fails →
  rcheevos_queue_achievement_sync() → pending.json gets new entry
  (active already cleared by cheevos.c, synced_active still set → NEED_SYNC in menu)
```

### Pending sync (background poll)
```
rcheevos_client_start_session()
  → allocates rcheevos_async_network_state_poll_state_t
  → rcheevos_client_start_network_state_poll()
      → retro_task fires every 60s: rcheevos_async_network_state_poll_handler()
          → rcheevos_poll_dispatch_pending_achievements(state)
              → if !pending_sync_request && queue_size > 0:
                  rcheevos_client_dispatch_pending_entry(state, locals, &queue[size-1])
                      → builds award HTTP request
                      → rcheevos_async_award_pending_callback  [HTTP level]
                          → rcheevos_parse_award_achievement_response()  [shared parser]
                      → rcheevos_client_award_pending_callback  [task level]
                          → if success:
                              rcheevos_cache_remove_pending_unlock()
                              rcheevos_register_achievement_unlocked()  → unlock cache
                              rcheevos_find_achievement_by_id() → synced_active = active
                              locals->pending_achievement_queue_size--
                              if size == 0: rcheevos_sync_pending_state()  → re-read disk
                              rcheevos_poll_dispatch_pending_achievements()  → chain next
                          → if failure: log, retry next poll cycle
```

### Initialization (game load)
```
fetch game data + fetch user unlocks (from server or local cache)
  → rcheevos_client_initialize_runtime_callback()
      → rcheevos_sync_pending_state(username)       ← single disk read of pending.json
      → rcheevos_client_copy_achievements()          ← sets active + synced_active from server data
      → rcheevos_client_apply_pending_unlocks()      ← clears active bits for pending entries
                                                        (prevents rc_runtime from retriggering)
      → rcheevos_client_copy_leaderboards()
  → rcheevos_client_start_session()
      → rcheevos_client_start_network_state_poll()  ← pending queue already in locals, no re-read
```

---

## Key Functions Reference

| Function | File | Purpose |
|----------|------|---------|
| `rcheevos_award_achievement()` | `cheevos.c` | Entry point when rc_runtime triggers achievement |
| `rcheevos_client_award_achievement()` | `cheevos_client.c` | Builds and fires HTTP award request |
| `rcheevos_parse_award_achievement_response()` | `cheevos_client.c` | Shared HTTP response parser for both normal and pending flows |
| `rcheevos_queue_achievement_sync()` | `cheevos_client.c` | Appends entry to pending.json |
| `rcheevos_sync_pending_state()` | `cheevos_client.c` | Reads pending.json into `locals->pending_achievement_queue`, sets `needs_sync` |
| `rcheevos_client_apply_pending_unlocks()` | `cheevos_client.c` | Clears `active` bits for pending entries after game load |
| `rcheevos_find_achievement_by_id()` | `cheevos_client.c` | Linear scan of `game.achievements` by id |
| `rcheevos_register_achievement_unlocked()` | `cheevos_client.c` | Writes confirmed unlock to unlock cache, shows mastery placard if complete |
| `rcheevos_poll_dispatch_pending_achievements()` | `cheevos_client.c` | Called each poll tick; dispatches one pending entry if no request in flight |
| `rcheevos_client_dispatch_pending_entry()` | `cheevos_client.c` | Builds and fires HTTP request for a single pending entry |
| `rcheevos_cache_remove_pending_unlock()` | `cheevos_cache.c` | Removes one entry from pending.json by id |
| `rcheevos_cache_get_pending_unlocks()` | `cheevos_cache.c` | Reads and deserializes pending.json |
| `rcheevos_cache_save_pending_unlocks()` | `cheevos_cache.c` | Serializes and writes pending.json |
| `rcheevos_menu_update_bucket()` | `cheevos_menu.c` | Sets NEED_SYNC bucket when `active != synced_active` |

---

## What's Not Done Yet

### Leaderboard pending sync
In `rcheevos_client_dispatch_pending_entry`, leaderboard entries are currently skipped with a log message. Need to implement similarly to achievement sync but using `rc_api_init_submit_lboard_entry_request` / `rc_api_process_submit_lboard_entry_response`.

### Retry cap
`rcheevos_cache_pending_t.retries` field exists but is never incremented or checked. No max retry limit is enforced — a permanently-invalid entry would be retried forever.

### Stale declaration
`rcheevos_cache_process_pending_queue()` is declared in `cheevos_cache.h` but never implemented (superseded by the poll-based approach). Safe to remove.

### Unlock cache not populated during offline session
When an achievement is queued offline, nothing is written to the unlock cache — only to pending.json. The unlock cache is written when pending sync succeeds. This is correct behavior: `apply_pending_unlocks` uses the pending queue to suppress rc_runtime retriggers, so the unlock cache absence doesn't cause duplicates. But it means the unlock cache may be stale until next successful sync.
