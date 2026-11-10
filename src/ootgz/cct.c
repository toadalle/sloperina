/* Use the same physics APIs as OneShotPhysicsCctChangePosition and
 * OneshotChangeForceNoHitMode. Writing a scanned position record alone leaves the
 * collision solver active and can disagree with the controller's other transforms.
 * This module changes neither code nor collision geometry. */
#include "cct.h"

#include "sys.h"
#include "z64hd.h"

typedef struct { uintptr_t actor, property, body; } controller_t;
static controller_t owner;
/* Link can switch bodies for traversal and leave the old one detached.
 * Keep each saved flag until that body is reachable again through Link. */
static struct collision_restore {
  uintptr_t actor, body;
  bool no_hit;
} restores[4];
static int active_restore = -1;

static uintptr_t next(uintptr_t p, size_t offset)
{
  return p && p <= UINTPTR_MAX - offset ? sys_ptr(p + offset) : 0;
}

static bool writable(uintptr_t p, size_t size)
{
  return p && !(p & 7) && sys_mapped(p, size, SYS_PERM_R | SYS_PERM_W);
}

static bool resolve_body(controller_t *out)
{
  /* not known for this build: nothing to call */
  if (!z64hd_game || !Z64HD_VT_CCT_PROPERTY || !Z64HD_VT_CCT_BODY || !Z64HD_CCT_FORCE_NO_HIT ||
      !Z64HD_CCT_SET_POSITION || !Z64HD_CCT_SET_VELOCITY)
    return false;
  uintptr_t actor = link_actor(), p = actor;
  static const unsigned offsets[] = { 0x270, 0x50, 0x20, 0x10, 8 };
  for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); i++)
    p = next(p, offsets[i]);
  uintptr_t body = next(p, 0x20);
  if (!writable(p, 0x60) || !writable(body, 0x320) ||
      sys_ptr(p) != MAIN(Z64HD_VT_CCT_PROPERTY) || sys_ptr(body) != MAIN(Z64HD_VT_CCT_BODY))
    return false;
  uintptr_t pending = next(body, 0x60);
  if (pending && !writable(pending, 0x108))
    return false;
  *out = (controller_t){ actor, p, body };
  return true;
}

static bool resolve(controller_t *out)
{
  if (!resolve_body(out))
    return false;
  /* Position setting visits the controller's attached-body lists. Do not call it
   * during partial construction/teardown or with an unreadable list. */
  uintptr_t lists = next(out->property, 0x58);
  if (!writable(lists, 0x60))
    return false;
  for (unsigned off = 0x10; off <= 0x50; off += 0x10) {
    uint32_t count;
    if (!sys_read32(lists + off, &count) || count > 128)
      return false;
    uintptr_t entries = next(lists, off + 8);
    if (count && (!entries || !sys_mapped(entries, count * sizeof(uintptr_t), SYS_PERM_R)))
      return false;
    for (unsigned i = 0; i < count; i++)
      if (!writable(sys_ptr(entries + i * sizeof(uintptr_t)), off == 0x10 ? 0x320 : 0x38))
        return false;
  }
  return true;
}

static bool same(controller_t a, controller_t b)
{
  return a.actor == b.actor && a.property == b.property && a.body == b.body;
}

static bool no_hit(uintptr_t body)
{
  /* The setter checks the queued flag first: an enable/disable may not have
   * reached the physics thread yet. +0x100 bit 19 makes +0xF8 bit 0 authoritative. */
  uintptr_t pending = next(body, 0x60);
  if (pending && (*(volatile uint64_t *)(pending + 0x100) & (1ull << 19)))
    return (*(volatile uint32_t *)(pending + 0xF8) & 1) != 0;
  return (*(volatile uint64_t *)(body + 0x70) & (1ull << 11)) != 0;
}

bool cct_snapshot(cct_snapshot_t *out)
{
  controller_t current;
  if (!resolve_body(&current))
    return false;
  *out = (cct_snapshot_t){
    .actor = current.actor,
    .property = current.property,
    .body = current.body,
    .pending = next(current.body, 0x60),
    .velocity = *(volatile vec3f_t *)(current.body + 0xE0),
    .body_flags = *(volatile uint64_t *)(current.body + 0x70),
    .no_hit = no_hit(current.body),
  };
  return true;
}

static void set_no_hit(uintptr_t body, bool enabled)
{
  MAIN_FN(void (*)(uintptr_t, bool), Z64HD_CCT_FORCE_NO_HIT)(body, enabled);
}

bool cct_reset_vertical_velocity(uintptr_t expected_actor)
{
  controller_t current;
  if (!expected_actor || !resolve(&current) || current.actor != expected_actor)
    return false;
  /* The game's vertical-gravity reducer reads this same applied velocity and
   * uses this setter (main+0x278AA90). The setter locks and queues the change;
   * writing the scanned Havok record leaves this velocity at terminal speed. */
  vec3f_t v = *(volatile vec3f_t *)(current.body + 0xE0);
  if (!(v.x > -10000 && v.x < 10000 && v.z > -10000 && v.z < 10000))
    return false;
  v.y = 0;
  MAIN_FN(void (*)(uintptr_t, const vec3f_t *), Z64HD_CCT_SET_VELOCITY)(current.body, &v);
  return true;
}

