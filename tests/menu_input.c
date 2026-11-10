/* Exercise the game's HID filter with synthetic states, including sample history. */
#include <assert.h>
#include <stdio.h>

#include "../src/ootgz/input.c"

static npad_state_t raw;

void hid_detail_fullkey(int *n, void *out, int count, const uint32_t *id)
{
  *n = 1;
  *(npad_state_t *)out = raw;
}

void hid_detail_handheld(int *n, void *out, int count, const uint32_t *id) { *n = 0; }
void hid_detail_joydual(int *n, void *out, int count, const uint32_t *id) { *n = 0; }

#define STUB_PUBLIC(n) int hid_public_##n(void *out, int count, const uint32_t *id) { return 0; }
STUB_PUBLIC(0)
STUB_PUBLIC(1)
STUB_PUBLIC(2)
STUB_PUBLIC(3)
STUB_PUBLIC(4)

const char *got_hook(uintptr_t module, const char *name, void *replacement) { return 0; }

int main(void)
{
  raw = (npad_state_t){ .sampling_number = 10, .buttons = PRACTICE_MENU_COMBO, .attributes = NPAD_ATTR_CONNECTED };
  input_update();
  npad_state_t sample = raw;
  filter_game_states(&sample, 1, 0, NPAD_NO1);
  assert(!sample.buttons && input_shortcut(PRACTICE_MENU_COMBO));
  raw.buttons = BTN_A;
  input_update();
  assert(!input_shortcut(PRACTICE_MENU_COMBO));
  raw.buttons = PRACTICE_MENU_COMBO | BTN_ZR;
  input_update();
  assert(input_shortcut(PRACTICE_MENU_COMBO)); /* D-pad completes a held A */
  sample = raw;
  filter_game_states(&sample, 1, 0, NPAD_NO1);
  assert(sample.buttons == BTN_ZR);
  input_set_native_ui(true);
  input_hide_from_game = BTN_A | BTN_B | BTN_X | BTN_Y;
  sample = raw;
  filter_game_states(&sample, 1, 0, NPAD_NO1);
  assert(sample.buttons == (PRACTICE_MENU_COMBO | BTN_ZR)); /* preserve the ocarina note */
  sample.buttons = BTN_A | BTN_B;
  filter_game_states(&sample, 1, 0, NPAD_NO1);
  assert(sample.buttons == (BTN_A | BTN_B)); /* preserve dialogue controls */
  input_set_native_ui(false);
  input_hide_from_game = 0;
  raw.sampling_number++;
  raw.buttons = BTN_DLEFT;
  input_update();
  assert(!input_shortcut(MINI_HUD_COMBO));
  raw.buttons = MINI_HUD_COMBO;
  input_update();
  assert(input_shortcut(MINI_HUD_COMBO));
  sample = raw;
  filter_game_states(&sample, 1, 0, NPAD_NO1);
  assert(!sample.buttons);

  input_set_menu_block(true);
  raw.sampling_number++;
  raw.buttons = BTN_B | BTN_LS_RIGHT | BTN_RS_UP;
  raw.lx = 24000;
  raw.ry = 18000;
  input_update();
  sample = raw;
  filter_game_states(&sample, 1, 0, NPAD_NO1);
  assert(!sample.buttons && !sample.lx && !sample.ry);
  assert(input_held() & BTN_B); /* the menu still receives raw controls */

  input_set_menu_block(false);
  npad_state_t history[2] = { raw, raw };
  history[0].sampling_number++;
  filter_game_states(history, 2, 0, NPAD_NO1);
  for (unsigned i = 0; i < 2; i++)
    assert(!history[i].buttons && !history[i].lx && !history[i].ry);
  assert(input_menu_releasing());

  raw.sampling_number += 2;
  raw.buttons = raw.lx = raw.ry = 0;
  input_update();
  assert(!input_menu_releasing());

  history[0] = raw;
  history[0].buttons = BTN_PLUS;
  history[0].lx = -20000;
  history[1].sampling_number = 12;
  history[1].buttons = BTN_B;
  filter_game_states(history, 2, 0, NPAD_NO1);
  assert(history[0].buttons == BTN_PLUS && history[0].lx == -20000);
  assert(!history[1].buttons); /* historical close input still never reaches the game */
  puts("PASS: practice chord, active menu isolation, post-close neutral and HID history");
}
