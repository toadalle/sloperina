# Fritchii validation checklist

Install the build for this branch using the `lichii/load` layout in the Actions
artifact. Keep the previous mod folder available for rollback and use one
SloperinaGZ build at a time. The transition and native controller paths are
primarily for OoT 1.1.1. The controls, dashboard, and text capture are also
available on 1.0.0 where the corresponding fields can be read.

1. **Menu and HUD.** On a map with Link and visible animated actors, open with
   D-pad right + A. Expect a narrow vertical menu at the top left and a static
   backdrop with Link, actors, and the stock HUD visible. On RGBA8, the frozen
   menu background uses the opacity setting. The header should move from
   FREEZING to PAUSED. Turn off Settings > Freeze game while open and check
   that animations advance behind the menu. Navigate using D-pad and left
   stick, then close with B or D-pad right + A while holding a stick off center.
   The held stick should stay suppressed until centered. Plus alone should
   still open the stock pause screen. While playing the ocarina, hold D-pad right and
   press A. The note should play and the practice menu should stay closed.
   While hovering, open a dialogue box and verify that A and B work normally
   until the box closes. Hover should resume after those buttons are released. Test opening from a bumper frame-advance
   hold and verify that closing returns to the hold. If backdrop capture
   fails, native pause still runs and the log reports the capture failure.
   The game can hide actors during this fallback. If native pause itself
   is unavailable, the dashboard stays LIVE. Check Settings > Show gameplay
   mini-HUD or use D-pad left + B to hide or show the mod's watch overlay.
2. **Flight presets.** Start hover with L3. At low and high presets, compare
   horizontal travel and the distance covered by holding X or B for the same
   time. Y/A step the preset, D-pad right + R3 sets it to 6, and X+B holds altitude.
   The base rise speed is the X/B speed at preset 6. Change Settings →
   Reset flight speed on hover toggle. Confirm that speed resets on both L3
   edges when enabled and is retained on both edges when disabled.
3. **Recovery.** Hover in place for several seconds in normal and ghost mode,
   then move. In ghost mode, void out, wait until gameplay is back, and press
   L3 again. Repeat around a natural scene exit, including with a record search
   in progress. The old hover should release at transition, actor replacement,
   or a large relocation. Check for ordinary collision on arrival and capture
   the log if a black screen remains or hover fails to restart.
4. **Collision outlines.** In Scene > Collision, try the layers separately.
   Check yellow loading volumes at Dodongo's Cavern, Forest Temple and the
   Kakariko grotto, violet fall-safety cylinders below grotto entrances, red
   standalone player void-out triggers, and blue invisible barriers. The menu
   lists counts by category and the nearest standalone void-out trigger. The
   overlay draws through scenery and caps each frame at 20 volumes and 720
   rectangle commands. Dense terrain wireframe needs a separate batched GPU path.
   The small cross marks a sensor's center when that center is in view.
   Barrier outlines have no center marker. On 1.1.1, a larger orange cross
   marks Link's first sampled position during the most recent fall event in
   the current map. It is a recorded point, not a trigger boundary.
5. **Camera and ledges.** Capture Quick Snapshot in open space, then fly through
   a wall or near a ledge. If the camera or Link gets stuck, immediately take
   another Quick Snapshot, followed by Deep Diagnostics. Include the map,
   route, controls and expected behavior with the two captures.
   If a traversal action briefly swaps Link's controller, check that the log
   records a deferred collision restore followed by restore to the original
   mode when the original body returns. Test ordinary walking separately.
   D-pad right + R3 toggles wall-pass. A pass now requires native ground contact
   at entry and temporarily holds height while passing.

## Investigating a loading stall

The supplied Dodongo Cavern dump has a grounded Link on the destination map,
no active transition request, and no mod-owned ghost controller. Its log
shows ghost collision restored before the transition. This is an arrival
sample, so it cannot attribute an intermittent stall. The new build writes
`transition:` breadcrumbs and flushes them to the mod log as map reservations
and transition pauses change. If the game hangs, copy `sloperinaGZ/log.txt`
from the virtual SD before restarting. A restart creates a fresh log. Compare
the same entrance with the mod disabled and with hover off, normal hover and
ghost enabled. Record whether the screen was black and whether the emulator
still displayed new frames.

## Export diagnostics

Open Debug Tools → Diagnostics and select **Quick Snapshot** or **Deep
Diagnostics**. Each successful capture reports its guest SD path in the mod
log. Find `sloperinaGZ/sloperina-devdump-*` in the emulator's **virtual SD
card** folder. The `ootgz:` prefix is the guest mount name. Folder names use
the guest's tick counter and a capture number. Zip the selected folders on the
host to share them. A manifest lists the build, sample conditions, addresses,
field offsets, unavailable fields and any binary write errors. The report is
bounded to 8 KiB. Deep Diagnostics can also include a redacted Link record
(up to 72 bytes) and the camera eye/look-at vector pair (24 bytes).

The capture happens from the menu. Controller inputs reflect that capture
frame, and the game can continue updating while the live menu is open.
The native camera update owner, camera collision query, ledge action owner,
and collision-aware CCT sweep remain unverified. The build adds read-only
camera evidence to compare the two snapshots.

## Tracing an out-of-bounds fall

In 1.1.1, reproduce the fall with the practice menu closed. After Link returns,
take a Quick Snapshot or Deep Diagnostics. The manifest keeps the last observed
`DmF_SY_FallDown`, the native abyss type if readable, and Link's coordinates
before and during the event. It also logs the event to the SD card. The fall
flow is shared by several abyss types, so its name alone does not identify the
predicate that triggered it. See [FINDINGS](FINDINGS.md#96-abyss-event-tracing).
