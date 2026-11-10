/* Explicit, bounded captures on the game thread through the existing SD mount. */
#include "diagnostics.h"

#include <stdarg.h>

#include "airwall.h"
#include "cct.h"
#include "cheats.h"
#include "gamedata.h"
#include "gfx.h"
#include "input.h"
#include "levitate.h"
#include "pause.h"
#include "sdfs.h"
#include "sys.h"
#include "z64hd.h"

#define REPORT_CAPACITY 8192
#define HISTORY_LINES 28

static char report[REPORT_CAPACITY];
static size_t report_size;

typedef struct {
  uint32_t type, player_flags, contact_flags;
  uint64_t contact_tags;
  uintptr_t query_flags_fn, query_scalar_fn;
  uint32_t mode, query_flags;
  uint16_t extended_flags;
  uint8_t gate, query_ready;
  float gate_value, query_distance, fall_distance;
  bool valid, query_valid, extended_valid;
} abyss_sample_t;

static struct {
  char map[GD_STR_LEN];
  unsigned frame;
  uint32_t abyss_type;
  vec3f_t before, first, lowest;
  abyss_sample_t decision_inputs, before_inputs, active_inputs;
  bool has_before, has_position;
} last_fall;

/* Keep a live timeline plus the two most recent fall windows. All writes to
 * disk are explicit. Sampling runs at 30 Hz and on observed state changes. */
#define TRACE_RECENT 512
#define TRACE_PRE 128
#define TRACE_POST 96
#define TRACE_FALLS 2

enum {
  TRACE_FEET = 1 << 0, TRACE_CAMERA = 1 << 1, TRACE_CONTROLLER = 1 << 2,
  TRACE_GROUND_KNOWN = 1 << 3, TRACE_GROUNDED = 1 << 4, TRACE_NO_HIT = 1 << 5,
  TRACE_MENU = 1 << 6, TRACE_NATIVE_UI = 1 << 7, TRACE_TRANSITION = 1 << 8,
  TRACE_FALL = 1 << 9, TRACE_LEVITATE = 1 << 10, TRACE_GHOST = 1 << 11,
  TRACE_MENU_PAUSE = 1 << 12, TRACE_ABYSS = 1 << 13,
};

typedef struct {
  uint64_t tick;
  uintptr_t actor, camera, body, pending;
  vec3f_t feet, eye, target, velocity;
  uint64_t body_flags;
  abyss_sample_t abyss;
  uint32_t frame, state, held, pressed;
  int32_t sticks[4], speed;
  char map[GD_STR_LEN], next_map[GD_STR_LEN];
} trace_sample_t;

static trace_sample_t recent[TRACE_RECENT];
static unsigned recent_head, recent_count;
static struct {
  trace_sample_t samples[TRACE_PRE + TRACE_POST];
  uint64_t tick;
  unsigned before, count;
} fall_traces[TRACE_FALLS];
static unsigned fall_head;
static abyss_sample_t frame_abyss;
static bool frame_fall, frame_fall_started;

