# How DOAX3VR2 works

> The mechanism, at a high level. What the two games do with PS VR and their data, with
> addresses, is in [DOAX3.md](DOAX3.md).

Dead or Alive Xtreme 3 Fortune and Scarlet are PS4 games. On a PS5 they run in backwards
compatibility mode: PS4 system libraries, a PS4 process, and a PS4-mode GPU context. PS VR2 is
driven by PS5-only libraries (libSceHmd2 for the headset and its reprojection, libSceVrTracker2
for tracking, libSceAgc and libSceAgcDriver for the GPU). None of them were made to run in a PS4
process.

DOAX3VR2 has two parts:

- **the payload** (`payload/`), a normal PS5 payload that stays resident. It prepares the PS5
  libraries once, puts the agent into the game when it starts, patches a few bytes of the game's
  code at its entry point, and does the things only a kernel-level process can do (mounting
  files into the game's sandbox, re-pointing import slots, patching the AV control service while
  the game runs).
- **the agent** (`agent/`), freestanding code that runs inside the game. It answers the game's
  PS VR calls, drives PS VR2, composes the eyes, serves the game's package files from
  `/data/doax3vr2/port`, and keeps a few of the game's values (Owner Level, flags) where the
  settings ask.

The firmware offsets below are 12.40's, the only firmware this port has run on. Each supported
firmware's own values are in `common/firmware.h`; what differs between them is in
[FIRMWARE.md](FIRMWARE.md).

## 1. First run: preparing the PS VR2 libraries

The payload builds four files in `/data/doax3vr2/libs` from the console's own firmware, and a
fifth (libSceVrHand, section 10) where the firmware's profile has one (`payload/prepare.c`):

1. Each library is read from `/system/common/lib` and decrypted with idlesauce's self pager.
2. The ELF ABI version is changed from 2 to 3. PS5-only libraries are refused in a PS4 process
   ("ABIVERSION mismatch") otherwise.
3. In libSceHmd2, the PLT stub for `sceKernelGetProsperoCompiledSdkVersion` is replaced with code
   that returns SDK 8.05. A PS4 process has no PS5 SDK version, and Hmd2 checks it before
   anything else.
4. In libSceAgcDriver, the address of its 2 MB command-buffer area moves from 0xfe0000000, where
   the game already has a reservation, to 0xfe0400000.
5. The result is wrapped as a fake-signed SELF (the SDK's `make_fself.py` format, written by
   `fself.c`), which kstuff lets the game load.

Each decrypted library is checked against the SHA-256 of its build for the running firmware
before it is changed, so an unknown firmware fails cleanly instead of producing a broken file.
The prepared files are tagged with the firmware they were built for, and rebuilt after a firmware
change.

The same step translates Hmd2's reprojection shader (section 5) and writes it to
`/data/doax3vr2/hmd2_gcn.bin`.

## 2. Getting into the game

The payload watches for the `eboot.bin` of one of the two games (`payload/main.c` `k_builds`:
the title id and the APP_VER its `param.sfo` gives, confirmed by three stretches of the game's
code unique to that build). Then it:

- sets the PS VR2 bit (0x400) in the app info attribute3 the kernel keeps for the process, so
  the system treats the game as a PS VR2 app;
- attaches with ptrace as soon as the game starts, while it is still in libkernel's start code,
  and puts a breakpoint on the eboot's entry point. The main thread stops there before any of the
  game's own code has run;
- maps the agent, applies its relocations, resolves its imports by NID, and points the game's
  import slots (its GOT) for every hooked function at the agent;
- applies its patches to the game's code (section 3), still at the entry point;
- detaches. The game never stops for more than a moment.

Dead or Alive Xtreme 3 sets PS VR up once, at its boot. An agent put in later would never see
that, so the payload goes into a game only at its entry point: a game already running when the
payload is sent is left alone (the log says so).

Any other copy of the game (another region, or a version whose code is none of the known
builds') gets a notification saying which one it is. So do the other ways it can't start: the
wrong firmware, the libraries not loading, ps-patch-system's `patch-server` running.

From then on the payload relays the agent's log (to `/data/doax3vr2/agent.log` and the kernel
log) and serves its requests over a small shared memory block: mount the libraries into the
sandbox, re-point an import of a loaded library, resolve imports of libraries loaded later. When
the game loads a system module, its imports from that module are bound again and the payload
puts the hooks back.

## 3. The payload's patches to the game

