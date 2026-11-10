/* Air walls: port of airwall.py (FINDINGS §5.11), drawn as dotted outlines.
 *
 * Walls come from data.c (airwalls.json, built from the map layouts by tools/airwalls.py). A wall's
 * world point for a local shape point p is t + R (s * p), with R = Rz Ry Rx from its rotation, as
 * sead's makeR. Each frame the walls within AW_RANGE of the camera are projected with the game's
 * camera (+0x38 position, +0x44 look-at, +0x130 vertical fov, +0x140 aspect) and their edges turned
 * into rectangles, nearest wall first, which nvn.c draws by clearing them (no shaders). Edges within
 * about 19 degrees of level or upright are solid lines of a few long rectangles (a staircase); steeper
 * ones are dots, which takes fewer clears than a staircase there. (FINDINGS §5.11 had see-through
 * faces; here only the outlines, and like the Python view they show through scenery.) */
#include "airwall.h"

#include "collision_mesh.h"
#include "data.h"
#include "diagnostics.h"
#include "gamedata.h"
#include "link.h"
#include "lang.h"
#include "sys.h"
#include "view.h"

float sinf(float x); /* from sdk */
float cosf(float x);
float sqrtf(float x);

bool airwalls_shown, airwalls_all;
bool loading_zones_shown, void_zones_shown;

#define AW_RANGE 60.0f
#define DEATH_RANGE 120.0f
#define MAX_WALLS 512
#define MAX_POINTS 8192
#define MAX_EDGES 16384
#define MAX_DOTS 720
#define MAX_VISIBLE_VOLUMES 20
#define MAX_RECTS_PER_EDGE 18
#define REF_WIDTH 1920    /* the picture is worked out in pixels of a 1920x1080 one */
#define REF_HEIGHT 1080
#define DOT_SPACING 12.0f  /* reference pixels between dots */
#define LINE_WIDTH 2.0f   /* reference pixels: a line's thickness, one staircase step at most */
#define STRAIGHT 0.344f   /* tan 19 degrees: up to this slope a staircase takes fewer clears */
#define NEAR_PLANE 0.05f
#define PHYS_Y 0.585f     /* Link's physics position is this far above his drawn one */
#define FACE_UPRIGHT 0.7f /* a face normal with more height than this points up (or down) */

#define SHAPE_BOX 0        /* any other shape type is a cylinder */
#define CYLINDER_SEGMENTS 12
#define CYLINDER_SIDE_EVERY 4 /* a side line at every 4th segment: four in all */
#define TWO_PI 6.2831853f

/* --- geometry, worked out once per map ------------------------------------------------------------ */

static const airmap_t *current_map, *current_scene;
static char current_map_name[64];
static struct {
  float R[9]; /* rotation, row-major */
  float centre[3], radius;
  uint16_t first_point, n_points, first_edge, n_edges;
} wall_geo[MAX_WALLS];
static int n_walls;
static float points[MAX_POINTS][3];
static uint16_t edges[MAX_EDGES][2];
static float camera_points[MAX_POINTS][3];
static int n_points, n_edges;

static const airwall_t *map_wall(int index)
{
  if (current_scene && index < current_scene->n)
    return &data_airwalls[current_scene->first + index];
  if (current_scene)
    index -= current_scene->n;
  return &data_airwalls[current_map->first + index];
}

static bool wall_shown(const airwall_t *wall)
{
  switch (wall->kind) {
    case VOLUME_BLOCKER: return airwalls_shown;
    case VOLUME_OTHER_WALL: return airwalls_shown && airwalls_all;
    case VOLUME_LOADING: return loading_zones_shown;
    case VOLUME_VOID:
    case VOLUME_GROTTO_SAFETY: return void_zones_shown;
    default: return false;
  }
}

/* R = Rz Ry Rx for rotation angles (x, y, z) */
static void rotation_matrix(const float angles[3], float R[9])
{
  float sx = sinf(angles[0]), cx = cosf(angles[0]);
  float sy = sinf(angles[1]), cy = cosf(angles[1]);
  float sz = sinf(angles[2]), cz = cosf(angles[2]);
  float m[9] = { cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz,
                 cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz,
                 -sy,     sx * cy,                cx * cy };
  memcpy(R, m, sizeof(m));
}

