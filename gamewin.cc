/**
 ** Gamewin.cc - X-windows Ultima7 map browser.
 **
 ** Written: 7/22/98 - JSF
 **/

/*
 *
 *  Copyright (C) 1998-1999  Jeffrey S. Freedman
 *  Copyright (C) 2000-2013  The Exult Team
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

#ifndef ALPHA_LINUX_CXX
#  include <cstdlib>
#  include <cstring>
#  include <cstdarg>
#  include <cstdio>
#endif

#include "Astar.h"
#include "Audio.h"
#include "Configuration.h"
#include "chunks.h"
#include "gamemap.h"
#include "Face_stats.h"
#include "Flex.h"
#include "Gump.h"
#include "Gump_manager.h"
#include "actions.h"
#include "monsters.h"
#include "animate.h"
#include "barge.h"
#include "cheat.h"
#include "chunkter.h"
#include "combat_opts.h"
#include "delobjs.h"
#include "dir.h"
#include "effects.h"
#include "egg.h"
#include "exult.h"
#include "files/U7file.h"
#include "flic/playfli.h"
#include "fnames.h"
#include "game.h"
#include "gamewin.h"
#include "gameclk.h"
#include "gamerend.h"
#include "items.h"
//#include "jawbone.h" // CHECKME: this doesn't seem to be needed
#include "keys.h"
#include "mouse.h"
#include "npcnear.h"
#include "objiter.h"
#include "paths.h"
#include "schedule.h"
#include "spellbook.h"
#include "ucmachine.h"
#include "ucsched.h"            /* Only used to flush objects. */
#include "utils.h"
#include "virstone.h"
#include "mappatch.h"
#include "version.h"
#include "drag.h"
#include "glshape.h"
#include "party.h"
#include "Notebook_gump.h"
#include "AudioMixer.h"
#include "combat.h"
#include "keyactions.h"
#include "monstinf.h"
#include "usefuns.h"
#include "audio/midi_drivers/XMidiFile.h"

#ifdef USE_EXULTSTUDIO
#include "server.h"
#include "servemsg.h"
#endif

#ifndef UNDER_EMBEDDED_CE
using std::cerr;
using std::cout;
using std::endl;
using std::istream;
using std::ifstream;
using std::ios;
using std::memcpy;
using std::memset;
using std::ofstream;
using std::rand;
using std::strcmp;
using std::strcpy;
using std::string;
using std::strlen;
using std::srand;
using std::vector;
#endif

// THE game window:
Game_window *Game_window::game_window = 0;

/*
 *  Provide chirping birds.
 */
class Background_noise : public Time_sensitive {
	int repeats;            // Repeats in quick succession.
	int last_sound;         // # of last sound played.
	Game_window *gwin;
	enum Song_states {
	    Invalid,
	    Outside,
	    Dungeon,
	    Nighttime,
	    RainStorm,
	    SnowStorm,
	    DangerNear
	} laststate;            // Last state for SFX music tracks,
	// 1 outside, 2 dungeon, 3 nighttime, 4 rainstorm,
	// 5 snowstorm, 6 for danger nearby
public:
	Background_noise(Game_window *gw) : repeats(0), last_sound(-1),
		gwin(gw), laststate(Invalid) {
		gwin->get_tqueue()->add(5000, this, 0L);
	}
	virtual ~Background_noise() {
		gwin->get_tqueue()->remove(this);
	}
	virtual void handle_event(unsigned long curtime, long udata);
	static bool is_combat_music(int num) {
		// Lumping music 16 as if it were a combat music in order to simplify
		// the check.
		return (num >= Audio::game_music(9) && num <= Audio::game_music(12)) ||
		       (num >= Audio::game_music(15) && num <= Audio::game_music(18));
	}
};

/*
 *  Play background sound.
 */

void Background_noise::handle_event(
    unsigned long curtime,
    long udata
) {
#ifndef COLOURLESS_REALLY_HATES_THE_BG_SFX

	unsigned long delay = 8000;
	Song_states currentstate;
	int bghour = gwin->get_clock()->get_hour();
	int weather = gwin->get_effects()->get_weather();
	bool nighttime = bghour < 6 || bghour > 20;
	bool nearby_hostile = gwin->is_hostile_nearby();
	if (nearby_hostile && !gwin->in_combat())
		currentstate = DangerNear;
	else if (gwin->is_in_dungeon())
		currentstate = Dungeon;
	else if (weather == 2)
		currentstate = RainStorm;
	else if (weather == 1)
		currentstate = SnowStorm;
	else if (nighttime)
		currentstate = Nighttime;   //Night time
	else
		currentstate = Outside;

	MyMidiPlayer *player = Audio::get_ptr()->get_midi();
	// Lets allow this for Digital Muisc and MT32Emu only,
	// for MT32/FakeMT32 conversion as well.
	// if (player) { 
	//if (player && player->get_ogg_enabled()){
	if (player && (player->get_ogg_enabled() || player->is_mt32())) {
		delay = 1000;   //Quickly get back to this function check
		//We've got OGG so play the background SFX tracks

		int curr_track = player->get_current_track();

		// Testing. Original seems to allow crickets for all songs at night,
		// except when in a dungeon. Also, only do it sometimes.
		if (nighttime && currentstate != Dungeon && rand() % 6 == 0) {
			//Play the cricket sounds at night
			Audio::get_ptr()->play_sound_effect(Audio::game_sfx(61),
			                                    AUDIO_MAX_VOLUME - 30);
		}

		if ((curr_track == -1 || laststate != currentstate) &&
		        Audio::get_ptr()->is_music_enabled()) {
			// Don't override bee cave music with dungeon music.
			bool notbees = !GAME_BG || curr_track != 54;
			// ++++ TODO: Need to come up with a way to replace repeating songs
			// here, just so they don't loop forever.
			// Conditions: not playing music, playing a background music
			if (curr_track == -1 || gwin->is_bg_track(curr_track) ||
			        (((currentstate == Dungeon && notbees) ||
			          currentstate == DangerNear) && !is_combat_music(curr_track))) {
				//Not already playing music
				int tracknum = 255;

				//Get the relevant track number.
				if (nearby_hostile && !gwin->in_combat()) {
					tracknum = Audio::game_music(10);
					laststate = DangerNear;
				} else if (gwin->is_in_dungeon()) {
					//Start the SFX music track then
					tracknum = Audio::game_music(52);
					laststate = Dungeon;
				} else if (weather == 1) { // Snowstorm
					tracknum = Audio::game_music(5);
					laststate = SnowStorm;
				} else if (weather == 2) { // Rainstorm
					tracknum = Audio::game_music(4);
					laststate = RainStorm;
				} else if (bghour < 6 || bghour > 20) {
					tracknum = Audio::game_music(7);
					laststate = Nighttime;
				} else {
					//Start the SFX music track then
					tracknum = Audio::game_music(6);
					laststate = Outside;
				}
				Audio::get_ptr()->start_music(tracknum, true);
			}
		}
	}else {
		Main_actor *ava = gwin->get_main_actor();
		//Tests to see if track is playing the SFX tracks, possible
		//when the game has been restored
		//and the Audio option was changed from OGG to something else
		if (player && player->get_current_track() >= Audio::game_music(4) &&
		        player->get_current_track() <= Audio::game_music(8))
			player->stop_music();

		//Not OGG so play the SFX sounds manually
		// Only if outside.
		if (ava && !gwin->is_main_actor_inside()  &&
		        // +++++SI SFX's don't sound right.
		        Game::get_game_type() == BLACK_GATE) {
			int sound;      // BG SFX #.
			static unsigned char bgnight[] = {61, 61, 255},
			                                 bgday[] = {82, 85, 85};
			if (repeats > 0)    // Repeating?
				sound = last_sound;
			else {
				int hour = gwin->get_clock()->get_hour();
				if (hour < 6 || hour > 20)
					sound = bgnight[rand() % sizeof(bgnight)];
				else
					sound = bgday[rand() % sizeof(bgday)];
				// Translate BG to SI #'s.
				sound = Audio::game_sfx(sound);
				last_sound = sound;
			}
			Audio::get_ptr()->play_sound_effect(sound);
			repeats++;      // Count it.
			if (rand() % (repeats + 1) == 0)
				// Repeat.
				delay = 500 + rand() % 1000;
			else {
				delay = 4000 + rand() % 3000;
				repeats = 0;
			}
		}
	}

	gwin->get_tqueue()->add(curtime + delay, this, udata);
#endif
}

/*
 *  Set renderer (OpenGL or normal SDL).
 */

void Set_renderer(
    Image_window8 *win,
    Palette *pal,
    bool resize
) {
	GL_manager *glman = GL_manager::get_instance();
#ifdef HAVE_OPENGL
	delete glman;
	glman = 0;
	if (win->get_scaler() == Image_window::OpenGL) {
		glman = new GL_manager();
		glman->set_palette(pal);
		if (resize)
			glman->resized(win->get_full_width(), win->get_full_height(),
			               win->get_scale_factor());
	}
#endif
	// Tell shapes how to render.
	Shape_frame::set_to_render(win->get_ib8(), glman);
}

#ifdef HAVE_OPENGL
/*
 *  Set palette and reset all textures. If given null palette, uses current
 *  game window palette.
 */

void GL_manager::set_palette(Palette *pal, bool rotation) {
	Chunk_terrain::clear_glflats(rotation);
	if (rotation) {
		// Free only those that rotate.
		GL_texshape *next = shapes;
		while (next) {
			GL_texshape *tex = next;
			// Point to next element to be safe.
			next = next->lru_next;
			if (tex->has_palette_rotation()) {
				// Unlink.
				if (shapes == tex)
					shapes = next;
				if (tex->lru_next)
					tex->lru_next->lru_prev = tex->lru_prev;
				if (tex->lru_prev)
					tex->lru_prev->lru_next = tex->lru_next;
				delete tex;
			}
		}
	} else  // Kill them all.
		while (shapes) {
			GL_texshape *next = shapes->lru_next;
			delete shapes;
			shapes = next;
		}
	if (!palette)
		palette = new unsigned char[768];
	if (pal) {
		for (int i = 0; i < 256; i++) {
			// Palette colors are 0-63.
			palette[3 * i] = 4 * pal->get_red(i);
			palette[3 * i + 1] = 4 * pal->get_green(i);
			palette[3 * i + 2] = 4 * pal->get_blue(i);
		}
	} else {
		const unsigned char *cpal =
		    Game_window::get_instance()->get_win()->get_palette();
		std::memcpy(palette, cpal, 768);
	}
}
#endif

/*
 *  Set palette and reset all textures. If given null palette, uses current
 *  game window palette.
 */

bool Set_glpalette(Palette *pal, bool rotation) {
#ifdef HAVE_OPENGL
	if (GL_manager::get_instance()) {
		GL_manager::get_instance()->set_palette(pal, rotation);
		return true;
	}
#endif
	return false;
}

/*
 *  Create game window.
 */

Game_window::Game_window(
    int width, int height, bool fullscreen, int gwidth, int gheight, int scale, int scaler, Image_window::FillMode fillmode, unsigned int fillsclr      // Window dimensions.
) :
	dragging(0), effects(new Effects_manager(this)), map(new Game_map(0)),
	render(new Game_render), gump_man(new Gump_manager),
	party_man(new Party_manager), win(0),
	npc_prox(new Npc_proximity_handler(this)), pal(0),
	tqueue(new Time_queue()), background_noise(new Background_noise(this)),
	usecode(0), combat(false), focus(true), ice_dungeon(false),
	painted(false), ambient_light(false),
	skip_above_actor(31), in_dungeon(0),
	std_delay(c_std_delay), time_stopped(0), special_light(0),
	theft_warnings(0), theft_cx(255), theft_cy(255),
	moving_barge(0), main_actor(0), camera_actor(0), npcs(0), bodies(0),
	removed(new Deleted_objects()), dirty(0, 0, 0, 0),
	mouse3rd(false), fastmouse(false), double_click_closes_gumps(false),
	text_bg(false), step_tile_delta(8), allow_right_pathfind(2),
#ifdef RED_PLASMA
	load_palette_timer(0), plasma_start_color(0), plasma_cycle_range(0),