bool cct_stop_velocity(uintptr_t expected_actor)
{
  controller_t current;
  if (!expected_actor || !resolve(&current) || current.actor != expected_actor)
    return false;
  const vec3f_t stopped = { 0, 0, 0 };
  MAIN_FN(void (*)(uintptr_t, const vec3f_t *), Z64HD_CCT_SET_VELOCITY)(current.body, &stopped);
  return true;
}

bool cct_on_ground(uintptr_t expected_actor, bool *grounded)
{
  controller_t current;
  if (!expected_actor || !resolve(&current) || current.actor != expected_actor)
    return false;
  /* QueryPhysicsCctIsOnGround (main+0x2AABA20): current result in a
   * three-slot buffer, state 0 means supported. Never infer landing from a
   * stationary position: pauses and midair hover are stationary too. */
  uintptr_t results = next(current.property, 0x48);
  uint32_t slot, state;
  if (!results || !sys_read32(results + 0x58, &slot) || slot >= 3)
    return false;
  uintptr_t result = next(results, 0x40 + slot * 8);
  if (!result || !sys_read32(result + 0x10, &state))
    return false;
  *grounded = state == 0;
  return true;
}

bool cct_clear_fall_distance(uintptr_t expected_actor)
{
  uintptr_t actor = link_actor();
  if (!expected_actor || actor != expected_actor)
    return false;
  uintptr_t components = next(actor, 0x270);
  uintptr_t player = next(components, 0x358), base = next(components, 0x2B8);
  if (!writable(player, 0x1910) || !writable(base, 0x18A0) ||
      next(player, 0x18) != actor || next(base, 0x1340) != actor ||
      !sys_mapped(actor + 0x320, 4, SYS_PERM_R))
    return false;
  float y = *(volatile float *)(actor + 0x320);
  if (!(y > -100000 && y < 100000))
    return false;
  /* main+0x5FCF6C computes player+0x190C from +0x18D4 minus actor Y;
   * +0x5FD128 maintains the peak at +0x18E0. +0x5FD19C feeds the base
   * fall origin, whose distance is computed at +0x484A10. Reset history,
   * never the actor's position, gravity, health, or JS Height (+0x18E8). */
  *(volatile float *)(player + 0x18D4) = y;
  *(volatile float *)(player + 0x18E0) = y;
  *(volatile float *)(player + 0x190C) = 0;
  *(volatile float *)(base + 0x1898) = y;
  *(volatile float *)(base + 0x189C) = 0;
  return true;
}

/* Return a free slot after servicing any restores that are safe now. A
 * detached body is never dereferenced, even if its old address is mapped. */
static int service_restores(uintptr_t actor, uintptr_t body)
{
  int free_slot = -1;
  for (int i = 0; i < (int)(sizeof(restores) / sizeof(*restores)); i++) {
    struct collision_restore *r = &restores[i];
    if (r->body && i != active_restore) {
      if (actor && actor != r->actor) {
        *r = (struct collision_restore){0};
      } else if (actor == r->actor && body == r->body) {
        if (no_hit(body) != r->no_hit)
          set_no_hit(body, r->no_hit);
        log_printf("levitate: body %#lx collision bypass restored to %d", (unsigned long)body, r->no_hit);
        *r = (struct collision_restore){0};
      }
    }
    if (!r->body)
      free_slot = i;
  }
  return free_slot;
}

bool cct_noclip_active(void) { return active_restore >= 0; }

bool cct_noclip_begin(void)
{
  controller_t current;
  if (!resolve(&current))
    return false;
  if (cct_noclip_active())
    return same(owner, current);
  int slot = service_restores(current.actor, current.body);
  if (slot < 0) {
    log_printf("levitate: all collision restore slots are pending");
    return false;
  }
  bool original_no_hit = no_hit(current.body);
  restores[slot] = (struct collision_restore){ current.actor, current.body, original_no_hit };
  active_restore = slot;
  owner = current;
  if (!original_no_hit)
    set_no_hit(owner.body, true);
  log_printf("levitate: controller %#lx, body %#lx, collision bypass on (was %d)",
             (unsigned long)owner.property, (unsigned long)owner.body, original_no_hit);
  return true;
}

bool cct_noclip_move(const vec3f_t *position)
{
  controller_t current;
  if (!cct_noclip_active() || !resolve(&current) || !same(owner, current) ||
      !(position->x > -100000 && position->x < 100000 &&
        position->y > -100000 && position->y < 100000 &&
        position->z > -100000 && position->z < 100000))
    return false;
  if (!no_hit(owner.body))
    set_no_hit(owner.body, true);
  MAIN_FN(void (*)(uintptr_t, const vec3f_t *), Z64HD_CCT_SET_POSITION)(owner.property, position);
  return true;
}

bool cct_noclip_end(void)
{
  int released = active_restore;
  active_restore = -1;
  bool pending = false;
  for (unsigned i = 0; i < sizeof(restores) / sizeof(*restores); i++)
    pending |= restores[i].body != 0;
  if (!pending)
    return true;

  controller_t current;
  bool resolved = resolve_body(&current);
  service_restores(resolved ? current.actor : link_actor(), resolved ? current.body : 0);
  if (released >= 0 && restores[released].body)
    log_printf("levitate: body %#lx detached/unavailable; collision restore pending",
               (unsigned long)restores[released].body);

  if (resolved)
    return true; /* Any remaining restore belongs to a different body. */
  for (unsigned i = 0; i < sizeof(restores) / sizeof(*restores); i++)
    if (restores[i].body)
      return false;
  return true;
}
