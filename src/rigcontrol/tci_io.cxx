// ---------------------------------------------------------------------
//
// tci_io.cxx, a part of fldigi (adapted from flrig's tci_io.cxx)
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
// GNU General Public License for more detailrx.
//
// You should have received a copy of the GNU General Public License
// along with the program; if not, write to the
//
//  Free Software Foundation, Inc.
//  51 Franklin Street, Fifth Floor
//  Boston, MA  02110-1301 USA.
//
// ---------------------------------------------------------------------

#include "tci_io.h"

#include <atomic>
#include <list>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <climits>
#include <exception>

#include <assert.h>
#include <stdio.h>
#include <ctype.h>

#include "ringbuffer.h"
#include "threads.h"
#include "misc.h"
#include "strutil.h"
#include "timeops.h"
#include "debug.h"

LOG_FILE_SOURCE(debug::LOG_RIGCONTROL);

TCI_VALS tci_vals;
pthread_mutex_t tci_vals_mutex = PTHREAD_MUTEX_INITIALIZER;

using WSclient::WebSocket;

static WebSocket::pointer ws = (WebSocket::pointer)0;

// Single-writer (this file's receiver thread, via handle_binary()) /
// single-reader (SoundTCI::Read(), called from trx_thread) ring buffer of
// decoded mono float samples at TCI_AUDIO_SAMPLE_RATE. ~1.4s of audio at
// 48kHz -- generous enough to absorb jitter between TCI's ~2048-sample
// frame cadence and the modem's much smaller SCBLOCKSIZE reads.
static ringbuffer<float> rx_audio_rb(65536);

// Opposite direction/roles from rx_audio_rb: written by SoundTCI::Write()
// (trx_thread, mono float already resampled to TCI_AUDIO_SAMPLE_RATE),
// read by handle_binary() (this file's receiver thread) when a TX_CHRONO
// frame arrives and a TX_AUDIO reply is due. Still single-writer/
// single-reader, just with the writer/reader roles swapped vs RX.
static ringbuffer<float> tx_audio_rb(65536);

static_assert(sizeof(TciAudioHeader) == 64, "TCI audio header must be 64 bytes");

// Discarding stale TX audio without either thread reaching into the other's
// ring index.
//
// tx_audio_rb is single-writer (trx_thread) / single-reader (the receiver
// thread), and ringbuffer<T> permits nothing else: only the reader may move
// ridx, only the writer may move widx, and reset() writes both with no
// barrier. So the writer cannot drop the residue itself -- it can only say
// which samples are stale and let the reader skip them.
//
// A flag would not do. "Discard what's queued" set at the end of one over can
// be observed by the reader after the next over has already been written, and
// would then eat the start of it. A MARK does: tx_discard_mark is a point in
// the write stream, so the reader skips only samples written before it and
// anything written after is always kept, regardless of when the reader
// happens to run. That makes the handshake order-independent, which matters
// precisely because the stall case is one where the reader is not running.
//
// Counters are in samples and monotonic. At 48 kHz a size_t wraps in ~12
// million years.
static std::atomic<size_t> tx_written(0);      // writer-owned, published
static std::atomic<size_t> tx_discard_mark(0); // writer-owned, published
static size_t tx_read = 0;                     // reader-owned, private

// Wake-ups between the receiver thread and trx_thread, replacing 5 ms poll
// loops with event signalling. Each mutex guards only its wait/signal
// handshake, not the ring (the rings keep their own single-reader/writer
// safety). The lost-wakeup-free rule: the waiter checks the ring predicate
// while holding the mutex before waiting, and the signaller takes the same
// mutex around its signal -- so a write that lands between the waiter's check
// and its wait cannot be missed.
//
// tx: handle_tx_chrono (receiver) signals after draining tx_audio_rb;
//     tci_tx_audio_write (trx) waits while more than `lead` is still queued.
// rx: handle_binary (receiver) signals after filling rx_audio_rb;
//     src_read_cb (trx) waits for the ring to become non-empty.
static pthread_mutex_t tx_wake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  tx_wake_cond  = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t rx_wake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  rx_wake_cond  = PTHREAD_COND_INITIALIZER;

// True while the receiver thread is running and therefore able to drain the TX
// ring. Set by tci_open() once the thread is created, cleared by tci_close()
// and by the thread itself on exit (a server-side socket drop).
//
// The TX back-pressure/drain waits below use THIS instead of tci_connected():
// tci_connected() takes run_mutex, and those waits run on trx_thread while
// holding tx_wake_mutex. tci_close() holds run_mutex across pthread_join() of
// the receiver, and the receiver takes tx_wake_mutex in handle_tx_chrono() --
// so "check tci_connected() under tx_wake_mutex" closes a three-way deadlock
// (trx: tx_wake_mutex->run_mutex; closer: run_mutex->join; receiver:
// ->tx_wake_mutex). A lock-free flag breaks the cycle: the wait never reaches
// for run_mutex, and a stopped receiver flips it false so the wait ends
// promptly instead of burning the full 2 s bound.
static std::atomic<bool> receiver_active(false);

