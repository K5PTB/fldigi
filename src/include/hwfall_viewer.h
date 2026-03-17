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

#ifndef viewer_h
#define viewer_h

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Light_Button.H>
#include <FL/Fl_Check_Button.H>

#include "flslider2.h"
#include "hwfall.h"

namespace hwf_viewer {

extern hwfall *wfall;

extern Fl_Double_Window* create_viewer();
extern Fl_Counter2* viewer_range;
extern Fl_Counter2* viewer_min_db;
extern Fl_Counter2* waterfall_speed;
extern Fl_Choice*  viewer_palette;
extern Fl_Button*  btn_clear_display;
extern Fl_Light_Button* btn_pause_display;

//extern void data_update(double *, int);

extern Fl_Double_Window *viewer;

extern void init();
extern void update_labels(bool show = false);
extern int gfft_rx_process();

extern int rx_process(const double *buf, int len);
};

#endif
