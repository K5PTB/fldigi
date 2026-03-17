// ----------------------------------------------------------------------------
// qrss.cxx  --  VERY LONG DURATION morse code modem
//
// Copyright (C) 2006-2010
//		Dave Freese, W1HKJ
//		   (C) Mauri Niininen, AG1LE
//
// This file is part of fldigi.  Adapted from code contained in gmfsk source code
// distribution.
//  gmfsk Copyright (C) 2001, 2002, 2003
//  Tomi Manninen (oh2bns@sral.fi)
//  Copyright (C) 2004
//  Lawrence Glaister (ve7it@shaw.ca)
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

#define QRSS_DEBUG 0

#include <config.h>

#include <cstring>
#include <string>
#include <stdio.h>
#include <iostream>
#include <fstream>
#include <cstdlib>

#include "timeops.h"
#ifdef __MINGW32__
#  include "compat.h"
#endif

#if !HAVE_CLOCK_GETTIME
#  ifdef __APPLE__
#    include <mach/mach_time.h>
#  endif
#endif

#include "digiscope.h"
#include "waterfall.h"
#include "fl_digi.h"
#include "fftfilt.h"
#include "serial.h"
#include "ptt.h"
#include "main.h"

#include "qrss.h"
#include "misc.h"
#include "configuration.h"
#include "confdialog.h"
#include "status.h"
#include "debug.h"
#include "FTextRXTX.h"
#include "modem.h"

#include "qrunner.h"

#include "winkeyer.h"
#include "nanoIO.h"
#include "KYkeying.h"
#include "ICOMkeying.h"
#include "YAESUkeying.h"

#include "audio_alert.h"

#include "gpio_common.h"

#include "cw.h"

#include "hwfall_viewer.h"

QRSS_PREFS qrss_prefs = {
	false, false, true,
	10, 8,
	2.0, 523.0, 880.0, 440.0, 1.0, 0.25
};

void start_qrssio_thread();
void stop_qrssio_thread();

#if USE_LIBGPIOD
static gpio_num_t qrss_gpio_num = GPIO_COMMON_UNKNOWN;
#endif

void qrss::tx_init()
{
	qrss_phase = 0;
}

void qrss::rx_init()
{
}

void qrss::init()
{
	create_edges();

	memset(outbuf, 0, sizeof(outbuf));

	morse->init();

	rx_init();

}

qrss::~qrss() {
	delete qrss_filter;

	for (size_t n = 0; n < NUM_BINS; n++) delete binfilter[n];

	stop_qrssio_thread();

}

qrss::qrss() : modem()
{
	mode = MODE_QRSS;

	samplerate = QRSS_SAMPLERATE;

	frequency = progdefaults.CWsweetspot;

	qrss_filter = new sfft(SFFT_SIZE, FIRST_BIN, LAST_BIN);

	for (size_t n = 0; n < NUM_BINS; n++) binfilter[n] = new Cmovavg(4);

	qrss_speed = 1.2 / qrss_prefs.QRSS_DOT;
//	QRSS_DOT_length = 1000.0 * qrss_prefs.QRSS_DOT;

	symbollen = (int)round(samplerate * qrss_prefs.QRSS_DOT);  // transmit char rate

	qrss_phase = 0.0;

	init();

	put_MODEstatus("QRSS");

/*
std::cout <<
"QRSS_SAMPLERATE    : " << QRSS_SAMPLERATE  << std::endl <<
"SFFT_SIZE          : " << SFFT_SIZE  << std::endl <<
"QRSS_F0            : " << QRSS_F0  << std::endl <<
"QRSS_HZPERBIN      : " << QRSS_HZPERBIN  << std::endl <<
"QRSS_BW2           : " << QRSS_BW2  << std::endl <<
"FIRST_BIN          : " << FIRST_BIN  << std::endl <<
"LAST_BIN           : " << LAST_BIN  << std::endl <<
"NUM_BINS           : " << NUM_BINS << std::endl;
*/
}

