// ----------------------------------------------------------------------------
// qrss.h  --  VERY LONG DURATION morse code modem
//
// Copyright (C) 2006-2009
//		Dave Freese, W1HKJ
//
// This file is part of fldigi.  Adapted in part from code contained in 
// gmfsk source code distribution.
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

#ifndef _QRSS_H
#define _QRSS_H

#include <cstring>
#include <string>

#include <config.h>

#include "modem.h"
#include "filters.h"
#include "fftfilt.h"
#include "mbuffer.h"

#define	QRSS_SAMPLERATE   8000
#define SFFT_SIZE         8000

#define QRSS_F0           600
#define QRSS_HZPERBIN     (( QRSS_SAMPLERATE) / (1.0 * SFFT_SIZE))

#define QRSS_BW2 400
#define NUM_BINS (QRSS_BW2 * 2)

#define FIRST_BIN ((int)(QRSS_F0/(QRSS_HZPERBIN)) - QRSS_BW2)
#define LAST_BIN  (FIRST_BIN + NUM_BINS)

#define QRSS_SIZE (OUTBUFSIZE / 8)

struct QRSS_PREFS {
	int    CW_qrss;
	int    FM_qrss;
	int    DFCW_qrss;
	int    QRSS_FM_SHIFT;
	int    QRSS_ENV_MSEC;
	double QRSS_DOT;
	double QRSS_DFCW_DOT_SHIFT;
	double QRSS_DFCW_DASH_SHIFT;
	double QRSS_DFCW_KEYUP_SHIFT;
	double QRSS_DFCW_ELEMENT_LEN;
	double QRSS_DFCW_KEYUP_LEN;
};

extern QRSS_PREFS qrss_prefs;
extern void save_qrss_prefs();
extern void load_qrss_prefs();

class qrss : public modem {

protected:
	int			symbollen;		// length of a dot in sound samples (tx)

	double		qrss_phase;		// used by NCO for rx/tx tones

	cmplx		sfft_bins[NUM_BINS];
	Cmovavg		*binfilter[NUM_BINS];
	double		bins[NUM_BINS];
	sfft		*qrss_filter;

// user configurable data - local copy passed in from gui
	float qrss_speed;

	long int QRSS_DOT_length;			// Length of a send Dot, in Usec 

	double qrssbuf[QRSS_SIZE];

	cmplx mixer(cmplx);

	double nco(double freq);

	void create_edges();
	void send_symbol(int, long);
	void send_tones(int);
	void send_dfcw(int);
	void send_dfcw_dot();
	void send_dfcw_dash();
	void send_dfcw_keyup();

public:
	qrss();
	~qrss();
	void	init();
	void	rx_init();
	void	tx_init();
	void	restart(){}

	int		rx_process(const double *, int);
	void	rx_sfft(const double *, int);

	int		tx_process();

	double 	*qrss_bins() { return bins; }
};

void open_qrss_KEYLINE();
void close_QRSS_KEYLINE();

void qrssio_key(int);
void qrssio_ptt(int);
double qrssio_now();
void qrssio_bit(int, double);
void send_qrssio(int);
void start_qrssio_thread(void);
void stop_qrssio_thread(void);
void * qrssio_loop(void *args);

void send_via_keyline(int);

#endif
