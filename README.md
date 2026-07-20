# fldigi (K5PTB fork) — with native TCI support

A fork of [fldigi](https://sourceforge.net/projects/fldigi/) that adds native
**TCI (Transceiver Control Interface)** support: CAT rig control **and**
full-duplex RX/TX audio over a single WebSocket connection to an
ExpertSDR / SunSDR / AetherSDR-class SDR server — with **no** hardware CAT port
and **no** virtual audio cable. Everything else is stock fldigi 4.2.11 (all the
usual digital modes, logging, etc.).

## Download & install

| Platform | How |
|----------|-----|
| **Windows 11 (x64)** | Download the portable zip from [**Releases**](https://github.com/K5PTB/fldigi/releases) (`fldigi-4.2.11-tci-win64.zip`), unzip anywhere, run `fldigi.exe`. No installer. |
| **Linux** (Debian / Ubuntu / Raspberry Pi OS) | Build from source — see **[BUILDING.md](BUILDING.md)** (a few minutes). |
| **macOS** (Intel & Apple Silicon) | Build from source — see **[BUILDING.md](BUILDING.md)**. |

There is intentionally no prebuilt Linux binary: Linux users build against their
own system libraries, exactly as with upstream fldigi.

## Build from source (Linux & macOS)

Full, copy-paste dependency and build steps are in **[BUILDING.md](BUILDING.md)**
(Debian/Ubuntu/Raspberry Pi OS, and macOS Intel/Apple Silicon). The shape of it:

```sh
git clone https://github.com/K5PTB/fldigi.git
cd fldigi
autoreconf -vfi        # needs gettext/autopoint — macOS also needs it on PATH; see BUILDING.md
./configure            # macOS needs an FLTK_CONFIG line — see BUILDING.md
make -j4
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
the `ws://` address and port (default `50001`).

## About this fork

Based on fldigi **4.2.11**. The only additions are the TCI feature and a small
FLTK 1.4 Wayland launch-crash fix; non-TCI behavior is unchanged. For the full
list of modes, features, and documentation, see the upstream project:

- Upstream source: <https://sourceforge.net/projects/fldigi/>
- Documentation: <http://www.w1hkj.org/>
