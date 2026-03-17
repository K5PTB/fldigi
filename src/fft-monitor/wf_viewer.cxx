
#include <iostream>
#include "configuration.h"
#include "wf_viewer.h"
#include "hwfall.h"
#include "plot_xy.h"

plot_xy*    wf_fft_plot = (plot_xy *)0;
hwfall*     wf_wfall = (hwfall *)0;

Fl_Counter* wf_viewer_range = (Fl_Counter *)0;
Fl_Counter* wf_viewer_min_db = (Fl_Counter *)0;
Fl_Choice*  wf_viewer_palette = (Fl_Choice *)0;
Fl_Choice*  wf_waterfall_speed = (Fl_Choice *)0;
Fl_Button*  btn_wf_clear_display = (Fl_Button *)0;
Fl_Output*  wf_cursor_freq = (Fl_Output *)0;

static void cb_wf_viewer_min_db(Fl_Counter* o, void*) {
  progdefaults.wf_viewer_min_db = o->value();
  progdefaults.changed = true;
}

static void cb_wf_viewer_range(Fl_Counter* o, void*) {
  progdefaults.wf_viewer_range = o->value();
  progdefaults.changed = true;
}

static void cb_wf_viewer_palette (Fl_Choice* o, void*) {
	if (o->value() >= 0 && o->value() < 27) {
		progdefaults.wf_palette = o->value();
		wf_wfall->setcolors(progdefaults.wf_palette);
		progdefaults.changed = true;
	}
}

static void cb_wf_waterfall_speed (Fl_Choice* o, void*) {
	progdefaults.wf_waterfall_rate = o->value();
	progdefaults.changed = true;
}

static void cb_wf_clear_display (Fl_Choice* o, void*) {
	wf_wfall->clear();
}

Fl_Double_Window* create_wf_viewer() {
	Fl_Double_Window* w = new Fl_Double_Window(660, 860, "Signal Viewer");

		wf_fft_plot = new plot_xy(5, 5, 50, 800, "");
		wf_fft_plot->box(FL_DOWN_BOX);
		wf_fft_plot->x_scale(0, 100, 5);
		wf_fft_plot->y_scale(0, 800, 8);
		wf_fft_plot->bk_color(FL_BLACK);
		wf_fft_plot->axis_color(fl_rgb_color(128,128,128));
		wf_fft_plot->line_color_1(fl_rgb_color(192,192,192));
		wf_fft_plot->plot_over_axis(true);
		wf_fft_plot->legends(false);
		wf_fft_plot->set_borders(false);

		wf_wfall = new hwfall(
			wf_fft_plot->x() + wf_fft_plot->w(), wf_fft_plot->y(),
			600, 800, 
			600, 800);
		wf_wfall->box(FL_THIN_DOWN_BOX);
		wf_wfall->color(FL_BLACK);
		wf_wfall->selection_color(FL_BACKGROUND_COLOR);
		wf_wfall->labeltype(FL_NORMAL_LABEL);
		wf_wfall->labelfont(0);
		wf_wfall->labelsize(14);
		wf_wfall->labelcolor(FL_FOREGROUND_COLOR);
		wf_wfall->setcolors(progdefaults.wf_palette);
		wf_wfall->align(Fl_Align(FL_ALIGN_CENTER));
		wf_wfall->when(FL_WHEN_RELEASE);

		wf_cursor_freq = new Fl_Output(
			wf_fft_plot->x(), wf_fft_plot->y() + wf_fft_plot->h() + 10, 
			100, 22, "");
		wf_cursor_freq->box(FL_DOWN_BOX);
		wf_cursor_freq->value("7030.000");

		wf_viewer_min_db = new Fl_Counter( 
			wf_cursor_freq->x() + wf_cursor_freq->w() + 10, wf_cursor_freq->y(),
			100, 22, "Minimum (db)");
		wf_viewer_min_db->value(progdefaults.wf_viewer_min_db);
		wf_viewer_min_db->step(1);
		wf_viewer_min_db->lstep(10);
		wf_viewer_min_db->minimum(-20);
		wf_viewer_min_db->maximum(20);
		wf_viewer_min_db->align(Fl_Align(FL_ALIGN_BOTTOM));
		wf_viewer_min_db->callback((Fl_Callback*)cb_wf_viewer_min_db);

		wf_viewer_range = new Fl_Counter( 
			wf_viewer_min_db->x() + wf_viewer_min_db->w() + 10, wf_cursor_freq->y(),
			100, 22, "Range (db)");
		wf_viewer_range->value(progdefaults.wf_viewer_range);
		wf_viewer_range->step(1);
		wf_viewer_range->lstep(10);
		wf_viewer_range->minimum(20);
		wf_viewer_range->maximum(140);
		wf_viewer_range->align(Fl_Align(FL_ALIGN_BOTTOM));
		wf_viewer_range->callback((Fl_Callback*)cb_wf_viewer_range);

		wf_viewer_palette = new Fl_Choice(
			wf_viewer_range->x() + wf_viewer_range->w() + 10, wf_cursor_freq->y(),
			120, 22, "Color Palette");
		for (int n = 0; n < 27; n++) \
			wf_viewer_palette->add(hwfall::palettes[n].name.c_str());
		wf_viewer_palette->align(Fl_Align(FL_ALIGN_BOTTOM));
		wf_viewer_palette->value(progdefaults.wf_palette);
		wf_viewer_palette->callback((Fl_Callback*)cb_wf_viewer_palette);

		wf_waterfall_speed = new Fl_Choice(
			wf_viewer_palette->x() + wf_viewer_palette->w() + 10, 815,
			120, 22, "Waterfall rate");
		for (int n = 0; n < 27; n++) \
			wf_waterfall_speed->add("Fast|Medium|Slow|Very Slow|Pause");
		wf_waterfall_speed->align(Fl_Align(FL_ALIGN_BOTTOM));
		wf_waterfall_speed->value(progdefaults.wf_waterfall_rate);
		wf_waterfall_speed->callback((Fl_Callback*)cb_wf_waterfall_speed);

		btn_wf_clear_display = new Fl_Button(
			wf_waterfall_speed->x() + wf_waterfall_speed->w() + 10, wf_cursor_freq->y(),
			80, 22, "Clear");
		btn_wf_clear_display->callback((Fl_Callback*)cb_wf_clear_display);

	w->end();

  return w;
}

std::vector<float> wf_slice;
PLOT_XY data[800];

static void update_data(void *)
{
	wf_wfall->add_slice(wf_slice);
	wf_fft_plot->data_1(data, 800);
	wf_fft_plot->redraw();
}

double maxbin = 0;
double gain = 0.05;
void wf_data_update(double *bins)
{
	int p = 0;
	double db;
	if (!wf_wfall) return;
	wf_slice.assign(800, 0);
	for (int n = 0; n < 800; n++) {
		db = (10 * log10(bins[n] + 1e-10) - progdefaults.wf_viewer_min_db) / progdefaults.wf_viewer_range;
		data[n].x = 100 * db;
		data[n].y = n;

		p = 799 - n;
		wf_slice[p] = db;
		if (wf_slice[p] < 0) wf_slice[p] = 0;
		if (wf_slice[p] > 1) wf_slice[p] = 1;

	}
	Fl::awake(update_data);
}
