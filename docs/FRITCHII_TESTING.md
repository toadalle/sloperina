# Fritchii validation checklist

Install the build for this branch using the `lichii/load` layout in the Actions
artifact. Keep the previous mod folder available for rollback and use one
SloperinaGZ build at a time. The transition and native controller paths are
primarily for OoT 1.1.1. The controls, dashboard, and text capture are also
available on 1.0.0 where the corresponding fields can be read.

1. **Menu and HUD.** On a map with Link and visible animated actors, open with
   R + Plus. Expect a centered card dashboard and a static game backdrop
   with Link, actors, and the stock HUD visible. The header should move from
   FREEZING to PAUSED. Turn off Settings > Freeze game while open and check
   that animations advance behind the menu. Navigate using D-pad and left
   stick, then close with B or R + Plus while holding a stick off center.
   The held stick should stay suppressed until centered. Plus alone should
   still open the stock pause screen. Test opening from a bumper frame-advance
   hold and verify that closing returns to the hold. If backdrop capture or
   native pause is unavailable, the dashboard stays LIVE and reports the
   fallback. The freeze choice persists under `practice_menu_pause`.
2. **Flight presets.** Start hover with L3. At low and high presets, compare
   horizontal travel and the distance covered by holding X or B for the same
   time. Y/A step the preset, R + R3 sets it to 6, and X+B holds altitude.
   The base rise speed is the X/B speed at preset 6. Change Settings →
   Reset flight speed on hover to verify that both reset and retain behavior
   survive a restart.
3. **Recovery.** Hover in place for several seconds in normal and ghost mode,
   then move. In ghost mode, void out, wait until gameplay is back, and press
   L3 again. Repeat around a natural scene exit, including with a record search
   in progress. The old hover should release at transition, actor replacement,
   or a large relocation. Check for ordinary collision on arrival and capture
   the log if a black screen remains or hover fails to restart.
4. **Collision outlines.** In Scene → Collision, enable Show. Inspect cyan Link
   blockers, violet other air walls, and the red inside indicator with a nearby
   volume. Rotate and approach a wall to check for screen-spanning lines or
   partial outlines. Try All Kinds and move between maps. The view uses the
   repository's placed air-wall data, so map meshes and loading/death triggers
   are outside its coverage.
5. **Camera and ledges.** Capture Quick Snapshot in open space, then fly through
   a wall or near a ledge. If the camera or Link gets stuck, immediately take
   another Quick Snapshot, followed by Deep Diagnostics. Include the map,
   route, controls and expected behavior with the two captures.

## Export diagnostics

Open Debug Tools → Diagnostics and select **Quick Snapshot** or **Deep
Diagnostics**. Each successful capture reports its guest SD path in the mod
log. Find `sloperinaGZ/sloperina-devdump-*` in the emulator's **virtual SD
card** folder. The `ootgz:` prefix is the guest mount name. Folder names use
the guest's tick counter and a capture number. Zip the selected folders on the
host to share them. A manifest lists the build, sample conditions, addresses,
field offsets, unavailable fields and any binary write errors. The report is
bounded to 8 KiB, and the optional redacted Link record is at most 72 bytes.

The capture happens from the menu. Controller inputs reflect that capture
frame, and the game can continue updating while the live menu is open.
The native camera update owner, camera collision query, ledge action owner,
and collision-aware CCT sweep remain unverified. The build adds read-only
camera evidence to compare the two snapshots.
