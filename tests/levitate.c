/* Real levitation step/frame logic with a simulated physics record; no game process. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/ootgz/levitate.c"

static double physics[9];
_Alignas(FAST_SCAN_STRIDE) static unsigned char scan_heap[512];
static unsigned char actor_memory[1024];
static float cam[24];
static vec3f_t drawn_pos;
static uint32_t held, pressed;
static int32_t stick_x;
static bool mapped = true;
static bool controller_active, controller_available = true;
static int controller_moves, controller_restores;
static int velocity_resets, velocity_stops, distance_resets;
static bool on_ground;
static bool transition_active;
bool pause_transition_active(void) { return transition_active; }
bool pause_menu_owned(void) { return false; }
bool cct_reset_vertical_velocity(uintptr_t a) { (void)a; velocity_resets++; return controller_available; }
bool cct_stop_velocity(uintptr_t a) { (void)a; velocity_stops++; return controller_available; }
bool cct_clear_fall_distance(uintptr_t a) { (void)a; distance_resets++; return controller_available; }
bool cct_on_ground(uintptr_t a, bool *grounded) { (void)a; *grounded = on_ground; return controller_available; }
bool cct_noclip_begin(void) { return controller_active = controller_available; }
bool cct_noclip_active(void) { return controller_active; }
bool cct_noclip_move(const vec3f_t *p) { (void)p; controller_moves++; return controller_available; }
bool cct_noclip_end(void) { if (controller_active) controller_restores++; controller_active = false; return true; }
bool cheats[CHEAT_COUNT];
uint32_t input_hide_from_game;
uint32_t input_hide_sticks_from_game;

uintptr_t link_actor(void) { return (uintptr_t)actor_memory; }
uintptr_t camera(void) { return (uintptr_t)cam; }
bool link_drawn_pos(vec3f_t *p) { *p = drawn_pos; return true; }
bool link_health(int32_t **cur, int32_t **max) { (void)cur; (void)max; return false; }
bool link_facing(double *fx, double *fz) { *fx = 0; *fz = 1; return true; }
bool gd_game_paused(bool *is_pause, int32_t *requests)
{
  if (is_pause) *is_pause = false;
  if (requests) *requests = 0;
  return false;
}
bool sys_mapped(uintptr_t a, size_t n, uint32_t perm)
{
  (void)perm;
  return mapped && ((a >= (uintptr_t)physics && a + n <= (uintptr_t)(physics + 9)) ||
                    (a >= (uintptr_t)cam && a + n <= (uintptr_t)(cam + 24)) ||
                    (a >= (uintptr_t)scan_heap && a + n <= (uintptr_t)(scan_heap + sizeof(scan_heap))));
}
uint32_t sys_query_memory(sys_meminfo_t *m, uint64_t a) { (void)m; (void)a; return 1; }
void sys_regions(uint32_t perm, void (*fn)(uintptr_t, size_t, void *), void *ctx)
{ (void)perm; (void)fn; (void)ctx; }
uint64_t sys_tick(void) { return 100; }
uint64_t sys_tick_freq(void) { return 1000; }
uint32_t input_held(void) { return held; }
uint32_t input_shortcut_held(void)
{
  if ((held & (BTN_DLEFT | BTN_B)) == MINI_HUD_COMBO)
    return MINI_HUD_COMBO;
  uint32_t action = held & PRACTICE_RIGHT_ACTIONS;
  return (held & BTN_DRIGHT) && action && !(action & (action - 1)) ? BTN_DRIGHT | action : 0;
}
bool input_shortcut(uint32_t combo) { return input_shortcut_held() == combo && (pressed & combo); }
uint32_t input_pressed(void) { return pressed; }
void input_sticks(int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry)
{ *lx = stick_x; *ly = *rx = *ry = 0; }
void log_printf(const char *fmt, ...) { (void)fmt; }
void gfx_notice(const char *s) { (void)s; }
void input_suppress_until_release(uint32_t mask) { (void)mask; }

static void sync_drawn(void)
{
  drawn_pos = (vec3f_t){ (float)physics[0], (float)(physics[1] - PHYS_Y), (float)physics[2] };
}

static void start(bool walls)
{
  memset(physics, 0, sizeof(physics));
  memset(cam, 0, sizeof(cam));
  physics[1] = 100;
  cam[0x44 / 4 + 2] = -1; /* camera forward -z, right +x */
  mapped = levitate_enabled = floating = true;
  controller_active = false;
  controller_available = true;
  controller_moves = controller_restores = 0;
  velocity_resets = velocity_stops = distance_resets = 0;
  input_hide_from_game = input_hide_sticks_from_game = 0;
  on_ground = false;
  transition_active = false;
  levitate_through_walls = walls;
  levitate_ghost_default = false;
  levitate_speed = 4;
  levitate_move_speed = 6;
  held = 0;
  pressed = 0;
  stick_x = 0;
  record = (uintptr_t)physics;
  record_actor = link_actor();
  record_has_motion = false;
  search_state = SEARCH_READY;
  sync_drawn();
  float_start();
}

