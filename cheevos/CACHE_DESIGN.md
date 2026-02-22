# RetroAchievements Offline Cache Design

## Overview

This document describes the data structures that need to be cached for offline achievement functionality. The goal is to cache data when a game is opened online, so subsequent offline sessions can display and trigger achievements locally.

## Cache Flow

1. **Online game load:** Fetch data from server, save to cache
2. **Offline game load:** Load from cache, skip server calls
3. **Achievement triggered offline:** Queue for later sync
4. **Back online:** Process pending queue, submit to server

---

## Data Structures to Cache

### 1. Hash → Game ID Mapping

**Purpose:** Skip the `/resolveHash` API call on subsequent loads by caching the ROM hash to game ID mapping.

**Source:** Result of `rcheevos_client_identify_game()`

```c
typedef struct {
    char hash[33];           /* SHA1 hash of ROM (32 hex chars + null) */
    uint32_t game_id;        /* RetroAchievements game ID */
} rcheevos_cache_hash_entry_t;
```

**Cache key:** ROM file path or hash
**Lifetime:** Permanent (hash→game_id never changes)

---

### 2. Game Data

**Purpose:** Store game metadata and all achievement/leaderboard definitions.

**Source:** `rc_api_fetch_game_data_response_t` from `/API/API_GetGameData.php`

```c
typedef struct {
    uint32_t id;                      /* Game ID */
    uint32_t console_id;              /* Console type ID */
    char* title;                      /* Game title */
    char* image_name;                 /* Game badge image filename */
    char* rich_presence_script;       /* Rich presence script (optional) */

    uint32_t num_achievements;
    rcheevos_cache_achievement_t* achievements;

    uint32_t num_leaderboards;
    rcheevos_cache_leaderboard_t* leaderboards;
} rcheevos_cache_game_data_t;
```

**Cache key:** `game_id`
**Lifetime:** Can be refreshed periodically (achievements may be updated)

---

### 3. Achievement Definition

**Purpose:** Store complete achievement data including trigger logic for offline evaluation.

**Source:** `rc_api_achievement_definition_t` from server response

```c
typedef struct {
    uint32_t id;              /* Achievement ID */
    uint32_t points;          /* Points value */
    uint32_t category;        /* 3 = core (official), 5 = unofficial */
    char* title;              /* Achievement name */
    char* description;        /* Achievement description */
    char* definition;         /* TRIGGER LOGIC STRING - CRITICAL FOR OFFLINE */
    char* author;             /* Author username */
    char* badge_name;         /* Badge image filename (without extension) */
    time_t created;           /* Server creation timestamp */
    time_t updated;           /* Server last modified timestamp */
} rcheevos_cache_achievement_t;
```

**Critical field:** `definition` - This is the trigger condition string (called `memaddr` internally). It gets compiled by `rc_runtime_activate_achievement()` and evaluated every frame against game memory. **MUST be cached for offline triggering.**

**Category values:**
- `RC_ACHIEVEMENT_CATEGORY_CORE = 3` - Official achievements
- `RC_ACHIEVEMENT_CATEGORY_UNOFFICIAL = 5` - Unofficial/community achievements

---

### 4. Leaderboard Definition

**Purpose:** Store leaderboard data for offline tracking.

**Source:** `rc_api_leaderboard_definition_t` from server response

```c
typedef struct {
    uint32_t id;              /* Leaderboard ID */
    int32_t format;           /* Score format type for rc_format_value() */
    char* title;              /* Leaderboard name */
    char* description;        /* Leaderboard description */
    char* definition;         /* TRIGGER/SUBMIT LOGIC STRING */
    int lower_is_better;      /* Non-zero if lower scores are better */
    int hidden;               /* Non-zero if hidden from leaderboard list */
} rcheevos_cache_leaderboard_t;
```

**Critical field:** `definition` - Contains start/cancel/submit/value conditions.

---

### 5. User Unlock State

**Purpose:** Track which achievements the user has already earned (to avoid re-awarding).

**Source:** `rc_api_fetch_user_unlocks_response_t` from `/API/API_GetUserUnlocks.php`

```c
typedef struct {
    uint32_t* achievement_ids;      /* Array of earned achievement IDs */
    uint32_t num_achievement_ids;   /* Count of earned achievements */
} rcheevos_cache_user_unlocks_t;
```

**Cache key:** `username` + `game_id` + `hardcore_mode`
**Note:** Separate caches needed for hardcore and softcore modes.

---

### 6. Pending Sync Queue

**Purpose:** Queue offline achievement unlocks for later submission to server.

**Source:** Generated locally when achievements trigger offline

```c
typedef struct {
    uint32_t id;              /* Achievement or leaderboard ID */
    time_t timestamp;         /* When the unlock/submit occurred */
    uint32_t retries;         /* Number of failed sync attempts */
    bool hardcore;            /* Hardcore mode flag */
    bool is_leaderboard;      /* true = leaderboard, false = achievement */
    int32_t score;            /* Leaderboard score (if is_leaderboard) */
} rcheevos_cache_pending_t;
```

**Note:** `game_id` is not stored in this struct — the pending queue is per-game (stored under `users/<username>/<game_id>/pending.json`), so it is implicit.

