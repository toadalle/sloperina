/* ootgz: a gz-style practice menu for Ocarina of Time (Switch 2), loaded by the game itself.
 *
 * How it gets control (BUILDING.md has the details):
 *   1. The game's exefs gets this module as subsdk9. rtld loads it with the game's other
 *      modules, links its imports (nnMain from main; nn::hid, vsnprintf... from sdk) and
 *      calls ootgz_module_init (DT_INIT) before the game starts.
 *   2. An IPS patch on main redirects the per-frame GameData unlock (FINDINGS §3) to a stub
 *      in main's code cave. The stub loads a pointer from a slot in main's bss and jumps to
 *      it if it's set, or runs the original unlock otherwise.
 *   3. ootgz_module_init checks the patch is the matching one and stores ootgz_frame_entry
 *      in the slot. From then on the module runs on the game's thread once per frame, right
 *      where the old patch handled the menu's requests, so the game's own functions are
 *      safe to call (give items, redraw screens...).
 */
#include "cheats.h"
#include "gamedata.h"
#include "diagnostics.h"
#include "pause.h"
#include "gfx.h"
#include "input.h"
#include "job.h"
#include "link.h"
#include "menu.h"
#include "menus.h"
#include "airwall.h"
#include "enemyhp.h"
#include "levitate.h"
#include "settings.h"
#include "warp.h"
#include "watches.h"
#include "nvn.h"
#include "got.h"
#include "ootgz_version.h"
#include "pouch.h"
#include "position.h"
#include "sys.h"
#include "z64hd.h"
#include "z64hd_builds.h"

uintptr_t z64hd_main;
const struct z64hd_build *z64hd_game;

/* exported by main */
extern void nnMain(void);

typedef uint64_t (*gd_unlock_t)(void *lock);

static bool started;
static uint32_t frames_run;

/* Frame advance is controlled from the practice menu. */
bool pause_display = true;
static bool fa_paused, fa_step;

void frame_advance_set(bool paused, bool step)
{
  fa_paused = paused;
  fa_step = step;
  gfx_pause_icon = paused && pause_display;
  menu_close();
}

/* Opens, closes and drives the menu. Returns true if the input was the menu's. */
static bool menu_controls(void)
{
  if (!menu_current && input_shortcut(PRACTICE_MENU_COMBO)) {
    if (gd_game_paused(0, 0) || pause_transition_active()) {
      input_suppress_until_release(PRACTICE_MENU_COMBO);
      return true; /* Leave native pause screens and transitions to the game. */
    }
    menu_open(&menu_root);
  }
  else if (menu_current && input_shortcut(PRACTICE_MENU_COMBO))
    menu_close(); /* the open combo toggles it */
  else if (menu_current) {
    watches_sync();
    menu_input();
  } else
    return false;
  return true;
}

static void visual_and_void_shortcuts(void)
{
  if (input_shortcut(OVERLAY_TOGGLE_COMBO)) {
    gfx_overlays_hidden = !gfx_overlays_hidden;
    input_suppress_until_release(OVERLAY_TOGGLE_COMBO);
  }
  if (input_shortcut(VOID_BYPASS_COMBO)) {
    input_suppress_until_release(VOID_BYPASS_COMBO);
    if (void_bypass_available()) {
      void_bypass_set(!void_bypass_enabled());
      gfx_notice(void_bypass_enabled() ? "contact void-out bypass on" : "contact void-out bypass off");
    } else {
      gfx_notice("contact void-out bypass unavailable");
    }
  }
}

static void fa_frame(void)
{
  if (!fa_paused || menu_current)
    return;
  if (fa_step) {
    fa_step = false;
    return;
  }
  while (fa_paused && !menu_current) {
    sys_sleep_ns(16 * 1000 * 1000);
    input_update();
    menu_controls();
    if (!menu_current && !input_menu_releasing()) {
      visual_and_void_shortcuts();
      watches_frame(false);
    }
  }
}

/* --- frame hook -------------------------------------------------------------------------- */

static void first_frame(void)
{
  menus_build();
  settings_load();
  log_printf("running on game %s (patch v%d, main %#lx)", z64hd_game->version,
             OOTGZ_PATCH_VERSION, (unsigned long)z64hd_main);
  menus_splash();
}

static void frame(void)
{
  if (!started) {
    started = true;
    first_frame();
  }
  frames_run++;
  bool native_ui = gd_game_paused(0, 0) || gd_game_pre_pause() || game_message_open();
  input_set_native_ui(native_ui);
  input_update();
  static bool wait_for_ui_release;
  wait_for_ui_release = native_ui || (wait_for_ui_release &&
      (input_held() & (BTN_A | BTN_B | BTN_X | BTN_Y | BTN_LSTICK | BTN_RSTICK)));
  bool transition = pause_transition_active();
  bool flush_log = diagnostics_transition_frame(transition);
  flush_log |= diagnostics_fall_frame();
  if ((transition || native_ui) && menu_current)
    menu_close();

  if (!transition && !wait_for_ui_release)
    menu_controls();
  else if (input_shortcut(PRACTICE_MENU_COMBO))
    log_printf("input: practice chord deferred (%s)", transition ? "transition" :
               native_ui ? "vanilla UI" : "waiting for UI buttons to release");
  if (menu_current) {
    watches_sync();
    menu_draw();
  }
  if (!menu_current && !transition && !wait_for_ui_release)
    visual_and_void_shortcuts();
  airwall_frame(menu_current || transition || gfx_overlays_hidden);
  enemyhp_frame(menu_current || transition || gfx_overlays_hidden);
  menus_research_frame();
  bool menu_owns_input = menu_current || input_menu_releasing();
  if (!transition)
    watches_frame(menu_owns_input || wait_for_ui_release);
  levitate_frame(menu_owns_input, wait_for_ui_release && !transition);
  if (!transition)
    position_frame(menu_owns_input || wait_for_ui_release);
  warp_frame();
  diagnostics_trace_frame(transition, native_ui, menu_current != 0);
  gfx_frame();

  if (!transition) {
    cheats_frame();
    pouch_frame();
    job_frame();
    settings_frame(menu_current != 0);
  }
  settings_log_frame(flush_log);
  if (!transition && !wait_for_ui_release)
    fa_frame(); /* last: the frame's work is done before the game is held */
}