/* t + R (s * local) */
static void to_world(const airwall_t *wall, const float R[9], const float local[3], float out[3])
{
  float scaled[3] = { local[0] * wall->s[0], local[1] * wall->s[1], local[2] * wall->s[2] };
  for (int i = 0; i < 3; i++)
    out[i] = R[i * 3] * scaled[0] + R[i * 3 + 1] * scaled[1] + R[i * 3 + 2] * scaled[2] + wall->t[i];
}

static int add_point(const float p[3])
{
  if (n_points >= MAX_POINTS)
    return -1;
  memcpy(points[n_points], p, sizeof(points[0]));
  return n_points++;
}

static void add_edge(int a, int b)
{
  if (a >= 0 && b >= 0 && n_edges < MAX_EDGES) {
    edges[n_edges][0] = (uint16_t)a;
    edges[n_edges][1] = (uint16_t)b;
    n_edges++;
  }
}

/* A box: 8 corners (bit k of the corner number picks the side on axis k) and 12 edges. */
static void add_box(const airwall_t *wall, const float R[9], const airshape_t *box)
{
  int base = n_points;
  for (int corner = 0; corner < 8; corner++) {
    float local[3], world[3];
    for (int axis = 0; axis < 3; axis++)
      local[axis] = box->a[axis] + (corner >> axis & 1 ? box->b[axis] : -box->b[axis]);
    to_world(wall, R, local, world);
    add_point(world);
  }
  for (int corner = 0; corner < 8; corner++)
    for (int axis = 0; axis < 3; axis++)
      if (!(corner >> axis & 1))
        add_edge(base + corner, base + (corner | 1 << axis));
}

/* A cylinder: rings at both ends, joined by four side lines. */
static void add_cylinder(const airwall_t *wall, const float R[9], const airshape_t *cylinder)
{
  int base = n_points;
  for (int ring = 0; ring < 2; ring++) {
    const float *centre = ring ? cylinder->b : cylinder->a;
    for (int j = 0; j < CYLINDER_SEGMENTS; j++) {
      float angle = TWO_PI * j / CYLINDER_SEGMENTS;
      float local[3] = { centre[0] + cylinder->radius * cosf(angle), centre[1],
                         centre[2] + cylinder->radius * sinf(angle) };
      float world[3];
      to_world(wall, R, local, world);
      add_point(world);
    }
  }
  for (int j = 0; j < CYLINDER_SEGMENTS; j++) {
    add_edge(base + j, base + (j + 1) % CYLINDER_SEGMENTS);
    add_edge(base + CYLINDER_SEGMENTS + j, base + CYLINDER_SEGMENTS + (j + 1) % CYLINDER_SEGMENTS);
    if (j % CYLINDER_SIDE_EVERY == 0)
      add_edge(base + j, base + CYLINDER_SEGMENTS + j);
  }
}

/* The sphere around points[first_point..n_points), for skipping walls quickly. */
static void bounding_sphere(const airwall_t *wall, int first_point, float centre[3], float *radius)
{
  float sum[3] = { 0 };
  int count = n_points - first_point;
  for (int j = first_point; j < n_points; j++)
    for (int axis = 0; axis < 3; axis++)
      sum[axis] += points[j][axis];
  for (int axis = 0; axis < 3; axis++)
    centre[axis] = count ? sum[axis] / count : wall->t[axis];
  *radius = 0;
  for (int j = first_point; j < n_points; j++) {
    float dx = points[j][0] - centre[0];
    float dy = points[j][1] - centre[1];
    float dz = points[j][2] - centre[2];
    float distance = sqrtf(dx * dx + dy * dy + dz * dz);
    if (distance > *radius)
      *radius = distance;
  }
}

static void build_map_geometry(void)
{
  n_walls = 0;
  n_points = 0;
  n_edges = 0;
  int count = (current_map ? current_map->n : 0) + (current_scene ? current_scene->n : 0);
  for (int i = 0; i < count && n_walls < MAX_WALLS; i++) {
    const airwall_t *wall = map_wall(i);
    __typeof__(wall_geo[0]) *geo = &wall_geo[n_walls++];
    rotation_matrix(wall->r, geo->R);
    geo->first_edge = (uint16_t)n_edges;
    geo->first_point = (uint16_t)n_points;
    for (int k = 0; k < wall->n_shapes; k++) {
      const airshape_t *shape = &data_airshapes[wall->first_shape + k];
      if (shape->type == SHAPE_BOX)
        add_box(wall, geo->R, shape);
      else
        add_cylinder(wall, geo->R, shape);
    }
    geo->n_edges = (uint16_t)(n_edges - geo->first_edge);
    geo->n_points = (uint16_t)(n_points - geo->first_point);
    bounding_sphere(wall, geo->first_point, geo->centre, &geo->radius);
  }
  log_printf("collision: %s: %d volumes, %d edges", current_map_name, n_walls, n_edges);
}