#endif
	skip_lift(255), paint_eggs(false), armageddon(false),
	walk_in_formation(false), debug(0), blits(0),
	scrolltx_l(0), scrollty_l(0), scrolltx_lp(0), scrollty_lp(0),
	scrolltx_lo(0), scrollty_lo(0), avposx_ld(0), avposy_ld(0),
	lerping_enabled(0) {
	memset(save_names, 0, sizeof(save_names));
	game_window = this;     // Set static ->.
	clock = new Game_clock(tqueue);
	shape_man = new Shape_manager();// Create the single instance.
	maps.push_back(map);        // Map #0.
	// Create window.
	win = new Image_window8(width, height, gwidth, gheight, scale, fullscreen, scaler, fillmode, fillsclr);
	win->set_title("Exult Ultima7 Engine");
	pal = new Palette();
	Game_singletons::init(this);    // Everything but 'usecode' exists.
	Set_renderer(win, pal, true);

	string str;
	config->value("config/gameplay/textbackground", text_bg, -1);
	config->value("config/gameplay/mouse3rd", str, "no");
	if (str == "yes")
		mouse3rd = true;
	config->set("config/gameplay/mouse3rd", str, false);
	config->value("config/gameplay/fastmouse", str, "no");
	if (str == "yes")
		fastmouse = true;
	config->set("config/gameplay/fastmouse", str, false);
	config->value("config/gameplay/double_click_closes_gumps", str, "no");
	if (str == "yes")
		double_click_closes_gumps = true;
	config->set("config/gameplay/double_click_closes_gumps", str, false);
	config->value("config/gameplay/combat/difficulty",
	              Combat::difficulty, 0);
	config->set("config/gameplay/combat/difficulty",
	            Combat::difficulty, false);
	config->value("config/gameplay/combat/charmDifficulty", str, "normal");
	Combat::charmed_more_difficult = (str == "hard" ? true : false);
	config->set("config/gameplay/combat/charmDifficulty", str, false);
	config->value("config/gameplay/combat/mode", str, "original");
	if (str == "keypause")
		Combat::mode = Combat::keypause;
	else
		Combat::mode = Combat::original;
	config->set("config/gameplay/combat/mode", str, false);
	config->value("config/gameplay/combat/show_hits", str, "no");
	Combat::show_hits = (str == "yes");
	config->set("config/gameplay/combat/show_hits", str, false);
	config->value("config/audio/disablepause", str, "no");
	config->set("config/audio/disablepause", str, false);

	config->value("config/gameplay/step_tile_delta", step_tile_delta, 8);
	if (step_tile_delta < 1) step_tile_delta = 1;
	config->set("config/gameplay/step_tile_delta", step_tile_delta, false);

	config->value("config/gameplay/allow_right_pathfind", str, "double");
	if (str == "no")
		allow_right_pathfind = 0;
	else if (str == "single")
		allow_right_pathfind = 1;
	config->set("config/gameplay/allow_right_pathfind", str, false);

	// New 'formation' walking?
	config->value("config/gameplay/formation", str, "yes");
	// Assume "yes" on anything but "no".
	walk_in_formation = str != "no";
	config->set("config/gameplay/formation", walk_in_formation ? "yes" : "no",
	            false);

	config->value("config/gameplay/smooth_scrolling", lerping_enabled, 0);
	config->set("config/gameplay/smooth_scrolling", lerping_enabled, false);
	config->value("config/gameplay/alternate_drop", str, "no");
	alternate_drop = str == "yes";
	config->set("config/gameplay/alternate_drop", alternate_drop ? "yes" : "no", false);
	config->value("config/gameplay/allow_autonotes", str, "no");
	allow_autonotes = str == "yes";
	config->set("config/gameplay/allow_autonotes", allow_autonotes ? "yes" : "no", false);
	config->value("config/gameplay/scroll_with_mouse", str, "no");
	scroll_with_mouse = str == "yes";
	config->set("config/gameplay/scroll_with_mouse",
	            scroll_with_mouse ? "yes" : "no", false);
	// ShortcutBar
#ifdef __IPHONEOS__
	config->value("config/shortcutbar/use_shortcutbar", str, "translucent");
#else
	config->value("config/shortcutbar/use_shortcutbar", str, "no");
#endif
	if(str == "no") {
		use_shortcutbar = 0;
	} else if(str == "yes") {
		use_shortcutbar = 2;
	} else {
		str = "translucent";
		use_shortcutbar = 1;
	}
	config->set("config/shortcutbar/use_shortcutbar", str, false);

#ifdef __IPHONEOS__
	config->value("config/shortcutbar/use_outline_color", str, "black");
#else
	config->value("config/shortcutbar/use_outline_color", str, "no");
#endif

	if(str == "no") {
		outline_color = NPIXCOLORS;
	} else if(str == "green") {
		outline_color = POISON_PIXEL;
	} else if(str == "white") {
		outline_color = PROTECT_PIXEL;
	} else if(str == "yellow") {
		outline_color = CURSED_PIXEL;
	} else if(str == "blue") {
		outline_color = CHARMED_PIXEL;
	} else if(str == "red") {
		outline_color = HIT_PIXEL;
	} else if(str == "purple") {
		outline_color = PARALYZE_PIXEL;
	} else {
		str = "black";
		outline_color = BLACK_PIXEL;
	}
	config->set("config/shortcutbar/hide_missing_items", str, false);

	config->value("config/shortcutbar/hide_missing_items", str, "yes");
	sb_hide_missing = str != "no";
	config->set("config/shortcutbar/hide_missing_items", sb_hide_missing ? "yes" : "no", false);
	config->write_back();
}

/*
 *  Blank out screen.
 */
void Game_window::clear_screen(bool update) {
	win->fill8(0, win->get_full_width(), win->get_full_height(), win->get_start_x(), win->get_start_y());

	// update screen
	if (update)
		show(1);
}



/*
 *  Deleting game window.
 */

Game_window::~Game_window(
) {
	gump_man->close_all_gumps(true);
	clear_world(false);         // Delete all objects, chunks.
	for (size_t i = 0; i < sizeof(save_names) / sizeof(save_names[0]); i++)
		delete [] save_names[i];
	delete shape_man;
	delete gump_man;
	delete party_man;
	delete background_noise;
	delete tqueue;
	delete win;
	delete dragging;
	delete pal;
	for (vector<Game_map *>::iterator it = maps.begin();
	        it != maps.end(); ++it)
		delete *it;
	delete usecode;
	delete removed;
	delete clock;
	delete npc_prox;
	delete effects;
	delete render;
}

/*
 *  Abort.
 */

void Game_window::abort(
    const char *msg,
    ...
) {
	std::va_list ap;
	va_start(ap, msg);
	char buf[512];
	vsprintf(buf, msg, ap);     // Format the message.
	cerr << "Exult (fatal): " << buf << endl;
	delete this;
	throw quit_exception(-1);
}

bool Game_window::is_bg_track(int num) { // ripped out of Background_noise
	// Have to do it this way because of SI.
	if (num == Audio::game_music(4) || num == Audio::game_music(5) ||
			num == Audio::game_music(6) || num == Audio::game_music(7) ||
			num == Audio::game_music(8) || num == Audio::game_music(52))
		return true;
	else
		return false;
}

#if 0
#define BLEND(alpha, newc, oldc) ((newc*255L) - (oldc*(255L-alpha)))/alpha
void Analyze_xform(unsigned char *xform, int alpha, Palette *pal) {
	long br = 0, bg = 0, bb = 0;    // Trying to figure out blend color.
	for (int i = 0; i < 256; i++) {
		int xi = xform[i];  // Index of color mapped to.
		br += BLEND(alpha, pal->get_red(xi), pal->get_red(i));
		bg += BLEND(alpha, pal->get_green(xi), pal->get_green(i));
		bb += BLEND(alpha, pal->get_blue(xi), pal->get_blue(i));
	}
	br /= 256;          // Take average.
	bg /= 256;
	bb /= 256;
	cout << "Blend (r,g,b) = " << br << ',' << bg << ',' << bb << endl;
}
#endif


void Game_window::init_files(bool cycle) {
#ifdef RED_PLASMA
	// Display red plasma during load...
	if (cycle)
		setup_load_palette();
#endif

	usecode = Usecode_machine::create();
	Game_singletons::init(this);    // Everything should exist here.

	CYCLE_RED_PLASMA();
	shape_man->load();      // All the .vga files!
	CYCLE_RED_PLASMA();

	unsigned long timer = SDL_GetTicks();
	srand(timer);           // Use time to seed rand. generator.
	// Force clock to start.
	tqueue->add(timer, clock, reinterpret_cast<long>(this));
	// Go to starting chunk
	scrolltx_lp = scrolltx_l = scrolltx = game->get_start_tile_x();
	scrollty_lp = scrollty_l = scrollty = game->get_start_tile_y();
	scrolltx_lo = scrollty_lo = 0;
	avposx_ld = avposy_ld = 0;

	// initialize keybinder
	delete keybinder;
	keybinder = new KeyBinder();

	std::string d, keyfilename;
	d = "config/disk/game/" + Game::get_gametitle() + "/keys";
	config->value(d.c_str(), keyfilename, "(default)");
	if (keyfilename == "(default)") {
		config->set(d.c_str(), keyfilename, true);
		keybinder->LoadDefaults();
	} else {
		try {
			keybinder->LoadFromFile(keyfilename.c_str());
		} catch (file_open_exception &err) {
			cerr << "Key mappings file '" << keyfilename << "' not found, falling back to default mappings." << endl;
			keybinder->LoadDefaults();
		}
	}
	keybinder->LoadFromPatch();
#ifdef UNDER_CE
	// Take care of special WinCE D-PAD Options
	int dpadopt;
	config->value("config/gameplay/dpadopt", dpadopt, 3);
	keybinder->WINCE_LoadFromDPADOPT(dpadopt);
#endif
	CYCLE_RED_PLASMA();

	int fps;            // Init. animation speed.
	config->value("config/video/fps", fps, 5);
	if (fps <= 0)
		fps = 5;
	std_delay = 1000 / fps;     // Convert to msecs. between frames.
}

/*
 *  Read any map. (This is for "multimap" games, not U7.)
 */

Game_map *Game_window::get_map(
    int num             // Should be > 0.
) {
	if (num >= static_cast<int>(maps.size()))
		maps.resize(num + 1);
	if (maps[num] == 0) {
		Game_map *newmap = new Game_map(num);
		maps[num] = newmap;
		maps[num]->init();
	}
	return maps[num];
}

/*
 *  Set current map to given #.
 */

void Game_window::set_map(
    int num
) {
	map = get_map(num);
	if (!map)
		abort("Map #d doesn't exist", num);
	Game_singletons::gmap = map;
}

/*
 *  Get map patch list.
 */
Map_patch_collection *Game_window::get_map_patches() {
	return map->get_map_patches();
}

/*
 *  Set/unset barge mode.
 */

void Game_window::set_moving_barge(
    Barge_object *b
) {
	if (b && b != moving_barge) {
		b->gather();        // Will 'gather' on next move.
		if (!b->contains(main_actor))
			b->set_to_gather();
	} else if (!b && moving_barge)
		moving_barge->done();   // No longer 'barging'.
	moving_barge = b;
}

/*
 *  Is character moving?
 */

bool Game_window::is_moving(
) {
	return moving_barge ? moving_barge->is_moving()
	       : main_actor->is_moving();
}

/*
 *  Are we in dont_move mode?
 */

bool Game_window::main_actor_dont_move() {
	return !cheat.in_map_editor() && main_actor != 0 && // Not if map-editing.
	       ((main_actor->get_flag(Obj_flags::dont_move) != 0) ||
	        (main_actor->get_flag(Obj_flags::dont_render) != 0));
}

/*
 *  Are we in dont_move mode?
 */

bool Game_window::main_actor_can_act() {
	return main_actor->can_act();
}

bool Game_window::main_actor_can_act_charmed() {
	return main_actor->can_act_charmed();
}

/*
 *  Add time for a light spell.
 */

void Game_window::add_special_light(
    int units           // Light=500, GreatLight=5000.
) {
	if (!special_light) {   // Nothing in effect now?
		special_light = clock->get_total_minutes();
		clock->set_palette();
	}
	special_light += units / 20; // Figure ending time.
}

/*
 *  Set 'stop time' value.
 */

void Game_window::set_time_stopped(
    long delay          // Delay in ticks (1/1000 secs.),
    // -1 to stop indefinitely, or 0
    // to end.
) {
	if (delay == -1)
		time_stopped = -1;
	else if (!delay)
		time_stopped = 0;
	else {
		long new_expire = Game::get_ticks() + delay;
		if (new_expire > time_stopped)  // Set expiration time.
			time_stopped = new_expire;
	}
}

/*
 *  Return delay to expiration (or 3000 if indefinite).
 */

long Game_window::check_time_stopped(
) {
	if (time_stopped == -1)
		return 3000;
	long delay = time_stopped - Game::get_ticks();
	if (delay > 0)
		return delay;
	time_stopped = 0;       // Done.
	return 0;
}

/*
 *  Toggle combat mode.
 */

void Game_window::toggle_combat(
) {
	combat = !combat;
	// Change party member's schedules.
	int newsched = combat ? Schedule::combat : Schedule::follow_avatar;
	int cnt = party_man->get_count();
	for (int i = 0; i < cnt; i++) {
		int party_member = party_man->get_member(i);
		Actor *person = get_npc(party_member);
		if (!person)
			continue;
		if (!person->can_act_charmed())
			newsched = Schedule::combat;
		int sched = person->get_schedule_type();
		if (sched != newsched && sched != Schedule::wait &&
		        sched != Schedule::loiter)
			person->set_schedule_type(newsched);
	}
	if (!main_actor_can_act_charmed())
		newsched = Schedule::combat;
	if (main_actor->get_schedule_type() != newsched)
		main_actor->set_schedule_type(newsched);
	if (combat) {       // Get rid of flee modes.
		main_actor->ready_best_weapon();
		set_moving_barge(0);    // And get out of barge mode.
		Actor *all[9];
		int cnt = get_party(all, 1);
		for (int i = 0; i < cnt; i++) {
			// Did Usecode set to flee?
			Actor *act = all[i];
			if (act->get_attack_mode() == Actor::flee &&
			        !act->did_user_set_attack())
				act->set_attack_mode(Actor::nearest);
			// And avoid attacking party members,
			//  in case of Usecode bug.
			Game_object *targ = act->get_target();
			if (targ && targ->get_flag(Obj_flags::in_party))
				act->set_target(0);
		}
	} else              // Ending combat.
		Combat::resume();   // Make sure not still paused.
	if(g_shortcutBar)
		g_shortcutBar->set_changed();
}

