/* Test stub header for cheevos_client.c unit tests.
 * Force-included before all sources via -include flag.
 *
 * Blocks heavy RetroArch headers and provides just enough types/decls for
 * cheevos_client.c to compile in isolation.
 */

#ifndef TEST_CHEEVOS_CLIENT_STUBS_H
#define TEST_CHEEVOS_CLIENT_STUBS_H

/* cheevos_locals.h replacement */
#ifndef __RARCH_CHEEVOS_LOCALS_H
#define __RARCH_CHEEVOS_LOCALS_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <boolean.h>
#include <retro_common_api.h>
#include <queues/task_queue.h>

#include "../../../deps/rcheevos/include/rc_runtime.h"
#include "../../../deps/rcheevos/src/rcheevos/rc_libretro.h"
#include "../../cheevos_cache_data.h"

#define RCHEEVOS_STRINGIFY2(x) #x
#define RCHEEVOS_STRINGIFY(x)  RCHEEVOS_STRINGIFY2(x)
#define RCHEEVOS_TAG "[RCHEEVOS]: (" __FILE__ ":" RCHEEVOS_STRINGIFY(__LINE__) ") "
#define CHEEVOS_LOG(...) fprintf(stderr, __VA_ARGS__)
#define CHEEVOS_ERR(...) fprintf(stderr, __VA_ARGS__)

enum
{
   RCHEEVOS_ACTIVE_SOFTCORE    = 1 << 0,
   RCHEEVOS_ACTIVE_HARDCORE    = 1 << 1,
   RCHEEVOS_ACTIVE_UNOFFICIAL  = 1 << 2,
   RCHEEVOS_ACTIVE_UNSUPPORTED = 1 << 3
};

enum rcheevos_load_state
{
   RCHEEVOS_LOAD_STATE_NONE = 0,
   RCHEEVOS_LOAD_STATE_IDENTIFYING_GAME,
   RCHEEVOS_LOAD_STATE_FETCHING_GAME_DATA,
   RCHEEVOS_LOAD_STATE_STARTING_SESSION,
   RCHEEVOS_LOAD_STATE_FETCHING_BADGES,
   RCHEEVOS_LOAD_STATE_DONE,
   RCHEEVOS_LOAD_STATE_UNKNOWN_GAME,
   RCHEEVOS_LOAD_STATE_NETWORK_ERROR,
   RCHEEVOS_LOAD_STATE_LOGIN_FAILED,
   RCHEEVOS_LOAD_STATE_ABORTED
};

enum rcheevos_summary_notif
{
   RCHEEVOS_SUMMARY_ALLGAMES = 0,
   RCHEEVOS_SUMMARY_HASCHEEVOS,
   RCHEEVOS_SUMMARY_OFF,
   RCHEEVOS_SUMMARY_LAST
};

typedef struct rcheevos_load_info_t
{
   enum rcheevos_load_state state;
   int hashes_tried;
   int outstanding_requests;
} rcheevos_load_info_t;

typedef struct rcheevos_racheevo_t
{
   const char *title;
   const char *description;
   const char *badge;
   const char *memaddr;
   unsigned id;
   unsigned points;
   retro_time_t unlock_time;
   uint8_t active;
   uint8_t synced_active;
} rcheevos_racheevo_t;

typedef struct rcheevos_ralboard_t
{
   const char *title;
   const char *description;
   const char *mem;
   unsigned id;
   unsigned format;
} rcheevos_ralboard_t;

typedef struct rcheevos_game_info_t
{
   int id;
   int console_id;
   char *title;
   char badge_name[16];
   const char *hash;
   bool mastery_placard_shown;
   rc_libretro_hash_set_t hashes;
   rcheevos_racheevo_t *achievements;
   rcheevos_ralboard_t *leaderboards;
   unsigned achievement_count;
   unsigned leaderboard_count;
   bool needs_sync;
} rcheevos_game_info_t;

typedef struct rcheevos_locals_t
{
   rc_runtime_t runtime;
   rcheevos_game_info_t game;
   rc_libretro_memory_regions_t memory;
   bool logged_in;
   char displayname[32];
   char username[32];
   char token[32];
   char user_agent_prefix[128];
   char user_agent_core[256];
   bool hardcore_active;
   bool loaded;
   bool core_supports;
   bool leaderboards_enabled;
   bool leaderboard_notifications;
   bool leaderboard_trackers;
   rcheevos_load_info_t load_info;
   rcheevos_cache_pending_t *pending_achievement_queue;
   int pending_achievement_queue_size;
} rcheevos_locals_t;

