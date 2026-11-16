# DOAX3VR2

Play the VR modes of **Dead or Alive Xtreme 3 Fortune** and **Dead or Alive Xtreme 3
Scarlet** (PS4) on **PlayStation VR2**, on a jailbroken PS5. A payload puts an in-game
"agent" into the running PS4 game. The agent answers the game's PS VR calls and sends the
frames it draws to PS VR2 through the PS5's own VR libraries, with head tracking. The game
files on disc are not changed.

> Homebrew. Not affiliated with or endorsed by Sony, Koei Tecmo or Team Ninja. You need
> your own copy of the games; nothing from them is included here.

> **Warning: experimental.** It runs with the kernel's help and patches a live game. During
> development the console has hung and once powered itself off. Back up your saves, and
> don't use it on a console you can't afford to reset. The known limits are in
> [docs/LIMITS.md](docs/LIMITS.md).

## What it does

- **PS VR2 for both games' VR modes** (VR PARADISE: Event, Pictorial and Photo Paradise).
  No PS VR, PS Camera or setup dialog is needed. VR mode also works **without the VR Passport
  add-on**, which can no longer be bought.
- **120 fps by default.** A new picture for every refresh of the headset, from the newest head
  pose, while the game keeps its own 60 steps a second (her movement, the physics and the sound
  keep their speed).
- **Resolution presets.** The game draws each VR view at 940x1052; the presets raise the size
  it really draws at. The default is *sharp+* at 120 fps and *2048* at 60 fps
  ([docs/SETTINGS.md](docs/SETTINGS.md)).
- **Eye-tracked sharp area**: the sharp middle of the picture follows your gaze.
- **Touch and head push**: the VR controller touch the games shipped without, with the game's
  own rumble and reactions; your head pushes the soft bodies too.
- **Look at you**: in VR Paradise, when close contact rises, the girl looks at you.
- **Real height** (off by default): in Photo Paradise's places and poses your eye is as high above the sand as
  your head is above your floor.
- **World scale** 1.2 (the game's own) by default; 1.0 is true size.
- **No black fade or blackout** when your head goes inside a girl (the menu's Fade inside her).
- **The scenery stays.** The game's VR "GPU too slow" fallback, which removed the palms, huts,
  hills and boats for the rest of a scene, is kept from tripping.
- **Hand tracking**: PS VR2's palm tracking places the controller in your hands (the menu's
  Controller: in both hands, the default, or in one hand).
- **Start-up boxes skipped**: the autosave notice, and Fortune's offline "Failed to download the
  updated shop data".
- **The headset outside VR** shows the game's flat picture (the title, the vacation). **The TV**
  shows the left eye (or both eyes, or black).
- **Everything obtainable offline**: the Owner Shop's Special tab, which sold the PSN-only
  swimsuits, works offline in both games, with everything on sale and Premium-Ticket items also
  sold for Zack Dollars.
- **Cut content re-enabled** in both games: items and unlocks that are in the game's data but
  switched off for PS4 (fans, skincare, figurines, the Hourglass, the Secret Key, pose cards).
  Most need a higher Owner Level; `agent.ownerlv` and `agent.money` can set it.
- **Wardrobe malfunction and destruction with the Jet Black Fan**, in 2D Owner mode: shake the
  controller (the menu's **Fan shake: easy** makes one swing enough). The 15 Scarlet costumes
  that lost the [Malfunction] tag get it back.
- **Optional, with files you build from your own game dumps** (not included): Fortune's five
  missing swimsuit designs in Scarlet (below), Fortune's break models for Scarlet's restored
  malfunction costumes, and the Switch version's three Fan☆ in Fortune
  ([docs/SETTINGS.md](docs/SETTINGS.md), `agent.port`).
- **A settings menu** in the headset and on the TV (L1 + R1, then the touchpad), saved per game.

## Supported

| Game | Title id | Version (APP_VER) |
|---|---|---|
| Dead or Alive Xtreme 3 Fortune | CUSA04555 | 01.19 |
| Dead or Alive Xtreme 3 Scarlet | CUSA14173 | 01.08 |
| Dead or Alive Xtreme 3 Scarlet (Asia) | CUSA14831 | 01.00 |

CUSA14831 1.00 is mapped from its eboot and **untested on a console**. It has no update, so the
features built on 1.08's update data are off for it: `agent.port` (and the ported suits' shop and
closet), `agent.give` / `giveport`, `agent.wfix`, `agent.starfans` and `agent.malfunction`; and
`agent.nohole` (its wide views have no Pro-mode factor to patch). It shares Scarlet's settings
files.

The payload checks the game's code, not only its version, before it touches it. Any other
version is left alone with a notification.

Firmware: tested on **12.40** only. The payload has profiles for 22 firmware versions
([docs/FIRMWARE.md](docs/FIRMWARE.md)); no other has been tried with these games.

**Another version, region or firmware:** the payload saves the files needed for adding it by
itself (keep the game open about a minute, until the notice says they are saved); give the
developer the `.tar` it names
([docs/DUMP.md](docs/DUMP.md)).

## Requirements

- A PS5 on firmware 12.40, jailbroken, with kstuff and an ELF loader listening on port 9021
  (elfldr, etaHEN or similar)
- One of the games above, at that version. Fortune's VR mode came with its updates.
- PS VR2, connected and set up

## Install and first run

1. Build `payload/doax3vr2.elf` (`make RELEASE=1`; [docs/BUILDING.md](docs/BUILDING.md)), or use
   a release payload.
2. With the game **closed**, send it to the ELF loader on port 9021 (with netcat or any payload
   sender). It stays in the background and waits for the game. It must be in before the game
   starts: a game already running is not entered.
3. The first time, it builds the PS VR2 libraries and a shader from the console's own firmware
   into `/data/doax3vr2` (a few seconds, once).
4. Start the game from the home screen, choose VR PARADISE, and put the headset on.

Everything lives in `/data/doax3vr2`: the settings files and a log of each session,
`agent.log`. If something doesn't work, look there first.

## Controls

- **Hold OPTIONS for a second**: reset the view.
- **Hold L1 + R1 and click the touchpad**: the settings menu. Up/down choose, left/right (or
  cross) change, circle closes. Lines marked `*` take effect at the next start of the game.

All settings: [docs/SETTINGS.md](docs/SETTINGS.md).

### Fortune's swimsuits in Scarlet (optional)

Scarlet's disc has the models of five Fortune swimsuit designs (Clam, Sandwich, Illusion,
Pistachio, Salmon Roe: 45 suits) but not their item rows. To add them, put a rebuilt copy of
Scarlet's `/app0/03/package_data.bin` with Fortune's 54 missing rows appended at
`/data/doax3vr2/port/03/package_data.bin`. The agent serves it to Scarlet in place of the game's
own (`agent.port`, on by default), and the log says `port: item map 1286 rows` (the game's own:
1232). The Special tab then sells them. The file is made from your own dumps of both games; no
script for it is included. Once your save owns one of these suits, keep the file in place.