/* Called by the IPS stub instead of the GameData unlock (x0 = the lock), on the game's thread.
 * Unlock first, exactly as the site did, then do our work. */
__attribute__((used)) uint64_t ootgz_frame_entry(void *lock)
{
  uint64_t result = MAIN_FN(gd_unlock_t, z64hd_game->gd_unlock)(lock);
  frame();
  return result;
}

/* --- module init (DT_INIT) --------------------------------------------------------------- */

#define A64_B_OPCODE     0x14000000u /* B <label> */
#define A64_B_IMM26_MASK 0x3FFFFFF   /* word offset, 26 bits */

/* The AArch64 "b" instruction at `from` that jumps to `to`. */
static uint32_t encode_branch(uintptr_t from, uintptr_t to)
{
  return A64_B_OPCODE | (uint32_t)(((intptr_t)(to - from) >> 2) & A64_B_IMM26_MASK);
}

static bool void_patch_present;

bool void_bypass_available(void) { return void_patch_present; }

bool void_bypass_enabled(void)
{
  return void_patch_present &&
         (__atomic_load_n((uintptr_t *)MAIN(z64hd_game->hook_slot), __ATOMIC_RELAXED) & 1);
}

void void_bypass_set(bool enabled)
{
  if (!void_patch_present)
    return;
  uintptr_t *slot = (uintptr_t *)MAIN(z64hd_game->hook_slot);
  uintptr_t target = (uintptr_t)ootgz_frame_entry;
  __atomic_store_n(slot, target | (uintptr_t)enabled, __ATOMIC_RELAXED);
  log_printf("void: contact bypass %s", enabled ? "on" : "off");
}

/* nnMain is resolved by rtld. Check the matching main header before reading any game offsets. */
static const struct z64hd_build *detect_build(uintptr_t nnmain)
{
  for (size_t i = 0; i < sizeof(z64hd_builds) / sizeof(*z64hd_builds); i++) {
    const struct z64hd_build *build = &z64hd_builds[i];
    uintptr_t base = nnmain - build->nnmain;
    uint32_t mod0, magic;
    if (sys_read32(base + 4, &mod0) && mod0 == build->mod0 &&
        sys_read32(base + mod0, &magic) && magic == 0x30444F4D) /* MOD0 */
      return build;
  }
  return 0;
}

/* Runs inside rtld's DT_INIT: no SDK calls, just checked reads and the slot. */
static const char *patch_problem(void)
{
  const uint8_t *cave = (const uint8_t *)MAIN(z64hd_game->cave_start);
  if (!sys_mapped((uintptr_t)cave, 16, SYS_PERM_R))
    return "patch marker is not mapped";
  const char *marker = OOTGZ_MARKER;
  for (int i = 0; i < 8; i++)
    if (cave[i] != (uint8_t)marker[i])
      return "no ootgz patch in main: install the .ips next to subsdk9";
  if (*(const uint32_t *)(cave + 8) != OOTGZ_PATCH_VERSION ||
      *(const uint32_t *)(cave + 12) != z64hd_game->revision)
    return "patch version or game revision mismatch: install the matching .ips";
  uintptr_t site_addr = MAIN(z64hd_game->frame_site);
  uintptr_t stub = MAIN(z64hd_game->cave_start + OOTGZ_STUB_OFF);
  uint32_t site;
  if (!sys_read32(site_addr, &site) || site != encode_branch(site_addr, stub))
    return "frame site not patched (another mod on the same site, e.g. ootmenu?)";
  uintptr_t slot = MAIN(z64hd_game->hook_slot);
  if (!sys_mapped(slot, sizeof(uintptr_t), SYS_PERM_R | SYS_PERM_W) ||
      *(volatile uintptr_t *)slot != 0)
    return "hook slot in main's bss is unavailable";
  return 0;
}

__attribute__((used)) void ootgz_module_init(void)
{
  z64hd_game = detect_build((uintptr_t)&nnMain);
  if (!z64hd_game) {
    log_raw("unsupported Ocarina of Time build");
    return;
  }
  z64hd_main = (uintptr_t)&nnMain - z64hd_game->nnmain;
  const char *problem = patch_problem();
  if (problem) {
    log_raw(problem);
    return;
  }
  *(volatile uintptr_t *)MAIN(z64hd_game->hook_slot) = (uintptr_t)ootgz_frame_entry;
  if (z64hd_game->void_contact_site) {
    uintptr_t site = MAIN(z64hd_game->void_contact_site);
    uint32_t instruction;
    void_patch_present = sys_read32(site, &instruction) &&
      instruction == encode_branch(site, MAIN(z64hd_game->cave_start + OOTGZ_VOID_STUB_OFF));
  }
  log_raw(z64hd_game->version);
  log_raw("frame hook installed");

  /* main's imports: NVN for drawing, nn::hid to hide input while the menu is open */
  const char *error = got_hook(z64hd_main, "nvnBootstrapLoader", nvn_bootstrap_hook);
  log_raw(error ? "NVN hook failed" : "NVN hook installed");
  if (error)
    log_raw(error);
  error = input_hook_game(z64hd_main);
  log_raw(error ? "input hook failed" : "input hook installed");
  if (error)
    log_raw(error);
}
