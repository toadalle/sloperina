# Stage 1 implementation status

This branch is an implementation checkpoint built from SloperinaGZ v0.0.3
(`cad59954d64209a59f57823177d65e2e47eec07a`). It has **not** been validated
in Fritchii yet. Keep `master` on the known-working release until runtime tests pass.

## Implemented

### Practice menu foundation

- R + Plus toggles the practice menu.
- The HID wrapper removes the R + Plus activation chord from the same game read
  that detects it, so the stock Plus pause screen cannot win the input race.
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

### Air-wall overlay cleanup

- Retains the existing cached-per-map geometry and nearest-first dot budget.
- Rejects out-of-range wall spheres with squared distance before paying for a square root.
- Rejects wall spheres wholly outside the camera frustum before projecting individual edges.
- This is intentionally a small optimization of the legacy overlay, not a substitute for the
  planned batched/depth-aware scene renderer.

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
2. Download the CI artifact for this branch (or build locally) and install the
   staged `dist/lichii` tree for game 1.1.1.
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

`.github/workflows/build.yml` runs the existing host regression suites first,
then `./docker-make dist` with the repository's devkitA64 container. The host
tests cover levitation/CCT lifecycle, the new ghost toggle and stick masking,
settings persistence, watches and menu/HUD regressions.

On success CI uploads a seven-day `sloperina-stage1-<sha>` artifact containing
the repository's existing staged install trees, including `dist/lichii` for
Lichii/Fritchii-style mod directories. A green workflow confirms host behavior
covered by those mocks plus native compilation/linking; it still does **not**
count as Fritchii runtime validation.

## Fritchii runtime findings (2026-11-10)

- **Confirmed:** R + Plus opens the practice menu, controller ghost movement is smoother, but normal-hover collision is not reliable (can cross walls without ghost).
- **Confirmed:** ghost mode does **not** give the camera noclip. The camera continues to collide with the scene and can remain behind a wall while Link moves through it. Do not write camera position fields without establishing camera update ownership and a safe restore path.
- **Confirmed:** ledge-grab/climb interaction can interrupt ghost levitation. Link's character controller ForceNoHit is not a complete player action/state override. Identify the actual player traversal state and a safe suppression mechanism before attempting to patch it; blanket input suppression is insufficient because ledge grab is collision-triggered.
- **Observed:** after temporarily failing to stand properly on release, ordinary standing recovered. Treat the landing regression as intermittent/unconfirmed; test both release from normal hover and release from ghost, with landing on flat ground and after a scene transition. The cleanup branch now queues a CCT velocity stop during release; this is **not** a confirmed fix for grounding.
- **UI:** first runtime screenshot showed an enlarged traditional text menu rather than the proposed two-column dashboard. The branch now renders a text-only two-column root layout inside the existing renderer. Card backgrounds, font design, and native GUI controls remain unimplemented.
- **Pause race:** one screenshot shows the native system pause UI underneath the practice menu. The R+Plus HID filter is not confirmed to suppress Plus on all in-game input paths/frames. Investigate state sequencing, HID routes, and close-frame masking before considering pause interception fully verified.
- **Collision visualization:** current Scene > Collision settings control only the existing **air walls** overlay. The desired categorized death/barrier/loading-zone renderer, toggleable classes and activation controls still require new verified scene/physics hooks, native overlay support, and classification. No placeholder options are introduced that pretend to implement them.

### Codex handoff priorities

1. Establish action/state hooks to prevent ledge grabs and other traversal events while ghost hover owns Link, with explicit restoration on disable, transitions and failures.
2. Identify native camera collision/follow state; offer an opt-in camera ghost mode with owner checks, no stale pointers and proper restoration, while preserving right-stick rotation.
3. Trace R+Plus native pause leakage using the real HID routing and fix it without suppressing unrelated Plus behavior.
4. Validate hover release and standard-hover collision, especially the difference between ForceNoHit ownership and record-only motion.
5. Replace root/menu text styling with real independent cards/layout primitives only after renderer architecture review; add actual scene-layer categories with measured GPU cost and verified semantics.

