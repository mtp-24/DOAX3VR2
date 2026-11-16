# Known limits

## The games

| Game | Title id | Version (APP_VER) |
|---|---|---|
| Dead or Alive Xtreme 3 Fortune | CUSA04555 | 01.19 |
| Dead or Alive Xtreme 3 Scarlet | CUSA14173 | 01.08 |
| Dead or Alive Xtreme 3 Scarlet | CUSA14831 | 01.00 (untested on a console) |

The package's VERSION is 01.00 in all three; APP_VER is what tells the builds apart. The payload
checks each game's code, not only its version, before it touches it: another version is left
alone with a notification.

- **Scarlet CUSA14831 1.00** (Asia, no update) is mapped from its eboot, not run on a console.
  Off for it, as they are built on 1.08's update data: `agent.port` (and the ported suits' shop
  and closet), `agent.give` / `giveport`, `agent.wfix`, `agent.starfans`, `agent.malfunction`;
  and `agent.nohole` (no Pro-mode factor to patch). The payload's swimsuit-set patch has nothing
  to do there (1.00 has no such refusal).

- **Without the VR Passport add-on** VR PARADISE opens through the payload's patch in both
  games (tested with the add-on hidden from the game, not uninstalled). With the add-on
  installed the patch is skipped and the game's own check decides.
- The game must start **after** the payload is in: the game sets PS VR up once at its boot, so
  the payload does not go into a game already running.

## Firmware

Tested on firmware **12.40** only. The payload has profiles for 22 firmware versions
([FIRMWARE.md](FIRMWARE.md)); none other has been tried with these games. Hand tracking needs
the firmware's own `libSceVrHand`, which the profiles for 12.00, 12.02, 12.40, 12.60, 12.70,
13.00, 13.20, 13.40, 13.42 and 13.60 have.

## Testing so far

- Both games' VR modes, hand tracking and the settings menu were played worn with an earlier
  release build. Worn tests have been mostly in Scarlet; Fortune's mostly automated.
- **The current release build has not been tried worn.** Its newer features (the Resolution
  presets, Real height, World scale, Look at you, the eye-tracked sharp area, the start-up boxes
  skipped, the controller's held / not held rule) were checked by replaying a recorded worn
  session ([TESTING.md](TESTING.md)).
- **Look at you** has not been seen worn (its code checks pass and its calls are logged).

## Picture and frame rate

- The presets were measured on a replayed worn session (sharp and sharp+ at 120 fps about
  6.6 ms of GPU a picture; 2048, 2400 and 2720 steady at 60 fps), not yet judged worn one by one.
  At 120 fps the eye is at most 1400 wide (wider stuttered), except sharp+'s 1440.
- **sharp+** also draws the flat screens (title, Owner mode) at 2688x1512, scaled down for the
  TV.
- **At 120 fps** the menus' and message windows' fades and scrolling text run twice as fast.
  Water, timers and the scene's fades are held at their 60 fps speed.
- **The TV mirror at 120 fps** showed tearing and a dropped picture; it is now held at 60
  pictures a second in VR, which has not yet been watched on a TV.
- **World scale** other than the game's 1.2: where you sit relative to her is still to be judged
  in the headset. **Real height** acts only in Photo Paradise's places and poses.

## Hand tracking

On by default (`agent.hands=0` turns it off). It starts once a session, only after the headset
has been worn for 5 s with no system screen up and the system has stopped using the headset
cameras ([HOW-IT-WORKS.md](HOW-IT-WORKS.md) 10). The controller is placed between both palms by
default; the settings menu's Controller *in one hand* (the held / put-down rule) was reported
not following the hand on a tester's console and is not yet tuned worn.

## Fans, malfunction, destruction

- **Fans work only in 2D Owner mode**, not in VR scenes: the game turns every fan off in a VR
  scene. To see a malfunction: Owner mode (the hotel room at night, the touchpad), Take photo,
  the Jet Black Fan owned, a [Malfunction] or [Destructible] suit, shake the controller.
- **The game's own shake** wants two strong swings within a few hundredths of a second. The
  menu's Fan shake: easy makes one swing enough.
- **A malfunction can be subtle.** On Kasumi's Lily it is the bottom slipping at the back and
  hip; a front view shows nothing.
- **The 15 Scarlet costumes** given the [Malfunction] tag back (AYA_sh_07 a-c, HON_tk_06 a-c,
  OWN_uw_03 a-i): Scarlet's own models of them have break parts for Helena only. The others need
  Fortune's models in the port directory ([SETTINGS.md](SETTINGS.md), `agent.port`). Honoka's
  and Kasumi's OWN_uw_03 have no break parts in either game.

## Shops and items

- **The Softness Sensors do nothing on PS4.** The Switch's Soft 4D (rumble from the soft-body
  motion) has no code in the PS4 game. They are kept out of the shops by default
  (`agent.sensors=1` stocks them). A shop stock already drawn keeps them until the next day's.
- **Most re-enabled items need a higher Owner Level** and come at the game's next unlock pass.
  `agent.ownerlv` raises it.
- **Shop stock is drawn by the game.** Once unlocked an item is in the shop's pool, but the shop
  picks from its pool each day (the Owner Shop stocks one of the Golden and Silver Fans a day). An
  item missing today can be there another day.
- **Other platforms' items come through too.** The cut-features patch lets everything in the
  data through, including Switch and Vita items. The Switch version's three Fan☆ work as the
  plain fans do in Scarlet's own fan code; the Switch's own code was not available to compare.
- **Add-on content that is not on the disc** (the girls Scarlet has no models for) cannot be
  made.
- `agent.vrunlock=1` most likely does nothing (the `CATEGORY…` names look like PS Store
  category ids, not add-on checks).

## Fortune's swimsuits in Scarlet

- **They behave as Fortune's own rows** (group 6, the same flags and prices): sold by the
  Special tab, as in Fortune. `agent.ownersuits`, `agent.portshop` and `agent.portgive` are other
  ways to get them ([SETTINGS.md](SETTINGS.md)).
- **Four girls only**: their models are on Scarlet's disc for Kasumi, Marie Rose, Honoka and
  Helena only, and `agent.portgive` fills Kasumi's closet only.
- **The package file** is not included and no script to build it is. Once a save owns one of
  these suits, keep the file in place: a belonging whose id has no item row may break a lookup.
- **Bigger Fortune content is not possible yet:** new models (Fortune / Venus, the girls whose
  costume sets are only in Fortune's install) would need a new package and an archive path hash
  that is not known.

## While playing

- **Don't send the payload again while the game is running**, and wait about two minutes after
  a payload quits before sending another: both have brought the console down.
- **Don't turn on PS VR2's see-through view while the game runs**: it has hung the console.
- If ps-patch-system's `patch-server` is running, the payload stays out of the game and says so.
  Stop it and start the game again.
- The game's PSN error dialog at the title is not shown (`agent.errdialog=1` shows it), nor the
  game's autosave notice and shop-data message (`agent.bootnotes=1` shows them).
- **Sound after leaving VR** has not been checked by ear. The picture after leaving VR works, and
  VR can be entered again.
