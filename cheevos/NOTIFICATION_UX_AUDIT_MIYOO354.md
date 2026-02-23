# Cheevos Notification UX Audit (MIYOO354)

Date: 2026-02-23

Scope:
- Build/runtime assumptions: `MIYOO354=1`
- Focus: user-facing notification UX for Cheevos lifecycle and offline/pending-sync behavior
- Target paths: `cheevos/cheevos.c`, `cheevos/cheevos_client.c`

## Executive Summary

The current UX is functionally correct for local unlocking and basic login/load messaging, but it does not expose the new offline queue + background sync behavior clearly to the user.

Main gap:
- The system now supports pending offline unlock sync, but user-facing notifications still mostly imply a binary world ("unlocked" vs "error"), without communicating "unlocked locally, pending synchronization".

Result:
- Users can get an achievement popup while offline and assume it was fully synced.
- Users do not get closure when pending items are eventually synchronized.
- Load summary does not reflect unsynced local unlocks.

## Platform Context (MIYOO354)

`MIYOO354=1` enables Cheevos, but on this device/profile the UX is typically the non-widget message queue path (`runloop_msg_queue_push(...)`) rather than `gfx_widgets`.

Why this matters:
- Messages are text-only and stacked in sequence.
- Message spam is more noticeable.
- Wording must carry state clearly and briefly.

Relevant code paths:
- `cheevos/cheevos.c`
- `cheevos/cheevos_client.c`

## Current User Journey and Notifications

## 1. Login

### Current behavior

Success (if account notifications enabled):
- `RetroAchievements: Logged in as "X".`
- Source: `cheevos/cheevos.c:2128`

Failure / network / auth problems:
- Generic async error popup (e.g. communication/login error)
- Source: `cheevos/cheevos_client.c:500`, `cheevos/cheevos_client.c:540`, `cheevos/cheevos_client.c:587`

### Observations

- Good: success is explicit and short.
- Gap: no explicit "offline/cache mode" message when login fails but cached game data can still be used.

## 2. Game Identification / Load Start

### Current behavior

Unknown game:
- `RetroAchievements: Game could not be identified.` (verbose mode)
- Source: `cheevos/cheevos.c:1902`

Network error before fetch:
- Logs only (`No remote achievements available.`)
- Source: `cheevos/cheevos.c:1917`

### Observations

- Reasonable baseline.
- If cached data loads successfully after network/login issues, the user is not told that data came from cache and sync may be unavailable.

## 3. Game Summary Placard (post-load)

### Current behavior

Shown after load/session setup via:
- `rcheevos_start_session_finish()` -> `rcheevos_show_game_placard()`
- Source: `cheevos/cheevos.c:1859`, `cheevos/cheevos.c:1662`

Current summary strings include:
- `This game has no achievements.`
- `All X achievements activated for this session.`
- `You have X of Y achievements unlocked.`
- Optional `(... unsupported)` suffix

### Observations

- Good: user gets immediate context for the loaded game.
- Gap: summary does **not** reflect sync state (`active` vs `synced_active`), even though pending queue/sync is now a first-class feature.

### Impact

A user can have local unlocks pending sync, but the placard only shows unlocked totals and implies a fully settled state.

## 4. Achievement Unlock Event (runtime)

### Current behavior

When an achievement triggers:
- Local unlock popup shown immediately:
  - `Achievement Unlocked: <title>`
  - plus description as a second message
- Source: `cheevos/cheevos.c:317`, `cheevos/cheevos.c:345`

Then async submission is attempted:
- `rcheevos_client_award_achievement(...)`
- Source: `cheevos/cheevos.c:366`

### Observations

- Good: local feedback is immediate and responsive.
- Gap: this popup is identical whether server submission succeeds or fails and gets queued.

## 5. Award Submission Result (online success vs offline queue)

### Current behavior

Online success:
- Client updates unlock cache and `synced_active`
- Logs only (`Achievement ... successfully registered.`)
- Source: `cheevos/cheevos_client.c:2696`

Offline/failure:
- Achievement is appended to pending queue (`pending.json`)
- Logs only (`Queueing achievement ...`, pending queue save log)
- Source: `cheevos/cheevos_client.c:2715`, `cheevos/cheevos_client.c:2639`

### Observations

- This is the biggest UX inconsistency.
- The feature works, but the user is not told the unlock is local-only/pending sync.

## 6. Background Network Poll / Ping / Pending Sync

