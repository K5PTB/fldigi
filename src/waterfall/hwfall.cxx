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

#include <iostream>
#include <cstring>

#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Tooltip.H>

#include "hwfall.h"
#include "fl_digi.h"

hwfall::PALETTE		hwfall::palettes[28]	= { 
	{	"banana",	{ {   0,   0,   0 }, {  59,  59,  27 }, { 119, 119,  59 }, { 179, 179,  91 }, { 227, 227, 123 }, { 235, 235, 151 }, { 239, 239, 183 }, { 247, 247, 219 }, { 255, 255, 255 } } },
	{	"blue1",	{ {   0,   0,   0 }, {   0,   0,  64 }, {   7,  11, 128 }, {  39,  47, 192 }, {  95, 115, 217 }, { 151, 179, 231 }, { 187, 203, 239 }, { 219, 227, 247 }, { 255, 255, 255 } } },
	{	"blue2",	{ {   0,   0,   0 }, {   7,  11, 128 }, {  39,  47, 192 }, {  95, 115, 217 }, { 151, 179, 231 }, { 187, 203, 239 }, { 219, 227, 247 }, { 255, 255, 255 }, { 255, 253, 108 } } },
	{	"blue3",	{ {   0,   0,   0 }, {  31,  31,  31 }, {  63,  63,  63 }, {  91,  91, 167 }, { 119, 119, 191 }, { 155, 155, 219 }, { 191, 191, 191 }, { 223, 223, 223 }, { 255, 255, 255 } } },
	{	"brown",	{ {   0,   0,   0 }, { 107,  63,  11 }, { 175,  95,  31 }, { 199, 119,  43 }, { 215, 163,  63 }, { 231, 211,  87 }, { 243, 247, 111 }, { 247, 251, 179 }, { 255, 255, 255 } } },
	{	"cyan1",	{ {   0,   0,   0 }, {   5,  10,  10 }, {  22,  42,  42 }, {  52,  99,  99 }, {  94, 175, 175 }, { 131, 209, 209 }, { 162, 224, 224 }, { 202, 239, 239 }, { 255, 255, 255 } } },
	{	"cyan2",	{ {   0,   0,   0 }, {  35,  51,  51 }, {  75, 103, 103 }, { 115, 159, 159 }, { 155, 211, 211 }, { 183, 231, 231 }, { 203, 239, 239 }, { 227, 247, 247 }, { 255, 255, 255 } } },
	{	"cyan3",	{ {   0,   0,   0 }, {  94, 114, 114 }, { 138, 162, 162 }, { 171, 201, 201 }, { 199, 232, 232 }, { 216, 243, 243 }, { 228, 247, 247 }, { 241, 251, 251 }, { 255, 255, 255 } } },
	{	"default",	{ {   0,   0,   0 }, {   0,   6, 136 }, {   0,  19, 198 }, {   0,  32, 239 }, { 172, 167, 105 }, { 194, 198,  49 }, { 225, 228, 107 }, { 255, 255,   0 }, { 255,  51,   0 } } },
	{	"digipan",	{ {   0,   0,   0 }, {   0,   0,  64 }, {   0,   0, 128 }, {   0,   0, 217 }, { 150, 147,  92 }, { 183, 186,  46 }, { 225, 228, 107 }, { 255, 255,   0 }, { 255,  51,   0 } } },
	{	"fldigi",	{ {   0,   0,   0 }, {   0,   0, 177 }, {   3, 110, 227 }, {   0, 204, 204 }, { 223, 223, 223 }, {   0, 234,   0 }, { 244, 244,   0 }, { 250, 126,   0 }, { 244,   0,   0 } } },
	{	"gmfsk",	{ {   0,   0,   0 }, {   0,  62, 194 }, {   0, 126, 130 }, {   0, 190,  66 }, {   0, 254,   2 }, {  62, 194,   0 }, { 126, 130,   0 }, { 190,  66,   0 }, { 254,   2,   0 } } },
	{	"gray1",	{ {   0,   0,   0 }, {  69,  69,  69 }, {  99,  99,  99 }, { 121, 121, 121 }, { 140, 140, 140 }, { 157, 157, 157 }, { 172, 172, 172 }, { 186, 186, 186 }, { 199, 199, 199 } } },
	{	"gray2",	{ {   0,   0,   0 }, {  88,  88,  88 }, { 126, 126, 126 }, { 155, 155, 155 }, { 179, 179, 179 }, { 200, 200, 200 }, { 220, 220, 220 }, { 237, 237, 237 }, { 254, 254, 254 } } },
	{	"green1",	{ {   0,   0,   0 }, {   0,  32,   0 }, {   0,  64,   0 }, {   0,  96,   0 }, {   0, 128,   0 }, {   0, 160,   0 }, {   0, 192,   0 }, {   0, 224,   0 }, { 255, 255, 255 } } },
	{	"green2",	{ {   0,   0,   0 }, {   0,  60,   0 }, {   0, 102,   0 }, {   0, 151,   0 }, {   0, 242,   0 }, { 255, 255,  89 }, { 240, 120,   0 }, { 255, 148,  40 }, { 255,   0,   0 } } },
	{	"jungle",	{ {   0,   0,   0 }, { 107,  67,   0 }, { 223, 143,   0 }, { 255, 123,  27 }, { 255,  91,  71 }, { 255, 195,  95 }, { 195, 255, 111 }, { 151, 255, 151 }, { 255, 255, 255 } } },
	{	"negative",	{ {   0,   0,   0 }, { 223, 223, 223 }, { 191, 191, 191 }, { 159, 159, 159 }, { 127, 127, 127 }, {  95,  95,  95 }, {  63,  63,  63 }, {  31,  31,  31 }, {   0,   0,   0 } } },
	{	"orange",	{ {   0,   0,   0 }, {  63,  27,   0 }, { 131,  63,   0 }, { 199,  95,   0 }, { 251, 127,  11 }, { 251, 155,  71 }, { 251, 187, 131 }, { 251, 219, 191 }, { 255, 255, 255 } } },
	{	"pink",		{ {   0,   0,   0 }, {  63,   3,  35 }, { 135,  75,  75 }, { 203, 111, 111 }, { 255, 147, 147 }, { 255, 175, 175 }, { 255, 199, 199 }, { 255, 227, 227 }, { 255, 255, 255 } } },
	{	"qrss",		{ {   0,   0,   0 }, {   0,   0,  32 }, {   0,   0,  64 }, {   0,   0, 128 }, {  64,  64,  64 }, { 128, 128, 128 }, { 255, 255,   0 }, { 255, 255,   0 }, { 255, 255,   0 } } },
	{	"rainbow",	{ {   0,   0,   0 }, {   0,  87, 191 }, {   0, 207, 219 }, {   0, 247, 139 }, {   0, 255,  23 }, {  95, 255,   0 }, { 219, 255,   0 }, { 255, 171, 155 }, { 255, 255, 255 } } },
	{	"scope",	{ {   0,   0,   0 }, {   0,   0, 167 }, {   0,  79, 255 }, {   0, 239, 255 }, {   0, 255,  75 }, {  95, 255,   0 }, { 255, 255,   0 }, { 255, 127,   0 }, { 255,   0,   0 } } },
	{	"sunburst",	{ {   0,   0,   0 }, {   0,   0,  59 }, {   0,   0, 123 }, { 131,   0, 179 }, { 235,   0,  75 }, { 255,  43,  43 }, { 255, 215, 111 }, { 255, 255, 183 }, { 255, 255, 255 } } },
	{	"vk4bdj",	{ {   0,   0,   0 }, {   0,  32,   0 }, {   0, 154,   0 }, {   0, 161,   0 }, {   0, 177,   0 }, { 156, 209, 144 }, { 192, 185, 183 }, { 214, 222, 224 }, { 255, 255, 255 } } },
	{	"yellow1",	{ {   0,   0,   0 }, {  31,  31,   0 }, {  63,  63,   0 }, {  95,  95,   0 }, { 127, 127,   0 }, { 159, 159,   0 }, { 191, 191,   0 }, { 223, 223,   0 }, { 255, 255,   0 } } },
	{	"yellow2",	{ {   0,   0,   0 }, {  39,  39,   0 }, {  75,  75,   0 }, { 111, 111,   0 }, { 147, 147,   0 }, { 183, 183,   0 }, { 219, 219,   0 }, { 255, 255,   0 }, { 255, 255, 255 } } },
	{	"yl2kf",	{ {   0,   0,   0 }, {   0,   0, 119 }, {   7,  11, 195 }, {  39,  47, 159 }, {  95, 115, 203 }, { 151, 179, 255 }, { 187, 203, 255 }, { 219, 227, 255 }, { 255, 255,   5 } } }
	};

