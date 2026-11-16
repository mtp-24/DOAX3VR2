# Dumping a new game version or firmware

DOAX3VR2 patches the game at exact places in its code, and loads PS VR2 libraries from the
console's own firmware. A game version, region (for example the Asian Scarlet, CUSA14831) or
firmware it doesn't know needs those places found again, from that version's files.
The DOAX3VR2 payload saves them by itself when it meets one.

## For a tester

1. If the game has an update, install it first: the update is the version that gets supported.
2. Send the DOAX3VR2 payload as usual and start the game.
3. When the payload doesn't know the game's version, a notification says so and that its files
   are being saved. Keep the game open about a minute, until the next one: "the files for adding
   ... are saved".
4. Over FTP, fetch the one file it names, `/data/doax3vr2-dump/<title>-<version>-fw<firmware>.tar`
   (for example `CUSA14831-01.08-fw13.60.tar`, about 20 to 30 MB), and send it to the developer.

It is saved once: starting the game again only names the file. On a firmware without a profile,
the payload saves the firmware part the same way (`fw<firmware>.tar`) and quits.

## What it saves

| In the .tar | What it is for |
|---|---|
| `info.txt` | firmware, title id, APP_VER, VERSION, content id, category, title, counts |
| `dump.log` | every file saved, and anything that failed |
| `firmware/` | libSceAgcDriver, libSceAgc, libSceHmd2, libSceVrTracker2, libSceVideoOut, libSceVrHand (`.sprx`) and SceSysAvControl.elf, decrypted: a firmware profile (`common/firmware.h`) |
| `game/appmeta/` | the installed game's `param.sfo` / `param.json`: version and content id |
| `game/app0/`, `game/patch0/` | the base game's and the update's `eboot.bin` and every `.prx` / `.sprx` / `.elf`, decrypted, with `sce_sys/param.*` and `changeinfo` |
| `game/files-app0.txt`, `files-patch0.txt` | every file of the game with its size (its data layout) |
| `game/addcont.txt` | the installed add-ons' folder names (DLC labels) |

It only reads: the game's mounted files, `/system/common/lib`, `/SceSysAvControl.elf` and the
game's `appmeta`. It writes only under `/data/doax3vr2-dump`. It never reads save data, the
account, `sce_sys/keystone`, `nptitle.dat` or console identifiers. The decrypted files are the
tester's own game's and console's: they are for making the profile, not to be shared further.

## For the developer

- Game: compare `game/patch0/eboot.bin` (or `app0` with no update) with the supported build's
  (`dumps/<title>/`): the `k_builds` table in `payload/main.c`, and every Scarlet or Fortune
  address in `agent/`, are found again in it. A region build of the same version may be the
  same code moved (the Asian Scarlet 1.00 is the Japanese 1.00 with 0x80 bytes more code).
- Firmware: the profile in `common/firmware.h` from `firmware/` ([FIRMWARE.md](FIRMWARE.md)).
- Add-on PKGs for another region are rebuilt with its content id prefix (`info.txt`).

The code: `common/dump.c`, called from the payload's unknown-version notice
(`check_other_title`, the dump on a thread of its own) and its unknown-firmware check at its start.