### Current behavior

Network state ping:
- Success/failure logs only (`Ping succeeded/failed`)
- Source: `cheevos/cheevos_client.c:2143`, `cheevos/cheevos_client.c:2149`

Pending sync success:
- Removes pending entry, updates unlock cache, sets `synced_active`
- Logs only (`Pending achievement ... synced to server.`)
- Source: `cheevos/cheevos_client.c:1979`

Pending sync failure:
- Logs only (`Failed to sync pending ..., will retry.`)
- Source: `cheevos/cheevos_client.c:2009`

Async HTTP error handling:
- Some failures still produce user-facing error popups via generic async layer
- Source: `cheevos/cheevos_client.c:500`, `cheevos/cheevos_client.c:540`

### Observations

- Good: background behavior is mostly silent.
- Gap: no positive completion signal when pending queue drains.
- Risk: repeated retry errors may surface as user-facing popup noise.

## 7. Mastery / Completion

### Current behavior

Mastery/completion placard is shown when the server award response reports `achievements_remaining == 0`:
- `rcheevos_show_mastery_placard()`
- Source: `cheevos/cheevos.c:1598`, `cheevos/cheevos_client.c:1958`

### Observations

- This is already good and should remain distinct from "sync complete" messaging.
- "All achievements synchronized" is not equivalent to "mastered/completed game".

## State Model (What UX Should Reflect)

The implementation now has at least three meaningful user-visible states for an unlocked achievement:

1. Local runtime unlocked
- The achievement fired in-game and local state changed (`active`).

2. Pending synchronization
- Local unlock exists, but server has not confirmed it yet (`active != synced_active`)
- Pending entry exists on disk and/or in memory queue.

3. Synchronized
- Server confirmed unlock, local sync state updated (`active == synced_active`)

The current UX mostly surfaces only state (1).

## Code Flow Map (Current)

### Login + Load Flow

1. `rcheevos_load()` starts load sequence
2. login request (if needed)
3. `rcheevos_login_callback()` shows login success message
4. `rcheevos_fetch_game_data()`
5. `rcheevos_client_initialize_runtime(...)`
6. `rcheevos_initialize_runtime_callback()` -> `rcheevos_start_session()`
7. `rcheevos_start_session_finish()` -> `rcheevos_show_game_placard()`

Key references:
- `cheevos/cheevos.c:2116`
- `cheevos/cheevos.c:1889`
- `cheevos/cheevos.c:1859`
- `cheevos/cheevos.c:1662`

### Unlock + Sync Flow

1. Runtime event triggers `rcheevos_award_achievement(...)`
2. Immediate local unlock popup shown
3. `rcheevos_client_award_achievement(...)` sends async award request
4. Client callback:
   - success -> mark synced and update unlock cache
   - failure -> queue pending unlock
5. Background ping/poll later retries pending
6. Pending callback syncs entry and updates `synced_active`

Key references:
- `cheevos/cheevos.c:317`
- `cheevos/cheevos_client.c:2696`
- `cheevos/cheevos_client.c:2639`
- `cheevos/cheevos_client.c:1979`
- `cheevos/cheevos_client.c:2107`

## UX Gaps (Prioritized)

## Gap A: Unlock popup does not indicate sync outcome

Severity: High

Problem:
- Offline unlocks look identical to online synchronized unlocks.

Effect:
- User may assume the unlock is safely synced when it is only queued locally.

## Gap B: Summary placard ignores pending-sync state

Severity: High

Problem:
- `rcheevos_show_game_placard()` only reports unlocked/total and unsupported counts.

Effect:
- The new offline sync feature is invisible at the main summary touchpoint.

## Gap C: No "sync complete" closure

Severity: Medium

Problem:
- Background sync completion has no user notification.

Effect:
- Users don't know if offline progress has been reconciled.

## Gap D: Potential background retry error spam

Severity: Medium

Problem:
- Generic async error messaging can show repeated errors for pending sync retries.

Effect:
- Noisy UX on flaky/offline connections, especially on Miyoo text queue.

## Proposed UX Improvements

## 1. Add Pending-Sync Info to Game Summary Placard (Recommended)

Where:
- `cheevos/cheevos.c:1662` (`rcheevos_show_game_placard`)

How:
- Count achievements where `active != synced_active`
- Append sync suffix to the existing summary string

