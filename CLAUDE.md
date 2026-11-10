# CLAUDE.md — XY Project

Guidance for Claude (and any contributor) working in this repository, or starting a **new project based on it**.
Created by Kimiroto. Read this first; the full development history is in `MANUAL.md` (PT/EN/ES/JA).

## What this project is

A native PC port of Pokémon X / Y (Nintendo 3DS) by **static recompilation**: the game's ARM11 code is translated
once into C and compiled for the PC. Around it there is an **HLE runtime**: a reimplementation of the 3DS OS
(kernel + services), a PICA200 GPU front-end (software, OpenGL 3.3, Vulkan 1.3), audio (DSP HLE from Azahar/Citra
plus faad2), input, an in-game menu and a Windows launcher.

## Hard rules (non-negotiable)

1. **Only the user's own dump.** The project never includes, downloads or generates game data for anyone else.
   Everything game-specific is produced on the user's machine from their decrypted `.3ds`.
2. **Never commit or distribute game files or anything generated from them**: ROMs, `code.bin`, RomFS, the
   generated C (`out/`, `jogos/`), DSP firmware, game sounds, textures, icons, logos, screenshots of the game.
   `.gitignore` covers the usual paths; check `git status` before every commit anyway.
3. **No third-party assets bundled.** Texture packs, menu background art (`fundos/`), the Roller Skates icon
   (`icones/`) and the 3D cartridge (`cartuchos/`) are optional files the *user* places in their own folder.
   They are read at run time and never shipped. The project icon (`lancador/icone.ico`) is an original design;
   do not replace it with official artwork in the repository.
4. Public images (README, releases) show only the project's own UI, never game content.
5. Keep third-party licenses intact (see "Licenses"). The project is GPL-2.0-or-later because it includes GPL code.

## Repository layout

| Path | What |
|---|---|
| `tools/ctr.py` | Reads the decrypted `.3ds` (NCSD/NCCH, ExeFS, RomFS) |
| `tools/cro.py` | Parses CRO0/CRS dynamic modules (segments, imports/exports, relocations) |
| `tools/link.py` | Links `code.bin` at `0x00100000` and every CRO from `0x40000000` into one image |
| `tools/funcs.py` | Function discovery (BL/BLX, vtables, literal pools, relative tables, CRO exports, ARMv4 calls, ADR, prologues) |
| `tools/recomp.py` | ARMv6K + VFPv2 + ARMv6 SIMD + Thumb-1 → C translator. `HOOKS` = game patches (see below) |
| `tools/recomp_game.py` | Iterates discovery + recompilation until no new function appears |
| `tools/jogar.py` | **Main driver**: reads the ROM, recompiles, compiles in parallel, launches the game, save backups, crash reports |
| `tools/extra_funcs_conhecidas.txt` | Extra function entry points found at run time (addresses only, no game code) |
| `tools/mods_merge.py`, `romfs_build.py`, `pack.py` | Mods (LayeredFS-style RomFS overlay) and image packing |
| `tools/gerar_fonte.py`, `gerar_fonte_menu.py` | Regenerate the bitmap fonts (`runtime/fonte_aviso.h`, `runtime/fonte_menu.h`) |
| `tools/armi.py`, `lockstep*.py`, `dis.py`, `profile.py` | Reference ARM interpreter, lockstep comparison, disassembler, profiler |
| `runtime/runtime.c` | Loader, indirect-call dispatch, options, `main()` |
| `runtime/kernel.c` | HLE kernel: threads, single-core scheduler with preemption, events/mutexes/semaphores/timers, virtual clock (fast-forward), watchdog |
| `runtime/services.c` | `srv:`, APT, HID, CFG, FS, PTM, CAM, MIC, NDM, FRD, BOSS… (unknown commands answer success and are logged once) |
| `runtime/gsp.c` | GSP (GPU commands, screen transfers), present, screenshot hook |
| `runtime/ldr.c` | `ldr:ro` — loads CROs onto the pre-recompiled code |
| `runtime/pica*.c` | PICA200: command lists, vertex/geometry shaders, rasterizer, lighting, textures |
| `runtime/pica_gl.c`, `runtime/pica_vk.c`, `runtime/vk/` | OpenGL 3.3 and Vulkan 1.3 back-ends (`vk/gerar.sh` regenerates `vk_shaders.h`) |
| `runtime/texpack.c` | Custom texture packs (Citra/Azahar old hash, CityHash64) |
| `runtime/display_sdl.c` | SDL2 window, input, controller mapping/remapping, audio output, HUD, toasts, fast-forward, Roller Skates |
| `runtime/menu_jogo.h`, `runtime/menu_ui.h` | In-game menu (software-drawn UI) and its drawing library (shared with the launcher) |
| `runtime/input.c` | HID state, touch, Nuzlocke mode, `g_ui_lang` |
| `runtime/idioma_ui.h` | `TR(pt, en, es, ja)` translation macro for the program UI |
| `runtime/captura.c`, `runtime/crash.c` | F12 screenshots (stb_image_write), crash handler |
| `runtime/audio/`, `runtime/faad2/`, `runtime/dsp_*.c*` | Audio (third-party, GPL) |
| `runtime/display_null.c`, `display_win32.c` | Headless and plain Win32 fallbacks |
| `lancador/` | Windows launcher (plain C + Win32 API); `compilar.sh` / `compilar.bat` build `XY Project.exe` |
| `jogar.sh`, `jogar.bat`, `instalar.ps1` | Entry points for Linux / Windows (text mode) / toolchain download |
| `test/` | Recompiler differential tests (`run_tests.py`), kernel tests, `make_test_3ds.py` |

