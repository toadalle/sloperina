/* Reversible ownership of the game's GamePause PauseManager request count. */
#ifndef PAUSE_H
#define PAUSE_H
#include <stdbool.h>
bool pause_menu_acquire(void);
void pause_menu_release(void);
bool pause_menu_owned(void);
/* Read-only: true only when a known native loading/transition pause type has an active request. */
bool pause_transition_active(void);
#endif