static abyss_sample_t abyss_inputs(uintptr_t actor)
{
  uintptr_t components = actor ? sys_ptr(actor + 0x270) : 0;
  uintptr_t player = components ? sys_ptr(components + 0x358) : 0;
  if (!player || !sys_mapped(player, 0x1920, SYS_PERM_R) || sys_ptr(player + 0x18) != actor)
    return (abyss_sample_t){0};
  /* Inputs inspected by the type selector at main+0x82C510. The event can
   * clear flags before our frame hook, so retain the previous sample too. */
  abyss_sample_t sample = {
    .type = *(const volatile uint32_t *)(player + 0x191C),
    .player_flags = *(const volatile uint32_t *)(player + 0x28),
    .contact_flags = *(const volatile uint32_t *)(player + 0x13D0),
    .contact_tags = *(const volatile uint64_t *)(player + 0x13E8),
    .mode = *(const volatile uint32_t *)(player + 0x7E8),
    .gate = *(const volatile uint8_t *)(player + 0x13FB),
    .gate_value = *(const volatile float *)(player + 0x16A0),
    .fall_distance = *(const volatile float *)(player + 0x190C),
    .valid = true,
  };
  sample.extended_valid = sys_mapped(player + 0x1C30, sizeof(uint16_t), SYS_PERM_R);
  if (sample.extended_valid)
    sample.extended_flags = *(const volatile uint16_t *)(player + 0x1C30);
  /* The producer of contact bit 19 at main+0x5FBC44 reads this object.
   * Its flags and scalar are evidence; do not call its virtual queries. */
  uintptr_t query = sys_ptr(components + 0x460);
  sample.query_valid = query && sys_mapped(query, 0x514, SYS_PERM_R);
  if (sample.query_valid) {
    sample.query_flags = *(const volatile uint32_t *)(query + 0xA8);
    sample.query_ready = *(const volatile uint8_t *)(query + 0x37C);
    sample.query_distance = *(const volatile float *)(query + 0x510);
    uintptr_t vtable = sys_ptr(query + 0x108);
    if (vtable) {
      sample.query_flags_fn = sys_ptr(vtable + 0xB0);
      sample.query_scalar_fn = sys_ptr(vtable + 0xB8);
    }
  }
  return sample;
}

/* Read the active flow using the same pointer chain as main+0xBCC0B0.
 * The game's getter writes a terminator. Read only the known name here. */
static bool fall_flow_active(void)
{
  static const char flow[] = "DmF_SY_FallDown";
  uintptr_t holder = sys_ptr(MAIN(z64hd_game->event_mgr_ptr));
  uintptr_t manager = holder ? sys_ptr(holder) : 0;
  uintptr_t context = manager ? sys_ptr(manager + 0x68) : 0;
  if (!context || !sys_mapped(context, 0xF1C, SYS_PERM_R))
    return false;
  unsigned count = *(const volatile uint32_t *)(context + 0xEB0);
  unsigned slot = *(const volatile uint32_t *)(context + 0xF18);
  uintptr_t slots = sys_ptr(context + 0xEB8);
  if (!slots || slot >= count || count > 4096 || slots > UINTPTR_MAX - slot * sizeof(uintptr_t))
    return false;
  uintptr_t entry = sys_ptr(slots + slot * sizeof(uintptr_t));
  uintptr_t name = entry ? sys_ptr(entry + 8) : 0;
  if (!name || !sys_mapped(name, sizeof(flow), SYS_PERM_R))
    return false;
  for (unsigned i = 0; i < sizeof(flow); i++)
    if (((const volatile char *)name)[i] != flow[i])
      return false;
  return true;
}

