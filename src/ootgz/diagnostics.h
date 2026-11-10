#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H
#include <stdbool.h>
#include "link.h"

void diagnostics_dump(bool deep);
bool diagnostics_transition_frame(bool active); /* true when a breadcrumb should be flushed */
bool diagnostics_fall_frame(void);
void diagnostics_trace_frame(bool transition, bool native_ui, bool menu);
bool diagnostics_last_fall(const char *map, vec3f_t *position);
#endif
