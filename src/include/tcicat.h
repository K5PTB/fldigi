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
extern bool tci_cat_active();    // like hamlib_active()

extern void tci_set_qsy(unsigned long long f);   // like hamlib_set_qsy
extern void tci_setfreq(unsigned long long f);   // like hamlib_setfreq
extern void tci_setmode(const char *md);         // like rigCAT_setmode
extern void tci_set_ptt(int on);                 // like set_flrig_ptt

extern bool init_Tci_RigDialog();

#endif
