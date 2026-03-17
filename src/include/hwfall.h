// ----------------------------------------------------------------------------
// Horizontal Waterfall Spectrum Analyzer Widget
// Copyright (C) 2026 Dave Freese, W1HKJ
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

#ifndef _HWFALL_H
#define _HWFALL_H

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Tooltip.H>
#include <vector>
#include <cmath>
#include <string>

class hwfall : public Fl_Widget {
public:
	struct RGB {
		uchar R;
		uchar G;
		uchar B;
	};

	struct PALETTE {
		std::string	name;
		RGB			rgb[9];
	};

//	RGB RGBpalette[9];
	RGB RGByellow    = { 255, 255, 0 };
	RGB RGBgreen     = { 0, 255, 0 };
	RGB RGBdkgreen   = { 0, 128, 0 };
	RGB RGBblue      = { 0, 0, 255 };
	RGB RGBred       = { 255, 0, 0 };
	RGB RGBwhite     = { 255, 255, 255 };
	RGB RGBblack     = { 0, 0, 0 };
	RGB RGBmagenta   = { 196, 0, 196 };
	RGB RGBgrey      = { 128, 128, 128 };
	RGB RGBdarkgrey  = { 64, 64, 64 };
	RGB RGBlightgrey = { 192, 192, 192 };
	RGB RGBmedgrey   = { 96, 96, 96 };

	RGB		db_RGB[256];

	static	PALETTE palettes[];

private:
	int img_w, img_h;
	RGB *image;
	unsigned char *data;
//	unsigned char *dp;

	int  minx, miny;
	double  scaley;
	long long fmin;
	bool   USB;
	bool   graticule_on;

	int  pal_select;
	PALETTE palette;

	struct {
		bool enabled;
		double delay;
		double hide_delay;
	} tooltips;

public:
// X - horizontal position
// Y - vertical position
// W - width
// H - height
// w - horizontal range
// h - vertical range

	hwfall(int X, int Y, int W, int H);
	~hwfall() {}

	void handle_leftclick( int x1, int y1);
	void handle_rightclick( int x, int y);
	int handle(int);

	void add_slice(const std::vector<double>& new_slice);
	void draw() override;
	void setcolors(int nbr);
	void clear();

	void graticules();

	int w() { return img_w; }
	int h() { return img_h; }

	double get_minx() { return minx; }
	void  set_minx(double val) { minx = val; }

	double get_miny() { return miny; }
	void  set_miny(double val) { miny = val; }

	void set_fmin( long long f0 ) { fmin = f0; }
	long long get_fmin() { return fmin; }

	void set_USB(bool usb) { USB = usb; }

	void set_y_scale( double scale ) { scaley = scale; }

	void set_graticule_on( int val ) { graticule_on = val; }
};

#endif // _HWFALL_H

