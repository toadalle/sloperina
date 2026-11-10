# SloperinaGZ — Ghost/Hover Engineering Handoff

Status: Fritchii runtime feedback collected on the Stage 1 build; transition safety patch compiled but NOT YET Fritchii validated. Use `feature/practice-menu-foundation` (draft PR #2), not master. Do not merge without runtime validation.

## Priority order

1. **Ghost hover must actually own Link's locomotion/action state.** While ghost is active, ledge grabbing/climbing and other automatic traversal actions must not interrupt movement. Restore prior state exactly on disable, scene load, actor replacement, and failure.
2. **Camera must follow Link through colliders in ghost mode**, preserving right-stick rotation and sensible third-person distance. A camera collision bypass or a tested freecam/follow controller is required; Link CCT ForceNoHit alone is not enough.
3. **Loading-zone exit must finish when hover is on.** Runtime reproduction: enter a natural exit/load zone while levitating -> screen fades black -> scene never completes until levitation toggled OFF. The game appears to initiate the transition. Avoid repeatedly writing Link's position during transition.
4. **Normal hover (ghost off) must respect walls, ceilings, floors, and ledges.** Current `respect_walls` is only a push-back heuristic; direct physics-record writes can force Link through colliders.
5. **Grounded R+R3 wall pass** currently toggles the existing walking-through-walls cheat (temporary ForceNoHit, fixed feet height). Improve to true side-only/per-axis bypass if supported; do not assume floor collision exists during ForceNoHit.

Defer new aesthetic renderer, teleport projectile plumbing, and general menu redesign until these are stable.

## Proven mechanics and relevant modules

- `src/ootgz/levitate.c`: heap-scanned physics record, hover movement, `drop`, `float_step`, `walls_step`, `protect_landing`; after actor movement Link's display follows, but other state machines still act.
- `src/ootgz/cct.c`: validated 1.1.1 CCT property/body chain; `cct_noclip_begin/move/end` use `OneshotChangeForceNoHitMode` + `OneShotPhysicsCctChangePosition`; `cct_stop_velocity`, `cct_reset_vertical_velocity`, `cct_clear_fall_distance`. Preserve original collision mode, check owner identity and queued mode, avoid stale writes.
- `src/ootgz/link.c`: read-only active camera `[[main+0x4E98068]+0x68]`, camera position +0x38, look-at +0x44. These fields are **not** verified write-safe; camera update owner and collision logic must be traced before manipulation.
- `src/ootgz/pause.c`: verified PauseManager table at `main+0x4E97E38`, 0x20 entry size, hash ID +0x8, request count +0x1C (high flag bit). New `pause_transition_active()` only reads `LoadingPause`, `SceneTransition`, `EventBancChangePause`, `SystemPauseWhileLoading`; levitation now releases ownership when one becomes active. The exact timing of these requests at natural exits is **unverified**.
- `src/ootgz/gamedata.h`: `GD_STR_NEXT_MAP` is potentially helpful but **not** a reliable independent trigger without verifying natural-exit reservation timing. Avoid treating any non-empty value as a transition proof.
- `docs/FINDINGS.md` sections 5, 8, 9 and `docs/COLLISION.md`: pre-existing traced code and reverse engineering; prefer these over new guesses.

## Runtime observations (not established root causes)

- Ghost flight is noticeably smoother than baseline, with less jitter.
- Ghost mode permits Link's controller to move through walls, but camera still clips or stays behind walls.
- Link can initiate a ledge grab while ghost is active, interrupting mod movement.
- Normal hover can force its way through walls even when ghost is OFF.
- A previous standing-after-disable issue was intermittent and later disappeared; do not assume it is fixed.
- While opening the practice menu, a screenshot caught the native Plus pause UI simultaneously; the R+Plus HID filter may not cover every input route.
- Existing air-wall visualization shows cyan and violet boxes. Death/barrier/loading-zone visualization is NOT implemented.

## Required investigation and acceptance criteria

### Ghost action ownership
- Locate Link's traversal/action state, especially ledge-grab detection and climb transition, on the target game build.
- Find a reversible toggle or scoped override, **not** a permanent actor byte patch or global collision/event kill switch.
- Test hovering past grab-height ledges from above, below, sideways, and while holding movement inputs.
- Verify no launch/stuck animation on disable; no effect on ordinary walking; restore on death, reload, and cutscene.

### Ghost camera
- Identify camera world-collision/occlusion query and update order.
- During ghost, preserve follow offset, look-at, player framing and right-stick yaw/pitch while bypassing physical occlusion.
- Probe a building with walls/roof, a tunnel, and a thin wall; camera should follow Link without snapping to previous side.
- On ghost disable restore original camera settings and re-enter standard collision smoothly.
- Do not just set read-only camera position each frame: that risks fighting the engine and softlocking.

### Loading zones
- Test natural exits with ghost ON/OFF and normal hover, at slow/high speed.
- Determine first reliable event that precedes fade and physics control handoff.
- Release hover before stale actor/controller destruction. Do not suppress or disable zone triggers.
- Transition must complete without manually toggling L3; arrival scene must have normal collision/grounding.

### Normal hover and ground walk
- Prefer native CCT sweep/slide/shape cast to raw position override; isolate horizontal and vertical collision.
- Validate walls, corners, ceilings, floor support, stairs, ramps, edges and 2/4/6/8/12/20/30 speed presets.
- Never restore collision while Link is inside solid geometry without a verified overlap/clearance policy.

## Test instructions for user

Install GitHub Actions artifact from the latest green PR build. In a reliable exit and a building with grabbable ledges:

1. Start grounded; test standing, jump, and camera as control.
2. L3 hover (ghost OFF): approach a solid wall/ceiling slowly, then at higher movement speed; note any penetration.
3. R3 ghost ON: fly through a wall; observe whether camera follows and whether ledge grab interrupts Link.
4. R3 ghost OFF, then L3 hover OFF on safe open ground; verify landing, normal camera, walking.
5. Hover ON, enter loading zone; record whether fade completes on its own. Check the `sloperinaGZ` log for `native transition pause active`.
6. From normal walking, R+R3 toggles wall-pass; test flat wall, uneven ground, stairs, and toggle back. It is not true floor-safe noclip.
7. Confirm R+R3 in hover resets speed to 6, Y/A change speed, left stick navigates practice menu, and HUD shows `flight speed`.

Provide the *map/exit, mode, button sequence, expected behavior, actual behavior, and if available log snippet*. A short screen recording of a ledge-grab or loading-stall reproduction is especially useful.

## Guardrails

- Keep the branch draft; preserve the last green build and changelog.
- Require tests and native CI for each change. Host mocks do not prove engine runtime behavior.
- Don't enable unsafe memory writes by default; map exactly to OoT 1.1.1.
- Defer hooks without verified ownership and failure recovery. Mark blocked items rather than emulating behavior inaccurately.
