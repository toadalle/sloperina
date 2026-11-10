/* Practice menu actions for saving and restoring Link's position. */
#ifndef POSITION_H
#define POSITION_H

#include <stdbool.h>

void position_save(void);
void position_load(void);
/* frame hook, after levitate_frame: the shortcuts, and a load in progress */
void position_frame(bool menu_open);

#endif
