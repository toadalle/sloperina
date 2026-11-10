/* Air walls (port of airwall.py): which walls are in the current map, whether Link is inside one,
 * and their outlines projected with the game's camera, published as dots for nvn.c to draw. */
#ifndef AIRWALL_H
#define AIRWALL_H

#include <stdbool.h>
#include <stdint.h>

extern bool airwalls_shown, baked_barriers_shown; /* draw them */
extern bool loading_zones_shown, void_zones_shown;
extern bool airwalls_all;   /* also the kinds that don't stop Link (enemies, sight, objects) */

/* a piece of an outline: a rectangle of the visible picture, 0..65535 each way, colour index. A side
 * thinner than the line width (a dot, or a straight piece of a line) is widened to it when drawn. */
enum { AW_BLOCKS, AW_OTHER, AW_LOADING, AW_VOID, AW_GROTTO_SAFETY, AW_FALL_SAMPLE, AW_BAKED_BARRIER, AW_NCOLORS };
typedef struct { uint16_t x0, y0, x1, y1; uint8_t color; bool fill; } aw_dot_t;

void airwall_frame(bool menu_open);                     /* game thread, once per frame */
const aw_dot_t *airwall_published(int *n);    /* render thread */
void airwall_nearest_barrier(char *buf, int len);
void airwall_nearest_death(char *buf, int len);
void airwall_summary(char *buf, int len);     /* "12 walls, 3 within 60" */
void airwall_link_text(char *buf, int len);   /* the air wall watch: depth inside, or "outside" */

#endif
