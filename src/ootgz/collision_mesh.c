/* A simplified edge mesh of the static player collision, indexed in 8m cells. */
#include "collision_mesh.h"

#include "sdfs.h"
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
static char loaded_map[64], path[128];
static mesh_cell_t cells[MAX_CELLS];
static mesh_edge_t nearby[9 * EDGES_PER_CELL];
static unsigned cell_count, nearby_count;
static int last_x, last_z;
static bool have_cell;

static bool read_exact(fs_file_t file, int64_t offset, void *data, size_t size)
{
  size_t got;
  return fs_read(&got, file, offset, data, size) == 0 && got == size;
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
  for (const char *p = map; *p; p++) {
    char c = *p;
    if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
      return;
  }
  snprintf(path, sizeof(path), SD_DIR "/mesh/%s.mesh", loaded_map);
  fs_file_t file;
  if (fs_open(&file, path, FS_READ) != 0)
    return;
  uint32_t header[2];
  int64_t size;
  if (fs_size(&size, file) == 0 && read_exact(file, 0, header, sizeof(header)) &&
      header[0] == 0x314d4753 && header[1] && header[1] <= MAX_CELLS &&
      read_exact(file, sizeof(header), cells, header[1] * sizeof(*cells))) {
    uint32_t next = 0;
    bool valid = true;
    for (unsigned i = 0; i < header[1]; i++) {
      const mesh_cell_t *cell = &cells[i];
      if (cell->first != next || !cell->count || cell->count > EDGES_PER_CELL ||
          (i && compare_cell(&cells[i - 1], cell->x, cell->z) >= 0)) {
        valid = false;
        break;
      }
      next += cell->count;
    }
    if (valid && size == (int64_t)(sizeof(header) + header[1] * sizeof(*cells) + next * sizeof(*nearby)))
      cell_count = header[1];
  }
  fs_close(file);
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
  fs_file_t file;
  if (fs_open(&file, path, FS_READ) != 0)
    return;
  int64_t start = 8 + cell_count * sizeof(*cells);
  for (unsigned i = 0; i < 9; i++) {
    const mesh_cell_t *cell = find_cell(x + offsets[i][0], z + offsets[i][1]);
    if (cell && read_exact(file, start + cell->first * sizeof(*nearby), nearby + nearby_count,
                           cell->count * sizeof(*nearby)))
      nearby_count += cell->count;
  }
  fs_close(file);
}

const mesh_edge_t *collision_mesh_near(const char *map, vec3f_t position, unsigned *count)
{
  *count = 0;
  if (!map || !*map || !(position.x >= -2048 && position.x < 2048 &&
                        position.z >= -2048 && position.z < 2048))
    return nearby;
  unsigned i = 0;
  while (i < sizeof(loaded_map) - 1 && loaded_map[i] && loaded_map[i] == map[i])
    i++;
  if (loaded_map[i] != map[i])
    load_map(map);
  if (cell_count) {
    int x = cell_coordinate(position.x), z = cell_coordinate(position.z);
    if (!have_cell || x != last_x || z != last_z)
      load_nearby(x, z);
    *count = nearby_count;
  }
  return nearby;
}