## Build and run

* **Windows**: `XY Project.exe` → choose ROM → PLAY. First run downloads portable Python, llvm-mingw and SDL2 into
  `ferramentas/` (inside the project folder only), then `tools/jogar.py` does everything.
* **Linux**: `./jogar.sh path/to/game.3ds` (needs `python3`, `clang`, `libsdl2-dev`; without SDL2 it runs headless).
* Output goes to `jogos/<title id>/` (generated C, objects, `jogo.exe`, `save/`, `backups/`, `erros/`, `jogo.log`).
* `jogar.py` recompiles **only the runtime** when runtime sources change, and the game code when the recompiler
  changes. Fast-path runtime files (`RT_FAST`) are built with `-O2`.
* Launcher: `MINGW=/path/to/llvm-mingw/bin sh lancador/compilar.sh`. `compilar.bat` must keep **CRLF** line endings.

## Testing without a screen

* Run the game headless with `SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy`.
* Vulkan without a GPU: point `VK_ICD_FILENAMES` to a SwiftShader ICD (slow; good for correctness, misleading for
  performance).
* Debug variables (all optional): `MENU_TESTE=page,sel` (open the menu on a page), `MENU_PPM=file` (dump the menu
  canvas), `JANELA_PRINT=file` (dump the window), `CAPTURA_QUADRO=n` (screenshot at frame n), `ACELERAR_TESTE=s`,
  `PATINS_TESTE=s`, `SHOT=…`, `TECLAS=…` (scripted input), `PICA_PERFIL=1`, `LOG_IPC=1`, `WATCHW=addr`.
  See `MANUAL.md` for the full list.
* Recompiler: `python3 test/run_tests.py`. Windows compile check: cross-compile with llvm-mingw.
* Run long tests in the background; the game can take minutes to reach a given scene.

## Conventions