// Reader side. Skip anything the writer marked stale, never past the mark.
static void tx_apply_discard(void)
{
	size_t mark = tx_discard_mark.load();
	if (tx_read >= mark)
		return;

	size_t skip = mark - tx_read;
	size_t avail = tx_audio_rb.read_space();
	if (skip > avail)
		skip = avail;
	if (skip) {
		tx_audio_rb.read_advance(skip);
		tx_read += skip;
		LOG_DEBUG("dropped %zu stale TX samples", skip);
	}
}

// Writer side. Mark everything queued so far as stale.
static void tx_request_discard(void)
{
	tx_discard_mark.store(tx_written.load());
}

// Drop everything currently queued for RX. Returns how much was dropped.
//
// No handshake needed here, unlike the TX side: the caller (SoundTCI::flush()
// on trx_thread) IS rx_audio_rb's reader, so read_advance() moves its own
// index and carries the barrier that makes the drop visible to the receiver
// thread. The roles are simply reversed between the two rings -- on TX the
// discarding thread is the writer, which is why that side needs a mark.
size_t tci_rx_audio_discard(void)
{
	size_t n = rx_audio_rb.read_space();
	if (n)
		rx_audio_rb.read_advance(n);
	return n;
}

// Cap the RX backlog so a transient consumer stall cannot become permanent
// latency.
//
// handle_binary() deliberately drops the NEWEST samples when rx_audio_rb is
// full (the writer may not touch the reader's index, so it can only decline
// to write). But the consumer runs at exactly real time, so the queue depth
// never shrinks on its own: once any stall -- a waterfall repaint, disk I/O,
// a CPU spike -- fills the ring, every sample thereafter is decoded ~1.4 s
// (the full ring) after it arrived, indefinitely. The only existing reset is
// flush(O_RDONLY) on a TX->RX transition, which an RX-only monitoring
// session never performs.
//
// So the READER bounds its own backlog: on each read, if more than
// RX_BACKLOG_CAP samples are queued, skip forward to RX_BACKLOG_TRIM. Same
// legality as tci_rx_audio_discard() above -- read_advance() from the
// consumer's thread moves the reader's own index. The cap/trim gap is
// hysteresis: trimming to just under the cap would re-trip on every frame
// while near-full, turning one re-sync into a steady dribble of small drops.
//
// 0.5 s / 0.25 s at 48 kHz mono: normal operating depth is well under 0.2 s
// (the modem paces ~100 ms pulls against ~21 ms frames), so the cap is not
// reachable in healthy operation; a genuine stall costs one audible skip and
// then RX is realigned to ~0.25 s behind live.
static const size_t RX_BACKLOG_CAP  = 24000; // 0.5 s @ 48 kHz mono
static const size_t RX_BACKLOG_TRIM = 12000; // post-trim depth: 0.25 s

static void rx_trim_backlog(void)
{
	size_t backlog = rx_audio_rb.read_space();
	if (backlog <= RX_BACKLOG_CAP)
		return;

	size_t drop = backlog - RX_BACKLOG_TRIM;
	rx_audio_rb.read_advance(drop);
	LOG_INFO("RX backlog %zu samples exceeded cap %zu: dropped %zu oldest to re-sync",
		backlog, RX_BACKLOG_CAP, drop);
}

// Bounded (~2 s) wait until the TX ring has drained down to at most `target`
// samples, or the receiver thread that drains it stops. Runs on trx_thread and
// takes only tx_wake_mutex -- never run_mutex -- so it cannot deadlock against
// tci_close(); see receiver_active. Re-checks receiver_active each pass rather
// than blocking on it: only the receiver thread drains this ring, and only
// while TX_CHRONO frames arrive, so once it stops nothing will ever drain the
// ring and waiting longer is pointless. The 100 ms cadence bounds how long a
// disconnect goes unnoticed.
static void tx_wait_drained_to(size_t target)
{
	int waited = 0;
	guard_lock L(&tx_wake_mutex);
	while (tx_audio_rb.read_space() > target && waited < 2000) {
		if (!receiver_active.load())
			break;
		pthread_cond_timedwait_rel(&tx_wake_cond, &tx_wake_mutex, 0.1);
		waited += 100;
	}
}

