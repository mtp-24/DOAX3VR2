# Dead or Alive Xtreme 3 on PS VR2: what the games do, and what the port does about it

> The general mechanism (the payload, the injection, the PS VR2 libraries in a PS4 process, the
> GPU work) is in [HOW-IT-WORKS.md](HOW-IT-WORKS.md). This file is what is particular to Dead or
> Alive Xtreme 3, read from the two eboots and measured on the console (firmware 12.40).
> Addresses are from the eboot's base (VA = file offset - 0x4000); Scarlet's unless said.

## The games

| | Fortune | Scarlet | Scarlet (Asia) |
|---|---|---|---|
| Title id | CUSA04555 | CUSA14173 | CUSA14831 |
| Content id | HP0106-CUSA04555_00-DOAX300000000000 | JP0106-CUSA14173_00-ISLANDGAMEAPP000 | |
| APP_VER (VERSION is 01.00 in all) | 01.19 | 01.08 | 01.00 (no update; untested on a console) |
| PS VR | came with its updates (1.00 has no PS VR import) | in the game, behind its "VR Passport" add-on | as 1.08 |
| Code | 0x895580 bytes | 0x89ef8c bytes | 0x89d4d4 bytes |

Both games run on Team Ninja's own engine. The two share their PS VR code instruction for instruction
(about 4,700 instructions compared, one float constant differs), at their own addresses. The
payload knows a build by title id, APP_VER and three code stretches (`payload/main.c` `k_builds`).
Scarlet 1.00 is 1.08's code at its own addresses, less two things an update added: the Special
purchase's swimsuit-set refusal and the wide masks' Pro-mode 0.98 (so `agent.nohole` has nothing
to patch, and the moving hole takes the factor as 1.0). It has none of 1.08's update packages
(01-07): the agent's features built on their rows are off for it (`agent.port` and the ported
suits' shop and closet, `agent.give` / `giveport`, `agent.wfix`, `agent.starfans`,
`agent.malfunction`; each says so once in the log).

Both run in the PS4 Pro mode on a PS5. Both ask PSN for shop data at the title; out of reach,
the system's error dialog comes up (NX-102249-8) and holds the controller. The agent does not show
it (`agent.errdialog=1` shows it again).

## How the game uses PS VR

**Once, at its boot** (Fortune 0x5484f9 on): the modules, `sceHmdInitialize315`, the tracker's
memory and `sceVrTrackerInit`, the reprojection's buffers and `sceHmdReprojectionInitialize`, an
event queue and `sceHmdReprojectionSetUserEventStart(queue, 1)`. A failure there and VR is never
offered. So the agent must be in from the entry point.

**Each time VR PARADISE is entered**: the VR service dialog (once), the headset's information
(the setup dialog only if it is not ready), `sceHmdOpen`, the field of view, the camera, the
tracker's devices (the headset and the controller), then on the main thread the display port
closed and opened again with the VR buffer attribute and PS VR's 119.88 Hz mode asked for,
`sceHmdReprojectionSetDisplayBuffers`, and a "VrRenderer Thread" that hands frames over.
**Leaving** (back from the VR menu): that thread stopped, the camera and devices closed,
`sceHmdReprojectionUnsetDisplayBuffers`, the port back, `sceHmdClose`. Every frame in VR mode
needs the camera attached and the headset's status clean, or the game raises its VR error.
The game never reads whether the headset is worn.

**A frame**: the main thread waits for the reprojection's start event until the frame label of
the views it is about to draw into is zero and its port's flip count has moved on by two since its
last frame (60 fps on the 119.88 Hz port); it never flips the port itself in VR mode. Its GPU
commands end with an end-of-pipe event and a wait; the VrRenderer thread wakes on that event and
hands the frame over, which lets the GPU go on. So a frame is complete at its hand-over.
The time step is a 60 Hz tick. The head pose is asked once a frame, predicted 33.3 ms plus the
headset's latency ahead.

**The hand-over** is `sceHmdReprojectionStartWideNear`: each eye twice at one size, a wide view
over the whole field of view with a black disc in its middle, and a near view over the middle
(half the upward tangent each way, about +-34 degrees), twice as dense, which PS VR's
reprojection put into the disc with a blend between two tangent radii (0.6145 and 0.6645). The
four views are slices 0-3 of one 2D array texture (wide left, wide right, near left, near
right), 940x1052 each (672x752 on a base PS4). The game draws each view at that size in its
render mode 3 and its last pass enlarges it to the eye size: only a larger render size (the
Resolution presets) adds detail. Their alpha holds nothing usable. Flat screens (the VR menu,
loading) come by `sceHmdReprojectionStart2dVr`.

## What the agent does in VR