static void step(void)
{
  sync_drawn();
  float_step(1.0 / 30.0, false);
}

static void test_cold_scan(void)
{
  const uintptr_t base = (uintptr_t)scan_heap;
  const double position[3] = { 10, 100 + PHYS_Y, 30 };
  mapped = true;
  drawn_pos = (vec3f_t){ 10, 100, 30 };
  n_heap_regions = 1;
  heap_regions[0].lo = base;
  heap_regions[0].hi = base + sizeof(scan_heap);

  memset(scan_heap, 0, sizeof(scan_heap));
  memcpy(scan_heap + FAST_SCAN_STRIDE, position, sizeof(position));
  start_full_scan();
  fast_scan = true;
  assert(scan_step() && candidate.addr == base + FAST_SCAN_STRIDE);

  memset(scan_heap, 0, sizeof(scan_heap));
  memcpy(scan_heap + sizeof(double), position, sizeof(position));
  start_full_scan();
  fast_scan = true;
  assert(!scan_step());
  start_full_scan();
  assert(scan_step() && candidate.addr == base + sizeof(double));
  n_heap_regions = 0;
  puts("PASS: cold search finds aligned records and falls back for other addresses");
}

static void test_release(void)
{
  for (int disable = 0; disable <= 1; disable++) {
    start(true);
    record_has_motion = true;
    float motion[12];
    for (int i = 0; i < 12; i++) motion[i] = (float)(i + 1);
    motion[(0x2C - 24) / 4] = motion[(0x44 - 24) / 4] = -80;
    memcpy((char *)physics + 24, motion, sizeof(motion));
    unsigned char expected[sizeof(physics)];
    memcpy(expected, physics, sizeof(expected));
    memset(expected + 0x2C, 0, 4);
    memset(expected + 0x44, 0, 4);
    if (disable) levitate_enabled = false;
    else pressed = BTN_LSTICK;
    levitate_frame(false, false);
    assert(!floating && !moving_link);
    assert(velocity_resets == 1 && landing_actor == link_actor());
    assert(!memcmp(physics, expected, sizeof(expected))); /* no position or horizontal writes */
    pressed = 0;
    motion[(0x2C - 24) / 4] = motion[(0x44 - 24) / 4] = -1;
    memcpy((char *)physics + 24, motion, sizeof(motion));
    physics[1] -= 0.1; /* next game frame applies gravity */
    sync_drawn();
    memcpy(expected, physics, sizeof(expected));
    levitate_frame(false, false);
    assert(!memcmp(physics, expected, sizeof(expected))); /* no continued velocity clamp */
    assert(velocity_resets == 1);
    for (int i = 0; i < 300; i++) levitate_frame(false, false);
    assert(landing_actor == link_actor()); /* Long drops do not outlast a healing timer. */
    on_ground = true;
    for (int i = 0; i < 6; i++) levitate_frame(false, false);
    assert(!landing_actor && velocity_resets == 1);
    int saved_resets = distance_resets;
    levitate_frame(false, false);
    assert(distance_resets == saved_resets); /* Later ordinary falls are unchanged. */
  }
  for (int invalid = 0; invalid < 4; invalid++) {
    start(true);
    record_has_motion = true;
    if (invalid == 0) mapped = false;
    if (invalid == 1) drawn_pos.x += 100; /* stale record after relocation */
    if (invalid == 2) record_has_motion = false;
    if (invalid == 3) { /* implausible motion */
      uint32_t nan = 0x7FC00000;
      memcpy((char *)physics + 24, &nan, sizeof(nan));
    }
    unsigned char before[sizeof(physics)];
    memcpy(before, physics, sizeof(before));
    pressed = BTN_LSTICK;
    levitate_frame(false, false);
    assert(!floating && !moving_link && !memcmp(before, physics, sizeof(before)));
  }
  puts("PASS: L3 controller reset once, long-drop landing protection, no later clamp; invalid records untouched");
}