### Additional input refinements

- While hovering, Y lowers and A raises **horizontal** flight speed through the existing 2/4/6/8/12/20/30 units-per-second presets. Both pressed together have no effect. Game actions on Y/A are masked during hover; menu A still confirms selections when open.
- Practice-menu navigation now accepts the left analog stick in addition to the D-pad with a high dead zone, single dominant axis and key-repeat cadence, including value adjustments and the two-column root grid.

### Confirmed normal-hover collision failure

Fritchii runtime feedback confirms **normal hover (ghost OFF) can force Link through colliders**. Current `respect_walls()` is a heuristic that detects/reconciles some game push-back, but `float_step()` still directly overwrites the physics position record every frame. This is *not* reliable wall collision. Do not describe the non-ghost mode as collision-safe. Codex should identify and use a native controller sweep/slide/shape cast, or move Link through a normal collision-aware physics API; if none can be established, prioritize conservative behavior rather than pretending the push-back heuristic is sufficient. Validate hard corners, thin walls, floors, ceilings, ledges, high flight speed, and transitions. Preserve verified ghost ForceNoHit and explicit camera/action independence.

### Flight-speed reset and watch HUD

- R + R3 (right-stick click) while levitating resets horizontal flight speed to the original mod's default of **6 units/s**, with priority over the R3 ghost toggle. Plain R3 still toggles ghost. This is the hover preset, **not** a modification to ordinary grounded Link movement speed.
- The bottom-left watches HUD includes a separate `flight speed` row for the selected hover movement-speed setting. The existing `speed` row remains a measured Link velocity. The additional line is deliberately outside the persisted watch IDs, avoiding saved-ID migration; it follows watch HUD visibility and placement.

### Loading-zone crossing while levitating (new runtime report)

Fritchii reports an issue when Link passes through a loading/scene-transition zone while levitating. **Exact observed outcome still needs classification**: transition may fire and leave hover/ghost state stale, may fail to trigger, or may trigger repeatedly/softlock. Do not assume which occurred.

The mod currently tracks a scanned Link physics record and a CCT ForceNoHit owner across frames. It attempts to re-resolve a stale actor/record after a scene transition, but it does not have a proven loading-zone crossing callback or a verified transition-ownership boundary. Existing `GD_STR_NEXT_MAP` in `gamedata.h` and the warp module provide potential transition signals, but the presence/absence semantics have not been validated for natural exits.

Codex: trace loading-zone trigger and scene change lifecycle, ensure hover/ghost stops safely and collision ownership is restored before unloading the old controller; avoid reusing stale physics pointers or resuming hover automatically in the new scene. Test natural and mod-triggered transitions with ghost both on and off, high-speed entry, repeated crossings, and pause/menu interactions. **Do not** disable trigger volumes globally to implement this; desired zone activation control is a separate opt-in feature.

### Loading-zone failure clarified and contextual walking shortcut

Fritchii reproduction: **passing through a natural loading zone while hover is active begins the black transition fade, but the next scene does not actually load until levitation is switched OFF**. This is a stalled transition, not a failure to initiate the fade. It strongly suggests hover's per-frame position/control ownership interferes with transition handoff. Proper repair needs a confirmed transition event/state and release before the scene unloads; blindly treating a non-empty NextMap string as proof of a transition is unsafe.

R + R3 while **not hovering** now toggles the existing `CHEAT_WALLS` walking wall-pass mode. During ordinary walking, the game's collision stays in place. When the existing blocked-wall heuristic detects a wall, the controller enters temporary ForceNoHit and holds the current feet height while moving through; after releasing/turning the stick, collision is restored. This is a pragmatic interim solution, **not** per-axis collision masking: floors are not collision-active while ForceNoHit is on, and the current held-height mechanism can fail at stairs, slopes, dropped floors, or loading zones. Do not claim robust floor support. R + R3 while hovering still resets flight speed, and plain R3 while hovering still toggles ghost.
