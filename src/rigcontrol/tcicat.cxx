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
#include <atomic>

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

// ---------------------------------------------------------------------------
// Link watchdog -- notice a dead TCI connection and re-establish it.
//
// Without this, a server restart or network drop was permanent: the receiver
// thread exits, receiver_active goes false, and nothing anywhere retries --
// the Reconnect button ("Press only if you change the address/port") was the
// only way back, and in the default full-duplex audio config SoundTCI::Read()
// keeps pulling an empty ring forever, which is indistinguishable from a
// quiet band. The reported field symptom is exactly this: "the audio feed
// breaks and can't be recovered."
//
// The watchdog runs on the FLTK main thread via Fl::add_timeout -- the same
// thread that owns every other tci_open()/tci_close() call, so no new
// synchronization is introduced. The receiver thread cannot rebuild the
// socket itself: swapping ws needs run_mutex, and taking run_mutex on the
// receiver thread deadlocks against tci_close()'s join-under-lock.
//
// Recovery is complete once tci_open() succeeds: it bumps
// connection_generation, which SoundTCI::Read() observes to re-subscribe RX
// audio (audio_start), and the server's initial parameter burst repopulates
// CAT state through handle_message() as on any fresh connect.
//
// Cost bound: a reconnect attempt to a host that silently drops SYNs blocks
// this thread for up to WS_CONNECT_TIMEOUT_SEC (10 s, WSclient.cxx) -- the
// same bound tci_init() already accepts at config-apply. The exponential
// backoff below keeps that worst case to one bounded stall per retry,
// decaying to one per TCI_RETRY_MAX_S; the common failure (server process
// restarting, host alive) refuses instantly and costs nothing.
// ---------------------------------------------------------------------------
static const double TCI_WATCHDOG_PERIOD_S = 2.0;  // probe cadence while healthy
static const double TCI_RETRY_MIN_S       = 5.0;  // first retry delay
static const double TCI_RETRY_MAX_S       = 60.0; // backoff ceiling

// Atomic: written on the FLTK main thread (arm/disarm) but read from
// trx_thread via tci_watchdog_active() in SoundTCI::Open(). Plain-bool access
// across threads is a data race; the value is only ever a flag, so a relaxed
// atomic is enough to make the read well-defined. The other two watchdog
// statics stay plain -- they are touched only on the main thread.
static std::atomic<bool> tci_watchdog_armed(false);
static double tci_retry_delay = TCI_RETRY_MIN_S;

// False until tci_init() has completed once this run. Distinguishes the
// watchdog's two jobs: RE-connecting a link that was up (tci_open() alone --
// the rig dialog and waterfall state already reflect TCI) versus completing a
// COLD START where fldigi came up before the server did and full init (rig
// dialog, waterfall QSY) is still pending.
static bool tci_ever_connected = false;

static void tci_watchdog_cb(void *)
{
	if (!tci_watchdog_armed) return;

	if (tci_connected()) {
		tci_retry_delay = TCI_RETRY_MIN_S;
		Fl::repeat_timeout(TCI_WATCHDOG_PERIOD_S, tci_watchdog_cb);
		return;
	}

	if (!tci_ever_connected) {
		// Start-order durability: fldigi launched before the TCI server.
		// Run the FULL init path, not a bare tci_open() -- the rig dialog is
		// still in its no-CAT state and the waterfall isn't QSY-enabled.
		// tci_init() arms a fresh watchdog timeout itself on success, so do
		// not also repeat_timeout here (that would leave two timers pending).
		if (tci_init()) {
			LOG_INFO("%s", "TCI server appeared -- rig control initialized");
			wf->USB(true);
			wf->setQSY(1);
			return;
		}
	}
	else {
		LOG_WARN("TCI link down -- reconnecting to %s:%s",
			progdefaults.tci_ip_address.c_str(),
			progdefaults.tci_ip_port.c_str());

		// tci_open() retires the dead receiver thread itself (its "if (ws)
		// tci_close()" preamble) before connecting anew.
		tci_open(progdefaults.tci_ip_address, progdefaults.tci_ip_port);

		if (tci_connected()) {
			LOG_INFO("%s", "TCI link re-established");
			tci_retry_delay = TCI_RETRY_MIN_S;
			Fl::repeat_timeout(TCI_WATCHDOG_PERIOD_S, tci_watchdog_cb);
			return;
		}
	}

	Fl::repeat_timeout(tci_retry_delay, tci_watchdog_cb);
	tci_retry_delay = tci_retry_delay * 2.0 > TCI_RETRY_MAX_S
		? TCI_RETRY_MAX_S : tci_retry_delay * 2.0;
}

