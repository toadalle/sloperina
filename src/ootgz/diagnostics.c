/* Explicit, bounded captures on the game thread through the existing SD mount. */
#include "diagnostics.h"

#include <stdarg.h>

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

static struct {
  char map[GD_STR_LEN];
  unsigned frame;
  uint32_t abyss_type;
  vec3f_t before, first, lowest;
  bool has_before, has_position;
} last_fall;

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
  if (!z64hd_game || !z64hd_game->event_flow_layout)
    return false;
  bool active = fall_flow_active();
  vec3f_t feet;
  uintptr_t actor = link_actor();
  bool have_feet = actor && link_drawn_pos(&feet);
  if (active && !was_active) {
    const char *map = gd_get_str(GD_STR_MAP);
    snprintf(last_fall.map, sizeof(last_fall.map), "%.63s", map ? map : "");
    last_fall.frame = frame_counter();
    last_fall.abyss_type = 0;
    last_fall.has_before = had_previous && actor == previous_actor;
    last_fall.before = previous;
    last_fall.has_position = false;
    log_printf("fall: DmF_SY_FallDown active frame=%u map=%.30s", last_fall.frame, last_fall.map);
    if (last_fall.has_before)
      log_printf("fall: previous feet %.3f %.3f %.3f", previous.x, previous.y, previous.z);
  }
  if (active && actor && !last_fall.abyss_type) {
    /* SelectorEventQueryCheckPlayerAbyssType (main+0x2B1B620) reads
     * this field. Keep its raw value until the types have been mapped. */
    uintptr_t components = sys_ptr(actor + 0x270);
    uintptr_t player = components ? sys_ptr(components + 0x358) : 0;
    uint32_t type;
    if (player && sys_ptr(player + 0x18) == actor && sys_read32(player + 0x191C, &type) && type) {
      last_fall.abyss_type = type;
      log_printf("fall: native abyss type %u", type);
    }
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
  append("SloperinaGZ diagnostics v1 (%s)\nMod=%s\nGame=%s build=%s main=%#lx\n",
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
  if (last_fall.frame) {
    append("Last observed DmF_SY_FallDown frame=%u map=%s\n", last_fall.frame, last_fall.map);
    if (last_fall.abyss_type)
      append("Native abyss type=%u\n", last_fall.abyss_type);
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
    sample_history();
  }
  if (!write_file(dir, "manifest.txt", report, report_size)) {
    gfx_notice("diagnostics: manifest write failed");
    return;
  }
  log_printf("diagnostics: %s", dir);
  gfx_notice(partial ? "diagnostics saved with file error" : "diagnostics saved to SD");
}