size_t tci_tx_audio_write(const float *buf, size_t count)
{
	// Real-time back-pressure so the modem's tx_process() loop is paced to
	// AetherSDR's TX_CHRONO pull rate -- the throttle a blocking soundcard
	// Write() used to provide. Without it the trx loop free-runs at CPU
	// speed during TX/Tune: the waterfall races ~10x and on-air TX timing
	// is wrong. Keep only a small real-time lead queued (low latency, but
	// enough headroom over one ~21 ms TX_CHRONO block to avoid underruns).
	const size_t lead = TCI_AUDIO_SAMPLE_RATE / 10; // ~100 ms queued
	tx_wait_drained_to(lead);

	size_t n = tx_audio_rb.write(buf, count);
	tx_written.fetch_add(n);
	return n;
}

// AetherSDR's TX_CHRONO advertises hdr.channels=2, hdr.length=2048 (i.e.
// 1024 stereo frames) and expects a matching TX_AUDIO reply. WSJT-X's own
// TCI modulator writes duplicated L=R stereo pairs (a well-known quirk the
// server explicitly detects and handles), so mono content is sent the same
// way here rather than as true mono -- true mono risks a false-positive
// match against the server's *other* heuristic (treating near-identical
// adjacent samples as accidental stereo pairs), which narrowband digital-
// mode audio can easily trigger. Sending genuine duplicated pairs sidesteps
// that ambiguity entirely by matching the one path known to work.
// Ceiling on what one TX_CHRONO can make us allocate and send. AetherSDR asks
// for 1024 frames (~21 ms); this leaves 16x headroom for a peer with a larger
// block size while keeping the reply bounded at ~128 KB.
static const size_t TCI_MAX_TX_FRAMES = 16384;

static void handle_tx_chrono(const TciAudioHeader& chrono)
{
	static unsigned sent = 0, underruns = 0, oversize = 0;

	if (!ws) return;

	// fldigi subscribes TX audio for a single TRX (audio_start:0). A pull for
	// any other receiver must not be answered with RX0's TX audio, which would
	// radiate this station's transmission on the wrong slice.
	if (chrono.receiver != 0) return;

	size_t channels_req = chrono.channels ? chrono.channels : 2;
	size_t total_req = chrono.length ? chrono.length : 2048;
	size_t frames = total_req / channels_req;
	if (frames == 0) frames = 1024;

	// chrono.length is a peer-supplied uint32 and was used unvalidated: a
	// garbage or hostile value (length=0xFFFFFFFF, channels=1) asks for 4e9
	// frames, so mono.resize() and the reply vector below attempt 16 GB and
	// 32 GB. std::bad_alloc would then unwind out of tci_loop(), which has no
	// handler and is a pthread entry point -- std::terminate(), i.e. fldigi
	// aborts mid-QSO. A merely buggy server does this as easily as a hostile
	// one. Drop the block rather than clamp: a reply of the wrong length
	// desyncs the stream anyway, and no sane peer asks for this.
	if (frames > TCI_MAX_TX_FRAMES) {
		if (++oversize <= 5 || oversize % 200 == 0)
			LOG_ERROR("TX_CHRONO #%u asks %zu frames (length=%u channels=%u), max %zu -- dropped",
				oversize, frames, chrono.length, chrono.channels, TCI_MAX_TX_FRAMES);
		return;
	}

	// Drop anything the writer marked stale before serving this block, so a
	// previous over's undrained tail is never radiated ahead of this one.
	tx_apply_discard();

	static std::vector<float> mono;
	mono.resize(frames);
	size_t got = tx_audio_rb.read(mono.data(), frames);
	tx_read += got;

	// Space freed: wake tci_tx_audio_write() if it is applying back-pressure.
	{
		guard_lock L(&tx_wake_mutex);
		pthread_cond_signal(&tx_wake_cond);
	}

	if (got < frames) {
		std::fill(mono.begin() + got, mono.end(), 0.0f); // underrun -> pad silence
		if (++underruns <= 5 || underruns % 200 == 0)
			LOG_DEBUG("TX_CHRONO underrun #%u: wanted %zu frames, had %zu", underruns, frames, got);
	}

	TciAudioHeader out;
	memset(&out, 0, sizeof(out));
	out.receiver = chrono.receiver;
	out.sampleRate = TCI_AUDIO_SAMPLE_RATE;
	out.format = 3; // float32
	out.length = (uint32_t)(frames * 2); // total interleaved stereo samples
	out.type = 2; // TX_AUDIO
	out.channels = 2;

	// Reused across calls, like the mono buffer above. A fresh vector here
	// would malloc and zero-fill ~8 KB per ~21 ms TX_CHRONO on the receiver
	// thread -- the thread that owes the reply promptly -- and the memcpy and
	// interleave loop below overwrite every byte of it immediately, so the
	// zero-fill is pure waste. resize() keeps the capacity, so steady state
	// is allocation-free. Safe as a static: handle_tx_chrono() is only ever
	// reached from tci_loop() via handle_binary().
	static std::vector<uint8_t> frame;
	frame.resize(sizeof(out) + frames * 2 * sizeof(float));
	memcpy(frame.data(), &out, sizeof(out));
	float *dst = reinterpret_cast<float*>(frame.data() + sizeof(out));
	for (size_t i = 0; i < frames; i++) {
		dst[2*i]   = mono[i];
		dst[2*i+1] = mono[i];
	}

	ws->sendBinary(frame);

	if (++sent <= 5 || sent % 200 == 0)
		LOG_INFO("TX_AUDIO frame #%u: frames=%zu rb_read_space_left=%zu", sent, frames, tx_audio_rb.read_space());
}