/*
 *  Add an NPC.
 */

void Game_window::add_npc(
    Actor *npc,
    int num             // Number. Has to match npc->num.
) {
	assert(num == npc->get_npc_num());
	assert(num <= static_cast<int>(npcs.size()));
	if (num == static_cast<int>(npcs.size()))       // Add at end.
		npcs.push_back(npc);
	else {
		// Better be unused.
		assert(!npcs[num] || npcs[num]->is_unused());
		delete npcs[num];
		npcs[num] = npc;
	}
}

/*
 *  Show desired NPC.
 */

void Game_window::locate_npc(
    int npc_num
) {
	char msg[80];
	Actor *npc = get_npc(npc_num);
	if (!npc) {
		snprintf(msg, 80, "NPC %d does not exist.", npc_num);
		effects->center_text(msg);
	} else if (npc->is_pos_invalid()) {
		snprintf(msg, 80, "NPC %d is not on the map.", npc_num);
		effects->center_text(msg);
	} else {
		// ++++WHAT IF on a different map???
		Tile_coord pos = npc->get_tile();
		center_view(pos);
		cheat.clear_selected();
		cheat.append_selected(npc);
		snprintf(msg, 80, "NPC %d: '%s'.", npc_num,
		         npc->get_npc_name().c_str());
		int above = pos.tz + npc->get_info().get_3d_height() - 1;
		if (skip_above_actor > above)
			set_above_main_actor(above);
		effects->center_text(msg);
	}
}

/*
 *  Find first unused NPC #.
 */

int Game_window::get_unused_npc(
) {
	int cnt = npcs.size();      // Get # in list.
	int i = 0;
	for (; i < cnt; i++) {
		if (i >= 356 && i <= 359)
			continue;   // Never return these.
		if (!npcs[i] || npcs[i]->is_unused())
			break;
	}
	if (i >= 356 && i <= 359) {
		// Special, 'reserved' cases.
		i = 360;
		do {
			npcs.push_back(new Npc_actor("Reserved", 0));
			npcs[cnt]->set_schedule_type(Schedule::wait);
			npcs[cnt]->set_unused(true);
		} while (++cnt < 360);
	}
	return i;           // First free one is past the end.
}

/*
 *  Resize event occurred.
 */

void Game_window::resized(
    unsigned int neww,
    unsigned int newh,
    bool newfs,
    unsigned int newgw,
    unsigned int newgh,
    unsigned int newsc,
    unsigned int newsclr,
    Image_window::FillMode newfill,
    unsigned int newfillsclr
) {
	win->resized(neww, newh, newfs, newgw, newgh, newsc, newsclr, newfill, newfillsclr);
	pal->apply(false);
	Set_renderer(win, pal, true);
	if (!main_actor)        // In case we're before start.
		return;
	center_view(main_actor->get_tile());
	paint();
	// Do the following only if in game (not for menus)
	if (!gump_man->gump_mode()) {
		char msg[80];
		snprintf(msg, 80, "%ux%ux%u", neww, newh, newsc);
		effects->center_text(msg);
	}
	if(g_shortcutBar)
		g_shortcutBar->set_changed();
}

/*
 *  Clear out world's contents. Should be used during a 'restore'.
 */

void Game_window::clear_world(
    bool restoremapedit
) {
	bool edit = cheat.in_map_editor();
	cheat.set_map_editor(false);
	Combat::resume();
	tqueue->clear();        // Remove all entries.
	clear_dirty();
	removed->flush();       // Delete.
	Usecode_script::clear();    // Clear out all scheduled usecode.
	// Most NPCs were deleted when the map is cleared; we have to deal with some stragglers.
	for (vector<Actor *>::iterator it = npcs.begin();
	        it != npcs.end(); ++it)
		if ((*it)->is_unused())
			delete *it;
	for (vector<Game_map *>::iterator it = maps.begin();
	        it != maps.end(); ++it)
		(*it)->clear();
	set_map(0);         // Back to main map.
	Monster_actor::delete_all();    // To be safe, del. any still around.
	Notebook_gump::clear();
	main_actor = 0;
	camera_actor = 0;
	num_npcs1 = 0;
	theft_cx = theft_cy = -1;
	combat = 0;
	npcs.resize(0);         // NPC's already deleted above.
	bodies.resize(0);
	moving_barge = 0;       // Get out of barge mode.
	special_light = 0;      // Clear out light spells.
	ambient_light = false;  // And ambient lighting.
	effects->remove_all_effects(false);
	Schedule_change::clear();
	cheat.set_map_editor(edit && restoremapedit);
}

/*
 *  Look throughout the map for a given shape. The search starts at
 *  the first currently-selected shape, if possible.
 *
 *  Output: true if found, else 0.
 */

bool Game_window::locate_shape(
    int shapenum,           // Desired shape.
    bool upwards,           // If true, search upwards.
    int frnum,          // Frame.
    int qual            // Quality/quantity.
) {
	// Get (first) selected object.
	const std::vector<Game_object *> &sel = cheat.get_selected();
	Game_object *start = sel.size() ? sel[0] : 0;
	char msg[80];
	snprintf(msg, sizeof(msg), "Searching for shape %d", shapenum);
	effects->center_text(msg);
	paint();
	show();
	Game_object *obj = map->locate_shape(shapenum, upwards, start, frnum, qual);
	if (!obj) {
		effects->center_text("Not found");
		return false;       // Not found.
	}
	effects->remove_text_effects();
	cheat.clear_selected();     // Select obj.
	cheat.append_selected(obj);
	//++++++++Got to show it.
	Game_object *owner = obj->get_outermost(); //+++++TESTING
	if (owner != obj)
		cheat.append_selected(owner);
	center_view(owner->get_tile());
	return true;
}

/*
 *  Set location to ExultStudio.
 */

inline void Send_location(
    Game_window *gwin
) {
#ifdef USE_EXULTSTUDIO
	if (client_socket >= 0 &&   // Talking to ExultStudio?
	        cheat.in_map_editor()) {
		unsigned char data[50];
		unsigned char *ptr = &data[0];
		Write4(ptr, gwin->get_scrolltx());
		Write4(ptr, gwin->get_scrollty());
		Write4(ptr, gwin->get_width() / c_tilesize);
		Write4(ptr, gwin->get_height() / c_tilesize);
		Write4(ptr, gwin->get_win()->get_scale_factor());
		Exult_server::Send_data(client_socket, Exult_server::view_pos,
		                        &data[0], ptr - data);
	}
#endif
}

/*
 *  Send current location to ExultStudio.
 */

void Game_window::send_location(
) {
	Send_location(this);
}

/*
 *  Set the scroll position to a given tile.
 */

void Game_window::set_scrolls(
    int newscrolltx, int newscrollty
) {
	scrolltx = newscrolltx;
	scrollty = newscrollty;
	// Set scroll box.
	// Let's try 2x2 tiles.
	scroll_bounds.w = scroll_bounds.h = 2;
	scroll_bounds.x = scrolltx +
	                  (get_width() / c_tilesize - scroll_bounds.w) / 2;
	// OFFSET HERE
	scroll_bounds.y = scrollty +
	                  ((get_height()) / c_tilesize - scroll_bounds.h) / 2;

	Barge_object *old_active_barge = moving_barge;
	map->read_map_data();       // This pulls in objects.
	// Found active barge?
	if (!old_active_barge && moving_barge) {
		// Do it right.
		Barge_object *b = moving_barge;
		moving_barge = 0;
		set_moving_barge(b);
	}
	// Set where to skip rendering.
	int cx = camera_actor->get_cx(), cy = camera_actor->get_cy();
	Map_chunk *nlist = map->get_chunk(cx, cy);
	nlist->setup_cache();
	int tx = camera_actor->get_tx(), ty = camera_actor->get_ty();
	set_above_main_actor(nlist->is_roof(tx, ty,
	                                    camera_actor->get_lift()));
	set_in_dungeon(nlist->has_dungeon() ? nlist->is_dungeon(tx, ty) : 0);
	set_ice_dungeon(nlist->is_ice_dungeon(tx, ty));
	send_location();        // Tell ExultStudio.
}

/*
 *  Set the scroll position so that a given tile is centered. (Used by
 *  center_view.)
 */

void Game_window::set_scrolls(
    Tile_coord cent         // Want center here.
) {
	// Figure in tiles.
	// OFFSET HERE
	int tw = get_width() / c_tilesize, th = (get_height()) / c_tilesize;
	set_scrolls(DECR_TILE(cent.tx, tw / 2), DECR_TILE(cent.ty, th / 2));
}

/*
 *  Center view around a given tile. This is called during a 'restore'
 *  to init. stuff as well.
 */

void Game_window::center_view(
    Tile_coord const &t
) {
	set_scrolls(t);
	set_all_dirty();
}

/*
 *  Set actor to center view around.
 */

void Game_window::set_camera_actor(
    Actor *a
) {
	if (a == main_actor &&      // Setting back to main actor?
	        camera_actor &&     // Change in chunk?
	        (camera_actor->get_cx() != main_actor->get_cx() ||
	         camera_actor->get_cy() != main_actor->get_cy()))
		// Cache out temp. objects.
		emulate_cache(camera_actor->get_chunk(),
		              main_actor->get_chunk());
	camera_actor = a;
	Tile_coord t = a->get_tile();
	set_scrolls(t);         // Set scrolling around position,
	// and read in map there.
	set_all_dirty();
}

/*
 *  Scroll if necessary.
 *
 *  Output: 1 if scrolled (screen updated).
 */

bool Game_window::scroll_if_needed(
    Tile_coord t
) {
	bool scrolled = false;
	// 1 lift = 1/2 tile.
	int tx = t.tx - t.tz / 2, ty = t.ty - t.tz / 2;
	if (Tile_coord::gte(DECR_TILE(scroll_bounds.x), tx)) {
		view_left();
		scrolled = true;
	} else if (Tile_coord::gte(tx,
	                           (scroll_bounds.x + scroll_bounds.w) % c_num_tiles)) {
		view_right();
		scrolled = true;
	}
	if (Tile_coord::gte(DECR_TILE(scroll_bounds.y), ty)) {
		view_up();
		scrolled = true;
	} else if (Tile_coord::gte(ty,
	                           (scroll_bounds.y + scroll_bounds.h) % c_num_tiles)) {
		view_down();
		scrolled = true;
	}
	return (scrolled);
}

/*
 *  Show the absolute game location, where each coordinate is of the
 *  8x8 tiles clicked on.
 */

void Game_window::show_game_location(
    int x, int y            // Point on screen.
) {
	x = get_scrolltx() + x / c_tilesize;
	y = get_scrollty() + y / c_tilesize;
	cout << "Game location is (" << x << ", " << y << ")" << endl;
}

/*
 *  Get screen area used by object.
 */

Rectangle Game_window::get_shape_rect(Game_object *obj) {
	if (!obj->get_chunk()) {    // Not on map?
		Gump *gump = gump_man->find_gump(obj);
		if (gump)
			return gump->get_shape_rect(obj);
		else
			return Rectangle(0, 0, 0, 0);
	}
	Shape_frame *s = obj->get_shape();
	if (!s) {
		// This is probably fatal.
#if DEBUG
		std::cerr << "DEATH! get_shape() returned a NULL pointer: " << __FILE__ << ":" << __LINE__ << std::endl;
		std::cerr << "Betcha it's a little doggie." << std::endl;
#endif
		return Rectangle(0, 0, 0, 0);
	}
	Tile_coord t = obj->get_tile(); // Get tile coords.
	int lftpix = (c_tilesize * t.tz) / 2;
	t.tx += 1 - get_scrolltx();
	t.ty += 1 - get_scrollty();
	// Watch for wrapping.
	if (t.tx < -c_num_tiles / 2)
		t.tx += c_num_tiles;
	if (t.ty < -c_num_tiles / 2)
		t.ty += c_num_tiles;
	return get_shape_rect(s,
	                      t.tx * c_tilesize - 1 - lftpix,
	                      t.ty * c_tilesize - 1 - lftpix);
}

/*
 *  Get screen location of given tile.
 */

inline void Get_shape_location(
    Tile_coord t,
    int scrolltx, int scrollty,
    int &x, int &y
) {
	int lft = 4 * t.tz;
	t.tx += 1 - scrolltx;
	t.ty += 1 - scrollty;
	// Watch for wrapping.
	if (t.tx < -c_num_tiles / 2)
		t.tx += c_num_tiles;
	if (t.ty < -c_num_tiles / 2)
		t.ty += c_num_tiles;
	x = t.tx * c_tilesize - 1 - lft;
	y = t.ty * c_tilesize - 1 - lft;
}

