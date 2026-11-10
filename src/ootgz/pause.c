/* GamePause ownership for the practice menu.
 *
 * FINDINGS section 9.1 identifies a 0x20-byte PauseManager entry for GamePause.
 * Its +0x1c word is a 31-bit request count plus a high-bit flag. External memory
 * testing showed count > 0 stops world simulation while presentation continues.
 * We own exactly one increment and remove exactly one on close.
 */
#include "pause.h"
#include <stdint.h>
#include "sys.h"
#include "z64hd.h"

#define PAUSE_TYPE_SIZE 0x20
#define PAUSE_TYPE_ID 0x08
#define PAUSE_TYPE_COUNT 0x1C
#define PAUSE_COUNT_MASK 0x7FFFFFFFu
#define PAUSE_FLAG_MASK 0x80000000u
#define PAUSE_TYPES_MAX 64
#define GAME_PAUSE_ID 0xBA597383u
/* MurmurHash3(name), seed 0 (verified against GamePause). These four
 * PauseManager types represent actual loading or scene-transition work,
 * not the practice menu's GamePause request. */
#define LOADING_PAUSE_ID          0x0EAFE200u
#define SCENE_TRANSITION_ID       0xEC94B0F4u
#define EVENT_BANC_CHANGE_ID     0x95DFB2F7u
#define SYSTEM_LOADING_PAUSE_ID  0x956925E5u

static volatile uint32_t *owned_count;
static uintptr_t owned_entry;
static bool unavailable_logged;

static uintptr_t find_game_pause(void)
{
  if (!z64hd_game || !z64hd_game->pause_mgr_ptr)
    return 0;
  uintptr_t manager = sys_ptr(MAIN(z64hd_game->pause_mgr_ptr));
  uint32_t n;
  if (!manager || !sys_read32(manager, &n) || !n || n > PAUSE_TYPES_MAX)
    return 0;
  uintptr_t table = sys_ptr(manager + 8);
  if (!table)
    return 0;
  for (uint32_t i = 0; i < n; i++) {
    uintptr_t entry = table + (uintptr_t)i * PAUSE_TYPE_SIZE;
    uint32_t id;
    if (!sys_read32(entry + PAUSE_TYPE_ID, &id))
      return 0;
    if (id == GAME_PAUSE_ID) {
      if (!sys_mapped(entry + PAUSE_TYPE_COUNT, sizeof(uint32_t), SYS_PERM_R | SYS_PERM_W))
        return 0;
      return entry;
    }
  }
  return 0;
}

static bool adjust_count(volatile uint32_t *word, int delta)
{
  uint32_t old = __atomic_load_n(word, __ATOMIC_ACQUIRE);
  for (;;) {
    uint32_t count = old & PAUSE_COUNT_MASK;
    if ((delta > 0 && count == PAUSE_COUNT_MASK) || (delta < 0 && count == 0))
      return false;
    uint32_t next = (old & PAUSE_FLAG_MASK) | (uint32_t)((int32_t)count + delta);
    if (__atomic_compare_exchange_n(word, &old, next, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return true;
  }
}

bool pause_menu_acquire(void)
{
  if (owned_count)
    return true;
  uintptr_t entry = find_game_pause();
  if (!entry) {
    if (!unavailable_logged) {
      log_printf("menu pause: GamePause unavailable; menu stays live");
      unavailable_logged = true;
    }
    return false;
  }
  volatile uint32_t *count = (volatile uint32_t *)(entry + PAUSE_TYPE_COUNT);
  if (!adjust_count(count, 1)) {
    log_printf("menu pause: couldn't acquire GamePause");
    return false;
  }
  owned_entry = entry;
  owned_count = count;
  log_printf("menu pause: GamePause acquired");
  return true;
}

void pause_menu_release(void)
{
  if (!owned_count)
    return;
  uint32_t id;
  if (!sys_read32(owned_entry + PAUSE_TYPE_ID, &id) || id != GAME_PAUSE_ID ||
      !sys_mapped((uintptr_t)owned_count, sizeof(uint32_t), SYS_PERM_R | SYS_PERM_W)) {
    log_printf("menu pause: GamePause entry changed; refusing stale write");
  } else if (!adjust_count(owned_count, -1)) {
    log_printf("menu pause: GamePause count already zero");
  } else {
    log_printf("menu pause: GamePause released");
  }
  owned_count = 0;
  owned_entry = 0;
}

bool pause_menu_owned(void)
{
  return owned_count != 0;
}

/* Read native pause state, without owning or modifying its counts. Fail closed:
 * an absent/invalid table must not be interpreted as an active transition.
 * The game can raise these only after the exit has begun, so this is a
 * safety handoff rather than a replacement for a transition callback. */
bool pause_transition_active(void)
{
  if (!z64hd_game || !z64hd_game->pause_mgr_ptr)
    return false;
  uintptr_t manager = sys_ptr(MAIN(z64hd_game->pause_mgr_ptr));
  uint32_t n;
  if (!manager || !sys_read32(manager, &n) || !n || n > PAUSE_TYPES_MAX)
    return false;
  uintptr_t table = sys_ptr(manager + 8);
  if (!table || !sys_mapped(table, (size_t)n * PAUSE_TYPE_SIZE, SYS_PERM_R))
    return false;
  for (uint32_t i = 0; i < n; i++) {
    uintptr_t entry = table + (uintptr_t)i * PAUSE_TYPE_SIZE;
    uint32_t id, count;
    if (!sys_read32(entry + PAUSE_TYPE_ID, &id) ||
        !sys_read32(entry + PAUSE_TYPE_COUNT, &count))
      return false;
    if ((id == LOADING_PAUSE_ID || id == SCENE_TRANSITION_ID ||
         id == EVENT_BANC_CHANGE_ID || id == SYSTEM_LOADING_PAUSE_ID) &&
        (count & PAUSE_COUNT_MASK))
      return true;
  }
  return false;
}