- **The pace** (`hooks.c`): the start event is raised at each refresh of the headset, and the
  game's port reports those refreshes as its flip count while the game is in VR mode: 600 frames
  each 10 s, each shown for two refreshes.
- **At 120 fps** (the default, `agent.fps120`): a frame every refresh, 1,200 each 10 s, while the
  game keeps 60 steps a second: every other loop of the main thread gets no step (`hooks.c`
  `tick_frame`). The game's update still runs on those loops, so what it times by loop would run
  twice as fast. Held at the 60 fps rate: everything stepped by the game speed (`agent.tickhold`:
  the speed 0 for that loop's update: the water's shader time and animated materials, the VR seat
  follow, the timer lists, the camera-effect countdowns) and five frame counters stepped whatever
  the speed (`agent.vrtimers`, `vrreact.c`: the light set crossfade, an overlay fade, a light
  fade, a ramp and its envelope time). Not yet held: the menu and message layer (37 UI managers
  updated each loop with 1/60 s), which `agent.uihold` (debug) skips on loops with no step.
- **The port** is kept open across the game's close and open (the same handle given back): the TV
  mirror's buffers and PS VR2's output stay on it.
- **The eyes** (`compose.c`): each eye written into one texture of the agent's own from its wide
  and near view, at the hand-over. The slices themselves stay as the game gives them (a slice is
  not a 2D texture at an offset: slices after the first are laid out with their banks rotated).
- **The VR port takes two buffer sets.** The flat screens' textures go into the first
  registration, the composed eyes are the second; a third is refused (0x8029000a).
- **Labels**: the label of the frame before is zeroed at each hand-over, of whichever kind, and
  the last one when VR mode is entered or left (the game waits for a zero label without a timeout).
- **The GPU fallback**: the game's VR "GPU too slow" fallback hides every stage node but 19 for
  the rest of a scene once tripped. The payload makes its `jbe` (Scarlet 0x4a1f64, Fortune
  0x4a12f4) a `jmp`.
- **Out of VR** the headset shows the game's flat picture (the title, the vacation), the buffer
  the TV shows, at the game's own rate.

## The payload's patches at the entry point

All check the game's bytes first and leave a site alone if they differ.

- **Resolution** (both games): [HOW-IT-WORKS.md](HOW-IT-WORKS.md) 3 and
  [SETTINGS.md](SETTINGS.md).
