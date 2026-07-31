// ---------------------------------------------------------------------
//
// tci_io.h, a part of fldigi (adapted from flrig's tci_io.hpp)
//
// Copyright (C) 2022
// Dave Freese, W1HKJ
//
// This library is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with the program; if not, write to the
//
//  Free Software Foundation, Inc.
//  51 Franklin Street, Fifth Floor
//  Boston, MA  02110-1301 USA.
//
// ---------------------------------------------------------------------
#ifndef TCI_IO_H
#define TCI_IO_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <pthread.h>

#include "WSclient.h"

// RX audio is always requested mono at this rate (matches AetherSDR's
// TciServer default and its WSJT-X-compatible optimization path).
#define TCI_AUDIO_SAMPLE_RATE 48000

// fldigi controls ONE TCI receiver at a time, but which one is selectable
// (Rig Control/TCI -> Rig), unlike flrig which tracks two independent slices
// (slice_0/slice_1) simultaneously. Only the fields fldigi actually consumes
// are kept: everything else the protocol reports (DDS, volume, drive, squelch,
// tune, split, TX power/SWR, filter bandwidth and inbound PTT) had no reader
// and is no longer parsed -- see handle_command() in tci_io.cxx.
struct TCI_VFO {
	int freq;
	std::string mod;
	int smeter;
};

struct TCI_VALS {
	TCI_VFO A;
};

extern TCI_VALS tci_vals;
extern pthread_mutex_t tci_vals_mutex;

// TCI binary audio frame header, verified against AetherSDR's
// TciServer.cpp (TciAudioHeader, TciServer.cpp:55-66): 64 bytes, 16 x
// uint32_t, sent host-native/little-endian with no byte-swap.
struct TciAudioHeader {
	uint32_t receiver;
	uint32_t sampleRate;
	uint32_t format;   // 0=int16, 1=int24, 2=int32, 3=float32
	uint32_t codec;    // 0 (uncompressed)
	uint32_t crc;      // 0 (unused)
	uint32_t length;   // total sample count = frames * channels
	uint32_t type;     // 0=IQ, 1=RX_AUDIO, 2=TX_AUDIO, 3=TX_CHRONO
	uint32_t channels;
	uint32_t reserved[8];
};

extern void tci_open(std::string address, std::string port);
extern void tci_close();
extern void tci_send(std::string txt);
extern bool tci_running();

// ---------------------------------------------------------------------
// Receiver selection.
//
// TCI addresses state with TWO independent indexes: the RECEIVER (`trx`,
// an independent receiver -- on AetherSDR one owned FlexRadio slice, with
// its own frequency, mode, S-meter AND audio stream) and the CHANNEL
// (VFO A/B within that receiver, the split axis). fldigi drives one
// receiver, VFO A. This block owns the receiver; the channel stays 0.
//
// ONE index drives CAT *and* audio -- deliberately, matching WSJT-X, whose
// TCITransceiver has a single `rx_` member behind audio_start, both audio
// frame filters, PTT, vfo, modulation and every inbound notification guard.
// Splitting them would let fldigi log a QSO at one receiver's frequency
// while decoding another's audio, silently writing the wrong frequency into
// the ADIF.
//
// This file stays protocol-only -- it must not read progdefaults (nothing
// else here does). tcicat.cxx, which owns the fldigi-side config, pushes the
// selection in with tci_set_receiver(), mirroring how the tci_on_*_update()
// hooks push the other way.
//
// Thread safety: written on the FLTK main thread, read on the receiver
// thread for every inbound frame -- so the value is atomic, for the same
// reason tci_watchdog_armed is (see tcicat.cxx).

// Highest receiver fldigi offers. 8 = the FLEX-6700's slice ceiling, the
// largest receiver count AetherSDR can advertise. A server reporting more
// is capped here rather than misaddressed.
#define TCI_MAX_RECEIVERS 8