/*
 *  Get screen loc. of object which MUST be on the map (no owner).
 */

void Game_window::get_shape_location(Game_object *obj, int &x, int &y) {
	Get_shape_location(obj->get_tile(), scrolltx, scrollty, x, y);
	// Smooth scroll the avatar as well, if possible
	if (obj == get_camera_actor()) {
		x += avposx_ld;
		y += avposy_ld;
	}
	x -= scrolltx_lo;
	y -= scrollty_lo;
}
void Game_window::get_shape_location(Tile_coord const &t, int &x, int &y) {
	Get_shape_location(t, scrolltx, scrollty, x, y);
	x -= scrolltx_lo;
	y -= scrollty_lo;
}

/*
 *  Put the actor(s) in the world.
 */

void Game_window::init_actors(
) {
	if (main_actor) {       // Already done?
		Game::clear_avname();
		Game::clear_avsex();
		Game::clear_avskin();
		return;
	}
	read_npcs();            // Read in all U7 NPC's.
	int avsched = combat ? Schedule::combat : Schedule::follow_avatar;
	if (!main_actor_can_act_charmed())
		avsched = Schedule::combat;
	main_actor->set_schedule_type(avsched);

	// Was a name, sex or skincolor set in Game
	// this bascially detects
	bool    changed = false;


	if (Game::get_avsex() == 0 || Game::get_avsex() == 1 || Game::get_avname()
	        || (Game::get_avskin() >= 0 && Game::get_avskin() <= 2))
		changed = true;

	Game::clear_avname();
	Game::clear_avsex();
	Game::clear_avskin();

	// Update gamedat if there was a change
	if (changed) {
		schedule_npcs(6, false);
		write_npcs();
	}

}

/*
 *  Create initial 'gamedat' directory if needed
 *
 */

bool Game_window::init_gamedat(bool create) {
	// Create gamedat files 1st time.
	if (create) {
		cout << "Creating 'gamedat' files." << endl;
		if (is_system_path_defined("<PATCH>") &&
		        U7exists(PATCH_INITGAME))
			restore_gamedat(PATCH_INITGAME);
		else {
			// Flag that we're reading U7 file.
			Game::set_new_game();
			restore_gamedat(INITGAME);
		}
		ofstream out;
		// Editing, and no IDENTITY?
		if (Game::is_editing() && !U7exists(IDENTITY)) {
			U7open(out, IDENTITY);
			std::string gametitlestr = Game::get_gametitle();
			out << gametitlestr.c_str() << endl;
			out.close();
		}

		// log version of exult that was used to start this game
		U7open(out, GNEWGAMEVER);
		getVersionInfo(out);
		out.close();
	}
	//++++Maybe just test for IDENTITY+++:
	else if ((U7exists(U7NBUF_DAT) || !U7exists(NPC_DAT)) &&
	         !Game::is_editing()) {
		return false;
	} else {
		ifstream identity_file;
		U7open(identity_file, IDENTITY);
		char gamedat_identity[256];
		identity_file.read(gamedat_identity, 256);
		char *ptr = gamedat_identity;
		for (; (*ptr != 0x1a && *ptr != 0x0d &&
		        *ptr != 0x0a); ptr++)
			;
		*ptr = 0;
		cout << "Gamedat identity " << gamedat_identity << endl;
		const char *static_identity = get_game_identity(INITGAME);
		if (strcmp(static_identity, gamedat_identity)) {
			delete [] static_identity;
			return false;
		}
		delete [] static_identity;
		// scroll coords.
	}
	read_save_names();      // Read in saved-game names.
	return true;
}

/*
 *  Save game by writing out to the 'gamedat' directory.
 *
 *  Output: 0 if error, already reported.
 */

void Game_window::write(
) {
	// Lets just show a nice message on screen first

	int width = get_width();
	int centre_x = width / 2;
	int height = get_height();
	int centre_y = height / 2;
	int text_height = shape_man->get_text_height(0);
	int text_width = shape_man->get_text_width(0, "Saving Game");

	win->fill_translucent8(0, width, height, 0, 0,
	                       shape_man->get_xform(8));
	shape_man->paint_text(0, "Saving Game", centre_x - text_width / 2,
	                      centre_y - text_height);
	show(true);
	for (vector<Game_map *>::iterator it = maps.begin();
	        it != maps.end(); ++it)
		(*it)->write_ireg();    // Write ireg files.
	write_npcs();           // Write out npc.dat.
	usecode->write();       // Usecode.dat (party, global flags).
	Notebook_gump::write();     // Write out journal.
	write_gwin();           // Write our data.
	write_saveinfo();
}

/*
 *  Restore game by reading in 'gamedat'.
 *
 *  Output: 0 if error, already reported.
 */

void Game_window::read(
) {
	Audio::get_ptr()->cancel_streams();
#ifdef RED_PLASMA
	// Display red plasma during load...
	setup_load_palette();
#endif

	clear_world(true);      // Wipe clean.
	read_gwin();            // Read our data.
	// DON'T do anything that might paint()
	// before calling read_npcs!!
	setup_game(cheat.in_map_editor());  // Read NPC's, usecode.
	Mouse::mouse->set_speed_cursor();
}

/*
 *  Write data for the game.
 *
 *  Output: 0 if error.
 */

void Game_window::write_gwin(
) {
	ofstream gout_stream;
	U7open(gout_stream, GWINDAT);   // Gamewin.dat.
	StreamDataSource gout(&gout_stream);
	// Start with scroll coords (in tiles).
	gout.write2(get_scrolltx());
	gout.write2(get_scrollty());
	// Write clock.
	gout.write2(clock->get_day());
	gout.write2(clock->get_hour());
	gout.write2(clock->get_minute());
	gout.write4(special_light); // Write spell expiration minute.
	MyMidiPlayer *player = Audio::get_ptr()->get_midi();
	if (player) {
		gout.write4(static_cast<uint32>(player->get_current_track()));
		gout.write4(static_cast<uint32>(player->is_repeating()));
	} else {
		gout.write4(static_cast<uint32>(-1));
		gout.write4(0);
	}
	gout.write1(armageddon ? 1 : 0);
	gout.write1(ambient_light ? 1 : 0);
	gout.write1(combat ? 1 : 0);
	gout_stream.flush();
	if (!gout_stream.good())
		throw file_write_exception(GWINDAT);
}

/*
 *  Read data for the game.
 *
 *  Output: 0 if error.
 */

void Game_window::read_gwin(
) {
	if (!clock->in_queue())     // Be sure clock is running.
		tqueue->add(Game::get_ticks(), clock,
		            reinterpret_cast<long>(this));
	ifstream gin_stream;
	try {
		U7open(gin_stream, GWINDAT);    // Gamewin.dat.
	} catch (const file_open_exception &) {
		return;
	}

	StreamDataSource gin(&gin_stream);

	// Start with scroll coords (in tiles).
	scrolltx_lp = scrolltx_l = scrolltx = gin.read2();
	scrollty_lp = scrollty_l = scrollty = gin.read2();
	scrolltx_lo = scrollty_lo = 0;
	avposx_ld = avposy_ld = 0;
	// Read clock.
	clock->reset();
	clock->set_day(gin.read2());
	clock->set_hour(gin.read2());
	clock->set_minute(gin.read2());
	if (!gin_stream.good())     // Next ones were added recently.
		throw file_read_exception(GWINDAT);
	special_light = gin.read4();
	armageddon = false;     // Old saves may not have this yet.

	if (!gin_stream.good()) {
		special_light = 0;
		return;
	}

	int track_num = gin.read4();
	int repeat = gin.read4();
	if (!gin_stream.good()) {
		Audio::get_ptr()->stop_music();
		return;
	}
	MyMidiPlayer *midi = Audio::get_ptr()->get_midi();
	if(!is_bg_track(track_num) || (midi && (midi->get_ogg_enabled() || midi->is_mt32())))
		Audio::get_ptr()->start_music(track_num, repeat != 0);
	armageddon = gin.read1() == 1 ? true : false;
	if (!gin_stream.good())
		armageddon = false;

	ambient_light = gin.read1() == 1 ? true : false;
	if (!gin_stream.good())
		ambient_light = false;

	combat = gin.read1() == 1 ? true : false;
	if (!gin_stream.good())
		combat = false;
}

/*
 *  Was any map modified?
 */

bool Game_window::was_map_modified(
) {
	if (Game_map::was_chunk_terrain_modified())
		return true;
	for (vector<Game_map *>::iterator it = maps.begin();
	        it != maps.end(); ++it) {
		Game_map *map = *it;
		if (map && map->was_map_modified())
			return true;
	}
	return false;
}

/*
 *  Write out map data (IFIXxx, U7CHUNKS, U7MAP) to static, and also
 *  save 'gamedat' to <PATCH>/initgame.dat.
 *
 *  Note: This is for map-editing.
 *
 *  Output: Errors are reported.
 */

void Game_window::write_map(
) {
	for (vector<Game_map *>::iterator it = maps.begin();
	        it != maps.end(); ++it) {
		Game_map *map = *it;
		if (map && map->was_map_modified())
			map->write_static();    // Write ifix, map files.
	}
	write();            // Write out to 'gamedat' too.
	save_gamedat(PATCH_INITGAME, "Saved map");
}

/*
 *  Reinitialize game from map.
 */

void Game_window::read_map(
) {
	init_gamedat(true);     // Unpack 'initgame.dat'.
	read();             // This does the whole restore.
}

/*
 *  Reload (patched) usecode.
 */

void Game_window::reload_usecode(
) {
	// Get custom usecode functions.
	if (is_system_path_defined("<PATCH>") && U7exists(PATCH_USECODE)) {
		ifstream file;
		U7open(file, PATCH_USECODE);
		usecode->read_usecode(file, true);
		file.close();
	}
}

/*
 *  Shift view by one tile.
 */

void Game_window::view_right(
) {
	int w = get_width(), h = get_height();
	// Get current rightmost chunk.
	int old_rcx = ((scrolltx + (w - 1) / c_tilesize) / c_tiles_per_chunk) %
	              c_num_chunks;
	scrolltx = INCR_TILE(scrolltx);
	scroll_bounds.x = INCR_TILE(scroll_bounds.x);
	if (gump_man->showing_gumps()) {    // Gump on screen?
		paint();
		return;
	}
	map->read_map_data();       // Be sure objects are present.
#ifdef HAVE_OPENGL
	if (GL_manager::get_instance()) // OpenGL? Just repaint all.
		paint();
	else
#endif
	{
		// Shift image to left.
		win->copy(c_tilesize, 0, w - c_tilesize, h, 0, 0);
		// Paint 1 column to right.
		paint(w - c_tilesize, 0, c_tilesize, h);
	}
	dirty.x -= c_tilesize;  // Shift dirty rect.
	dirty = clip_to_win(dirty);
	// New chunk?
	int new_rcx = ((scrolltx + (w - 1) / c_tilesize) / c_tiles_per_chunk) %
	              c_num_chunks;
	if (new_rcx != old_rcx)
		Send_location(this);
}
void Game_window::view_left(
) {
	int old_lcx = (scrolltx / c_tiles_per_chunk) % c_num_chunks;
	// Want to wrap.
	scrolltx = DECR_TILE(scrolltx);
	scroll_bounds.x = DECR_TILE(scroll_bounds.x);
	if (gump_man->showing_gumps()) {        // Gump on screen?
		paint();
		return;
	}
	map->read_map_data();       // Be sure objects are present.
#ifdef HAVE_OPENGL
	if (GL_manager::get_instance()) // OpenGL? Just repaint all.
		paint();
	else
#endif
	{
		win->copy(0, 0, get_width() - c_tilesize, get_height(),
		          c_tilesize, 0);
		int h = get_height();
		paint(0, 0, c_tilesize, h);
	}
	dirty.x += c_tilesize;      // Shift dirty rect.
	dirty = clip_to_win(dirty);
	// New chunk?
	int new_lcx = (scrolltx / c_tiles_per_chunk) % c_num_chunks;
	if (new_lcx != old_lcx)
		Send_location(this);
}
void Game_window::view_down(
) {
	int w = get_width(), h = get_height();
	// Get current bottomost chunk.
	int old_bcy = ((scrollty + (h - 1) / c_tilesize) / c_tiles_per_chunk) %
	              c_num_chunks;
	scrollty = INCR_TILE(scrollty);
	scroll_bounds.y = INCR_TILE(scroll_bounds.y);
	if (gump_man->showing_gumps()) {        // Gump on screen?
		paint();
		return;
	}
	map->read_map_data();       // Be sure objects are present.
#ifdef HAVE_OPENGL
	if (GL_manager::get_instance()) // OpenGL? Just repaint all.
		paint();
	else
#endif
	{
		win->copy(0, c_tilesize, w, h - c_tilesize, 0, 0);
		paint(0, h - c_tilesize, w, c_tilesize);
	}
	dirty.y -= c_tilesize;      // Shift dirty rect.
	dirty = clip_to_win(dirty);
	// New chunk?
	int new_bcy = ((scrollty + (h - 1) / c_tilesize) / c_tiles_per_chunk) %
	              c_num_chunks;
	if (new_bcy != old_bcy)
		Send_location(this);
}
void Game_window::view_up(
) {
	int old_tcy = (scrollty / c_tiles_per_chunk) % c_num_chunks;
	// Want to wrap.
	scrollty = DECR_TILE(scrollty);
	scroll_bounds.y = DECR_TILE(scroll_bounds.y);
	if (gump_man->showing_gumps()) {    // Gump on screen?
		paint();
		return;
	}
	map->read_map_data();       // Be sure objects are present.
#ifdef HAVE_OPENGL
	if (GL_manager::get_instance()) // OpenGL? Just repaint all.
		paint();
	else
#endif
	{
		int w = get_width();
		win->copy(0, 0, w, get_height() - c_tilesize, 0, c_tilesize);
		paint(0, 0, w, c_tilesize);
	}
	dirty.y += c_tilesize;      // Shift dirty rect.
	dirty = clip_to_win(dirty);
	// New chunk?
	int new_tcy = (scrollty / c_tiles_per_chunk) % c_num_chunks;
	if (new_tcy != old_tcy)
		Send_location(this);
}

