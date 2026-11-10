#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H
#include <stdbool.h>
#include "link.h"

void diagnostics_dump(bool deep);
bool diagnostics_transition_frame(bool active); /* true when a breadcrumb should be flushed */
bool diagnostics_fall_frame(void);
bool diagnostics_last_fall(const char *map, vec3f_t *position);
#endif