Written into the game's code at the entry point, each only where the bytes are the expected
stock ones (`payload/main.c`; details and addresses in [DOAX3.md](DOAX3.md)):

- **Resolution** (`res_settings`, then `patch_mode0`, `patch_vdensity`, `patch_eye_size`,
  `patch_renderw`, `patch_viewsize`, `patch_nearn`): the settings menu's preset sets the game's
  real render size for the sharp views (its render mode 3), the wide views' own size (its unused
  mode 1), the sharp area's angle, the views' height, the base render size (mode 0, for sharp+)
  and the eye width (the two constants the game's last pass enlarges into, 940.8 x 1052.8 in the
  PS4 Pro mode).
- **Cut features** (`patch_cut_features`, both games): the six tests of the unlock rows'
  per-platform marker made never to skip a row, the item master's availability test NOPed, and
  in Scarlet the shop filter's two availability tests NOPed, so the fans, skincare and Softness
  Sensors can be stocked. `agent.cutfeatures=0` skips all of it.
- **VR Passport** (`patch_vr_passport`, both games): the two writes that mark the add-on
  missing after the PSN check fails are made to write 1, so VR PARADISE opens. Skipped when the
  game's own VR Passport add-on is installed.
- **Start-up boxes** (`patch_bootnotes`): each site's text lookup for the autosave notice and
  the shop-data message jumps to the game's own "no such text" path, so no box opens and the code
  goes on as after Next (`agent.bootnotes=1` skips the patch).
- **Fade inside her** (`patch_headfade`): the head-inside-a-girl fade's radius made unreachable
  and the blackout's branch removed (unless `agent.headfade=1`).
- **GPU fallback** (`patch_gpufallback`): the game's VR "GPU too slow" fallback, a one-way latch
  that hides most of the stage (palms, huts, hills, boats) for the rest of a scene once a suspend
  (the PS button, the play-area setup) or a heavy frame sets it off, is kept from tripping
  (`agent.gpufallback=1` leaves it).
- **World scale** (`patch_worldscale`): the 1.2 the four VR scene starts write as the world scale,
  and the scale the draw uses with it, replaced with the chosen one.

## 4. Pretending to be PS VR

The game talks to PS VR through libSceHmd, libSceVrTracker, libSceCamera, libSceHmdSetupDialog
and libSceVrServiceDialog. The agent replaces those calls (`agent/hooks.c`):

- The headset is always connected, and its setup and service dialogs finish as soon as they
  open, so the system's "connect PS VR" screen never comes up.
- The headset reports PS VR2's field of view (tangents 1.8418 out, 0.9472 in, 1.3290 up and
  down) instead of PS VR's, so the game renders what the PS VR2 lenses show.
- The camera is always attached and delivers blank frames (the game checks it on every VR
  frame), and the tracker answers every request with the PS VR2 head pose converted to PS VR's
  tracking space (section 7).