static bool str_equal(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

/* Rebuild the geometry when the map changes. */
static void update_map(void)
{
  const char *name = gd_get_str(GD_STR_MAP);
  if (!name || !name[0]) {
    current_map = current_scene = 0;
    current_map_name[0] = 0;
    n_walls = 0;
    return;
  }
  if (str_equal(current_map_name, name))
    return;
  snprintf(current_map_name, sizeof(current_map_name), "%s", name);
  current_map = current_scene = 0;
  for (int i = 0; i < data_n_airmaps; i++)
    if (str_equal(data_airmaps[i].map, name)) {
      current_map = &data_airmaps[i];
      break;
    }
  for (int i = 0; i < data_n_scene_maps; i++)
    if (str_equal(data_scene_maps[i].map, name)) {
      current_scene = &data_scene_maps[i];
      break;
    }
  build_map_geometry();
}

/* --- Link inside a wall (airwall.py inside()) ------------------------------------------------------- */

/* How deep point P is inside wall `index` (0 if outside). *up: the nearest face points up (+1),
 * down (-1) or sideways (0). Only boxes count. */
static float depth_inside(int index, const float P[3], int *up)
{
  const airwall_t *wall = map_wall(index);
  const float *R = wall_geo[index].R;
  float d[3] = { P[0] - wall->t[0], P[1] - wall->t[1], P[2] - wall->t[2] };
  float rotated[3]; /* R^T d: P in the wall's own axes, still scaled */
  for (int i = 0; i < 3; i++)
    rotated[i] = R[i] * d[0] + R[3 + i] * d[1] + R[6 + i] * d[2];
  for (int k = 0; k < wall->n_shapes; k++) {
    const airshape_t *box = &data_airshapes[wall->first_shape + k];
    if (box->type != SHAPE_BOX)
      continue;
    float best = 1e9f;
    float normal[3] = { 0 };
    bool in = true;
    for (int axis = 0; axis < 3; axis++) {
      float scale = wall->s[axis] < 0 ? -wall->s[axis] : wall->s[axis];
      if (scale < 1e-9f)
        scale = 1e-9f;
      float local = rotated[axis] / scale - box->a[axis];
      if (local <= -box->b[axis] || local >= box->b[axis]) {
        in = false;
        break;
      }
      /* distance to the face on each side of this axis */
      for (int sign = -1; sign <= 1; sign += 2) {
        float depth = (box->b[axis] - sign * local) * scale;
        if (depth < best) {
          best = depth;
          for (int i = 0; i < 3; i++)
            normal[i] = R[i * 3 + axis] * sign;
        }
      }
    }
    if (in) {
      *up = normal[1] > FACE_UPRIGHT ? 1 : normal[1] < -FACE_UPRIGHT ? -1 : 0;
      return best;
    }
  }
  return 0;
}

static float link_depth;
static int link_wall = -1; /* the wall Link is deepest in, or -1 */
static int link_up;

static void check_link(void)
{
  vec3f_t drawn;
  link_wall = -1;
  if (!current_map || !link_drawn_pos(&drawn))
    return;
  float P[3] = { drawn.x, drawn.y + PHYS_Y, drawn.z };
  float best = 0;
  for (int i = 0; i < n_walls; i++) {
    const airwall_t *wall = map_wall(i);
    if (wall->kind != VOLUME_BLOCKER && !(airwalls_all && wall->kind == VOLUME_OTHER_WALL))
      continue;
    float dx = P[0] - wall_geo[i].centre[0];
    float dy = P[1] - wall_geo[i].centre[1];
    float dz = P[2] - wall_geo[i].centre[2];
    if (dx * dx + dy * dy + dz * dz > wall_geo[i].radius * wall_geo[i].radius)
      continue;
    int up;
    float depth = depth_inside(i, P, &up);
    if (depth > best) {
      best = depth;
      link_wall = i;
      link_up = up;
    }
  }
  link_depth = best;
}

void airwall_link_text(char *buf, int len)
{
  if (!current_map)
    snprintf(buf, len, TF("no walls here"));
  else if (link_wall < 0)
    snprintf(buf, len, TF("outside"));
  else
    snprintf(buf, len, TF("inside %.2f, out %s"), link_depth,
             T(link_up > 0 ? "up" : link_up < 0 ? "down" : "sideways"));
}

/* --- projection to dots ------------------------------------------------------------------------- */

/* Two buffers: the game thread fills one while the render thread draws the other. */
static aw_dot_t dots[2][MAX_DOTS];
static int n_dots[2];
static volatile int published = -1; /* the buffer to draw, or -1 for none */

/* 0..size to 0..65535, clamped */
static uint16_t to_unit16(float v, float size)
{
  v = v / size * 65535;
  return (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v);
}

/* a rectangle from two corners in reference pixels */
static void add_rect(aw_dot_t *out, int *n, float ax, float ay, float bx, float by, uint8_t color)
{
  aw_dot_t *rect = &out[(*n)++];
  rect->x0 = to_unit16(ax < bx ? ax : bx, REF_WIDTH);
  rect->x1 = to_unit16(ax < bx ? bx : ax, REF_WIDTH);
  rect->y0 = to_unit16(ay < by ? ay : by, REF_HEIGHT);
  rect->y1 = to_unit16(ay < by ? by : ay, REF_HEIGHT);
  rect->color = color;
}

/* Cut the line va-vb (camera space) at the near plane. False if it's all behind it. */
static bool clip_near(float va[3], float vb[3])
{
  if (va[2] < NEAR_PLANE && vb[2] < NEAR_PLANE)
    return false;
  if (va[2] < NEAR_PLANE || vb[2] < NEAR_PLANE) {
    float t = (NEAR_PLANE - va[2]) / (vb[2] - va[2]);
    float cut[3];
    for (int i = 0; i < 3; i++)
      cut[i] = va[i] + t * (vb[i] - va[i]);
    if (va[2] < NEAR_PLANE)
      memcpy(va, cut, sizeof(cut));
    else
      memcpy(vb, cut, sizeof(cut));
  }
  return true;
}

/* Cut the line (x0, y0)-(x1, y1) to the reference picture (Liang-Barsky). False if it's all outside. */
static bool clip_to_picture(float *x0, float *y0, float *x1, float *y1)
{
  float dx = *x1 - *x0, dy = *y1 - *y0;
  float t0 = 0, t1 = 1;
  float p[4] = { -dx, dx, -dy, dy };
  float q[4] = { *x0, REF_WIDTH - *x0, *y0, REF_HEIGHT - *y0 };
  for (int i = 0; i < 4; i++) {
    if (p[i] == 0) {
      if (q[i] < 0)
        return false;
      continue;
    }
    float r = q[i] / p[i];
    if (p[i] < 0) {
      if (r > t1)
        return false;
      if (r > t0)
        t0 = r;
    } else {
      if (r < t0)
        return false;
      if (r < t1)
        t1 = r;
    }
  }
  *x1 = *x0 + t1 * dx;
  *y1 = *y0 + t1 * dy;
  *x0 += t0 * dx;
  *y0 += t0 * dy;
  return true;
}

/* A line on the picture: near level or upright, a staircase of long rectangles; otherwise dots. */
static void add_line(aw_dot_t *out, int *n, float x0, float y0, float x1, float y1, uint8_t color, int limit)
{
  float dx = x1 - x0, dy = y1 - y0;
  float abs_dx = dx < 0 ? -dx : dx, abs_dy = dy < 0 ? -dy : dy;
  bool level = abs_dx >= abs_dy;
  float major = level ? abs_dx : abs_dy, minor = level ? abs_dy : abs_dx;
  int pieces = (int)(minor / LINE_WIDTH) + 1;
  if (minor <= major * STRAIGHT && pieces <= limit) {
    /* Solid runs only if they fit the edge budget. Close-up edges otherwise
     * use evenly spaced dots so one side cannot consume the rest of a volume. */
    for (int i = 0; i < pieces && *n < MAX_DOTS; i++) {
      float ta = (float)i / pieces, tb = (float)(i + 1) / pieces, t_mid = (ta + tb) / 2;
      float ax, ay, bx, by;
      if (level) {
        ax = x0 + dx * ta;
        bx = x0 + dx * tb;
        ay = by = y0 + dy * t_mid;
      } else {
        ay = y0 + dy * ta;
        by = y0 + dy * tb;
        ax = bx = x0 + dx * t_mid;
      }
      add_rect(out, n, ax, ay, bx, by, color);
    }
    return;
  }
  int steps = (int)(major / DOT_SPACING);
  if (steps >= limit)
    steps = limit - 1;
  for (int i = 0; i <= steps && *n < MAX_DOTS; i++) {
    float t = steps ? (float)i / steps : 0;
    float x = x0 + dx * t, y = y0 + dy * t;
    add_rect(out, n, x, y, x, y, color);
  }
}

/* The edge a-b (camera space), clipped and projected into out[*n...]. */
static void add_edge_dots(aw_dot_t *out, int *n, const view_cam_t *cam, const float a[3], const float b[3],
                          uint8_t color, int limit)
{
  float va[3], vb[3];
  memcpy(va, a, sizeof(va));
  memcpy(vb, b, sizeof(vb));
  if (!clip_near(va, vb))
    return;
  float x0, y0, x1, y1;
  view_screen(cam, va, &x0, &y0);
  view_screen(cam, vb, &x1, &y1);
  x0 *= REF_WIDTH;
  x1 *= REF_WIDTH;
  y0 *= REF_HEIGHT;
  y1 *= REF_HEIGHT;
  if (!clip_to_picture(&x0, &y0, &x1, &y1))
    return;
  add_line(out, n, x0, y0, x1, y1, color, limit);
}

/* --- hidden while the game is paused ------------------------------------------------------------- */

/* True while the game shows its pause screen or an item menu, where the walls would draw over it.
 * Frame advance doesn't count (see debug > pause research), so the walls still show while advancing. */
static bool game_is_paused(void)
{
  bool is_pause;
  int32_t pause_requests;
  bool paused = gd_game_paused(&is_pause, &pause_requests);

  /* Log each change, with both values, so it can be checked which one the game's menus set. */
  static bool was_paused;
  if (paused != was_paused) {
    if (paused)
      log_printf("air walls: hidden, game paused (IsPause %d, GamePauseRequestCount %d)", (int)is_pause,
                 (int)pause_requests);
    else
      log_printf("air walls: shown again, game unpaused");
    was_paused = paused;
  }
  return paused;
}

/* --- frame ---------------------------------------------------------------------------------------- */

static int16_t near_order[MAX_VISIBLE_VOLUMES];  /* walls within AW_RANGE of the camera, nearest first */
static float near_distance[MAX_VISIBLE_VOLUMES];

/* A wall's bounding sphere is wholly outside the camera frustum. This is conservative:
 * intersecting spheres stay in, so no visible edge is dropped before the exact line clipping. */
static bool sphere_outside_view(const view_cam_t *cam, const float centre[3], float radius,
                                float side_norm, float top_norm)
{
  float v[3];
  view_point(cam, centre, v);
  if (v[2] + radius < NEAR_PLANE)
    return true;
  if (v[0] - v[2] * cam->tan_x > radius * side_norm ||
      -v[0] - v[2] * cam->tan_x > radius * side_norm)
    return true;
  if (v[1] - v[2] * cam->tan_y > radius * top_norm ||
      -v[1] - v[2] * cam->tan_y > radius * top_norm)
    return true;
  return false;
}

/* Fill near_order (an insertion sort) and return how many walls are in it. */
static int sort_near_walls(const view_cam_t *cam)
{
  int count = 0;
  const float side_norm = sqrtf(1 + cam->tan_x * cam->tan_x);
  const float top_norm = sqrtf(1 + cam->tan_y * cam->tan_y);
  for (int i = 0; i < n_walls; i++) {
    const airwall_t *wall = map_wall(i);
    if (!wall_shown(wall))
      continue;
    float dx = wall_geo[i].centre[0] - cam->pos[0];
    float dy = wall_geo[i].centre[1] - cam->pos[1];
    float dz = wall_geo[i].centre[2] - cam->pos[2];
    bool death = wall->kind == VOLUME_VOID;
    float limit = (death ? DEATH_RANGE : AW_RANGE) + wall_geo[i].radius;
    float distance_sq = dx * dx + dy * dy + dz * dz;
    /* Reject the common far case without a square root. */
    if (distance_sq > limit * limit)
      continue;
    if (sphere_outside_view(cam, wall_geo[i].centre, wall_geo[i].radius, side_norm, top_norm))
      continue;
    float distance = sqrtf(distance_sq) - wall_geo[i].radius;
    /* Reserve the nearest portion of the list for trigger volumes so
     * dense barriers cannot hide an entrance or a death volume. */
    if (wall->kind >= VOLUME_LOADING)
      distance -= DEATH_RANGE;
    if (count == MAX_VISIBLE_VOLUMES && distance >= near_distance[count - 1])
      continue;
    int j = count < MAX_VISIBLE_VOLUMES ? count++ : count - 1;
    while (j > 0 && near_distance[j - 1] > distance) {
      near_order[j] = near_order[j - 1];
      near_distance[j] = near_distance[j - 1];
      j--;
    }
    near_order[j] = (int16_t)i;
    near_distance[j] = distance;
  }
  return count;
}

static void add_world_cross(aw_dot_t *out, int *n, const view_cam_t *cam, const float point[3],
                            uint8_t color, float radius)
{
  float centre[3], sx, sy;
  view_point(cam, point, centre);
  if (*n + 2 > MAX_DOTS || centre[2] <= NEAR_PLANE)
    return;
  view_screen(cam, centre, &sx, &sy);
  if (sx > .01f && sx < .99f && sy > .01f && sy < .99f) {
    float x = sx * REF_WIDTH, y = sy * REF_HEIGHT;
    add_rect(out, n, x - radius, y, x + radius, y, color);
    add_rect(out, n, x, y - radius, x, y + radius, color);
  }
}

static void draw_collision_mesh(aw_dot_t *out, int *n, const view_cam_t *cam)
{
  vec3f_t link;
  if (!link_drawn_pos(&link))
    return;
  unsigned count;
  const mesh_edge_t *mesh = collision_mesh_near(current_map_name, link, &count);
  int limit = *n + 360;
  if (limit > MAX_DOTS)
    limit = MAX_DOTS;
  for (unsigned i = 0; i < count && *n + 4 <= limit; i++) {
    const mesh_edge_t *edge = &mesh[i];
    if (edge->kind > 2)
      continue;
    float a[3], b[3], va[3], vb[3];
    for (int axis = 0; axis < 3; axis++) {
      a[axis] = edge->a[axis] / 16.0f;
      b[axis] = edge->b[axis] / 16.0f;
    }
    float dy = (a[1] + b[1]) * .5f - link.y;
    if (dy < -12 || dy > 12)
      continue;
    view_point(cam, a, va);
    view_point(cam, b, vb);
    add_edge_dots(out, n, cam, va, vb, AW_MESH_FLOOR + edge->kind, 4);
  }
}

void airwall_frame(bool menu_open)
{
  update_map();
  check_link();
  int next = published == 0 ? 1 : 0;
  int n = 0;
  bool enabled = airwalls_shown || loading_zones_shown || void_zones_shown;
  vec3f_t fall;
  bool show_fall = void_zones_shown && diagnostics_last_fall(current_map_name, &fall);
  bool draw = ((enabled && (n_walls || show_fall)) || collision_mesh_shown) &&
              !menu_open && !game_is_paused();
  view_cam_t cam;
  if (draw && view_camera(&cam)) {
    if (show_fall) {
      float point[3] = { fall.x, fall.y, fall.z };
      add_world_cross(dots[next], &n, &cam, point, AW_FALL_SAMPLE, 9);
    }
    /* nearest wall first, so the dot budget goes to what's close */
    int n_walls_near = sort_near_walls(&cam);
    for (int k = 0; k < n_walls_near && n < MAX_DOTS; k++) {
      int i = near_order[k];
      const airwall_t *wall = map_wall(i);
      static const uint8_t colors[] = {
        [VOLUME_BLOCKER] = AW_BLOCKS, [VOLUME_OTHER_WALL] = AW_OTHER,
        [VOLUME_LOADING] = AW_LOADING, [VOLUME_VOID] = AW_VOID,
        [VOLUME_GROTTO_SAFETY] = AW_GROTTO_SAFETY,
      };
      uint8_t color = colors[wall->kind];
      const __typeof__(wall_geo[0]) *geo = &wall_geo[i];
      if (wall->kind >= VOLUME_LOADING)
        add_world_cross(dots[next], &n, &cam, geo->centre, color, 5);
      /* Reduce detail under pressure instead of dropping the whole outline.
       * No nearby volume can spend more than one fifth of the frame budget. */
      int budget = MAX_DOTS - n;
      if (budget > MAX_DOTS / 5)
        budget = MAX_DOTS / 5;
      if (!geo->n_edges || budget < geo->n_edges)
        continue;
      int limit = budget / geo->n_edges;
      if (limit > MAX_RECTS_PER_EDGE)
        limit = MAX_RECTS_PER_EDGE;
      for (int p = geo->first_point; p < geo->first_point + geo->n_points; p++)
        view_point(&cam, points[p], camera_points[p]);
      for (int e = geo->first_edge; n < MAX_DOTS && e < geo->first_edge + geo->n_edges; e++)
        add_edge_dots(dots[next], &n, &cam, camera_points[edges[e][0]], camera_points[edges[e][1]], color, limit);
    }
    if (collision_mesh_shown)
      draw_collision_mesh(dots[next], &n, &cam);
  }
  n_dots[next] = n;
  __atomic_store_n(&published, n ? next : -1, __ATOMIC_RELEASE);
}

const aw_dot_t *airwall_published(int *n)
{
  int buffer = __atomic_load_n(&published, __ATOMIC_ACQUIRE);
  *n = buffer < 0 ? 0 : n_dots[buffer];
  return buffer < 0 ? 0 : dots[buffer];
}

void airwall_summary(char *buf, int len)
{
  int counts[VOLUME_COUNT] = {0};
  for (int i = 0; i < n_walls; i++)
    counts[map_wall(i)->kind]++;
  snprintf(buf, len, TF("walls %d  load %d  void %d  safety %d"),
           counts[VOLUME_BLOCKER] + counts[VOLUME_OTHER_WALL],
           counts[VOLUME_LOADING], counts[VOLUME_VOID], counts[VOLUME_GROTTO_SAFETY]);
}

void airwall_nearest_death(char *buf, int len)
{
  vec3f_t link = { 0 };
  int nearest = -1;
  float best = 0;
  if (link_drawn_pos(&link)) {
    for (int i = 0; i < n_walls; i++) {
      if (map_wall(i)->kind != VOLUME_VOID)
        continue;
      const float *p = wall_geo[i].centre;
      float x = p[0] - link.x, y = p[1] - link.y, z = p[2] - link.z;
      float distance = x*x + y*y + z*z;
      if (nearest < 0 || distance < best) {
        nearest = i;
        best = distance;
      }
    }
  }
  if (nearest < 0) {
    snprintf(buf, len, TF("no placed death trigger found"));
    return;
  }
  const float *p = wall_geo[nearest].centre;
  float x = p[0] - link.x, y = p[1] - link.y, z = p[2] - link.z;
  snprintf(buf, len, TF("nearest death: %.0fm  %.1f / %.1f / %.1f"),
           (double)sqrtf(x * x + y * y + z * z), (double)p[0], (double)p[1], (double)p[2]);
}

/* Distance to the center of the placed geometry, not a collision query. */
void airwall_nearest_barrier(char *buf, int len)
{
  vec3f_t link;
  int nearest = -1;
  float best = 0;
  if (link_drawn_pos(&link)) {
    for (int i = 0; i < n_walls; i++) {
      if (map_wall(i)->kind > VOLUME_OTHER_WALL)
        continue;
      const float *p = wall_geo[i].centre;
      float x = p[0] - link.x, y = p[1] - link.y, z = p[2] - link.z;
      float distance = x*x + y*y + z*z;
      if (nearest < 0 || distance < best) {
        nearest = i;
        best = distance;
      }
    }
  }
  if (nearest < 0) {
    snprintf(buf, len, "unavailable");
    return;
  }
  const airwall_t *wall = map_wall(nearest);
  const float *p = wall_geo[nearest].centre;
  float x = p[0] - link.x, y = p[1] - link.y, z = p[2] - link.z;
  snprintf(buf, len, "%s, %.2fm to center, %s", wall->actor, (double)sqrtf(x * x + y * y + z * z),
           wall->kind == VOLUME_BLOCKER ? "player barrier" : "special-purpose barrier");
}
