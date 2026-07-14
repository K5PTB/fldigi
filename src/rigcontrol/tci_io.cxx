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

#include <list>
#include <string>
#include <vector>
#include <cstring>

#include <assert.h>
#include <stdio.h>
#include <ctype.h>

#include "ringbuffer.h"
#include "threads.h"
#include "misc.h"
#include "debug.h"

LOG_FILE_SOURCE(debug::LOG_RIGCONTROL);

TCI_VALS tci_vals;
pthread_mutex_t tci_vals_mutex = PTHREAD_MUTEX_INITIALIZER;

using WSclient::WebSocket;

// Single-writer (this file's receiver thread, via handle_binary()) /
// single-reader (SoundTCI::Read(), called from trx_thread) ring buffer of
// decoded mono float samples at TCI_AUDIO_SAMPLE_RATE. ~1.4s of audio at
// 48kHz -- generous enough to absorb jitter between TCI's ~2048-sample
// frame cadence and the modem's much smaller SCBLOCKSIZE reads.
static ringbuffer<float> rx_audio_rb(65536);

static_assert(sizeof(TciAudioHeader) == 64, "TCI audio header must be 64 bytes");

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

	if (hdr.type != 1) { // only RX_AUDIO; ignore IQ/TX_AUDIO/TX_CHRONO
		if (++rejected <= 5 || rejected % 200 == 0)
			LOG_DEBUG("binary frame type=%u ignored (not RX_AUDIO)", hdr.type);
		return;
	}

	size_t bytes_per_sample;
	if (hdr.format == 3)      bytes_per_sample = sizeof(float);
	else if (hdr.format == 0) bytes_per_sample = sizeof(int16_t);
	else return; // int24/int32 not sent for RX_AUDIO by any known TCI peer

	size_t channels = hdr.channels ? hdr.channels : 1;
	const uint8_t *payload = msg.data() + sizeof(hdr);
	size_t payload_bytes = msg.size() - sizeof(hdr);

	// hdr.length is trusted per spec, but clamp to what actually arrived
	// rather than read past the buffer if a peer under-sends.
	size_t nsamples = hdr.length;
	if (nsamples * bytes_per_sample > payload_bytes)
		nsamples = payload_bytes / bytes_per_sample;
	size_t frames = nsamples / channels;
	if (frames == 0)
		return;

	static std::vector<float> mono;
	mono.resize(frames);

	if (hdr.format == 3) {
		const float *f = reinterpret_cast<const float*>(payload);
		if (channels == 2)
			for (size_t i = 0; i < frames; i++) mono[i] = 0.5f * (f[2*i] + f[2*i+1]);
		else
			for (size_t i = 0; i < frames; i++) mono[i] = f[i];
	} else {
		const int16_t *s = reinterpret_cast<const int16_t*>(payload);
		const float scale = 1.0f / 32768.0f;
		if (channels == 2)
			for (size_t i = 0; i < frames; i++) mono[i] = 0.5f * (s[2*i] + s[2*i+1]) * scale;
		else
			for (size_t i = 0; i < frames; i++) mono[i] = s[i] * scale;
	}

	if (++accepted <= 5 || accepted % 200 == 0)
		LOG_INFO("RX_AUDIO frame #%u: format=%u channels=%zu frames=%zu rb_write_space=%zu",
			accepted, hdr.format, channels, frames, rx_audio_rb.write_space());

	rx_audio_rb.write(mono.data(), frames); // drops overflow if consumer is slow/absent
}

