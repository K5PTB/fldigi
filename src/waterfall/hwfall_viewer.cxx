// ----------------------------------------------------------------------------
// viewer - horizontal waterfall / spectrum view for long duration modes
//
// Copyright (C) 2026
//		Dave Freese, W1HKJ
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

#include <iostream>
#include "configuration.h"
#include "hwfall_viewer.h"
#include "hwfall.h"
#include "plot_xy.h"
#include "util.h"

#include "modem.h"
#include "filters.h"
#include "fftfilt.h"
#include "mbuffer.h"
#include "fl_digi.h"
#include "threads.h"
#include "trx.h"
#include "rigsupport.h"
#include "status.h"
#include "threads.h"
#include "debug.h"

#include "gettext.h"

#include "gfft.h"		// gfft definition
#include "misc.h"		// window functions

#include "icons.h"

namespace hwf_viewer {

bool use_gfft = true;

enum {
	WF_FFT_RECTANGULAR, WF_FFT_BLACKMAN, WF_FFT_HAMMING, WF_FFT_HANNING
};

#define HWF_FFTLEN		8192
#define HWF_SIZE		(HWF_FFTLEN / 2)
#define HWF_SAMPLERATE	8000

double		*fftwindow = 0;

g_fft<double>	*wfft = 0;

double		*wfreal = 0;
cmplx		*wfcplx = 0;

#define		NUM_BINS		400 // progStatus.hwfall_num_bins
#define		NUM_SLICES		600 // progStatus.hwfall_num_slices * 100
#define		NUM_FREQ_LABELS		(NUM_BINS / 100 + 1)
#define		NUM_TIME_LABELS		(NUM_SLICES / 60)

double		bins[HWF_FFTLEN];
double		buf[HWF_FFTLEN];
double		gfft_fifo[HWF_FFTLEN];

std::vector<double> slice;
PLOT_XY data[NUM_BINS];

double			blkcnt = 0;
double			blkincr = 0.01;

int numpts = 1;
double		nextslice = 0.1;

int			buflen = 1;

sfft		*viewer_sfft;
cmplx		sfft_bins[NUM_BINS];

int			gfft_start_bin = 0;

int			sfft_start_bin = 0;
int			sfft_end_bin = 1;

plot_xy*    fft_plot = (plot_xy *)0;
hwfall*     wfall = (hwfall *)0;

Fl_Double_Window	*viewer	= (Fl_Double_Window *)0;

Fl_Box*		freq_label[NUM_FREQ_LABELS];
Fl_Box*		time_label[NUM_TIME_LABELS];

Fl_Counter2* viewer_range = (Fl_Counter2 *)0;
Fl_Counter2* viewer_min_db = (Fl_Counter2 *)0;
Fl_Counter2* waterfall_rate = (Fl_Counter2 *)0;
Fl_Counter2* center_freq = (Fl_Counter2 *)0;

Fl_Choice*  viewer_palette = (Fl_Choice *)0;
Fl_Choice*	lowpass = (Fl_Choice *)0;
Fl_Choice*	freq_scale = (Fl_Choice *)0;
Fl_Choice*	fft_window = (Fl_Choice *)0;
Fl_Choice*	fft_select = (Fl_Choice *)0;

Fl_Button*  btn_clear_display = (Fl_Button *)0;
Fl_Button*	btn_rf_audio = (Fl_Button *)0;
Fl_Check_Button*	graticule_on_off = (Fl_Check_Button *)0;

Fl_Light_Button* btn_pause_display = (Fl_Light_Button *)0;
Fl_Choice* averaging = (Fl_Choice *)0;

// "100|200|400|500|1000"
static int scf[] = { 1, 2, 4, 5, 10 };

static void cb_viewer_min_db(Fl_Counter2* o, void*) {
  progStatus.hwfall_viewer_min_db = o->value();
}

static void cb_viewer_range(Fl_Counter2* o, void*) {
  progStatus.hwfall_viewer_range = o->value();
}

static void cb_viewer_palette (Fl_Choice* o, void*) {
	if (o->value() >= 0 && o->value() < 28) {
		progStatus.hwfall_palette = o->value();
		wfall->setcolors(progStatus.hwfall_palette);
		wfall->redraw();
	}
}

static void cb_scale (Fl_Choice* o, void*) {
	progStatus.hwfall_scale = o->value() + 1;
	wfall->set_y_scale( scf[progStatus.hwfall_scale - 1] );
	init();
}

static void cb_waterfall_rate (Fl_Counter2* o, void*) {
	progStatus.hwfall_waterfall_rate = o->value();
	blkcnt = 0;
	nextslice = waterfall_rate->value();
	update_labels();
}

static void cb_clear_display (Fl_Button* o, void*) {
	wfall->clear();
	blkcnt = 0;
	nextslice = waterfall_rate->value();
}

static void cb_btn_rf_audio (Fl_Button* o, void*) {
	progStatus.show_rf_labels = !progStatus.show_rf_labels;
	update_labels();
}

static void cb_center_freq (Fl_Counter2* o, void *) {
	init();
}

static void cb_averaging (Fl_Choice *, void *)
{
	progStatus.hwfall_average_selection  = averaging->value();
}

static void cb_fft_window (Fl_Choice *, void *)
{
	progStatus.hwfall_fft_window = fft_window->value();
}

static void cb_fft_select (Fl_Choice *, void *)
{
	if (fft_select->value() == 0) {
		use_gfft = true;
		fft_window->activate();
	}
	else  {
		use_gfft = false;
		fft_window->deactivate();
	}
	init();
}

static void cb_graticule_on_off (Fl_Check_Button *, void *)
{
	progStatus.graticule_on_off = graticule_on_off->value();
	wfall->set_graticule_on (progStatus.graticule_on_off);
}

Fl_Double_Window* create_viewer() {

	Fl_Double_Window* w = new Fl_Double_Window(NUM_SLICES + 160, NUM_BINS + 100, _("Waterfall Signal Viewer"));

		wfall = new hwfall(
			8, 8, NUM_SLICES, NUM_BINS);
		wfall->box(FL_THIN_DOWN_BOX);
		wfall->color(FL_BLACK);
		wfall->selection_color(FL_BACKGROUND_COLOR);
		wfall->labeltype(FL_NORMAL_LABEL);
		wfall->labelfont(0);
		wfall->labelsize(14);
		wfall->labelcolor(FL_FOREGROUND_COLOR);
		wfall->setcolors(progStatus.hwfall_palette);
		wfall->align(Fl_Align(FL_ALIGN_CENTER));
		wfall->when(FL_WHEN_RELEASE);
		wfall->set_miny(0);
		wfall->tooltip(_("Left click for frequeny"));

		fft_plot = new plot_xy(
			wfall->x() + wfall->w(), wfall->y(), 60, wfall->h(), "");
		fft_plot->box(FL_DOWN_BOX);
		fft_plot->x_scale(-100, 0, 5);
		fft_plot->y_scale(0, NUM_BINS, NUM_BINS / 100);
		fft_plot->bk_color(FL_BLACK);
		fft_plot->axis_color(fl_rgb_color(192,192,192));
		fft_plot->line_color_1(fl_rgb_color(192,192,192));
		fft_plot->plot_over_axis(true);
		fft_plot->legends(false);
		fft_plot->set_borders(false);
		fft_plot->reverse_x(true);

		for (int n = 0; n < NUM_FREQ_LABELS; n++) {
			freq_label[n] = new Fl_Box(
				fft_plot->x() + fft_plot->w(), wfall->y() + NUM_BINS - 10 - n * 100, 90, 20, "14070.100");
				freq_label[n]->box(FL_FLAT_BOX);
				freq_label[n]->color(fl_rgb_color(192,192,192));
		}

		for (int n = 0; n < NUM_TIME_LABELS; n++) {
			time_label[n] = new Fl_Box(
				wfall->x() + wfall->w() - 25 - n * 60, wfall->y() + wfall->h(),
				50, 20, "");
				time_label[n]->box(FL_FLAT_BOX);
				time_label[n]->color(fl_rgb_color(192,192,192));
		}

//----------------------------------------------------------------------

		viewer_min_db = new Fl_Counter2( 
			4, wfall->y() + wfall->h() + 20,
			100, 22, _("Minimum (db)"));
		viewer_min_db->value(progStatus.hwfall_viewer_min_db);
		viewer_min_db->step(1);
		viewer_min_db->lstep(10);
		viewer_min_db->minimum(-60);
		viewer_min_db->maximum(60);
		viewer_min_db->value(progStatus.hwfall_viewer_min_db);
		viewer_min_db->align(Fl_Align(FL_ALIGN_BOTTOM));
		viewer_min_db->callback((Fl_Callback*)cb_viewer_min_db);

		viewer_range = new Fl_Counter2( 
			viewer_min_db->x() + viewer_min_db->w() + 4, viewer_min_db->y(),
			100, 22, _("Range (db)"));
		viewer_range->value(progStatus.hwfall_viewer_range);
		viewer_range->step(1);
		viewer_range->lstep(10);
		viewer_range->minimum(20);
		viewer_range->maximum(140);
		viewer_range->value(progStatus.hwfall_viewer_range);
		viewer_range->align(Fl_Align(FL_ALIGN_BOTTOM));
		viewer_range->callback((Fl_Callback*)cb_viewer_range);

		center_freq = new Fl_Counter2(
			viewer_range->x() + viewer_range->w() + 4, viewer_range->y(),
			75, 22, "Center");
		center_freq->tooltip(_("Center frequency"));
		center_freq->minimum(100);
		center_freq->maximum(2700);
		center_freq->step(100);
		center_freq->type(FL_SIMPLE_COUNTER);
		center_freq->value(progStatus.hwfall_center_freq);
		center_freq->callback((Fl_Callback*)cb_center_freq);

		waterfall_rate = new Fl_Counter2(
			center_freq->x() + center_freq->w() + 4, center_freq->y(),
			108, 22, "Update");
		waterfall_rate->value(0.5);
		waterfall_rate->step(.1);
		waterfall_rate->lstep(1.0);
		waterfall_rate->minimum(0.1);
		waterfall_rate->maximum(60.0);
		waterfall_rate->value(progStatus.hwfall_waterfall_rate);
		waterfall_rate->align(Fl_Align(FL_ALIGN_BOTTOM));
		waterfall_rate->callback((Fl_Callback*)cb_waterfall_rate);
		waterfall_rate->tooltip(_("Update rate in seconds"));

		freq_scale = new Fl_Choice(
			waterfall_rate->x() + waterfall_rate->w() + 5, waterfall_rate->y(),
			83, 22, _("Freq Scale"));
		freq_scale->add("100|200|400|500|1000");
		freq_scale->value(progStatus.hwfall_scale - 1);
		freq_scale->align(Fl_Align(FL_ALIGN_BOTTOM));
		freq_scale->callback((Fl_Callback*)cb_scale);
		freq_scale->tooltip(_("Hertz / division"));

		viewer_palette = new Fl_Choice(
			freq_scale->x() + freq_scale->w() + 5, freq_scale->y(),
			83, 22, _("Color Palette"));
		for (int n = 0; n < 28; n++) \
			viewer_palette->add(hwfall::palettes[n].name.c_str());
		viewer_palette->align(Fl_Align(FL_ALIGN_BOTTOM));
		viewer_palette->value(progStatus.hwfall_palette);
		viewer_palette->callback((Fl_Callback*)cb_viewer_palette);

		fft_window = new Fl_Choice(
			viewer_palette->x() + viewer_palette->w() + 5, viewer_palette->y(),
			85, 22, _("fft window"));
			fft_window->add(_("Rect|Blackman|Hamming|Hanning"));
			fft_window->align(Fl_Align(FL_ALIGN_BOTTOM));
			fft_window->value(progStatus.hwfall_fft_window);
			fft_window->callback((Fl_Callback *)cb_fft_window);

		averaging = new Fl_Choice(
			fft_window->x() + fft_window->w() + 5, viewer_min_db->y(),
			85, 22, _("Filter"));
			averaging->add(_("Average|Peak|Last"));
			averaging->value(progStatus.hwfall_average_selection);
			averaging->align(Fl_Align(FL_ALIGN_BOTTOM));
			averaging->callback((Fl_Callback*)cb_averaging);

		btn_clear_display = new Fl_Button(
			freq_scale->x(), freq_scale->y() + center_freq->h() + 20,
			83, 22, _("Clear"));
		btn_clear_display->callback((Fl_Callback*)cb_clear_display);

		btn_pause_display = new Fl_Light_Button(
			btn_clear_display->x() + btn_clear_display->w() + 5, btn_clear_display->y(),
			83, 22, _("Pause"));

		btn_rf_audio = new Fl_Button(
			btn_pause_display->x() + btn_pause_display->w() + 5, btn_clear_display->y(),
			83, 22, "RF/Audio");
		btn_rf_audio->callback((Fl_Callback*)cb_btn_rf_audio);

		fft_select = new Fl_Choice( 
			viewer_min_db->x(), btn_clear_display->y(),
			150, 22, "");
		fft_select->add( _(
			"  Standard FFT  |  Sliding DFT  ") );
		fft_select->value(progStatus.fft_select);
		fft_select->callback( (Fl_Callback *)cb_fft_select);
		fft_select->tooltip (_("Time->Frequency Transform algorithm"));

		graticule_on_off = new Fl_Check_Button(
			center_freq->x(), fft_select->y(),
			120, 18, _("Graticule ON") );
		graticule_on_off->value(progStatus.graticule_on_off);
		graticule_on_off->callback((Fl_Callback *)cb_graticule_on_off);

	w->end();

	wfall->set_y_scale( scf[progStatus.hwfall_scale - 1] );

	init();

  return w;
}

static void update_signal_plot(void *)
{
	fft_plot->data_1(data, NUM_BINS);
	fft_plot->redraw();
	update_labels();
}

static void update_wfall(void *)
{
	wfall->add_slice(slice);
	for (int n = 0; n < NUM_BINS; n++) slice[n] = 0;
}

void update_frequency()
{
	std::string testmode = qso_opMODE->value();
	int mdoffset = 0;

	if (testmode.find("CW") != std::string::npos)
		mdoffset = progdefaults.CWsweetspot;
	long long freq = wf->rfcarrier();
	if (wf->USB()) freq -= mdoffset;
	else           freq += mdoffset;

	if (progStatus.show_rf_labels)
		wfall->set_fmin( freq );
	else
		wfall->set_fmin( 0 );

	wfall->set_USB ( wf->USB() );

}


void data_update(double *bins, int len)
{
	if (!wfall) return;

	int p = 0;
	double db;
	double fft_scale = scf[progStatus.hwfall_scale - 1];

	if (btn_pause_display->value()) return;

	for (int bin = 0; bin < NUM_BINS; bin++) {

		int gfft_bin = (gfft_start_bin + fft_scale * bin) * HWF_FFTLEN / active_modem->get_samplerate();
		if (gfft_bin >= HWF_SIZE)
			gfft_bin = HWF_SIZE - 1;

		if (use_gfft)
			db = 10 * log10(bins[gfft_bin] + 1e-10) - 10;
		else
			db = 10 * log10(bins[bin] + 1e-10) - 10;

	// map to 0 to 1
		db = (db - progStatus.hwfall_viewer_min_db) / progStatus.hwfall_viewer_range;
		if (db > 1) db = 1;
		if (db < 0) db = 0;
		p = NUM_BINS - 1 - bin;

		switch (progStatus.hwfall_average_selection) {
			default:
			case 0 : // average
				slice[p] = (slice[p] * (numpts - 1) + db) / numpts;
				break;
			case 1 : // peak
				if (db > slice[p]) slice[p] = db;
				break;
			case 2 : // last
				slice[p] = db;
				break;
		}
		data[bin].x = -100 * db;
		data[bin].y = bin;
	}

	update_frequency();

	Fl::awake(update_signal_plot);

	if (blkcnt > nextslice ) {
		Fl::awake( update_wfall );
		numpts = 1;
		nextslice += waterfall_rate->value();
	} else {
		blkcnt += blkincr;
		++numpts;
	}

}

static pthread_mutex_t	mutex = PTHREAD_MUTEX_INITIALIZER;

void update_labels(bool show)
{
	char szlabel[20];
	double fft_scale = scf[progStatus.hwfall_scale - 1];
	double freq = center_freq->value() - (NUM_BINS / 2) * fft_scale;

	bool isLSB = ModeIsLSB(qso_opMODE->value());
	std::string mode_name = qso_opMODE->value();
	bool isCW = mode_name.find("CW") != std::string::npos;

	long rfc = wf->rfcarrier();
	if (isCW && isLSB) rfc += progdefaults.CWsweetspot;
	if (isCW && !isLSB) rfc -= progdefaults.CWsweetspot;
/*
//if (show)
std::cout << 
"------------------------------------------------" << std::endl <<
"scale:       " << fft_scale << std::endl <<
"center freq: " << center_freq->value() << std::endl <<
"rfcarrier:   " << rfc << std::endl <<
"freq:        " << freq << std::endl <<
"LSB:         " << (isLSB ? "yes" : "no") << std::endl <<
"is CW:       " << (isCW ? "yes" : "no") << std::endl;
*/
	for (int n = 0; n < NUM_FREQ_LABELS; n++) {

		if (progStatus.show_rf_labels) {
			if (!isLSB)
				snprintf(szlabel, sizeof(szlabel), "%-11.3f", (rfc + freq) / 1000.0);
			else
				snprintf(szlabel, sizeof(szlabel), "%-11.3f", (rfc - freq) / 1000.0);
		}
			else
				snprintf(szlabel, sizeof(szlabel), "%-11.0f", freq);

		freq_label[n]->copy_label(szlabel);
		freq_label[n]->redraw_label();
		freq += 100 * fft_scale;
	}

	int time = 0;
	for (int n = 1; n < NUM_TIME_LABELS; n++) {
		if ( (time = n * 60 * waterfall_rate->value()) >= 60)
			if (time % 60 == 0)
				snprintf(szlabel, sizeof(szlabel), "%.0f m", n * waterfall_rate->value());
			else
				snprintf(szlabel, sizeof(szlabel), "%.1f m", n * waterfall_rate->value());
		else
			snprintf(szlabel, sizeof(szlabel), "%.0f s", n * 60 * waterfall_rate->value());
		time_label[n]->copy_label(szlabel);
		time_label[n]->redraw_label();
	}

}

void init()
{
	if (!wfall) return;

	guard_lock sfft_lock( &mutex );

	delete viewer_sfft;

	double fft_scale = scf[progStatus.hwfall_scale - 1];

	gfft_start_bin = (int)center_freq->value() - fft_scale * NUM_BINS / 2;
	if (gfft_start_bin < 0) {
		gfft_start_bin = 0;
		center_freq->value (gfft_start_bin + fft_scale * NUM_BINS / 2);
	}

	sfft_start_bin = center_freq->value() / fft_scale - NUM_BINS / 2;

	int sfft_len = active_modem->get_samplerate() / fft_scale;
	if (sfft_start_bin < 0) {
		sfft_start_bin = 0;
		center_freq->value ((sfft_start_bin + NUM_BINS / 2) * fft_scale);
	} else if (sfft_start_bin > (sfft_len - NUM_BINS) ) {
		sfft_start_bin = sfft_len - NUM_BINS;
		center_freq->value ((sfft_start_bin + NUM_BINS / 2) * fft_scale);
	}
	progStatus.hwfall_center_freq = center_freq->value();

	sfft_end_bin = sfft_start_bin + NUM_BINS;

	wfall->set_graticule_on (progStatus.graticule_on_off);

/*
if (!use_gfft)
std::cout << 
"------------------------------------------------" << std::endl <<
"start bin:   " << sfft_start_bin << std::endl <<
"end bin:     " << sfft_end_bin << std::endl <<
"num bins:    " << NUM_BINS << std::endl <<
"center freq: " << center_freq->value() << std::endl <<
"fft scale:   " << fft_scale << std::endl <<
"sfft length: " << sfft_len << std::endl;
*/

	viewer_sfft = new sfft( active_modem->get_samplerate() / fft_scale, sfft_start_bin, sfft_end_bin);

	slice.assign(NUM_BINS, 0);

	blkcnt = 0;
	blkincr = 1.0 * buflen / active_modem->get_samplerate();
	nextslice = waterfall_rate->value();
	update_frequency();
	if (use_gfft)
		wfall->set_miny(gfft_start_bin);
	else
		wfall->set_miny(sfft_start_bin * fft_scale);

	update_labels();

}

static pthread_t       hwf_pthread;
static pthread_cond_t  hwf_cond;
static pthread_mutex_t hwf_mutex        = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t hwf_buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t        hwf_ptt_mutex    = PTHREAD_MUTEX_INITIALIZER;

static bool hwf_thread_running   = false;
static bool hwf_terminate_flag   = false;

int gfft_rx_process()
{
	pthread_mutex_lock (&hwf_buffer_mutex);

		memcpy(gfft_fifo, &gfft_fifo[buflen], sizeof(double) * (HWF_FFTLEN - buflen));
		memcpy(&gfft_fifo[HWF_FFTLEN - buflen], buf, sizeof(double) * buflen);

	pthread_mutex_unlock (&hwf_buffer_mutex);

	switch (progStatus.hwfall_fft_window) {
		case 0: RectWindow(fftwindow, HWF_FFTLEN); break;
		default:
		case 1: BlackmanWindow(fftwindow, HWF_FFTLEN); break;
		case 2: HammingWindow(fftwindow, HWF_FFTLEN); break;
		case 3: HanningWindow(fftwindow, HWF_FFTLEN);
	}
	for (int n = 0; n < HWF_FFTLEN; n++)
		wfreal[n] = gfft_fifo[n] * fftwindow[n];

	wfft->RealFFT(wfcplx);

	for (int n = 0; n < HWF_SIZE; n++) 
		bins[n] = std::norm(wfcplx[n]);

	data_update(bins, HWF_SIZE);

	return 0;
}

static void * hwf_loop(void *args)
{
	SET_THREAD_ID(CWIO_TID);

	hwf_thread_running   = true;
	hwf_terminate_flag   = false;

	while(1) {
		pthread_mutex_lock(&hwf_mutex);
		pthread_cond_wait(&hwf_cond, &hwf_mutex);
		pthread_mutex_unlock(&hwf_mutex);

		if (hwf_terminate_flag)
			break;

		gfft_rx_process();
	}
	return (void *)0;
}

void start_hwf_thread(void)
{
	memset((void *) &hwf_pthread, 0, sizeof(hwf_pthread));
	memset((void *) &hwf_mutex,   0, sizeof(hwf_mutex));
	memset((void *) &hwf_cond,    0, sizeof(hwf_cond));

	if(pthread_cond_init(&hwf_cond, NULL)) {
		LOG_ERROR("hwf thread create fail (pthread_cond_init)");
		return;
	}

	if(pthread_mutex_init(&hwf_mutex, NULL)) {
		LOG_ERROR("hwf thread create fail (pthread_mutex_init)");
		return;
	}

	if (pthread_create(&hwf_pthread, NULL, hwf_loop, NULL) < 0) {
		pthread_mutex_destroy(&hwf_mutex);
		LOG_ERROR("hwf thread create fail (pthread_create)");
	}

	LOG_DEBUG("started hwf thread");

}

void stop_hwf_thread(void)
{
	if(!hwf_thread_running) return;

	hwf_terminate_flag = true;
	pthread_cond_signal(&hwf_cond);

	MilliSleep(10);

	pthread_join(hwf_pthread, NULL);

	pthread_mutex_destroy(&hwf_mutex);
	pthread_cond_destroy(&hwf_cond);

	memset((void *) &hwf_pthread, 0, sizeof(hwf_pthread));
	memset((void *) &hwf_mutex,   0, sizeof(hwf_mutex));

	hwf_thread_running   = false;
	hwf_terminate_flag   = false;

	delete fftwindow;
	delete wfft;
}


//int smpl_count = 0;

int rx_process(const double *buffer, int len)
{
	if (!wfall || !wfall->visible()) return 0;

if (use_gfft) {
	if (!wfreal)    {
		wfreal = new double[HWF_FFTLEN];
		wfcplx = (cmplx *)(wfreal);
	}
	if (!wfft)      wfft = new g_fft<double>(HWF_FFTLEN);
	if (!fftwindow) fftwindow = new double[HWF_FFTLEN];

	if (!hwf_thread_running) start_hwf_thread();

	pthread_mutex_lock(&hwf_buffer_mutex);

		buflen = len;
		blkincr = 1.0 * buflen / active_modem->get_samplerate();
		memcpy(buf, buffer, sizeof(double) * len);

	pthread_mutex_unlock(&hwf_buffer_mutex);

	pthread_cond_signal (&hwf_cond);

} // use_gfft

else { //use_sfft

	cmplx cmplx_in;

	{
		guard_lock sfft_lock( &mutex );
		for (int i = 0; i < len; i++) {
			cmplx_in = cmplx(buffer[i], buffer[i]);
			viewer_sfft->run(cmplx_in, sfft_bins, 1);
		}
	}
	blkincr = 1.0 * buflen / active_modem->get_samplerate();
	for (int n = 0; n < NUM_BINS; n++)
		bins[n] = norm(sfft_bins[n]);
	data_update(bins, len);
} // use_sfft

	return 0;
}

}; // namespace