// Arm the watchdog in COLD-START mode: TCI is the user's configured backend
// but the server hasn't answered yet this run. Called by configuration.cxx's
// initInterface() fallback instead of silently un-configuring TCI, so "start
// fldigi first, start the radio server whenever" just works -- the watchdog
// completes the full init when the server appears. The immediate "TCI server
// not responding" feedback still fires first, so a typo'd address/port is
// still surfaced to a user standing at the config dialog.
void tci_watchdog_arm_pending()
{
	tci_ever_connected = false;
	tci_watchdog_armed = true;
	tci_retry_delay = TCI_RETRY_MIN_S;
	Fl::remove_timeout(tci_watchdog_cb); // never two pending
	Fl::add_timeout(TCI_RETRY_MIN_S, tci_watchdog_cb);
}

// Is reconnection being handled? SoundTCI::Open() keys its
// throw-vs-open-pending decision on this: with the watchdog armed, a
// disconnected open may proceed (audio self-subscribes on the
// connection_generation bump when the link lands, and the silence-filling
// read callback keeps the interim safe and paced); without it, nothing
// would ever recover the device, so failing loudly remains correct.
bool tci_watchdog_active()
{
	return tci_watchdog_armed;
}

bool tci_init()
{
	std::string address = progdefaults.tci_ip_address;
	std::string port = progdefaults.tci_ip_port;

	// Push the configured receiver in BEFORE opening: tci_io.cxx is
	// protocol-only and never reads progdefaults, and the init burst starts
	// arriving the moment the socket is up. Setting it afterwards would race
	// the first vfo:/modulation: reports, which are filtered by receiver.
	// The value is re-clamped against trx_count when that arrives.
	tci_set_receiver(progdefaults.tci_receiver);

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

	// Arm the watchdog. A failed initial connect does not reach here -- it
	// returns false above and configuration.cxx decides whether to arm the
	// cold-start retry (tci_watchdog_arm_pending) after surfacing the
	// failure to the user.
	tci_ever_connected = true;
	tci_watchdog_armed = true;
	tci_retry_delay = TCI_RETRY_MIN_S;
	Fl::remove_timeout(tci_watchdog_cb); // never two pending
	Fl::add_timeout(TCI_WATCHDOG_PERIOD_S, tci_watchdog_cb);

	return true;
}

// The user changed the Rig selection (Rig Control/TCI -> Rig). Called from the
// FLTK main thread.
//
// Audio deliberately is NOT re-subscribed here: SoundTCI::Read() notices
// tci_receiver() no longer matches what it subscribed to and does the
// stop/discard/start itself, because it runs on trx_thread and is the only
// thread allowed to move the RX ring's read index.
//
// What this DOES do is ask the new receiver for its current state. Every
// inbound report is filtered by receiver, so without a re-read the frequency
// and mode on screen would stay at the old receiver's values until the new
// one happened to change by itself -- looking exactly like the switch had not
// taken effect.
void tci_apply_receiver()
{
	tci_set_receiver(progdefaults.tci_receiver);

	if (!tci_running()) return;

	char cmd[32];
	// TCI read form: name:args; with the value omitted (spec section 4.2).
	snprintf(cmd, sizeof(cmd), "vfo:%d,0;", tci_receiver());
	tci_send(cmd);
	snprintf(cmd, sizeof(cmd), "modulation:%d;", tci_receiver());
	tci_send(cmd);
	snprintf(cmd, sizeof(cmd), "rx_smeter:%d,0;", tci_receiver());
	tci_send(cmd);

	LOG_INFO("TCI receiver -> RX%d (of %d reported)",
		tci_receiver() + 1, tci_trx_count());
}

void tci_cat_close()
{
	// Disarm first: this is the user/config-driven teardown, the one case
	// where a downed link must STAY down.
	tci_watchdog_armed = false;
	tci_ever_connected = false;
	Fl::remove_timeout(tci_watchdog_cb);

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
	// vfo:<receiver>,<channel>,<hz>; -- channel stays 0 (VFO A); the receiver
	// is the user's Rig selection. See tci_io.h for why one index drives both
	// CAT and audio.
	snprintf(cmd, sizeof(cmd), "vfo:%d,0,%llu;", tci_receiver(), f);
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
	char pfx[24];
	snprintf(pfx, sizeof(pfx), "modulation:%d,", tci_receiver());
	std::string cmd(pfx);
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
	// Keys the SELECTED receiver -- the same one the TX audio is subscribed
	// to, which is guaranteed because a single index drives both (tci_io.h).
	// Transmitting on a receiver whose audio we are not feeding would be
	// silent carrier on someone else's slice.
	char cmd[40];
	if (on && progdefaults.btnAudioIOis == SND_IDX_TCI)
		snprintf(cmd, sizeof(cmd), "TRX:%d,true,tci;", tci_receiver());
	else
		snprintf(cmd, sizeof(cmd), "TRX:%d,%s;", tci_receiver(), on ? "true" : "false");
	tci_send(cmd);
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