bool diagnostics_fall_frame(void)
{
  static bool was_active, had_previous;
  static uintptr_t previous_actor;
  static vec3f_t previous;
  static abyss_sample_t previous_inputs;
  static abyss_sample_t first_type_inputs;
  if (!z64hd_game || !z64hd_game->event_flow_layout)
    return false;
  bool active = fall_flow_active();
  vec3f_t feet;
  uintptr_t actor = link_actor();
  bool have_feet = actor && link_drawn_pos(&feet);
  abyss_sample_t inputs = abyss_inputs(actor);
  if (actor != previous_actor || !inputs.type)
    first_type_inputs = (abyss_sample_t){0};
  if (inputs.type && (!first_type_inputs.valid || inputs.type != first_type_inputs.type))
    first_type_inputs = inputs;
  frame_abyss = inputs;
  frame_fall = active;
  frame_fall_started = active && !was_active;
  if (frame_fall_started) {
    const char *map = gd_get_str(GD_STR_MAP);
    snprintf(last_fall.map, sizeof(last_fall.map), "%.63s", map ? map : "");
    last_fall.frame = frame_counter();
    last_fall.abyss_type = 0;
    last_fall.has_before = had_previous && actor == previous_actor;
    last_fall.before = previous;
    last_fall.before_inputs = actor == previous_actor ? previous_inputs : (abyss_sample_t){0};
    last_fall.active_inputs = inputs;
    last_fall.decision_inputs = first_type_inputs;
    last_fall.has_position = false;
    log_printf("fall: DmF_SY_FallDown active frame=%u map=%.30s", last_fall.frame, last_fall.map);
    if (last_fall.has_before)
      log_printf("fall: previous feet %.3f %.3f %.3f", previous.x, previous.y, previous.z);
  }
  if (active && !last_fall.abyss_type && inputs.type) {
    last_fall.abyss_type = inputs.type;
    if (!last_fall.decision_inputs.valid)
      last_fall.decision_inputs = inputs;
    log_printf("fall: native abyss type %u", inputs.type);
    const abyss_sample_t *sample = &last_fall.decision_inputs;
    log_printf("fall: first type inputs player=%#x contact=%#x tags=%#llx",
               sample->player_flags, sample->contact_flags, (unsigned long long)sample->contact_tags);
  }
  if (active && have_feet) {
    if (!last_fall.has_position) {
      last_fall.has_position = true;
      last_fall.first = last_fall.lowest = feet;
      log_printf("fall: first active feet %.3f %.3f %.3f", feet.x, feet.y, feet.z);
    } else if (feet.y < last_fall.lowest.y) {
      last_fall.lowest = feet;
    }
  }
  if (have_feet)
    previous = feet;
  had_previous = have_feet;
  previous_actor = actor;
  previous_inputs = inputs;
  bool changed = was_active != active;
  was_active = active;
  return changed;
}

bool diagnostics_last_fall(const char *map, vec3f_t *position)
{
  if (!map || !last_fall.map[0] || !last_fall.has_position)
    return false;
  for (unsigned i = 0; i < sizeof(last_fall.map); i++) {
    if (map[i] != last_fall.map[i])
      return false;
    if (!map[i]) {
      *position = last_fall.first;
      return true;
    }
  }
  return false;
}