cmplx qrss::mixer(cmplx in)
{
	static double phase = 0;
	cmplx z (cos(phase), sin(phase));
	z = z * in;

	phase += TWOPI * frequency / samplerate;
	if (phase > TWOPI) phase -= TWOPI;

	return z;
}

int qrss::rx_process(const double *buf, int len)
{
	return 0;
}

//=====================================================================
// qrss transmit routines
//=====================================================================

//---------------------------------------------------------------------
// qrss_txprocess()
// Read characters from screen and either generate a sound card signal
// at the current transmit audio frequency, or toggle a keyline signa.
// This is called repeatedly from the Rx/Tx thread during tx.
//---------------------------------------------------------------------
int qrss::tx_process()
{
	int c = get_tx_char();

	if (c == GET_TX_CHAR_NODATA) {
		Fl::awake();
		MilliSleep(50);
		return 0;
	}

	if (c == GET_TX_CHAR_ETX || stopflag) {
		stopflag = false;
		put_echo_char('\n');
		return -1;
	}

	if (qrss_prefs.CW_qrss ) {
		if (CW_KEYLINE_isopen ||
			progdefaults.CW_KEYLINE_on_cat_port ||
			progdefaults.CW_KEYLINE_on_ptt_port ||
			progdefaults.use_FLRIGkeying ) {
				send_via_keyline(c);
				MilliSleep(50);
				put_echo_char(c);
		} else
			send_tones(c);
	} else if (qrss_prefs.FM_qrss) {
		send_tones(c);
	} else if (qrss_prefs.DFCW_qrss) {
		send_dfcw(c);
	}
	if (stopflag) {
		stopflag = false;
		return -1;
	}

	return 0;
}

//----------------------------------------------------------------------
// send morse character using audio tones
//----------------------------------------------------------------------

double qrss::nco(double freq)
{
	qrss_phase += TWOPI * freq / samplerate;

	if (qrss_phase > TWOPI) qrss_phase -= TWOPI;

	return cos(qrss_phase);
}

//---------------------------------------------------------------------
// send_symbol()
// Sends a part of a morse character at the correct freq or silence.
// or sends a part of a morse character as a narrow FSK signal
// where keydown is higher in frequency than keyup.
//
// Left channel contains the QRSS waveform
//
//---------------------------------------------------------------------
#define KNUM  (QRSS_SAMPLERATE * 20 / 1000)  // 20 msec maximum sec rise/fall time
static double keyshape[KNUM];

void qrss::create_edges()
{
	int NUM = QRSS_SAMPLERATE * qrss_prefs.QRSS_ENV_MSEC / 1000;
	for (int i = 0; i < NUM; i++) {
		keyshape[i] = (0.42 - 0.50 * cos(M_PI * i/ NUM) + 0.08 * cos(2 * M_PI * i / NUM));
	}
}

