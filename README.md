# DRA2 core: game-specific GameCube runtime (Linux build, milestone 2)

A clean-room runtime for playing **your own legally owned disc** of Digimon Rumble Arena 2 (Europe, GD6P70).
It contains no Nintendo, Bandai or Atari code or data. The game's executable and assets are read from your disc image at run time.
The Android port is built on the same core and has not been started yet.

## Build (Debian/Ubuntu)
    sudo apt install g++ cmake libsdl2-dev libgles2-mesa-dev python3-pip
    cmake -S . -B build && cmake --build build -j
    pip install zstandard
    python3 tools/rvz2iso.py game.rvz game.iso      # only needed if your dump is an RVZ
    ./build/dra2core game.iso

## Controls
Keyboard: arrows = D-pad and stick, Z = A, X = B, C = X, V = Y, Enter = Start, A/S/D = Z/R/L. Any SDL game controller also works.
Esc or closing the window quits.

## Status
Plays through the intro, menus, character select and full matches (4 players, stage, HUD, effects, music and sound).
Rendering is GX to GLES 2.0 with generated TEV shaders. Audio is the AX DSP implemented in C++ and played through SDL.
Speed: pure interpreter, roughly 1.7x real time on a recent x86 desktop CPU. An ARMv7 JIT is the next step for phones.
Known gaps: memory card saves, some rarely used TEV and indirect-texture modes, and the DSP code is HLE (AX only).

## Layout
cpu.cpp Gekko interpreter · machine.cpp bus, disc and DOL boot, per-game patches · hw.cpp PI/VI/DI/EXI/SI/AI/DSP/PE registers
gx.cpp GX command parser · gl.cpp GLES2 renderer · ax.cpp AX audio HLE · main.cpp loop, SDL audio and pacing

## Debugging (environment variables)
STATUS=1, DVDLOG=1, DRAWLOG=<frame>, SHOT="<frame>:<file.ppm>", WAVOUT=<file.wav>, HIDDEN=1 (no window), NOGFX=1, NORT=1 (run unpaced), PADSCRIPT (scripted input used by the tests).

## JIT (ARMv7)
Native translation covers the integer, branch, load/store and floating-point load/store (lfs/lfd/stfs/stfd) instructions; blocks are chained
(constant-target exits jump straight into the next translated block while under an instruction budget). Everything else runs through the
interpreter helper, so behaviour stays identical.
On ARMv7 hosts the core runs an A32 dynamic recompiler (`core/jit*.cpp`) by default; set `INTERP=1` to force the interpreter
or `NOJIT=1` to disable. On x86 the interpreter is used. `test/jittest` (differential per-instruction tests) and
`test/jitrun` (full-game smoke test, translated code run under Unicorn) validate it; both need the Unicorn library.

## Android (armeabi-v7a, minSdk 24, GLES 2.0)
Sources are in `android/` (Gradle + CMake, SDL2 as the platform layer). Nothing proprietary is included: on first launch the
app asks for your own disc image (.iso/.gcm; an RVZ must be converted to ISO first) with the system file picker and reads it
through `/proc/self/fd/N`.

    cd android && ./fetch_sdl.sh      # clones SDL2 (zlib licence) and links its Java glue
    gradle assembleDebug              # needs Android SDK + NDK r26, JDK 17

No local NDK? Push the project to GitHub and run the `android-apk` workflow (`.github/workflows/android.yml`); the debug APK
is uploaded as a build artifact.

Touch controls are drawn on screen (left: stick, right: A/B/X/Y, top: L/R/Z, bottom: Start); Bluetooth/USB gamepads work too.
The JIT is used automatically on ARMv7; if executable memory cannot be mapped the interpreter is used instead.
Status: the native core cross-compiles for ARMv7 and the JIT is verified under emulation; the APK itself has not been
built or run on a device yet (no Android SDK/NDK was available in the development sandbox).

## Frameskip
`-fs off|N|auto` (or `FRAMESKIP=`): skipped frames drop all draw calls and do not present, which saves GPU and vertex-decoding work
while CPU emulation is unaffected. It stays off for 2 seconds whenever the game copies the EFB to a texture (skipping would leave
those textures stale). The Android launcher has a Frameskip button (default Auto: engages when frames take longer than 24 ms).

## Output filters
`-filter 0..4` (or `FILTER=`; F2 cycles on desktop): 0 off (bilinear), 1 scanlines, 2 sharp-bilinear, 3 Scale2x (EPX, rendered once at 2x
internal size and then scaled smoothly), 4 Scale2x + scanlines. Filters run only in the final present pass and need `highp` float in
fragment shaders (GPUs without it, e.g. Mali-400, fall back to plain bilinear). Scanlines look best when the screen is at least ~2x the
game's 528 lines.

## Internal resolution
`-scale 0.5..1.0` (or `RENDERSCALE=`; launcher: 100% / 75% / 50%) renders the EFB at a fraction of the native 640x528 and stretches it
in the final pass. Texture copies and scissor rectangles are scaled accordingly. It reduces fill-rate and bandwidth, not CPU work.

## Saves (memory card)
Slot A is emulated as a 16 Mbit (251-block) card backed by a raw image: `dra2_card_a.raw` in the working directory on
Linux (override with `DRA2_CARD=/path`, disable with `NOCARD=1`), `card_a.raw` in the app's private storage on Android.
On first start the game itself reports an unformatted card; choose "Format Memory Card" (the autosave then creates the
save file).
