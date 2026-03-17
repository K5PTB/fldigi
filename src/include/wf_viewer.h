
#ifndef wf_viewer_h
#define wf_viewer_h
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Counter.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Output.H>

#include "hwfall.h"

extern hwfall *wf_wfall;

extern Fl_Double_Window* create_wf_viewer();
extern Fl_Counter* wf_viewer_range;
extern Fl_Counter* wf_viewer_min_db;
extern Fl_Choice*  wf_viewer_palette;
extern Fl_Choice*  wf_waterfall_speed;
extern Fl_Button*  btn_wf_clear_display;
extern Fl_Output*  wf_cursor_freq;

extern void wf_data_update(double *);

#endif
