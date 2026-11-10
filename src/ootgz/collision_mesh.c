/* A simplified edge mesh of the static player collision, indexed in 8m cells. */
#include "collision_mesh.h"

#include "sys.h"

#define MAX_CELLS 8192
#define EDGES_PER_CELL 96
#define CELL_SIZE 8

typedef struct {
  int16_t x, z;
  uint32_t first, count;
} mesh_cell_t;

_Static_assert(sizeof(mesh_cell_t) == 12 && sizeof(mesh_edge_t) == 14, "SGM1 layout");

bool collision_mesh_shown;
static char loaded_map[64];
static const mesh_cell_t *cells;
static const mesh_edge_t *edges;
static mesh_edge_t nearby[9 * EDGES_PER_CELL];
static unsigned cell_count, nearby_count;
static int last_x, last_z;
static bool have_cell;

#define ASSET(name) extern const uint8_t mesh_##name[], mesh_##name##_end[];
ASSET(DodongoCavern)
ASSET(ForestTemple)
ASSET(InsideTheDekuTree)
ASSET(KakarikoVillage_Adult)
ASSET(KakarikoVillage_Child)
ASSET(KokiriForest_Adult)
ASSET(KokiriForest_Child)
#undef ASSET

static const struct {
  const char *name;
  const uint8_t *start, *end;
} assets[] = {
#define ASSET(name) { #name, mesh_##name, mesh_##name##_end },
  ASSET(DodongoCavern)
  ASSET(ForestTemple)
  ASSET(InsideTheDekuTree)
  ASSET(KakarikoVillage_Adult)
  ASSET(KakarikoVillage_Child)
  ASSET(KokiriForest_Adult)
  ASSET(KokiriForest_Child)
#undef ASSET
};

static bool same_map(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static int compare_cell(const mesh_cell_t *cell, int x, int z)
{
  return cell->x != x ? cell->x - x : cell->z - z;
}

static void load_map(const char *map)
{
  cell_count = nearby_count = 0;
  have_cell = false;
  snprintf(loaded_map, sizeof(loaded_map), "%s", map);
  for (unsigned i = 0; i < sizeof(assets) / sizeof(*assets); i++) {
    if (!same_map(map, assets[i].name))
      continue;
    const uint32_t *header = (const uint32_t *)assets[i].start;
    if (header[0] != 0x314d4753 || !header[1] || header[1] > MAX_CELLS)
      return;
    cells = (const mesh_cell_t *)(header + 2);
    edges = (const mesh_edge_t *)(cells + header[1]);
    if ((uintptr_t)edges > (uintptr_t)assets[i].end)
      return;
    unsigned next = 0;
    for (unsigned j = 0; j < header[1]; j++) {
      const mesh_cell_t *cell = &cells[j];
      if (cell->first != next || !cell->count || cell->count > EDGES_PER_CELL ||
          (j && compare_cell(&cells[j - 1], cell->x, cell->z) >= 0))
        return;
      next += cell->count;
    }
    if ((uintptr_t)assets[i].end - (uintptr_t)edges == next * sizeof(*edges))
      cell_count = header[1];
    return;
  }
}

static const mesh_cell_t *find_cell(int x, int z)
{
  unsigned lo = 0, hi = cell_count;
  while (lo < hi) {
    unsigned mid = lo + (hi - lo) / 2;
    int order = compare_cell(&cells[mid], x, z);
    if (!order)
      return &cells[mid];
    if (order < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return 0;
}

static int cell_coordinate(float value)
{
  float scaled = value / CELL_SIZE;
  int n = (int)scaled;
  return n - (scaled < n);
}

static void load_nearby(int x, int z)
{
  static const int offsets[9][2] = {
    {0, 0}, {-1, 0}, {0, -1}, {1, 0}, {0, 1}, {-1, -1}, {-1, 1}, {1, -1}, {1, 1},
  };
  last_x = x;
  last_z = z;
  have_cell = true;
  nearby_count = 0;
  for (unsigned i = 0; i < 9; i++) {
    const mesh_cell_t *cell = find_cell(x + offsets[i][0], z + offsets[i][1]);
    if (cell) {
      memcpy(nearby + nearby_count, edges + cell->first, cell->count * sizeof(*edges));
      nearby_count += cell->count;
    }
  }
}

const mesh_edge_t *collision_mesh_near(const char *map, vec3f_t position, unsigned *count)
{
  *count = 0;
  if (!map || !*map || !(position.x >= -2048 && position.x < 2048 &&
                        position.z >= -2048 && position.z < 2048))
    return nearby;
  if (!same_map(map, loaded_map))
    load_map(map);
  if (cell_count) {
    int x = cell_coordinate(position.x), z = cell_coordinate(position.z);
    if (!have_cell || x != last_x || z != last_z)
      load_nearby(x, z);
    *count = nearby_count;
  }
  return nearby;
}