void qrss::send_symbol(int bit, long len)
{
	if (qrss_prefs.CW_qrss) {
		double xmt_freq = get_txfreq_woffset();
		int n = 0;
		int send_samples = 0;
		int NUM = QRSS_SAMPLERATE * qrss_prefs.QRSS_ENV_MSEC / 1000;
		if (bit == 1) { // keydown
			n = 0;
			while (n < len) {
				memset(outbuf, 0, sizeof(outbuf));
				for (int i = 0; i < QRSS_SIZE; i++) {
					outbuf[i] = nco(xmt_freq);
					if ((n + i) < NUM) outbuf[i] *= keyshape[i];
					if (len - (n + i) < NUM) outbuf[i] *= keyshape[len - (n + i)];
				}
				if (len - n > QRSS_SIZE) send_samples = QRSS_SIZE;
				else send_samples = len - n;
				if (stopflag) return;
				ModulateXmtr(outbuf, send_samples);
				n += QRSS_SIZE;
			}

		} else { // keyup
			n = 0;
			while (n < len) {
				memset(outbuf, 0, sizeof(outbuf));
				if (len - n > QRSS_SIZE) send_samples = QRSS_SIZE;
				else send_samples = len - n;
				if (stopflag) return;
				ModulateXmtr(outbuf, send_samples);
				n += QRSS_SIZE;
			}
		}
	}
	else if (qrss_prefs.FM_qrss) {
		double xmt_freq_OFF = get_txfreq_woffset();
		double xmt_freq_ON = xmt_freq_OFF + qrss_prefs.QRSS_FM_SHIFT;

		int n = 0;
		int send_samples = 0;
		int NUM = QRSS_SAMPLERATE * qrss_prefs.QRSS_ENV_MSEC / 1000;

		n = 0;
		while (n < len) {
			memset(outbuf, 0, sizeof(outbuf));
			for (int i = 0; i < QRSS_SIZE; i++) {
				if (bit == 1)
					outbuf[i] = nco(xmt_freq_ON);
				else
					outbuf[i] = nco(xmt_freq_OFF);
				if ((n + i) < NUM) outbuf[i] *= keyshape[i];
				if (len - (n + i) < NUM) outbuf[i] *= keyshape[len - (n + i)];
			}
			if (len - n > QRSS_SIZE) send_samples = QRSS_SIZE;
			else send_samples = len - n;

			if (stopflag) return;
			ModulateXmtr(outbuf, send_samples);
			n += QRSS_SIZE;
		}
	}
	else if (qrss_prefs.DFCW_qrss) {
	}
}

//---------------------------------------------------------------------
// send_ch()
// sends a morse character and the space afterwards
//---------------------------------------------------------------------
void qrss::send_tones(int ch)
{
	std::string code;

	symbollen = (int)round(samplerate * qrss_prefs.QRSS_DOT);

	float tc = symbollen;  //qrss_prefs.QRSS_DOT; // 1200.0 / qrss_speed;
	float tch = 3 * tc, twd = 4 * tc;

	if ((ch == ' ') || (ch == '\n')) {
		send_symbol(0, twd);
		put_echo_char(ch);
		return;
	}

	code = morse->tx_lookup(ch);

	if (!code.length()) {
		return;
	}

	int elements = code.length();

	for (int n = 0; n < elements; n++) {
		send_symbol(1, (code[n] == '-' ? 3 : 1) * symbollen);
		if (stopflag) return;
		send_symbol(0, ((n < elements - 1) ? symbollen : tch));
		if (stopflag) return;
	}

	if (ch != -1) {
		std::string prtstr = morse->tx_print();
		for (size_t n = 0; n < prtstr.length(); n++)
			put_echo_char(
				prtstr[n],
				prtstr[0] == '<' ? FTextBase::CTRL : FTextBase::XMIT);
	}
}

void qrss::send_dfcw_keyup()
{
	int n = 0;
	int send_samples = 0;
	int len = qrss_prefs.QRSS_DFCW_KEYUP_LEN * QRSS_SAMPLERATE;
	int NUM = QRSS_SAMPLERATE * qrss_prefs.QRSS_ENV_MSEC / 1000;
	while (n < len) {
		memset(outbuf, 0, sizeof(outbuf));
		for (int i = 0; i < QRSS_SIZE; i++) {
			outbuf[i] = nco( qrss_prefs.QRSS_DFCW_KEYUP_SHIFT);
			if ((n + i) < NUM) outbuf[i] *= keyshape[i];
			if (len - (n + i) < NUM) outbuf[i] *= keyshape[len - (n + i)];
		}
		if (len - n > QRSS_SIZE) send_samples = QRSS_SIZE;
		else send_samples = len - n;

		if (stopflag) return;
		ModulateXmtr(outbuf, send_samples);
		n += QRSS_SIZE;
	}
}