static void test_idle_and_respawn(void)
{
  start(true);
  float_clock = 1;
  for (int i = 0; i < 30; i++)
    step();
  held = BTN_X;
  levitate_move_speed = 20;
  for (int i = 0; i < 15; i++)
    step();
  assert(vel_y > 13 && physics[1] > 102);

  physics[1] -= 100; /* same actor, record and draw both moved by respawn */
  sync_drawn();
  levitate_frame(false, false);
  assert(!floating && !controller_active && !record && search_state == SEARCH_IDLE);
  assert(physics[1] < 10); /* old ghost target did not overwrite the spawn */
  puts("PASS: ghost resumes after idle, vertical speed scales, respawn releases ownership");
}

static void test_dialogue_handoff(void)
{
  start(true);
  controller_active = true;
  held = BTN_A | BTN_B;
  pressed = BTN_A;
  double height = physics[1];
  levitate_frame(false, true);
  assert(floating && levitate_through_walls && !moving_link && !controller_active);
  assert(!input_hide_from_game && !input_hide_sticks_from_game);
  assert(physics[1] == height && levitate_move_speed == 6);
  held = pressed = 0;
  levitate_frame(false, false);
  assert(floating && moving_link && controller_active);
  puts("PASS: dialogue owns controls, restores collision temporarily, and resumes hover");
}

static void test_reset_speed(void)
{
  for (int reset = 0; reset < 2; reset++) {
    start(false);
    drop();
    levitate_reset_speed = reset != 0;
    levitate_move_speed = 20;
    pressed = BTN_LSTICK;
    levitate_frame(false, false);
    assert(floating && levitate_move_speed == (reset ? 6 : 20));
    levitate_move_speed = 30;
    levitate_frame(false, false); /* second L3 edge */
    assert(!floating && levitate_move_speed == (reset ? 6 : 30));
  }
  puts("PASS: each hover resets speed when enabled and retains it when disabled");
}

