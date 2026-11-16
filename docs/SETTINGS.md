# Settings and controls

The settings menu in the headset has what a player would change. The files below, in
`/data/doax3vr2`, are read as the game starts and have the rest; a change to them takes effect
at the next start of the game.

## Controls

On top of the game's own (every button still reaches the game, except while the menu is open):

- **L1 + R1, then a click of the touchpad**: the settings menu (below).
- **Hold OPTIONS for a second**: reset the view. Where you sit and where you look become the
  centre.
- **First press of circle**, with `agent.recentre=1`: takes your seat where you are then.
- **Shake the controller** in 2D Owner mode with the Jet Black Fan: the fan (the game turns fans
  off in its VR scenes).

## The settings menu

Hold **L1 + R1** and click the **touchpad**: the menu opens a little below straight ahead in VR
scenes, and low in the middle of the game's flat pages (the title and menus, in the headset and on
the TV). The same closes it. While it is open the game gets no input: **up/down** choose a line,
**left/right** (or **cross**) change it, **circle** closes it. Lines marked `*` take effect at the
next start of the game; the others at once.

| Line | Default | Choices | `cvars.txt` name |
|---|---|---|---|
| Resolution\* | sharp+ at 120 fps, 2048 at 60 fps | see below | `agent.res` (0 to 6) |
| Hand tracking\* | on | on, off | `agent.hands` |
| 120 fps\* | on | on, off | `agent.fps120` |
| Fade inside her\* | off | off, on | `agent.headfade` |
| World scale\* | 1.2 (the game's) | 1.0, 1.1, 1.2 (the game's), 1.3 | `agent.worldscale` (100 to 130) |
| Real height | off | on, off | `agent.realheight` |
| Touch\* | on | on, off | `agent.touch` |
| Controller | in both hands | in both hands, in one hand | `agent.padmode` (0, 1) |
| Look at you | on | on, off | `agent.lookat` |
| Eye-tracked sharp area\* | on | on, off | `agent.gazefollow` |
| Sharpening | 50 | off, 50, 100 | `agent.sharpen` (0 to 100) |
| Fan shake | the game's (2 swings) | the game's, easy (1 swing) | `agent.fanswings` |
| Malfunction, all suits | on | on, off | `agent.malfunction` |
| TV picture | left eye | left eye, both eyes, black | `agent.mirrorboth`, `agent.tvblank` |

Then **Reset view** (as holding OPTIONS), **Reset all** (every line back to its default, and the
saved choices removed) and **Close**.

