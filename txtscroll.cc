/*
 *  Copyright (C) 2000-2022  The Exult Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <cstring>
#include <cstdlib>

#include "common_types.h"
#include "files/U7file.h"
#include "gamewin.h"
#include "shapeid.h"
#include "txtscroll.h"
#include "font.h"
#include "game.h"

#ifdef __GNUC__
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wold-style-cast"
#endif    // __GNUC__
#include <SDL.h>
#ifdef __GNUC__
#	pragma GCC diagnostic pop
#endif    // __GNUC__

using std::atoi;
using std::size_t;
using std::strchr;
using std::string;
using std::strlen;
using std::strncmp;
using std::unique_ptr;
using std::make_unique;
using std::vector;

TextScroller::TextScroller(const char *archive, int index, Font *fnt, Shape *shp) {
	font = fnt;
	shapes = shp;
	unique_ptr<U7object> txtobj;
	// Hack to patch MAINSHP_FLX.
	if (!strncmp(archive, MAINSHP_FLX, sizeof(MAINSHP_FLX) - 1))
		txtobj = make_unique<U7multiobject>(archive, PATCH_MAINSHP, index);
	else
		txtobj = make_unique<U7object>(archive, index);
	size_t len;
	const char  CR = '\r';
	const char  LF = '\n';

	unique_ptr<unsigned char[]> txt = txtobj->retrieve(len);
	if (!txt || len <= 0) {
		text = new vector<string>();
		return;
	}
	char *ptr = reinterpret_cast<char*>(txt.get());
	char *end = ptr + len;

	text = new vector<string>();
	while (ptr < end) {
		char *start = ptr;
		ptr = strchr(ptr, LF);
		if (ptr) {
			if (*(ptr - 1) == CR) // It's CR/LF
				*(ptr - 1) = 0;
			else
				*ptr = 0;
			text->push_back(string(start));
			ptr += 1;
		} else
			break;
	}
}

TextScroller::~TextScroller() {
	delete text;
}

int TextScroller::show_line(Game_window *gwin, int left, int right, int y, int index) {
	Shape_manager *sman = Shape_manager::get_instance();

	//The texts used in the main menu contains backslashed sequences that
	//indicates the output format of the lines:
	// \Px   include picture number x (frame nr. of shape passed to constructor)
	// \C    center line
	// \L    left aligned to right center line
	// \R    right aligned to left center line
	// |     carriage return (stay on same line)
	// #xxx  display character with number xxx
	string str = (*text)[index];
	const char *ptr = str.c_str();
	char *txt = new char[strlen(ptr) + 1];

	char *txtptr = txt;
	int ypos = y;
	int vspace = 2; // 2 extra pixels between lines
	// Align text to the left by default
	int align = -1;
	int xpos = left;
	int center = (right + left) / 2;
	bool add_line = true;

	while (*ptr) {
		if (!strncmp(ptr, "\\P", 2)) {
			int pix = *(ptr + 2) - '0';
			ptr += 3;
			Shape_frame *frame = shapes->get_frame(pix);
			if (frame) {
				sman->paint_shape(center - frame->get_width() / 2,
				                  ypos, frame);
				ypos += frame->get_height() + vspace;
			}
		} else if (!strncmp(ptr, "\\C", 2)) {
			ptr += 2;
			align = 0;
		} else if (!strncmp(ptr, "\\L", 2)) {
			ptr += 2;
			align = 1;
		} else if (!strncmp(ptr, "\\R", 2)) {
			ptr += 2;
			align = -1;
		} else if (*ptr == '|' || *(ptr + 1) == 0) {
			if (*(ptr + 1) == 0 && *ptr != '|') {
				*txtptr++ = *ptr;
				add_line = false;
			}
			*txtptr = 0;

			if (align < 0)
				xpos = center - font->get_text_width(txt);
			else if (align == 0)
				xpos = center - font->get_text_width(txt) / 2;
			else
				xpos = center;
			font->draw_text(gwin->get_win()->get_ib8(),
			                xpos, ypos, txt);
			if (*ptr != '|') ypos += font->get_text_height() + vspace;
			txtptr = txt;   // Go to beginning of string
			++ptr;
		} else if (*ptr == '#') {
			ptr++;
			if (*ptr == '#') { // Double hash
				*txtptr++ = *ptr++;
				continue;
			}
			char numerical[4] = {0, 0, 0, 0};
			char *num = numerical;
			while (std::isdigit(static_cast<unsigned char>(*ptr)))
				*num++ = *ptr++;
			*txtptr++ = atoi(numerical);
		} else
			*txtptr++ = *ptr++;
	}

	delete [] txt;
	if (add_line)
		ypos += font->get_text_height();
	return ypos;
}


bool TextScroller::run(Game_window *gwin) {
	gwin->clear_screen();
	gwin->show(true);

	int topx = (gwin->get_width() - 320) / 2;
	int topy = (gwin->get_height() - 200) / 2;
	int endy = topy + 200;
	uint32 starty = endy;
	uint32 startline = 0;
	unsigned int maxlines = text->size();
	if (!maxlines) {
		gwin->clear_screen();
		gwin->show(true);
		return false;
	}
	bool looping = true;
	bool complete = false;
	SDL_Event event;
	uint32 next_time = SDL_GetTicks() + 200;
	uint32 incr = 120;
	//  pal.apply();
	gwin->get_pal()->apply();

	while (looping) {
		int ypos = starty;
		uint32 curline = startline;
		gwin->clear_screen();
		do {
			if (curline == maxlines)
				break;
			ypos = show_line(gwin, topx, topx + 320, ypos, curline++);
			if (ypos < topy) {  // If this line doesn't appear, don't show it next time
				++startline;
				starty = ypos;
				if (startline >= maxlines) {
					looping = false;
					complete = true;
					break;
				}
			}
		} while (ypos < endy);
//		pal.apply();
		gwin->show(true);
		do {
			// this could be a problem when too many events are produced
			while (SDL_PollEvent(&event)) {
				switch (event.type) {
				case SDL_KEYDOWN:
					if (event.key.keysym.sym == SDLK_RSHIFT || event.key.keysym.sym == SDLK_LSHIFT)
						incr = 0;
					else
						looping = false;
					break;

				case SDL_KEYUP:
					incr = 120;
					next_time = SDL_GetTicks();
					break;
				case SDL_MOUSEBUTTONUP:
					looping = false;
					break;
				default:
					break;
				}
			}
		} while (next_time > SDL_GetTicks());
		next_time = SDL_GetTicks() + incr;
		if (!looping)
			gwin->get_pal()->fade_out(c_fade_out_time);
		starty--;
	}
	gwin->clear_screen();
	gwin->show(true);
	return complete;
}

int TextScroller::get_count() {
	return text->size();
}