// Downmix `frames` of interleaved `channels`-channel samples to mono float,
// applying `scale` (1.0f for float32 input, 1/32768 for int16). One body for
// both sample types and both channel counts -- the RX decode carried four
// near-identical copies of this before.
template <typename T>
static void tci_downmix(const T *in, float *out, size_t frames, size_t channels, float scale)
{
	if (channels == 2)
		for (size_t i = 0; i < frames; i++) out[i] = 0.5f * (in[2*i] + in[2*i+1]) * scale;
	else
		for (size_t i = 0; i < frames; i++) out[i] = in[i] * scale;
}

static void handle_binary(const std::vector<uint8_t>& msg)
{
	static unsigned accepted = 0, rejected = 0;

	if (msg.size() < sizeof(TciAudioHeader)) {
		if (++rejected <= 5 || rejected % 200 == 0)
			LOG_ERROR("binary msg too small: %zu bytes (need %zu)", msg.size(), sizeof(TciAudioHeader));
		return;
	}

	TciAudioHeader hdr;
	memcpy(&hdr, msg.data(), sizeof(hdr));

	if (hdr.type == 3) { // TX_CHRONO: server wants a TX_AUDIO reply now
		handle_tx_chrono(hdr);
		return;
	}

	if (hdr.type != 1) { // only RX_AUDIO handled below; ignore IQ/TX_AUDIO
		if (++rejected <= 5 || rejected % 200 == 0)
			LOG_DEBUG("binary frame type=%u ignored (not RX_AUDIO)", hdr.type);
		return;
	}

	size_t bytes_per_sample;
	if (hdr.format == 3)      bytes_per_sample = sizeof(float);
	else if (hdr.format == 0) bytes_per_sample = sizeof(int16_t);
	else return; // int24/int32 not sent for RX_AUDIO by any known TCI peer

	// The stream is requested at TCI_AUDIO_SAMPLE_RATE (audio_samplerate:48000)
	// and SoundTCI::Read() resamples on that fixed assumption, so a peer that
	// honored the request differently -- or a peer defaulting to 96/192 kHz --
	// would be decoded at the wrong pitch and speed with no error surfaced.
	// Reject rather than silently mis-decode. (0 = rate unspecified; accept.)
	if (hdr.sampleRate && hdr.sampleRate != TCI_AUDIO_SAMPLE_RATE) {
		if (++rejected <= 5 || rejected % 200 == 0)
			LOG_ERROR("RX_AUDIO sample rate %u != %d -- dropped",
				hdr.sampleRate, TCI_AUDIO_SAMPLE_RATE);
		return;
	}

	size_t channels = hdr.channels ? hdr.channels : 1;
	// Only mono and stereo are meaningful for an audio stream; a larger count
	// would be read as consecutive mono samples (interleaved -> garbage), so
	// reject it the same way an unknown format is rejected above.
	if (channels != 1 && channels != 2) {
		if (++rejected <= 5 || rejected % 200 == 0)
			LOG_ERROR("RX_AUDIO channels=%zu unsupported -- dropped", channels);
		return;
	}
	const uint8_t *payload = msg.data() + sizeof(hdr);
	size_t payload_bytes = msg.size() - sizeof(hdr);

	// hdr.length is trusted per spec, but clamp to what actually arrived
	// rather than read past the buffer if a peer under-sends.
	//
	// The comparison must not multiply: hdr.length is a uint32, so on a 32-bit
	// build (win32 ships one) "nsamples * bytes_per_sample" wraps in a 32-bit
	// size_t -- length=0x40000000 with float32 gives 0x100000000, truncated to
	// 0, so "0 > payload_bytes" is false and the clamp this comment promises
	// never runs. nsamples then stays enormous and the loops below read
	// gigabytes past the payload. Dividing the bound instead cannot overflow.
	size_t max_samples = payload_bytes / bytes_per_sample;
	size_t nsamples = hdr.length;
	if (nsamples > max_samples)
		nsamples = max_samples;
	size_t frames = nsamples / channels;
	if (frames == 0)
		return;

	static std::vector<float> mono;
	mono.resize(frames);

	if (hdr.format == 3)
		tci_downmix(reinterpret_cast<const float*>(payload), mono.data(), frames, channels, 1.0f);
	else
		tci_downmix(reinterpret_cast<const int16_t*>(payload), mono.data(), frames, channels, 1.0f / 32768.0f);

	if (++accepted <= 5 || accepted % 200 == 0)
		LOG_INFO("RX_AUDIO frame #%u: format=%u channels=%zu frames=%zu rb_write_space=%zu",
			accepted, hdr.format, channels, frames, rx_audio_rb.write_space());

	rx_audio_rb.write(mono.data(), frames); // drops overflow if consumer is slow/absent

	// Data available: wake src_read_cb if it is waiting on an empty ring.
	{
		guard_lock L(&rx_wake_mutex);
		pthread_cond_signal(&rx_wake_cond);
	}
}