// fldigi tracks a single TRX/receiver (RX0) via TCI; the rxnbr/slice index
// carried by the protocol is parsed but ignored (accept updates regardless
// of which receiver/slice they were addressed to), unlike flrig which
// tracks slice_0/slice_1 independently.
void handle_message(const std::string & message)
{
	int rxnbr, vfo, ival;
	float fval;
	char szstr[50];
	std::string str;
	std::string rx = message;
	size_t p = 0;

	for (size_t n = 0; n < rx.length(); n++)
		rx[n] = toupper(rx[n] & 0xFF);

	LOG_DEBUG("R: %s", rx.c_str());

	if ((p = rx.find("RX_SMETER:")) != std::string::npos) { // smeter reading
		sscanf(rx.substr(p).c_str(), "RX_SMETER:%d,%d,%d;", &rxnbr, &vfo, &ival);
		{
			guard_lock lock(&tci_vals_mutex);
			if (vfo == 0) tci_vals.A.smeter = ival;
			else          tci_vals.B.smeter = ival;
		}
		tci_on_smeter_update();
	}
	else if ((p = rx.find("VFO:")) != std::string::npos) { // vfo:0,0,7032050;
		sscanf(rx.substr(p).c_str(), "VFO:%d,%d,%d", &rxnbr, &vfo, &ival);
		{
			guard_lock lock(&tci_vals_mutex);
			if (vfo == 0) tci_vals.A.freq = ival;
			else          tci_vals.B.freq = ival;
		}
		if (vfo == 0) tci_on_freq_update();
	}
	else if ((p = rx.find("DDS:")) != std::string::npos) { // dds:1,14070000;
		sscanf(rx.substr(p).c_str(), "DDS:%d,%d", &rxnbr, &ival);
		guard_lock lock(&tci_vals_mutex);
		tci_vals.dds = ival;
	}
	else if ((p = rx.find("RX_FILTER_BAND:")) != std::string::npos) { // rx_filter_band:0,-600,600;
		int slice;
		sscanf(rx.substr(p).c_str(), "RX_FILTER_BAND:%d,%s", &slice, szstr);
		guard_lock lock(&tci_vals_mutex);
		tci_vals.A.bw = tci_vals.B.bw = szstr;
	}
	else if ((p = rx.find("MODULATION:")) != std::string::npos) { // modulation:0,cw;
		int slice = 0;
		sscanf(rx.substr(p).c_str(), "MODULATION:%d,%s", &slice, szstr);
		{
			guard_lock lock(&tci_vals_mutex);
			tci_vals.A.mod = tci_vals.B.mod = szstr;
		}
		tci_on_mode_update();
	}
	else if ((p = rx.find("TRX:")) != std::string::npos) {
		sscanf(rx.substr(p).c_str(), "TRX:%d,%s", &rxnbr, szstr);
		str = szstr;
		{
			guard_lock lock(&tci_vals_mutex);
			tci_vals.ptt = (str == "TRUE;");
		}
		tci_on_ptt_update();
	}
	else if ((p = rx.find("SPLIT_ENABLE:")) != std::string::npos) { // split_enable:0,false;
		sscanf(rx.substr(p).c_str(), "SPLIT_ENABLE:%d,%s", &rxnbr, szstr);
		str = szstr;
		guard_lock lock(&tci_vals_mutex);
		tci_vals.split = (str == "TRUE;");
	}
	else if ((p = rx.find("VOLUME:")) != std::string::npos) { // volume:-16;
		sscanf(rx.substr(p).c_str(), "VOLUME:%d", &ival);
		guard_lock lock(&tci_vals_mutex);
		tci_vals.vol = ival;
	}
	else if ((p = rx.find("SQL_ENABLE:")) != std::string::npos) { // sql_enable:1,false;
		sscanf(rx.substr(p).c_str(), "SQL_ENABLE:%d,%s", &rxnbr, szstr);
		str = szstr;
		guard_lock lock(&tci_vals_mutex);
		tci_vals.sql = (str == "TRUE;");
	}
	else if ((p = rx.find("SQL_LEVEL:")) != std::string::npos) { // sql_level:0,-79;
		sscanf(rx.substr(p).c_str(), "SQL_LEVEL:%d,%d", &rxnbr, &ival);
		guard_lock lock(&tci_vals_mutex);
		tci_vals.sql_level = ival;
	}
	else if ((p = rx.find("DRIVE:")) != std::string::npos) { // drive:100;
		sscanf(rx.substr(p).c_str(), "DRIVE:%d", &ival);
		guard_lock lock(&tci_vals_mutex);
		tci_vals.pwr = ival;
	}
	else if ((p = rx.find("TUNE:")) != std::string::npos) { // tune:0,false;
		sscanf(rx.substr(p).c_str(), "TUNE:%d,%s", &rxnbr, szstr);
		str = szstr;
		guard_lock lock(&tci_vals_mutex);
		tci_vals.tune = (str == "TRUE;");
	}
	else if ((p = rx.find("TX_POWER:")) != std::string::npos) { // tx_power:4.3;
		sscanf(rx.substr(p).c_str(), "TX_POWER:%f", &fval);
		guard_lock lock(&tci_vals_mutex);
		tci_vals.tx_power = fval;
	}
	else if ((p = rx.find("TX_SWR:")) != std::string::npos) { // tx_swr:1.3;
		sscanf(rx.substr(p).c_str(), "TX_SWR:%f", &fval);
		guard_lock lock(&tci_vals_mutex);
		tci_vals.tx_swr = fval;
	}
}

