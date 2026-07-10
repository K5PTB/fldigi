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

#include <string>
#include <pthread.h>

#include "WSclient.h"

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

extern void tci_open(std::string address, std::string port);
extern void tci_close();
extern void tci_send(std::string txt);
extern bool tci_running();

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