hwfall::hwfall(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H), img_w(W), img_h(H) {
	data = new unsigned char[img_w * img_h];
	image = new RGB[img_w * img_h];
	setcolors(8);
	USB = true;
	fmin = 7025000.00;
	miny = 200;
	scaley = 1;
	graticule_on = 1;
	clear();
}

void hwfall::clear()
{
	memset( data, 0, img_w * img_h );
	memset( image, 0, img_w * img_h * 3);
	graticules();
	redraw();
}

void hwfall::graticules()
{
	if (!graticule_on) return;

	RGB color = RGBmedgrey;
	for (int row = 100; row < img_h; row += 100) {
		for (int col = 0; col < img_w; col++ ) {
			image[ row * img_w + col ] = color;
		}
	}

	for (int col = 60; col < img_w; col += 60) {
		for (int row = 0; row < img_h; row++) {
			image[ row * img_w + col ] = color;
		}
	}
}

void hwfall::add_slice(const std::vector<double>& new_slice) 
{
	double val = 0;
	// shift raw data right
	memmove(data + 1, data, img_w * img_h - 1);

	// add new data slice
	for (int y = 0; y < img_h; ++y)  {
		val = new_slice[y];
		if (val < 0) val = 0;
		if (val > 1) val = 1;
		data[y * img_w] = 255 * new_slice[y];
	}

	redraw();

}