## Status

Tested on firmware 12.40. Both games' VR modes, hand tracking and the settings menu have been
played with the headset worn; most newer features were checked by replaying a recorded worn
session from a PC ([docs/TESTING.md](docs/TESTING.md)). **The current release build has not
yet been tried worn.** Known problems: [docs/LIMITS.md](docs/LIMITS.md).

Don't send the payload again while a game runs, and don't turn on PS VR2's see-through view in
the game: both have hung the console.

## Documentation

- [RELEASE-NOTES.md](RELEASE-NOTES.md): this test release: what's new, the defaults, what to test
- [docs/SETTINGS.md](docs/SETTINGS.md): the settings menu, `cvars.txt`, `resolution.txt`, the
  controls, the debug build's extras
- [docs/LIMITS.md](docs/LIMITS.md): versions, firmware and the known problems
- [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md): the mechanism: the payload, the agent, the
  PS VR2 libraries in a PS4 process, the GPU work, tracking
- [docs/DOAX3.md](docs/DOAX3.md): what the two games do with PS VR and their data, and what the
  port changes, with addresses
- [docs/BUILDING.md](docs/BUILDING.md): building, release and debug builds, the source
- [docs/TESTING.md](docs/TESTING.md): driving the games from a PC without a player
- [docs/FIRMWARE.md](docs/FIRMWARE.md): the firmware profiles

## Credits

- theghostonline, for [AstroBotVR2](https://theghostonline.dev/theghostonline/AstroBotVR2),
  which this is built on, and [PS5VR](https://theghostonline.dev/theghostonline/PS5VR), where
  the hand tracking comes from; and for Ghost VR, where the memory reserve, the exit logging
  and the `patch-server` check come from.
- AstroQuest, for how a PS VR game is seated and its controller placed.
- [shadPS4](https://github.com/shadps4-emu/shadPS4), for the PS VR structure layouts.
- John Törnblom, for the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk),
  `pt.c` and the fake-SELF format.
- idlesauce, for [ps5-self-pager](https://github.com/idlesauce/ps5-self-pager).
- The people who sent firmware dumps for the profiles in `common/firmware.h`.

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
