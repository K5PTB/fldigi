# fldigi (K5PTB fork) — with native TCI support

A fork of [fldigi](https://sourceforge.net/projects/fldigi/) that adds native
**TCI (Transceiver Control Interface)** support: CAT rig control **and**
full-duplex RX/TX audio over a single WebSocket connection to an
ExpertSDR / SunSDR / AetherSDR-class SDR server — with **no** hardware CAT port
and **no** virtual audio cable. Everything else is stock fldigi 4.2.13 (all the
usual digital modes, logging, etc.).

## Download & install

| Platform | How |
|----------|-----|
| **Windows 11 (x64)** | Download `fldigi-4.2.13-tci-win64.zip` from [**Releases**](https://github.com/K5PTB/fldigi/releases), unzip anywhere, run `fldigi.exe`. No installer. |
| **Linux** (x86‑64) | Download the portable `fldigi-4.2.13-tci-x86_64.AppImage` from [**Releases**](https://github.com/K5PTB/fldigi/releases), `chmod +x` it, and run — nothing to install. Or [build from source](BUILDING.md). |
| **macOS** (Intel & Apple Silicon) | Build from source — see **[BUILDING.md](BUILDING.md)**. |

The Windows zip and the Linux AppImage are built by GitHub Actions from the
tagged release and each carries a `BUILD-INFO.txt` naming the exact commit. The
AppImage is **x86‑64 only** — on ARM (e.g. Raspberry Pi) build from source; the
steps are in [BUILDING.md](BUILDING.md) and are verified on the Pi 5. There is no
prebuilt **macOS** binary: an unsigned Mac app can't be distributed without an
Apple Developer certificate, so macOS is source-build (a bundle *you* build opens
normally).

## Build from source (Linux & macOS)

Full, copy-paste dependency and build steps are in **[BUILDING.md](BUILDING.md)**
(Debian/Ubuntu/Raspberry Pi OS, and macOS Intel/Apple Silicon). The shape of it:

```sh
git clone https://github.com/K5PTB/fldigi.git
cd fldigi
autoreconf -vfi        # needs gettext/autopoint — macOS also needs it on PATH; see BUILDING.md
./configure            # macOS needs an FLTK_CONFIG line — see BUILDING.md
make -C src -j4        # builds just the app (skips the man-page docs)
./src/fldigi
```

…but **install the dependencies first** — see [BUILDING.md](BUILDING.md).

## What is TCI?

TCI (Transceiver Control Interface) is a WebSocket protocol used by ExpertSDR3,
SunSDR, and AetherSDR-class SDR servers. This fork speaks it directly, so fldigi
can control the radio (frequency, mode, PTT, S-meter) **and** exchange
receive/transmit audio over one network connection — replacing both the
serial/hamlib CAT link and the soundcard / virtual-audio path a hardware setup
would otherwise need.

Select it in **Rig Control → TCI** and **Soundcard → Devices → TCI**, then set
the `ws://` address and port (default `50001`). On a multi-slice radio, choose
which receiver fldigi drives (for rig control **and** audio alike) under
**Rig Control → TCI → Rig**.

## Reporting TCI issues — your help is appreciated

TCI support here is new and under active testing. **The intent is to contribute
it upstream to fldigi once it has been sufficiently tested.** If you find any
issues associated with TCI, please
[**open an issue**](https://github.com/K5PTB/fldigi/issues) on this repo —
reports from real-world use across different radios and platforms are exactly
what get this feature ready for upstream. Your help would be appreciated.

## About this fork

Based on fldigi **4.2.13**. The only addition is the TCI feature; non-TCI
behavior is unchanged. For the full
list of modes, features, and documentation, see the upstream project:

- Upstream source: <https://sourceforge.net/projects/fldigi/>
- Documentation: <http://www.w1hkj.org/>