- **Resolution\***: how sharp the picture is. The game draws each of its four VR views (a wide
  and a sharp middle view per eye) at 940x1052; the presets raise the size it really draws at,
  and the agent composes each eye from its two views:

  | Preset | Sizes | Frame rate |
  |---|---|---|
  | 0 game's own | views 940, eye 940 | both |
  | 1 sharp | sharp views 1280 over a smaller area, outer views 940, eye 1400 | 120 fps |
  | 2 wide area | every view 1280, eye 1400 | 60 fps only |
  | 3 2048 | every view 1280, eye 2048 (supersampled) | 60 fps only; default at 60 |
  | 4 2400 | as 3, eye 2400 | 60 fps only |
  | 5 sharp+ | sharp views 1440, outer views 700, both a little shorter (the panel's pixel shape), eye 1440 | 120 fps; default at 120 |
  | 6 2720 | as 3, eye 2720 | 60 fps only |

  A 60 fps-only preset chosen at 120 fps is taken as 1. sharp+ raises the game's base render
  size from 1920x1080 to 2688x1512, which is set once at the game's boot: the **flat screens**
  (title, Owner mode) are drawn at that size too and scaled down for the TV, and the game takes
  one more 512 MB block of memory. If that cannot be patched, the payload uses preset 1. An
  older menu file's `agent.resolution=N` (the former "Eye width") is mapped to a preset. The log's
  `resolution:` line says what was applied.
- **Hand tracking\***: PS VR2's palm tracking places the controller. Held in a hand, the
  controller follows that hand; not held, one hand is followed. It starts once a session, only
  after the headset has been worn for 5 s with no system screen up, and never mid-game.
- **120 fps\***: on, a new picture for every refresh of the headset, from the newest head pose,
  while the game's clock stays at 60 steps a second. The game's per-picture timers (water and
  animated materials, the seat's settling, countdowns, the scene's light fades) are held at their
  60 fps speed. Off: the game's own 60 fps, each picture shown twice.
- **Fade inside her\***: off, no black fade as your head goes into a girl and no blackout when it
  is inside her (her reactions are kept). On: the game's own behaviour.
- **World scale\***: the game draws its VR scenes 1.2 times the models' size while your head's
  movement and eye spacing stay real. 1.0 shows the girls at their models' own size; 1.3 larger
  still. Your controller and the touch follow. Out of VR nothing changes.
- **Controller**: where the hand tracking puts the controller. *In both hands*: between your palms
  while both are seen, as in Summer Lesson, Ghost-VR and Astro Bot. *In one hand*: the hand holding
  it, or one hand when it is put down (newer, less tried). Taken at once.
- **Real height**: in Photo Paradise's places and poses, the scene's seat is raised so your eye
  is as high above the sand as your head is above your floor (the game seats everyone at the same
  height). The girls' gaze and the touch follow; head tracking is not changed. It is taken at
  each scene start and at each view reset, within -0.3 to +0.8 m. VR Paradise's fixed viewpoints
  and scripted scenes are left as they are.
- **Touch\***: the VR touch the games shipped without: your controller's and your head's
  collision spheres, made as the game would make them, so the controller touches (with the
  game's rumble and reactions) and your head pushes the soft bodies. `agent.touchhead=0` in
  `cvars.txt` leaves the head out.
- **Look at you**: in VR Paradise, when the close-contact count rises, the girl looks at you (a
  call the game was written to make but never reaches).
- **Eye-tracked sharp area\***: the sharp middle view follows your eyes through PS VR2's eye
  tracking, and the cutout in the wide views moves with it. Off: the sharp area stays in the
  middle. With eye tracking off in the system settings, or the eyes lost, it eases back to the
  middle.
- **Sharpening**: the composed eye sharpened, live.
- **Fan shake**: the Jet Black Fan needs two strong swings within a few hundredths of a second;
  *easy* makes one swing enough.
- **Malfunction, all suits**: Scarlet's 15 costumes that lack the malfunction flag (AYA_sh_07,
  HON_tk_06, OWN_uw_03 sets) get it, as in Fortune.
- **TV picture**: the left eye, both eyes side by side, or black.

The choices are saved per game in `menu-fortune.txt` / `menu-scarlet.txt`, within 2 s of a
change: one `name=value` line (in `cvars.txt`'s names) for each setting that differs from its
default. The file is read after `cvars.txt`, so the menu's choices win.

## `cvars.txt`

One setting a line, `agent.name=value`. `cvars-fortune.txt` and `cvars-scarlet.txt` are read
after it, for that game only, and their lines win. A line for an option this build doesn't have
is logged as unknown and ignored.

Every line of the settings menu can be set here by its name (table above). The others:

### Game content

- `agent.ownerlv=N` (1 to 110): the Owner Level kept at least N. A higher level unlocks its
  items at the game's next unlock pass (after an activity, or the next morning): in Scarlet the
  Softness Sensors at 20, the Golden and Silver Fans at 60, the Jet Black Fan at 70, the Skincare
  EX at 110; in Fortune at 110 the fans, skincare, figurines, the Hourglass, the Secret Key, the
  Moebius pose cards and more. The game saves the level.
- `agent.money=N`: the Zack Dollars kept at least N, up to the game's cap (Scarlet 3,000,000,
  Fortune 1,500,000).
- `agent.special=N`: the Owner Shop's Special tab, which sells the PSN-only swimsuits and sets,
  offline. The PSN clock it waits for is answered with the console's clock.
  - `2` (the default): everything on sale at once (Scarlet 375 entries, Fortune 363).
  - `1`: Scarlet's own weekly rotation, plus every item no week lists (Fortune: as 2).
  - `0`: left to PSN (empty offline).

  Items sold for Premium Tickets only (tickets need PSN) also get a Zack Dollar price, 50,000 a
  ticket, at most the wallet's cap. A set bought for Zack Dollars is one gift in its Premium
  Wrapping; given to a girl, all its suits go into her belongings.
- `agent.cutfeatures=0`: the cut features left as the game has them. By default the payload
  re-enables the items, poses and unlocks switched off for PS4, and in Scarlet the shop filter
  that kept them out.
- `agent.sensors=1`: the Softness Sensors stocked in the shops. By default they are kept out:
  they do nothing on PS4.
- `agent.vrpassport=0` / `=1`: the VR Passport patch never / always applied. Without the line it
  is applied only when the game's VR Passport add-on is not installed (Scarlet
  `00DLC00130000000`, Fortune `10DLC00000000000`).