/*
 *  Get gump being dragged.
 */

Gump *Game_window::get_dragging_gump(
) {
	return dragging ? dragging->gump : 0;
}

/*
 *  Alternative start actor function
 *  Placed in an alternative function to prevent breaking barges
 */
void Game_window::start_actor_alt(
    int winx, int winy, // Mouse position to aim for.
    int speed           // Msecs. between frames.
) {
	int ax, ay;
	int blocked[8];
	get_shape_location(main_actor, ax, ay);

	Tile_coord start = main_actor->get_tile();
	int dir;
	bool checkdrop = (main_actor->get_type_flags() & MOVE_LEVITATE) == 0;

	for (dir = 0; dir < 8; dir++) {
		Tile_coord dest = start.get_neighbor(dir);
		blocked[dir] = main_actor->is_blocked(dest, &start,
		                                      main_actor->get_type_flags());
		if (checkdrop && abs(start.tz - dest.tz) > 1)
			blocked[dir] = true;
	}

	dir = Get_direction_NoWrap(ay - winy, winx - ax);

	if (blocked[dir] && !blocked[(dir + 1) % 8])
		dir = (dir + 1) % 8;
	else if (blocked[dir] && !blocked[(dir + 7) % 8])
		dir = (dir + 7) % 8;
	else if (blocked[dir]) {
		Game_object *block = main_actor->is_moving() ? 0
		                     : main_actor->find_blocking(start.get_neighbor(dir), dir);
		// We already know the blocking object isn't the avatar, so don't
		// double check it here.
		if (!block || !block->move_aside(main_actor, dir)) {
			stop_actor();
			if (main_actor->get_lift() % 5) { // Up on something?
				// See if we're stuck in the air.
				int savetz = start.tz;
				if (!Map_chunk::is_blocked(start, 1,
				                           MOVE_WALK, 100) &&
				        start.tz < savetz)
					main_actor->move(start.tx, start.ty,
					                 start.tz);
			}
			return;
		}
	}

	const int delta = step_tile_delta * c_tilesize; // Bigger # here avoids jerkiness,
	// but causes probs. with followers.
	switch (dir) {
	case north:
		//cout << "NORTH" << endl;
		ay -= delta;
		break;

	case northeast:
		//cout << "NORTH EAST" << endl;
		ay -= delta;
		ax += delta;
		break;

	case east:
		//cout << "EAST" << endl;
		ax += delta;
		break;

	case southeast:
		//cout << "SOUTH EAST" << endl;
		ay += delta;
		ax += delta;
		break;

	case south:
		//cout << "SOUTH" << endl;
		ay += delta;
		break;

	case southwest:
		//cout << "SOUTH WEST" << endl;
		ay += delta;
		ax -= delta;
		break;

	case west:
		//cout << "WEST" << endl;
		ax -= delta;
		break;

	case northwest:
		//cout << "NORTH WEST" << endl;
		ay -= delta;
		ax -= delta;
		break;
	}

	int lift = main_actor->get_lift();
	int liftpixels = 4 * lift;  // Figure abs. tile.
	int tx = get_scrolltx() + (ax + liftpixels) / c_tilesize,
	    ty = get_scrollty() + (ay + liftpixels) / c_tilesize;
	// Wrap:
	tx = (tx + c_num_tiles) % c_num_tiles;
	ty = (ty + c_num_tiles) % c_num_tiles;
	main_actor->walk_to_tile(tx, ty, lift, speed, 0);
	if (walk_in_formation && main_actor->get_action())
		//++++++In this case, may need to set schedules back to
		// follow_avatar after, say, sitting.++++++++++
		main_actor->get_action()->set_get_party(true);
	else                // "Traditional" Exult walk:-)
		main_actor->get_followers();
}

/*
 *  Start the actor.
 */

void Game_window::start_actor(
    int winx, int winy, // Mouse position to aim for.
    int speed           // Msecs. between frames.
) {
	if (main_actor->Actor::get_flag(Obj_flags::asleep))
		return;         // Zzzzz....
	if (!cheat.in_map_editor() &&
	        (main_actor->in_usecode_control() ||
	         main_actor->get_flag(Obj_flags::paralyzed)))
		return;
	if (gump_man->gump_mode() && !gump_man->gumps_dont_pause_game())
		return;
//	teleported = 0;
	if (moving_barge) {
		// Want to move center there.
		int lift = main_actor->get_lift();
		int liftpixels = 4 * lift;  // Figure abs. tile.
		int tx = get_scrolltx() + (winx + liftpixels) / c_tilesize,
		    ty = get_scrollty() + (winy + liftpixels) / c_tilesize;
		// Wrap:
		tx = (tx + c_num_tiles) % c_num_tiles;
		ty = (ty + c_num_tiles) % c_num_tiles;
		Tile_coord atile = moving_barge->get_center(),
		           btile = moving_barge->get_tile();
		// Go faster than walking.
		moving_barge->travel_to_tile(
		    Tile_coord(tx + btile.tx - atile.tx,
		               ty + btile.ty - atile.ty, btile.tz),
		    speed / 2);
	} else {
		/*
		main_actor->walk_to_tile(tx, ty, lift, speed, 0);
		main_actor->get_followers();
		*/
		// Set schedule.
		int sched = main_actor->get_schedule_type();
		if (sched != Schedule::follow_avatar &&
		        sched != Schedule::combat &&
		        !main_actor->get_flag(Obj_flags::asleep))
			main_actor->set_schedule_type(Schedule::follow_avatar);
		// Going to use the alternative function for this at the moment
		start_actor_alt(winx, winy, speed);
	}
}

/*
 *  Find path to where user double-right-clicked.
 */

void Game_window::start_actor_along_path(
    int winx, int winy, // Mouse position to aim for.
    int speed           // Msecs. between frames.
) {
	if (main_actor->Actor::get_flag(Obj_flags::asleep) ||
	        main_actor->Actor::get_flag(Obj_flags::paralyzed) ||
	        main_actor->get_schedule_type() == Schedule::sleep ||
	        moving_barge)       // For now, don't do barges.
		return;         // Zzzzz....
	// Animation in progress?
	if (!cheat.in_map_editor() && main_actor->in_usecode_control())
		return;
//	teleported = 0;
	int lift = main_actor->get_lift();
	int liftpixels = 4 * lift;  // Figure abs. tile.
	Tile_coord dest(get_scrolltx() + (winx + liftpixels) / c_tilesize,
	                get_scrollty() + (winy + liftpixels) / c_tilesize, lift);
	if (!main_actor->walk_path_to_tile(dest, speed))
		cout << "Couldn't find path for Avatar." << endl;
	else
		main_actor->get_followers();
}

/*
 *  Stop the actor.
 */

void Game_window::stop_actor(
) {
	if (moving_barge)
		moving_barge->stop();
	else {
		main_actor->stop(); // Stop and set resting state.
		if (!gump_man->gump_mode() || gump_man->gumps_dont_pause_game())
//+++++++The following line is for testing:
//			if (!walk_in_formation)
			main_actor->get_followers();
	}
}

/*
 *  Teleport the party.
 */

void Game_window::teleport_party(
    Tile_coord const &t,    // Where to go.
    bool skip_eggs,         // Don't activate eggs at dest.
    int newmap,         // New map #, or -1 for same map.
    bool no_status_check
) {
	Tile_coord oldpos = main_actor->get_tile();
	main_actor->set_action(0);  // Definitely need this, or you may
	//   step back to where you came from.
	moving_barge = 0;       // Calling 'done()' could be risky...
	int i, cnt = party_man->get_count();
	if (newmap != -1)
		set_map(newmap);
	main_actor->move(t.tx, t.ty, t.tz, newmap); // Move Avatar.
	// Fixes a rare crash when moving between maps and teleporting:
	newmap = main_actor->get_map_num();
	center_view(t);         // Bring pos. into view, and insure all
	clock->reset();         // Reset and re-display palette.
	clock->set_palette();
	//   objs. exist.
	for (i = 0; i < cnt; i++) {
		int party_member = party_man->get_member(i);
		Actor *person = get_npc(party_member);
		if (person && !person->is_dead() &&
		        person->get_schedule_type() != Schedule::wait
		        && (person->can_act() || no_status_check)) {
			person->set_action(0);
			Tile_coord t1 = Map_chunk::find_spot(t, 8,
			                                     person->get_shapenum(), person->get_framenum(),
			                                     1);
			if (t1.tx != -1) {
				person->move(t1, newmap);
				person->set_frame(person->get_dir_framenum(Actor::standing));
			}
		}
	}
	main_actor->get_followers();
	if (!skip_eggs)         // Check all eggs around new spot.
		Map_chunk::try_all_eggs(main_actor, t.tx, t.ty, t.tz,
		                        oldpos.tx, oldpos.ty);
//	teleported = 1;
	// generate mousemotion event
	int x, y;
	SDL_GetMouseState(&x, &y);
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_WarpMouseInWindow(win->get_screen_window(), x, y);
#else
	SDL_WarpMouse(x, y);
#endif
}

/*
 *  Get party members.
 *
 *  Output: Number returned in 'list'.
 */

int Game_window::get_party(
    Actor **a_list,         // Room for 9.
    int avatar_too          // 1 to include Avatar too.
) {
	int n = 0;
	if (avatar_too && main_actor)
		a_list[n++] = main_actor;
	int cnt = party_man->get_count();
	for (int i = 0; i < cnt; i++) {
		int party_member = party_man->get_member(i);
		Actor *person = get_npc(party_member);
		if (person)
			a_list[n++] = person;
	}
	return n;           // Return # actually stored.
}

/*
 *  Find a given shaped item amongst the party, and 'activate' it.  This
 *  is used, for example, by the 'f' command to feed.
 *
 *  Output: True if the item was found, else false.
 */

bool Game_window::activate_item(
    int shnum,          // Desired shape.
    int frnum,          // Desired frame
    int qual            // Desired quality
) {
	Actor *party[9];        // Get party.
	int cnt = get_party(party, 1);
	for (int i = 0; i < cnt; i++) {
		Actor *person = party[i];
		Game_object *obj = person->find_item(shnum, qual, frnum);
		if (obj) {
			obj->activate();
			return true;
		}
	}
	return false;
}
/*
 *  Find the top object that can be selected, dragged, or activated.
 *  The one returned is the 'highest'.
 *
 *  Output: ->object, or null if none.
 */

Game_object *Game_window::find_object(
    int x, int y            // Pos. on screen.
) {
#ifdef DEBUG
	cout << "Clicked at tile (" << get_scrolltx() + x / c_tilesize << ", " <<
	     get_scrollty() + y / c_tilesize << ")" << endl;
#endif
	int not_above = get_render_skip_lift();
	// Figure chunk #'s.
	int start_cx = ((scrolltx +
	                 x / c_tilesize) / c_tiles_per_chunk) % c_num_chunks;
	int start_cy = ((scrollty +
	                 y / c_tilesize) / c_tiles_per_chunk) % c_num_chunks;
	// Check 1 chunk down & right too.
	int stop_cx = (2 + (scrolltx +
	                    (x + 4 * not_above) / c_tilesize) / c_tiles_per_chunk) % c_num_chunks;
	int stop_cy = (2 + (scrollty +
	                    (y + 4 * not_above) / c_tilesize) / c_tiles_per_chunk) % c_num_chunks;

	Game_object *best = 0;      // Find 'best' one.
	bool trans = true;      // Try to avoid 'transparent' objs.
	// Go through them.
	for (int cy = start_cy; cy != stop_cy; cy = INCR_CHUNK(cy))
		for (int cx = start_cx; cx != stop_cx; cx = INCR_CHUNK(cx)) {
			Map_chunk *olist = map->get_chunk(cx, cy);
			if (!olist)
				continue;
			Object_iterator next(olist->get_objects());
			Game_object *obj;
			while ((obj = next.get_next()) != 0) {
				if (obj->get_lift() >= not_above ||
				        !get_shape_rect(obj).has_world_point(x, y) ||
				        !obj->is_findable())
					continue;
				// Check the shape itself.
				Shape_frame *s = obj->get_shape();
				int ox, oy;
				get_shape_location(obj, ox, oy);
				if (!s->has_point(x - ox, y - oy))
					continue;
				// Fixes key under rock in BG at [915, 2434, 0]; need to
				// know if there are side effects.
				if (!best || best->lt(*obj) != 0 || trans) {
					bool ftrans = obj->get_info().is_transparent() != 0;
					// TODO: Fix this properly, instead of with an ugly hack.
					// This fixes clicking the Y shapes instead of the Y
					// depression in SI. This has the effect of also making
					// the Y depressions not draggable.
					if (GAME_SI && obj->get_shapenum() == 0xd1
					    && obj->get_framenum() == 17) {
						ftrans = true;  // Pretend it is transparent
					}
					if (!ftrans || trans) {
						best = obj;
						trans = ftrans;
					}
				}
			}
		}
	return (best);
}

