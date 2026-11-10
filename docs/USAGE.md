# Using sloperinaGZ

A quick guide to get you practising. For every option in every menu, see the [manual](MANUAL.md).

## Contents

- [Getting started](#getting-started)
- [Controls](#controls)
- [Shortcuts in the game](#shortcuts-in-the-game)
- [How do I...?](#how-do-i)
- [Things on the screen](#things-on-the-screen)
- [Your settings](#your-settings)
- [Something's not right](#somethings-not-right)

## Getting started

1. **Install it.** Follow [INSTALLATION.md](../INSTALLATION.md) (an emulator's mod folder, Atmosphère, or a patched copy).
   sloperinaGZ works with **The Legend of Zelda: Ocarina of Time (Switch 2), versions 1.0.0 and 1.1.1**.
2. **Start the game.** For a few seconds the bottom-right corner shows sloperinaGZ's name, version and
   build date, the website, and how to open the menu. That means it's running.
3. **Open the menu:** press **D-pad right + Plus**. Press the chord again to close it.

The menu freezes the game over a captured backdrop by default on supported
builds. Turn off **Settings > Freeze game while open** for live inspection.
Menu input stays separate from gameplay in either mode.

## Controls

Navigate with the D-pad or left stick. **A** selects and **B** goes back.

| Button | In the menu |
|---|---|
| **D-pad right + Plus** | Open or close the menu |
| **D-pad up / down** | Move up and down the list |
| **D-pad left / right** | Change a value (switches, numbers), or pick a button on a row of buttons |
| **A** | Open a menu, tick a box or activate an action |
| **L held + D-pad left / right** | Change a number by 10 |
| **B** | Go back one menu or close the dashboard |

The line at the bottom of the menu always tells you what the buttons do for the item you're on.

**What the items look like**

| Looks like | It is | Use |
|---|---|---|
| `items >` | A menu | **L** opens it |
| `☑ name` | A checkbox | **L** (or D-pad left / right) ticks and unticks it |
| `name  < value >` | A switch | **D-pad left / right** (or **L**) changes it |
| `name  < 3 > / 9` | A number | **D-pad left / right**; hold **L** to change by 10; tap **L** to fill (where it says so) |
| grey text | A heading or information | Can't be selected |
| `✕ ☑ 🔧 label ...` | A row of buttons | **D-pad left / right** picks a button (it turns blue), **L** presses it |

## Shortcuts in the game

These work with the menu closed.

| Shortcut | What it does |
|---|---|
| **D-pad right + Plus** | Open the menu |
| **D-pad right + A** | Start a new timer run or stop the current run |
| **D-pad right + B** | Pause or resume the current timer run |
| **D-pad right + Minus** | Hide or restore practice overlays |
| **D-pad right + X** | Toggle the experimental contact void-out bypass (1.1.1) |
| **X / B** (held, while floating) | Levitate up / down; release to hover |

Everything you do shows for a moment in the bottom-right corner (gz's log), for example `infinite magic on` or
`warp to kokiri forest`.

## How do I...?

**...go somewhere?**
**warps > places**, pick the area (overworld child or adult, dungeons, boss rooms, other), the map, then where in
the map. You arrive straight away, with the game's own fade.

**...be adult (or child)?**
**link > age** to **adult**, then **link > reload map**. The map reloads as the adult version. (Sword and shield
swap with the ones you had as the other age.)

**...get an item?**
**inventory > items** (child, adult or universal), **equipment items** (swords, shields, tunics, boots...) or
**collection** (ocarinas, strength, scale, wallets and bags...). Tick it. Switches like **hookshot**
(none / hookshot / longshot) and **big sword** give you one at a time.

**...refill health, magic or rupees?**
**link > health** (or **rupees**), then tap **L**. Magic is in **inventory > collection > magic**.

**...never run out?**
**cheats**: infinite energy, rupees, magic, and each kind of ammo.

**...see my position, speed or angle?**
**watches**: open the folder (📁) and pick from the **known watches**. They show on the screen in a box. Tick or
untick the checkbox on a watch to show or hide just that one.

**...watch a value in memory?**
**watches**, press the **+**, type the address (D-pad left / right to pick a digit, up / down to change it), **L**
to add it. Or open **debug > memory**, find the value, edit the row (**L**) and press **L** on its address.

**...move the watches box?**
**watches > watches box > move**, then the D-pad (hold it to go faster). **L** when it's in place.

**...see the invisible walls?**
**scene > collision > show**. Cyan walls stop Link; red ones are walls he's inside.

**...float around?**
Open **Cheats > Levitation & Ghost** and turn on **Enabled** and **Hover Active**.
While hovering, use the left stick to move, hold **X** to rise or **B** to descend,
and release both to hold altitude. Ghost mode and flight speed are set in the
same menu. Turn off **Hover Active** to restore collision and drop.

Use **Debug Tools > Frame Advance** to pause gameplay, advance one frame, or
resume. Open the menu with D-pad right + Plus even when gameplay is held.

## Things on the screen

| Where | What |
|---|---|
| Top left, under the D-pad icon | The menu |
| Any corner (you choose) | The watches box |
| Bottom right | The log: what you just did, fading out after a second |
| Bottom left | The input display (if it's on) |
| Top left corner | The pause icon, while frame advance has the game paused |
| Over the game | Air walls (if they're on) |

## Your settings

Everything you change in **settings**, **cheats** and **watches** (and the watches box position) is saved by
itself a couple of seconds after you close the menu, and comes back next time you start the game. It's kept on
the SD card in `sloperinaGZ/settings.ini`, plain text you can edit. Your game saves are never touched.

## Something's not right

- **Nothing happens when I press D-pad right + Plus.** Check the sloperinaGZ splash showed at start-up. If not,
  sloperinaGZ needs the module and the IPS for the active game build. See [INSTALLATION.md](../INSTALLATION.md).
- **The game doesn't react to a button.** Frame advance takes **L** (or **D-pad right**) away from the game while it's on,
  hover uses **X/B** and the left stick while active.
- **The screen froze.** Frame advance may have paused the game: open the practice menu and select **Debug Tools > Frame Advance > Resume Gameplay**.
- **Something I ticked didn't change in the game.** Some things only show after the game redraws them: open and
  close the pause screen, or go through a loading zone. Story stages apply after a loading zone.
- **"settings won't be kept".** The game couldn't open the SD card; everything works, it just won't remember your
  settings.

Still stuck? The **log** menu (on the main menu) lists what sloperinaGZ did and what went wrong (set **type** to
**problems**), and everything is also saved to `sloperinaGZ/log.txt` on the SD card: send that file with any
problem report. The [manual](MANUAL.md) explains every option.
