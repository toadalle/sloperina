/* Drawing the menu over the game's picture, through the game's own NVN.
 *
 * How the game uses NVN (main v131072): it gets nvnDeviceGetProcAddress from nvnBootstrapLoader and
 * then fetches every function by name (loader at main+0x129e000), keeping the pointers in globals. So
 * wrapping nvnBootstrapLoader (main's GOT, see got.c) and the GetProcAddress it returns lets us replace
 * any NVN function the game uses:
 *   nvnWindowBuilderSetTextures  remember the swapchain textures (called again when the window is
 *                                rebuilt, main+0x528288)
 *   nvnQueuePresentTexture       before the game's present, copy our panels into the texture being
 *                                presented
 *   nvnDeviceInitialize          remember the device
 *
 * Drawing uses no shaders (Switch 2 shader binaries can't be built with public tools): the text is
 * rasterised on the CPU into a buffer in our own memory pool, in the swapchain's format, and copied
 * into the texture with nvnCommandBufferCopyBufferToTexture. Two slots (pixel buffers + command
 * memory + sync) alternate, and a slot is only reused once the GPU has finished with it.
 *
 * Signatures and enum values are NVN 1's. Checked against this game: WindowBuilderSetTextures is
 * called as (builder, count, textures); TextureBuilderSetFormat gets 37 (RGBA8), 1, 13 and 61
 * (RGB10A2); MemoryPoolBuilderSetFlags gets 0x21, 0x12, 0x22, 0x24, 0xA1..., all NVN 1 values.
 */
#include "nvn.h"

#include "font.h"
#include "airwall.h"
#include "enemyhp.h"
#include "gfx.h"
#include "sys.h"

/* --- NVN types (opaque; sizes are NVN 1's, storage is oversized in case NVN 2 grew) ------- */

typedef struct nvn_device nvn_device_t;
typedef struct nvn_queue nvn_queue_t;
typedef struct nvn_window nvn_window_t;
typedef struct nvn_texture nvn_texture_t;
typedef uint64_t nvn_addr_t;
typedef uint64_t nvn_handle_t;
typedef struct { int32_t x, y, z, w, h, d; } nvn_region_t;
typedef struct { int32_t x, y, w, h; } nvn_rect_t;

#define OBJ_STORAGE 0x400 /* NVN 1: pool builder 0x40, pool 0x100, command buffer 0xA0, sync 0x40 */
typedef struct { _Alignas(16) uint8_t b[OBJ_STORAGE]; } nvn_obj_t;

enum {
  /* memory pool flags */
  POOL_CPU_UNCACHED = 0x02,
  POOL_CPU_CACHED = 0x04,
  POOL_GPU_UNCACHED = 0x10,
  POOL_GPU_CACHED = 0x20,
  /* sync condition */
  SYNC_ALL_GPU_COMMANDS_COMPLETE = 0,
  /* texture formats */
  FMT_RGBA8 = 37,
  FMT_RGBA16F = 41,
  FMT_RGBX8_SRGB = 55,
  FMT_RGBA8_SRGB = 56,
  FMT_RGB10A2 = 61,
};

/* nvnSyncWait's results */
enum {
  SYNC_ALREADY_SIGNALLED = 0,
  SYNC_CONDITION_SATISFIED = 1,
  SYNC_TIMEOUT_EXPIRED = 2,
  SYNC_FAILED = 3,
};
#define SYNC_TIMEOUT_NS 1000000000ull /* a second */

#define CLEAR_ALL_CHANNELS 0xF /* nvnCommandBufferClearColor's mask: R, G, B and A */
#define SCISSOR_ALL 65535      /* a scissor size that covers any texture */

typedef void *(*getproc_t)(nvn_device_t *dev, const char *name);
extern void *nvnBootstrapLoader(const char *name); /* from sdk */
void *aligned_alloc(size_t align, size_t size);     /* from main */
void free(void *p);                                  /* from main */

/* The NVN functions we use, filled in by name from fn_names (in the same order). */
static struct {
  void (*pool_builder_defaults)(nvn_obj_t *);
  void (*pool_builder_device)(nvn_obj_t *, nvn_device_t *);
  void (*pool_builder_flags)(nvn_obj_t *, int);
  void (*pool_builder_storage)(nvn_obj_t *, void *, size_t);
  bool (*pool_init)(nvn_obj_t *, const nvn_obj_t *);
  nvn_addr_t (*pool_address)(const nvn_obj_t *);
  bool (*cmd_init)(nvn_obj_t *, nvn_device_t *);
  void (*cmd_add_command)(nvn_obj_t *, const nvn_obj_t *pool, ptrdiff_t offset, size_t size);
  void (*cmd_add_control)(nvn_obj_t *, void *, size_t);
  void (*cmd_begin)(nvn_obj_t *);
  nvn_handle_t (*cmd_end)(nvn_obj_t *);
  void (*cmd_copy_to_texture)(nvn_obj_t *, nvn_addr_t, const nvn_texture_t *, const void *view,
                              const nvn_region_t *, int flags);
  void (*queue_submit)(nvn_queue_t *, int, const nvn_handle_t *);
  void (*queue_fence)(nvn_queue_t *, nvn_obj_t *sync, int condition, int flags);
  bool (*sync_init)(nvn_obj_t *, nvn_device_t *);
  int (*sync_wait)(const nvn_obj_t *, uint64_t timeout_ns);
  int (*tex_width)(const nvn_texture_t *);
  int (*tex_height)(const nvn_texture_t *);
  int (*tex_format)(const nvn_texture_t *);
  void (*window_crop)(const nvn_window_t *, nvn_rect_t *);
  void (*cmd_copy_to_buffer)(nvn_obj_t *, const nvn_texture_t *, const void *view, const nvn_region_t *,
                             nvn_addr_t, int flags);
  void (*pool_invalidate)(const nvn_obj_t *, ptrdiff_t offset, size_t size);
  void (*set_render_targets)(nvn_obj_t *, int n, nvn_texture_t *const *colors, const void *const *views,
                             const nvn_texture_t *depth, const void *depth_view);
  void (*set_scissor)(nvn_obj_t *, int x, int y, int w, int h);
  void (*clear_color)(nvn_obj_t *, int index, const float *rgba, int mask);
  int (*window_origin)(const nvn_device_t *);
  size_t (*cmd_free)(const nvn_obj_t *);
} fn;

static const char *const fn_names[] = {
  "nvnMemoryPoolBuilderSetDefaults", "nvnMemoryPoolBuilderSetDevice", "nvnMemoryPoolBuilderSetFlags",
  "nvnMemoryPoolBuilderSetStorage", "nvnMemoryPoolInitialize", "nvnMemoryPoolGetBufferAddress",
  "nvnCommandBufferInitialize", "nvnCommandBufferAddCommandMemory", "nvnCommandBufferAddControlMemory",
  "nvnCommandBufferBeginRecording", "nvnCommandBufferEndRecording",
  "nvnCommandBufferCopyBufferToTexture", "nvnQueueSubmitCommands", "nvnQueueFenceSync",
  "nvnSyncInitialize", "nvnSyncWait", "nvnTextureGetWidth", "nvnTextureGetHeight",
  "nvnTextureGetFormat", "nvnWindowGetCrop", "nvnCommandBufferCopyTextureToBuffer",
  "nvnMemoryPoolInvalidateMappedRange", "nvnCommandBufferSetRenderTargets", "nvnCommandBufferSetScissor",
  "nvnCommandBufferClearColor", "nvnDeviceGetWindowOriginMode", "nvnCommandBufferGetCommandMemoryFree",
};
#define FN_COUNT (sizeof(fn_names) / sizeof(*fn_names))
_Static_assert(FN_COUNT == sizeof(fn) / sizeof(void *), "fn table");

/* --- hooks ---------------------------------------------------------------------------------- */

static getproc_t real_getproc;
static void (*real_present)(nvn_queue_t *, nvn_window_t *, int);
static void (*real_set_textures)(void *builder, int n, nvn_texture_t *const *textures);
static bool (*real_device_init)(nvn_device_t *, const void *builder);

static nvn_device_t *device;
#define MAX_TEXTURES 8
static nvn_texture_t *swapchain_textures[MAX_TEXTURES];
static int swapchain_texture_count;