- **Cut features** (both games, `patch_cut_features`; `agent.cutfeatures=0` leaves them as the
  game has them). Unlock rows carry a per-platform marker that the code tests at six sites:
  Scarlet skips markers 4, 5 and 6, Fortune 4 and 5. Each test's immediate is changed so no row
  is skipped. The item master's availability test is NOPed. Then, in Scarlet, the shop filter's
  two availability tests (0x190248, 0x190279) are NOPed: without that the fans, skincare and
  Softness Sensors were still rejected by every shop. Everything present in the data is let
  through, other platforms' items too. Log: `cut features: 6/6 unlock sites patched, item flag
  cleared, shop filter 2/2`.
- **VR Passport, Scarlet** (`patch_vr_passport`). Scarlet asks PSN for its service
  entitlements; out of reach, a flag (0xaaf188) and a menu item's active byte (0xaaf173) stay 0.
  The two `mov byte ..., 0` that write them (0x42065d, 0x41ca61) are made to write 1. VR PARADISE
  then opens. Skipped when Scarlet's add-on `00DLC00130000000` is installed
  (`/user/addcont/CUSA14173`), or with `agent.vrpassport=0`; `agent.vrpassport=1` applies it
  anyway.
- **VR Passport, Fortune**: the same two sites, found by the same code around them: the flag
  0xaa7098 cleared at 0x41e3a5 after the PSN check, the menu item's active byte 0xaa7083 cleared
  at 0x41a741; both made to write 1. Its add-on `10DLC00000000000` is more than a key: installed,
  the game mounts it and loads a package from it, so reporting it installed when it is not crashes
  the game. The patch leaves the add-on list alone.
- **Start-up boxes** (both games, `patch_bootnotes`): the autosave notice and Fortune's offline
  "Failed to download the updated shop data" (text ids 0x302bf, 0x302ed; three sites a game) are
  skipped as if Next were pressed (`agent.bootnotes=1` shows them).
- **Fade inside her** (`patch_headfade`), **World scale** (`patch_worldscale`) and the **GPU
  fallback** (`patch_gpufallback`): [HOW-IT-WORKS.md](HOW-IT-WORKS.md) 3.

## The agent's game-side features

Each runs on the agent's 2-second poll, checks the game's code before writing, and walks the
game's own structures (no memory search).

- **Add-on entitlements** (both games, `agent.vrunlock=1`, off by default): the add-on list the
  game reads at boot (`sceAppContentGetAddcontInfoList`) is reported with the five `CATEGORY…`
  names installed. In Scarlet the code uses those names only at 0x3f8809-0x3f8894, 0x420cb2 and
  0x428efb, where a switch on the shop builds the name its online page opens: PS Store category
  ids, most likely.
- **Scarlet's add-on swimsuits** (`agent.ownersuits=1`, off by default): 279 owner swimsuits are
  group 6 (+0x1c), all modelled on the disc for Marie Rose, Honoka, Helena and Kasumi: the suits
  of the swimsuit sets (0x469-0x48b, 0x4cf-0x522: "Swimsuit Set No. 1-12" and sets named after
  Fortune's add-ons). Group 6 is listed only by the Owner Shop's Special tab (0x4249f0). The
  Swimsuits tab's two filters (0x424020: the partner's at 0x4240b8, the other girls' at 0x424201,
  `movw $0, disp8(%rbp)`) are made 0xffff (any group); only groups 0 and 6 have Owner Shop
  swimsuits, so exactly these and the 45 ported suits join the daily draw. Fortune is the same:
  its Special tab (0x423150) lists group 6, and its catalog add-ons give models only.
- **The star fans** (`agent.starfans=1`, off by default; Scarlet): the Fan☆ rows 0x538-0x53a differ
  from the plain fans' (0x2a0, 0x2a1, 0x3be) only in id, name, the Owner Shop flag +0x1f (0) and
  byte +0x39. The agent sets +0x1f. The fan code takes them: 0x408780 returns bit 1-2 for a
  Golden/Silver Fan or its ☆ (0x4087c7: id-0x2a0 < 2 or id-0x538 < 2) and bit 4 for the plain one
  only; 0x4088d0 the same for the Jet Black Fan (0x3be or 0x53a). The bits (0xab8108) drive two
  wind targets at 0x4848f5 (bit 2: +0x260) and 0x48491a (bit 4: +0x240). Fortune's tables have no
  Fan☆ rows, though its code handles them (0x4064b9, 0x406606) and its MES_ITEM has their names;
  its rows are added through `port-fortune/15/package_data.bin`.
- **Owner Level and money** (both games, `agent.ownerlv`, `agent.money`): O = `**P`, Owner Level
  at O+0x20 (EXP into it at O+0x1c), Zack Dollars at `*(O+0x08)+0x58`. P is Scarlet 0xaa9e40,
  Fortune 0xaa1de0: the global the mission-reward code loads (Scarlet 0x3ad379, Fortune 0x3ad8b9)
  70 bytes before the same level-cap test (`cmp $0x6e`); the agent checks both instructions'
  bytes. Money caps: Scarlet 3,000,000, Fortune 1,500,000 (the game's own clamp). A raised level
  unlocks its items at the next unlock pass.
- **Fan swings** (`agent.fanswings`, default 0 = the game's own 2). The Jet Black Fan's thread
  wants two strong swings within two motion samples, a few hundredths of a second. With 1, one
  swing fires it.
- **The malfunction** (Scarlet). What breaks a suit is the model's `COS_BREAK_DISP_TYPE`
  (malfunction) or `COS_BURST_DISP_TYPE` (destruction), set off by the Jet Black Fan and a shake
  of the controller; the item's label flag only drives the "[Malfunction]" tag and the
  "Malfunction: On/Off" row. Lily's model and the fan code are byte-identical to Fortune's:
  nothing was censored. Fans work in 2D Owner mode only: the game turns them off in VR scenes
  (byte 0x8d4f84).
- **The 15 costumes without the flag** (Scarlet, `agent.malfunction`, on by default): AYA_sh_07,
  HON_tk_06 and OWN_uw_03 have the flag 1 in Fortune and 0 in Scarlet. The agent writes 1 into
  the Item label map (`*(base+0xbf1960)`); the log says "15 flags written". Of Scarlet's own
  models of them only Helena's have `COS_BREAK_DISP_TYPE` and break meshes; Fortune's models can
  be served for the others (below).
- **The Softness Sensors** (Scarlet): kept out of the shops by default, their rows' shop flags
  (+0x1e, +0x1f) set to 0 in the item map. `agent.sensors=1` stocks them. They do nothing: the
  Switch's Soft 4D code is not in the PS4 game.

## The Special tab offline, and the break parts

- **The Special tab** (`agent.special`): the games date their online sale data with the PSN clock
  (`sceRtcGetCurrentNetworkTick`, NID zO9UL3qIINQ; 0x80b50001 offline); the shop builds its Special
  tab (Scarlet 0x424020 -> 0x427170 -> 0x4249f0) and allows a Special purchase (0x4029b3) only when
  it answers. The agent answers with the console's clock. Scarlet then makes its own schedule into
  the sale list (*(base+0xc51b08)+0xb8, 0x20-byte records: start, end (time_t), id +0x10, Zack
  Dollars +0x14 (-1 tickets only), tickets +0x18): 310 items, weekly. The items no schedule lists
  are appended at their rows' prices (+0x20, +0x24), on sale until 2099-12-31, from the clock hook
  (the shop calls it right before building the tab), in a block from the allocator the list is
  freed with (Scarlet *(base+0x1c1a908), Fortune 0x1c127e8), and only rows whose icon files are in
  the file table (Scarlet 0xc4c5d0, Fortune 0xc444d0; a row without one crashed Fortune's shop).
  Fortune keeps no schedule offline: there the list is the agent's alone (363 items).
- **Break parts** (`agent.port`, `agent.malfunction` on): a btif record (package_data.bin, 0x24
  bytes: hash, type, name, ?, ?, flags +0x14, size +0x18, ...) flagged 0x40000000 is read from the
  package's Archive.psarc (`archive<pkg>:/...`), 0 from a loose file (`/app0/<pkg>/...`). Packages
  00DAT02/03/12 rebuilt with the 54 replaced models flagged loose at Fortune's sizes make the game
  open them as loose files, and the agent serves `/data/doax3vr2/port/<pkg>/character/...` in
  their place (Fios open, Kernel open, stat). Fortune's models (from its app0/01 and patch0/11
  archives, readable with `tools/psarc_pull.py`) carry COS_BREAK_DISP_TYPE and the break meshes.

## Fortune's swimsuits in Scarlet

Scarlet's item table holds every Fortune row but 56. Five swimsuit designs, nine suits each, are
modelled on Scarlet's disc for Kasumi, Marie Rose, Honoka and Helena, with names and icons, but
have no item rows: Clam (0x2fa), Sandwich (0x3ac), Illusion (0x3d8), Pistachio (0x4b7), Salmon
Roe (0x4f7), each +0..8. Also missing: the three Moebius pose cards (0x461-0x463), a manual, and
Fortune / Venus, whose models are not on Scarlet's disc.

- **The package file.** The game stats and opens each `/app0/NN/package_data.bin` at its start
  (`sceKernelStat`, `sceFiosFHOpen`). When `/data/doax3vr2/port/NN/package_data.bin` exists the
  agent serves it instead (`agent.port`, default on). The file is Scarlet's package 03 with
  Fortune's 54 missing rows appended, converted to Scarlet's row layout (0x70 to 0x80 bytes, the
  icon hashes remapped).
- **The item map.** Every package row goes through the merged item map's insert
  (`*(base+0xc4c488)`, an unordered map by id), so new ids are added. Log: `port: item map 1286
  rows` (the game's own 1232).
- **The shop.** The 45 suits are in group 6, which only the Special tab lists; with
  `agent.ownersuits` the Swimsuits tab's draw takes them. `agent.portshop=N` (1 to 8) writes
  batch N of them over the Owner Shop's Swimsuits tab entries, after checking each entry's row
  pointer is the map's.
- **The closet** (`agent.portgive=1`): the agent calls the game's own closet functions, 0x383210
  ("obtained") and 0x3834d0 ("in the closet") on her belongings +0x88 for each id (the save
  loader and the gift path use them), then empties the owner's copies. `agent.wfix=1` puts the W
  (equipped) marks right once.
- **Keep the file** once the save owns one of these suits: a belonging whose id has no row may
  break a lookup.

## Navigating the games

In the VR PARADISE menu (Event / Pictorial / Photo / VR Tutorial): Photo Paradise is down down
circle, circle (Kasumi), circle (pose card), right, circle, circle (swimsuit), up, circle (play).
Event and Pictorial Paradise need a played vacation. Fortune shows no tutorial panels.

Scarlet's Owner Shop: in the hotel room at night, the touchpad (Owner Mode), "Go to the Owner
Shop". Tabs: Swimsuits, Sundries, Gift Wrapping, Special. Circle on an item starts a purchase
(cross cancels).

## Not done

- The menu and message layer's speed at 120 fps (`agent.uihold` needs a flicker check). 90 fps.
- Sound after leaving VR, checked by ear.
- The ported swimsuits in Marie Rose's, Honoka's and Helena's closets (only Kasumi's is done).
- What the games' every-fourth-day swimsuit pick per island girl does (Scarlet 0x38ec9c, Fortune
  0x38e2cd: day % 4 == 3): maybe the girls getting suits themselves, which could bring group 6 in
  offline.
- The malfunction set off by the fan in the vacation with one of the replaced models (seen in
  Photo Paradise with its Malfunction setting).