static void test_vertical_controls(void)
{
  for (int speed = 0; speed <= 4; speed += 4) {
    start(true);
    levitate_speed = speed;
    held = BTN_X;
    step();
    assert(vel_y > 0 && physics[1] > 100);
    held = 0;
    double height = physics[1];
    step();
    assert(vel_y == 0 && physics[1] == height);
    held = BTN_B;
    stick_x = 32767;
    for (int i = 0; i < 30; i++) {
      physics[1] = 101; /* floor keeps trying to push Link up */
      physics[0] -= 0.2; /* simultaneous wall push-back */
      step();
    }
    assert(physics[1] < 97 && physics[0] > 5 && vel_y < 0);
    held = 0;
    height = physics[1];
    step();
    assert(physics[1] == height && vel_y == 0);
    physics[1] += 0.5; /* floor support resumes after releasing B */
    step();
    assert(physics[1] == height + 0.5);

    held = BTN_X;
    step();
    assert(vel_y > 0);
    held = BTN_B;
    step();
    assert(vel_y < 0); /* no remaining upward momentum on reversal */
    held = BTN_X;
    step();
    assert(vel_y > 0);
    height = physics[1];
    held = BTN_X | BTN_B;
    step();
    assert(vel_y == 0 && physics[1] == height);
    held = BTN_B | BTN_RSTICK;
    step();
    assert(vel_y < 0 && physics[1] < height); /* R3 no longer overrides vertical movement */
    height = physics[1];
    held = BTN_B;
    float_step(1.0 / 30.0, true);
    assert(vel_y == 0 && physics[1] == height);
  }
  start(false);
  held = BTN_B;
  physics[1] = 101;
  step();
  assert(physics[1] > 100.9); /* collision mode retains the floor correction */

  start(true);
  floating = moving_link = false;
  held = BTN_X;
  pressed = BTN_LSTICK;
  levitate_frame(false, false);
  assert(floating && input_hide_from_game == (BTN_LSTICK | BTN_RSTICK | BTN_X | BTN_B | BTN_Y | BTN_A));
  assert(input_hide_sticks_from_game == INPUT_HIDE_LEFT_STICK);
  assert((input_hide_from_game & (BTN_Y | BTN_A)) == (BTN_Y | BTN_A));
  levitate_frame(false, false); /* second L3 press turns it off in the same frame */
  assert(!floating && input_hide_from_game == BTN_LSTICK && input_hide_sticks_from_game == 0);
  levitate_enabled = false;
  levitate_frame(false, false);
  assert(input_hide_from_game == 0 && input_hide_sticks_from_game == 0);
  start(true);
  mapped = false;
  levitate_frame(false, false);
  assert(!floating && input_hide_from_game == BTN_LSTICK);
  puts("PASS: X/B ascent/descent, hover, reversal, both/hold/menu, floor bypass and button masking");
}

static void test_walk_wall_shortcut(void)
{
  start(false);
  floating = moving_link = false;
  cheats[CHEAT_WALLS] = false;
  held = HOVER_ACTION_COMBO;
  pressed = BTN_RSTICK;
  levitate_frame(false, false);
  assert(cheats[CHEAT_WALLS] && !floating && !levitate_through_walls);
  held = BTN_DRIGHT;
  pressed = 0;
  levitate_frame(false, false);
  assert(cheats[CHEAT_WALLS]);
  held = HOVER_ACTION_COMBO;
  pressed = BTN_RSTICK;
  levitate_frame(false, false);
  assert(!cheats[CHEAT_WALLS]);
  start(false);
  cheats[CHEAT_WALLS] = false;
  held = HOVER_ACTION_COMBO;
  pressed = BTN_RSTICK;
  levitate_frame(true, false);
  assert(!cheats[CHEAT_WALLS]); /* practice menu doesn't toggle */
  start(false);
  cheats[CHEAT_WALLS] = false;
  held = HOVER_ACTION_COMBO;
  pressed = BTN_RSTICK;
  levitate_frame(false, false);
  assert(floating && levitate_move_speed == 6 && !cheats[CHEAT_WALLS]);
  puts("PASS: walking D-right+R3 toggles existing wall pass, hover resets speed, menu guard");
}

static void test_transition_handoff(void)
{
  start(true);
  held = pressed = 0;
  levitate_frame(false, false);
  assert(controller_active && floating && moving_link);
  transition_active = true;
  int before = controller_moves;
  levitate_frame(false, false);
  assert(!floating && !moving_link && !controller_active);
  assert(controller_restores == 1);
  assert(controller_moves == before); /* no movement through loading state */
  held = pressed = BTN_LSTICK;
  levitate_frame(false, false);
  assert(!floating); /* L3 cannot reactivate while transitioning */
  transition_active = false;
  held = pressed = BTN_LSTICK;
  levitate_frame(false, false);
  assert(floating); /* re-entry is possible after loading */
  puts("PASS: scene transition releases ghost and suppresses hover position writes/re-entry");
}