// TCI text protocol.
//
// A frame is one or more semicolon-terminated commands, each "cmd:arg,arg,...;"
// with no whitespace anywhere. A frame may carry several -- the server batches
// replies -- so "modulation:0,usb;drive:100;" is one message containing two
// commands.
//
// This is parsed by splitting rather than searching. The previous version ran
// an if/else-if chain of rx.find("CMD:") over the whole frame and sscanf'd the
// remainder into a fixed char[50], which was wrong in four separate ways:
//
//   - find() is unanchored, so a command matched anywhere in the frame.
//     "TUNE_DRIVE:20;" contains "DRIVE:" at offset 5, and DRIVE is tested
//     first, so the tune drive level was parsed as the TX drive level and the
//     TUNE_DRIVE branch was unreachable.
//   - Only the first matching command in a batched frame was handled at all;
//     the rest were silently dropped by the else-if chain.
//   - "%s" has no whitespace to stop at, so it swallowed the rest of the frame
//     into a 50-byte stack buffer -- a remote stack smash, later bounded with
//     "%49s" but still leaving the token as "USB;VFO:0,0,7032050;DRIVE:100",
//     which no consumer could match. Trimming the trailing ';' did not help,
//     because the ';' was not at the end.
//   - Values were sscanf'd into uninitialized scratch variables shared by
//     every branch.
//
// Splitting on ';' and matching the command exactly removes all four: there is
// no buffer to overflow, no chain order to get wrong, every command in a frame
// is seen, and an argument cannot contain a ';' or a ',' by construction.

// Split "a,b,c" into its comma-separated fields. An empty input yields one
// empty field, which the arg_* accessors below reject.
static std::vector<std::string> tci_args(const std::string& s)
{
	std::vector<std::string> v;
	size_t p = 0;
	for (;;) {
		size_t c = s.find(',', p);
		if (c == std::string::npos) { v.push_back(s.substr(p)); return v; }
		v.push_back(s.substr(p, c - p));
		p = c + 1;
	}
}

// Strict accessors: the whole field must parse, or the command is ignored.
// Nothing here is allowed to leave an indeterminate value behind for a caller
// to store into tci_vals and push at the UI.
static bool arg_int(const std::vector<std::string>& a, size_t i, int& out)
{
	if (i >= a.size() || a[i].empty()) return false;
	char *end = 0;
	errno = 0;
	long v = strtol(a[i].c_str(), &end, 10);
	if (*end || errno || v < INT_MIN || v > INT_MAX) return false;
	out = (int)v;
	return true;
}