void qrss::send_dfcw_dot()
{
	int n = 0;
	int send_samples = 0;
	int len = qrss_prefs.QRSS_DFCW_ELEMENT_LEN * QRSS_SAMPLERATE;
	int NUM = QRSS_SAMPLERATE * qrss_prefs.QRSS_ENV_MSEC / 1000;
	while (n < len) {
		memset(outbuf, 0, sizeof(outbuf));
		for (int i = 0; i < QRSS_SIZE; i++) {
			outbuf[i] = nco( qrss_prefs.QRSS_DFCW_DOT_SHIFT);
			if ((n + i) < NUM) outbuf[i] *= keyshape[i];
			if (len - (n + i) < NUM) outbuf[i] *= keyshape[len - (n + i)];
		}
		if (len - n > QRSS_SIZE) send_samples = QRSS_SIZE;
		else send_samples = len - n;

		if (stopflag) return;
		ModulateXmtr(outbuf, send_samples);
		n += QRSS_SIZE;
	}
}

void qrss::send_dfcw_dash()
{
	int n = 0;
	int send_samples = 0;
	int len = qrss_prefs.QRSS_DFCW_ELEMENT_LEN * QRSS_SAMPLERATE;
	int NUM = QRSS_SAMPLERATE * qrss_prefs.QRSS_ENV_MSEC / 1000;
	while (n < len) {
		memset(outbuf, 0, sizeof(outbuf));
		for (int i = 0; i < QRSS_SIZE; i++) {
			outbuf[i] = nco( qrss_prefs.QRSS_DFCW_DASH_SHIFT);
			if ((n + i) < NUM) outbuf[i] *= keyshape[i];
			if (len - (n + i) < NUM) outbuf[i] *= keyshape[len - (n + i)];
		}
		if (len - n > QRSS_SIZE) send_samples = QRSS_SIZE;
		else send_samples = len - n;

		if (stopflag) return;
		ModulateXmtr(outbuf, send_samples);
		n += QRSS_SIZE;
	}
}

void qrss::send_dfcw(int c)
{
	if (c == ' ') {
		send_dfcw_keyup();
		send_dfcw_keyup();
		return;
	}

	std::string code = morse->tx_lookup(c);
	if (!code.length()) {
		send_dfcw_keyup();
		return;
	}

	for (size_t n = 0; n < code.length(); n++) {
		if (code[n] == '.')
			send_dfcw_dot();
		else 
			send_dfcw_dash();
		send_dfcw_keyup();
	}
	send_dfcw_keyup();
}

// ---------------------------------------------------------------------
// QRSS output on DTR/RTS signal lines
// or xmlrpc interface to flrig as the DTR/RTS line controller
//----------------------------------------------------------------------

void QRSS_flrig_key(int down)
{
	flrig_key_state(down);
}

bool QRSS_KEYLINE_is_open = false;

void open_qrss_KEYLINE()
{
	if (open_CW_KEYLINE())
		QRSS_KEYLINE_is_open = true;;
}

void close_QRSS_KEYLINE()
{
	close_CW_KEYLINE();
	QRSS_KEYLINE_is_open = false;
}

//----------------------------------------------------------------------
#include <queue>

static pthread_t       qrssio_pthread;
static pthread_cond_t  qrssio_cond;
static pthread_mutex_t qrssio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fifo_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t qrssio_ptt_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool qrssio_thread_running   = false;
static bool qrssio_terminate_flag   = false;

//----------------------------------------------------------------------

static int qrssio_ch;
static cMorse *qrssio_morse = 0;
static std::queue<int> fifo;

//----------------------------------------------------------------------

void qrssio_key(int on)
{
	if (progdefaults.use_FLRIGkeying) {
		QRSS_flrig_key(on);
		return;
	}

	if (QRSS_KEYLINE_is_open ||
		progdefaults.CW_KEYLINE_on_cat_port ||
		progdefaults.CW_KEYLINE_on_ptt_port) {

		Cserial *ser = &CW_KEYLINE_serial;

		if (progdefaults.CW_KEYLINE_on_cat_port)
			ser = &rigio;
		else if (progdefaults.CW_KEYLINE_on_ptt_port)
			ser = &push2talk->serPort;
		switch (progdefaults.CW_KEYLINE) {
			case 0: break;
			case 1: ser->SetRTS(on); break;
			case 2: ser->SetDTR(on); break;
		}
	}
}

