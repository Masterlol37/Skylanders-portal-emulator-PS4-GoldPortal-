# Goldportal: a virtual Skylanders portal for a jailbroken PS4

Play your Skylanders figures digitally on a PS4: a GoldHEN plugin makes the game think a real Portal of Power is plugged in, and a PC app puts `.dump` figure files on it.

**Tested and working (since plugin 1.14):** Swap Force (CUSA00024), Trap Team (CUSA00697, including the trap slot and the portal speaker), SuperChargers (CUSA02180) and Imaginators (CUSA05158), on a PS4 with firmware 12.50 and GoldHEN.

| Part | Folder | What it does |
|---|---|---|
| **PS4 plugin** | `ps4_plugin/` | `goldportal.prx` runs inside the game. It hooks `libSceUsbd` (Sony's libusb) to emulate the portal, and runs a small web server on **port 8085**. |
| **PC app** | `pc_app/` | Python GUI and command line: put figures on or take them off, and save the game's changes back into your `.dump` files. |

```
PC app --HTTP :8085--> goldportal.prx (inside the game) --virtual USB--> Skylanders game
                                                       --speaker audio--> DualShock 4 speaker
```

You can also skip the PC app: open `http://<PS4-IP>:8085/` in any browser (phone, tablet or PC).

> **No figure data is included.** Use dumps of figures you own (1024-byte MIFARE Classic 1K dumps, the same format RPCS3 and Dolphin use).

## Download

Get `goldportal.prx` and the PC app from the [Releases](../../releases) page, or build the plugin yourself (see below).

## Install

1. Copy `ps4_plugin/goldportal.prx` to `/data/GoldHEN/plugins/` (FTP works; GoldHEN's FTP server runs on port 2121).
2. Add each game to `/data/GoldHEN/plugins.ini`:
   ```ini
   [CUSA00024]
   /data/GoldHEN/plugins/goldportal.prx
   [CUSA00697]
   /data/GoldHEN/plugins/goldportal.prx
   [CUSA02180]
   /data/GoldHEN/plugins/goldportal.prx
   [CUSA05158]
   /data/GoldHEN/plugins/goldportal.prx
   ```
   Others are not tested report them as issues if they work or not.
   
4. Turn on plugins in GoldHEN and start the game. You'll see a "Goldportal 1.15 loading" notification. Unplug any real portal.

## Use the PC app

You need Python 3.8 or newer. Only the standard library is used.

```bash
python pc_app/goldportal_pc.py
```

- Enter the PS4's IP and click **Connect**. The web server is up about 10â€“15 seconds after the game starts.
- Click **Choose main folderâ€¦** and pick the folder with your dumps (subfolders per game work well). The sidebar shows it as a tree with every subfolder, sorted naturally. **Search** filters figures by name, and **Rescan** picks up new files.
- Double-click a figure to place it on the first free slot, or select it and click **Place** on a slot.
- **Autosave** (on by default) writes every figure the game changed (XP, gold, upgrades) back into its `.dump` file every 30 seconds. **Remove**, **Remove all**, replacing a figure and closing the app save as well.
  - The first save of a file in a session keeps the original as `.dump.bak`. Later saves leave that `.bak` alone, so it is always the pre-session version.
  - A figure is only saved when two reads a second apart match, so it is never saved halfway through the game writing to it.
  - Figures placed some other way (web page, command line) are matched to their file by name, if exactly one file in the folder tree has that name.
- **Save asâ€¦** writes a slot's current data to a file of your choice.
- **Dark mode** toggles the theme. It, the folder, the IP and the autosave setting are remembered in `~/.goldportal.json`.
- Use figures the game supports. For example, Swap Force only accepts Spyro's Adventure, Giants and Swap Force figures.

Command line: `--ip <PS4-IP>` plus `status`, `load <slot> <file>`, `remove <slot>` or `save <slot> <file>`.

The plugin also writes changed figures to `/data/goldportal/backup_<name>` on the PS4 when they are removed or replaced.

## How the emulation works (what we learned)

- **Start-up:** `plugin_load` returns immediately and does all its work on its own thread. Doing work inside `plugin_load` crashed the games. The plugin never loads `libSceUsbd` itself: it waits until the game loads it (between 2 s and 11 s after start), then hooks it.
- **Hooking:** many `sceUsbd*` exports are 5-byte jump stubs (`E9 rel32` + NOPs). These are patched in place, and the original is the jump target. Other functions get a relocating inline hook ([hook.c](ps4_plugin/source/hook.c)).
- **Timing:** the virtual portal only appears in the device list after *all* hooks are installed. Newer games poll the list in a tight loop and crashed on a half-hooked library.
- **Pacing:** interrupt IN reports are paced like the real portal (one every 60 ms). The games only read the latest report each frame and missed the replies when reports came faster.
- **Portal model:** the reply to `R` identifies the portal model. `02 03` (Swap Force portal) works for Swap Force, SuperChargers and Imaginators. Trap Team requires its own portal: `02 1B`.
- **Speaker:** Trap Team sends the portal speaker's sound as 64-byte interrupt OUT packets of 16-bit mono PCM at 8 kHz. The plugin upsamples it to 48 kHz and plays it on the DualShock 4 speaker (pad-speaker audio port). Packets are only accepted when the buffer has room, which paces the game in real time.
- **Other commands:** `CheckConnected`, `GetDescriptor`, string descriptors and transfer cancel/free are emulated for the virtual portal.

## Files and switches on the PS4 (`/data/goldportal/`)

| File | Purpose |
|---|---|
| `log_<TITLE>.txt`, `log_<TITLE>_prev.txt` | Log of the current and previous run of each game (also at `http://<PS4-IP>:8085/api/log` and `/api/log?prev`). |
| `portal_id_<TITLE>.txt` | Override the portal model, e.g. `02 1B`. |
| (dev)`sniff_<TITLE>` (empty file) | **Sniff mode** for that game: no virtual portal; a real portal's traffic is logged and its speaker audio is saved to `audio_<TITLE>.raw`. |
| (dev)`nohook` (empty file) | Safe mode: web server only, no hooks. |
| (dev)`trace` (empty file) | Extra-verbose logging. |
| `skip_<TITLE>.txt` | Hooks the crash guard disabled after a crash while hooking (one name per line; delete to retry). |

`/api/status` reports the plugin version, the game's title ID, the number of hooks, and whether the game opened and activated the portal.

## Build the plugin

Build on Linux or WSL (Debian/Ubuntu):

1. Install `clang`, `lld` and `llvm`.
2. Get the [OpenOrbis PS4 toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) (release tarball) into `~/ps4/OpenOrbis/PS4Toolchain`.
3. Clone and build the [GoldHEN Plugins SDK](https://github.com/GoldHEN/GoldHEN_Plugins_SDK) in `~/ps4/GoldHEN_Plugins_SDK` (`make`).
4. Run:
   ```bash
   bash ps4_plugin/build.sh
   ```
   `build.sh` sets `OO_PS4_TOOLCHAIN` and `GOLDHEN_SDK` itself if they point to `~/ps4/...`.

## Test without a PS4

```bash
python pc_app/mock_ps4.py --simulate-game
```

Then connect the app to `127.0.0.1`. `python pc_app/test_portal.py` runs the automated tests.

## Adding another game

1. Add the game's title ID to `plugins.ini` and start it. Check `log_<TITLE>.txt` for `virtual portal is now visible to the game`.
2. If the game rejects the portal, record a real portal with sniff mode (`/data/goldportal/sniff_<TITLE>`). Look at the reply to `R` and set it with `portal_id_<TITLE>.txt`, or add it to the defaults table in `main.c`.
3. Pull requests with new title IDs and portal IDs are welcome.

## Files

```
ps4_plugin/
  build.sh, Makefile
  source/main.c         plugin entry, libSceUsbd hooks, virtual device, sniff mode
  source/hook.c/.h      inline hooks (thunk patching + relocating detour)
  source/portal.c/.h    portal protocol emulation (A, C, J, L, M, Q, R, S, V, W)
  source/audio.c/.h     portal speaker -> DualShock 4 speaker
  source/http.c/.h      web server + web page
  source/log.c/.h       per-game logs
  source/usbd_types.h   libusb-compatible structs
pc_app/
  goldportal_pc.py       GUI + CLI client
  mock_ps4.py           fake PS4 for testing (Python port of portal.c)
  test_portal.py        tests
```

## Credits

- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) and the [GoldHEN Plugins SDK](https://github.com/GoldHEN/GoldHEN_Plugins_SDK) (its HDE64 decoder is used for the inline hooks).
- The portal protocol was worked out by recording real portals with this plugin's sniff mode. RPCS3's and Dolphin's Skylanders portal emulation were a useful reference for the command set.

## Disclaimer

This is an unofficial fan project for preserving and playing figures you own. It is not affiliated with or endorsed by Activision, Toys for Bob, Vicarious Visions or Sony Interactive Entertainment. "Skylanders" and "Portal of Power" are trademarks of their respective owners. It contains no game code, figure data or copyrighted assets. Use at your own risk.

## License

[MIT](LICENSE)
