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

// fldigi controls a single TRX/receiver (RX0) via TCI, unlike flrig which
// tracks two independent slices (slice_0/slice_1). Stage 1 is CAT only:
// VFO A frequency/mode/PTT/S-meter/split. Stage 2/3 (RX/TX audio) will add
// fields here as needed.
struct TCI_VFO {
	int freq;
	std::string bw;  // lower, upper pair
	std::string mod; // noun name
	int smeter;
};

struct TCI_VALS {
	TCI_VFO A;
	TCI_VFO B;
	int dds;
	int vol, sql_level, pwr;
	bool ptt, tune, split, sql;
	float tx_power, tx_swr;
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

// RX audio (Stage 2): subscribe/unsubscribe TRX 0's audio stream, and pull
// mono float samples the receiver thread has decoded from binary TCI
// frames into an internal ring buffer. tci_audio_start() requests a fixed
// format (48000 Hz, mono, float32) so SoundTCI never has to branch on
// int16 vs float32 or downmix stereo at the consumer end, even though
// handle_binary() (tci_io.cxx) still accepts either defensively.
extern void tci_audio_start(int trx);
extern void tci_audio_stop(int trx);
extern size_t tci_rx_audio_read(float *buf, size_t count);
extern size_t tci_rx_audio_available(void);

// TX audio (Stage 3): SoundTCI::Write()/Write_stereo() push modem TX audio,
// already resampled to 48000 Hz mono, here. tci_io.cxx's receiver thread
// drains this ring buffer each time a type=3 TX_CHRONO frame arrives from
// the server and answers with a type=2 TX_AUDIO frame -- pacing is driven
// entirely by TX_CHRONO arrival, not a local timer, matching AetherSDR's
// ~1024-frame/~21ms cadence.
extern size_t tci_tx_audio_write(const float *buf, size_t count);

// Bumped by tci_io.cxx every time tci_open() establishes a new underlying
// connection. The TCI CAT connection (Rig Control/TCI tab) and the TCI
// audio device (Soundcard/Devices tab) are independent subsystems -- a CAT
// reconnect can happen without the audio device ever being closed/reopened,
// which would otherwise leave a stale audio_start subscription on a socket
// nobody is using anymore. SoundTCI compares this against the generation it
// last subscribed under (in Read()) to detect that case and re-subscribe.
extern unsigned tci_connection_generation(void);

// UI-update hooks: tci_io.cxx is protocol-only (no fldigi/FLTK dependency
// beyond these four extern points), called from the receiver thread right
// after the corresponding tci_vals field is updated under tci_vals_mutex.
// Implemented in tcicat.cxx, which marshals to the FLTK main thread via
// Fl::awake() (matching src/rigcontrol/xmlrpc_rig.cxx's existing pattern
// for the same purpose) since the TCI receiver thread has no registered
// qrunner thread-id for REQ()-style dispatch.
extern void tci_on_freq_update();
extern void tci_on_mode_update();
extern void tci_on_ptt_update();
extern void tci_on_smeter_update();

#endif
