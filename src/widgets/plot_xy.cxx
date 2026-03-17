// ---------------------------------------------------------------------
// plot_xy.cxx
//
// Copyright (C) 2019
//		David Freese, W1HKJ
//
// This is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This software is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with fldigi.  If not, see <http://www.gnu.org/licenses/>.
// ---------------------------------------------------------------------

#include <iostream>
#include <cmath>
#include <cstring>

#include "plot_xy.h"

#define PLOT_XY_DEFAULT_WIDTH	100
#define PLOT_XY_DEFAULT_HEIGHT	100
#define	PLOT_XY_MAX_LEN			57600 //8192
#define PLOT_XY_NUM_GRIDS		100

plot_xy::plot_xy (int X, int Y, int W, int H, const char *lbl) :
	Fl_Widget (X, Y, W, H, lbl)
{

	_bk_color = FL_BACKGROUND2_COLOR;
	_axis_color = FL_BLACK;
	_color_1 = FL_DARK_RED;
	_color_2 = FL_GREEN;
	_legend_color = FL_BLACK;

	color(_bk_color);

	_len_1 = PLOT_XY_MAX_LEN;
	_len_2 = PLOT_XY_MAX_LEN;

	buf_1.resize( PLOT_XY_MAX_LEN, { 0.0, 0.0 } );
	buf_2.resize( PLOT_XY_MAX_LEN, { 0.0, 0.0 } );

	xmin = 0; xmax = 10.0; x_graticule = 2.0;
	ymin = 0; ymax = 10.0; y_graticule = 2.0;

	x_legend = y_legend = borders = false;
	sx_legend.clear();
	sy_legend.clear();

	_show_1 = true;
	_show_2 = false;

	xreverse = false;

	_thick_lines = false;
}

plot_xy::~plot_xy()
{
}

void plot_xy::clear(int n)
{
	if (n == 1) {
		for (int n = 0; n < PLOT_XY_MAX_LEN; n++) {
			buf_1[n].x = 0;
			buf_1[n].y = ymax * 2;
		}
		_len_1 = 0;
	}
	if (n == 2) {
		for (int n = 0; n < PLOT_XY_MAX_LEN; n++) {
			buf_2[n].x = 0;
			buf_2[n].y = ymax * 2;
		}
		_len_2 = 0;
	}
}

void plot_xy::data_1(PLOT_XY *data, int len)
{
	_len_1 = (len > PLOT_XY_MAX_LEN ) ? PLOT_XY_MAX_LEN : len;
	for (int n = 0; n < _len_1; n++) {
		buf_1[PLOT_XY_MAX_LEN - _len_1 + n] = data[n];
	}
}

void plot_xy::data_2(PLOT_XY *data, int len)
{
	if (data == 0 || len == 0) {
		for (int n = 0; n < PLOT_XY_MAX_LEN; n++) {
			buf_2[n].x = 0;
			buf_2[n].y = ymax * 2;
		}
		_len_2 = 0;
	} else {
		_len_2 = (len > PLOT_XY_MAX_LEN ) ? PLOT_XY_MAX_LEN : len;
		for (int n = 0; n < _len_2; n++)
			buf_2[PLOT_XY_MAX_LEN - _len_2 + n] = data[n];
	}
}