void qrssio_ptt(int on)
{
	if (progdefaults.use_FLRIGkeying)
		return;

	if (QRSS_KEYLINE_is_open ||
		progdefaults.CW_KEYLINE_on_cat_port ||
		progdefaults.CW_KEYLINE_on_ptt_port) {
		Cserial *ser = &CW_KEYLINE_serial;
		if (progdefaults.CW_KEYLINE_on_cat_port)
			ser = &rigio;
		else if (progdefaults.CW_KEYLINE_on_ptt_port)
			ser = &push2talk->serPort;
		switch (progdefaults.PTT_KEYLINE) {
			case 0: break;
			case 1: ser->SetRTS(on); break;
			case 2: ser->SetDTR(on); break;
		}
	}
}

#if USE_LIBGPIOD
void set_gpio_pin(int key)
{
	if (gpio_common_set(qrss_gpio_num, key) < 0) {
		LOG_ERROR("Error setting GPIO");
	}
}
#endif

// return accurate time of day in secs

double qrssio_now()
{
	static struct timespec tp;

#if HAVE_CLOCK_GETTIME
	clock_gettime(CLOCK_MONOTONIC, &tp); 
#elif defined(__WIN32__)
	DWORD msec = GetTickCount();
	return 1.0 * msec;
	tp.tv_sec = msec / 1000;
	tp.tv_nsec = (msec % 1000) * 1000000;
#elif defined(__APPLE__)
	static mach_timebase_info_data_t info = { 0, 0 };
	if (unlikely(info.denom == 0))
		mach_timebase_info(&info);
	uint64_t t = mach_absolute_time() * info.numer / info.denom;
	tp.tv_sec = t / 1000000000;
	tp.tv_nsec = t % 1000000000;
#endif
	return 1.0 * tp.tv_sec + tp.tv_nsec * 1e-9;

}

void qrssio_bit(int bit, double msecs)
{
#ifdef QRSS_TTEST
	if (!qrssio_test) qrssio_test = fopen("qrssio_test.txt", "a");
#endif
	static double secs;
	static struct timespec tv = { 0, 1000000L};
	static double end1 = 0;
	static double end2 = 0;
	static double t1 = 0;
#ifdef QRSS_TTEST
	static double t2 = 0;
#endif
	static double t3 = 0;
	static double t4 = 0;
	int loop1 = 0;
	int loop2 = 0;
	int n1 = msecs * 1e3;

	secs = msecs * 1e-3;

#ifdef __WIN32__
	timeBeginPeriod(1);
#endif

	t1 = qrssio_now();

	end2 = t1 + secs - 0.00001;

#if USE_LIBGPIOD
	if (progdefaults.gpio_cw_line != -1)
		set_gpio_pin(bit);
	else
		qrssio_key(bit);
#else
	qrssio_key(bit);
#endif

#ifdef QRSS_TTEST
	t2 = t3 = qrssio_now();
#else
	t3 = qrssio_now();
#endif
	end1 = end2 - 0.005;

	while (t3 < end1 && (++loop1 < n1)) {
		nano_sleep(&tv, NULL);
		t3 = qrssio_now();
	}

	t4 = t3;
	while (t4 <= end2) {
		loop2++;
		t4 = qrssio_now();
	}

#ifdef __WIN32__
	timeEndPeriod(1);
#endif

#ifdef QRSS_TTEST
	if (qrssio_test)
		fprintf(qrssio_test, "%d, %d, %d, %6f, %6f, %6f, %6f, %6f, %6f, %6f\n",
			bit, loop1, loop2,
			secs * 1e3,
			(t2 - t1)*1e3,
			(t3 - t1)*1e3,
			(t3 - end1) * 1e3,
			(t4 - t1)*1e3,
			(t4 - end2) * 1e3,
			(t4 - t1 - secs)*1e3);
#endif
}

