# Testing without a player

The games can be driven on the console from a PC, with nobody at the console: started, steered
through their menus by button presses the agent makes up, photographed, measured and closed. All
of it needs a **debug build** (`make`, [BUILDING.md](BUILDING.md)); a release build has none of
it.

The PC needs the console's address: `PS5_HOST` in the environment, or a line `PS5_HOST=<address>`
in `tools/local.env` (not committed). The scripts use the console's FTP server (port 2121), the
ELF loader (9021), the kernel log (3232) and the payload's control port (9089).

## The control port

The payload listens on port 9089 on every interface. A connection saying (all but `quit` and the
probe in debug builds only):

- `quit` ends the resident payload (only while no game is served);
- `launch` starts Fortune, `launch CUSA14173` Scarlet, `launch CUSA14831` Scarlet 1.00; refused
  while a game is served;
- `kill` closes the running game;
- `suspend` / `resume` suspends and resumes the game, as switching apps does;
- `ps` lists every process;
- anything else is a probe, answered `yes`.

## `tools/drive.py`

One step at a time, from the PC:

    tools/drive.py start [TITLE]     the payload sent unless one of ours is resident, cvars.txt
                                     and test.txt uploaded, the game launched (CUSA04555 Fortune,
                                     the default, CUSA14173 Scarlet or CUSA14831 Scarlet 1.00),
                                     the kernel log streamed
    tools/drive.py tap NAME [MS]     a button held for MS ms: cross circle square triangle up down
                                     left right options l1 r1 l2 r2 l3 r3 touch, the left stick
                                     (lsleft lsright lsup lsdown), the right stick (rsleft rsright), shake
    tools/drive.py press HEX [MS]    the same by bit mask
    tools/drive.py shot [NAME]       a picture -> logs/drive/NAME.png
    tools/drive.py log [N]           the agent's log fetched, its last N lines shown
    tools/drive.py stop              the game closed, the log fetched; the payload stays resident
    tools/drive.py quit              the payload asked to quit

How it works: the agent reads `cvars.txt` every 2 s, so drive.py writes `agent.press` and
`agent.shot` lines there; a press or a shot takes effect within about 2.5 s. The presses are
mixed into what the game reads from its controller. A shot is the flat display buffer the game
flipped last, or in VR mode the frame's eye texture (the right eye with `agent.shoteye=1`), copied
as the GPU holds it and untiled on the PC by `tools/detile.c`. `shake` flips the controller's
up-down acceleration while held; it has counted one swing of the Jet Black Fan but not two (a
real shake by hand fired it).

Environment:

- `DRIVE_CVARS`: the lines every upload carries. The default, `agent.novr2=1`, `agent.hands=0`,
  is an observing run: the game's PS VR calls are answered and logged, but PS VR2 is never
  started and nothing is composed. **A run that measures the picture must set `DRIVE_CVARS`
  without `agent.novr2=1`.**
- `DRIVE_TEST`: the words for `test.txt` (default `worn`: the headset on the desk counts as on a
  head). Empty for none.
- `DRIVE_AGENT=<agent.elf>`: that agent goes in instead of the resident payload's built-in one,
  to try an agent change without sending a payload again.
- `DRIVE_DIR` (default `logs/drive`): the state, pictures and logs.

**Send a payload as rarely as possible.** Every payload send has been a moment the console could
go down: drive.py keeps a resident payload of ours, and after any payload quits wait about two
minutes before sending another. If another VR payload answers on port 9087 or 9088, drive.py does
not start.

## The routes

- `tools/route-vr.sh [TITLE]`: the game started and taken into VR PARADISE. Both games:
  "press any button" (circle), down twice, circle; the start-up boxes are skipped by the payload.
  With `agent.bootnotes=1` in `DRIVE_CVARS` a circle for each box first (Fortune: the autosave
  notice and the shop-data message; Scarlet: the autosave notice). A circle too many chooses a
  menu item. A shot at the menu (`logs/drive/route-menu.png`) shows VR PARADISE highlighted.
- `tools/route-photo.sh [TITLE]`: `route-vr.sh`, then on into a Photo Paradise pose scene
  (Kasumi, the first pose card, the stage by the sea, the first suit): the heaviest scene tried,
  used for the eye size.
- `tools/sweep-res.sh TITLE WIDTH...`: for each eye width (or `stock`), `resolution.txt`
  uploaded, the route run (`ROUTE=route-photo.sh` for the pose scene), 45 s measured, a picture
  taken, the game stopped. Each width's log and picture go to `logs/sweep/`.

Scarlet without a vacation save plays its opening movie after "Travel to New Zack Island"
(options skips it), then the character select. The routes assume a save.

## `tools/testrun.sh`: recorded sessions

A timed run with a recorded or replayed session:

    tools/testrun.sh <payload.elf> <seconds> <out.log> ["record|replay worn"] [pad.txt] [head.txt]

It checks the game is closed, asks the resident payload to leave, sends this one, uploads the
settings and `test.txt`, launches the game (`PS5_TITLE`, default Fortune), waits, closes it,
pulls the log and prints its health lines. `PS5_REUSE=1` keeps the resident payload when it is
the same build. `PS5_CVARS=<file>` adds that file's lines to `cvars.txt` for the run, and
`PS5_CVARS_AT="<s>:<file> ..."` switches them at the given seconds; the console's own `cvars.txt`
is saved first and put back however the run ends (`tools/consfile.sh`).

The test words in `/data/doax3vr2/test.txt` (read once at the game's start):

- `record`: the controller and the head's pose go to the log as `pad:` and `head:` lines;
- `replay`: the controller is made up from `pad.txt` and the head taken from `head.txt` (made
  from a recording's log by `tools/padscript.sh agent.log pad` / `head`);
- `worn`: the headset counts as on a head.

A worn session recorded this way and replayed (the head's movement and the controller) is how
most features were checked without a player. Recordings are not included in the repository.

Never edit a runner script while it runs: the shell reads it as it goes.

Other scripts: `tools/step.sh` (a tap, a wait, a shot), `tools/shot2png.sh` (a raw shot to
pictures), `tools/sharpen-ab.sh` (the sharpening levels compared in one scene).

## Reading the log

Every 10 s: `hmd: N frames handed over, N shown, N shown again, N never shown; the game's GPU up
to X ms a frame`, and the compose pass's count and time. The aim is 600 handed over at 60 fps
(1,200 at 120), none never shown. Other lines to look for: `resolution:`, `cut features:`, `vr passport:` (the
payload's patches), `port: item map N rows`, `sensors:`, `malfunction:`, `menu:`. L3+R3 (debug
builds) marks a moment with the 10 s before and 5 s after ([SETTINGS.md](SETTINGS.md)).
