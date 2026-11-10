/* Hover: L3 toggles it, R3 toggles ghost collision while hovering, X/B rise/descend,
 * and release holds altitude. Left stick moves horizontally but is hidden from the
 * game's normal movement state machine while hover owns Link. */
#ifndef LEVITATE_H
#define LEVITATE_H

#include <stdbool.h>

extern bool levitate_enabled;
extern bool levitate_through_walls;
extern int levitate_speed;        /* X/B units/s; legacy 0 uses 4, idle always hovers */
extern int levitate_move_speed;   /* left stick, units per second */

void levitate_frame(bool menu_open);
const char *levitate_state(void); /* for watches/menus */

/* Link's physics record (where his position really lives), found and kept here; save / load position
 * (position.c) moves him through it too. Positions are drawn ones (the record sits 0.585 above). */
typedef enum { REC_NONE, REC_SEARCHING, REC_READY, REC_FAILED } link_record_status_t;
link_record_status_t link_record_status(void);
void link_record_request(void);   /* look for it, if it isn't known (a heap scan over a few frames) */
/* move Link there now; v (x, z) is his velocity, which turns him to face it. The record may then lag
 * behind the drawn position for settle_frames without counting as lost. False if it isn't known. */
bool link_record_move(double x, double y, double z, float face_vx, float face_vz, int settle_frames);
void link_record_resync(void);    /* back to where Link is drawn (a move that didn't take) */

#endif