bool qrssio_sending = false;
void send_qrssio(int c)
{
	if (c == GET_TX_CHAR_NODATA || c == 0x0d) {
		qrssio_sending = false;
		return;
	}

	qrssio_sending = true;

	float tc = 1000.0 * qrss_prefs.QRSS_DOT;
	if (tc <= 0) tc = 1;
	float tch = 3 * tc, twd = 4 * tc;

	if (c == 0x0a) c = ' ';

	if (c == ' ') {
		qrssio_bit(0, twd);
		qrssio_sending = false;
		return;
	}

	std::string code;
	code = qrssio_morse->tx_lookup(c);
	if (!code.length()) {
		qrssio_sending = false;
		return;
	}

	guard_lock lk(&qrssio_ptt_mutex);

	for (size_t n = 0; n < code.length(); n++) {
		if (code[n] == '.') {
			qrssio_bit(1, tc);
		} else {
			qrssio_bit(1, 3*tc);
		}
		if (n < code.length() -1) {
			qrssio_bit(0, tc);
		} else {
			qrssio_bit(0, tch);
		}
	}

	qrssio_sending = false;
}

void * qrssio_loop(void *args)
{
//	SET_THREAD_ID(QRSSIO_TID);

	qrssio_thread_running   = true;
	qrssio_terminate_flag   = false;

	while(1) {
		pthread_mutex_lock(&qrssio_mutex);
		pthread_cond_wait(&qrssio_cond, &qrssio_mutex);
		pthread_mutex_unlock(&qrssio_mutex);

		if (qrssio_terminate_flag)
			break;
		while (!fifo.empty()) {
			{
				guard_lock lk(&fifo_mutex);
				qrssio_ch = fifo.front();
				fifo.pop();
			}
			send_qrssio(qrssio_ch);
		}
	}
	return (void *)0;
}

void stop_qrssio_thread(void)
{
	if(!qrssio_thread_running) return;

	qrssio_terminate_flag = true;
	pthread_cond_signal(&qrssio_cond);

	MilliSleep(10);

	pthread_join(qrssio_pthread, NULL);

	pthread_mutex_destroy(&qrssio_mutex);
	pthread_cond_destroy(&qrssio_cond);

	memset((void *) &qrssio_pthread, 0, sizeof(qrssio_pthread));
	memset((void *) &qrssio_mutex,   0, sizeof(qrssio_mutex));

	qrssio_thread_running   = false;
	qrssio_terminate_flag   = false;

	delete qrssio_morse;
	qrssio_morse = 0;
}

void start_qrssio_thread(void)
{
	if (qrssio_thread_running) return;

	memset((void *) &qrssio_pthread, 0, sizeof(qrssio_pthread));
	memset((void *) &qrssio_mutex,   0, sizeof(qrssio_mutex));
	memset((void *) &qrssio_cond,    0, sizeof(qrssio_cond));

	if(pthread_cond_init(&qrssio_cond, NULL)) {
		LOG_ERROR("Alert thread create fail (pthread_cond_init)");
		return;
	}

	if(pthread_mutex_init(&qrssio_mutex, NULL)) {
		LOG_ERROR("AUDIO_ALERT thread create fail (pthread_mutex_init)");
		return;
	}

	if (pthread_create(&qrssio_pthread, NULL, qrssio_loop, NULL) < 0) {
		pthread_mutex_destroy(&qrssio_mutex);
		LOG_ERROR("AUDIO_ALERT thread create fail (pthread_create)");
	}

	LOG_DEBUG("started audio qrssio thread");

	MilliSleep(10); // Give the CPU time to set 'qrssio_thread_running'
}

void qrss_wait(int c)
{
//	int len = qrssio_morse->tx_length(c);
	while (qrssio_sending) {
		MilliSleep(50);
		Fl::awake();
	}
}

void send_via_keyline(int c)
{
	if (!qrssio_thread_running)
		start_qrssio_thread();

	if (qrssio_morse == 0) {
		qrssio_morse = new cMorse;
		qrssio_morse->init();
	}

	guard_lock lk(&fifo_mutex);
	fifo.push(c);

	pthread_cond_signal(&qrssio_cond);

	qrss_wait(c); // wait for send to complete
}

