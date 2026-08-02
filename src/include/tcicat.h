// ----------------------------------------------------------------------------
// tcicat.h  --  TCI (Transceiver Control Interface) rig-control adapter
//               for fldigi
//
// Copyright (C) 2026
//		Dave Freese, W1HKJ
//
// This file is part of fldigi.
//
// Fldigi is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Fldigi is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with fldigi.  If not, see <http://www.gnu.org/licenses/>.
// ----------------------------------------------------------------------------

#ifndef TCICAT_H
#define TCICAT_H

// This is the fldigi-integration adapter over tci_io.cxx (protocol-only).
// tcicat.cxx owns the fldigi/FLTK-specific glue: UI marshaling via
// Fl::awake(), the config-driven connect/disconnect lifecycle, and the
// call surface rigsupport.cxx / configuration.cxx / fl_digi.cxx dispatch
// to, mirroring hamlib.h's exposed surface for the Hamlib mechanism.

extern bool tci_init();          // like hamlib_init() / rigCAT_init()
extern void tci_cat_close();     // like hamlib_close() / rigCAT_close()

// Arm the reconnect watchdog in cold-start mode after a failed tci_init():
// TCI stays the configured backend and full init completes automatically
// when the server appears (start-order durability). Called from
// configuration.cxx's initInterface() fallback.
extern void tci_watchdog_arm_pending();

// True while the reconnect watchdog is armed (connected or retrying).
// SoundTCI::Open() uses this to decide throw-now vs open-pending.
extern bool tci_watchdog_active();

extern void tci_set_qsy(unsigned long long f);   // like hamlib_set_qsy
extern void tci_setfreq(unsigned long long f);   // like hamlib_setfreq
extern void tci_setmode(const char *md);         // like rigCAT_setmode
extern void tci_set_ptt(int on);                 // like set_flrig_ptt

// Apply progdefaults.tci_receiver to the protocol layer and re-read the newly
// selected receiver's state. Called from the Rig selector's callback in
// confdialog; safe to call when not connected.
extern void tci_apply_receiver();

// Populate/grey/enable the Rig selector to match the current connection and
// trx_count. Safe to call before the config dialog exists or before
// connecting. Lives here rather than in the fluid-generated confdialog.cxx so
// a regeneration cannot drop it -- same reason as tci_audio_ui_enable().
extern void tci_receiver_ui_refresh();

// Update the "TX: RXn" indicator beside the Rig selector, including the
// warning shown when the radio transmits on a receiver other than the one
// fldigi is driving. Safe before the config dialog exists or before
// connecting. Lives here rather than in the fluid-generated confdialog.cxx
// so a regeneration cannot drop it.
extern void tci_tx_indicator_refresh();

extern bool init_Tci_RigDialog();

#endif