static WebSocket::pointer ws = (WebSocket::pointer)0;

static bool tci_run = true;
static std::string  send_txt = "";

static pthread_mutex_t send_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t run_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t *receiver = (pthread_t *)0;

static std::list<std::string> *send_list = (std::list<std::string> *)0;

void *tci_loop(void *)
{
	// TCI servers report S-meter only on request (confirmed from flrig's
	// tcisdr.cxx RIG_TCI_SDR::get_smeter(), which explicitly polls via
	// "rx_smeter:0,0;" rather than receiving it unsolicited). Poll roughly
	// every 500ms (100 iterations * 5ms sleep below).
	int smeter_poll = 0;

	while (tci_run && tci_running()) {
		if (++smeter_poll >= 100) {
			smeter_poll = 0;
			tci_send("rx_smeter:0,0;");
		}
		if (!send_list->empty()) {
			guard_lock S(&send_mutex);
			while (!send_list->empty()) {
				send_txt = send_list->front();
				send_list->pop_front();
				if (send_txt.find("rx_smeter") == std::string::npos)
					LOG_DEBUG("SEND: %s", send_txt.c_str());
				ws->send(send_txt);
				MilliSleep(1);
			}
		}
		ws->poll();
		ws->dispatchCombined(handle_message, handle_binary);
		MilliSleep(5);
	}
	return NULL;
}

static unsigned connection_generation = 0;

unsigned tci_connection_generation(void)
{
	return connection_generation;
}

void tci_open(std::string address, std::string port)
{
	std::string url;
	url.assign("ws://").append(address).append(":").append(port);

	if (ws) tci_close();

	ws = WebSocket::from_url(url);

	if (ws && (ws->getReadyState() != WebSocket::CLOSED)) {

		if (!send_list)
			send_list = new std::list<std::string>;
		send_list->clear();

		tci_run = true;
		++connection_generation;

		receiver = new pthread_t;
		if (pthread_create(receiver, NULL, tci_loop, NULL) < 0) {
			LOG_ERROR("%s", "tci pthread_create failed");
			delete receiver;
			receiver = (pthread_t *)0;
			delete ws;
			ws = (WebSocket::pointer)0;
		}
	} else
		delete ws;
}

void tci_close()
{
	guard_lock R(&run_mutex);

	if (ws) {
		tci_run = false;

		pthread_join(*receiver, NULL);
		delete receiver;
		receiver = (pthread_t *)0;

		delete ws;
		ws = (WebSocket::pointer)0;
	}

	delete send_list;
	send_list = (std::list<std::string> *)0;
}

void tci_send(std::string txt)
{
	guard_lock R(&run_mutex);
	if (!send_list) return;
	{
		guard_lock S(&send_mutex);
		send_list->push_back(txt);
		if (txt.find("rx_smeter") == std::string::npos)
			LOG_DEBUG("PUSH: %s", txt.c_str());
	}
}

bool tci_running()
{
	if (!ws) return false;
	return (ws->getReadyState() != WebSocket::CLOSED);
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
	return rx_audio_rb.read(buf, count);
}

size_t tci_rx_audio_available(void)
{
	return rx_audio_rb.read_space();
}
