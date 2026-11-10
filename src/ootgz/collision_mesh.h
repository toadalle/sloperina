#ifndef COLLISION_MESH_H
#define COLLISION_MESH_H

#include "link.h"

typedef struct {
  int16_t a[3], b[3];
  uint16_t kind;
} mesh_edge_t;

extern bool collision_mesh_shown;
bool collision_mesh_available(const char *map);
const mesh_edge_t *collision_mesh_near(const char *map, vec3f_t position, unsigned *count);

#endif