#ifdef __IPHONEOS__
void Game_window::find_nearby_objects(Game_object_map_xy *mobjxy, int x, int y, Gump *gump) {
	Game_object *iobj;
	// Find object at each pixel
	for (int iy = y - 10; iy < (y + 10); iy++) {
		for (int ix = x - 10; ix < (x + 10); ix++) {
			if (gump) {
				iobj = gump->find_object(ix, iy);
			} else {
				iobj = find_object(ix, iy);
			}

			if (iobj) {
				int *arrXY = new int[2];
				arrXY[0] = ix;
				arrXY[1] = iy;
				std::pair<Game_object_map_xy::iterator, bool> ret;

				ret = mobjxy->insert(std::pair<Game_object *, int *>(iobj, arrXY));
				if (ret.second == false)
					delete arrXY;
			}
		}
	}
}
#endif
static inline string Get_object_name(Game_object *obj) {
	if (obj == Game_window::get_instance()->get_main_actor()) {
		if (GAME_BG)
			return get_misc_name(0x42);
		else if (GAME_SI)
			return get_misc_name(0x4e);
		else
			return obj->get_name();
	}
	return obj->get_name();
}

/*
 *  Show the name of the item the mouse is clicked on.
 */

void Game_window::show_items(
    int x, int y,           // Coords. in window.
    bool ctrl           // Control key is pressed.
) {
	// Look for obj. in open gump.
	Gump *gump = gump_man->find_gump(x, y);
	Game_object *obj;       // What we find.
	if (gump) {
		obj = gump->find_object(x, y);
		if (!obj) obj = gump->get_cont_or_actor(x, y);
	} else {            // Search rest of world.
		obj = find_object(x, y);
	}
#ifdef __IPHONEOS__
	Game_object_map_xy mobjxy;
	find_nearby_objects(&mobjxy, x, y, gump);
	if (mobjxy.size() > 0) {
		Itemmenu_gump *itemgump = new Itemmenu_gump(&mobjxy, x, y);
		Game_window::get_instance()->get_gump_man()->do_modal_gump(itemgump, Mouse::hand);
		itemgump->postCloseActions();
		delete itemgump;
		obj = NULL;
	}
#endif
	// Map-editing?
	if (obj && cheat.in_map_editor()) {

		if (ctrl)       // Control?  Toggle.
			cheat.toggle_selected(obj);
		else {
			// In normal mode, sel. just this one.
			cheat.clear_selected();
			if (cheat.get_edit_mode() == Cheat::move)
				cheat.append_selected(obj);
		}
	} else              // All other cases:  unselect.
		cheat.clear_selected();

	// Do we have an NPC?
	Actor *npc = obj ? obj->as_actor() : 0;
	if (npc && cheat.number_npcs() &&
	        (npc->get_npc_num() > 0 || npc == main_actor)) {
		char str[64];
		std::string namestr = Get_object_name(obj);
		snprintf(str, 64, "(%i) %s", npc->get_npc_num(),
		         namestr.c_str());
		effects->add_text(str, obj);
	} else if (obj) {
		// Show name.
		std::string namestr = Get_object_name(obj);
		const char *objname = namestr.c_str();
		if (Game_window::get_instance()->failed_copy_protection() &&
		        (npc == main_actor || !npc)) {  // Avatar and items
			char oink[6];
			snprintf(oink, 6, "Oink!");
			objname = &oink[0];
		}
		// Combat and an NPC?
		if (in_combat() && Combat::mode != Combat::original && npc) {
			char buf[128];
			sprintf(buf, "%s (%d)", objname,
			        npc->get_property(Actor::health));
			objname = &buf[0];
		}
		effects->add_text(objname, obj);
	} else if (cheat.in_map_editor() && skip_lift > 0) {
		// Show flat, but not when editing ter.
		ShapeID id = get_flat(x, y);
		char str[12];
		snprintf(str, 12, "Flat %d:%d", id.get_shapenum(),
		         id.get_framenum());
		effects->add_text(str, x, y);
	}
	// If it's an actor and we want to grab the actor, grab it.
	if (npc && cheat.grabbing_actor() &&
	        (npc->get_npc_num() || npc == main_actor))
		cheat.set_grabbed_actor(npc);

#ifdef DEBUG
	int shnum, frnum;
	if (obj) {
		shnum = obj->get_shapenum(), frnum = obj->get_framenum();
		Shape_info &info = obj->get_info();
		cout << "Object " << shnum << ':' << frnum <<
		     " has 3d tiles (x, y, z): " <<
		     info.get_3d_xtiles(frnum) << ", " <<
		     info.get_3d_ytiles(frnum) << ", " <<
		     info.get_3d_height();
		Actor *npc = obj->as_actor();
		if (npc)
			cout  << ", sched = " <<
			      npc->get_schedule_type() << ", real align = " <<
			      npc->get_alignment() << ", eff. align = " <<
			      npc->get_effective_alignment() << ", npcnum = " <<
			      npc->get_npc_num();
		cout << endl;
		if (obj->get_chunk()) {
			Tile_coord t = obj->get_tile();
			cout << "tx = " << t.tx << ", ty = " << t.ty <<
			     ", tz = " << t.tz << ", ";
		}
		cout << "quality = " <<
		     obj->get_quality() <<
		     ", okay_to_take = " <<
		     static_cast<int>(obj->get_flag(Obj_flags::okay_to_take)) <<
		     ", flag0x1d = " << static_cast<int>(obj->get_flag(0x1d)) <<
		     ", hp = " << obj->get_obj_hp() << ", weight = " << obj->get_weight()
		     << ", volume = " << obj->get_volume()
		     << endl;
		cout << "obj = " << static_cast<void *>(obj) << endl;
		if (obj->get_flag(Obj_flags::asleep))
			cout << "ASLEEP" << endl;
		if (obj->is_egg())  // Show egg info. here.
			obj->as_egg()->print_debug();
	} else {            // Obj==0
		ShapeID id = get_flat(x, y);
		shnum = id.get_shapenum();
		cout << "Clicked on flat shape " <<
		     shnum << ':' << id.get_framenum() << endl;

#ifdef CHUNK_OBJ_DUMP
		Map_chunk *chunk = map->get_chunk_safely(x / c_tiles_per_chunk, y / c_tiles_per_chunk);
		Object_iterator it(chunk->get_objects());
		Game_object *each;
		cout << "Chunk Contents: " << endl;
		while ((each = it.get_next()) != 0)
			cout << "    " << each->get_name() << ":" << each->get_shapenum() << ":" << each->get_framenum() << endl;
#endif
		if (id.is_invalid())
			return;
	}
	Shape_info &info = ShapeID::get_info(shnum);
	cout << "TFA[1][0-6]= " << ((static_cast<int>(info.get_tfa(1))) & 127) << endl;
	cout << "TFA[0][0-1]= " << ((static_cast<int>(info.get_tfa(0)) & 3)) << endl;
	cout << "TFA[0][3-4]= " << ((static_cast<int>((info.get_tfa(0) >> 3)) & 3)) << endl;
	if (info.is_animated())
		cout << "Object is ANIMATED" << endl;
	if (info.has_translucency())
		cout << "Object has TRANSLUCENCY" << endl;
	if (info.is_transparent())
		cout << "Object is TRANSPARENT" << endl;
	if (info.is_light_source())
		cout << "Object is LIGHT_SOURCE" << endl;
	if (info.is_door())
		cout << "Object is a DOOR" << endl;
	if (info.is_solid())
		cout << "Object is SOLID" << endl;
#endif
}

/*
 *  Handle right click when combat is paused.  The user can right-click
 *  on a party member, then on an enemy to attack.
 */

void Game_window::paused_combat_select(
    int x, int y            // Coords in window.
) {
	Gump *gump = gump_man->find_gump(x, y);
	if (gump)
		return;         // Ignore if clicked on gump.
	Game_object *obj = find_object(x, y);
	Actor *npc = obj ? obj->as_actor() : 0;
	if (!npc || !npc->is_in_party() ||
	        npc->get_flag(Obj_flags::asleep) || npc->is_dead() ||
	        npc->get_flag(Obj_flags::paralyzed) ||
	        !npc->can_act_charmed())
		return;         // Want an active party member.
	npc->paint_outline(PROTECT_PIXEL);
	show(true);         // Flash white outline.
	SDL_Delay(100);
	npc->add_dirty();
	paint_dirty();
	show();
	// Pick a spot.
	if (!Get_click(x, y, Mouse::greenselect, 0, true))
		return;
	obj = find_object(x, y);    // Find it.
	if (!obj) {         // Nothing?  Walk there.
		// Needs work if lift > 0.
		int lift = npc->get_lift();
		int liftpixels = 4 * lift;
		Tile_coord dest(scrolltx + (x + liftpixels) / c_tilesize,
		                scrollty + (y + liftpixels) / c_tilesize, lift);
		// Aim within 1 tile.
		if (!npc->walk_path_to_tile(dest, std_delay, 0, 1))
			Mouse::mouse->flash_shape(Mouse::blocked);
		else            // Make sure he's in combat mode.
			npc->set_target(0, true);
		return;
	}
	Actor *target = obj->as_actor();
	// Don't attack party or body.
	if ((target && target->is_in_party()) ||
	        obj->get_info().is_body_shape()) {
		Mouse::mouse->flash_shape(Mouse::redx);
		return;
	}
	npc->set_target(obj, true);
	obj->paint_outline(HIT_PIXEL);  // Flash red outline.
	show(true);
	SDL_Delay(100);
	add_dirty(obj);
	paint_dirty();
	show();
	if (npc->get_schedule_type() != Schedule::combat)
		npc->set_schedule_type(Schedule::combat);
}

/*
 *  Get the 'flat' that a screen point is in.
 */

ShapeID Game_window::get_flat(
    int x, int y            // Window point.
) {
	int tx = (get_scrolltx() + x / c_tilesize) % c_num_tiles;
	int ty = (get_scrollty() + y / c_tilesize) % c_num_tiles;
	int cx = tx / c_tiles_per_chunk, cy = ty / c_tiles_per_chunk;
	tx = tx % c_tiles_per_chunk;
	ty = ty % c_tiles_per_chunk;
	Map_chunk *chunk = map->get_chunk(cx, cy);
	ShapeID id = chunk->get_flat(tx, ty);
	return id;
}

/*
 *  Remove an item from the world and set it for later deletion.
 */

void Game_window::delete_object(
    Game_object *obj
) {
	obj->set_invalid();     // Set to invalid chunk.
	if (!obj->is_monster())     // Don't delete these!
		removed->insert(obj);   // Add to pool instead.
}
#if 0   /* ++++++Goes away. */
/*
 *  A sign or plaque?
 */

static bool Is_sign(
    int shnum
) {
	switch (shnum) {
	case 820:           // Plaque.
	case 360:
	case 361:
	case 379:
		return true;
	default:
		return false;
	}
}
#endif
/*
 *  Handle a double-click.
 */

void Game_window::double_clicked(
    int x, int y            // Coords in window.
) {
#if 0
//++++++++++++TESTING
	static int ncnt = 0;
	cout << "Showing xform for ncnt = " << ncnt << endl;
	std::size_t nxforms = sizeof(xforms) / sizeof(xforms[0]);
	pal->load(PALETTES_FLX, PATCH_PALETTES, 0, XFORMTBL, nxforms - 1 - ncnt);
	pal->apply(false);
	ncnt = (ncnt + 1) % nxforms;
//^^^^^^^^^^^^TESTING
#endif
	// Animation in progress?
	if (main_actor_dont_move())
		return;
	// Nothing going on?
	if (!Usecode_script::get_count())
		removed->flush();   // Flush removed objects.
	// Look for obj. in open gump.
	Game_object *obj = 0;
	bool gump = gump_man->double_clicked(x, y, obj);
	bool avatar_can_act = main_actor_can_act();

	// If gump manager didn't handle it, we search the world for an object
	if (!gump) {
		obj = find_object(x, y);
		if (!avatar_can_act && obj && obj->as_actor()
		        && obj->as_actor() == main_actor->as_actor()) {
			ActionFileGump(0);
			return;
		}
		// Check path, except if an NPC, sign, or if editing.
		if (obj && !obj->as_actor() &&
		        !cheat.in_hack_mover() &&
		        //!Is_sign(obj->get_shapenum()) &&
		        !Fast_pathfinder_client::is_grabable(main_actor, obj)) {
			Mouse::mouse->flash_shape(Mouse::blocked);
			return;
		}
	}
	if (!obj || !avatar_can_act)
		return;         // Nothing found or avatar disabled.
	if (combat && !gump &&      // In combat?
	        !Combat::is_paused() && main_actor_can_act_charmed() &&
	        (!gump_man->gump_mode() || gump_man->gumps_dont_pause_game())) {
		Actor *npc = obj->as_actor();
		// But don't attack party members.
		if ((!npc || !npc->is_in_party()) &&
		        // Or bodies.
		        !obj->get_info().is_body_shape()) {
			// In combat mode.
			// Want everyone to be in combat.
			combat = 0;
			main_actor->set_target(obj);
			toggle_combat();
			return;
		}
	}
	effects->remove_text_effects(); // Remove text msgs. from screen.
#ifdef DEBUG
	cout << "Object name is " << obj->get_name() << endl;
#endif
	usecode->init_conversation();
	obj->activate();
	npc_prox->wait(4);      // Delay "barking" for 4 secs.
}