// fldigi tracks a single TRX/receiver (RX0) via TCI; the rxnbr/slice index
// carried by the protocol is parsed but ignored (accept updates regardless
// of which receiver/slice they were addressed to), unlike flrig which
// tracks slice_0/slice_1 independently.
static void handle_command(const std::string& cmd, const std::vector<std::string>& a)
{
	int ival = 0;

	// TCI addresses every report as cmd:<receiver>,<channel>,...  The receiver
	// (a[0]) is the slice/radio index; the channel (a[1]) is VFO A/B within it.
	// fldigi tracks RX0's VFO A only, so BOTH must be 0 -- a[0] filters out the
	// second receiver on a dual-slice rig, a[1] filters out its VFO B. Checking
	// only the channel (the earlier bug) let another slice's VFO A overwrite
	// RX0's displayed frequency/mode/S-meter.
	if (cmd == "RX_SMETER") {              // rx_smeter:<rx>,<chan>,-73;
		int rx = 0, chan = 0;
		if (!arg_int(a, 0, rx) || rx != 0) return;
		if (!arg_int(a, 1, chan) || chan != 0) return;
		if (!arg_int(a, 2, ival)) return;
		{
			guard_lock lock(&tci_vals_mutex);
			tci_vals.A.smeter = ival;
		}
		tci_on_smeter_update();
	}
	else if (cmd == "VFO") {               // vfo:<rx>,<chan>,7032050;
		int rx = 0, chan = 0;
		if (!arg_int(a, 0, rx) || rx != 0) return;
		if (!arg_int(a, 1, chan) || chan != 0) return;
		if (!arg_int(a, 2, ival)) return;
		// A negative frequency would be sign-extended to ~1.8e19 Hz by
		// tci_do_freq_update()'s cast and handed to show_frequency().
		if (ival < 0) return;
		{
			guard_lock lock(&tci_vals_mutex);
			tci_vals.A.freq = ival;
		}
		tci_on_freq_update();
	}
	else if (cmd == "MODULATION") {        // modulation:<rx>,cw;
		int rx = 0;
		if (!arg_int(a, 0, rx) || rx != 0) return;   // RX0 only (no channel field)
		if (a.size() < 2 || a[1].empty()) return;
		{
			guard_lock lock(&tci_vals_mutex);
			tci_vals.A.mod = a[1];
		}
		tci_on_mode_update();
	}
	// Inbound TRX (radio PTT) is deliberately NOT handled. AetherSDR broadcasts
	// it for its own front-panel Tune/TX and for other clients, not only for
	// transmits fldigi initiated -- and fldigi is the authoritative initiator
	// of its own digital-mode TX. Driving fldigi's T/R from a radio PTT report
	// created a feedback loop: a panel Tune keyed fldigi, fldigi echoed
	// TRX:0,true,tci back (tci_set_ptt), the radio stayed keyed and re-reported
	// TX, latching fldigi in transmit. The one-way constraint is enforced here
	// by there being no hook to fldigi's TX state, not by a comment asking for
	// restraint. A radio-PTT *status indicator* could be added, but only if it
	// cannot re-enter trx.
	//
	// Everything else the server volunteers (DDS, VOLUME, DRIVE, SQL_*, TUNE,
	// SPLIT, TX_POWER, TX_SWR, RX_FILTER_BAND, TUNE_DRIVE, and the second
	// receiver's reports) has no consumer in fldigi and is ignored. fldigi
	// tracks a single receiver (RX0); reports addressed to other slices or to
	// VFO B are dropped by the receiver/channel guards above.
}

void handle_message(const std::string & message)
{
	std::string rx = ucasestr(message);

	LOG_DEBUG("R: %s", rx.c_str());

	// Every ';'-terminated command in the frame, not just the first.
	size_t p = 0;
	while (p < rx.length()) {
		size_t end = rx.find(';', p);
		std::string seg = (end == std::string::npos) ? rx.substr(p)
													 : rx.substr(p, end - p);
		size_t colon = seg.find(':');
		if (colon != std::string::npos && colon > 0)
			handle_command(seg.substr(0, colon), tci_args(seg.substr(colon + 1)));

		if (end == std::string::npos) break;
		p = end + 1;
	}
}

// Written by tci_close() on the main thread after the receiver thread is
// already running, and read by tci_loop()'s condition -- there is no lock or
// pthread_create/join edge ordering those two, so a plain bool is a data race
// (ThreadSanitizer flags it) and the compiler is free to hoist the load out of
// the loop. Atomic rather than mutex-guarded because the receiver thread must
// never take run_mutex; see tci_queue().
static std::atomic<bool> tci_run(true);
static std::string  send_txt = "";

static pthread_mutex_t send_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t run_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t *receiver = (pthread_t *)0;

static std::list<std::string> *send_list = (std::list<std::string> *)0;

// Queue a command taking send_mutex only.
//
// The receiver thread must NEVER acquire run_mutex: tci_close() holds it
// across pthread_join(), so any receiver-side acquisition deadlocks the two
// threads against each other -- the closer waits for a join that cannot
// happen while the receiver waits for a lock that is not coming back. That
// invariant is why tci_loop() calls this rather than tci_send().
//
// Callers on other threads go through tci_send(), which wraps this in the
// run_mutex guard that makes the send_list null-check safe against
// tci_close() deleting the list.
static void tci_queue(const std::string& txt)
{
	guard_lock S(&send_mutex);
	if (!send_list) return;
	send_list->push_back(txt);
	if (txt.find("rx_smeter") == std::string::npos)
		LOG_DEBUG("PUSH: %s", txt.c_str());
}

static unsigned long mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long)ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL;
}