- `agent.vrunlock=1`: the add-on list the game reads at boot also lists five `CATEGORY…` names
  found in the games' code as installed. Most likely PS Store category ids: probably no effect.
  `2` only logs the game's queries.
- `agent.port=0`: the game's own package data and models only. By default the agent serves
  files you put in the game's port directory in place of the game's own: Scarlet
  `/data/doax3vr2/port/`, Fortune `/data/doax3vr2/port-fortune/`. A `NN/package_data.bin`
  replaces the game's `/app0/NN/package_data.bin`, and `<pkg>/character/...` model files replace
  the game's loose ones. None of these files is included; they are built from your own dumps of
  the games. Used for:
  - Fortune's swimsuits in Scarlet: `port/03/package_data.bin` (README).
  - The break parts of Scarlet's restored malfunction costumes: Fortune's models for Marie Rose
    (all 15), Honoka and Kasumi (AYA_sh_07, HON_tk_06), with packages 00DAT02, 03 and 12 listing
    them as loose files.
  - The Switch version's three Fan☆ in Fortune: `port-fortune/15/package_data.bin` with their
    rows added.
- `agent.ownersuits=1` (Scarlet): the Owner Shop's Swimsuits tab's daily draw also takes the
  add-on and ported suits (group 6).
- `agent.starfans=1` (Scarlet): the three Fan☆ get the Owner Shop flag the plain fans have.
- `agent.portshop=N` (1 to 8; Scarlet): the Swimsuits tab stocked with batch N of the 45 ported
  swimsuits.
- `agent.portgive=1` (Scarlet): the 45 ported swimsuits put in Kasumi's closet and taken out of
  the owner's belongings. In her room at night: View closet (the last pages), Take out, then
  Equip. Saved with the game's next save; needed once.
- `agent.wfix=1` (Scarlet): repairs equipped marks (W) left by an earlier build, once.
- `agent.errdialog=1`: the system's PSN error dialog at the title shown (hidden by default).
- `agent.bootnotes=1`: the game's start-up boxes shown (the autosave notice; Fortune's offline
  shop-data message). Skipped by default.

### Picture, tracking and timing

- `agent.recentre=1`: the first press of circle also takes your seat.
- `agent.padreach=0` keeps the game's 1.5 reach gain (by default undone, so the controller is at
  your hands). `agent.handpredict=0` / `agent.padimu=0` turn off the controller's prediction;
  `agent.padlead=N` (ms, 0 to 60, default 30) bounds it. `agent.padheld=N` (thousandths, default
  20) is the controller motion that counts as held.
- `agent.movehole=0`: the wide views' cutout not moved (then the sharp area stays in the middle).
- `agent.gpufallback=1`: the game's own VR "GPU too slow" fallback kept (by default it is kept
  from tripping, as it removes the scenery for the rest of a scene).
- `agent.tickhold=0` / `agent.vrtimers=0`: at 120 fps, the game's per-picture timing left as it
  is (twice as fast), for comparison. Both are on by default.
- `agent.composemax=N` (1024 to 4096, default 4096): the composed eye's width at most.
- `agent.compose=0`: the wide views alone (the black disc in the middle shows).
- `agent.netlog=1`: the log sent only to port 9025, nothing written to the console's storage.
- For testing: `agent.fxaa=0` (the game's FXAA pass a plain copy), `agent.eyecap120=0` (no 1400
  eye cap at 120 fps: it stutters).

## `resolution.txt`

The width of an eye view, 672 to 2720 (a debug build: 3200); the height follows the game's
proportion. `stock`: the game's own 940. Used only when the settings menu's file has no
Resolution line; it changes the eye width alone (the preset's render sizes stay, and the eye is
never narrower than them). At 120 fps the eye is at most 1400 (or the preset's own eye).
`resolution-fortune.txt` / `resolution-scarlet.txt` is used instead for that game when present.

## Debug builds

A debug build ([BUILDING.md](BUILDING.md)) adds the tools used to develop the port. A release
build ignores all of the options below (its log says so).

- **The debug menu**, in the settings menu's place: every line of the settings menu (saved the
  same way; Fan shake has 0 to 4 swings here), then debug lines, live and not saved: Owner Level
  and Zack Dollars kept at least, the Special tab, the Fan☆ in the Owner Shop, the Softness
  Sensors stocked, the ported-suit batch, the characters' log, compose on/off and its width, and
  the TV showing the headset's view.