**Processing:** On next online session, iterate queue and call:
- `rcheevos_client_award_achievement()` for achievements
- `rcheevos_client_submit_lboard_entry()` for leaderboards

---

## File Structure

Organized for efficient lookup - only load what you need:

```
<cache_dir>/cheevos/
├── hashes/
│   └── <hash>.json                       # One file per ROM hash → game_id
├── games/
│   └── <game_id>/
│       └── game.json                     # Achievement + leaderboard definitions
└── users/
    └── <username>/
        └── <game_id>/
            ├── unlocks_softcore.json     # User's softcore unlocks for this game
            ├── unlocks_hardcore.json     # User's hardcore unlocks for this game
            └── pending.json             # Queued unlocks for this game (if any)
```

**Example for game 1234, user "player1":**
```
<cache_dir>/cheevos/
├── hashes/
│   └── abc123def456789...json            # → {"game_id": 1234}
├── games/
│   └── 1234/
│       └── game.json                     # Shared achievement definitions
└── users/
    └── player1/
        └── 1234/
            ├── unlocks_softcore.json     # player1's softcore progress
            ├── unlocks_hardcore.json     # player1's hardcore progress
            └── pending.json             # player1's pending unlocks for game 1234
```

**Loading game for user:**
1. Hash ROM → `"abc123def456789..."`
2. Read `hashes/abc123def456789....json` → get `game_id: 1234`
3. Read `games/1234/game.json` → achievement definitions (shared)
4. Read `users/player1/1234/unlocks_*.json` → user's progress

---

## JSON Schemas

### hashes/{hash}.json
```json
{
  "game_id": 1234
}
```

### games/{game_id}/game.json
```json
{
  "id": 1234,
  "console_id": 4,
  "title": "Game Title",
  "image_name": "012345",
  "rich_presence_script": "Display @Score(0x1234)",
  "cached_at": 1609459200,
  "achievements": [
    {
      "id": 1001,
      "points": 10,
      "category": 3,
      "title": "First Achievement",
      "description": "Do something cool",
      "definition": "0xH1234=1",
      "author": "DevName",
      "badge_name": "00001",
      "created": 1609459200,
      "updated": 1609459200
    }
  ],
  "leaderboards": [
    {
      "id": 2001,
      "format": 1,
      "title": "High Score",
      "description": "Get the highest score",
      "definition": "STA:0xH1234=1::CAN:0xH1234=0::SUB:0xH1234=2::VAL:0xH5678",
      "lower_is_better": 0,
      "hidden": 0
    }
  ]
}
```

### users/{username}/{game_id}/unlocks_softcore.json
```json
{
  "achievement_ids": [1001, 1002, 1005],
  "last_updated": 1609459200
}
```

### users/{username}/{game_id}/unlocks_hardcore.json
```json
{
  "achievement_ids": [1001, 1002],
  "last_updated": 1609459200
}
```

### users/{username}/pending.json
```json
[
  {
    "game_id": 1234,
    "id": 1003,
    "timestamp": 1609460000,
    "retries": 0,
    "hardcore": true,
    "is_leaderboard": false,
    "score": 0
  },
  {
    "game_id": 1234,
    "id": 2001,
    "timestamp": 1609460500,
    "retries": 0,
    "hardcore": true,
    "is_leaderboard": true,
    "score": 99999
  }
]
```

---

## Integration Points

### When to Save Cache

1. **After `rcheevos_client_identify_game()` succeeds:** Save hash→game_id
2. **After `rcheevos_client_initialize_runtime()` completes:** Save game data + user unlocks
3. **After `rcheevos_award_achievement()` locally:** Update user unlocks + add to pending queue

### When to Load Cache

1. **In `rcheevos_identify_game()`:** Check hash cache before calling server
2. **In `rcheevos_fetch_game_data()`:** Load from cache if offline or cache-first mode
3. **On startup:** Process pending queue if online

### Key Functions to Hook

- `cheevos_client.c`: `rcheevos_async_resolve_hash_callback()` - hash resolved
- `cheevos_client.c`: `rcheevos_async_fetch_game_data_callback()` - game data received
- `cheevos_client.c`: `rcheevos_async_fetch_user_unlocks_callback()` - unlocks received
- `cheevos.c`: `rcheevos_award_achievement()` - achievement triggered

---

## Source References

- `cheevos/cheevos_locals.h:68-87` - `rcheevos_racheevo_t` (internal achievement struct)
- `cheevos/cheevos_locals.h:89-103` - `rcheevos_ralboard_t` (internal leaderboard struct)
- `cheevos/cheevos_locals.h:138-155` - `rcheevos_game_info_t` (game info struct)
- `deps/rcheevos/include/rc_api_runtime.h:80-96` - `rc_api_leaderboard_definition_t`
- `deps/rcheevos/include/rc_api_runtime.h:99-121` - `rc_api_achievement_definition_t`
- `deps/rcheevos/include/rc_api_runtime.h:129-154` - `rc_api_fetch_game_data_response_t`
- `deps/rcheevos/include/rc_api_user.h:98-107` - `rc_api_fetch_user_unlocks_response_t`
