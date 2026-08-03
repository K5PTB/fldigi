# Building fldigi (with TCI) from source

This fork's default branch (`master`) has the full TCI feature on top of
fldigi 4.2.13. A plain clone builds it.

Two dependencies are **bundled in the source tree** — mbedtls (`src/mbedtls/`,
used by the WebSocket client) and the FFT — so you never install those
separately. Hamlib is optional (only for hardware CAT; TCI does not need it).

The build command below is `make -C src`, which builds **just the fldigi
application**. It deliberately skips the man-page / HTML documentation build
(that needs the `asciidoc` + `xsltproc` toolchain and is fragile on some
systems); none of it is needed to run fldigi.

## FLTK version

The instructions below use **FLTK 1.4** — what Raspberry Pi OS / Debian trixie
(1.4.3) and the macOS and Windows builds all ship. This fork adds no FLTK
version requirement of its own: FLTK 1.3 also builds (compile-checked in CI on
every push), exactly as upstream fldigi does.

---

## Linux — Debian / Ubuntu / Raspberry Pi OS

```sh
# 1. Dependencies
sudo apt-get update
sudo apt-get install -y \
    build-essential autoconf automake libtool pkg-config gettext autopoint \
    libfltk1.4-dev libsamplerate0-dev libsndfile1-dev libpng-dev \
    portaudio19-dev libpulse-dev libasound2-dev \
    libhamlib-dev libxft-dev libxinerama-dev libudev-dev

# 2. Source (master = TCI feature on fldigi 4.2.13)
git clone https://github.com/K5PTB/fldigi.git
cd fldigi

# 3. Build (just the application; skips the man-page/doc build)
autoreconf -vfi        # the git tree ships only configure.ac, no ./configure
./configure            # leave FLTK_CFLAGS / FLTK_LIBS unset
make -C src -j4        # -C src avoids the asciidoc/xsltproc man-page build

# 4. Run
./src/fldigi
```

- **Verified** on Raspberry Pi 5 / Debian trixie (aarch64); the same steps apply
  to x86-64 Debian/Ubuntu.
- If your distribution ships FLTK 1.3 rather than 1.4, install `libfltk1.3-dev`
  instead of `libfltk1.4-dev` — it builds the same.

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

# 3. Build (just the application; skips the man-page/doc build)
export PATH="$(brew --prefix gettext)/bin:$PATH"     # autopoint, for autoreconf
autoreconf -vfi
./configure \
    PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig:$(brew --prefix libsndfile)/lib/pkgconfig" \
    FLTK_CONFIG="$(brew --prefix)/bin/fltk-config" \
    LDFLAGS="-L$(brew --prefix)/lib"
make -C src -j4        # -C src avoids the asciidoc/xsltproc man-page build

# 4. Run
./src/fldigi
```

- Homebrew's `fltk` is 1.4.x — what you want.
- `brew install hamlib` too if you also want hardware rig control (optional,
  autodetected).
- **`LDFLAGS` is required on Apple Silicon, not optional.** FLTK 1.4.5's
  `fltk-config` emits `-ljpeg`, and `/opt/homebrew/lib` is not a default linker
  search path on arm64 — while `/usr/local/lib` is, on Intel. Without it the
  link fails with `ld: library 'jpeg' not found`. It is harmless on Intel, so
  the line above is correct for both.

### Optional: a double-clickable `.app` bundle

`make -C src appbundle` wraps the binary, copies its Homebrew dylibs into
`Contents/Frameworks`, and rewrites their install paths so the result is
self-contained:

```sh
make -C src appbundle
```

**On Apple Silicon you must re-sign afterwards, or macOS will kill the app**
(it exits 137 with no message). `install_name_tool` invalidates the ad-hoc code
signatures that arm64 requires, and the bundler does not re-apply them:

```sh
cd src/fldigi-*/
find fldigi-*.app/Contents/Frameworks -name '*.dylib' -exec codesign --force --sign - {} \;
codesign --force --sign - fldigi-*.app/Contents/MacOS/*
codesign --force --sign - fldigi-*.app
```

Verify with `codesign -v fldigi-*.app` (silence means good) and by running
`fldigi-*.app/Contents/MacOS/fldigi --version`.

A bundle you built yourself opens normally. One you *download* is quarantined
by macOS and — because these builds are not signed with an Apple Developer
certificate — will be refused with *"Apple could not verify … is free of
malware"*, offering **Move to Trash**. Clear it with
`xattr -dr com.apple.quarantine <path>.app`, or approve it under
**System Settings → Privacy & Security → Open Anyway**.

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