/*
 *  Add an NPC to the 'nearby' list.
 */

void Game_window::add_nearby_npc(
    Npc_actor *npc
) {
	if (!npc->is_nearby()) {
		npc->set_nearby();
		npc_prox->add(Game::get_ticks(), npc);
	}
}

/*
 *  Remove an NPC from the nearby list.
 */

void Game_window::remove_nearby_npc(
    Npc_actor *npc
) {
	if (npc->is_nearby())
		npc_prox->remove(npc);
}

/*
 *  Add all nearby NPC's to the given list.
 */

void Game_window::get_nearby_npcs(
    Actor_vector &a_list
) {
	npc_prox->get_all(a_list);
}

/*
 *  Tell all npc's to update their schedules at a new 3-hour period.
 */

void Game_window::schedule_npcs(
    int hour,           // 24 hour
    bool repaint
) {
	// Go through npc's, skipping Avatar.
	for (Actor_vector::iterator it = npcs.begin() + 1;
	        it != npcs.end(); ++it) {
		Actor *npc = *it;
		if (!npc)
			continue;
		// Don't want companions leaving.
		if (npc->get_schedule_type() != Schedule::wait &&
		        (npc->get_schedule_type() != Schedule::combat ||
		         npc->get_target() == 0))
			npc->update_schedule(hour / 3, hour % 3 == 0 ? -1 : 0);
	}

	if (repaint)
		paint();            // Repaint all.
}

/*
 *  Tell all npc's to restore some of their HP's and/or mana on the hour.
 */

void Game_window::mend_npcs(
) {
	// Go through npc's.
	for (Actor_vector::iterator it = npcs.begin();
	        it != npcs.end(); ++it) {
		Actor *npc = *it;
		if (npc)
			npc->mend_wounds(true);
	}
}

/*
 *  Get guard shape.
 */

int Game_window::get_guard_shape(
) {
	// Base on avatar's position.
	Tile_coord const pos = Game_window::get_instance()->main_actor->get_tile();
	if (!GAME_SI)           // Default (BG).
		return 0x3b2;
	// Moonshade?
	if (pos.tx >= 2054 && pos.ty >= 1698 &&
	        pos.tx < 2590 && pos.ty < 2387)
		return 0x103;       // Ranger.
	// Fawn?
	if (pos.tx >= 895 && pos.ty >= 1604 &&
	        pos.tx < 1173 && pos.ty < 1960)
		return 0x17d;       // Fawn guard.
	if (pos.tx >= 670 && pos.ty >= 2430 &&
	        pos.tx < 1135 && pos.ty < 2800)
		return 0xe4;        // Pikeman.
	return -1;          // No local guard.
}

/*
 *  Find a witness to the Avatar's thievery.
 *
 *  Output: ->witness, or NULL.
 *      closest_npc = closest one that's nearby.
 */

Actor *Game_window::find_witness(
    Actor  *&closest_npc,       // Closest one returned.
    int align                   // Desired alignment.
) {
	Actor_vector npcs;          // See if someone is nearby.
	main_actor->find_nearby_actors(npcs, c_any_shapenum, 12, 0x28);
	closest_npc = 0;        // Look for closest NPC.
	int closest_dist = 5000;
	Actor *witness = 0;     // And closest facing us.
	int closest_witness_dist = 5000;
	int gshape = get_guard_shape();
	for (Actor_vector::const_iterator it = npcs.begin();
	        it != npcs.end(); ++it) {
		Actor *npc = *it;
		// Want non-party intelligent and not disabled NPCs only.
		if (npc->is_in_party() || !npc->is_sentient() || !npc->can_act())
			continue;
		// Witnesses must either match desired alignment or they must be
		// local guards and the alignment must be neutral. This makes guards
		// assist neutral and chaotic NPCs.
		if (npc->get_effective_alignment() != align ||
		        (npc->get_shapenum() == gshape && align != Actor::neutral))
			continue;
		int dist = npc->distance(main_actor);
		if (dist >= closest_witness_dist ||
		        !Fast_pathfinder_client::is_grabable(npc, main_actor))
			continue;
		// Looking toward Avatar?
		Schedule::Schedule_types sched =
		    static_cast<Schedule::Schedule_types>(npc->get_schedule_type());
		int dir = npc->get_direction(main_actor);
		int facing = npc->get_dir_facing();
		int dirdiff = (dir - facing + 8) % 8;
		if (dirdiff < 3 || dirdiff > 5 || sched == Schedule::hound) {
			// Yes.
			witness = npc;
			closest_witness_dist = dist;
		} else if (dist < closest_dist) {
			closest_npc = npc;
			closest_dist = dist;
		}
	}
	return witness;
}

/*
 *  Handle theft.
 */

void Game_window::theft(
) {
	// See if in a new location.
	int cx = main_actor->get_cx(), cy = main_actor->get_cy();
	if (cx != theft_cx || cy != theft_cy) {
		theft_cx = cx;
		theft_cy = cy;
		theft_warnings = 0;
	}
	Actor *closest_npc;
	Actor *witness = find_witness(closest_npc, Actor::neutral);
	if (!witness) {
		if (closest_npc && rand() % 2) {
			if (closest_npc->is_goblin())
				closest_npc->say(goblin_heard_something);
			else if (closest_npc->can_speak())
				closest_npc->say(heard_something);
		}
		return;         // Didn't get caught.
	}
	bool avainvisible = main_actor->get_flag(Obj_flags::invisible);
	if (avainvisible && !witness->can_see_invisible()) {
		if (witness->is_goblin())
			witness->say(goblin_invis_theft);
		else if (witness->can_speak())
			witness->say(first_invis_theft, last_invis_theft);
		return;         // Didn't get caught because was invisible.
	}
	int dir = witness->get_direction(main_actor);
	// Face avatar.
	witness->change_frame(witness->get_dir_framenum(dir,
	                      Actor::standing));
	// If not in combat, change to hound schedule.
	Schedule::Schedule_types sched =
	    static_cast<Schedule::Schedule_types>(witness->get_schedule_type());
	if (sched != Schedule::combat && sched != Schedule::hound)
		witness->set_schedule_type(Schedule::hound);
	theft_warnings++;
	if (theft_warnings < 2 + rand() % 3) {
		// Just a warning this time.
		if (witness->is_goblin())
			witness->say(goblin_theft);
		else if (witness->can_speak())
			witness->say(first_theft, last_theft);
		return;
	}
	gump_man->close_all_gumps();    // Get gumps off screen.
	call_guards(witness, true);
}

/*
 *  Create a guard to arrest the Avatar.
 */

void Game_window::call_guards(
    Actor *witness,         // ->witness, or 0 to find one.
    bool theft              // called from Game_window::theft
) {
	Actor *closest;
	if (armageddon || in_dungeon)
		return;
	int gshape = get_guard_shape(),
	    align = witness ? witness->get_effective_alignment() : Actor::neutral;
	if (witness || (witness = find_witness(closest, align)) != 0) {
		if (witness->is_goblin()) {
			if (gshape < 0)
				witness->say(goblin_need_help);
			else if (theft)
				witness->say(goblin_call_guards_theft);
			else
				witness->say(first_goblin_call_guards, last_goblin_call_guards);
		} else if (witness->can_speak()) {
			if (gshape < 0)
				witness->say(first_need_help, last_need_help);
			else if (theft)
				witness->say(first_call_guards_theft, last_call_guards_theft);
			else
				witness->say(first_call_guards, last_call_guards);
		}
	}
	if (gshape < 0) { // No local guards; lets forward to attack_avatar.
		attack_avatar(0, align);
		return;
	}

	Tile_coord dest = Map_chunk::find_spot(main_actor->get_tile(), 5, gshape, 0, 1);
	if (dest.tx != -1) {
		// Show guard running up.
		// Create it off-screen.
		int numguards = 1 + rand() % 3;
		Tile_coord offscreen(main_actor->get_tile() + Tile_coord(128, 128, 0));
		// Start in combat if avatar is fighting.
		// FIXME: Disabled for now, as causes guards to attack
		// avatar if you break glass, when they should arrest
		// instead.
		Schedule::Schedule_types sched = /*combat ? Schedule::combat
                                                :*/ Schedule::arrest_avatar;
		for (int i = 0; i < numguards; i++) {
			Monster_actor *guard = Monster_actor::create(gshape, offscreen,
			                       sched, Actor::chaotic);
			add_nearby_npc(guard);
			guard->approach_another(main_actor);
		}
		// Guaranteed way to do it.
		MyMidiPlayer *player = Audio::get_ptr()->get_midi();
		if (player &&
		        !Background_noise::is_combat_music(player->get_current_track()))
			Audio::get_ptr()->start_music(Audio::game_music(10), true);
	}
}

/*
 *  Have nearby residents attack the Avatar.
 */

void Game_window::attack_avatar(
    int create_guards,      // # of extra guards to create.
    int align
) {
	if (armageddon)
		return;
	int gshape = get_guard_shape();
	if (gshape >= 0 && !in_dungeon) {
		while (create_guards--) {
			// Create it off-screen.
			Monster_actor *guard = Monster_actor::create(gshape,
			                       main_actor->get_tile() + Tile_coord(128, 128, 0),
			                       Schedule::combat, Actor::chaotic);
			add_nearby_npc(guard);
			guard->set_target(main_actor, true);
			guard->approach_another(main_actor);
		}
	}

	int numhelpers = 0;
	int const maxhelpers = 3;
	Actor_vector npcs;      // See if someone is nearby.
	main_actor->find_nearby_actors(npcs, c_any_shapenum, 20, 0x28);
	for (Actor_vector::const_iterator it = npcs.begin();
	        it != npcs.end() && numhelpers < maxhelpers; ++it) {
		Actor *npc = *it;
		if (npc->can_act() && !npc->is_in_party() && npc->is_sentient() &&
		        ((npc->get_shapenum() == gshape && !in_dungeon) ||
		         align == npc->get_effective_alignment()) &&
		        // Only if can get there.
		        Fast_pathfinder_client::is_grabable(npc, main_actor)) {
			numhelpers++;
			npc->set_target(main_actor, true);
		}
	}
	// Guaranteed way to do it.
	MyMidiPlayer *player = Audio::get_ptr()->get_midi();
	if (player &&
	        !Background_noise::is_combat_music(player->get_current_track()))
		Audio::get_ptr()->start_music(Audio::game_music(10), true);
}

/*
 *  Gain/lose focus.
 */

void Game_window::get_focus(
) {
	cout << "Game resumed" << endl;
	Audio::get_ptr()->resume_audio();
	focus = 1;
	tqueue->resume(Game::get_ticks());
}
void Game_window::lose_focus(
) {
	if (!focus)
		return;         // Fixes SDL bug.
	cout << "Game paused" << endl;

	string str;
	config->value("config/audio/disablepause", str, "no");
	if (str == "no")
		Audio::get_ptr()->pause_audio();

	focus = false;
	tqueue->pause(Game::get_ticks());
}


/*
 *  Prepare for game
 */

