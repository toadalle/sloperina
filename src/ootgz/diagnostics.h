#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H
#include <stdbool.h>

void diagnostics_dump(bool deep);
bool diagnostics_transition_frame(bool active); /* true when a breadcrumb should be flushed */
#endif