Suggested text variants:
- `You have 23 of 30 achievements unlocked (2 pending sync).`
- `You have 23 of 30 achievements unlocked (all synchronized).`
- `All 30 achievements activated for this session (2 pending sync).` (if start-active mode)

Notes:
- Only show sync suffix when logged in or when pending entries exist (to avoid weirdness for anonymous/no-account sessions).

## 2. Add Explicit "Saved Locally / Will Sync Later" Notice on Award Queue (Recommended)

Where:
- `cheevos/cheevos_client.c:2696` (`rcheevos_client_award_achievement_callback`)

How:
- In the failure/queue branch, show a short info notification after enqueue succeeds

Suggested text:
- `Achievement saved locally (will sync later).`
- Better if cheap to compute count: `Achievement saved locally (2 pending sync).`

Notes:
- Keep local unlock popup unchanged; this is a second status message clarifying sync state.
- On Miyoo, keep message short to reduce clutter.

## 3. One-Shot "All Synchronized" Completion Message (Recommended)

Where:
- `cheevos/cheevos_client.c:1979` (`rcheevos_client_award_pending_callback`)

How:
- After successful pending sync decrements queue size, if queue becomes zero, show one info message

Suggested text:
- `All achievements synchronized :)`
- Alternative (more neutral): `Pending achievements synchronized.`

Notes:
- Do not emit per-item success popups for background sync (too noisy).
- Completion-only is the best UX/verbosity tradeoff for Miyoo.

## 4. Suppress User Popup Spam for Pending Retry Failures (Recommended)

Where:
- `cheevos/cheevos_client.c:500` (generic async error path)

How:
- Treat pending sync retry failures like ping/rich-presence/badge failures for UI purposes:
  - log them
  - do not `runloop_msg_queue_push(...)` on every retry

Notes:
- Keep a user-facing popup only for explicit foreground actions (login/manual attempts), not recurring background retry loops.

## 5. Optional: Cache-Mode Load Notice (Nice to Have)

Where:
- after initialize_runtime completes via cached data / login failed path

Suggested text:
- `Achievements loaded from cache (sync unavailable).`
- or `Offline achievements mode: progress will sync when you sign in.`

Notes:
- Useful when login failed but local features still work.
- Should be shown only once per load.

## Proposed Notification Policy (MIYOO354-Friendly)

Use a simple, low-noise model:

Show to user:
- login success
- game summary placard (with pending-sync suffix)
- local unlock popup
- local unlock queued-for-sync status (only when queueing occurs)
- all-pending-synced completion (only when queue drains)
- major blocking errors (unknown game, missing credentials, etc.)

Log only:
- ping fail/success
- per-item pending sync retries
- per-item pending sync successes
- rich presence/badge network noise

## Example User Journeys (Proposed)

## A. Normal Online Session

1. `RetroAchievements: Logged in as "Simo".`
2. `You have 23 of 30 achievements unlocked (all synchronized).`
3. `Achievement Unlocked: Combo Master`
4. (No extra sync popup if immediate award succeeds)

## B. Offline During Unlock

1. `Achievement Unlocked: Combo Master`
2. `Land 100 hits without damage`
3. `Achievement saved locally (will sync later) (1 pending sync).`

## C. Connection Returns Later

1. (No ping spam)
2. `All achievements synchronized :)`

## D. Reload Game With Pending Items

1. `You have 24 of 30 achievements unlocked (2 pending sync).`

## Implementation Checklist (Suggested)

1. Add helper to count pending-unsynced achievements in `cheevos.c`
2. Update `rcheevos_show_game_placard()` message formatting to append sync suffix
3. Add queue-notice popup in `rcheevos_client_award_achievement_callback()` failure branch
4. Add queue-drained completion popup in `rcheevos_client_award_pending_callback()`
5. Suppress recurring pending-sync retry popups in generic async error handling
6. Add tests for message behavior (mock `runloop_msg_queue_push`) where feasible

## Risks / Tradeoffs

- Over-notification risk:
  - Mitigated by only notifying on queueing and final completion, not every retry/success item.

- Wording confusion with mastery:
  - Keep "all synchronized" distinct from "mastered/completed game".

- Edge cases (logged out / no account):
  - Avoid claiming "synchronized" when no account is active.

## Closing Assessment

The underlying feature set (offline pending queue + background sync) is now stronger than the current UX communicates. A small set of targeted text-message updates would make the system feel coherent and trustworthy on Miyoo354, especially under unreliable connectivity.