RETRO_BEGIN_DECLS
rcheevos_locals_t *get_rcheevos_locals(void);
void rcheevos_begin_load_state(enum rcheevos_load_state state);
int rcheevos_end_load_state(void);
bool rcheevos_load_aborted(void);
void rcheevos_show_mastery_placard(void);
RETRO_END_DECLS

#endif /* __RARCH_CHEEVOS_LOCALS_H */

/* retroarch.h replacement */
#ifndef __RETROARCH_H
#define __RETROARCH_H

#include <retro_miscellaneous.h>
#include <queues/message_queue.h>

struct retro_system_info
{
   const char *library_name;
   const char *library_version;
   const char *valid_extensions;
   bool need_fullpath;
   bool block_extract;
};

typedef struct
{
   struct { struct retro_system_info info; } system;
} runloop_state_t;

RETRO_BEGIN_DECLS
runloop_state_t *runloop_state_get_ptr(void);
void runloop_msg_queue_push(const char *msg, unsigned prio, unsigned duration,
      bool flush, char *title, enum message_queue_icon icon,
      enum message_queue_category category);
RETRO_END_DECLS

#endif

/* tasks_internal.h replacement */
#ifndef TASKS_HANDLER_INTERNAL_H
#define TASKS_HANDLER_INTERNAL_H

typedef struct
{
   char *data;
   size_t len;
   int status;
} http_transfer_data_t;

RETRO_BEGIN_DECLS
void *task_push_http_transfer_with_user_agent(
      const char *url, bool mute, const char *type,
      const char *user_agent, retro_task_callback_t cb, void *userdata);
void *task_push_http_post_transfer_with_user_agent(
      const char *url, const char *post_data, bool mute, const char *type,
      const char *user_agent, retro_task_callback_t cb, void *userdata);
RETRO_END_DECLS

#endif

#ifndef __NET_HTTP_SPECIAL_H
#define __NET_HTTP_SPECIAL_H
#endif

/* frontend_driver.h replacement */
#ifndef __FRONTEND_DRIVER_H
#define __FRONTEND_DRIVER_H
typedef struct frontend_ctx_driver
{
   void (*init)(void*);
   void (*deinit)(void*);
   void (*environ_cb)(int, void*);
   void (*process_args)(int*, char*[]);
   void (*exec)(const char*, bool);
   bool (*set_fork)(void*);
   void (*shutdown)(bool);
   void (*get_name)(char*, size_t);
   void (*get_os)(char*, size_t, int*, int*);
} frontend_ctx_driver_t;
RETRO_BEGIN_DECLS
frontend_ctx_driver_t *frontend_get_ptr(void);
RETRO_END_DECLS
#endif

/* paths.h replacement */
#ifndef __PATHS_H
#define __PATHS_H
enum rarch_path_type
{
   RARCH_PATH_CORE = 0,
   RARCH_PATH_CONFIG,
   RARCH_PATH_SAVEFILE
};
RETRO_BEGIN_DECLS
const char *path_get(enum rarch_path_type type);
RETRO_END_DECLS
#endif

/* version.h replacement */
#ifndef RARCH_VERSION_H__
#define RARCH_VERSION_H__
#define PACKAGE_VERSION "test-stub"
#endif

/* file_path_special.h replacement */
#ifndef _FILE_PATH_SPECIAL_H
#define _FILE_PATH_SPECIAL_H
#define FILE_PATH_PNG_EXTENSION ".png"
#endif

/* features_cpu.h replacement */
#ifndef _LIBRETRO_SDK_CPU_INFO_H
#define _LIBRETRO_SDK_CPU_INFO_H
RETRO_BEGIN_DECLS
retro_time_t cpu_features_get_time_usec(void);
RETRO_END_DECLS
#endif

/* presence.h replacement */
#ifndef __RARCH_PRESENCE_H
#define __RARCH_PRESENCE_H
enum presence
{
   PRESENCE_NONE = 0,
   PRESENCE_MENU,
   PRESENCE_GAME,
   PRESENCE_RETROACHIEVEMENTS,
   PRESENCE_DISCORD
};
RETRO_BEGIN_DECLS
void presence_update(enum presence p);
RETRO_END_DECLS
#endif

#endif /* TEST_CHEEVOS_CLIENT_STUBS_H */