void *tci_loop(void *)
{
	// ws->poll(POLL_MS) blocks in select() until the socket is readable (or a
	// frame is ready to send), up to POLL_MS -- event-driven, unlike the old
	// poll(0) + MilliSleep(5), which spun at 200 Hz and added up to 5 ms of
	// latency to every inbound frame including each TX_CHRONO reply.
	const int POLL_MS = 5;

	// TCI servers report S-meter only on request (confirmed from flrig's
	// tcisdr.cxx RIG_TCI_SDR::get_smeter(), which polls via "rx_smeter:0,0;"
	// rather than receiving it unsolicited). Cadence is now wall-clock, not an
	// iteration count: with an event-driven poll the loop period varies with
	// traffic, so counting iterations would poll erratically.
	const unsigned long SMETER_MS = 500;
	unsigned long next_smeter = mono_ms();

	// Thread-boundary backstop. This is a pthread entry point, so any C++
	// exception that escapes it calls std::terminate() and aborts fldigi. The
	// individual allocation sites are bounded (WS_MAX_FRAME, TCI_MAX_TX_FRAMES,
	// WS_MAX_TXBUF), but an escaping bad_alloc/length_error must degrade to a
	// dropped connection, not a dead process -- fldigi stays up, and the audio
	// layer falls back to File I/O the same as any other TCI disconnect.
	try {

	while (tci_run && tci_running()) {
		unsigned long now = mono_ms();
		if (now >= next_smeter) {
			next_smeter = now + SMETER_MS;
			tci_queue("rx_smeter:0,0;");
		}
		{
			// The list must be examined under the lock: an unguarded
			// empty() check races push_back() from tci_send(). ws->send()
			// only appends to txbuf (poll() does the socket write), so there
			// is nothing here to pace -- the old MilliSleep(1) per message
			// just held send_mutex longer and delayed the poll().
			guard_lock S(&send_mutex);
			while (send_list && !send_list->empty()) {
				send_txt = send_list->front();
				send_list->pop_front();
				if (send_txt.find("rx_smeter") == std::string::npos)
					LOG_DEBUG("SEND: %s", send_txt.c_str());
				ws->send(send_txt);
			}
		}
		ws->poll(POLL_MS);
		ws->dispatchCombined(handle_message, handle_binary);
	}

	} catch (const std::exception& e) {
		LOG_ERROR("TCI receiver thread exiting on exception: %s", e.what());
	} catch (...) {
		LOG_ERROR("%s", "TCI receiver thread exiting on unknown exception");
	}

	// The drainer is gone: release any trx_thread TX wait immediately rather
	// than let it burn the full 2 s bound. Covers a server-side socket drop,
	// where the loop above exits on its own before any tci_close().
	receiver_active.store(false);
	{
		guard_lock L(&tx_wake_mutex);
		pthread_cond_signal(&tx_wake_cond);
	}
	return NULL;
}

// Bumped by tci_open() on the main thread, read by SoundTCI::Read() on
// trx_thread once per audio block to notice a reconnect and re-subscribe.
// Nothing synchronizes those two threads, so a plain unsigned is a race whose
// practical cost is a missed generation bump: the audio device stays
// subscribed to a dead socket and RX audio silently never returns. Atomic
// keeps the read lock-free, which matters on the per-block audio path.
static std::atomic<unsigned> connection_generation(0);

unsigned tci_connection_generation(void)
{
	return connection_generation;
}

void tci_open(std::string address, std::string port)
{
	std::string url;
	url.assign("ws://").append(address).append(":").append(port);

	// Must be called before run_mutex is taken below: tci_close() acquires
	// it itself, and guard_lock is not recursive.
	if (ws) tci_close();

	// Connect into a local, then publish only on success. Assigning straight
	// into the global would leave it dangling on the failure path, where the
	// socket is deleted but the pointer would survive for tci_running() to
	// dereference and tci_close() to free a second time.
	WebSocket::pointer sock = WebSocket::from_url(url);

	if (!sock || sock->getReadyState() == WebSocket::CLOSED) {
		delete sock;
		return;
	}

	// send_list, tci_run, receiver and ws are all reachable from tci_send()
	// on trx_thread and from tci_close() on the main thread; publishing them
	// unlocked races a concurrent tci_send() into the list.
	guard_lock R(&run_mutex);

	ws = sock;

	// Stale audio from a previous connection is NOT dropped here. This runs on
	// the main thread, but a CAT reconnect can happen while the audio device
	// stays open (see SoundTCI::Read()), so trx_thread may be concurrently
	// reading rx_audio_rb / writing tx_audio_rb -- and ringbuffer<T> forbids
	// anyone but the reader touching ridx or the writer touching widx, which
	// reset() violates (it writes both, barrier-free). Each side drops its own
	// residue from the thread that owns it instead: SoundTCI::Read() discards
	// the stale RX (it is the RX reader) on the connection_generation bump
	// below, and the writer-side discard mark (tci_tx_audio_drain at PTT-down)
	// retires any stale TX. The sample counters are monotonic and intentionally
	// never reset -- the discard mark is a point in that stream.
	{
		guard_lock S(&send_mutex);
		if (!send_list)
			send_list = new std::list<std::string>;
		send_list->clear();
	}

	tci_run = true;
	++connection_generation;

	receiver = new pthread_t;
	// pthread_create returns a positive errno on failure and 0 on success --
	// never a negative value, so a "< 0" test never fires and leaves an
	// uninitialized pthread_t for tci_close() to join.
	if (pthread_create(receiver, NULL, tci_loop, NULL) != 0) {
		LOG_ERROR("%s", "tci pthread_create failed");
		delete receiver;
		receiver = (pthread_t *)0;
		delete ws;
		ws = (WebSocket::pointer)0;
	}
	else {
		receiver_active.store(true);
	}
}

