# Hover controls baseline (v0.0.3)

Base: upstream commit `cad59954d64209a59f57823177d65e2e47eec07a` (SloperinaGZ 0.0.3).
Target: Ocarina of Time (Switch 2), update 1.1.1; Fritchii (Lichii fork).

## Controls

- L3 toggles hovering (when the menu is closed, excluding L/R + L3 frame-advance chords).
- R3 toggles collision while hovering. Collision starts enabled each time hovering is turned on.
- X ascends; B descends; release both to hold height.
- Left stick moves in the camera-relative horizontal plane.
- Turning hovering off restores the original collision state through the existing CCT code.

## Build

Use the upstream instructions in `docs/BUILDING.md` and the supported devkitA64 environment. Build from this branch rather than modifying the installation directly. Back up the working release files before installing a new `subsdk9`. The existing 1.1.1 IPS patch should remain unchanged; don't mix modules from different branches.

## Runtime tests (not completed)

1. Confirm the unmodified v0.0.3 release works in Fritchii on game 1.1.1.
2. Activate levitation from Settings; press L3; verify hovering begins with collision enabled.
3. Press/hold X, then release: verify rise followed by a stable hover. Repeat using B for descent.
4. Press R3 once while hovering: verify collision bypass activates; test passing through a wall in an expendable test save.
5. Press R3 again: verify collision returns; confirm Link doesn't continue through solid geometry.
6. Turn hover off with L3: verify a safe landing and restored collision.
7. Turn hover on again: verify collision starts enabled, regardless of the prior toggle.
8. Open/close the menu; ensure inputs aren't accidentally toggled.
9. Test L/R + L3 frame-advance resume combinations for unintended hover changes.
10. Reload a map, test fall/void areas, and review logs for failures involving `cct_noclip_end`.

## Limitations

No compilation, emulator execution, or regression test has been performed yet. The collision toggle uses the existing per-hover `levitate_through_walls` setting. Because this is also serialized in `settings.ini`, enabling a new hover may change its stored value; review this after the initial functional test if persistence should be separated from the active runtime state.

Keep this branch unmerged until tests pass.