// Draw the image to the screen
void hwfall::draw() {
	// transfer to image
	for (int col = 0; col < img_w; col++)
		for (int row = 0; row < img_h; row++)
			image[ row * img_w + img_w - 1 - col] = db_RGB[data[row * img_w + col]];

	graticules();

	fl_draw_image(reinterpret_cast<unsigned char *>(image), x(), y(), img_w, img_h, 3, 0);
}

void hwfall::setcolors(int n) {
	if (n < 0) n = 0;
	if (n > 26) n = 26;
	PALETTE *pal = &palettes[n];

	for (int k = 0; k < 8; k++) {
		for (int i = 0; i < 32; i++) {
			db_RGB[i + 32*k].R = pal->rgb[k].R + (int)(1.0 * i * (pal->rgb[k+1].R - pal->rgb[k].R) / 32.0);
			db_RGB[i + 32*k].G = pal->rgb[k].G + (int)(1.0 * i * (pal->rgb[k+1].G - pal->rgb[k].G) / 32.0);
			db_RGB[i + 32*k].B = pal->rgb[k].B + (int)(1.0 * i * (pal->rgb[k+1].B - pal->rgb[k].B) / 32.0);
		}
	}
}

void cb_cmenu(Fl_Widget *w, void *d) {
//tbd
}

static Fl_Menu_Item cmenu[] = {
	{ "NNNNN.NNN", 0, (Fl_Callback*)cb_cmenu, 0, 0, FL_NORMAL_LABEL },
	{0,0,0,0,0,0,0,0,0},
};

void hwfall::handle_leftclick( int x1, int y1)
{
	int freq = miny + (img_h - y1) * scaley;
	if (!USB) freq *= -1;
	freq += fmin;

std::cout << 
"miny:  " << miny << std::endl <<
"img_h: " << img_h << std::endl <<
"y1:    " << y1 << std::endl <<
"scale: " << scaley << std::endl <<
"fmin:  " << fmin << std::endl <<
"miny + (img_h - y1) * scale:" << miny + (img_h - y1) * scaley << std::endl <<
"freq:  " << freq << std::endl;

	char szfreq[20];
	if (freq < 10000)
		snprintf(szfreq, sizeof(szfreq), "%d", freq);
	else
		snprintf(szfreq, sizeof(szfreq), "%0.3f", freq * 1e-3);
	cmenu[0].label(szfreq);
	int t = Fl_Tooltip::enabled();
	Fl_Tooltip::disable();
	cmenu->popup(Fl::event_x() + 20, Fl::event_y() + 20);
	Fl_Tooltip::enable(t);
}


void hwfall::handle_rightclick( int x, int y)
{
	fl_cursor(FL_CURSOR_CROSS);
}


int hwfall::handle(int event)
{
	if (event == FL_ENTER) {
		fl_cursor(FL_CURSOR_CROSS);
		tooltips.enabled = Fl_Tooltip::enabled();
		tooltips.delay = Fl_Tooltip::delay();
		Fl_Tooltip::enable(1);
		Fl_Tooltip::delay(1.0f);
#if FLDIGI_FLTK_API_MINOR >= 4
		tooltips.hide_delay = Fl_Tooltip::hidedelay();
		Fl_Tooltip::hidedelay(3.0f);
#endif
		return 1;
	}
	if (event == FL_LEAVE) {
		fl_cursor(FL_CURSOR_DEFAULT);
		Fl_Tooltip::enable(tooltips.enabled);
		Fl_Tooltip::delay(tooltips.delay);
#if FLDIGI_FLTK_API_MINOR >= 4
		Fl_Tooltip::hidedelay(tooltips.hide_delay);
#endif
		return 1;
	}

	if (!Fl::event_inside(this))
		return 0;

	switch (event) {
		case FL_PUSH :
			break;
		case FL_RELEASE :
			if (!Fl::event_inside(this))
				break;
			switch (Fl::event_button()) {
				case FL_LEFT_MOUSE:
					handle_leftclick(Fl::event_x() - x(), Fl::event_y() - y());
					break;
				case FL_RIGHT_MOUSE :
					handle_rightclick(Fl::event_x() - x(), Fl::event_y() - y());
					break;
				default :
					break;
			}
		default :
			break;
	}
	return 1;
}