* Code comments, identifiers and logs are in **Portuguese** (the original author's language). Keep that style
  when editing; the README is English, `MANUAL.md` has all four languages.
* **Every user-facing string** goes through `TR("pt", "en", "es", "ja")` (C) or `L(pt, en, es, ja)`
  (`jogar.py`) / the launcher's `TR`. After adding text, **regenerate the fonts** so new characters exist:
  `python3 -I tools/gerar_fonte_menu.py` and `python3 -I tools/gerar_fonte.py` (`-I` is required because
  `tools/dis.py` shadows the standard library module).
* Settings live in `opcoes.txt` (key=value); `jogar.py` turns them into environment variables for the game.
* Log lines are prefixed by subsystem: `[vk]`, `[gl]`, `[gsp]`, `[ldr]`, `[ipc]`, `[kernel]`, `[crash]`…
* Exit codes: 3 unsupported instruction, 5 missing function, 6 `svcBreak`, 7 unimplemented svc, 8 deadlock,
  9 fatal error, 10 crash; 77/78/79 are menu actions (restart, change ROM, reset).
* When the user changes a file's design or text, keep the four languages and `MANUAL.md` in sync.

## Game-specific parts (what to change for another game)

Most of the project is generic 3DS infrastructure. The parts tied to Pokémon X/Y are:

* **Title IDs** `0004000000055D00` (X) and `0004000000055E00` (Y): `tools/jogar.py` (`JOGO_VERSAO`),
  `lancador/lancador.c`, texture folder names.
* **Game patches** in `tools/recomp.py` → `HOOKS`: matched by **instruction signature**, not address, so one
  patch works for both versions (Poké Radar, battle XP multiplier). Nuzlocke logic is in `runtime/input.c`.
* **Menu branding**: version name/colors (`mu_tema`, `g_versao`) in `runtime/menu_jogo.h`; optional cartridge
  model and background art read from user folders.
* **Known extra functions**: `tools/extra_funcs_conhecidas.txt` is X/Y specific; start empty for a new game.
* Services: X/Y exercise a particular subset (camera/mic in Pokémon-Amie, `nwm::UDS` stubs for PSS). Another game
  will hit other commands; the log prints `[ipc] <service> cmd XXXX ainda sem implementação` the first time a
  command has no implementation. That is the first place to look when a game aborts (`svcBreak`).

To start a new game from this base: keep `tools/` and `runtime/`, clear the X/Y specifics above, run the pipeline
on the new dump, and iterate on unsupported instructions, missing functions and service commands reported in
`jogo.log` / the crash report.

## Known limitations

* Online/local wireless features are not implemented (stubs only).
* Camera and microphone return black frames / silence.
* The game's native 30 fps is kept; the 60 fps interpolation (`FPS=60 FPS60_TESTE=1`) is experimental.
* Without skates, D-pad emulation of the Circle Pad may not give diagonal walking (depends on the game).
* Vulkan "deferred clear" (`VK_LIMPEZA_ADIADA=1`) is experimental and off by default.

## Releases

1. Commit and push the source (check that no game data or personal folders are staged).
2. Build `XY Project.exe` with the original icon, then package `XY-Project-<ver>-Windows.zip` (repo files +
   exe) and `XY-Project-<ver>-Linux.tar.gz` (repo files, scripts executable).
3. The maintainer creates the GitHub release in the web UI (tag `v<ver>` on `main`) and attaches both packages;
   release notes in English.

## Licenses

Project code: GPL-2.0-or-later. Included: Azahar/Citra audio core (GPL-2.0+), faad2 (GPL-2.0+), stb_image /
stb_image_write (public domain/MIT), CityHash (MIT), Vulkan headers (Apache-2.0/MIT). Fonts rendered to bitmaps:
Inter and Noto Sans CJK JP (OFL 1.1), DejaVu Sans (DejaVu license). Downloaded at first run, not redistributed:
SDL2 and SDL_GameControllerDB (zlib), llvm-mingw (Apache-2.0 w/ LLVM exceptions), Python embeddable (PSF).
References: 3DBrew, Azahar/Citra, ARM Architecture Reference Manual. Inspiration: N64Recomp / Zelda64Recomp.