void plot_xy::draw()
{
	draw_box();

	int X = x(), Y = y(), W = w(), H = h();

	if (x_legend || y_legend) {
		X += 2;
		Y += 2;
		W -= 4;
		H -= 4;
	}

	fl_clip(X, Y, W, H);
	fl_color(_bk_color);
	fl_rectf(X, Y, W, H);

	if (y_legend) {
		X += 45;
		W -= 50;
	} else if (borders) {
		X += 5;
		W -= 10;
	}

	if (x_legend) {
		Y += 10;
		H -= 30;
	} else if (borders) {
		Y += 5;
		H -= 10;
	}

	fl_push_matrix();

	fl_translate( X, (Y + H));
	fl_scale (1.0 * W / (xmax - xmin), -1.0 * H / (ymax - ymin));

// horizontal & vertical grids
	fl_line_style(FL_SOLID, 0, NULL);
	fl_color(_axis_color);

// vertical graticules
	for (int n = 0; n <= x_graticule; n++) {
		fl_begin_line();
		fl_vertex((xmax - xmin)*n/x_graticule, 0);
		fl_vertex((xmax - xmin)*n/x_graticule, (ymax - ymin));
		fl_end_line();
	}
// horizontal graticules
	for (int n = 0; n <= y_graticule; n++) {
		fl_begin_line();
		fl_vertex(0,           (ymax - ymin)*n/y_graticule);
		fl_vertex((xmax-xmin), (ymax - ymin)*n/y_graticule);
		fl_end_line();
	}

// data
	float xp, yp, xp1, yp1;
	xp = yp = xp1 = yp1 = 0.0;

// line 1
	if (_show_1) {
		int xs = PLOT_XY_MAX_LEN - _len_1;
		fl_color(_color_1);
		yp = buf_1[xs].y;
		xp = buf_1[xs].x;
		for (int i = 1; i < _len_1; i++) {
			yp1 = buf_1[xs + i].y;
			xp1 = buf_1[xs + i].x;
			if (yp == 0 && yp1 == 0 && !_plot_over_axis) {
				fl_color (_axis_color);
				fl_line_style(FL_SOLID, 0, NULL);
			} else {
				fl_color (_color_1);
				if (_thick_lines)
					fl_line_style(FL_SOLID, 2, NULL);
			}
			if ( yp > ymin  && yp < ymax  && xp > xmin  && xp < xmax &&
				 yp1 > ymin && yp1 < ymax && xp1 > xmin && xp1 < xmax ) {
				fl_begin_line();
				if (xreverse) {
					fl_vertex(xmax - xp, yp - ymin);
					fl_vertex(xmax - xp1, yp - ymin);
				} else {
					fl_vertex(xp - xmin,  yp - ymin);
					fl_vertex(xp1 - xmin, yp1 - ymin);
				}
				fl_end_line();
			}
			xp = xp1; yp = yp1;
		}
	}

// line 2
	if (_show_2) {
		int xs = PLOT_XY_MAX_LEN - _len_2;
		fl_color(_color_2);
		yp = buf_2[xs].y;
		xp = buf_2[xs].x;
		for (int i = 1; i < _len_2; i++) {
			yp1 = buf_2[xs + i].y;
			xp1 = buf_2[xs + i].x;
			if (yp == 0 && yp1 == 0 && !_plot_over_axis) {
				fl_color (_axis_color);
				fl_line_style(FL_SOLID, 0, NULL);
			} else {
				fl_color (_color_2);
				if (_thick_lines)
					fl_line_style(FL_SOLID, 2, NULL);
			}
			if ( yp > ymin  && yp < ymax  && xp > xmin  && xp < xmax &&
				 yp1 > ymin && yp1 < ymax && xp1 > xmin && xp1 < xmax ) {
				fl_begin_line();
				if (xreverse) {
					fl_vertex(xmax - xp, yp - ymin);
					fl_vertex(xmax - xp1, yp - ymin);
				} else {
					fl_vertex(xp - xmin,  yp - ymin);
					fl_vertex(xp1 - xmin, yp1 - ymin);
				}
				fl_end_line();
			}
			xp = xp1; yp = yp1;
		}
	}

	fl_pop_matrix();

	fl_line_style(FL_SOLID, 0, NULL);

// legends
	fl_color(_legend_color);
	fl_font(FL_COURIER, 12);
	int lbl_w = fl_width("XX") + 2;
	int lbl_h = fl_height();

	if (x_legend && !sx_legend.empty()) {
		std::string tmp = sx_legend;
		std::string lgnd;

		float xd = 0, yd = Y + H + 20 - lbl_h/2;
		size_t p = tmp.find("|");
		for (int n = 0; n <= x_graticule; n++) {
			if (tmp.empty()) break;
			lgnd = tmp.substr(0,p);
			lbl_w = fl_width(lgnd.c_str());
			if (xreverse)
				xd = X + W - n * W / x_graticule - lbl_w / 2;
			else
				xd = X + n * W / x_graticule - lbl_w / 2;
			if (lgnd != " ")
				fl_draw(lgnd.c_str(), xd, yd);
			tmp.erase(0, p+1);
			p = tmp.find("|");
		}
	}

	if (y_legend && !sy_legend.empty()) {
		std::string tmp = sy_legend;
		size_t p = tmp.find("|");
		std::string lgnd;
		float xd, yd;
		for (int n = 0; n <= y_graticule; n++) {
			if (tmp.empty()) break;
			lgnd = tmp.substr(0,p);
			lbl_w = fl_width(lgnd.c_str());
			xd = X - lbl_w - 4; yd = Y + H * (1 - n / y_graticule) + lbl_h / 3;
			if (lgnd != " ")
				fl_draw(lgnd.c_str(), xd, yd);
			tmp.erase(0, p+1);
			p = tmp.find("|");
		}
	}

	fl_pop_clip();

}

int plot_xy::handle(int event)
{
	if (!Fl::event_inside(this))
		return 0;
	return 1;
}

void plot_xy::resize(int x, int y, int w, int h)
{
	Fl_Widget::resize(x, y, w, h);
}