static void test_flight_speed_buttons(void)
{
  start(false);
  levitate_move_speed = 6;
  held = pressed = BTN_A;
  levitate_frame(false, false);
  assert(levitate_move_speed == 8);
  assert((input_hide_from_game & (BTN_A | BTN_Y)) == (BTN_A | BTN_Y));
  pressed = 0;
  levitate_frame(false, false);
  assert(levitate_move_speed == 8); /* no repeat while held */
  held = pressed = BTN_Y;
  levitate_frame(false, false);
  assert(levitate_move_speed == 6);
  levitate_move_speed = 2;
  levitate_frame(false, false);
  assert(levitate_move_speed == 2); /* minimum */
  levitate_move_speed = 30;
  held = pressed = BTN_A;
  levitate_frame(false, false);
  assert(levitate_move_speed == 30); /* maximum */
  held = pressed = BTN_A | BTN_Y;
  levitate_frame(false, false);
  assert(levitate_move_speed == 30); /* simultaneous presses cancel */
  levitate_through_walls = true;
  held = HOVER_ACTION_COMBO;
  pressed = BTN_RSTICK;
  levitate_frame(false, false);
  assert(levitate_move_speed == 6 && levitate_through_walls); /* D-right+R3 reset, no ghost toggle */
  held = pressed = BTN_RSTICK;
  levitate_frame(false, false);
  assert(!levitate_through_walls && levitate_move_speed == 6); /* plain R3 toggles ghost */
  levitate_move_speed = 30;
  levitate_frame(true, false);
  assert(levitate_move_speed == 30); /* menu A/Y does not change flight speed */
  puts("PASS: Y/A bounded flight speed, D-right+R3 default reset/ghost isolation, masking and menu guard");
}

static void test_ghost_toggle(void)
{
  start(false);
  floating = moving_link = false;
  held = BTN_DRIGHT | BTN_LSTICK;
  pressed = BTN_LSTICK;
  levitate_frame(false, false);
  assert(!floating); /* frame-advance chord must not also toggle hover */

  start(false);
  floating = moving_link = false;
  held = BTN_LSTICK;
  pressed = BTN_LSTICK;
  levitate_frame(false, false);
  assert(floating && moving_link && !levitate_through_walls && !controller_active);
  assert(input_hide_sticks_from_game == INPUT_HIDE_LEFT_STICK);

  held = BTN_RSTICK;
  pressed = BTN_RSTICK;
  levitate_frame(false, false);
  assert(floating && levitate_through_walls && controller_active);
  assert(controller_moves == 1 && velocity_stops == 1);

  held = 0;
  pressed = 0;
  levitate_frame(false, false);
  assert(controller_moves == 2 && velocity_stops == 2);

  held = BTN_RSTICK;
  pressed = BTN_RSTICK;
  levitate_frame(false, false);
  assert(floating && !levitate_through_walls && !controller_active && controller_restores == 1);
  assert(input_hide_sticks_from_game == INPUT_HIDE_LEFT_STICK);
  drop();
  levitate_ghost_default = true;
  held = pressed = BTN_LSTICK;
  levitate_frame(false, false);
  assert(floating && moving_link && levitate_through_walls && controller_active);
  held = pressed = BTN_RSTICK;
  levitate_frame(false, false);
  assert(!levitate_through_walls && levitate_ghost_default);
  held = pressed = BTN_LSTICK;
  levitate_frame(false, false);
  assert(!floating && levitate_ghost_default);
  levitate_frame(false, false);
  assert(floating && levitate_through_walls && controller_active);
  puts("PASS: L3 hover guard, R3 ghost toggle, full velocity stop and left-stick gameplay masking");
}

static void test_controller_path(void)
{
  start(true);
  held = BTN_X;
  levitate_frame(false, false);
  assert(controller_active && controller_moves == 1 && velocity_stops == 1);
  double height = physics[1];
  physics[1] += 0.5; /* Ignore leftover collision corrections in actual noclip. */
  held = 0;
  levitate_frame(false, false);
  assert(physics[1] == height);
  pressed = BTN_LSTICK;
  levitate_frame(false, false);
  assert(!controller_active && controller_restores == 1 && !floating);

  start(true);
  levitate_frame(false, false);
  levitate_through_walls = false;
  levitate_frame(false, false);
  assert(!controller_active && controller_restores == 1);
  start(true);
  controller_available = false;
  levitate_frame(false, false);
  assert(!floating && !controller_active && controller_moves == 0 && physics[1] == 100);
  start(true);
  levitate_frame(false, false);
  mapped = false;
  levitate_frame(false, false);
  assert(!floating && !controller_active && controller_restores == 1);
  puts("PASS: controller move/restore, mode change, invalid controller and record loss");
}