static bool names_equal(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static void draw_overlay(nvn_queue_t *queue, nvn_window_t *window, int index);

static void hook_present(nvn_queue_t *queue, nvn_window_t *window, int index)
{
  gfx_presents++;
  draw_overlay(queue, window, index);
  real_present(queue, window, index);
}

static void hook_set_textures(void *builder, int count, nvn_texture_t *const *textures)
{
  swapchain_texture_count = 0; /* none usable while the list is being rewritten */
  for (int i = 0; i < count && i < MAX_TEXTURES; i++)
    swapchain_textures[i] = textures[i];
  swapchain_texture_count = count < MAX_TEXTURES ? count : MAX_TEXTURES;
  real_set_textures(builder, count, textures);
}

static bool hook_device_init(nvn_device_t *dev, const void *builder)
{
  bool ok = real_device_init(dev, builder);
  if (ok)
    device = dev;
  return ok;
}

static void *hook_getproc(nvn_device_t *dev, const char *name);

/* Swap in our version of a function the game looks up. */
static void *wrap_proc(const char *name, void *proc)
{
  if (!proc)
    return proc;
  if (names_equal(name, "nvnDeviceGetProcAddress")) {
    real_getproc = (getproc_t)proc;
    return (void *)hook_getproc;
  }
  if (names_equal(name, "nvnQueuePresentTexture")) {
    real_present = proc;
    return (void *)hook_present;
  }
  if (names_equal(name, "nvnWindowBuilderSetTextures")) {
    real_set_textures = proc;
    return (void *)hook_set_textures;
  }
  if (names_equal(name, "nvnDeviceInitialize")) {
    real_device_init = proc;
    return (void *)hook_device_init;
  }
  return proc;
}

static void *hook_getproc(nvn_device_t *dev, const char *name)
{
  if (dev && !device)
    device = dev;
  return wrap_proc(name, real_getproc(dev, name));
}

void *nvn_bootstrap_hook(const char *name)
{
  return wrap_proc(name, nvnBootstrapLoader(name));
}

/* --- overlay ---------------------------------------------------------------------------------- */

#define SLOTS 2
#define CMD_SIZE 0x40000  /* command memory per slot (see-through: a copy per sampled row) */
#define CTRL_SIZE 0x4000  /* control memory per slot */
#define PAGE_SIZE 0x1000
#define RGBA8_BYTES 4     /* bytes per pixel of an RGBA8 picture (the see-through path's only format) */
#define MAX_BYTES_PER_PIXEL 8
#define MAX_SCALE 3
#define SCALE_STEP_HEIGHT 360 /* the automatic scale: one more per this many lines of picture */
#define CELL_H (FONT_H + 2)
/* the text's top in a panel: half a cell of margin (then 4 font pixels over the text and 6 under it,
 * the last line's gap + the margin); panels with yoff 1 have it in the middle (watches, log, input) */
#define TEXT_TOP (FONT_H / 2)
/* Scales are in sixteenths of a screen pixel per font pixel (SC_ONE = 1x), so the text can be 1.75x
 * or 2.5x; a font pixel then covers 1 or 2 (or 2 or 3) screen pixels, nearest neighbour. */
#define SC_ONE 16
#define PANEL_W(cols, sc) ((((cols) + 2) * FONT_W * (sc) + SC_ONE - 1) / SC_ONE)
#define PANEL_H(rows, sc) ((((rows) * CELL_H + FONT_H) * (sc) + SC_ONE - 1) / SC_ONE)
#define MARGIN_FONT_PX 8 /* panels keep this far (font pixels, scaled) from the picture's edges */
#define MIN_PANEL_COLS 16
#define MIN_MENU_ROWS 6
/* the panels' indices in gfx_screen_t */
#define PANEL_MENU 0
#define PANEL_HUD 1
/* the watches panel (1) is never wider/taller than this */
#define HUD_COLS 32
#define HUD_ROWS 20
#define LOG_COLS 48
#define INPUT_COLS 58 /* both sticks (10 + 10) and every button (36) */
#define PAD_COLS 21   /* the controller drawing */
#define PAD_ROWS 10
/* The menu panel goes under the HUD's D-pad / ocarina icon. From the HUD layout
 * (romfs UI/Layout2Archive/Common › PlayerStatus_00 › Pa_GuideCross_00 › PaGuideCross_00, a
 * 3840x2160 layout): the cluster ends 33.5% down the screen and starts 3.5% in from the left. */
#define MENU_X_PERMILLE 35
#define MENU_Y_PERMILLE 345

static enum { OV_NONE, OV_READY, OV_FAILED } overlay_state;
static nvn_obj_t cmd_buffer, pixel_pool, command_pool, slot_sync[SLOTS];
static uint8_t *pixel_mem, *command_mem, *control_mem;
/* Opaque: the menu panel changes only on input, so it has one buffer, rewritten after waiting
 * for the GPU; the other panels change every frame, so they have one buffer per slot. */
static const int buffers_per_panel[GFX_PANELS] = { 1, SLOTS, SLOTS, SLOTS };
static size_t pixel_offset[GFX_PANELS][SLOTS], pixel_buffer_size[GFX_PANELS];
static const int panel_max_cols[GFX_PANELS] = { GFX_COLS, HUD_COLS, LOG_COLS, INPUT_COLS };
static const int panel_max_rows[GFX_PANELS] = { GFX_ROWS, HUD_ROWS, GFX_LOG_ROWS, PAD_ROWS };

/* The most pixels a panel can cover at this scale. The input display is either one long row or the
 * controller (PAD_COLS x PAD_ROWS), never both at once: its buffers are sized for the larger. */
static size_t panel_area(int panel, int scale)
{
  int rows = panel == GFX_PANEL_INPUT ? 1 : panel_max_rows[panel];
  size_t area = (size_t)PANEL_W(panel_max_cols[panel], scale) * PANEL_H(rows, scale);
  if (panel == GFX_PANEL_INPUT) {
    size_t pad_area = (size_t)PANEL_W(PAD_COLS, scale) * PANEL_H(PAD_ROWS, scale);
    area = area > pad_area ? area : pad_area;
  }
  return area;
}

static bool slot_used[SLOTS];
/* what each opaque buffer holds, so it's only redrawn when that changes */
static uint32_t drawn_hash[GFX_PANELS][SLOTS];
static int drawn_w[GFX_PANELS][SLOTS], drawn_h[GFX_PANELS][SLOTS];
static unsigned frame_count;
static int swapchain_format, bytes_per_pixel;
static int alloc_scale; /* the scale the buffers were sized for (whole screen pixels per font pixel) */
static bool origin_lower_left; /* scissor rectangles count from the bottom (NVN's default) */
static bool warned_format;

static void overlay_fail(const char *why)
{
  log_printf("on-screen drawing off: %s", why);
  overlay_state = OV_FAILED;
}

static size_t round_page(size_t n)
{
  return (n + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
}

static bool make_pool(nvn_obj_t *pool, void *mem, size_t size, int flags)
{
  nvn_obj_t builder;
  memset(&builder, 0, sizeof(builder));
  fn.pool_builder_defaults(&builder);
  fn.pool_builder_device(&builder, device);
  fn.pool_builder_flags(&builder, flags);
  fn.pool_builder_storage(&builder, mem, size);
  return fn.pool_init(pool, &builder);
}

/* bytes per pixel of a swapchain format we can draw into; 0 for any other */
static int format_bpp(int format)
{
  switch (format) {
    case FMT_RGBA8:
    case FMT_RGBA8_SRGB:
    case FMT_RGBX8_SRGB:
    case FMT_RGB10A2:
      return 4;
    case FMT_RGBA16F:
      return 8;
    default:
      return 0;
  }
}

static bool is_rgba8(int format)
{
  return format == FMT_RGBA8 || format == FMT_RGBA8_SRGB || format == FMT_RGBX8_SRGB;
}

/* the automatic scale for a picture this tall: 1x at 360 lines, 2x at 720, 3x at 1080 and above */
static int auto_scale(int height)
{
  int scale = height / SCALE_STEP_HEIGHT;
  return scale < 1 ? 1 : scale > MAX_SCALE ? MAX_SCALE : scale;
}

static bool overlay_init(void)
{
  if (!real_getproc || !device)
    return false; /* not yet: try again next frame */
  void **fn_slots = (void **)&fn;
  for (unsigned i = 0; i < FN_COUNT; i++) {
    fn_slots[i] = real_getproc(device, fn_names[i]);
    if (!fn_slots[i]) {
      log_printf("NVN has no %s", fn_names[i]);
      overlay_fail("missing NVN function");
      return false;
    }
  }
  nvn_texture_t *first = swapchain_textures[0];
  int height = fn.tex_height(first);
  swapchain_format = fn.tex_format(first);
  bytes_per_pixel = format_bpp(swapchain_format);
  log_printf("swapchain: %d textures, %dx%d, format %d", swapchain_texture_count, fn.tex_width(first), height,
             swapchain_format);
  if (!bytes_per_pixel) {
    overlay_fail("unsupported swapchain format (see the log line above)");
    return false;
  }
  /* sized for the scale at this resolution; a larger picture later keeps this scale */
  alloc_scale = auto_scale(height);
  size_t total = 0;
  for (int p = 0; p < GFX_PANELS; p++) {
    pixel_buffer_size[p] = round_page(panel_area(p, alloc_scale * SC_ONE) * bytes_per_pixel);
    for (int b = 0; b < buffers_per_panel[p]; b++) {
      pixel_offset[p][b] = total;
      total += pixel_buffer_size[p];
    }
  }
  pixel_mem = aligned_alloc(PAGE_SIZE, total);
  command_mem = aligned_alloc(PAGE_SIZE, CMD_SIZE * SLOTS);
  control_mem = aligned_alloc(PAGE_SIZE, CTRL_SIZE * SLOTS);
  if (!pixel_mem || !command_mem || !control_mem) {
    overlay_fail("out of memory");
    return false;
  }
  if (!make_pool(&pixel_pool, pixel_mem, total, POOL_CPU_UNCACHED | POOL_GPU_UNCACHED) ||
      !make_pool(&command_pool, command_mem, CMD_SIZE * SLOTS, POOL_CPU_UNCACHED | POOL_GPU_CACHED)) {
    overlay_fail("memory pool");
    return false;
  }
  if (!fn.cmd_init(&cmd_buffer, device)) {
    overlay_fail("command buffer");
    return false;
  }
  for (int s = 0; s < SLOTS; s++)
    if (!fn.sync_init(&slot_sync[s], device)) {
      overlay_fail("sync");
      return false;
    }
  origin_lower_left = fn.window_origin(device) == 0;
  log_printf("on-screen drawing ready (scale %d, %u KiB of panels, origin %s)", alloc_scale,
             (unsigned)(total / 1024), origin_lower_left ? "lower left" : "upper left");
  overlay_state = OV_READY;
  return true;
}

/* --- see-through background (opacity < 100) ------------------------------------------------------
 * A copy can't blend, so: each frame the region under a panel is also copied out of the picture
 * (texture to buffer, before the panel goes in), and two frames later, once that slot's fence has
 * passed, the CPU blends the panel's background with it. The background therefore lags the game
 * by two frames. To keep the memory small, only one row per text-pixel row (every `scale`th row)
 * is read back, and the text mask is kept at font resolution. Only for RGBA8 pictures; allocated
 * the first time it's used, in separate pieces (the game's heap had no room for one big block). */

static enum { ST_NONE, ST_READY, ST_FAILED } see_through_state;
/* what was under each panel, read back from the picture, per slot */
static nvn_obj_t readback_pool[GFX_PANELS][SLOTS];
static uint8_t *readback_mem[GFX_PANELS][SLOTS];
static size_t readback_size[GFX_PANELS];
static nvn_region_t readback_region[GFX_PANELS][SLOTS]; /* what each readback holds */
/* each panel's text as PAL_ indices at font resolution, and what it was built from */
static uint8_t *text_mask[GFX_PANELS];
static size_t text_mask_size[GFX_PANELS];
static uint32_t text_mask_hash[GFX_PANELS];
static int text_mask_cols[GFX_PANELS], text_mask_rows[GFX_PANELS];
#define MASK_W(cols) (((cols) + 2) * FONT_W) /* font pixels, unscaled */
#define MASK_H(rows) ((rows) * CELL_H + FONT_H)
#define MASK_ALIGN 64

static void see_through_free(void)
{
  for (int p = 0; p < GFX_PANELS; p++) {
    free(text_mask[p]);
    text_mask[p] = 0;
    for (int b = 0; b < SLOTS; b++) {
      free(readback_mem[p][b]);
      readback_mem[p][b] = 0;
    }
  }
}

/* Only the optional input monitor uses a see-through backdrop. */
static bool see_through_init(void)
{
  int p = GFX_PANEL_INPUT;
  int max_w = PANEL_W(panel_max_cols[p], alloc_scale * SC_ONE);
  size_t total = 0;
  bool ok;
  readback_size[p] = round_page(panel_area(p, alloc_scale * SC_ONE) / alloc_scale * RGBA8_BYTES + max_w * 8);
  text_mask_size[p] = (size_t)MASK_W(panel_max_cols[p]) * MASK_H(panel_max_rows[p]);
  text_mask[p] = aligned_alloc(MASK_ALIGN, text_mask_size[p]);
  ok = text_mask[p] != 0;
  for (int b = 0; b < SLOTS && ok; b++) {
    readback_mem[p][b] = aligned_alloc(PAGE_SIZE, readback_size[p]);
    ok = readback_mem[p][b] != 0 && make_pool(&readback_pool[p][b], readback_mem[p][b], readback_size[p],
                                              POOL_CPU_CACHED | POOL_GPU_UNCACHED);
    total += readback_size[p];
  }
  total += text_mask_size[p];
  if (!ok) {
    log_printf("see-through background off: not enough memory (needs %u KiB)", (unsigned)(total / 1024));
    gfx_notice("see-through background: the game has no memory free for it");
    see_through_free();
    see_through_state = ST_FAILED;
    return false;
  }
  log_printf("see-through background ready (%u KiB)", (unsigned)(total / 1024));
  see_through_state = ST_READY;
  return true;
}

static uint8_t *panel_buffer_ptr(int p, int b)
{
  return pixel_mem + pixel_offset[p][b];
}

static nvn_addr_t panel_buffer_addr(int p, int b)
{
  return fn.pool_address(&pixel_pool) + pixel_offset[p][b];
}

/* --- drawing ------------------------------------------------------------------------------------- */

/* colours (sRGB 0..255) */
static const uint8_t style_rgb[GFX_NSTYLES][3] = {
  [GFX_NORMAL] = { 0xFF, 0xFF, 0xFF },
  [GFX_SELECTED] = { 0x80, 0xFF, 0x80 },
  [GFX_TITLE] = { 0xA0, 0xC8, 0xFF },
  [GFX_DIM] = { 0xA3, 0xB1, 0xC3 },
  [GFX_NOTICE] = { 0xFF, 0xE0, 0x60 },
  [GFX_LOG] = { 0xC0, 0xC0, 0xC0 },   /* gz's log colour */
  [GFX_FAINT] = { 0x50, 0x50, 0x58 }, /* the controller's buttons not held */
};
static const uint8_t bg_rgb[3] = { 0x10, 0x17, 0x22 };

/* value / 255 as an IEEE half-precision float (0..1), from the single-precision bits */
static uint16_t to_half_float(uint8_t value)
{
  if (!value)
    return 0;
  float f = value / 255.0f;
  uint32_t bits;
  memcpy(&bits, &f, 4);
  int exponent = (int)((bits >> 23) & 0xFF) - 127 + 15; /* rebiased from float's to half's */
  return (uint16_t)(exponent << 10 | ((bits >> 13) & 0x3FF));
}

/* an opaque colour in the swapchain's format */
static uint64_t encode_pixel(const uint8_t rgb[3])
{
  switch (swapchain_format) {
    case FMT_RGB10A2:
      return (uint32_t)(rgb[0] * 1023 / 255) | (uint32_t)(rgb[1] * 1023 / 255) << 10 |
             (uint32_t)(rgb[2] * 1023 / 255) << 20 | 3u << 30;
    case FMT_RGBA16F: /* 0x3C00 is 1.0 */
      return to_half_float(rgb[0]) | (uint64_t)to_half_float(rgb[1]) << 16 | (uint64_t)to_half_float(rgb[2]) << 32 |
             (uint64_t)0x3C00 << 48;
    default:
      return rgb[0] | rgb[1] << 8 | rgb[2] << 16 | 0xFFu << 24;
  }
}

/* Palette of a panel pixel: 0 background, 1 + style for text, then gz's checkbox colours. */
enum {
  PAL_TEXT = 1,
  PAL_OUTLINE = PAL_TEXT + GFX_NSTYLES,
  PAL_BEVEL_HI,
  PAL_BEVEL_LO,
  PAL_TICK,
  PAL_REMOVE,
  PAL_REMOVE_HOT,
  PAL_HOT_LIGHT,
  PAL_FOLDER,                  /* + 0..3: the folder's yellows */
  PAL_SHADOW = PAL_FOLDER + 4, /* the log's drop shadow */
  PAL_FILL,                    /* + style: the box's inside, half the style colour */
  PAL_CARD = PAL_FILL + GFX_NSTYLES,
  PAL_CARD_SELECTED,
  PAL_CARD_BORDER,
  PAL_CARD_ACCENT,
  PAL_COUNT
};

/* Icons are 10x10 tiles, one string per row. */
#define ICON_SIZE 10
typedef char icon_row_t[ICON_SIZE + 1];

/* gz's checkbox.png: the box (tile 0) and the tick drawn over it (tile 2). '#' outline, 'c'/'b' the
 * inner edge (light, dark), 'w' the inside, 'G' the tick. */
static const icon_row_t check_box[ICON_SIZE] = {
  "..........", "########..", "#cccccc#..", "#cwwwwb#..", "#cwwwwb#..",
  "#cwwwwb#..", "#cwwwwb#..", "#cbbbbb#..", "########..", "..........",
};
static const icon_row_t check_tick[ICON_SIZE] = {
  "........GG", ".......GG.", "......GG..", ".....GG...", "GG...GG...",
  ".GG.GG....", "..GGGG....", "...GG.....", "....G.....", "..........",
};
/* list_icons.png tile 1 (gz draws it white tinted red; its soft edges made solid) and file_icons.png
 * tile 1, 'C'..'F' its yellows. Rows as the checkbox's: row 1 is the glyph's top. */
static const icon_row_t icon_remove[ICON_SIZE] = {
  "........", "XX....XX", "XXX..XXX", ".XXXXXX.", "..XXXX..",
  "..XXXX..", ".XXXXXX.", "XXX..XXX", "XX....XX", "........",
};
static const icon_row_t icon_wrench[ICON_SIZE] = { /* wrench.png, its soft edges made solid */
  "........", "....XX..", "....X...", "...XX..X", "...XXXX.",
  "..XXX...", ".XXX....", "XXX.....", "XX......", "........",
};
static const icon_row_t icon_up[ICON_SIZE] = { /* arrow_icons.png tiles 0 and 1 */
  "........", "........", "...XX...", "..XXXX..", ".XX..XX.", ".X....X.", "X......X", "X......X",
  "........", "........",
};
static const icon_row_t icon_down[ICON_SIZE] = {
  "........", "........", "X......X", "X......X", ".X....X.", ".XX..XX.", "..XXXX..", "...XX...",
  "........", "........",
};
static const icon_row_t icon_add[ICON_SIZE] = { /* '#' the outline, 'X' the plus (gz tints it green) */
  "........", "..####..", "..#XX#..", "###XX###", "#XXXXXX#", "#XXXXXX#", "###XX###", "..#XX#..",
  "..####..", "........",
};
static const icon_row_t icon_right[ICON_SIZE] = { /* arrow_icons.png tiles 2 and 3 */
  "........", ".XX.....", "...XX...", "....XX..", ".....XX.", ".....XX.", "....XX..", "...XX...", ".XX.....",
  "........",
};
static const icon_row_t icon_left[ICON_SIZE] = {
  "........", ".....XX.", "...XX...", "..XX....", ".XX.....", ".XX.....", "..XX....", "...XX...", ".....XX.",
  "........",
};
static const icon_row_t icon_folder[ICON_SIZE] = {
  ".........", ".CCC.....", "CDDDC....", "EEEEEEEE.", "EFFFFFFFE",
  "EFFFFFFFE", "EFFFFFFFE", "EEEEEEEEE", ".........", ".........",
};

/* The one-colour, one-cell icons (the wrench and the arrows); NULL for any other character. */
static const icon_row_t *plain_icon(unsigned char c)
{
  switch (c) {
    case GFX_ICON_WRENCH: return icon_wrench;
    case GFX_ICON_UP: return icon_up;
    case GFX_ICON_DOWN: return icon_down;
    case GFX_ICON_LEFT: return icon_left;
    case GFX_ICON_RIGHT: return icon_right;
    default: return 0;
  }
}

/* A pixel (PAL_ index) of icon c: sprite_x across its cells, tile_row its tile row. */
static uint8_t sprite_pixel(unsigned char c, int sprite_x, int tile_row, int style)
{
  if (sprite_x < 0 || tile_row < 0 || tile_row >= ICON_SIZE)
    return 0;
  bool selected = style == GFX_SELECTED;
  if (c == GFX_ICON_REMOVE) {
    if (sprite_x < FONT_W && icon_remove[tile_row][sprite_x] == 'X')
      return selected ? PAL_REMOVE_HOT : PAL_REMOVE;
    return 0;
  }
  if (c == GFX_ICON_ADD) {
    char k = sprite_x < FONT_W ? icon_add[tile_row][sprite_x] : '.';
    if (k == '#')
      return PAL_OUTLINE;
    if (k == 'X')
      return selected ? PAL_REMOVE_HOT : PAL_TICK;
    return 0;
  }
  const icon_row_t *plain = plain_icon(c);
  if (plain) {
    /* in the style's colour (white in the menu, faint on the controller), blue when selected */
    if (sprite_x < FONT_W && plain[tile_row][sprite_x] == 'X')
      return selected ? PAL_REMOVE_HOT : PAL_TEXT + style;
    return 0;
  }
  if (sprite_x >= ICON_SIZE)
    return 0;
  if (c == GFX_ICON_FOLDER) { /* selected: in the highlight colour */
    char k = icon_folder[tile_row][sprite_x];
    if (k < 'C' || k > 'F')
      return 0;
    if (selected)
      return k == 'E' ? PAL_REMOVE_HOT : PAL_HOT_LIGHT;
    return (uint8_t)(PAL_FOLDER + (k - 'C'));
  }
  if (c == GFX_CHECK_ON && check_tick[tile_row][sprite_x] == 'G') /* the tick: blue when selected */
    return selected ? PAL_REMOVE_HOT : PAL_TICK;
  switch (check_box[tile_row][sprite_x]) {
    case '#': return PAL_OUTLINE;
    case 'c': return PAL_BEVEL_HI;
    case 'b': return PAL_BEVEL_LO;
    case 'w': return (uint8_t)(PAL_FILL + style);
    default: return 0;
  }
}

static inline bool is_sprite(unsigned char c)
{
  switch (c) {
    case GFX_CHECK_OFF:
    case GFX_CHECK_ON:
    case GFX_ICON_REMOVE:
    case GFX_ICON_FOLDER:
    case GFX_ICON_WRENCH:
    case GFX_ICON_UP:
    case GFX_ICON_DOWN:
    case GFX_ICON_ADD:
    case GFX_ICON_LEFT:
    case GFX_ICON_RIGHT:
      return true;
    default:
      return false;
  }
}

/* The input display's controller drawing (a Switch 2 Pro Controller from above), in font pixels from
 * the text's top-left: the body's outline (a bar with rounded corners and a grip under each end), the
 * sticks (a ring with a dot where the stick is, filled while clicked in) and the d-pad (a cross, an arm
 * filled while held). The other buttons are text (watches.c). Rows of 10, columns of 8: the body
 * takes rows 1..8 of 20 columns, the readouts row 9. */
#define PAD_W 160
/* the body: a bar with rounded corners, the top ones rounder */
#define PAD_BODY_LEFT 4
#define PAD_BODY_RIGHT 156
#define PAD_BODY_TOP 12
#define PAD_BODY_BOTTOM 70
#define PAD_BODY_MIDDLE 41 /* rows above it have the top corners' radius, the rest the bottom's */
#define PAD_TOP_RADIUS 14
#define PAD_BOTTOM_RADIUS 10
/* the grips: circles centred on the body's bottom edge under each end */
#define PAD_GRIP_LEFT_X 36
#define PAD_GRIP_RIGHT_X 124
#define PAD_GRIP_Y 70
#define PAD_GRIP_RADIUS 18
static const struct { int cx, cy; } pad_sticks[2] = { { 36, 36 }, { 96, 56 } };
#define PAD_STICK_R 10
#define PAD_STICK_TRAVEL (PAD_STICK_R - 3) /* how far the dot moves at full tilt */
#define PAD_STICK_DOT_R2 5                 /* the dot's radius, squared */
#define DPAD_X 64
#define DPAD_Y 56
#define DPAD_ARM 9  /* from the centre to an arm's end */
#define DPAD_HALF 3 /* half an arm's width */
/* gfx_panel_t.clicks bits */
enum { CLICK_L3, CLICK_R3, CLICK_DPAD_UP, CLICK_DPAD_DOWN, CLICK_DPAD_LEFT, CLICK_DPAD_RIGHT };

static bool pad_inside(int x, int y)
{
  if (x >= PAD_BODY_LEFT && x <= PAD_BODY_RIGHT && y >= PAD_BODY_TOP && y <= PAD_BODY_BOTTOM) {
    bool top_half = y < PAD_BODY_MIDDLE;
    int radius = top_half ? PAD_TOP_RADIUS : PAD_BOTTOM_RADIUS;
    int corner_y = top_half ? PAD_BODY_TOP + radius : PAD_BODY_BOTTOM - radius;
    int corner_x = x < PAD_BODY_LEFT + radius    ? PAD_BODY_LEFT + radius
                   : x > PAD_BODY_RIGHT - radius ? PAD_BODY_RIGHT - radius
                                                 : x;
    /* between the corners, across or down */
    if (corner_x == x || (y >= PAD_BODY_TOP + PAD_TOP_RADIUS && y <= PAD_BODY_BOTTOM - PAD_BOTTOM_RADIUS))
      return true;
    int dx = x - corner_x;
    int dy = y - corner_y;
    return dx * dx + dy * dy <= radius * radius;
  }
  int left_dx = x - PAD_GRIP_LEFT_X;
  int right_dx = x - PAD_GRIP_RIGHT_X;
  int grip_dy = y - PAD_GRIP_Y;
  int left_dist2 = left_dx * left_dx + grip_dy * grip_dy;
  int right_dist2 = right_dx * right_dx + grip_dy * grip_dy;
  return left_dist2 <= PAD_GRIP_RADIUS * PAD_GRIP_RADIUS || right_dist2 <= PAD_GRIP_RADIUS * PAD_GRIP_RADIUS;
}

static bool dpad_inside(int dx, int dy)
{
  int ax = dx < 0 ? -dx : dx;
  int ay = dy < 0 ? -dy : dy;
  return (ax <= DPAD_HALF && ay <= DPAD_ARM) || (ay <= DPAD_HALF && ax <= DPAD_ARM);
}

/* The controller drawing's PAL_ index at font pixel (x, y); 0 where it draws nothing. */
static uint8_t pad_pixel(const gfx_panel_t *panel, int x, int y)
{
  for (int k = 0; k < 2; k++) { /* the left stick, then the right */
    int dx = x - pad_sticks[k].cx;
    int dy = y - pad_sticks[k].cy;
    int dist2 = dx * dx + dy * dy;
    if (dist2 > PAD_STICK_R * PAD_STICK_R)
      continue;
    int stick_x = pad_sticks[k].cx + panel->sticks[2 * k] * PAD_STICK_TRAVEL / 100;
    int stick_y = pad_sticks[k].cy - panel->sticks[2 * k + 1] * PAD_STICK_TRAVEL / 100;
    int ex = x - stick_x;
    int ey = y - stick_y;
    if (ex * ex + ey * ey <= PAD_STICK_DOT_R2)
      return PAL_TEXT + GFX_NORMAL; /* where the stick is */
    if (dist2 >= (PAD_STICK_R - 1) * (PAD_STICK_R - 1))
      return PAL_TEXT + GFX_DIM; /* its ring */
    return panel->clicks >> (CLICK_L3 + k) & 1 ? PAL_TEXT + GFX_FAINT : 0; /* clicked in */
  }
  int dx = x - DPAD_X;
  int dy = y - DPAD_Y;
  if (dpad_inside(dx, dy)) {
    if (!dpad_inside(dx - 1, dy) || !dpad_inside(dx + 1, dy) || !dpad_inside(dx, dy - 1) || !dpad_inside(dx, dy + 1))
      return PAL_TEXT + GFX_DIM; /* its edge */
    int arm = dy < -DPAD_HALF  ? CLICK_DPAD_UP
              : dy > DPAD_HALF ? CLICK_DPAD_DOWN
              : dx < -DPAD_HALF ? CLICK_DPAD_LEFT
              : dx > DPAD_HALF ? CLICK_DPAD_RIGHT
                               : -1; /* the centre */
    return arm >= 0 && (panel->clicks >> arm & 1) ? PAL_TEXT + GFX_NORMAL : 0;
  }
  if (x < 0 || x > PAD_W || !pad_inside(x, y))
    return 0;
  if (!pad_inside(x - 1, y) || !pad_inside(x + 1, y) || !pad_inside(x, y - 1) || !pad_inside(x, y + 1))
    return PAL_TEXT + GFX_FAINT; /* the outline */
  return 0;
}

/* whether character c of the font has a pixel at (glyph_x, glyph_y) */
static inline bool font_pixel(unsigned char c, int glyph_x, int glyph_y)
{
  return c >= FONT_FIRST && c < FONT_FIRST + FONT_COUNT && (font_8x8[c - FONT_FIRST][glyph_y] >> (7 - glyph_x) & 1);
}

static inline int style_at(const gfx_panel_t *panel, int row, int col)
{
  return panel->style[row][col] < GFX_NSTYLES ? panel->style[row][col] : 0;
}

/* What's at pixel (x, y) of a panel (a PAL_ index). Rows of 8x8 glyphs plus 2 px spacing, a cell of
 * margin left and right and half a cell above and below, scaled. A checkbox is 10 px tall: its
 * top row is in the spacing below the row above. */
static inline uint8_t panel_pixel(const gfx_panel_t *panel, int x, int y, int scale)
{
  int font_y = y * SC_ONE / scale - TEXT_TOP - panel->yoff;
  int font_x = x * SC_ONE / scale - FONT_W;
  if (font_y < 0 || font_x < 0)
    return 0;
  int row = font_y / CELL_H;
  int glyph_y = font_y % CELL_H;
  int col = font_x / FONT_W;
  int glyph_x = font_x % FONT_W;
  if (row >= GFX_ROWS || col >= GFX_COLS)
    return 0;
  if (panel->pad) { /* the controller drawing, under the buttons' text */
    unsigned char c = (unsigned char)panel->ch[row][col];
    bool glyph = glyph_y < FONT_H && font_pixel(c, glyph_x, glyph_y);
    if (!glyph && !is_sprite(c)) {
      uint8_t k = pad_pixel(panel, font_x, font_y);
      if (k)
        return k;
    }
  }
  /* an icon in this row, or the top of a checkbox in the row below */
  for (int below = 0; below <= 1; below++) {
    int r = row + below;
    int tile_row = below ? 0 : glyph_y + 1;
    if (r >= GFX_ROWS || (below && glyph_y != CELL_H - 1))
      break;
    unsigned char c = (unsigned char)panel->ch[r][col];
    int sprite_col = col;
    int sprite_x = glyph_x;
    if (c == GFX_WIDE_REST && col > 0) { /* the right half of a two-cell icon */
      sprite_col = col - 1;
      sprite_x = glyph_x + FONT_W;
      c = (unsigned char)panel->ch[r][sprite_col];
    }
    if (is_sprite(c)) {
      uint8_t k = sprite_pixel(c, sprite_x, tile_row, style_at(panel, r, sprite_col));
      if (k || !below)
        return k;
    }
  }
  if (glyph_y >= FONT_H)
    return 0;
  if (!font_pixel((unsigned char)panel->ch[row][col], glyph_x, glyph_y))
    return 0;
  return PAL_TEXT + style_at(panel, row, col);
}

/* the colours of the PAL_ indices (alphas from checkbox.png, blended over the background) */
static void build_palette(uint8_t pal[PAL_COUNT][3])
{
  memcpy(pal[0], bg_rgb, 3);
  for (int style = 0; style < GFX_NSTYLES; style++) {
    memcpy(pal[PAL_TEXT + style], style_rgb[style], 3);
    for (int c = 0; c < 3; c++)
      pal[PAL_FILL + style][c] = (uint8_t)((style_rgb[style][c] * 0x80 + bg_rgb[c] * 0x7F) / 255);
  }
  for (int c = 0; c < 3; c++) {
    pal[PAL_OUTLINE][c] = 0;
    pal[PAL_BEVEL_HI][c] = (uint8_t)((0x1D * 0xE5 + bg_rgb[c] * 0x1A) / 255);
    pal[PAL_BEVEL_LO][c] = (uint8_t)((0x0F * 0xF1 + bg_rgb[c] * 0x0E) / 255);
    pal[PAL_TICK][c] = c == 1 ? 0xFF : 0;
    pal[PAL_REMOVE][c] = c == 0 ? 0xFF : 0;
    /* gz's highlight colour is 0x2020FF; lightened, as on our dark background that was hard to see */
    pal[PAL_REMOVE_HOT][c] = c == 2 ? 0xFF : 0x58;
    pal[PAL_HOT_LIGHT][c] = c == 2 ? 0xFF : 0x98;
  }
  static const uint8_t folder[4][3] = {
    { 247, 209, 57 }, { 250, 237, 122 }, { 255, 227, 91 }, { 255, 255, 136 },
  };
  memcpy(pal[PAL_FOLDER], folder, sizeof(folder));
  memset(pal[PAL_SHADOW], 0, 3);
  static const uint8_t card_colours[][3] = {
    { 0x19, 0x25, 0x35 }, { 0x21, 0x35, 0x4D }, { 0x36, 0x4A, 0x62 }, { 0x82, 0xBD, 0xF6 },
  };
  memcpy(pal[PAL_CARD], card_colours, sizeof(card_colours));
}

static uint8_t dashboard_background(const gfx_panel_t *panel, int x, int y)
{
  int card_x = x - (GFX_CARD_COL + 1) * FONT_W;
  int card_y = y - (TEXT_TOP + GFX_CARD_ROW * CELL_H);
  if (card_x < 0 || card_y < 0)
    return 0;
  int row = card_y / (GFX_CARD_PITCH_Y * CELL_H);
  int inside_x = card_x;
  int inside_y = card_y % (GFX_CARD_PITCH_Y * CELL_H);
  int width = GFX_CARD_WIDTH * FONT_W;
  int height = GFX_CARD_PITCH_Y * CELL_H - 1;
  if (row >= 8 || inside_x >= width || inside_y >= height)
    return 0;
  bool selected = panel->dashboard == 1 + row;
  if ((inside_x == 0 || inside_x == width - 1) && (inside_y == 0 || inside_y == height - 1))
    return 0; /* clipped corners */
  if (inside_x == 0 || inside_x == width - 1 || inside_y == 0 || inside_y == height - 1)
    return selected ? PAL_CARD_ACCENT : PAL_CARD_BORDER;
  return selected ? PAL_CARD_SELECTED : PAL_CARD;
}

static uint8_t menu_row_background(const gfx_panel_t *panel, int x, int y)
{
  if (x < 2 * FONT_W || x >= GFX_COLS * FONT_W || y < TEXT_TOP)
    return 0;
  int row = (y - TEXT_TOP) / CELL_H;
  if (row >= GFX_ROWS || !(panel->menu_rows & (1u << row)))
    return 0;
  if (!(panel->menu_focus & (1u << row)))
    return PAL_CARD;
  return x < 2 * FONT_W + 2 ? PAL_CARD_ACCENT : PAL_CARD_SELECTED;
}

/* Opaque: write the panel straight into a buffer in the picture's format. Rows are built in a
 * cached line buffer and then copied to the (uncached) pool memory. */
#define MAX_PANEL_W PANEL_W(GFX_COLS, MAX_SCALE * SC_ONE)
static _Alignas(16) uint8_t row_buffer[MAX_PANEL_W * MAX_BYTES_PER_PIXEL];
static int screen_to_font_x[MAX_PANEL_W];
static const uint32_t *frozen_row(int x, int y);
static inline uint32_t blend_rgba8(uint32_t over, uint32_t under, unsigned alpha);

static void rasterise(const gfx_panel_t *panel, uint8_t *dst, int scale, const nvn_region_t *region,
                      bool frozen, int opacity)
{
  int w = region->w, h = region->h;
  uint8_t pal[PAL_COUNT][3];
  uint64_t encoded[PAL_COUNT];
  build_palette(pal);
  for (int i = 0; i < PAL_COUNT; i++)
    encoded[i] = encode_pixel(pal[i]);
  for (int y = 0; y < h; y++) {
    const uint32_t *under = frozen ? frozen_row(region->x, region->y + y) : 0;
    for (int x = 0; x < w; x++) {
      uint8_t pixel = panel_pixel(panel, x, y, scale);
      bool text = pixel != 0;
      if (!pixel && panel->fullscreen) {
        int fx = x * SC_ONE / scale, fy = y * SC_ONE / scale;
        pixel = panel->dashboard ? dashboard_background(panel, fx, fy) : menu_row_background(panel, fx, fy);
      }
      if (!pixel && panel->mini_hud) {
        int fx = x * SC_ONE / scale;
        int fy = y * SC_ONE / scale - TEXT_TOP;
        if (fx < 2 || y == 0)
          pixel = PAL_CARD_ACCENT;
        else if (fy >= 0 && fy / CELL_H < panel->rows && (fy / CELL_H & 1))
          pixel = PAL_CARD;
      }
      uint64_t colour = encoded[pixel];
      if (bytes_per_pixel == 8) {
        memcpy(row_buffer + x * 8, &colour, 8);
      } else {
        uint32_t colour32 = (uint32_t)colour;
        if (under && !text && pixel != PAL_CARD_ACCENT && pixel != PAL_CARD_BORDER) {
          int alpha = opacity;
          if (pixel == PAL_CARD || pixel == PAL_CARD_SELECTED)
            alpha += (100 - alpha) / (pixel == PAL_CARD_SELECTED ? 2 : 4);
          colour32 = blend_rgba8(colour32, under[x], (unsigned)alpha * 255 / 100);
        }
        memcpy(row_buffer + x * 4, &colour32, 4);
      }
    }
    memcpy(dst + (size_t)y * w * bytes_per_pixel, row_buffer, (size_t)w * bytes_per_pixel);
  }
}

/* A panel at font resolution, a PAL_ index per pixel. */
static void fill_mask(uint8_t *mask, const gfx_panel_t *panel, int width, int height)
{
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++)
      mask[(size_t)y * width + x] = panel_pixel(panel, x, y, SC_ONE);
}

/* gz's drop shadow, one pixel down and right of the text (bottom-up, so a shadow casts no shadow) */
static void add_drop_shadow(uint8_t *mask, int width, int height)
{
  for (int y = height - 1; y > 0; y--)
    for (int x = width - 1; x > 0; x--) {
      uint8_t *here = &mask[(size_t)y * width + x];
      uint8_t up_left = mask[(size_t)(y - 1) * width + x - 1];
      if (!*here && up_left && up_left != PAL_SHADOW)
        *here = PAL_SHADOW;
    }
}

/* See-through: the text as a byte mask at font resolution (cached; rebuilt when it changes) ... */
static void build_text_mask(int p, const gfx_panel_t *panel, int cols, int rows)
{
  int width = MASK_W(cols);
  int height = MASK_H(rows);
  if ((size_t)width * height > text_mask_size[p])
    return;
  fill_mask(text_mask[p], panel, width, height);
  if (panel->clear) /* no background (log, input display): it gets a shadow */
    add_drop_shadow(text_mask[p], width, height);
  text_mask_hash[p] = panel->hash;
  text_mask_cols[p] = cols;
  text_mask_rows[p] = rows;
}

/* the text row that font pixel row font_y (from the panel's top) is in */
static int text_row_at(int font_y, int yoff)
{
  int text_y = font_y - TEXT_TOP - yoff;
  return text_y < 0 ? 0 : text_y / CELL_H;
}

/* --- panels with no background (the log; the input display without its background) ------------
 * Only their pixels are drawn, as cleared rectangles straight into the picture (as the air walls are):
 * a run of one colour in a font-pixel row is a rectangle, and runs repeated in the rows under it (in the
 * same text row) grow it. Nothing around the text is touched, so nothing behind it lags or sticks. A
 * clear can't be see-through, so a fading log line darkens instead: its shadow goes in the first half,
 * and the text stops being drawn before it's dark enough to show as a black smudge. */
#define FADE_SHADOW_GONE 160 /* opacity (0..255) under which the shadow isn't drawn */
#define FADE_TEXT_GONE 64    /* ... and the text */
#define CLEAR_PANELS 2       /* the log and the input display (GFX_PANEL_LOG onwards) */
#define CLEAR_MASK (MASK_W(INPUT_COLS) * MASK_H(PAD_ROWS))
#define CLEAR_RECTS 3072
#define MAX_GROWING_RECTS 512
/* how often (in rectangles) to check the command memory left, and the least to leave */
#define CMD_CHECK_EVERY 64
#define CMD_FREE_MIN 16384
typedef struct {
  uint16_t x, y, w, h; /* font pixels */
  uint8_t colour;      /* PAL_ index */
  uint8_t text_row;    /* for the row's opacity */
} clear_rect_t;
static uint8_t clear_mask[CLEAR_PANELS][CLEAR_MASK];
static clear_rect_t clear_rects[CLEAR_PANELS][CLEAR_RECTS];
static int clear_rect_count[CLEAR_PANELS], clear_cols[CLEAR_PANELS], clear_rows[CLEAR_PANELS];
static uint32_t clear_hash[CLEAR_PANELS];

static bool rect_continues(const clear_rect_t *rect, int x, int w, uint8_t colour, int text_row, int y)
{
  return rect->x == x && rect->w == w && rect->colour == colour && rect->text_row == text_row &&
         rect->y + rect->h == y;
}

/* Turn a mask into rectangles of one colour: returns how many. */
static int mask_to_rects(const uint8_t *mask, int width, int height, int yoff, clear_rect_t *rects)
{
  static uint16_t growing[2][MAX_GROWING_RECTS]; /* the rectangles that reached the row above */
  int count = 0;
  int n_above = 0;
  int flip = 0;
  for (int y = 0; y < height; y++) {
    int text_row = text_row_at(y, yoff);
    int n_here = 0;
    uint16_t *above = growing[flip];
    uint16_t *here = growing[!flip];
    for (int x = 0; x < width;) {
      uint8_t colour = mask[(size_t)y * width + x];
      int run_start = x;
      while (x < width && mask[(size_t)y * width + x] == colour)
        x++;
      if (!colour)
        continue;
      int run_width = x - run_start;
      int j = 0; /* the same run in the row above (same text row): grow it */
      while (j < n_above && !rect_continues(&rects[above[j]], run_start, run_width, colour, text_row, y))
        j++;
      int index;
      if (j < n_above) {
        index = above[j];
        rects[index].h++;
      } else {
        if (count >= CLEAR_RECTS)
          continue;
        index = count++;
        rects[index] = (clear_rect_t){
          .x = (uint16_t)run_start, .y = (uint16_t)y, .w = (uint16_t)run_width, .h = 1,
          .colour = colour, .text_row = (uint8_t)text_row,
        };
      }
      if (n_here < MAX_GROWING_RECTS)
        here[n_here++] = (uint16_t)index;
    }
    n_above = n_here;
    flip = !flip;
  }
  return count;
}

static void build_clear_rects(int i, const gfx_panel_t *panel, int cols, int rows)
{
  int width = MASK_W(cols);
  int height = MASK_H(rows);
  clear_rect_count[i] = 0;
  clear_hash[i] = panel->hash;
  clear_cols[i] = cols;
  clear_rows[i] = rows;
  if ((size_t)width * height > CLEAR_MASK)
    return;
  uint8_t *mask = clear_mask[i];
  fill_mask(mask, panel, width, height);
  add_drop_shadow(mask, width, height);
  clear_rect_count[i] = mask_to_rects(mask, width, height, panel->yoff, clear_rects[i]);
}

/* font pixels to screen pixels, rounded up */
static int font_to_screen_ceil(int font_px, int scale)
{
  return (font_px * scale + SC_ONE - 1) / SC_ONE;
}

static void set_render_target(nvn_texture_t *tex)
{
  nvn_texture_t *targets[1] = { tex };
  fn.set_render_targets(&cmd_buffer, 1, targets, 0, 0, 0);
}

/* Fill a rectangle of the render target (y from the top) with a colour. */
static void clear_scissored(int x, int y, int w, int h, int texture_height, const float *rgba)
{
  fn.set_scissor(&cmd_buffer, x, origin_lower_left ? texture_height - y - h : y, w, h);
  fn.clear_color(&cmd_buffer, 0, rgba, CLEAR_ALL_CHANNELS);
}

static void reset_scissor(void)
{
  fn.set_scissor(&cmd_buffer, 0, 0, SCISSOR_ALL, SCISSOR_ALL);
}

/* Checked every CMD_CHECK_EVERY rectangles: stop well before the command memory runs out. */
static bool command_memory_low(int i)
{
  return i % CMD_CHECK_EVERY == 0 && fn.cmd_free(&cmd_buffer) < CMD_FREE_MIN;
}

static void draw_clear_rects(int i, const gfx_panel_t *panel, nvn_texture_t *tex, nvn_rect_t at, int texture_height,
                             int scale)
{
  uint8_t pal[PAL_COUNT][3];
  build_palette(pal);
  set_render_target(tex);
  const clear_rect_t *rects = clear_rects[i];
  for (int j = 0; j < clear_rect_count[i]; j++) {
    if (command_memory_low(j))
      break;
    const clear_rect_t *rect = &rects[j];
    int text_row = rect->text_row < GFX_ROWS ? rect->text_row : GFX_ROWS - 1;
    unsigned alpha = panel->alpha[text_row];
    if (alpha < FADE_TEXT_GONE || (rect->colour == PAL_SHADOW && alpha < FADE_SHADOW_GONE))
      continue;
    float brightness = alpha / (255.0f * 255.0f); /* the colour's 0..255 to 0..1, darkened by the fade */
    const uint8_t *rgb = pal[rect->colour];
    float rgba[4] = { rgb[0] * brightness, rgb[1] * brightness, rgb[2] * brightness, 1 };
    int x0 = font_to_screen_ceil(rect->x, scale);
    int x1 = font_to_screen_ceil(rect->x + rect->w, scale);
    int y0 = font_to_screen_ceil(rect->y, scale);
    int y1 = font_to_screen_ceil(rect->y + rect->h, scale);
    if (x1 > at.w)
      x1 = at.w;
    if (y1 > at.h)
      y1 = at.h;
    if (x1 <= x0 || y1 <= y0)
      continue;
    clear_scissored(at.x + x0, at.y + y0, x1 - x0, y1 - y0, texture_height, rgba);
  }
  reset_scissor();
}

/* a text pixel blended over what was under it, alpha 0..255 (RGBA8) */
static inline uint32_t blend_rgba8(uint32_t over, uint32_t under, unsigned alpha)
{
  uint32_t result = 0;
  for (int shift = 0; shift < 24; shift += 8)
    result |= (((over >> shift & 0xFF) * alpha + (under >> shift & 0xFF) * (255 - alpha) + 127) / 255) << shift;
  return result | 0xFFu << 24;
}

/* ... then each frame: text pixels from the mask, background blended with what was under it.
 * `under` holds one row of the region per font-pixel row (its first screen row), under_width pixels
 * each (it can be wider than the panel: from the same top-left). */
static void compose_see_through(int p, uint8_t *dst, const uint8_t *under, int under_width, int w, int h, int scale,
                                int opacity, const uint8_t *row_alpha, int yoff)
{
  uint8_t pal[PAL_COUNT][3];
  uint32_t text_colour[PAL_COUNT];
  uint32_t solid_background = (uint32_t)encode_pixel(bg_rgb);
  build_palette(pal);
  for (int i = 1; i < PAL_COUNT; i++)
    text_colour[i] = (uint32_t)encode_pixel(pal[i]);
  /* per channel: what was under it -> the background blended over it */
  uint8_t background_over[3][256];
  int background_alpha = opacity * 255 / 100;
  for (int c = 0; c < 3; c++)
    for (int u = 0; u < 256; u++)
      background_over[c][u] = (uint8_t)((bg_rgb[c] * background_alpha + u * (255 - background_alpha) + 127) / 255);
  int mask_width = MASK_W(text_mask_cols[p]);
  uint32_t *out = (uint32_t *)row_buffer;
  for (int x = 0; x < w; x++)
    screen_to_font_x[x] = x * SC_ONE / scale;
  for (int y = 0; y < h; y++) {
    int font_y = y * SC_ONE / scale;
    const uint8_t *mask_row = text_mask[p] + (size_t)font_y * mask_width;
    const uint32_t *under_row = under ? (const uint32_t *)(under + (size_t)font_y * under_width * RGBA8_BYTES) : 0;
    unsigned text_alpha = 255; /* the text's opacity on this row (the log fading) */
    if (row_alpha) {
      int text_row = text_row_at(font_y, yoff);
      text_alpha = row_alpha[text_row < GFX_ROWS ? text_row : GFX_ROWS - 1];
    }
    for (int x = 0; x < w; x++) {
      uint8_t k = mask_row[screen_to_font_x[x]];
      if (k && (text_alpha >= 255 || !under_row)) {
        out[x] = text_colour[k];
      } else if (k) {
        out[x] = blend_rgba8(text_colour[k], under_row[x], text_alpha);
      } else if (under_row) {
        uint32_t u = under_row[x];
        out[x] = background_over[0][u & 0xFF] | background_over[1][u >> 8 & 0xFF] << 8 |
                 background_over[2][u >> 16 & 0xFF] << 16 | 0xFFu << 24;
      } else {
        out[x] = solid_background;
      }
    }
    memcpy(dst + (size_t)y * w * RGBA8_BYTES, row_buffer, (size_t)w * RGBA8_BYTES);
  }
}

/* Air wall outlines (airwall.c): each dot is a small square cleared into the picture with a scissor
 * rectangle, which needs no shader. Before the panels, so the menu stays on top. Stops well before
 * the command memory runs out. */
#define DOT_COORD_MAX 65535 /* aw_dot_t's coordinates: 0..65535 across the visible picture */
#define MIN_DOT_SIZE 2

static const float airwall_colours[AW_NCOLORS][4] = {
  [AW_BLOCKS] = { 0.26f, 0.60f, 1.0f, 1.0f }, /* blue: stops Link */
  [AW_OTHER] = { 0.53f, 0.84f, 0.91f, 1.0f },  /* pale cyan: special-purpose barriers */
  [AW_LOADING] = { 1.0f, 0.86f, 0.23f, 1.0f },
  [AW_VOID] = { 1.0f, 0.26f, 0.22f, 1.0f },
  [AW_GROTTO_SAFETY] = { 0.79f, 0.48f, 1.0f, 1.0f },
  [AW_FALL_SAMPLE] = { 1.0f, 0.50f, 0.15f, 1.0f },
  [AW_MESH_FLOOR] = { 0.28f, 0.83f, 0.37f, 1.0f },
  [AW_MESH_WALL] = { 0.22f, 0.62f, 0.98f, 1.0f },
  [AW_MESH_CEILING] = { 0.94f, 0.34f, 0.72f, 1.0f },
};

/* enemy health (enemyhp.c) */
static const float enemyhp_colours[EH_NCOLORS][4] = {
  [EH_BACK] = { 0.05f, 0.05f, 0.05f, 1.0f },  /* the bar's back and the text's background */
  [EH_FILL] = { 0.30f, 0.85f, 0.30f, 1.0f },  /* green: life left */
  [EH_LOW] = { 1.0f, 0.28f, 0.28f, 1.0f },    /* red: under a quarter left */
  [EH_TEXT] = { 1.0f, 1.0f, 1.0f, 1.0f },     /* white digits */
};

/* The same, drawn the same way, for the enemy health bars: shared by both (colours: n_colours). */
static void draw_rects(const aw_dot_t *dots, int n, const float (*colours)[4], int n_colours, nvn_texture_t *tex,
                       nvn_rect_t area, int texture_height, int scale)
{
  int size = scale / SC_ONE < MIN_DOT_SIZE ? MIN_DOT_SIZE : scale / SC_ONE;
  set_render_target(tex);
  for (int i = 0; i < n; i++) {
    if (command_memory_low(i))
      break;
    const aw_dot_t *dot = &dots[i];
    /* to texture pixels; a side thinner than the line width is widened to it, about its middle */
    int x = (int)((int64_t)dot->x0 * area.w / DOT_COORD_MAX);
    int x1 = (int)((int64_t)dot->x1 * area.w / DOT_COORD_MAX);
    int y = (int)((int64_t)dot->y0 * area.h / DOT_COORD_MAX);
    int y1 = (int)((int64_t)dot->y1 * area.h / DOT_COORD_MAX);
    int w = x1 - x;
    int h = y1 - y;
    if (w < size) {
      x = (x + x1 - size) / 2;
      w = size;
    }
    if (h < size) {
      y = (y + y1 - size) / 2;
      h = size;
    }
    /* kept inside the visible area */
    x = x < 0 ? 0 : x + w > area.w ? area.w - w : x;
    y = y < 0 ? 0 : y + h > area.h ? area.h - h : y;
    clear_scissored(area.x + x, area.y + y, w, h, texture_height, colours[dot->color < n_colours ? dot->color : 0]);
  }
  reset_scissor();
}

/* gz's pause icons (pause_icons.png tiles 1 and 2), 16 font pixels tall, at the top left: grey edges
 * (gz's 0xBFBFBF) round white (gz's colour for the icon), each run of a row one cleared rectangle. */
#define PAUSE_X_PERMILLE 35
#define PAUSE_Y_PERMILLE 45
#define PAUSE_ICON_H 16
#define PAUSE_ICON_PAUSED 1 /* gfx_pause_icon: two bars; otherwise advancing a frame (a bar and play) */

/* rectangle (x, y, w, h) of the icon, in font pixels from its top-left (origin_x, origin_y) */
static void pause_rect(nvn_rect_t area, int texture_height, int scale, int origin_x, int origin_y, int x, int y, int w,
                       int h, const float *rgba)
{
  int screen_x = area.x + origin_x + x * scale / SC_ONE;
  int screen_y = area.y + origin_y + y * scale / SC_ONE;
  int screen_w = (x + w) * scale / SC_ONE - x * scale / SC_ONE;
  int screen_h = (y + h) * scale / SC_ONE - y * scale / SC_ONE;
  if (screen_w <= 0 || screen_h <= 0)
    return;
  clear_scissored(screen_x, screen_y, screen_w, screen_h, texture_height, rgba);
}

static void draw_pause_icon(int icon, nvn_texture_t *tex, nvn_rect_t area, int texture_height, int scale)
{
  static const float edge[4] = { 0.75f, 0.75f, 0.75f, 1 };
  static const float fill[4] = { 1, 1, 1, 1 };
  int origin_x = area.w * PAUSE_X_PERMILLE / 1000;
  int origin_y = area.h * PAUSE_Y_PERMILLE / 1000;
  set_render_target(tex);
  /* a bar (both icons), and a second bar (paused) or a play triangle (advancing) */
  pause_rect(area, texture_height, scale, origin_x, origin_y, 0, 0, 6, PAUSE_ICON_H, edge);
  pause_rect(area, texture_height, scale, origin_x, origin_y, 1, 1, 4, PAUSE_ICON_H - 2, fill);
  if (icon == PAUSE_ICON_PAUSED) {
    pause_rect(area, texture_height, scale, origin_x, origin_y, 7, 0, 6, PAUSE_ICON_H, edge);
    pause_rect(area, texture_height, scale, origin_x, origin_y, 8, 1, 4, PAUSE_ICON_H - 2, fill);
  } else {
    for (int row = 0; row < PAUSE_ICON_H; row++) { /* rows 2, 3, ... 9 wide, then back down */
      int width = row < 8 ? row + 2 : 17 - row;
      pause_rect(area, texture_height, scale, origin_x, origin_y, 7, row, width, 1, edge);
      if (width > 2 && row > 0 && row < PAUSE_ICON_H - 1)
        pause_rect(area, texture_height, scale, origin_x, origin_y, 8, row, width - 2, 1, fill);
    }
  }
  reset_scissor();
}

/* Wait (up to a second) until the GPU has finished what a slot submitted; false if it didn't. */
static bool slot_finished(int slot)
{
  return !slot_used[slot] || fn.sync_wait(&slot_sync[slot], SYNC_TIMEOUT_NS) < SYNC_TIMEOUT_EXPIRED;
}

/* Wait until the GPU has finished every copy we submitted. */
static bool wait_gpu_idle(void)
{
  for (int s = 0; s < SLOTS; s++)
    if (!slot_finished(s)) {
      overlay_fail("GPU didn't finish the previous copy");
      return false;
    }
  return true;
}

/* The part of the texture that's shown. The game can render into a larger texture than it
 * displays (Lichii: 2560x1440 textures, crop 1920x1080), so positions come from the crop. */
static nvn_rect_t visible_area(const nvn_window_t *window, int texture_width, int texture_height)
{
  nvn_rect_t crop = { 0, 0, 0, 0 };
  if (window)
    fn.window_crop(window, &crop);
  if (crop.w <= 0 || crop.h <= 0 || crop.x < 0 || crop.y < 0 || crop.x + crop.w > texture_width ||
      crop.y + crop.h > texture_height)
    crop = (nvn_rect_t){ 0, 0, texture_width, texture_height };
  return crop;
}

static void log_area_change(nvn_rect_t area, int texture_width, int texture_height)
{
  static nvn_rect_t logged_area;
  if (area.x != logged_area.x || area.y != logged_area.y || area.w != logged_area.w || area.h != logged_area.h) {
    logged_area = area;
    log_printf("visible area %dx%d at %d,%d of %dx%d", area.w, area.h, area.x, area.y, texture_width, texture_height);
  }
}

static bool same_region(const nvn_region_t *a, const nvn_region_t *b)
{
  return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

/* --- each frame ------------------------------------------------------------------------------ */

/* What's the same for every panel this frame. */
typedef struct {
  nvn_rect_t area;  /* the visible part of the texture */
  int scale;        /* the menu's, in SC_ONE units */
  int margin;       /* screen pixels kept from the area's edges */
  int menu_y;       /* the menu's top, under the D-pad */
  int opacity;      /* of the panels' background, 0..100 */
  bool see_through; /* opacity below 100, and it can be done */
  int slot;
} frame_t;

/* One panel's part of this frame's commands. */
typedef struct {
  nvn_region_t region; /* where it goes in the texture */
  int scale;
  bool copy; /* copy `source` into region */
  nvn_addr_t source;
  int clear_index; /* >= 0: drawn as cleared rectangles instead (draw_clear_rects) ... */
  int panel;       /* ... of this panel */
  nvn_addr_t readback; /* nonzero: first copy what's under it to here, readback_rows rows of readback_region */
  int readback_rows;
  nvn_region_t readback_region;
} panel_draw_t;

/* settings > menu > size: auto, 2x, 2.5x, 3x */
#define MENU_SIZES 4

static int menu_scale_for(nvn_rect_t area)
{
  static const int sizes[MENU_SIZES] = { 0, 32, 40, 48 };
  int scale = gfx_menu_scale > 0 && gfx_menu_scale < MENU_SIZES ? sizes[gfx_menu_scale] : auto_scale(area.h) * SC_ONE;
  if (scale > alloc_scale * SC_ONE)
    scale = alloc_scale * SC_ONE; /* the buffers are sized for no more */
  return scale;
}

/* The top-left of a w x h panel, in the visible area: the menu under the D-pad, the others in their
 * corner (a GFX_CUSTOM panel where it was put, kept whole on screen). */
static void place_panel(int p, const gfx_panel_t *panel, const frame_t *frame, int w, int h, int *x_out, int *y_out)
{
  nvn_rect_t area = frame->area;
  int margin = frame->margin;
  int x = margin;
  int y = margin;
  if (p == PANEL_MENU) {
    if (!panel->fullscreen) {
      x = area.w * MENU_X_PERMILLE / 1000;
      y = frame->menu_y;
    }
  } else {
    switch (panel->corner) {
      case GFX_TOP_RIGHT:
        x = area.w - w - margin;
        break;
      case GFX_BOTTOM_LEFT:
        y = area.h - h - margin;
        break;
      case GFX_BOTTOM_RIGHT:
        x = area.w - w - margin;
        y = area.h - h - margin;
        break;
      case GFX_TOP_CENTRE:
        x = (area.w - w) / 2;
        break;
      case GFX_BOTTOM_CENTRE:
        x = (area.w - w) / 2;
        y = area.h - h - margin;
        break;
      case GFX_CUSTOM:
        x = area.w * panel->x / 1000;
        y = area.h * panel->y / 1000;
        if (x > area.w - w - margin)
          x = area.w - w - margin;
        if (y > area.h - h - margin)
          y = area.h - h - margin;
        if (x < margin)
          x = margin;
        if (y < margin)
          y = margin;
        gfx_custom_x = x * 1000 / area.w;
        gfx_custom_y = y * 1000 / area.h;
        break;
      default:
        break;
    }
  }
  *x_out = x;
  *y_out = y;
}

/* A panel with no background: drawn as rectangles in the command buffer (draw_clear_rects). */
static void plan_clear_panel(panel_draw_t *draw, int p, const gfx_panel_t *panel, int cols, int rows, int scale)
{
  int i = p - GFX_PANEL_LOG;
  if (clear_hash[i] != panel->hash || clear_cols[i] != cols || clear_rows[i] != rows)
    build_clear_rects(i, panel, cols, rows);
  draw->clear_index = i;
  draw->panel = p;
  draw->copy = false;
  draw->scale = scale;
}

/* how many font-pixel rows h screen pixels hold, rounded up */
static int font_rows_in(int h, int scale)
{
  return (h * SC_ONE + scale - 1) / scale;
}

/* A see-through panel: composed with what the readback two frames ago found under it, and this
 * frame's read back for later. `placed` is where it goes in the visible area. false: skip it. */
static bool plan_see_through_panel(panel_draw_t *draw, int p, const gfx_panel_t *panel, int cols, int rows,
                                   nvn_rect_t placed, int scale, const frame_t *frame)
{
  int slot = frame->slot;
  int w = placed.w;
  int h = placed.h;
  int rb_rows = font_rows_in(h, scale);
  nvn_region_t rb_region = draw->region;
  int under_width = w;
  if ((size_t)under_width * rb_rows * RGBA8_BYTES > readback_size[p])
    return false;
  if (text_mask_hash[p] != panel->hash || text_mask_cols[p] != cols || text_mask_rows[p] != rows)
    build_text_mask(p, panel, cols, rows);
  /* this slot's readback is from two frames ago (its fence has passed) */
  const uint8_t *under = 0;
  if (slot_used[slot] && same_region(&readback_region[p][slot], &rb_region)) {
    fn.pool_invalidate(&readback_pool[p][slot], 0, (size_t)under_width * rb_rows * RGBA8_BYTES);
    under = readback_mem[p][slot];
  }
  bool no_background = panel->clear;
  compose_see_through(p, panel_buffer_ptr(p, slot), under, under_width, w, h, scale, no_background ? 0 : frame->opacity,
                      no_background ? panel->alpha : 0, panel->yoff);
  /* nothing to blend with yet (2 frames: just opened, or a new size): read what's there and show
   * it then, rather than a solid background for 2 frames (a flash) */
  if (!under && no_background)
    draw->copy = false;
  drawn_hash[p][slot] = 0; /* the opaque path redraws when it's next used */
  readback_region[p][slot] = rb_region;
  draw->readback_region = rb_region;
  draw->readback = fn.pool_address(&readback_pool[p][slot]);
  draw->readback_rows = rb_rows;
  draw->source = panel_buffer_addr(p, slot);
  return true;
}

/* An opaque panel: rasterised into its buffer when it's changed. false: the GPU hung (give up). */
static bool plan_buffered_panel(panel_draw_t *draw, int p, const gfx_panel_t *panel, int slot, int scale,
                                uint32_t backdrop, int opacity)
{
  int b = buffers_per_panel[p] > 1 ? slot : 0;
  int w = draw->region.w, h = draw->region.h;
  uint32_t hash = backdrop ? panel->hash ^ (backdrop * 16777619u + (unsigned)opacity) : panel->hash;
  if (drawn_hash[p][b] != hash || drawn_w[p][b] != w || drawn_h[p][b] != h) {
    if (buffers_per_panel[p] == 1 && !wait_gpu_idle()) /* the GPU may still be copying the old one */
      return false;
    rasterise(panel, panel_buffer_ptr(p, b), scale, &draw->region, backdrop != 0, opacity);
    drawn_hash[p][b] = hash;
    drawn_w[p][b] = w;
    drawn_h[p][b] = h;
  }
  draw->source = panel_buffer_addr(p, b);
  return true;
}

/* What the game drew under a panel, before the panel covers it: one row per font-pixel row. */
static void record_readback(nvn_texture_t *tex, const panel_draw_t *draw)
{
  const nvn_region_t *from = &draw->readback_region;
  for (int k = 0; k < draw->readback_rows; k++) {
    /* font row k starts at screen row ceil(k * scale / 16) */
    nvn_region_t row = { from->x, from->y + font_to_screen_ceil(k, draw->scale), 0, from->w, 1, 1 };
    if (row.y >= from->y + from->h)
      break;
    fn.cmd_copy_to_buffer(&cmd_buffer, tex, 0, &row, draw->readback + (nvn_addr_t)k * from->w * RGBA8_BYTES, 0);
  }
}

/* Keep one GPU-only copy of the visible game image, before any mod overlays.
 * Present must continue so the practice menu can redraw on top of it. */
#define FREEZE_STRIPS 16
#define FREEZE_BYTES_MAX (32u * 1024 * 1024)
static nvn_obj_t freeze_pool[FREEZE_STRIPS];
static uint8_t *freeze_mem[FREEZE_STRIPS];
static size_t freeze_capacity[FREEZE_STRIPS];
static bool freeze_storage_failed;
static nvn_queue_t *freeze_queue;
static nvn_region_t freeze_region;
static uint32_t captured_token;
static int capture_slot;
static bool capture_complete;

static const uint32_t *frozen_row(int x, int y)
{
  int row = y - freeze_region.y;
  int strip = ((row + 1) * FREEZE_STRIPS - 1) / freeze_region.h;
  int local_y = row - freeze_region.h * strip / FREEZE_STRIPS;
  return (const uint32_t *)freeze_mem[strip] + (size_t)local_y * freeze_region.w + x - freeze_region.x;
}

static void freeze_fail(uint32_t token, const char *reason)
{
  if (token && __atomic_exchange_n(&gfx_freeze_failed, token, __ATOMIC_RELEASE) != token)
    log_printf("menu pause: backdrop unavailable (%s)", reason);
}

static bool freeze_prepare(uint32_t token, nvn_queue_t *queue, nvn_rect_t area)
{
  if (!token || __atomic_load_n(&gfx_freeze_failed, __ATOMIC_ACQUIRE) == token)
    return false;
  nvn_region_t region = { area.x, area.y, 0, area.w, area.h, 1 };
  if (captured_token == token) {
    if (queue != freeze_queue || !same_region(&region, &freeze_region)) {
      freeze_fail(token, "presentation region changed");
      return false;
    }
    if (!capture_complete) {
      if (!slot_finished(capture_slot)) {
        freeze_fail(token, "capture fence did not finish");
        return false;
      }
      capture_complete = true;
      if (is_rgba8(swapchain_format))
        for (unsigned i = 0; i < FREEZE_STRIPS; i++)
          fn.pool_invalidate(&freeze_pool[i], 0, freeze_capacity[i]);
      __atomic_store_n(&gfx_freeze_ready, token, __ATOMIC_RELEASE);
    }
    return true;
  }

  size_t needed = (size_t)region.w * region.h * bytes_per_pixel;
  if (!needed || region.h < FREEZE_STRIPS || needed > FREEZE_BYTES_MAX) {
    freeze_fail(token, "capture dimensions exceed limit");
    return false;
  }
  if (freeze_storage_failed) {
    freeze_fail(token, "capture storage unavailable");
    return false;
  }
  size_t strip_size = round_page((size_t)region.w * ((region.h + FREEZE_STRIPS - 1) / FREEZE_STRIPS) *
                                 bytes_per_pixel);
  for (unsigned i = 0; i < FREEZE_STRIPS; i++) {
    if (!freeze_capacity[i]) {
      void *mem = aligned_alloc(PAGE_SIZE, strip_size);
      if (!mem || !make_pool(&freeze_pool[i], mem, strip_size, POOL_CPU_CACHED | POOL_GPU_UNCACHED)) {
        if (mem)
          free(mem);
        freeze_storage_failed = true;
        freeze_fail(token, mem ? "NVN rejected capture strip storage" : "capture strip allocation failed");
        return false;
      }
      freeze_mem[i] = mem;
      freeze_capacity[i] = strip_size;
    }
    if (strip_size > freeze_capacity[i]) {
      freeze_fail(token, "capture region outgrew its storage");
      return false;
    }
  }
  if (freeze_queue && freeze_queue != queue && !wait_gpu_idle()) {
    freeze_fail(token, "capture queue is busy");
    return false;
  }
  freeze_queue = queue;
  freeze_region = region;
  return true;
}

static void draw_overlay(nvn_queue_t *queue, nvn_window_t *window, int index)
{
  uint32_t token = __atomic_load_n(&gfx_freeze_request, __ATOMIC_ACQUIRE);
  const gfx_screen_t *screen = gfx_published();
  int n_dots, n_hp;
  const aw_dot_t *dots = airwall_published(&n_dots);
  const aw_dot_t *hp = enemyhp_published(&n_hp);
  int pause_icon = gfx_pause_icon;
  if (overlay_state == OV_FAILED) {
    freeze_fail(token, "overlay renderer disabled");
    return;
  }
  if (!screen && !n_dots && !n_hp && !pause_icon && !token)
    return;
  if (index < 0 || index >= swapchain_texture_count) {
    static bool warned;
    if (!warned)
      log_printf("present of texture %d, but %d swapchain textures seen", index, swapchain_texture_count);
    warned = true;
    freeze_fail(token, "swapchain image unavailable");
    return;
  }
  if (overlay_state == OV_NONE && !overlay_init()) {
    freeze_fail(token, "overlay renderer initialization failed");
    return;
  }
  nvn_texture_t *tex = swapchain_textures[index];
  int tw = fn.tex_width(tex);
  int th = fn.tex_height(tex);
  if (fn.tex_format(tex) != swapchain_format) {
    if (!warned_format)
      log_printf("swapchain format changed to %d; not drawing", fn.tex_format(tex));
    warned_format = true;
    freeze_fail(token, "swapchain format changed");
    return;
  }
  frame_t frame;
  frame.area = visible_area(window, tw, th);
  log_area_change(frame.area, tw, th);
  frame.scale = menu_scale_for(frame.area);
  frame.margin = MARGIN_FONT_PX * frame.scale / SC_ONE;
  int menu_scale = frame.scale;
  int fit_w = (frame.area.w - frame.margin * 2) * SC_ONE / PANEL_W(GFX_COLS, SC_ONE);
  int fit_h = (frame.area.h - frame.margin * 2) * SC_ONE / PANEL_H(GFX_ROWS, SC_ONE);
  if (menu_scale > fit_w)
    menu_scale = fit_w;
  if (menu_scale > fit_h)
    menu_scale = fit_h;
  if (menu_scale < SC_ONE)
    menu_scale = SC_ONE;
  bool full_menu = screen && screen->panel[PANEL_MENU].rows && screen->panel[PANEL_MENU].fullscreen;
  frame.menu_y = full_menu ? frame.margin : frame.area.h * MENU_Y_PERMILLE / 1000;
  /* Size backing storage for the large canvas even if another overlay initializes it first. */
  int menu_space = frame.area.h - frame.margin * 2;
  int fit = (menu_space * SC_ONE / menu_scale - FONT_H) / CELL_H;
  gfx_menu_rows = fit < MIN_MENU_ROWS ? MIN_MENU_ROWS : fit > GFX_ROWS ? GFX_ROWS : fit;

  bool freeze = freeze_prepare(token, queue, frame.area);
  bool capture = freeze && captured_token != token;
  frame.opacity = gfx_opacity;
  const gfx_panel_t *input_panel = screen ? &screen->panel[GFX_PANEL_INPUT] : 0;
  frame.see_through = !freeze && input_panel && input_panel->rows && !input_panel->clear &&
                      frame.opacity < 100 && is_rgba8(swapchain_format) && see_through_state != ST_FAILED;
  if (frame.see_through && see_through_state == ST_NONE && !see_through_init())
    frame.see_through = false;
  /* panels with no background are drawn as rectangles (draw_clear_rects): no read-back for them */
  int hud_bottom_right = -1; /* the watches' top edge when they're at the bottom right: the log goes above */
  int hud_bottom_left = -1;  /* ... at the bottom left: the input display goes above */

  frame.slot = frame_count++ % SLOTS;
  if (!slot_finished(frame.slot)) { /* see wait_gpu_idle */
    overlay_fail("GPU didn't finish the previous copy");
    return;
  }

  panel_draw_t draws[GFX_PANELS];
  int n = 0;
  for (int i = 0; i < GFX_PANELS && screen; i++) {
    int p = (i + 1) % GFX_PANELS; /* the menu (0) last: it's drawn over everything else */
    const gfx_panel_t *panel = &screen->panel[p];
    if (!panel->rows)
      continue;
    int cols = panel->cols > MIN_PANEL_COLS ? panel->cols : MIN_PANEL_COLS;
    int rows = panel->rows;
    if (p == PANEL_MENU && panel->dashboard && cols < GFX_CARD_COL + GFX_CARD_WIDTH)
      cols = GFX_CARD_COL + GFX_CARD_WIDTH;
    if (cols > panel_max_cols[p])
      cols = panel_max_cols[p];
    if (rows > panel_max_rows[p])
      rows = panel_max_rows[p];
    /* a panel can be drawn smaller than the menu (the input display's size), not below 1x */
    int scale = panel->scale_pct ? frame.scale * panel->scale_pct / 100 : frame.scale;
    if (p == PANEL_MENU && panel->fullscreen)
      scale = menu_scale;
    if (scale < SC_ONE)
      scale = SC_ONE;
    int w = PANEL_W(cols, scale);
    int h = PANEL_H(rows, scale);
    int x, y;
    place_panel(p, panel, &frame, w, h, &x, &y);
    if (p == PANEL_HUD && panel->corner == GFX_BOTTOM_RIGHT)
      hud_bottom_right = y;
    if (p == PANEL_HUD && panel->corner == GFX_BOTTOM_LEFT)
      hud_bottom_left = y;
    if (p == GFX_PANEL_LOG && hud_bottom_right >= 0 && y + h > hud_bottom_right)
      y = hud_bottom_right - h;
    if (p == GFX_PANEL_INPUT && hud_bottom_left >= 0 && y + h > hud_bottom_left)
      y = hud_bottom_left - h;
    if (y < frame.margin)
      y = frame.margin;
    if (w > frame.area.w - x - frame.margin)
      w = frame.area.w - x - frame.margin;
    if (h > frame.area.h - y - frame.margin)
      h = frame.area.h - y - frame.margin;
    if (w <= 0 || h <= 0)
      continue;
    if (p != PANEL_MENU && (size_t)w * h * bytes_per_pixel > pixel_buffer_size[p]) /* never: sized for the most */
      continue;
    panel_draw_t *draw = &draws[n];
    draw->region = (nvn_region_t){ frame.area.x + x, frame.area.y + y, 0, w, h, 1 };
    draw->readback = 0;
    draw->copy = true;
    draw->clear_index = -1;
    if (panel->clear && p >= GFX_PANEL_LOG) {
      plan_clear_panel(draw, p, panel, cols, rows, scale);
      n++;
      continue;
    }
    if (frame.see_through && p == GFX_PANEL_INPUT) {
      nvn_rect_t placed = { x, y, w, h };
      if (!plan_see_through_panel(draw, p, panel, cols, rows, placed, scale, &frame))
        continue;
    } else if (!plan_buffered_panel(draw, p, panel, frame.slot, scale,
                                    p == PANEL_MENU && freeze && capture_complete && is_rgba8(swapchain_format)
                                      ? token : 0, frame.opacity)) {
      return;
    }
    draw->scale = scale;
    n++;
  }
  if (!n && !n_dots && !n_hp && !pause_icon && !freeze)
    return;
  __asm__ volatile("dsb sy" ::: "memory"); /* pixels reach memory before the GPU copies them */

  fn.cmd_add_command(&cmd_buffer, &command_pool, frame.slot * CMD_SIZE, CMD_SIZE);
  fn.cmd_add_control(&cmd_buffer, control_mem + frame.slot * CTRL_SIZE, CTRL_SIZE);
  fn.cmd_begin(&cmd_buffer);
  if (freeze) {
    for (int i = 0; i < FREEZE_STRIPS; i++) {
      int y0 = freeze_region.h * i / FREEZE_STRIPS;
      int y1 = freeze_region.h * (i + 1) / FREEZE_STRIPS;
      nvn_region_t strip = freeze_region;
      strip.y += y0;
      strip.h = y1 - y0;
      nvn_addr_t address = fn.pool_address(&freeze_pool[i]);
      if (capture)
        fn.cmd_copy_to_buffer(&cmd_buffer, tex, 0, &strip, address, 0);
      else
        fn.cmd_copy_to_texture(&cmd_buffer, address, tex, 0, &strip, 0);
    }
    if (capture) {
      captured_token = token;
      capture_slot = frame.slot;
      capture_complete = false;
    }
  }
  if (n_dots)
    draw_rects(dots, n_dots, airwall_colours, AW_NCOLORS, tex, frame.area, th, frame.scale);
  if (n_hp)
    draw_rects(hp, n_hp, enemyhp_colours, EH_NCOLORS, tex, frame.area, th, frame.scale);
  if (pause_icon)
    draw_pause_icon(pause_icon, tex, frame.area, th, frame.scale);
  for (int i = 0; i < n; i++) {
    const panel_draw_t *draw = &draws[i];
    if (draw->readback)
      record_readback(tex, draw);
    if (draw->clear_index >= 0) {
      nvn_rect_t at = { draw->region.x, draw->region.y, draw->region.w, draw->region.h };
      draw_clear_rects(draw->clear_index, &screen->panel[draw->panel], tex, at, th, draw->scale);
    } else if (draw->copy) {
      fn.cmd_copy_to_texture(&cmd_buffer, draw->source, tex, 0, &draw->region, 0);
    }
  }
  nvn_handle_t handle = fn.cmd_end(&cmd_buffer);
  fn.queue_submit(queue, 1, &handle);
  fn.queue_fence(queue, &slot_sync[frame.slot], SYNC_ALL_GPU_COMMANDS_COMPLETE, 0);
  slot_used[frame.slot] = true;

  if (!gfx_on_screen) {
    gfx_on_screen = true;
    log_printf("drawing on screen: scale %d/16 on %dx%d", frame.scale, tw, th);
  }
}