- `sceHmdReprojectionStartWideNear`, where the game hands over each finished frame (four views:
  each eye's wide and near view), is where the frames are collected for PS VR2 (section 6). The
  flat screens (`sceHmdReprojectionStart2dVr`) are collected there too.
- The game's main video output asks for PS VR's 119.88 Hz mode. That is only pretended: with no
  PS VR attached, the real call makes the system suspend the game. The port is kept open across
  the game's close and open, so the TV mirror and PS VR2's output stay on it.
- With `agent.vrunlock=1` (off by default) the add-on list the game reads at boot
  (`sceAppContentGetAddcontInfoList`) also lists five CATEGORY names as installed; probably no effect.
- The system's error dialog for the PSN failure at the title is not shown (`agent.errdialog`).

## 5. The PS VR2 libraries in a PS4 process

Once the game is rendering VR frames, the agent starts a thread that loads the four libraries.
The game's sandbox can't see `/data`, so the payload mounts `/data/doax3vr2/libs` read-only into
the sandbox (nullfs) while the agent loads them, and unmounts it after.

Then the libraries need several things to look like they are in a PS5 app
(`agent/ps5compat.c`). Each one is an import of one library pointed at a wrapper, so the game's
own view of the system doesn't change:

- **PS4 or PS5.** libSceVideoOut refuses the VR output bus and the PS5 pixel formats to a PS4
  process. Its `sceKernelIsPs4Process` and `get_authinfo` imports answer "PS5" (and capability
  bit 62) on the threads the PS VR2 libraries run on: the agent's VR thread and every thread they
  create, which are tagged through their `scePthreadCreate` import.
- **App info.** Hmd2 reads attribute3 for the PS VR2 bit, as does VideoOut, which caches it.
- **Foreground.** With the headset in VR mode the system reports the PS4 title as running in the
  background (status byte 5). The game would pause on that, so the byte is cleared.
- **SDK version.** VrTracker2 gives up (0x8a740009) without a PS5 SDK version, so it is given
  7.00.
- **The AV control service.** SceSysAvControl refuses the PS VR2 output for a PS4 title in two
  places: the layout handler and `checkVideoModeForChangeOutput`. The payload patches both in the
  service's memory while the game runs, and puts the original bytes back when the game closes or
  the payload exits. On every firmware but 13.60 the service's own record of the app is changed
  to a PS VR2 title's too ([FIRMWARE.md](FIRMWARE.md)).

### The GPU: running PS5 GPU work in a PS4 context

The game's GPU context is "gen1", meaning PS4 mode: it runs GCN shader code and reads GCN
resource descriptors, and some RDNA registers are privileged in it. Hmd2 builds its reprojection
as an RDNA compute dispatch and submits it through AgcDriver. Unchanged, the GPU hangs. Every
command buffer Hmd2 submits is rewritten before it reaches the GPU (`agent/gen1.c`):

- **The shader.** Hmd2's reprojection shader (3526 dwords of RDNA code on 12.40) is translated to
  GCN (3878 dwords) by `payload/rdna2gcn.c`. Most instructions encode the same way; the rest are
  rewritten (scalar memory loads, SDWA, literal operands, wait counters, and the extra `s_nop`
  GCN needs). The dispatch is pointed at the translated code with GCN register counts.
- **Descriptors.** Image descriptors Hmd2 writes in the RDNA layout are converted to GCN. The
  game's own textures keep the game's GCN descriptors.
- **Packets.** Profiler markers (writes to privileged registers) become NOPs, and RELEASE_MEM and
  ACQUIRE_MEM are rewritten in their gfx7 form.
- **Headset buffers.** GCN can't address the RDNA swizzle Hmd2 uses for the 4000x2040 headset
  buffers, so they are registered linear, on the VR port with the "enhanced" buffer attribute.
- **Flips.** Hmd2 flips the headset from the GPU with an end-of-pipe interrupt, which a gen1
  context never delivers. The flip is done from the CPU instead, once the GPU has written the
  frame's label, and Hmd2's wait for the GPU is replaced by polling that label.
- **Brightness.** The game's eye textures are sRGB; Hmd2 and the TV mirror are given them as
  UNORM, or the picture is much too dark.
- **The game's video decoder.** Once AgcDriver is in the process, the PS4 driver can't map a new
  compute queue any more. AvPlayer's decoder maps one when a video starts (the tutorial screens),
  so decoder queues are made before the PS5 libraries load and lent to it when it asks.

## 6. The eyes: composing wide and near views

The game hands each eye over twice: a wide view over the whole field of view with a black disc in
its middle, and a near view over the middle at twice the density, which PS VR's reprojection put
into the disc. The four views are slices of one array texture; their alpha holds nothing usable,
so they cannot go to Hmd2 as two layers.

So the agent composes them (`agent/compose.c`, the shader `tools/gpu/compose.c`, built with
`tools/gcn_cc.py`). At each hand-over, one compute dispatch an eye on Hmd2's GPU queue writes one
texture of the agent's own: the wide view's field of view at the near view's density, the near
view inside the game's inner blend radius, the wide one outside the outer, a linear blend
between. The game's GPU has finished the frame by then (its command stream waits for the
hand-over). Four sets of outputs are taken in turn, made at the game's first frame and registered
with the VR port in one go (the port takes two buffer sets: the flat screens' textures, then the
composed eyes). The composed eye is never more than 4096 rows (more showed the wide view's black
disc). Texels beyond the lens circle are written black without sampling, and each view is
sampled only where the blend uses it. Until Hmd2's queue is up, and for any frame the pass cannot
take, the wide views go over as they are (`agent.compose=0` forces that).

**The eye-tracked sharp area** (`agent/gaze.c`): the near view's projection is moved to where
PS VR2's eye tracking says you look, written into the game's per-view projections as it reads the
frame's pose, and the compose pass's blend centre moves with it. The wide views' cutout moves too:
the agent rebuilds the game's masks with the hole centred there, rewriting only the tiles that
change.

**Sharpening**: the compose pass can sharpen the composed eye (a contrast-adaptive sharpen,
`agent.sharpen`).