// Select the receiver. Clamped to [0, TCI_MAX_RECEIVERS-1] and, once the
// server has reported trx_count, to what actually exists -- an out-of-range
// trx is NOT refused by AetherSDR, it silently resolves to the first owned
// slice, so an unclamped value drives the wrong receiver with no error
// anywhere. Safe to call before connecting; re-applied against trx_count
// when the init burst arrives.
extern void tci_set_receiver(int trx);

// The receiver actually in use: what tci_set_receiver() was given, after
// clamping. Every outbound command and inbound filter uses this, never a
// literal 0.
extern int tci_receiver(void);

// Init-burst capabilities (TCI Protocol v2.0 section 4.1). Both default to 1
// so a server that omits them behaves exactly as a single-receiver rig.
// trx_count re-sends whenever the count changes, so this tracks slices
// opening and closing rather than being latched at connect.
extern int tci_trx_count(void);
extern int tci_channels_count(void);

// RX audio: subscribe/unsubscribe a TRX's audio stream, and pull
// mono float samples the receiver thread has decoded from binary TCI
// frames into an internal ring buffer. tci_audio_start() requests a fixed
// format (48000 Hz, mono, float32) so SoundTCI never has to branch on
// int16 vs float32 or downmix stereo at the consumer end, even though
// handle_binary() (tci_io.cxx) still accepts either defensively.
extern void tci_audio_start(int trx);
extern void tci_audio_stop(int trx);
extern size_t tci_rx_audio_read(float *buf, size_t count);
extern size_t tci_rx_audio_read_wait(float *buf, size_t count, int timeout_ms);

// Drop whatever RX audio is queued, returning the number of samples dropped.
// Must be called from the RX consumer's thread (trx_thread) -- it moves the
// reader's own ring index. Backs SoundTCI::flush(O_RDONLY).
extern size_t tci_rx_audio_discard(void);

// TX audio: SoundTCI::Write()/Write_stereo() push modem TX audio,
// already resampled to 48000 Hz mono, here. tci_io.cxx's receiver thread
// drains this ring buffer each time a type=3 TX_CHRONO frame arrives from
// the server and answers with a type=2 TX_AUDIO frame -- pacing is driven
// entirely by TX_CHRONO arrival, not a local timer, matching AetherSDR's
// ~1024-frame/~21ms cadence.
extern size_t tci_tx_audio_write(const float *buf, size_t count);

// Blocks until the server has pulled the queued TX audio, so SoundTCI::flush()
// can honour SoundBase's drain-before-PTT-drop contract. Bounded; returns
// false and discards the residue if the server stops pulling. Must not be
// called from tci_io.cxx's own receiver thread.
extern bool tci_tx_audio_drain(void);

// tci_running() without the receiver thread's lifetime guarantees -- takes
// run_mutex, so it is safe from trx_thread/main but NOT from the receiver
// thread. Use this from anything that is not tci_loop().
extern bool tci_connected(void);

// Bumped by tci_io.cxx every time tci_open() establishes a new underlying
// connection. The TCI CAT connection (Rig Control/TCI tab) and the TCI
// audio device (Soundcard/Devices tab) are independent subsystems -- a CAT
// reconnect can happen without the audio device ever being closed/reopened,
// which would otherwise leave a stale audio_start subscription on a socket
// nobody is using anymore. SoundTCI compares this against the generation it
// last subscribed under (in Read()) to detect that case and re-subscribe.
extern unsigned tci_connection_generation(void);

// UI-update hooks: tci_io.cxx is protocol-only (no fldigi/FLTK dependency
// beyond these three extern points), called from the receiver thread right
// after the corresponding tci_vals field is updated under tci_vals_mutex.
// Implemented in tcicat.cxx, which marshals to the FLTK main thread via
// Fl::awake() (matching src/rigcontrol/xmlrpc_rig.cxx's existing pattern
// for the same purpose) since the TCI receiver thread has no registered
// qrunner thread-id for REQ()-style dispatch.
//
// There is deliberately no tci_on_ptt_update: inbound radio PTT must not reach
// fldigi's TX state (see the TRX note in handle_command()).
extern void tci_on_freq_update();
extern void tci_on_mode_update();
extern void tci_on_smeter_update();

#endif
