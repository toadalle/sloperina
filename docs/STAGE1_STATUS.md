# Stage 1 implementation status

This branch is an implementation checkpoint built from SloperinaGZ v0.0.3
(`cad59954d64209a59f57823177d65e2e47eec07a`). It has **not** been validated
in Fritchii yet. Keep `master` on the known-working release until runtime tests pass.

## Implemented

### Practice menu foundation

- R + Plus toggles the practice menu.
- A confirms/activates; B goes back; D-pad navigates and edits values.
- Buttons held while the menu closes are suppressed until release so the close
  action does not leak into gameplay or open the normal Plus menu.
- The existing breadcrumbs, retained submenu/selection, reusable item types and
  settings system are retained rather than replaced.
- The menu uses the renderer's maximum menu canvas and begins at the safe-area
  margin instead of the old corner/HUD placement.
- Top-level categories are Link, Cheats, Scene, Warps, Inventory, Story,
  Debug Tools and Settings. Existing features remain reachable under those groups.
- "pause game while open" is persisted. The default is on; turning it off gives
  the requested live-inspection mode.

### World pause

For game 1.1.1, the menu owns one request on the existing `GamePause`
PauseManager entry documented in `FINDINGS.md` section 9.1.

The implementation:

- keeps the 1.1.1 manager offset in `z64hd_builds.h`;
- validates the manager/table/entry before writing;
- changes only the low 31-bit request count and preserves the high flag bit;
- owns exactly one increment and removes exactly one on close;
- refuses stale writes if the entry changes;
- falls back to a live menu on unsupported/invalid states instead of guessing.

This path is source-reviewed but **runtime-unvalidated**.

### Hover / ghost mode

- L3 toggles hover.
- R3 toggles ghost collision while hovering.
- X ascends, B descends, and releasing both holds altitude.
- Each new hover starts with normal collision.
- The left stick is consumed by hover movement and hidden from Link's ordinary
  gameplay movement state machine, reducing grounded/ledge/climb conflicts.
- The right stick remains available to the camera.
- Ghost movement uses the existing native CCT position API and now queues zero
  controller velocity while the mod owns movement.
- Original ForceNoHit state is still restored through the existing defensive CCT
  owner tracking.
- L/R + L3 remains reserved for frame-advance behavior.

## Runtime test checklist

1. Confirm the v0.0.3 release still works before installing this branch.
2. Build and install this branch for game 1.1.1.
3. Press R + Plus from gameplay:
   - practice menu opens;
   - native Plus pause screen does not also open;
   - world simulation stops but menu input/rendering continues.
4. Close with R + Plus and with B from the root; confirm no R/Plus/B action leaks
   into gameplay.
5. Reopen the menu and verify it returns to the previous submenu/selection.
6. In Settings, disable "pause game while open"; verify the world runs while the
   menu remains usable. Re-enable it and verify the setting persists after restart.
7. Verify all eight top-level categories and that moved legacy features remain
   reachable (levitation under Cheats; watches/input/frame advance/log under
   Debug Tools; equipped gear under Inventory; About under Settings).
8. Enable levitation, then:
   - L3 starts normal hover;
   - X/B move vertically and release holds altitude;
   - left stick moves horizontally without Link entering ordinary walk/climb
     reactions;
   - R3 enables ghost and passes through walls/floors/ceilings;
   - R3 disables ghost and collision resumes;
   - L3 exits hover and normal physics/fall handling returns.
9. Test scene reloads and cutscenes while hover/ghost are active. Any controller
   replacement must fail closed rather than writing through a stale pointer.
10. Re-test frame advance, especially L/R + L3 resume chords and opening the
    practice menu near a frame-advance pause.

## Deliberately saved for later

These need runtime reverse engineering or a larger renderer change; no fake
implementation has been added.

- **Ghost exit solid check:** ForceNoHit can be restored safely as state, but the
  current known CCT API does not expose a proven "is this capsule placement free?"
  query. Exact prevention of collision re-enable while embedded needs that query
  (or a tested sweep/overlap adapter).
- **Projectile teleport:** shared destination semantics are settled, but real
  hookshot/slingshot/bow/elemental-arrow impact callbacks still need to be found.
  Do not substitute camera raycasts. The impact point must ultimately map to
  Link's feet with normal clearance and one-shot impact ownership.
- **Scene visualizer:** loading/death/barrier classification and a depth-aware
  batched geometry path require additional native NVN/physics work. Do not solve
  this by merely increasing the existing rectangle/dot budgets.
- **Zone activation overrides:** requires identifying the transition/hazard
  dispatch sites so display, selection and activation can remain independent.
- **Free camera:** camera ownership/detach/restore calls are not yet proven for
  this build.
- **Frame-advance/menu hold interaction:** the legacy artificial frame-advance
  hold can stop presents; a menu opened from that exact hold state needs a
  dedicated integration decision rather than another input hack.

## Build validation

`.github/workflows/build.yml` runs `./docker-make` using the same devkitA64
container documented by the repository. A green build confirms compilation and
linking only; it does not count as Fritchii validation.