int main(void)
{
  start(true);
  levitate_speed = 0;
  held = 0; /* no R3: zero rise must hover by itself */
  for (int i = 0; i < 60; i++) {
    physics[1] -= 0.1; /* simulated gravity */
    step();
    assert(physics[1] == 100 && physics[0] == 0 && physics[2] == 0 && vel_y == 0);
  }
  stick_x = 32767;
  for (int i = 0; i < 30; i++) {
    physics[0] -= 0.2; /* wall push-back still bypassed at zero rise */
    physics[1] -= 0.1;
    step();
  }
  assert(physics[0] > 5 && physics[1] == 100);
  start(true);
  held = BTN_X;
  step();
  assert(vel_y > 0);
  double rising_height = physics[1];
  levitate_speed = 0;
  held = 0;
  step();
  assert(vel_y == 0 && physics[1] == rising_height);
  levitate_speed = 4;
  held = BTN_X;
  step();
  assert(vel_y > 0 && physics[1] > rising_height);
  puts("PASS: hover with zero legacy speed, horizontal push-back bypass, stop and resume ascent");

  for (int walls = 0; walls <= 1; walls++) {
    start(walls);
    physics[1] += 0.4; /* game lifts Link out of a floor */
    step();
    assert(fabs(physics[1] - 100.4) < 1e-9);
    physics[1] -= 0.6; /* gravity between mod frames must not sink the hover */
    step();
    assert(fabs(physics[1] - 100.4) < 1e-9);
    for (int i = 0; i < 30; i++) {
      physics[1] += 0.05; /* rising platform / repeated upward floor corrections */
      step();
    }
    assert(fabs(physics[1] - 101.9) < 1e-9);
  }

  start(true);
  stick_x = 32767;
  for (int i = 0; i < 30; i++) {
    physics[0] -= 0.2; /* horizontal wall pushes back */
    physics[1] = i % 2 ? pos_y - 0.1 : pos_y + 0.05;
    step();
  }
  assert(physics[0] > 5 && fabs(physics[1] - 100.75) < 1e-9);

  start(false);
  stick_x = 32767;
  float_clock = 1;
  next_heading_sample = 2;
  vel_x = heading_x = past_headings[0][0] = past_headings[1][0] = 6;
  physics[0] = -0.2;
  physics[1] = 100.4;
  step();
  assert(fabs(physics[0] + 0.2) < 1e-6 && fabs(physics[1] - 100.4) < 1e-9);

  start(true);
  held = BTN_X; /* rise still works above corrected floor */
  physics[1] = 100.4;
  step();
  assert(physics[1] > 100.4);
  double height = physics[1];
  float_step(1.0 / 30.0, true);
  assert(physics[1] == height); /* menu stops intentional movement */
  drop();
  physics[1] -= 0.5;
  sync_drawn();
  levitate_frame(false, false);
  assert(physics[1] == height - 0.5); /* stopping levitation restores normal falling */

  start(true);
  mapped = false;
  levitate_frame(false, false);
  assert(search_state == SEARCH_IDLE && !floating && !moving_link && !record && physics[1] == 100);
  start(true); /* new record/scene starts from its own height, not an old floor */
  physics[1] = 20;
  sync_drawn();
  float_start();
  step();
  assert(physics[1] == 20);
  puts("PASS: levitation keeps upward floor corrections, rejects downward drift, passes walls only when enabled");
  puts("PASS: rising platforms, rise/hold/menu/drop, invalid record and fresh-scene height");
  test_release();
  test_vertical_controls();
  test_ghost_toggle();
  test_flight_speed_buttons();
  test_idle_and_respawn();
  test_reset_speed();
  test_dialogue_handoff();
  test_transition_handoff();
  test_walk_wall_shortcut();
  test_controller_path();
  test_cold_scan();
  return 0;
}