void tci_close()
{
	// Stop the TX waits (tx_wait_drained_to) before taking run_mutex: they run
	// on trx_thread and must not still be waiting on the receiver we are about
	// to join. Lock-free, so setting it here (outside run_mutex) is safe.
	receiver_active.store(false);

	guard_lock R(&run_mutex);

	if (ws) {
		tci_run = false;

		// Safe to join while holding run_mutex only because the receiver
		// thread never asks for it -- see tci_queue(). send_list is deleted
		// below rather than here for the same reason: the join must retire
		// the only other user of the list first.
		pthread_join(*receiver, NULL);
		delete receiver;
		receiver = (pthread_t *)0;

		delete ws;
		ws = (WebSocket::pointer)0;
	}

	guard_lock S(&send_mutex);
	delete send_list;
	send_list = (std::list<std::string> *)0;
}

void tci_send(std::string txt)
{
	guard_lock R(&run_mutex);
	tci_queue(txt);
}

// Unlocked, for the receiver thread's own loop condition: tci_loop() must
// never take run_mutex (see tci_queue()), and it needs no lock anyway --
// pthread_create()/pthread_join() bracket the whole lifetime of the ws it
// reads, so tci_open() cannot be publishing and tci_close() cannot be
// deleting while that thread is alive.
bool tci_running()
{
	if (!ws) return false;
	return (ws->getReadyState() != WebSocket::CLOSED);
}

// Same question, asked from a thread that has no such guarantee. trx_thread
// reaches SoundTCI::Open()/flush() while the main thread may be inside
// tci_close() deleting ws, so the read must be under the lock or it can land
// on freed memory. Safe from any thread EXCEPT the receiver thread, which
// would deadlock against tci_close()'s join.
bool tci_connected(void)
{
	guard_lock R(&run_mutex);
	return ws && (ws->getReadyState() != WebSocket::CLOSED);
}

// Block until the server has pulled everything queued for TX, so the caller
// can drop PTT without truncating the tail of the transmission.
//
// tci_tx_audio_write() deliberately keeps a ~100 ms real-time lead queued, and
// only the server's TX_CHRONO pulls drain it -- so without this the radio
// unkeys with the last symbols and the trailing RSID still sitting in the
// ring. Bounded (~2 s) on the same reasoning as the write side: a stalled or
// disconnected server must not wedge trx_thread.
//
// Any residue left after a timeout is discarded rather than kept, because the
// ring is FIFO: carried over, it would be radiated ahead of the *next*
// transmission's preamble.
bool tci_tx_audio_drain(void)
{
	tx_wait_drained_to(0);

	size_t left = tx_audio_rb.read_space();
	if (left) {
		// Mark the residue stale rather than reset()ing the ring: this runs
		// on trx_thread, the WRITER, and only the receiver thread may move
		// ridx. The reader drops it on its next TX_CHRONO, and the mark
		// guarantees the next over's audio -- written after it -- survives
		// even if the reader does not run again until then.
		tx_request_discard();
		LOG_ERROR("TX drain incomplete: %zu samples still queued (server not pulling), marked stale", left);
		return false;
	}
	return true;
}

void tci_audio_start(int trx)
{
	char cmd[64];
	tci_send("audio_samplerate:48000;");
	tci_send("audio_stream_channels:1;");
	tci_send("audio_stream_sample_type:float32;");
	snprintf(cmd, sizeof(cmd), "audio_start:%d;", trx);
	tci_send(cmd);
}

void tci_audio_stop(int trx)
{
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "audio_stop:%d;", trx);
	tci_send(cmd);
}

size_t tci_rx_audio_read(float *buf, size_t count)
{
	rx_trim_backlog();
	return rx_audio_rb.read(buf, count);
}

// Read, blocking up to timeout_ms for the receiver thread to deliver a frame
// if the ring is momentarily empty. Replaces src_read_cb's 20 x 5 ms poll.
// The predicate (ring non-empty) is checked under rx_wake_mutex before waiting
// and handle_binary() signals under the same mutex, so a frame that arrives in
// the check/wait window is never missed.
size_t tci_rx_audio_read_wait(float *buf, size_t count, int timeout_ms)
{
	rx_trim_backlog();
	size_t n = rx_audio_rb.read(buf, count);
	if (n) return n;

	guard_lock L(&rx_wake_mutex);
	n = rx_audio_rb.read(buf, count);   // re-check under the lock
	if (n) return n;
	pthread_cond_timedwait_rel(&rx_wake_cond, &rx_wake_mutex, timeout_ms / 1000.0);
	return rx_audio_rb.read(buf, count);
}