void Game_window::setup_game(
    bool map_editing
) {
#if 0
	// Was not working as intended; in a new game,
	// only map 0 was being loaded.
	for (vector<Game_map *>::iterator it = maps.begin();
	        it != maps.end(); ++it)
		(*it)->init();
#else
	// Map 0 always exists.
	get_map(0)->init();
	if (is_system_path_defined("<PATCH>"))
		// There is a patch dir; search for other maps.
		for (int i = 1; i <= 0xFF; i++) {
			char fname[128];
			if (U7exists(Get_mapped_name(PATCH_U7MAP, i, fname)))
				get_map(i)->init();
		}
#endif
	// Init. current 'tick'.
	Game::set_ticks(SDL_GetTicks());
	init_actors();      // Set up actors if not already done.
	// This also sets up initial
	// schedules and positions.

	CYCLE_RED_PLASMA();

	Notebook_gump::initialize();        // Read in journal.

	//read autonotes
	std::string d, autonotesfilename;
	d = "config/disk/game/" + Game::get_gametitle() + "/autonotes";
	config->value(d.c_str(), autonotesfilename, "(default)");
	if (autonotesfilename == "(default)") {
		config->set(d.c_str(), autonotesfilename, true);
		Notebook_gump::read_auto_text();
	} else {
		try {
			Notebook_gump::read_auto_text_file(autonotesfilename.c_str());
		} catch (file_open_exception &err) {
			cerr << "Autonotes file '" << autonotesfilename << "' not found, falling back to default autonotes." << endl;
			Notebook_gump::read_auto_text();
		}
	}

	usecode->read();        // Read the usecode flags
	CYCLE_RED_PLASMA();

	if (Game::get_game_type() == BLACK_GATE) {
		string yn;      // Override from config. file.
		// Skip intro. scene?
		config->value("config/gameplay/skip_intro", yn, "no");
		if (yn == "yes")
			usecode->set_global_flag(
			    Usecode_machine::did_first_scene, 1);

		// Should Avatar be visible?
		if (usecode->get_global_flag(Usecode_machine::did_first_scene))
			main_actor->clear_flag(Obj_flags::bg_dont_render);
		else
			main_actor->set_flag(Obj_flags::bg_dont_render);
	}

	CYCLE_RED_PLASMA();

	// Fade out & clear screen before palette change
	pal->fade_out(c_fade_out_time);
	clear_screen(true);
#ifdef RED_PLASMA
	load_palette_timer = 0;
#endif

	// note: we had to stop the plasma here already, because init_readied
	// and activate_eggs may update the screen through usecode functions
	// (Helm of Light, for example)
	Actor *party[9];
	int cnt = get_party(party, 1);  // Get entire party.
	for (int i = 0; i < cnt; i++) { // Init. rings.
		party[i]->init_readied();
	}
	// Ensure time is stopped if map-editing.
	time_stopped = map_editing ? -1 : 0;
//+++++The below wasn't prev. done by ::read(), so maybe it should be
//+++++controlled by a 'first-time' flag.
	// Want to activate first egg.
	Map_chunk *olist = main_actor->get_chunk();
	olist->setup_cache();

	Tile_coord t = main_actor->get_tile();
	// Do them immediately.
	if (!map_editing)       // (Unless map-editing.)
		olist->activate_eggs(main_actor, t.tx, t.ty, t.tz, -1, -1, true);
	// Force entire repaint.
	set_all_dirty();
	painted = true;         // Main loop uses this.
	gump_man->close_all_gumps(true);        // Kill gumps.
	Face_stats::load_config(config);
	if(using_shortcutbar())
		g_shortcutBar = new ShortcutBar_gump(0,0);

	// Set palette for time-of-day.
	clock->reset();
	clock->set_palette();
	pal->fade(6, 1, -1);        // Fade back in.
}



void Game_window::plasma(int w, int h, int x, int y, int startc, int endc) {
	Image_buffer8 *ibuf = get_win()->get_ib8();

	ibuf->fill8(startc, w, h, x, y);

	for (int i = 0; i < w * h; i++) {
		Uint8 pc = startc + rand() % (endc - startc + 1);
		int px = x + rand() % w;
		int py = y + rand() % h;

		for (int j = 0; j < 6; j++) {
			int px2 = px + rand() % 17 - 8;
			int py2 = py + rand() % 17 - 8;
			ibuf->fill8(pc, 3, 1, px2 - 1, py2);
			ibuf->fill8(pc, 1, 3, px2, py2 - 1);
		}
	}
	painted = true;
}

/*
 *  Chunk caching emulation: swap out chunks which are now at least
 *  3 chunks away.
 */
void Game_window::emulate_cache(Map_chunk *olist, Map_chunk *nlist) {
	if (!olist)
		return;         // Seems like there's nothing to do.
	// Cancel weather from eggs that are
	// far away.
	effects->remove_weather_effects(120);
	int newx = nlist->get_cx(), newy = nlist->get_cy(),
	    oldx = olist->get_cx(), oldy = olist->get_cy();
	Game_map *omap = olist->get_map(), *nmap = nlist->get_map();
	// Cancel scripts 4 chunks from this.
	Usecode_script::purge(Tile_coord(newx * c_tiles_per_chunk,
	                                 newy * c_tiles_per_chunk, 0), 4 * c_tiles_per_chunk);
	int nearby[5][5];       // Chunks within 3.
	int x, y;
	// Figure old range.
	int old_minx = c_num_chunks + oldx - 2,
	    old_maxx = c_num_chunks + oldx + 2;
	int old_miny = c_num_chunks + oldy - 2,
	    old_maxy = c_num_chunks + oldy + 2;
	if (nmap == omap) {     // Same map?
		// Set to 0
		// No casting _should_ be necessary at this point.
		// Who needs this?
		memset(reinterpret_cast<char *>(nearby), 0, sizeof(nearby));
		// Figure new range.
		int new_minx = c_num_chunks + newx - 2,
		    new_maxx = c_num_chunks + newx + 2;
		int new_miny = c_num_chunks + newy - 2,
		    new_maxy = c_num_chunks + newy + 2;
		// Now we write what we are now near
		for (y = new_miny; y <= new_maxy; y++) {
			if (y > old_maxy)
				break;      // Beyond the end.
			int dy = y - old_miny;
			if (dy < 0)
				continue;
			assert(dy < 5);
			for (x = new_minx; x <= new_maxx; x++) {
				if (x > old_maxx)
					break;
				int dx = x - old_minx;
				if (dx >= 0) {
					assert(dx < 5);
					nearby[dx][dy] = 1;
				}
			}
		}
	} else              // New map, so cache out all of old.
		memset(reinterpret_cast<char *>(nearby), 0, sizeof(nearby));

	// Swap out chunks no longer nearby (0).
	Game_object_vector removes;
	for (y = 0; y < 5; y++)
		for (x = 0; x < 5; x++) {
			if (nearby[x][y] != 0)
				continue;
			Map_chunk *list = omap->get_chunk_safely(
			                      (old_minx + x) % c_num_chunks,
			                      (old_miny + y) % c_num_chunks);
			if (!list) continue;
			Object_iterator it(list->get_objects());
			Game_object *each;
			while ((each = it.get_next()) != 0) {
				if (each->is_egg())
					each->as_egg()->reset();
				else if (each->get_flag(Obj_flags::is_temporary))
					removes.push_back(each);
			}
		}
	for (Game_object_vector::const_iterator it = removes.begin();
	        it != removes.end(); ++it) {
#ifdef DEBUG
		Tile_coord t = (*it)->get_tile();
		cout << "Culling object: " << (*it)->get_name() <<
		     '(' << static_cast<void *>(*it) << ")@" <<
		     t.tx << "," << t.ty << "," << t.tz << endl;
#endif
		(*it)->delete_contents();  // first delete item's contents
		(*it)->remove_this(0);
	}

	if (omap == nmap)
		omap->cache_out(newx, newy);
	else                    // Going to new map?
		omap->cache_out(-1, -1);    // Cache out whole of old map.
	// Could cause some problems
	removed->flush();
}

// Tests to see if a move goes out of range of the actors superchunk
bool Game_window::emulate_is_move_allowed(int tx, int ty) {
	int ax = camera_actor->get_cx() / c_chunks_per_schunk;
	int ay = camera_actor->get_cy() / c_chunks_per_schunk;
	tx /= c_tiles_per_schunk;
	ty /= c_tiles_per_schunk;

	int difx = ax - tx;
	int dify = ay - ty;

	if (difx < 0) difx = -difx;
	if (dify < 0) dify = -dify;

	// Is it within 1 superchunk range?
	if ((!difx || difx == 1 || difx == c_num_schunks || difx == c_num_schunks - 1) &&
	        (!dify || dify == 1 || dify == c_num_schunks || dify == c_num_schunks - 1))
		return true;

	return false;
}

//create mini-screenshot (96x60) for use in savegames
Shape_file *Game_window::create_mini_screenshot() {
	Shape_file *sh = 0;
	Shape_frame *fr = 0;
	unsigned char *img = 0;

	set_all_dirty();
	render->paint_map(0, 0, get_width(), get_height());
#ifdef HAVE_OPENGL
	if (GL_manager::get_instance())
		show();
#endif

	img = win->mini_screenshot();

	if (img) {
		fr = new Shape_frame();
		fr->xleft = 0;
		fr->yabove = 0;
		fr->xright = 95;
		fr->ybelow = 59;
		fr->create_rle(img, 96, 60);
		fr->rle = 1;
		delete [] img;

		sh = new Shape_file(fr);
	}

	set_all_dirty();
	paint();
	return sh;
}

#ifdef RED_PLASMA

#define BG_PLASMA_START_COLOR   128
#define BG_PLASMA_CYCLE_RANGE   80

#define SI_PLASMA_START_COLOR   16
#define SI_PLASMA_CYCLE_RANGE   96

void Game_window::setup_load_palette() {
	if (load_palette_timer != 0)
		return;

	if (Game::get_game_type() == BLACK_GATE) {
		plasma_start_color = BG_PLASMA_START_COLOR;
		plasma_cycle_range = BG_PLASMA_CYCLE_RANGE;
	} else { // Default: if (Game::get_game_type()==SERPENT_ISLE)
		plasma_start_color = SI_PLASMA_START_COLOR;
		plasma_cycle_range = SI_PLASMA_CYCLE_RANGE;
	}

	// Put up the plasma to the screen
	plasma(win->get_full_width(), win->get_full_height(), win->get_start_x(), win->get_start_y(), plasma_start_color, plasma_start_color + plasma_cycle_range - 1);

	// Load the palette
	if (Game::get_game_type() == BLACK_GATE)
		pal->load(INTROPAL_DAT, PATCH_INTROPAL, 2);
	else if (Game::get_game_type() == SERPENT_ISLE)
		pal->load(MAINSHP_FLX, PATCH_MAINSHP, 1);

	pal->apply();
	load_palette_timer = SDL_GetTicks();
}

void Game_window::cycle_load_palette() {
	if (load_palette_timer == 0)
		return;
	uint32 ticks = SDL_GetTicks();
	if (ticks > load_palette_timer + 75) {
		for (int i = 0; i < 4; ++i)
			get_win()->rotate_colors(plasma_start_color, plasma_cycle_range, 1);
#ifdef HAVE_OPENGL
		if (GL_manager::get_instance()) {
			int w = get_width(), h = get_height();
			Image_buffer8 *buf = get_win()->get_ib8();
			Shape_frame *screen =
			    new Shape_frame(buf->get_bits(), w, h, 0, 0, true);
			//Shape_manager::get_instance()->paint_shape(0, 0, screen);
			Set_glpalette(0, true);
			GL_manager::get_instance()->paint(screen, 0, 0);
		}
#endif
		show(true);

		// We query the timer here again, as the blit can take easily 50 ms and more
		// depending on the chosen scaler and the overall system speed
		load_palette_timer = SDL_GetTicks();
	}
}
#endif

bool Game_window::is_hostile_nearby() {
	/* If there is a hostile NPC nearby, the avatar isn't allowed to
	 * move very fast
	 * Note that the range at which this occurs in the original is
	 * less than the "potential target" range- that is, if I go into
	 * combat mode, even when I'm allowed to run at full speed,
	 * I'll sometime charge off to kill someone "too far away"
	 * to affect a speed limit.
	 * I don't know whether this is taken into account by
	 * get_nearby_npcs, but on the other hand, its a negligible point.
	 */
	Actor_vector nearby;
	if (!cheat.in_god_mode())
		get_nearby_npcs(nearby);

	bool nearby_hostile = false;
	for (Actor_vector::const_iterator it = nearby.begin(); it != nearby.end(); ++it) {
		Actor *actor = *it;

		if (!actor->is_dead() && actor->get_schedule() &&
		        actor->get_effective_alignment() >= Actor::evil &&
		        ((actor->get_schedule_type() == Schedule::combat &&
		          dynamic_cast<Combat_schedule *>(actor->get_schedule())->has_started_battle()) ||
		         actor->get_schedule_type() == Schedule::arrest_avatar)) {
			/* TODO- I think invisibles still trigger the
			 * slowdown, verify this. */
			nearby_hostile = true;
			break; /* No need to bother checking the rest :P */
		}
	}
	return nearby_hostile;
}

bool Game_window::failed_copy_protection() {
	bool confused = main_actor->get_flag(Obj_flags::confused);
	bool failureFlag = usecode->get_global_flag(56);
	return ((GAME_SI && confused) || (GAME_BG && failureFlag));
}

void Game_window::got_bad_feeling(int odds) {
	if (!(GAME_BG || GAME_SI))
		return;
	if ((rand() % odds) == 0) {
		char const *badfeeling = get_misc_name(GAME_BG ? 0x44 : 0x50);
		effects->remove_text_effect(main_actor);
		effects->add_text(badfeeling, main_actor);
	}
}

void Game_window::set_shortcutbar(uint8 s) {
	if(use_shortcutbar == s || using_shortcutbar() && s > 0)
		return;
	use_shortcutbar = s;
	if(using_shortcutbar()) {
		g_shortcutBar = new ShortcutBar_gump(0,0);
	} else {
		gump_man->remove_gump(g_shortcutBar);
		if(g_shortcutBar)
			delete g_shortcutBar;
		g_shortcutBar = NULL;
	}
}
