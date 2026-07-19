# Building fldigi (with TCI) from source

This fork's default branch (`master`) has the full TCI feature plus a FLTK 1.4
Wayland launch-crash fix. A plain clone builds it.

Two dependencies are **bundled in the source tree** — mbedtls (`src/mbedtls/`,
used by the WebSocket client) and the FFT — so you never install those
separately. Hamlib is optional (only for hardware CAT; TCI does not need it).

## FLTK version — please note

fldigi builds against **FLTK 1.4** or **FLTK 1.3**:

- **FLTK 1.4 — tested.** macOS (Homebrew 1.4.5), Raspberry Pi OS / Debian
  trixie (1.4.3), and the Windows build all use FLTK 1.4.
- **FLTK 1.3 — supported in source, but NOT yet verified.** The tree keeps
  upstream fldigi's FLTK 1.3 code paths, so it *should* build on older distros
  (e.g. Debian 12, Ubuntu 22.04) whose `apt` only provides FLTK 1.3. **This has
  not yet been compiled on a 1.3 system.** If you build it on FLTK 1.3, please
  report success or failure — testers wanted.

---

## Linux — Debian / Ubuntu / Raspberry Pi OS

```sh
# 1. Dependencies
sudo apt-get update
sudo apt-get install -y \
    build-essential autoconf automake libtool pkg-config gettext autopoint \
    libfltk1.4-dev libsamplerate0-dev libsndfile1-dev libpng-dev \
    portaudio19-dev libpulse-dev libasound2-dev \
    libhamlib-dev libxft-dev libxinerama-dev

# 2. Source (master = TCI feature + Wayland fix)
git clone https://github.com/K5PTB/fldigi.git
cd fldigi

# 3. Build
autoreconf -vfi        # the git tree ships only configure.ac, no ./configure
./configure            # leave FLTK_CFLAGS / FLTK_LIBS unset
make -j"$(nproc)"

# 4. Run
./src/fldigi           # or: sudo make install
```

- **Verified** on Raspberry Pi 5 / Debian trixie (aarch64); the same steps apply
  to x86-64 Debian/Ubuntu.
- On an older release whose `apt` only has FLTK 1.3, install `libfltk1.3-dev`
  instead of `libfltk1.4-dev` — but see the FLTK-version note above: 1.3 is not
  yet verified.

## macOS — Intel (x86-64) and Apple Silicon (ARM)

Uses [Homebrew](https://brew.sh/); the commands are identical on both
architectures (`$(brew --prefix)` resolves to `/usr/local` on Intel and
`/opt/homebrew` on Apple Silicon).

```sh
# 1. Dependencies
brew install fltk libsamplerate libsndfile portaudio \
    pkg-config autoconf automake libtool gettext

# 2. Source
git clone https://github.com/K5PTB/fldigi.git
cd fldigi

# 3. Build
export PATH="$(brew --prefix gettext)/bin:$PATH"     # autopoint, for autoreconf
autoreconf -vfi
./configure \
    PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig:$(brew --prefix libsndfile)/lib/pkgconfig" \
    FLTK_CONFIG="$(brew --prefix)/bin/fltk-config"
make -j"$(sysctl -n hw.ncpu)"

# 4. Run
./src/fldigi
```

- Homebrew's `fltk` is 1.4.x — what you want.
- `brew install hamlib` too if you also want hardware rig control (optional,
  autodetected).
- This produces a runnable `./src/fldigi`; building a distributable `.app`
  bundle is a separate packaging step.

## Windows

A prebuilt portable zip is on the
[Releases page](https://github.com/K5PTB/fldigi/releases). Building from source
on Windows uses MSYS2 / MinGW64 with a few workarounds — ask if you need the
steps.

## Rebuild gotcha (all platforms)

Do **not** run `./config.status --recheck` or `make --always-make` — they blank
the FLTK version macros. If a rebuild fails with an FLTK-header preprocessor
error, check `grep FLDIGI_FLTK_API src/config.h`; if it is empty, re-run a plain
`./configure`.
