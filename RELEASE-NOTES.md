# DOAX3VR2 test release: release notes

A test release of the PS VR2 port of Dead or Alive Xtreme 3 **Fortune** (CUSA04555, APP_VER
01.19) and **Scarlet** (CUSA14173, APP_VER 01.08; and CUSA14831, the Asian release, APP_VER 01.00,
untested on a console, without the features that need 1.08's update data), on a jailbroken PS5 on
firmware 12.40. It is experimental: back up your saves first, and read [Known issues](#known-issues).

## What's new

Since the previous test build (the one with the settings menu on flat pages and hand tracking on
by default):

**Changed defaults since the "final" test payload:** World scale 1.2 (was 1.0), Real height off
(was on), Controller in both hands (was one hand). The menu keeps only the lines you changed, so a
setting you left at its old default now takes the new one; set it again in the menu to keep the
old.

### Picture and frame rate
- **120 fps by default**: a new picture for every refresh of the headset, from your newest head
  pose, while the game keeps its own 60 steps a second (her movement, the physics and the sound
  at their speed). Water, timers, countdowns and the scene's fades are held at their 60 fps speed.
- **Resolution presets** (menu: Resolution, `agent.res` 0 to 6) that raise the size the game
  really draws its VR views at, in place of the old eye width (which only enlarged the game's
  940-wide views): game's own, sharp (120 fps), wide (60 fps),
  **2048 (60 fps)** (the default at 60), 2400 (60 fps), **sharp+ (120 fps)** (the default at 120), 2720 (60 fps).
  A 60 fps preset at 120 fps is taken as sharp. An older menu file's eye width is mapped to a
  preset.
- **Eye-tracked sharp area**: the sharp middle view follows your gaze (PS VR2's eye tracking),
  the cutout in the wide views moving with it.
- **Sharpening** (off, 50 or 100), live.
- **The scenery stays**: the game's VR "GPU too slow" fallback, which cut the palms, huts, hills
  and boats for the rest of a scene after the PS button, the play-area setup or a heavy frame,
  is kept from tripping.
- **The TV mirror** held at 60 pictures a second in VR and paced by the TV (it tore at 120).
- Cheaper compose pass and moving cutout (less GPU and main-thread time).

### In the scenes
- **Touch and head push**: the VR controller touch the games shipped without, with the game's
  rumble and reactions; your head pushes the soft bodies too.
- **Look at you**: in VR Paradise, when close contact rises, the girl looks at you.
- **Real height** (off by default): in Photo Paradise's places and poses the scene's seat is
  raised to your own head height; a view reset takes it again.
- **World scale**: 1.0 (true size), 1.1, 1.2 (the game's, the default) or 1.3.
- **Fade inside her** now off by default, and with it off no blackout when your head is inside.

### Hands
- **The controller follows your hand**: held in a hand, it follows that hand; put down, one hand
  is followed. Placed at the camera's time and moved forward to the display's, with the game's
  1.5 reach gain undone so the controller is at your hands.

### Start-up
- The game's own **start-up boxes are skipped**: the autosave notice (both games) and, offline,
  Fortune's "Failed to download the updated shop data". The PSN error dialog stays hidden.
- The Resolution preset and sizes are worked out again at each game start (a second game no
  longer reuses the first's).

### Fixes
- The menu over the composed eyes blended once (it could come out blended twice).
- After a stall of four frames or more at 120 fps the newest frame is shown, not an old one.
- A failed load at the game's entry no longer leaves the game stopped; the payload's AV restore,
  an unreadable hook slot and the palm tracker's stop order made safe.

### Still in from earlier builds
VR mode without the VR Passport add-on; the headset showing the game's flat picture out of VR;
the offline Owner Shop Special tab (everything on sale, Premium-Ticket items for Zack Dollars);
cut content re-enabled; Fortune's five missing swimsuit designs in Scarlet (with the rebuilt
package file); the malfunction on Scarlet's 15 costumes; the Jet Black Fan's shake (Fan shake:
easy); the settings menu in the headset and on the TV.

## Default settings

The settings menu (L1 + R1, then click the touchpad). `*`: takes from the next start of the game.

| Line | Default | Choices |
|---|---|---|
| Resolution* | sharp+ (120 fps); 2048 (60 fps) with 120 fps off | game's own, sharp (120 fps), wide (60 fps), 2048 (60 fps), 2400 (60 fps), sharp+ (120 fps), 2720 (60 fps) |
| Hand tracking* | on | on, off |
| 120 fps* | on | on, off |
| Fade inside her* | off | off, on |
| World scale* | 1.2 (the game's) | 1.0, 1.1, 1.2, 1.3 |
| Real height | off | on, off |
| Touch* | on | on, off |
| Controller | in both hands | in both hands, in one hand |
| Look at you | on | on, off |
| Eye-tracked sharp area* | on | on, off |
| Sharpening | 50 | off, 50, 100 |
| Fan shake | the game's (2 swings) | the game's, easy (1 swing) |
| Malfunction, all suits | on | on, off |
| TV picture | left eye | left eye, both eyes, black |

Then Reset view, Reset all (every line to its default) and Close. Choices are saved per game in
`/data/doax3vr2/menu-fortune.txt` / `menu-scarlet.txt`. The rest (`cvars.txt`) is in
[docs/SETTINGS.md](docs/SETTINGS.md).

## Install and use

1. Firmware 12.40, jailbroken, with kstuff and an ELF loader on port 9021 (elfldr, etaHEN or
   similar); PS VR2 connected and set up; one of the games at the version above.
2. With the game **closed**, send the release payload (`doax3vr2.elf`) to port 9021. It stays in
   the background and waits for the game. It must be in before the game starts.
3. The first time it builds the PS VR2 libraries and a shader from the console's own firmware
   into `/data/doax3vr2` (a few seconds, once).
4. Start the game from the home screen, choose VR PARADISE, put the headset on.

- **Hold OPTIONS for a second**: reset the view.
- **L1 + R1, then click the touchpad**: the settings menu (up/down choose, left/right or cross
  change, circle closes).
- Each session's log is `/data/doax3vr2/agent.log`: send it with any report.
- Fortune's swimsuits in Scarlet need a rebuilt package file at
  `/data/doax3vr2/port/03/package_data.bin` (README.md); without it everything else works.

## Known issues

- **This release build has not been tried worn yet.** Most of its new features were checked by
  automation (a worn session's head movement replayed); worn tests have been mostly in Scarlet,
  Fortune's mostly automated.
- **Look at you** has not been seen worn.
- **The controller's placement**: *in both hands* (the default) is the rule Summer Lesson, Ghost-VR
  and Astro Bot use; *in one hand* (the menu's Controller) is newer and was reported not following
  the hand on a tester's console.
- **sharp+ (120 fps)**, the default, also draws the flat screens (title, Owner mode) at
  2688x1512, scaled down for the TV.
- **At 120 fps** the menus' and message windows' fades and scrolling text run twice as fast.
- **The TV mirror's** tearing fix has not been watched on a TV.
- **World scale** other than the game's 1.2 (the default): where you sit relative to her
  is still to be judged.
  **Real height** acts in Photo Paradise's places and poses only.
- **The Softness Sensors do nothing on PS4** (the Switch's Soft 4D code is not in the game); they
  are kept out of the shops.
- The ported Fortune swimsuits go to Kasumi's closet only; Honoka's and Kasumi's OWN_uw_03 have
  no break parts.
- Sound after leaving VR has not been checked by ear.
- **Don't** send the payload again while a game runs (wait about two minutes after one quits), and
  **don't** turn on PS VR2's see-through view in the game: both have hung the console.

The full list: [docs/LIMITS.md](docs/LIMITS.md).

## What to look at

1. **Both games, worn**, from the home screen with nothing set: VR PARADISE, a Photo Paradise
   scene, Event or Pictorial Paradise. Does the picture feel smooth at 120 fps as you move your
   head? Any stutter, judder or black frames?
2. **The scenery** after the PS button and back, after the play-area setup, and after view
   resets (hold OPTIONS): do the palms, huts, hills and boats stay?
3. **The eye-tracked sharp area**: is the middle sharp wherever you look, with no black crescent
   or visible edge moving?
4. **Resolution presets**: sharp vs sharp+ at 120 fps; 2048 / 2400 / 2720 with 120 fps off. Which
   looks best, and does any stutter? With sharp+, the title and Owner mode on the TV and in photos.
5. **Touch and Look at you**: touching her with the controller (rumble, reactions), leaning in
   with your head; does she look at you on close contact?
6. **Your hands**: the controller held in both hands, then the menu's Controller *in one hand*
   held in one hand, in the other, put down: does it follow, and how jittery is it?
7. **Real height** on, standing and sitting, with a view reset after changing.
8. **World scale** 1.0 and 1.3: does she feel the right size, and where do you sit?
9. **Fade inside her** off: leaning into her, nothing goes black.
10. **Start-up**: no autosave notice, no shop-data box (Fortune offline); saves still work.
11. **The TV** at 120 fps: tearing or dropped pictures.
12. **The settings menu**: readable, the choices kept after the game restarts.

With any report: the game, what you did, the time, and `/data/doax3vr2/agent.log`.