- **L3+R3** marks a moment in the log with the 10 s before and 5 s after.
- `cvars.txt` is read again every 2 s while the game runs, for the options marked *live* and the
  automation's presses and shots.
- An `agent.elf` in `/data/doax3vr2` replaces the built-in agent.

Debug options in `cvars.txt` (a release build ignores them):

- **Automation** (`tools/drive.py`, [TESTING.md](TESTING.md)): `agent.press`, `agent.shot`,
  `agent.shoteye=1` (shots of the right eye), `agent.shotslice=N` (live, 1 to 4: only slice N-1 of
  the game's eye array: wide left, wide right, near left, near right), `agent.novr2=1` (an
  observing run: PS VR2 never started), `agent.capture=N` (live: each new N, the TV's picture into
  the log, read by `tools/fov/capture.py`), `agent.tvhmd=1` (live: the TV shows the headset's
  picture).
- **Game state**: `agent.closet=ID` (live: closet and belongings entries for ids ID to ID+9
  logged), `agent.inv=1` (the belongings logged), `agent.give=1` (the owner given the Jet Black
  Fan, the three Fan☆, Scarlet and Leanan-Sidhe), `agent.giveport=1` (the owner given the 45
  ported swimsuits), `agent.chars=1` (the characters' break state logged), `agent.rowscan=1`,
  `agent.nopassport=1` (the VR Passport add-on hidden from the game, to test the patch),
  `agent.clockhours=N` (live: the offline network clock and the sale list N hours ahead).
- **Timing**: `agent.rtime=1` (per-thread timing), `agent.ticklog=1` (the game's steps a loop,
  the main thread's CPU, each late picture at 120 fps), `agent.halftick=1` (every other loop with
  no step: the game at half speed), `agent.rateprobe=1` (how fast each per-picture timer moves,
  read only), `agent.uihold=1` (at 120 fps, the menu and message layer skipped on loops with no
  game step, so its fades and scrolling text keep their speed; under test).
- **Tracking and touch**: `agent.gaze=1` (the eye tracking read and logged), `agent.touchprobe=1`
  (the game's unused VR touch test run read only), `agent.handload=1` / `agent.handstep=1|2` (hand
  tracking's start in steps), `agent.resetevent=0` (the console's view reset event not passed to
  the game, only the agent's own re-seat), `agent.heightoffset=N` (cm, -30 to 80: added to Real
  height's raise, or the raise itself with Real height off), `agent.padreach=0` (the controller
  at the game's own reach, 1.5 times as far from its VR origin as it is held; by default it lands
  where the hands are), `agent.handpredict` (does nothing).
- **Picture**: `agent.nohole=1` (the wide
  views' middle drawn too), `agent.nearoffset=X,Y` (the sharp view moved off the axis by X,Y
  tangents; needs `agent.nohole=1` or `agent.movehole=1`), `agent.movehole=2` (the moving cutout's
  test: agent-built copies of the game's masks), `agent.sharpenwide=N` (live: the wide views
  sharpened too), `agent.composefast=N` (live, default 3: the compose pass's savings, 0 the pass
  without them, for an A/B of its GPU time).
- **Raw view sizes** (over the Resolution preset's, each on its own; 0 turns one off):
  - `agent.renderw=N` (941 to 1280): the sharp views' render size (and the wide ones' without
    `agent.widew`).
  - `agent.widew=M` (320 up to the cap): the wide views on the game's unused render mode 1 at M
    wide.
  - `agent.mode0=W` (2112, 2304, 2496 or 2688): the game's base render size, W x 9W/16, set once
    at boot. It raises the 1280 cap of `renderw` and `widew` to 1487, 1536, 1716 or 1792, draws
    the flat screens at that size, and takes one more 512 MB block of memory.
  - `agent.nearn=F` (25 to 49): the sharp view's half angle, F/100 of the top tangent (the
    game's 50); the wide views' cutout and the blend follow.
  - `agent.vdensity=P` (80 to 99): every view's height P% of the game's, for an even pixel
    density (96 matches the panel). `agent.vdensitywide=Q`: the wide views' height with
    `agent.widew`.
  - `agent.nearcull=1`: the sharp views culled with their own field of view (saves nothing
    measurable: the game builds its draw lists in the first view only).