//----------------------------------------------------------------------
// qrss prefs
//----------------------------------------------------------------------

void save_qrss_prefs()
{
#if FLDIGI_FLTK_API_MINOR < 4
	Fl_Preferences spref(HomeDir.c_str(), "w1hkj.org", "qrss");
#else
	Fl_Preferences spref(
		HomeDir.c_str(),
		"w1hkj.org",
		"qrss",
		Fl_Preferences::C_LOCALE);
#endif

	spref.set("CW_qrss", qrss_prefs.CW_qrss);
	spref.set("FM_qrss", qrss_prefs.FM_qrss);
	spref.set("DFCW_qrss", qrss_prefs.DFCW_qrss);
	spref.set("QRSS_FM_SHIFT", qrss_prefs.QRSS_FM_SHIFT);
	spref.set("QRSS_ENV_MSEC", qrss_prefs.QRSS_ENV_MSEC);
	spref.set("QRSS_DOT", qrss_prefs.QRSS_DOT);
	spref.set("QRSS_DFCW_DOT_SHIFT", qrss_prefs.QRSS_DFCW_DOT_SHIFT);
	spref.set("QRSS_DFCW_DASH_SHIFT", qrss_prefs.QRSS_DFCW_DASH_SHIFT);
	spref.set("QRSS_DFCW_KEYUP_SHIFT", qrss_prefs.QRSS_DFCW_KEYUP_SHIFT);
	spref.set("QRSS_DFCW_ELEMENT_LEN", qrss_prefs.QRSS_DFCW_ELEMENT_LEN);
	spref.set("QRSS_DFCW_KEYUP_LEN", qrss_prefs.QRSS_DFCW_KEYUP_LEN);

}

void load_qrss_prefs()
{
#if FLDIGI_FLTK_API_MINOR < 4
	Fl_Preferences spref(HomeDir.c_str(), "w1hkj.org", "qrss");
#else
	Fl_Preferences spref(
		HomeDir.c_str(),
		"w1hkj.org",
		"qrss",
		Fl_Preferences::C_LOCALE);
#endif

	spref.get("CW_qrss", qrss_prefs.CW_qrss, qrss_prefs.CW_qrss);
	spref.get("FM_qrss", qrss_prefs.FM_qrss, qrss_prefs.FM_qrss);
	spref.get("DFCW_qrss", qrss_prefs.DFCW_qrss, qrss_prefs.DFCW_qrss);
	spref.get("QRSS_FM_SHIFT", qrss_prefs.QRSS_FM_SHIFT, qrss_prefs.QRSS_FM_SHIFT);
	spref.get("QRSS_ENV_MSEC", qrss_prefs.QRSS_ENV_MSEC, qrss_prefs.QRSS_ENV_MSEC);
	spref.get("QRSS_DOT", qrss_prefs.QRSS_DOT, qrss_prefs.QRSS_DOT);
	spref.get("QRSS_DFCW_DOT_SHIFT", qrss_prefs.QRSS_DFCW_DOT_SHIFT, qrss_prefs.QRSS_DFCW_DOT_SHIFT);
	spref.get("QRSS_DFCW_DASH_SHIFT", qrss_prefs.QRSS_DFCW_DASH_SHIFT, qrss_prefs.QRSS_DFCW_DASH_SHIFT);
	spref.get("QRSS_DFCW_KEYUP_SHIFT", qrss_prefs.QRSS_DFCW_KEYUP_SHIFT, qrss_prefs.QRSS_DFCW_KEYUP_SHIFT);
	spref.get("QRSS_DFCW_ELEMENT_LEN", qrss_prefs.QRSS_DFCW_ELEMENT_LEN, qrss_prefs.QRSS_DFCW_ELEMENT_LEN);
	spref.get("QRSS_DFCW_KEYUP_LEN", qrss_prefs.QRSS_DFCW_KEYUP_LEN, qrss_prefs.QRSS_DFCW_KEYUP_LEN);

}