static void append(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void append(const char *format, ...)
{
  size_t available = sizeof(report) - report_size;
  va_list args;
  va_start(args, format);
  int n = vsnprintf(report + report_size, available, format, args);
  va_end(args);
  if (n > 0)
    report_size += (size_t)n < available ? (size_t)n : available - 1;
}

static void append_abyss_inputs(const char *phase, abyss_sample_t sample)
{
  if (!sample.valid)
    return;
  append("%s abyss inputs type=%u player=%#x contact=%#x tags=%#llx mode=%u gate=%#x value=%.6g fall-distance=%.6g\n",
         phase, sample.type, sample.player_flags, sample.contact_flags, (unsigned long long)sample.contact_tags,
         sample.mode, sample.gate, sample.gate_value, sample.fall_distance);
  if (sample.extended_valid)
    append("%s extended flags=%#x\n", phase, sample.extended_flags);
  if (sample.query_valid)
    append("%s contact-query flags=%#x ready=%#x scalar=%.6g flags-fn=%#lx scalar-fn=%#lx\n", phase,
           sample.query_flags, sample.query_ready, sample.query_distance,
           (unsigned long)sample.query_flags_fn, (unsigned long)sample.query_scalar_fn);
}

static bool write_file(const char *dir, const char *name, const void *data, size_t size)
{
  char path[128];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  fs_file_t file;
  if (fs_create_file(path, size) != 0)
    return false;
  bool written = false;
  if (fs_open(&file, path, FS_WRITE) == 0) {
    const fs_write_option_t flush = { FS_WRITE_FLUSH };
    written = fs_write(file, 0, data, size, &flush) == 0;
    fs_close(file);
  }
  if (!written)
    fs_delete_file(path);
  return written;
}

void diagnostics_trace_frame(bool transition, bool native_ui, bool menu)
{
  static uint64_t last_tick;
  static unsigned last_state;
  static uintptr_t last_actor;
  static abyss_sample_t last_abyss;
  uint64_t tick = sys_tick();
  uintptr_t actor = link_actor();
  unsigned state = (transition ? TRACE_TRANSITION : 0) |
                   (native_ui ? TRACE_NATIVE_UI : 0) |
                   (menu ? TRACE_MENU : 0) |
                   (pause_menu_owned() ? TRACE_MENU_PAUSE : 0) |
                   (levitate_enabled ? TRACE_LEVITATE : 0) |
                   (levitate_through_walls ? TRACE_GHOST : 0) |
                   (frame_fall ? TRACE_FALL : 0) |
                   (frame_abyss.valid ? TRACE_ABYSS : 0);
  if (tick - last_tick < sys_tick_freq() / 30 && last_state == state && last_actor == actor &&
      last_abyss.type == frame_abyss.type && last_abyss.player_flags == frame_abyss.player_flags &&
      last_abyss.contact_flags == frame_abyss.contact_flags && last_abyss.contact_tags == frame_abyss.contact_tags)
    return;

  last_tick = tick;
  last_state = state;
  last_actor = actor;
  last_abyss = frame_abyss;
  trace_sample_t *sample = &recent[recent_head];
  *sample = (trace_sample_t){
    .tick = tick, .actor = actor, .frame = frame_counter(), .state = state,
    .held = input_held(), .pressed = input_pressed(), .speed = levitate_move_speed, .abyss = frame_abyss,
  };
  input_sticks(&sample->sticks[0], &sample->sticks[1], &sample->sticks[2], &sample->sticks[3]);
  const char *map = gd_get_str(GD_STR_MAP), *next = gd_get_str(GD_STR_NEXT_MAP);
  snprintf(sample->map, sizeof(sample->map), "%.63s", map ? map : "");
  snprintf(sample->next_map, sizeof(sample->next_map), "%.63s", next ? next : "");
  if (actor && link_drawn_pos(&sample->feet))
    sample->state |= TRACE_FEET;
  uintptr_t cam = camera();
  if (cam && sys_mapped(cam + 0x38, 2 * sizeof(vec3f_t), SYS_PERM_R)) {
    sample->camera = cam;
    memcpy(&sample->eye, (const void *)(cam + 0x38), sizeof(vec3f_t));
    memcpy(&sample->target, (const void *)(cam + 0x44), sizeof(vec3f_t));
    sample->state |= TRACE_CAMERA;
  }
  cct_snapshot_t cct;
  if (actor && cct_snapshot(&cct) && cct.actor == actor) {
    sample->state |= TRACE_CONTROLLER | (cct.no_hit ? TRACE_NO_HIT : 0);
    sample->body = cct.body;
    sample->pending = cct.pending;
    sample->velocity = cct.velocity;
    sample->body_flags = cct.body_flags;
    bool grounded;
    if (cct_on_ground(actor, &grounded))
      sample->state |= TRACE_GROUND_KNOWN | (grounded ? TRACE_GROUNDED : 0);
  }
  if (frame_fall_started) {
    unsigned slot = fall_head++ % TRACE_FALLS;
    unsigned before = recent_count < TRACE_PRE ? recent_count : TRACE_PRE;
    fall_traces[slot].tick = tick;
    fall_traces[slot].before = fall_traces[slot].count = before;
    for (unsigned i = 0; i < before; i++)
      fall_traces[slot].samples[i] = recent[(recent_head + TRACE_RECENT - before + i) % TRACE_RECENT];
  }
  for (unsigned i = 0; i < TRACE_FALLS; i++)
    if (fall_traces[i].tick && fall_traces[i].count < fall_traces[i].before + TRACE_POST)
      fall_traces[i].samples[fall_traces[i].count++] = *sample;
  recent_head = (recent_head + 1) % TRACE_RECENT;
  if (recent_count < TRACE_RECENT)
    recent_count++;
}

static bool write_trace(const char *dir, const char *name, const trace_sample_t *samples,
                        unsigned capacity, unsigned first, unsigned count)
{
  static char csv[TRACE_RECENT * 640 + 512];
  size_t used = (size_t)snprintf(csv, sizeof(csv),
    "tick,frame,map,next_map,state,actor,camera,body,pending,held,pressed,lx,ly,rx,ry,speed,"
    "feet_x,feet_y,feet_z,eye_x,eye_y,eye_z,target_x,target_y,target_z,"
    "vel_x,vel_y,vel_z,body_flags,abyss_type,player_flags,contact_flags,contact_tags,"
    "abyss_mode,abyss_gate,abyss_value,fall_distance,extended_valid,extended_flags,"
    "query_valid,query_flags,query_ready,query_scalar,query_flags_fn,query_scalar_fn\n");
  for (unsigned i = 0; i < count; i++) {
    const trace_sample_t *p = &samples[(first + i) % capacity];
    /* Map IDs are internal strings. Strip CSV control characters before exporting. */
    char map[GD_STR_LEN], next[GD_STR_LEN];
    for (unsigned j = 0; j < GD_STR_LEN; j++) {
      char a = p->map[j], b = p->next_map[j];
      map[j] = a == ',' || a == '\r' || a == '\n' || a == '"' ? '?' : a;
      next[j] = b == ',' || b == '\r' || b == '\n' || b == '"' ? '?' : b;
    }
    int n = snprintf(csv + used, sizeof(csv) - used,
      "%llu,%u,%s,%s,%#x,%#lx,%#lx,%#lx,%#lx,%#x,%#x,%d,%d,%d,%d,%d,"
      "%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,"
      "%#llx,%u,%#x,%#x,%#llx,%u,%#x,%.6g,%.6g,%u,%#x,%u,%#x,%#x,%.6g,%#lx,%#lx\n",
      (unsigned long long)p->tick, p->frame, map, next, p->state,
      (unsigned long)p->actor, (unsigned long)p->camera, (unsigned long)p->body, (unsigned long)p->pending,
      p->held, p->pressed, p->sticks[0], p->sticks[1], p->sticks[2], p->sticks[3], p->speed,
      p->feet.x, p->feet.y, p->feet.z, p->eye.x, p->eye.y, p->eye.z, p->target.x, p->target.y, p->target.z,
      p->velocity.x, p->velocity.y, p->velocity.z, (unsigned long long)p->body_flags,
      p->abyss.type, p->abyss.player_flags, p->abyss.contact_flags, (unsigned long long)p->abyss.contact_tags,
      p->abyss.mode, p->abyss.gate, p->abyss.gate_value, p->abyss.fall_distance,
      p->abyss.extended_valid, p->abyss.extended_flags, p->abyss.query_valid, p->abyss.query_flags,
      p->abyss.query_ready, p->abyss.query_distance,
      (unsigned long)p->abyss.query_flags_fn, (unsigned long)p->abyss.query_scalar_fn);
    if (n < 0 || (size_t)n >= sizeof(csv) - used)
      return false;
    used += (size_t)n;
  }
  return write_file(dir, name, csv, used);
}

static bool export_traces(const char *dir)
{
  unsigned oldest = (recent_head + TRACE_RECENT - recent_count) % TRACE_RECENT;
  bool ok = write_trace(dir, "flight-recorder.csv", recent, TRACE_RECENT, oldest, recent_count);
  append("flight-recorder.csv %s (%u samples)\n", ok ? "written" : "write failed", recent_count);
  for (unsigned i = 0; i < TRACE_FALLS && i < fall_head; i++) {
    unsigned slot = (fall_head - 1 - i) % TRACE_FALLS;
    const __typeof__(fall_traces[0]) *trace = &fall_traces[slot];
    const char *file = i == 0 ? "fall-latest.csv" : "fall-previous.csv";
    bool saved = write_trace(dir, file, trace->samples, TRACE_PRE + TRACE_POST, 0, trace->count);
    append("%s %s trigger tick=%llu, %u before and %u at/after\n", file,
           saved ? "written" : "write failed", (unsigned long long)trace->tick, trace->before,
           trace->count - trace->before);
    ok &= saved;
  }
  append("CSV state flags: feet=0x1 camera=0x2 controller=0x4 ground-valid=0x8 grounded=0x10"
         " no-hit=0x20 menu=0x40 native-ui=0x80 transition=0x100 fall-flow=0x200"
         " levitate-enabled=0x400 ghost=0x800 menu-pause=0x1000 abyss-valid=0x2000\n");
  return ok;
}

static uintptr_t sample_link_and_camera(vec3f_t pose[2])
{
  uintptr_t actor = link_actor();
  vec3f_t feet;
  if (actor && link_drawn_pos(&feet))
    append("Link actor=%#lx feet=%.4f %.4f %.4f\n", (unsigned long)actor, feet.x, feet.y, feet.z);
  else
    append("Link position unavailable (actor=%#lx)\n", (unsigned long)actor);
  double fx, fz;
  if (actor && link_facing(&fx, &fz))
    append("Link facing XZ=%.5f %.5f\n", fx, fz);
  uintptr_t cam = camera();
  /* Camera +0x38 eye, +0x44 look-at. Sample once for both report and binary. */
  if (cam && sys_mapped(cam + 0x38, 2 * sizeof(*pose), SYS_PERM_R)) {
    memcpy(pose, (const void *)(cam + 0x38), 2 * sizeof(*pose));
    append("Camera=%#lx eye=%.4f %.4f %.4f look-at=%.4f %.4f %.4f\n", (unsigned long)cam,
           pose[0].x, pose[0].y, pose[0].z, pose[1].x, pose[1].y, pose[1].z);
  } else {
    append("Camera pose unavailable\n");
    cam = 0;
  }
  append("Room, action, animation, camera owner and camera collision fields unavailable\n");
  return cam;
}

static void sample_controller(void)
{
  cct_snapshot_t cct;
  if (!cct_snapshot(&cct)) {
    append("Character controller unavailable\n");
    return;
  }
  append("Controller actor=%#lx property=%#lx body=%#lx pending=%#lx\n",
         (unsigned long)cct.actor, (unsigned long)cct.property, (unsigned long)cct.body, (unsigned long)cct.pending);
  append("Controller velocity XYZ=%.4f %.4f %.4f flags(+0x70)=%#llx effective-no-hit=%d\n",
         cct.velocity.x, cct.velocity.y, cct.velocity.z, (unsigned long long)cct.body_flags, cct.no_hit);
  bool grounded;
  if (cct_on_ground(cct.actor, &grounded))
    append("Controller grounded=%d\n", grounded);
  else
    append("Controller grounding unavailable\n");
}

static bool history_prefix(const char *line, const char *prefix)
{
  while (*prefix && *prefix == *line) {
    prefix++;
    line++;
  }
  return !*prefix;
}

/* Record the phases before a possible stall. The report remains useful even
 * when a blocked game thread cannot service a user-triggered capture. */
bool diagnostics_transition_frame(bool active)
{
  static char last_map[GD_STR_LEN], last_next[GD_STR_LEN];
  static bool last_active, waiting_logged;
  static unsigned poll;
  static uint64_t start;
  if (active == last_active && ++poll < 8)
    return false;
  poll = 0;
  const char *map = gd_get_str(GD_STR_MAP), *next = gd_get_str(GD_STR_NEXT_MAP);
  if (!map)
    map = "";
  if (!next)
    next = "";
  unsigned i = 0;
  while (i < GD_STR_LEN - 1 && map[i] && map[i] == last_map[i])
    i++;
  bool changed = map[i] != last_map[i];
  i = 0;
  while (i < GD_STR_LEN - 1 && next[i] && next[i] == last_next[i])
    i++;
  changed |= next[i] != last_next[i];
  uint64_t now = sys_tick();
  if (active && !last_active) {
    start = now;
    waiting_logged = false;
  }
  bool waiting = active && !waiting_logged && now - start > sys_tick_freq() * 5;
  if (!changed && active == last_active && !waiting)
    return false;
  snprintf(last_map, sizeof(last_map), "%.63s", map);
  snprintf(last_next, sizeof(last_next), "%.63s", next);
  log_printf("transition: %s frame=%u map=%.23s next=%.23s",
             waiting ? "waiting >5s" : active ? "active" : last_active ? "released" : "map state",
             frame_counter(), last_map, last_next);
  if (active || last_active) {
    uint32_t counts[4];
    if (pause_transition_counts(counts))
      log_printf("transition: requests load=%u scene=%u event=%u system=%u", counts[0], counts[1], counts[2], counts[3]);
  }
  waiting_logged |= waiting;
  last_active = active;
  return true;
}

static void sample_history(void)
{
  append("\nRecent physics and pause log entries (newest first)\n");
  int kept = 0;
  for (int i = 0; i < LOG_LINES && kept < HISTORY_LINES; i++) {
    const char *line = log_line(i);
    if (!line)
      break;
    if ((history_prefix(line, "levitate:") && !history_prefix(line, "levitate: testing")) ||
        history_prefix(line, "transition:") ||
        history_prefix(line, "walls:") || history_prefix(line, "menu pause:")) {
      append("%.*s\n", LOG_LINE_LEN, line);
      kept++;
    }
  }
}

void diagnostics_dump(bool deep)
{
  if (!z64hd_game)
    return;
  static unsigned serial;
  uint64_t tick = sys_tick();
  char dir[96];
  snprintf(dir, sizeof(dir), SD_DIR "/sloperina-devdump-%llx-%u", (unsigned long long)tick, ++serial);

  report_size = 0;
  append("SloperinaGZ diagnostics v2 (%s)\nMod=%s\nGame=%s build=%s main=%#lx\n",
         deep ? "deep" : "quick", PACKAGE_VERSION, z64hd_game->version,
         z64hd_game->build_id, (unsigned long)z64hd_main);
  append("Tick=%llu frequency=%llu frame=%u\n", (unsigned long long)tick,
         (unsigned long long)sys_tick_freq(), frame_counter());
  const char *map = gd_get_str(GD_STR_MAP), *next_map = gd_get_str(GD_STR_NEXT_MAP);
  append("Map=%.63s next-map field=%.63s\n", map ? map : "unavailable", next_map ? next_map : "unavailable");
  int32_t lx, ly, rx, ry;
  input_sticks(&lx, &ly, &rx, &ry);
  append("Menu capture input held=%#x pressed=%#x sticks=%d %d %d %d\n",
         input_held(), input_pressed(), (int)lx, (int)ly, (int)rx, (int)ry);
  unsigned cheat_bits = 0;
  for (int i = 0; i < CHEAT_COUNT; i++)
    cheat_bits |= (unsigned)cheats[i] << i;
  append("Levitate=%s enabled=%d ghost=%d ghost-default=%d preset=%d base-rise=%d cct-owned=%d cheats=%#x\n",
         levitate_state(), levitate_enabled, levitate_through_walls, levitate_ghost_default, levitate_move_speed,
         levitate_speed, cct_noclip_active(), cheat_bits);
  bool paused;
  int32_t requests;
  gd_game_paused(&paused, &requests);
  append("Native pause IsPause=%d GameData requests=%d (defaults to zero if unavailable)\n", paused, (int)requests);
  append("Menu pause owned=%d native transition pause=%d\n", pause_menu_owned(), pause_transition_active());
  uint32_t counts[4];
  const char *const names[4] = {
    "LoadingPause", "SceneTransition", "EventBancChangePause", "SystemPauseWhileLoading",
  };
  if (pause_transition_counts(counts)) {
    for (unsigned i = 0; i < 4; i++) {
      if (counts[i] == UINT32_MAX)
        append("PauseManager %s unavailable\n", names[i]);
      else
        append("PauseManager %s requests=%u\n", names[i], counts[i]);
    }
  } else {
    append("PauseManager entries unavailable\n");
  }
  vec3f_t pose[2];
  uintptr_t cam = sample_link_and_camera(pose);
  sample_controller();
  append("Contact void-out bypass available=%d enabled=%d\n", void_bypass_available(), void_bypass_enabled());
  char overlay[128];
  airwall_summary(overlay, sizeof(overlay));
  append("Placed overlays: %s\n", overlay);
  airwall_nearest_barrier(overlay, sizeof(overlay));
  append("Nearest placed barrier: %s\n", overlay);
  if (last_fall.frame) {
    append("Last observed DmF_SY_FallDown frame=%u map=%s\n", last_fall.frame, last_fall.map);
    if (last_fall.abyss_type)
      append("Native abyss type=%u\n", last_fall.abyss_type);
    append_abyss_inputs("First nonzero type", last_fall.decision_inputs);
    append_abyss_inputs("Previous frame", last_fall.before_inputs);
    append_abyss_inputs("First active frame", last_fall.active_inputs);
    if (last_fall.has_before)
      append("Previous feet XYZ=%.3f %.3f %.3f\n", last_fall.before.x, last_fall.before.y, last_fall.before.z);
    if (last_fall.has_position) {
      append("First active feet XYZ=%.3f %.3f %.3f\n", last_fall.first.x, last_fall.first.y, last_fall.first.z);
      append("Lowest active feet XYZ=%.3f %.3f %.3f\n", last_fall.lowest.x, last_fall.lowest.y, last_fall.lowest.z);
    }
  } else {
    append("Fall event sampling %s\n", z64hd_game->event_flow_layout ? "active; no fall flow observed" : "unavailable");
  }

  uint8_t record[72];
  size_t size = 0;
  uintptr_t base = deep ? levitate_record_snapshot(record, sizeof(record), &size) : 0;
  if (fs_create_dir(dir) != 0) {
    gfx_notice("diagnostics: could not create folder on SD");
    return;
  }
  bool partial = false;
  if (deep) {
    if (base && write_file(dir, "link-record.bin", record, size)) {
      append("link-record.bin base=%#lx bytes=%u, little endian\n", (unsigned long)base, (unsigned)size);
      append("Offsets 0x00/0x08/0x10: f64 XYZ (Y includes +0.585)\n");
      if (size > 24)
        append("Offsets 0x24/0x3c: f32 ZXY requested/collided velocity. Other motion bytes redacted.\n");
    } else {
      partial = base != 0;
      append("link-record.bin unavailable (%s)\n", partial ? "write failed" : "no verified current record");
    }
    if (cam && write_file(dir, "camera-pose.bin", pose, sizeof(pose))) {
      append("camera-pose.bin base=%#lx bytes=%u, little endian\n", (unsigned long)(cam + 0x38),
             (unsigned)sizeof(pose));
      append("Offset 0x00: f32 XYZ eye. Offset 0x0c: f32 XYZ look-at.\n");
    } else {
      partial |= cam != 0;
      append("camera-pose.bin unavailable (%s)\n", cam ? "write failed" : "no readable camera");
    }
    partial |= !export_traces(dir);
    sample_history();
  }
  if (!write_file(dir, "manifest.txt", report, report_size)) {
    gfx_notice("diagnostics: manifest write failed");
    return;
  }
  log_printf("diagnostics: %s", dir);
  gfx_notice(partial ? "diagnostics saved with file error" : "diagnostics saved to SD");
}