The settings menu (`agent/menu.c`) is drawn into each composed eye by a small pass of its own,
and, out of VR, into the TV buffer on screen after each flip.

## 7. Tracking and timing

**Head.** VrTracker2 gives the headset pose in its own room space. The agent converts it to PS
VR's tracking space (metres, +Y up, the player seated in front of where the camera would be,
facing it; `agent/vr.c`), rotating only around the vertical axis so that "forward" is where you
faced when the game started. Tracking only counts as live once the pose changes on 8 samples in
a row. The seat is set again when the tracker relocalises, 2 s after the headset is put on, when
OPTIONS is held for a second (or the menu's Reset view), and, with `agent.recentre=1`, on the
first press of circle. Each time, the game is sent the ResetVrPosition system event a console
sends when the player resets the view. The head's speed goes with every pose.

**Controller.** The game registers its controller with the PS VR tracker. Its orientation comes
from the pad's own motion sensors; without hand tracking its position is held 17 cm below and
50 cm in front of the head, following the head's position but not its turn. With hand tracking it
is placed from your hands (section 10). The shake that fires the Jet Black Fan is the pad's own
motion data, which reaches the game unchanged.

**Headset off.** PS VR2's attachment sensor is read every half second. While the headset is off,
the game's system status says the system UI is over it (`isSystemUiOverlaid`), which a PS VR game
pauses for. Whether DOAX3 pauses on it has not been checked.

**Eye distance.** The game asks `sceHmdGet2DEyeOffset` once, before Hmd2 is up, so the lens
separation Hmd2 reports is kept in `/data/doax3vr2/ipd` and given to the game from the next start
on (63 mm until then).

**Frames.** Each composed pair goes to Hmd2 as a layer with each eye's field of view and the pose
the game rendered for. Flat screens (the VR menu, loading) are shown as a 16:9 screen fixed in
front of the seat, with a black border around it (no streaks from its edge pixels). Out of VR
(the title, the vacation) the PS VR2 session stays open, and the picture the TV shows goes to the
headset the same way: each time the game flips its main port to a new buffer, that buffer is
sent (`hooks_flat_poll`, polled from the VR thread's idle wait; the game's display buffers are
registered with the VR port in the first set), so the headset is never black. Hmd2 reprojects each frame to the latest head pose: at 60 fps each frame
twice.

**Pacing.** The game waits for the reprojection's start event and for its port's flip count to
move on by two since its last frame. The agent raises the start event at each of the headset's
own refreshes (each flip Hmd2 makes) and reports those refreshes as the port's flip count, so the
game runs at exactly half the headset's 119.88 Hz.

At 120 fps (the default) the game is let through at every refresh, and every other loop of its
main thread is given no time step: the game's clock stays at 60 steps a second while each loop
draws a new picture from the newest head pose. What the game times by loop rather than by step
(water, the seat's settling, timers, the scene's fades) is held at its 60 fps speed on the loops
with no step (`agent.tickhold`, `agent.vrtimers`). Every 10 s the log says how many frames the
game handed over and how many Hmd2 showed, showed again or never showed.

## 8. The TV and sound

**The TV.** With PS VR2 in VR mode the TV shows the game's main video port, which nothing flips
any more. So the agent registers three display buffers of its own there and, for each new frame,
runs a small GCN compute shader (`agent/mirror.s`) on Hmd2's GPU queue that crops an 80-degree
view around the centre of the left eye picture into one and flips it (`agent/mirror.c`), at most
60 times a second and paced by the TV. Both eyes side by side or a black TV are options. Out of
VR the mirror stands down and the TV shows the game's own buffers.

**Sound.** PS VR2 games leave the TV quiet. The agent opens the game's BGM port as a main port,
which only the headset's earphones get, and an AUX port (the TV mix PS VR had) plays silence.
DOAX3 opens only the BGM port. Not yet checked by ear.

## 9. The game's data: package files, the item map, the agent's writes

**Package files.** The game stats and opens each `/app0/NN/package_data.bin` (its item master
among the tables) at its start, through `sceKernelStat` and `sceFiosFHOpen`. The agent hooks both
(and `sceKernelOpen`): when `/data/doax3vr2/port/NN/package_data.bin` exists, that path is given
to the game instead (`agent.port`). This is how Fortune's missing swimsuit rows reach Scarlet:
package 03 rebuilt with them appended.

**The item map.** Every package's rows go through the merged item map's insert (an unordered map
by item id, `*(base+0xc4c488)` in Scarlet), so new ids are added. The agent walks it
(`item_map_row`) to find a row by id; it logs its size at each rebuild.

**The agent's writes** (Scarlet unless said; each checks the game's code first, runs on the
2-second poll, and only touches what the game's own structures point to):

- `agent.portshop=N`: the Owner Shop's Swimsuits tab entries pointed at the ported suits' rows
  (each entry's row checked against the item map first), so the game's own purchase files them.
- The Softness Sensors' shop flags set to 0 in the item map (unless `agent.sensors=1`).
- `agent.malfunction` (on by default): the malfunction flag written for 15 costumes in the Item
  label map; Fortune's break models served from the port directory.
- `agent.ownerlv`, `agent.money` (both games): the Owner Level and Zack Dollars kept at least
  that. The owner's record is found through the instruction the game's mission-reward code loads
  it with, checked byte for byte in each game.
- `agent.fanswings=N`: the swings the Jet Black Fan's thread needs.
- `agent.portgive=1`: the 45 ported suits put in Kasumi's closet by calling the game's own
  closet functions ("obtained", then "in the closet") from the agent's thread, and the owner's
  copies emptied. `agent.wfix=1`: the equipped marks (W) put right once.

## 10. Hand tracking

PS VR2 hand tracking is `libSceVrHand`, a PS5-only library like Hmd2: the payload builds a
fake-signed copy on first run (on the firmware whose profiles have it), and the agent loads it
with the other four as PS VR2 starts (`agent/hand.c`). It is on by default (`agent.hands=0`
turns it off).

**The start gate.** Its start takes the headset cameras, which PS VR2's safety notice, play-area
setup and other system screens use too, and a start during those can take the console down. So
it starts once a session, only when the headset picture has run for two
seconds, the headset has stayed on your head for 5 seconds with no system screen up, and the
headset cameras' gate is clear. The gate reads the kernel's message buffer: the payload polls it
every 200 ms (`payload/msgbuf.h`; it consumes nothing), finds each poll's new text after its own
marker line (`DOAX3VR2 mark <n> <nonce>`, written into the kernel log once a second), and parses
the system's camera and screen lines into the cameras' state (`common/camera.h`), written into the
agent's shared block every 100 ms. The gate (`hand.c gate_eval`) is clear only when the system's
camera work is 12 s behind, no system VR screen has the focus, the headset was not put on in the
last 5 s, the tracker is not relocalising, and no kernel log lines may have been missed in the
last 15 s. It is asked once more right before the start. A gate that never clears never starts
the tracker; its reason is logged every 30 s, and after 20 s without the kernel log's word the
player is told to put the headset on again or start the game again.

**What it needs that a PS4 process lacks:**

- **Its models** (`handCenter_00`, `handPose_00`, `handPose_02`), from `/system/priv/vr_hand_ro`,
  which the sandbox can't read. The payload copies them to `/data/doax3vr2/models` at its start
  (`payload/prepare.c`, when missing or of another size). The agent reads them there itself, with
  the game's own `sceKernelOpen`/`Read`, into buffers of its own before VrHand starts, and answers
  VrHand's model requests from memory. Having the payload write them into the game's memory
  instead (`kernel_proc_copyin`) brought the console down every time.
- **The headset cameras**: Hmd2 opens `/dev/hmd2_image` and `/dev/hmd2_leddet` for it, which an
  app is refused. The payload lends the game its own credentials while VrHand starts, then gives
  the game its own back.
- **App info**: the hand tracking bit in attribute3, answered by the agent.
- **Its GPU work**: 27 RDNA compute shaders, translated like Hmd2's when VrHand creates them; the
  8 wave32 ones run as wave64 with only the low 32 lanes on.

**The controller's placement** (`agent/vr.c`). By default (`agent.padmode=0`, the settings menu's
Controller *in both hands*) it sits between your palms while both are seen, turned along the line
between them, as in Summer Lesson, Ghost-VR and Astro Bot. Lost, it stays where the hands had it
while it is still, and for 2 s once it moves, then glides to its place. *In one hand* (`agent.padmode=1`): the controller's own motion
says whether it is held. Held, it follows the hand that holds it (a grip offset learnt while both
hands hold it); not held, one hand is followed consistently. Each palm result is placed at the
camera's time and moved forward to the display's time with the controller's accelerometer, and
the game's 1.5 reach gain is undone so the controller is at your hands. The head's tracking is not
touched.
