<p align="center"><img src=".github/logo.png" alt="reNsanity" width="600"></p>

reNsanity è un progetto spontaneo, guidato dalla passione e voglia di scoperta. Ci tengo a lasciare che il prossimo giochi e si lasci affascinare. reNsanity è il porting su Nintendo Switch di Crash Twinsanity PS2. Spero vi divertiate, mi sono preso del tempo per fare texture personalizzate, si può scegliere se accenderle o meno, grazie.

*reNsanity is a spontaneous project, driven by passion and the wish to discover. I want the next person to play it and be fascinated. reNsanity is the Nintendo Switch port of Crash Twinsanity for the PS2. I hope you have fun; I took the time to make custom textures, and you can choose whether to turn them on. Thank you.*

— dampestwriter

## What you need

- **The game**: your own copy of the PAL disc of Crash Twinsanity (SLES-52568), as a disc image (`.iso`). This repository contains no game data and no assets.
- **Linux x86_64** (on Windows use WSL) with `curl`, `tar`, `sha256sum`, `cmake` 3.21 or newer, `ninja` and `python3`. On Debian or Ubuntu:

  ```
  sudo apt install git curl tar coreutils cmake ninja-build python3
  ```

## Build

```
git clone https://github.com/dampestwriter/reNsanity.git
cd reNsanity
bash build.sh
```

`build.sh` downloads the toolchain into `tools/toolchain/`, every file checked against a pinned SHA-256: devkitPro (devkitA64, libnx, deko3d and the Switch libraries), wasi-sdk 24.0 and wabt 1.0.42. Nothing is installed on your system. The result is `out/switch/reNsanity/reNsanity.nro`.

## Install

Copy `out/switch` to the root of the SD card and put your disc image next to the game:

```
/switch/reNsanity/reNsanity.nro
/switch/reNsanity/game.iso
```

Start **reNsanity** from the Homebrew Menu. Settings are kept in `/switch/reNsanity/ps2r.ini` and saves in `/switch/reNsanity/memcards/`.

## Custom textures

Optional, from [reNsanity-texpack](https://github.com/dampestwriter/reNsanity-texpack). With the texture pack at `/switch/reNsanity/texpack/texpack.bin`, the Graphics settings show **custom textures** (on/off). Without the file the setting is not shown and the game uses its original textures.

## Credits

- **dampestwriter** — project lead and port director; testing, review, art direction, quality and performance.
- **twinsanity-reversed** by Smartkin — the Crash Twinsanity decompilation (`game/`, github.com/Smartkin/twinsanity-reversed), included with the author's permission.
- **TwinsanityEvolved** — the native port this work started from.
- **NativeEE** — the PS2 engine (`engine/`, MIT, see `engine/LICENSE`).
- **devkitPro**, **libnx**, **deko3d**, **SDL2**, **FFmpeg**, **wasi-sdk** and **wabt** — downloaded by `build.sh`.

Crash Twinsanity, Crash Bandicoot and their characters are trademarks of their owners. This project is not affiliated with or endorsed by them.

## License

MIT, see `LICENSE`. `engine/` carries its own MIT notice. `game/` is twinsanity-reversed, included with its author's permission; its terms are its author's (see `game/NOTICE`). A built `reNsanity.nro` links FFmpeg built with GPL components, so a distributed binary falls under the GNU GPL.
