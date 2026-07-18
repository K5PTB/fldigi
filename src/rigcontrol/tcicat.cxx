// ----------------------------------------------------------------------------
// tcicat.cxx  --  TCI (Transceiver Control Interface) rig-control adapter
//                 for fldigi
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

#include <config.h>

#include <string>
#include <cstdio>
#include <cctype>

#include <FL/Fl.H>

#include "tcicat.h"
#include "tci_io.h"

#include "fl_digi.h"
#include "trx.h"
#include "configuration.h"
#include "rigsupport.h"
#include "threads.h"
#include "misc.h"
#include "debug.h"
#include "status.h"
#include "soundconf.h"

LOG_FILE_SOURCE(debug::LOG_RIGCONTROL);

// TCI's fixed rig-mode vocabulary (matches flrig's tcisdr.cxx vTCI_modes[]
// for the SunSDR2/ExpertSDR3/AetherSDR TCI dialect). Stage 1 does not
// implement bandwidth control, so qso_opBW is left deactivated for TCI,
// matching init_NoRig_RigDialog()'s placeholder approach.
static const char *tci_modes[] = {
	"AM", "SAM", "DSB", "LSB", "USB", "CW", "NFM", "DIGL", "DIGU", "WFM", "DRM"
};

bool tci_init()
{
	std::string address = progdefaults.tci_ip_address;
	std::string port = progdefaults.tci_ip_port;

	tci_open(address, port);

	// tci_open() starts the receiver thread and returns immediately; the
	// WebSocket handshake happens asynchronously. Give it a short window
	// to complete before deciding whether to fall back to no-CAT, mirroring
	// hamlib_init()'s live-response check.
	int tries = 20; // ~200ms
	while (tries-- && !tci_running())
		MilliSleep(10);

	if (!tci_running()) {
		LOG_ERROR("%s", "TCI server not responding");
		tci_close();
		return false;
	}

	LOG_INFO("TCI connected: %s:%s", address.c_str(), port.c_str());

	init_Tci_RigDialog();

	return true;
}

void tci_cat_close()
{
	tci_close();
}

void tci_set_qsy(unsigned long long f)
{
	tci_setfreq(f);
	if (wf) {
		wf->rfcarrier(f);
		wf->movetocenter();
	}
	show_frequency(f);
	LOG_VERBOSE("set qsy: %llu", f);
}

void tci_setfreq(unsigned long long f)
{
	if (!tci_running()) return;
	char cmd[40];
	snprintf(cmd, sizeof(cmd), "vfo:0,0,%llu;", f);
	tci_send(cmd);
}

void tci_setmode(const char *md)
{
	if (!tci_running() || !md) return;
	// TCI modulation tokens are lowercase ("usb", "cw", "digu", ...), but the
	// mode menu carries the uppercase tci_modes[] labels. Sending the label
	// verbatim ("modulation:0,USB;") is ignored by a case-sensitive server, so
	// the radio never follows fldigi's mode selection. Lowercase before sending.
	std::string mode(md);
	for (size_t i = 0; i < mode.size(); i++)
		mode[i] = (char)tolower((unsigned char)mode[i]);
	std::string cmd = "modulation:0,";
	cmd.append(mode).append(";");
	tci_send(cmd);
}

void tci_set_ptt(int on)
{
	if (!progdefaults.chkUSETCIis) return;
	if (!tci_running()) return;

	// Explicit ",tci" on key-up requests TCI-audio TX routing regardless of
	// the slice's current mode -- AetherSDR only auto-selects that route
	// for a handful of mode names (DIGU/DIGL/RTTY/FDV*) when no 3rd arg is
	// given, and fldigi users commonly run digital modes with the rig set
	// to USB/LSB rather than those literal names. Only requested when TCI
	// is actually the active TX audio backend; CAT-only PTT (soundcard/DAX
	// still carrying the audio) must not force the server onto TX_CHRONO.
	if (on && progdefaults.btnAudioIOis == SND_IDX_TCI)
		tci_send("TRX:0,true,tci;");
	else
		tci_send(on ? "TRX:0,true;" : "TRX:0,false;");
}

bool init_Tci_RigDialog()
{
	LOG_DEBUG("%s", "tci");

	qso_opBW->clear();
	qso_opBW->add("  ");
	qso_opBW->index(0);
	qso_opBW->redraw();
	qso_opBW->deactivate();

	qso_opMODE->clear();
	for (size_t i = 0; i < sizeof(tci_modes)/sizeof(tci_modes[0]); i++)
		qso_opMODE->add(tci_modes[i]);
	qso_opMODE->index(4); // USB
	qso_opMODE->activate();

	xcvr_title = "TCI";
	setTitle();

	return true;
}

// ----------------------------------------------------------------------------
// UI-update hooks called from tci_io.cxx's receiver thread (background
// pthread, not FLMAIN_TID). Marshal to the FLTK main thread via Fl::awake(),
// matching src/rigcontrol/xmlrpc_rig.cxx's existing pattern for the same
// purpose (the TCI receiver thread has no registered qrunner thread-id, so
// REQ()-style dispatch, as used by hamlib_loop(), is not available here).
// ----------------------------------------------------------------------------

static void tci_do_freq_update(void *)
{
	guard_lock lock(&tci_vals_mutex);
	unsigned long long f = (unsigned long long)tci_vals.A.freq;
	show_frequency(f);
	if (wf) wf->rfcarrier(f);
}

void tci_on_freq_update()
{
	Fl::awake(tci_do_freq_update);
}

static void tci_do_mode_update(void *)
{
	if (!qso_opMODE) return;
	guard_lock lock(&tci_vals_mutex);
	// value(const char*) leaves the widget unchanged if the string matches no
	// menu item, so a modulation name outside tci_modes[] would silently show a
	// stale mode. Note it rather than fail quietly.
	const std::string& m = tci_vals.A.mod;
	for (size_t i = 0; i < sizeof(tci_modes)/sizeof(tci_modes[0]); i++) {
		if (m == tci_modes[i]) {
			qso_opMODE->value(m.c_str());
			return;
		}
	}
	LOG_VERBOSE("TCI modulation '%s' not in mode list -- display unchanged", m.c_str());
}

void tci_on_mode_update()
{
	Fl::awake(tci_do_mode_update);
}

static void tci_do_smeter_update(void *)
{
	guard_lock lock(&tci_vals_mutex);
	if (smeter && progStatus.meters) {
		if (!smeter->visible()) {
			if (pwrmeter) pwrmeter->hide();
			smeter->show();
		}
		smeter->value(tci_vals.A.smeter);
	}
}

void tci_on_smeter_update()
{
	Fl::awake(tci_do_smeter_update);
}
