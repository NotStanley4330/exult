/**
 ** Gamemap.cc - X-windows Ultima7 map browser.
 **/

/*
 *
 *  Copyright (C) 1998-1999  Jeffrey S. Freedman
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
#	include <config.h>
#endif

#include "gamemap.h"

#include "Flex.h"
#include "actors.h" /* For Dead_body, which should be moved. */
#include "animate.h"
#include "barge.h"
#include "cheat.h"
#include "chunks.h"
#include "databuf.h"
#include "effects.h"
#include "egg.h"
#include "exceptions.h"
#include "fnames.h"
#include "game.h"
#include "gamewin.h" /* With some work, could get rid of this. */
#include "ios_state.hpp"
#include "jawbone.h"
#include "mappatch.h"
#include "objiter.cc" /* Yes we #include the .cc here on purpose! Please don't "fix" this */
#include "objiter.h"
#include "objs.h"
#include "shapeinf.h"
#include "spellbook.h"
#include "ucsched.h"
#include "virstone.h"
#include "weaponinf.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

using std::cerr;
using std::cout;
using std::endl;
using std::ifstream;
using std::ios;
using std::istream;
using std::istringstream;
using std::ofstream;
using std::ostringstream;
using std::pair;
using std::string;
using std::vector;

vector<Chunk_terrain*>*       Game_map::chunk_terrains = nullptr;
std::unique_ptr<std::istream> Game_map::chunks;
bool                          Game_map::v2_chunks               = false;
bool                          Game_map::read_all_terrain        = false;
bool                          Game_map::chunk_terrains_modified = false;
bool                          Game_map::map_modified            = false;

const int   V2_CHUNK_HDR_SIZE = 4 + 4 + 2;    // 0xffff, "exlt", vers.
static char v2hdr[]
		= {static_cast<char>(0xff),
		   static_cast<char>(0xff),
		   static_cast<char>(0xff),
		   static_cast<char>(0xff),
		   'e',
		   'x',
		   'l',
		   't',
		   0,
		   0};

/*
 *  Create a chunk.
 */

Map_chunk* Game_map::create_chunk(int cx, int cy) {
	objects[cx][cy] = std::make_unique<Map_chunk>(this, cx, cy);
	return get_chunk_unsafe(cx, cy);
}

/*
 *  Read in a terrain chunk.
 */

Chunk_terrain* Game_map::read_terrain(
		int chunk_num    // Want this one from u7chunks.
) {
	const int ntiles = c_tiles_per_chunk * c_tiles_per_chunk;
	assert(chunk_num >= 0
		   && static_cast<unsigned>(chunk_num) < chunk_terrains->size());
	unsigned char buf[ntiles * 3];
	if (v2_chunks) {
		chunks->seekg(V2_CHUNK_HDR_SIZE + chunk_num * ntiles * 3);
		chunks->read(reinterpret_cast<char*>(buf), ntiles * 3);
	} else {
		chunks->seekg(chunk_num * ntiles * 2);
		chunks->read(reinterpret_cast<char*>(buf), ntiles * 2);
	}
	auto* ter = new Chunk_terrain(&buf[0], v2_chunks);
	if (static_cast<unsigned>(chunk_num) >= chunk_terrains->size()) {
		chunk_terrains->resize(chunk_num + 1);
	}
	(*chunk_terrains)[chunk_num] = ter;
	return ter;
}

/*
 *  Create game window.
 */

Game_map::Game_map(int n)
		: num(n), didinit(false), caching_out(0),
		  map_patches(std::make_unique<Map_patch_collection>()) {}

/*
 *  Deleting map.
 */

Game_map::~Game_map() {
	clear();    // Delete all objects, chunks.
}

/*
 *  Init. the static data.
 */

void Game_map::init_chunks() {
	int        num_chunk_terrains;
	const bool patch_exists = is_system_path_defined("<PATCH>");
	if (patch_exists && U7exists(PATCH_U7CHUNKS)) {
		chunks = U7open_in(PATCH_U7CHUNKS);
	} else {
		try {
			chunks = U7open_in(U7CHUNKS);
		} catch (const file_exception&) {
			if (!Game::is_editing() ||    // Ok if map-editing.
				!patch_exists) {          // But only if patch exists.
				throw;
			}
			auto pOchunks
					= U7open_out(PATCH_U7CHUNKS);    // Create one in 'patch'.
			if (!pOchunks) {
				throw file_write_exception(PATCH_U7CHUNKS);
			}
			auto& ochunks = *pOchunks;
			ochunks.write(v2hdr, sizeof(v2hdr));
			unsigned char buf[16 * 16 * 3]{};
			ochunks.write(reinterpret_cast<char*>(buf), sizeof(buf));
			pOchunks.reset();
			chunks = U7open_in(PATCH_U7CHUNKS);
		}
	}
	char v2buf[V2_CHUNK_HDR_SIZE];    // Check for V2.
	chunks->read(v2buf, sizeof(v2buf));
	int hdrsize = 0;
	int chunksz = c_tiles_per_chunk * c_tiles_per_chunk * 2;
	if (memcmp(v2hdr, v2buf, sizeof(v2buf)) == 0) {
		v2_chunks = true;
		hdrsize   = V2_CHUNK_HDR_SIZE;
		chunksz   = c_tiles_per_chunk * c_tiles_per_chunk * 3;
	}
	// Get to end so we can get length.
	chunks->seekg(0, ios::end);
	// 2 bytes/tile.
	num_chunk_terrains
			= (static_cast<int>(chunks->tellg()) - hdrsize) / chunksz;
	if (!chunk_terrains) {
		chunk_terrains = new vector<Chunk_terrain*>();
	}
	// Resize list to hold all.
	chunk_terrains->resize(num_chunk_terrains);
	read_all_terrain = false;
}

/*
 *  Initialize for new/restored game.
 */

void Game_map::init() {
	char fname[128];

	if (num == 0) {
		init_chunks();
	}
	map_modified = false;
	std::unique_ptr<std::istream> pU7map;    // Read in map.
	bool                          nomap = false;
	if (is_system_path_defined("<PATCH>")
		&& U7exists(get_mapped_name(PATCH_U7MAP, fname))) {
		pU7map = U7open_in(fname);
	} else {
		try {
			pU7map = U7open_in(get_mapped_name(U7MAP, fname));
		} catch (const file_exception& /*f*/) {
			if (!Game::is_editing()) {    // Ok if map-editing.
				cerr << "Map file '" << fname << "' not found." << endl;
			}
			nomap = true;
		}
	}
	if (!pU7map) {
		nomap = true;
	}
	for (int schunk = 0; schunk < c_num_schunks * c_num_schunks; schunk++) {
		// Read in the chunk #'s.
		unsigned char buf[16 * 16 * 2];
		if (nomap) {
			std::fill(std::begin(buf), std::end(buf), 0);
		} else {
			pU7map->read(reinterpret_cast<char*>(buf), sizeof(buf));
		}
		const int    scy     = 16 * (schunk / 12);    // Get abs. chunk coords.
		const int    scx     = 16 * (schunk % 12);
		const uint8* mapdata = buf;
		// Go through chunks.
		for (int cy = 0; cy < 16; cy++) {
			for (int cx = 0; cx < 16; cx++) {
				terrain_map[scx + cx][scy + cy] = little_endian::Read2(mapdata);
			}
		}
	}
	// Clear object lists, flags.
	for (auto& row : objects) {
		for (auto& obj : row) {
			obj.reset();
		}
	}
	std::fill(std::begin(schunk_read), std::end(schunk_read), false);
	std::fill(std::begin(schunk_modified), std::end(schunk_modified), false);
	std::fill(std::begin(schunk_cache), std::end(schunk_cache), nullptr);
	std::fill(std::begin(schunk_cache_sizes), std::end(schunk_cache_sizes), -1);

	didinit = true;
}

/*
 *  Clear the static data.
 */

void Game_map::clear_chunks() {
	if (chunk_terrains) {
		const int cnt = chunk_terrains->size();
		for (int i = 0; i < cnt; i++) {
			delete (*chunk_terrains)[i];
		}
		delete chunk_terrains;
		chunk_terrains = nullptr;
	}
	chunks.reset();    // Close 'u7chunks'.
	read_all_terrain = false;
}

/*
 *  Clear out world's contents.  Should be used during a 'restore'.
 */

void Game_map::clear() {
	if (num == 0) {
		clear_chunks();
	}

	if (didinit) {
		// Delete all chunks (& their objs).
		for (auto* i : schunk_cache) {
			delete[] i;
		}
	}
	for (auto& row : objects) {
		for (auto& obj : row) {
			obj.reset();
		}
	}
	didinit      = false;
	map_modified = false;
	// Clear 'read' flags.
	std::fill(std::begin(schunk_read), std::end(schunk_read), false);
	std::fill(std::begin(schunk_modified), std::end(schunk_modified), false);
	std::fill(std::begin(schunk_cache), std::end(schunk_cache), nullptr);
	std::fill(std::begin(schunk_cache_sizes), std::end(schunk_cache_sizes), -1);
}

/*
 *  Read in superchunk data to cover the screen.
 */

void Game_map::read_map_data() {
	Game_window* gwin     = Game_window::get_instance();
	const int    scrolltx = gwin->get_scrolltx();
	const int    scrollty = gwin->get_scrollty();
	const int    w        = gwin->get_width();
	const int    h        = gwin->get_height();
	// Start one tile to left.
	const int firstsx = (scrolltx - 1) / c_tiles_per_schunk;
	const int firstsy = (scrollty - 1) / c_tiles_per_schunk;
	// End 8 tiles to right.
	// These 1 added to the chunk limits reflect the Game_render::paint_map
	//       1 added to the chunk limits for the Smooth Scrolling.
	// This prevents a crash in the Edit Terrain mode when jumping
	//   to a chunk near a superchunk boundary.
	const int lastsx = (1
						+ (scrolltx + (w + c_tilesize - 2) / c_tilesize
						   + c_tiles_per_chunk / 2)
								  / c_tiles_per_chunk)
					   / c_chunks_per_schunk;
	const int lastsy = (1
						+ (scrollty + (h + c_tilesize - 2) / c_tilesize
						   + c_tiles_per_chunk / 2)
								  / c_tiles_per_chunk)
					   / c_chunks_per_schunk;
	// Watch for wrapping.
	const int stopsx = (lastsx + 1) % c_num_schunks;
	const int stopsy = (lastsy + 1) % c_num_schunks;
	// Read in "map", "ifix" objects for
	//  all visible superchunks.
	for (int sy = firstsy; sy != stopsy; sy = (sy + 1) % c_num_schunks) {
		for (int sx = firstsx; sx != stopsx; sx = (sx + 1) % c_num_schunks) {
			// Figure superchunk #.
			const int schunk = 12 * sy + sx;
			// Read it if necessary.
			if (!schunk_read[schunk]) {
				get_superchunk_objects(schunk);
			}
		}
	}
}

/*
 *  Get the map objects and scenery for a superchunk.
 */

void Game_map::get_map_objects(int schunk    // Superchunk # (0-143).
) {
	const int scy = 16 * (schunk / 12);    // Get abs. chunk coords.
	const int scx = 16 * (schunk % 12);
	// Go through chunks.
	for (int cy = 0; cy < 16; cy++) {
		for (int cx = 0; cx < 16; cx++) {
			get_chunk_objects(scx + cx, scy + cy);
		}
	}
}

/*
 *  Read in terrain graphics data into window's image.  (May also be
 *  called during map-editing if the chunknum changes.)
 */

void Game_map::get_chunk_objects(
		int cx, int cy    // Chunk index within map.
) {
	// Get list we'll store into.
	Map_chunk*     chunk     = get_chunk(cx, cy);
	const int      chunk_num = terrain_map[cx][cy];
	Chunk_terrain* ter       = get_terrain(chunk_num);
	chunk->set_terrain(ter);
}

/*
 *  Read in all terrain chunks (for editing).
 */

void Game_map::get_all_terrain() {
	if (read_all_terrain) {
		return;    // Already done.
	}
	const int num_chunk_terrains = chunk_terrains->size();
	for (int i = 0; i < num_chunk_terrains; i++) {
		if (!(*chunk_terrains)[i]) {
			read_terrain(i);
		}
	}
	read_all_terrain = true;
}

/*
 *  Set a chunk to a new terrain (during map-editing).
 */

void Game_map::set_chunk_terrain(
		int cx, int cy,    // Coords. of chunk to change.
		int chunknum       // New chunk #.
) {
	terrain_map[cx][cy] = chunknum;    // Set map.
	get_chunk_objects(cx, cy);         // Set chunk to it.
	map_modified = true;
}

/*
 *  Build a file name with the map directory before it; ie,
 *      get_mapped_name("<GAMEDAT>/ireg, 3, to) will store
 *          "<GAMEDAT>/map03/ireg".
 */

char* Game_map::get_mapped_name(const char* from, char* to) {
	return Get_mapped_name(from, num, to);
}

/*
 *  Get the name of an ireg or ifix file.
 *
 *  Output: ->fname, where name is stored.
 */

char* Game_map::get_schunk_file_name(
		const char* prefix,    // "ireg" or "ifix".
		int         schunk,    // Superchunk # (0-143).
		char*       fname      // Name is stored here.
) {
	get_mapped_name(prefix, fname);
	const int                   len      = strlen(fname);
	constexpr static const char hexLUT[] = "0123456789abcdef";
	fname[len]                           = hexLUT[schunk / 16];
	fname[len + 1]                       = hexLUT[schunk % 16];
	fname[len + 2]                       = 0;
	return fname;
}

/*
 *  Have shapes been added?
 */

static bool New_shapes() {
	const int u7nshapes = GAME_SI ? 1036 : 1024;
	const int nshapes
			= Shape_manager::get_instance()->get_shapes().get_num_shapes();
	return nshapes > u7nshapes;
}

/*
 *  Write out the chunks descriptions.
 */

void Game_map::write_chunk_terrains() {
	const int ntiles = c_tiles_per_chunk * c_tiles_per_chunk;
	const int cnt    = chunk_terrains->size();
	int       i;    // Any terrains modified?
	for (i = 0; i < cnt; i++) {
		if ((*chunk_terrains)[i] && (*chunk_terrains)[i]->is_modified()) {
			break;
		}
	}
	if (i < cnt) {            // Got to update.
		get_all_terrain();    // IMPORTANT:  Get all in memory.
		// Open file for chunks data.
		// This truncates the file.
		auto pOchunks = U7open_out(PATCH_U7CHUNKS);
		if (!pOchunks) {
			throw file_write_exception(U7CHUNKS);
		}
		auto& ochunks    = *pOchunks;
		v2_chunks        = New_shapes();
		const int nbytes = v2_chunks ? 3 : 2;
		if (v2_chunks) {
			ochunks.write(v2hdr, sizeof(v2hdr));
		}
		for (i = 0; i < cnt; i++) {
			Chunk_terrain* ter = (*chunk_terrains)[i];
			unsigned char  data[ntiles * 3];
			if (ter) {
				ter->write_flats(data, v2_chunks);
				ter->set_modified(false);
			} else {
				std::fill_n(data, ntiles * nbytes, 0);
				cerr << "nullptr terrain.  U7chunks may be bad." << endl;
			}
			ochunks.write(reinterpret_cast<char*>(data), ntiles * nbytes);
		}
		if (!ochunks.good()) {
			throw file_write_exception(U7CHUNKS);
		}
	}
	chunk_terrains_modified = false;
}

/*
 *  Write out the 'static' map files.
 */

void Game_map::write_static() {
	char fname[128];
	U7mkdir("<PATCH>", 0755);    // Create dir if not already there.
	//  Don't use PATCHDAT define cause
	//  it has a trailing slash

	int schunk;    // Write each superchunk to 'static'.
	for (schunk = 0; schunk < c_num_schunks * c_num_schunks; schunk++) {
		// Only write what we've modified.
		if (schunk_modified[schunk]) {
			write_ifix_objects(schunk);
		}
	}
	if (chunk_terrains_modified) {
		write_chunk_terrains();
	}
	// Write out map.
	auto pU7map = U7open_out(get_mapped_name(PATCH_U7MAP, fname));
	if (!pU7map) {
		throw file_write_exception(U7MAP);
	}
	auto& u7map = *pU7map;
	for (schunk = 0; schunk < c_num_schunks * c_num_schunks; schunk++) {
		const int scy = 16 * (schunk / 12);    // Get abs. chunk coords.
		const int scx = 16 * (schunk % 12);
		uint8     buf[16 * 16 * 2];
		uint8*    mapdata = buf;
		// Go through chunks.
		for (int cy = 0; cy < 16; cy++) {
			for (int cx = 0; cx < 16; cx++) {
				little_endian::Write2(mapdata, terrain_map[scx + cx][scy + cy]);
			}
		}
		u7map.write(reinterpret_cast<char*>(buf), sizeof(buf));
	}
	if (!u7map.good()) {
		throw file_write_exception(U7MAP);
	}
	map_modified = false;
}

/*
 *  Write out one of the "u7ifix" files.
 *
 *  Output: Errors reported.
 */

void Game_map::write_ifix_objects(int schunk    // Superchunk # (0-143).
) {
	char            fname[128];    // Set up name.
	OFileDataSource ifix(get_schunk_file_name(PATCH_U7IFIX, schunk, fname));
	// +++++Use game title.
	const int count = c_chunks_per_schunk * c_chunks_per_schunk;

	// Always save as V2 to support 8 bit lift
	bool        v2 = true;
	Flex_writer writer(ifix, "Exult", count, Flex_header::exult_v2);
	const int   scy = 16 * (schunk / 12);    // Get abs. chunk coords.
	const int   scx = 16 * (schunk % 12);
	// Go through chunks.
	for (int cy = 0; cy < 16; cy++) {
		for (int cx = 0; cx < 16; cx++) {
			writer.write_object(get_chunk(scx + cx, scy + cy), v2);
		}
	}
	schunk_modified[schunk] = false;
}

/*
 *  Read in the objects for a superchunk from one of the "u7ifix" files.
 */

void Game_map::get_ifix_objects(int schunk    // Superchunk # (0-143).
) {
	char fname[128];    // Set up name.
	if (!is_system_path_defined("<PATCH>") ||
		// First check for patch.
		!U7exists(get_schunk_file_name(PATCH_U7IFIX, schunk, fname))) {
		get_schunk_file_name(U7IFIX, schunk, fname);
	}
	IFileDataSource ifix(fname);
	if (!ifix.good()) {
		if (!Game::is_editing()) {    // Ok if map-editing.
			cerr << "Ifix file '" << fname << "' not found." << endl;
		}
		return;
	}
	FlexFile  flex(fname);
	const int vers = static_cast<int>(flex.get_vers());
	const int scy  = 16 * (schunk / 12);    // Get abs. chunk coords.
	const int scx  = 16 * (schunk % 12);
	// Go through chunks.
	for (int cy = 0; cy < 16; cy++) {
		for (int cx = 0; cx < 16; cx++) {
			// Get to index entry for chunk.
			const int    chunk_num = cy * 16 + cx;
			size_t       len;
			const uint32 offset = flex.get_entry_info(chunk_num, len);
			if (len) {
				get_ifix_chunk_objects(
						&ifix, vers, offset, len, scx + cx, scy + cy);
			}
		}
	}
}

/*
 *  Get the objects from one ifix chunk entry onto the screen.
 */

void Game_map::get_ifix_chunk_objects(
		IDataSource* ifix,
		int          vers,       // Flex file vers.
		long         filepos,    // Offset in file.
		int          len,        // Length of data.
		int cx, int cy           // Absolute chunk #'s.
) {
	Game_object_shared obj;
	ifix->seek(filepos);    // Get to actual shape.
	// Get buffer to hold entries' indices.
	auto           entries = ifix->readN(len);    // Read them in.
	unsigned char* ent     = entries.get();
	// Get object list for chunk.
	Map_chunk* olist = get_chunk(cx, cy);
	if (static_cast<Flex_header::Flex_vers>(vers) == Flex_header::orig) {
		const int cnt = len / 4;
		for (int i = 0; i < cnt; i++, ent += 4) {
			int               tx    = (ent[0] >> 4) & 0xf;
			int               ty    = ent[0] & 0xf;
			int               tz    = ent[1] & 0xf;
			int               shnum = ent[2] + 256 * (ent[3] & 3);
			int               frnum = ent[3] >> 2;
			const Shape_info& info  = ShapeID::get_info(shnum);
			obj                     = (info.is_animated() || info.has_sfx())
											  ? std::make_shared<Animated_ifix_object>(
                                    shnum, frnum, tx, ty, tz)
											  : std::make_shared<Ifix_game_object>(
                                    shnum, frnum, tx, ty, tz);
			olist->add(obj.get());
		}
	} else if (
			static_cast<Flex_header::Flex_vers>(vers)
			== Flex_header::exult_v2) {
		// b0 = tx,ty, b1 = lift, b2-3 = shnum, b4=frnum
		const int cnt = len / 5;
		for (int i = 0; i < cnt; i++, ent += 5) {
			int               tx    = (ent[0] >> 4) & 0xf;
			int               ty    = ent[0] & 0xf;
			int               tz    = ent[1];    // allow full 8 bit lift in v2
			int               shnum = ent[2] + 256 * ent[3];
			int               frnum = ent[4];
			const Shape_info& info  = ShapeID::get_info(shnum);
			obj                     = (info.is_animated() || info.has_sfx())
											  ? std::make_shared<Animated_ifix_object>(
                                    shnum, frnum, tx, ty, tz)
											  : std::make_shared<Ifix_game_object>(
                                    shnum, frnum, tx, ty, tz);
			olist->add(obj.get());
		}
	} else {
		assert(0);
	}
	olist->setup_dungeon_levels();    // Should have all dungeon pieces now.
}

/*
 *  Constants for IREG files:
 */
#define IREG_SPECIAL  255    // Precedes special entries.
#define IREG_UCSCRIPT 1      // Saved Usecode_script for object.
#define IREG_ENDMARK  2      // Just an 'end' mark.
#define IREG_ATTS     3      // Attribute/value pairs.
#define IREG_STRING   4      // A string; ie, function name.

/*
 *  Write out attributes for an object.
 */

void Game_map::write_attributes(
		ODataSource* ireg, vector<pair<const char*, int>>& attlist) {
	int       len = 0;    // Figure total length.
	int       i;
	const int cnt = attlist.size();
	if (!cnt) {
		return;
	}
	for (i = 0; i < cnt; ++i) {
		const char* att = attlist[i].first;
		len += strlen(att) + 1 + 2;    // Name, nullptr, val.
	}
	ireg->write1(IREG_SPECIAL);
	ireg->write1(IREG_ATTS);
	ireg->write2(len);
	for (i = 0; i < cnt; ++i) {
		const char* att = attlist[i].first;
		const int   val = attlist[i].second;
		ireg->write(att, strlen(att) + 1);
		ireg->write2(val);
	}
}

/*
 *  Write out scheduled usecode for an object.
 */

void Game_map::write_scheduled(
		ODataSource* ireg, Game_object* obj,
		bool write_mark    // Write an IREG_ENDMARK if true.
) {
	for (Usecode_script* scr = Usecode_script::find(obj); scr;
		 scr                 = Usecode_script::find(obj, scr)) {
		ostringstream     outbuf(ios::out);
		OStreamDataSource nbuf(&outbuf);
		const int         len = scr->save(&nbuf);
		if (len < 0) {
			cerr << "Error saving Usecode script" << endl;
		} else if (len > 0) {
			ireg->write1(IREG_SPECIAL);
			ireg->write1(IREG_UCSCRIPT);
			ireg->write2(len);    // Store length.
			ireg->write(outbuf.str());
		}
	}
	if (write_mark) {
		ireg->write1(IREG_SPECIAL);
		ireg->write1(IREG_ENDMARK);
	}
}

/*
 *  Write string entry and/or return length of what's written.
 */
int Game_map::write_string(
		ODataSource* ireg,    // Null if we just want length.
		const char*  str) {
	const int len = 1 + strlen(str);
	if (ireg) {
		ireg->write1(IREG_SPECIAL);
		ireg->write1(IREG_STRING);
		ireg->write2(len);
		ireg->write(str, len);
	}
	return len + 4;
}

/*
 *  Write modified 'u7ireg' files.
 */

void Game_map::write_ireg() {
	// Write each superchunk to Iregxx.
	for (int schunk = 0; schunk < c_num_schunks * c_num_schunks; schunk++) {
		// Only write what we've read.
		if (schunk_cache[schunk] && schunk_cache_sizes[schunk] >= 0) {
			// It's loaded in a memory buffer
			char fname[128];    // Set up name.
			auto ireg_stream
					= U7open_out(get_schunk_file_name(U7IREG, schunk, fname));
			if (ireg_stream) {
				ireg_stream->write(
						schunk_cache[schunk], schunk_cache_sizes[schunk]);
			}
		} else if (schunk_read[schunk]) {
			// It's active
			write_ireg_objects(schunk);
		}
	}
}

/*
 *  Write out one of the "u7ireg" files.
 *
 *  Output: 0 if error, which is reported.
 */

void Game_map::write_ireg_objects(int schunk    // Superchunk # (0-143).
) {
	char            fname[128];    // Set up name.
	OFileDataSource ireg(get_schunk_file_name(U7IREG, schunk, fname));
	write_ireg_objects(schunk, &ireg);
	ireg.flush();
}

/*
 *  Write out one of the "u7ireg" files.
 *
 *  Output: 0 if error, which is reported.
 */

void Game_map::write_ireg_objects(
		int          schunk,    // Superchunk # (0-143).
		ODataSource* ireg) {
	const int scy = 16 * (schunk / 12);    // Get abs. chunk coords.
	const int scx = 16 * (schunk % 12);
	// Go through chunks.
	for (int cy = 0; cy < 16; cy++) {
		for (int cx = 0; cx < 16; cx++) {
			Map_chunk*   chunk = get_chunk(scx + cx, scy + cy);
			Game_object* obj;
			// Restore original order (sort of).
			Object_iterator_backwards next(chunk);
			while ((obj = next.get_next()) != nullptr) {
				obj->write_ireg(ireg);
			}
			ireg->write2(0);    // End with 2 0's.
		}
	}
}

/*
 *  Read in the objects for a superchunk from one of the "u7ireg" files.
 *  (These are the moveable objects.)
 */

void Game_map::get_ireg_objects(int schunk    // Superchunk # (0-143).
) {
	char                         fname[128];    // Set up name.
	std::unique_ptr<IDataSource> ireg;

	if (schunk_cache[schunk] && schunk_cache_sizes[schunk] >= 0) {
		// No items
		if (schunk_cache_sizes[schunk] == 0) {
			return;
		}
		ireg = std::make_unique<IBufferDataView>(
				schunk_cache[schunk], schunk_cache_sizes[schunk]);
#ifdef DEBUG
		std::cout << "Reading " << get_schunk_file_name(U7IREG, schunk, fname)
				  << " from memory" << std::endl;
#endif
	} else {
		ireg = std::make_unique<IFileDataSource>(
				get_schunk_file_name(U7IREG, schunk, fname));
		if (!ireg->good()) {
			return;    // Just don't show them.
		}
	}
	const int scy = 16 * (schunk / 12);    // Get abs. chunk coords.
	const int scx = 16 * (schunk % 12);
	read_ireg_objects(ireg.get(), scx, scy);
	// A fixup:
	if (schunk == 10 * 12 + 11 && Game::get_game_type() == SERPENT_ISLE) {
		// Lever in SilverSeed:
		Game_object_vector vec;
		if (Game_object::find_nearby(
					vec, Tile_coord(2936, 2726, 0), 787, 0, 0, c_any_qual, 5)) {
			vec[0]->move(2937, 2727, 2);
		}
	}
	if (schunk_cache[schunk]) {
		delete[] schunk_cache[schunk];
		schunk_cache[schunk]       = nullptr;
		schunk_cache_sizes[schunk] = -1;
	}
}

/*
 *  Read in a 'special' IREG entry (one starting with 255).
 */

void Read_special_ireg(
		IDataSource* ireg,
		Game_object* obj    // Last object read.
) {
	const int type = ireg->read1();    // Get type.
	const int len  = ireg->read2();    // Length of rest.
	auto      data = ireg->readN(len);
	if (!obj) {
		// Something went wrong if we get here, but still.
		// Just return discarding the data read.
		cerr << "Special IREG entry attached to NULL object!" << endl;
		return;
	}
	auto* buf = data.get();
	if (type == IREG_UCSCRIPT) {    // Usecode script?
		IBufferDataView nbuf(buf, len);
		Usecode_script* scr = Usecode_script::restore(obj, &nbuf);
		if (scr) {
			scr->start(scr->get_delay());
		}
	} else if (type == IREG_ATTS) {    // Attribute/value pairs?
		obj->read_attributes(buf, len);
	} else if (type == IREG_STRING) {    // IE, Usecode egg function name?
		if (obj->is_egg()) {
			static_cast<Egg_object*>(obj)->set_str1(
					reinterpret_cast<char*>(buf));
		}
	} else {
		cerr << "Unknown special IREG entry: " << type << endl;
	}
}

/*
 *  Read in a 'special' IREG entry (one starting with 255).
 */

void Game_map::read_special_ireg(
		IDataSource* ireg,
		Game_object* obj    // Last object read.
) {
	while (ireg->peek() == IREG_SPECIAL && !ireg->eof()) {
		ireg->read1();    // Eat the IREG_SPECIAL.
		const unsigned char type = ireg->peek();
		if (type == IREG_ENDMARK) {
			// End of list.
			ireg->read1();
			return;
		}
		Read_special_ireg(ireg, obj);
	}
}

/*
 *  Containers and items classed as 'quality_flags' have a byte of flags.
 *  This routine returns them converted into Object_flags.
 */
inline unsigned long Get_quality_flags(
		unsigned char qualbyte    // Quality byte containing flags.
) {
	return ((qualbyte & 1) << Obj_flags::invisible)
		   | (((qualbyte >> 3) & 1) << Obj_flags::okay_to_take);
}

/*
 *  Read a list of ireg objects.  They are either placed in the desired
 *  game chunk, or added to their container.
 */

void Game_map::read_ireg_objects(
		IDataSource* ireg,          // File to read from.
		int scx, int scy,           // Abs. chunk coords. of superchunk.
		Game_object*  container,    // Container, or null.
		unsigned long flags         // Usecode item flags.
) {
	unsigned char entbuf[20];
	int           entlen;    // Gets entry length.
	sint8         index_id = -1;
	Game_object*  last_obj = nullptr;    // Last one read in this call.
	Game_window*  gwin     = Game_window::get_instance();
	// Go through entries.
	for (entlen = ireg->read1(); !ireg->eof(); entlen = ireg->read1()) {
		int  extended      = 0;    // 1 for 2-byte shape #'s.
		bool extended_lift = false;

		// Skip 0's & ends of containers.

		if (!entlen || entlen == 1) {
			if (container) {
				return;    // Skip 0's & ends of containers.
			} else {
				continue;
			}
		}
		// Detect the 2 byte index id
		else if (entlen == 2) {
			index_id = static_cast<sint8>(ireg->read2());
			continue;
		} else if (entlen == IREG_SPECIAL) {
			Read_special_ireg(ireg, last_obj);
			continue;
		} else if (entlen == IREG_EXTENDED || entlen == IREG_EXTENDED2) {
			if (entlen == IREG_EXTENDED) {
				extended = 1;
			}
			extended_lift = true;
			entlen        = ireg->read1();
		}
		// Get copy of flags.
		unsigned long oflags  = flags & ~(1 << Obj_flags::is_temporary);
		const int     testlen = entlen - extended;
		if (testlen != 6 && testlen != 10 && testlen != 12 && testlen != 13
			&& testlen != 14 && testlen != 18) {
			const long pos = ireg->getPos();
			cout << "Unknown entlen " << testlen << " at pos. " << pos << endl;
			ireg->seek(pos + entlen);
			continue;    // Only know these two types.
		}
		unsigned char* entry = &entbuf[0];    // Get entry.
		ireg->read(reinterpret_cast<char*>(entry), entlen);
		const int cx = entry[0] >> 4;    // Get chunk indices within schunk.
		const int cy = entry[1] >> 4;
		// Get coord. #'s where shape goes.
		int tilex;
		int tiley;
		if (container) {    // In container?  Get gump coords.
			tilex = entry[0];
			tiley = entry[1];
		} else {
			tilex = entry[0] & 0xf;
			tiley = entry[1] & 0xf;
		}
		int shnum;
		int frnum;    // Get shape #, frame #.
		if (extended) {
			shnum = entry[2] + 256 * entry[3];
			frnum = entry[4];
			++entry;    // So the rest is in the right place.
		} else {
			shnum = entry[2] + 256 * (entry[3] & 3);
			frnum = entry[3] >> 2;
		}
		const Shape_info&       info = ShapeID::get_info(shnum);
		unsigned int            quality;
		Ireg_game_object_shared obj;
		int                     is_egg = 0;    // Fields are eggs.

		// Has flag byte(s)
		if (testlen == 10) {
			// Temporary
			if (entry[6] & 1) {
				oflags |= 1 << Obj_flags::is_temporary;
			}
		}

		auto read_lift = [extended_lift](unsigned char val) {
			const unsigned int lift = nibble_swap(val);
			if (extended_lift) {
				return lift;
			}
			return lift & 0xfU;
		};
		// An "egg"?
		if (info.get_shape_class() == Shape_info::hatchable) {
			const bool              anim = info.is_animated() || info.has_sfx();
			const unsigned int      lift = read_lift(entry[9]);
			const Egg_object_shared egg  = Egg_object::create_egg(
                    entry, entlen, anim, shnum, frnum, tilex, tiley, lift);
			get_chunk(scx + cx, scy + cy)->add_egg(egg.get());
			last_obj = egg.get();
			continue;
		} else if (testlen == 6 || testlen == 10) {    // Simple entry?
			const unsigned int lift = read_lift(entry[4]);
			quality                 = entry[5];
			obj    = create_ireg_object(info, shnum, frnum, tilex, tiley, lift);
			is_egg = obj->is_egg();

			// Wierd use of flag:
			if (info.has_quantity()) {
				if (quality & 0x80) {
					oflags |= (1 << Obj_flags::okay_to_take);
					quality &= 0x7f;
				} else {
					oflags &= ~(1 << Obj_flags::okay_to_take);
				}
			} else if (info.has_quality_flags()) {
				// Use those flags instead of deflt.
				oflags  = Get_quality_flags(quality);
				quality = 0;
			}
		} else if (info.is_body_shape()) {
			// NPC's body.
			const int          extbody = testlen == 13 ? 1 : 0;
			const unsigned int type    = entry[4] + 256 * entry[5];
			const unsigned int lift    = read_lift(entry[9 + extbody]);
			quality                    = entry[7];
			oflags                     =    // Override flags (I think).
					Get_quality_flags(entry[11 + extbody]);
			int npc_num;
			if (quality == 1
				&& (extbody
					|| (entry[8] >= 0x80
						|| Game::get_game_type() == SERPENT_ISLE))) {
				npc_num = extbody ? (entry[8] + 256 * entry[9])
								  : ((entry[8] - 0x80) & 0xFF);
			} else {
				npc_num = -1;
			}
			if (!npc_num) {    // Avatar has no body.
				npc_num = -1;
			}
			const Dead_body_shared b = std::make_shared<Dead_body>(
					shnum, frnum, tilex, tiley, lift, npc_num);
			obj = b;
			if (npc_num > 0) {
				gwin->set_body(npc_num, b.get());
			}
			if (type) {    // (0 if empty.)
				// Don't pass along invisibility!
				read_ireg_objects(
						ireg, scx, scy, obj.get(),
						oflags & ~(1 << Obj_flags::invisible));
				obj->elements_read();
			}
		} else if (testlen == 12) {    // Container?
			unsigned int       type = entry[4] + 256 * entry[5];
			const unsigned int lift = read_lift(entry[9]);
			quality                 = entry[7];
			oflags                  =    // Override flags (I think).
					Get_quality_flags(entry[11]);
			if (info.get_shape_class() == Shape_info::virtue_stone) {
				// Virtue stone?
				const std::shared_ptr<Virtue_stone_object> v
						= std::make_shared<Virtue_stone_object>(
								shnum, frnum, tilex, tiley, lift);
				v->set_target_pos(entry[4], entry[5], entry[6], entry[7]);
				v->set_target_map(entry[10]);
				obj  = v;
				type = 0;
			} else if (info.get_shape_class() == Shape_info::barge) {
				const std::shared_ptr<Barge_object> b
						= std::make_shared<Barge_object>(
								shnum, frnum, tilex, tiley, lift, entry[4],
								entry[5], (quality >> 1) & 3);
				obj = b;
				if (!gwin->get_moving_barge() && (quality & (1 << 3))) {
					gwin->set_moving_barge(b.get());
				}
			} else if (info.is_jawbone()) {    // serpent jawbone
				obj = std::make_shared<Jawbone_object>(
						shnum, frnum, tilex, tiley, lift, entry[10]);
			} else {
				obj = std::make_shared<Container_game_object>(
						shnum, frnum, tilex, tiley, lift, entry[10]);
			}
			// Read container's objects.
			if (type) {    // (0 if empty.)
				// Don't pass along invisibility!
				read_ireg_objects(
						ireg, scx, scy, obj.get(),
						oflags & ~(1 << Obj_flags::invisible));
				obj->elements_read();
			}
		} else if (info.get_shape_class() == Shape_info::spellbook) {
			// Length 18 means it's a spellbook.
			// Get all 9 spell bytes.
			quality = 0;
			unsigned char circles[9];
			memcpy(&circles[0], &entry[4], 5);
			const unsigned int lift = read_lift(entry[9]);
			memcpy(&circles[5], &entry[10], 4);
			uint8* ptr = &entry[14];
			// 3 unknowns, then bookmark.
			unsigned char bmark = ptr[3];
			obj                 = std::make_shared<Spellbook_object>(
                    shnum, frnum, tilex, tiley, lift, &circles[0], bmark);
		} else {
			// Just to shut up spurious warnings by compilers and static
			// analyzers.
			const boost::io::ios_flags_saver sflags(cerr);
			const boost::io::ios_fill_saver  sfill(cerr);
			std::cerr << "Error: Invalid IREG entry on chunk (" << scx << ", "
					  << scy << "): extended = " << extended
					  << ", extended_lift = " << extended_lift
					  << ", entlen = " << entlen << ", shnum = " << shnum
					  << ", frnum = " << frnum << std::endl;
			std::cerr << "Entry data:" << std::hex;
			std::cerr << std::setfill('0');
			for (int i = 0; i < entlen; i++) {
				std::cerr << " " << std::setw(2) << static_cast<int>(entry[i]);
			}
			std::cerr << std::endl;
			continue;
		}
		obj->set_quality(quality);
		obj->set_flags(oflags);
		last_obj = obj.get();    // Save as last read.
		// Add, but skip volume check.
		if (container) {
			if (index_id != -1
				&& container->add_readied(obj.get(), index_id, true, true)) {
				continue;
			} else if (container->add(obj.get(), true)) {
				continue;
			} else {    // Fix tx, ty.
				obj->set_shape_pos(obj->get_tx() & 0xf, obj->get_ty() & 0xf);
			}
		}
		Map_chunk* chunk = get_chunk(scx + cx, scy + cy);
		if (is_egg) {
			chunk->add_egg(obj->as_egg());
		} else {
			chunk->add(obj.get());
		}
	}
}

/*
 *  Create non-container IREG objects.
 */

Ireg_game_object_shared Game_map::create_ireg_object(
		const Shape_info& info,    // Info. about shape.
		int shnum, int frnum,      // Shape, frame.
		int tilex, int tiley,      // Tile within chunk.
		int lift                   // Desired lift.
) {
	Ireg_game_object_shared newobj;
	// (These are all animated.)
	if (info.is_field() && info.get_field_type() >= 0) {
		newobj = std::make_shared<Field_object>(
				shnum, frnum, tilex, tiley, lift,
				Egg_object::fire_field + info.get_field_type());
	} else if (info.is_animated() || info.has_sfx()) {
		newobj = std::make_shared<Animated_ireg_object>(
				shnum, frnum, tilex, tiley, lift);
	} else if (shnum == 607) {    // Path.
		newobj = std::make_shared<Egglike_game_object>(
				shnum, frnum, tilex, tiley, lift);
	} else if (info.is_mirror()) {    // Mirror
		newobj = std::make_shared<Mirror_object>(
				shnum, frnum, tilex, tiley, lift);
	} else if (info.is_body_shape()) {
		newobj = std::make_shared<Dead_body>(
				shnum, frnum, tilex, tiley, lift, -1);
	} else if (info.get_shape_class() == Shape_info::virtue_stone) {
		newobj = std::make_shared<Virtue_stone_object>(
				shnum, frnum, tilex, tiley, lift);
	} else if (info.get_shape_class() == Shape_info::spellbook) {
		static unsigned char circles[9] = {0};
		newobj                          = std::make_shared<Spellbook_object>(
                shnum, frnum, tilex, tiley, lift, &circles[0], 0);
	} else if (info.get_shape_class() == Shape_info::barge) {
		newobj = std::make_shared<Barge_object>(
				shnum, frnum, tilex, tiley, lift,
				// FOR NOW: 8x16 tiles, North.
				8, 16, 0);
	} else if (info.get_shape_class() == Shape_info::container) {
		if (info.is_jawbone()) {
			newobj = std::make_shared<Jawbone_object>(
					shnum, frnum, tilex, tiley, lift);
		} else {
			newobj = std::make_shared<Container_game_object>(
					shnum, frnum, tilex, tiley, lift);
		}
	} else {
		newobj = std::make_shared<Ireg_game_object>(
				shnum, frnum, tilex, tiley, lift);
	}
	return newobj;
}

/*
 *  Create non-container IREG objects.
 */

Ireg_game_object_shared Game_map::create_ireg_object(
		int shnum, int frnum    // Shape, frame.
) {
	return create_ireg_object(ShapeID::get_info(shnum), shnum, frnum, 0, 0, 0);
}

/*
 *  Create 'fixed' (landscape, building) objects.
 */

Ifix_game_object_shared Game_map::create_ifix_object(
		int shnum, int frnum    // Shape, frame.
) {
	const Shape_info& info = ShapeID::get_info(shnum);
	return (info.is_animated() || info.has_sfx())
				   ? std::make_shared<Animated_ifix_object>(
							 shnum, frnum, 0, 0, 0)
				   : std::make_shared<Ifix_game_object>(shnum, frnum, 0, 0, 0);
}

/*
 *  Read in the objects in a superchunk.
 */

void Game_map::get_superchunk_objects(int schunk    // Superchunk #.
) {
	get_map_objects(schunk);       // Get map objects/scenery.
	get_ifix_objects(schunk);      // Get objects from ifix.
	get_ireg_objects(schunk);      // Get moveable objects.
	schunk_read[schunk] = true;    // Done this one now.
	map_patches->apply(schunk);    // Move/delete objects.
}

/*
 *  Just see if a tile is occupied by something.
 */

bool Game_map::is_tile_occupied(const Tile_coord& tile) {
	Map_chunk* chunk = get_chunk_safely(
			tile.tx / c_tiles_per_chunk, tile.ty / c_tiles_per_chunk);
	if (!chunk) {        // Outside the world?
		return false;    // Then it's not blocked.
	}
	return chunk->is_tile_occupied(
			tile.tx % c_tiles_per_chunk, tile.ty % c_tiles_per_chunk, tile.tz);
}

/*
 *  Locate a chunk with a given terrain # and center the view on it.
 *
 *  Output: true if found, else 0.
 */

bool Game_map::locate_terrain(
		int  tnum,           // # in u7chunks.
		int& cx, int& cy,    // Chunk to start at, or (-1,-1).
		//   Updated with chunk found.
		bool upwards    // If true, search upwards.
) {
	int cnum;     // Chunk #, counting L-R, T-B.
	int cstop;    // Stop when cnum == cstop.
	int dir;
	if (upwards) {
		cstop = -1;
		dir   = -1;
		if (cx == -1) {    // Start at end?
			cnum = c_num_chunks * c_num_chunks - 1;
		} else {
			cnum = cy * c_num_chunks + cx - 1;
		}
	} else {
		cstop = c_num_chunks * c_num_chunks;
		dir   = 1;
		cnum  = (cx == -1) ? 0 : cy * c_num_chunks + cx + 1;
	}
	while (cnum != cstop) {
		const int chunky = cnum / c_num_chunks;
		const int chunkx = cnum % c_num_chunks;
		if (terrain_map[chunkx][chunky] == tnum) {
			// Return chunk # found.
			cx = chunkx;
			cy = chunky;
			// Center window over chunk found.
			Game_window::get_instance()->center_view(Tile_coord(
					cx * c_tiles_per_chunk + c_tiles_per_chunk / 2,
					cy * c_tiles_per_chunk + c_tiles_per_chunk / 2, 0));
			return true;
		}
		cnum += dir;
	}
	return false;    // Failed.
}

/*
 *  Swap two adjacent terrain #'s, keeping the map looking the same.
 *
 *  Output: false if unsuccessful.
 */

bool Game_map::swap_terrains(int tnum    // Swap tnum and tnum + 1.
) {
	if (tnum < 0 || static_cast<unsigned>(tnum) >= chunk_terrains->size() - 1) {
		return false;    // Out of bounds.
	}
	// Swap in list.
	Chunk_terrain* tmp = get_terrain(tnum);
	tmp->set_modified();
	(*chunk_terrains)[tnum] = get_terrain(tnum + 1);
	(*chunk_terrains)[tnum]->set_modified();
	(*chunk_terrains)[tnum + 1] = tmp;
	chunk_terrains_modified     = true;
	// Update terrain maps.
	Game_window*             gwin = Game_window::get_instance();
	const vector<Game_map*>& maps = gwin->get_maps();
	for (auto* map : maps) {
		if (!map) {
			continue;
		}
		for (auto& row : map->terrain_map) {
			for (short& terrain : row) {
				if (terrain == tnum) {
					terrain++;
				} else if (terrain == tnum + 1) {
					terrain--;
				}
			}
		}
		map->map_modified = true;
	}
	gwin->set_all_dirty();
	return true;
}

/*
 *  Insert a new terrain after a given one, and push all the others up
 *  so the map looks the same.  The new terrain is filled with
 *  (shape, frame) == (0, 0) unless 'dup' is passed 'true'.
 *
 *  Output: False if unsuccessful.
 */

bool Game_map::insert_terrain(
		int  tnum,    // Insert after this one (may be -1).
		bool dup      // If true, duplicate #tnum.
) {
	const int ntiles = c_tiles_per_chunk * c_tiles_per_chunk;
	const int nbytes = v2_chunks ? 3 : 2;
	if (dup && tnum == -2) {
		const int selected = cheat.get_edit_chunknum();
		if (selected < 0
			|| selected >= static_cast<int>(chunk_terrains->size())) {
			return false;    // Invalid #.
		}
		get_all_terrain();                // Need all of 'u7chunks' read in.
		unsigned char buf[ntiles * 3];    // Set up buffer with shape #'s.
		// Want to duplicate the selected terrain.
		Chunk_terrain* ter  = (*chunk_terrains)[selected];
		unsigned char* data = &buf[0];
		for (int ty = 0; ty < c_tiles_per_chunk; ty++) {
			for (int tx = 0; tx < c_tiles_per_chunk; tx++) {
				const ShapeID id    = ter->get_flat(tx, ty);
				const int     shnum = id.get_shapenum();
				const int     frnum = id.get_framenum();
				if (v2_chunks) {
					Write1(data, shnum & 0xff);
					Write1(data, (shnum >> 8) & 0xff);
					Write1(data, frnum);
				} else {
					Write1(data, id.get_shapenum() & 0xff);
					Write1(data, ((id.get_shapenum() >> 8) & 3)
										 | (id.get_framenum() << 2));
				}
			}
		}
		auto* new_terrain = new Chunk_terrain(&buf[0], v2_chunks);
		new_terrain->set_modified();
		chunk_terrains->push_back(new_terrain);
		chunk_terrains_modified = true;
		map_modified            = true;
		return true;
	} else if (!dup && tnum == -2) {
		// Special case for creating an empty chunk and appending it
		get_all_terrain();
		unsigned char buf[ntiles * 3];
		memset(buf, 0, sizeof(buf));    // Create an empty chunk

		auto* new_terrain = new Chunk_terrain(&buf[0], v2_chunks);
		new_terrain->set_modified();
		chunk_terrains->push_back(new_terrain);
		chunk_terrains_modified = true;
		map_modified            = true;
		return true;
	}
	if (tnum < -1 || tnum >= static_cast<int>(chunk_terrains->size())) {
		return false;    // Invalid #.
	}
	get_all_terrain();                // Need all of 'u7chunks' read in.
	unsigned char buf[ntiles * 3];    // Set up buffer with shape #'s.
	if (dup && tnum >= 0) {
		// Want to duplicate given terrain.
		Chunk_terrain* ter  = (*chunk_terrains)[tnum];
		unsigned char* data = &buf[0];
		for (int ty = 0; ty < c_tiles_per_chunk; ty++) {
			for (int tx = 0; tx < c_tiles_per_chunk; tx++) {
				const ShapeID id    = ter->get_flat(tx, ty);
				const int     shnum = id.get_shapenum();
				const int     frnum = id.get_framenum();
				if (v2_chunks) {
					Write1(data, shnum & 0xff);
					Write1(data, (shnum >> 8) & 0xff);
					Write1(data, frnum);
				} else {
					Write1(data, id.get_shapenum() & 0xff);
					Write1(data, ((id.get_shapenum() >> 8) & 3)
										 | (id.get_framenum() << 2));
				}
			}
		}
	} else {
		std::fill_n(buf, ntiles * nbytes, 0);
	}
	auto* new_terrain = new Chunk_terrain(&buf[0], v2_chunks);
	// Insert in list.
	chunk_terrains->insert(chunk_terrains->begin() + tnum + 1, new_terrain);
	// Indicate terrains are modified.
	const int num_chunk_terrains = chunk_terrains->size();
	for (int i = tnum + 1; i < num_chunk_terrains; i++) {
		(*chunk_terrains)[i]->set_modified();
	}
	chunk_terrains_modified = true;
	if (tnum + 1 == num_chunk_terrains - 1) {
		return true;    // Inserted at end of list.
	}
	// Update terrain map.
	Game_window*             gwin = Game_window::get_instance();
	const vector<Game_map*>& maps = gwin->get_maps();
	for (auto* map : maps) {
		if (!map) {
			continue;
		}
		for (auto& row : map->terrain_map) {
			for (short& terrain : row) {
				if (terrain > tnum) {
					terrain++;
				}
			}
		}
		map->map_modified = true;
	}
	gwin->set_all_dirty();
	return true;
}

/*
 *  Remove a terrain, updating the map.
 *
 *  Output: false if unsuccessful.
 */

bool Game_map::delete_terrain(int tnum) {
	if (tnum < 0 || static_cast<unsigned>(tnum) >= chunk_terrains->size()) {
		return false;    // Out of bounds.
	}
	const int sz = chunk_terrains->size();
	delete (*chunk_terrains)[tnum];
	for (int i = tnum + 1; i < sz; i++) {
		// Move the rest downwards.
		Chunk_terrain* tmp = get_terrain(i);
		tmp->set_modified();
		(*chunk_terrains)[i - 1] = tmp;
	}
	chunk_terrains->resize(sz - 1);
	chunk_terrains_modified = true;
	// Update terrain map.
	Game_window*             gwin = Game_window::get_instance();
	const vector<Game_map*>& maps = gwin->get_maps();
	for (auto* map : maps) {
		if (!map) {
			continue;
		}
		for (auto& row : map->terrain_map) {
			for (short& terrain : row) {
				if (terrain >= tnum) {
					terrain--;
				}
			}
		}
		map->map_modified = true;
	}
	gwin->set_all_dirty();
	return true;
}

/*
 *  Commit edits made to terrain chunks.
 */

void Game_map::commit_terrain_edits() {
	const int num_terrains = chunk_terrains->size();
	// Create list of flags.
	std::vector<unsigned char> ters(num_terrains, 0);
	// Commit edits.
	for (int i = 0; i < num_terrains; i++) {
		if ((*chunk_terrains)[i] && (*chunk_terrains)[i]->commit_edits()) {
			ters[i] = 1;
		}
	}
	// Update terrain map.
	Game_window*             gwin = Game_window::get_instance();
	const vector<Game_map*>& maps = gwin->get_maps();
	for (auto* map : maps) {
		if (!map) {
			continue;
		}
		for (int cy = 0; cy < c_num_chunks; cy++) {
			for (int cx = 0; cx < c_num_chunks; cx++) {
				Map_chunk* chunk = map->get_chunk_unsafe(cx, cy);
				if (chunk && ters[map->terrain_map[cx][cy]] != 0
					&& chunk->get_terrain()) {
					// Reload objects.
					chunk->set_terrain(chunk->get_terrain());
				}
			}
		}
	}
}

/*
 *  Abort edits made to terrain chunks.
 */

void Game_map::abort_terrain_edits() {
	const int num_terrains = chunk_terrains->size();
	// Abort edits.
	for (int i = 0; i < num_terrains; i++) {
		if ((*chunk_terrains)[i]) {
			(*chunk_terrains)[i]->abort_edits();
		}
	}
}

/*
 *  Find all unused shapes in game.  This can take a while!!
 */

void Game_map::find_unused_shapes(
		unsigned char* found,      // Bits set for shapes found.
		int            foundlen    // # bytes.
) {
	std::fill_n(found, foundlen, 0);
	Game_window*   gwin = Game_window::get_instance();
	Shape_manager* sman = Shape_manager::get_instance();
	cout << "Reading all chunks";
	// Read in EVERYTHING!
	for (int sc = 0; sc < c_num_schunks * c_num_schunks; sc++) {
		cout << '.';
		cout.flush();
		char msg[80];
		snprintf(msg, sizeof(msg), "Scanning superchunk %d", sc);
		gwin->get_effects()->center_text(msg);
		gwin->paint();
		gwin->show();
		if (!schunk_read[sc]) {
			get_superchunk_objects(sc);
		}
	}
	cout << endl;
	int       maxbits = foundlen * 8;    // Total #bits in 'found'.
	const int nshapes = sman->get_shapes().get_num_shapes();
	if (maxbits > nshapes) {
		maxbits = nshapes;
	}
	// Go through chunks.
	for (int cy = 0; cy < c_num_chunks; cy++) {
		for (int cx = 0; cx < c_num_chunks; cx++) {
			Map_chunk*                chunk = get_chunk_unchecked(cx, cy);
			Recursive_object_iterator all(chunk->get_objects());
			Game_object*              obj;
			while ((obj = all.get_next()) != nullptr) {
				const int shnum = obj->get_shapenum();
				if (shnum >= 0 && shnum < maxbits) {
					found[shnum / 8] |= (1 << (shnum % 8));
				}
			}
		}
	}
	int i;
	for (i = 0; i < maxbits; i++) {    // Add all possible monsters.
		const Shape_info&   info = ShapeID::get_info(i);
		const Monster_info* minf = info.get_monster_info();
		if (minf) {
			found[i / 8] |= (1 << (i % 8));
		}
		const Weapon_info* winf = info.get_weapon_info();
		if (winf) {    // Get projectiles for weapons.
			const int proj = winf->get_projectile();
			if (proj > 0 && proj < maxbits) {
				found[proj / 8] |= (1 << (proj % 8));
			}
		}
	}
	for (i = c_first_obj_shape; i < maxbits; i++) {    // Ignore flats (<0x96).
		if (!(found[i / 8] & (1 << (i % 8)))) {
			cout << "Shape " << i << " not found in game" << endl;
		}
	}
}

/*
 *  Look throughout the map for a given shape.  The search starts at
 *  the first currently-selected shape, if possible.
 *
 *  Output: ->object if found, else 0.
 */

Game_object* Game_map::locate_shape(
		int          shapenum,    // Desired shape.
		bool         upwards,     // If true, search upwards.
		Game_object* start,       // Start here if !0.
		int          frnum,       // Frame, or c_any_frame
		int          qual         // Quality, or c_any_qual
) {
	int cx   = -1;
	int cy   = 0;    // Before chunk to search.
	int dir  = 1;    // Direction to increment.
	int stop = c_num_chunks;
	if (upwards) {
		dir  = -1;
		stop = -1;
		cx   = c_num_chunks;    // Past last chunk.
		cy   = c_num_chunks - 1;
	}
	Game_object* obj = nullptr;
	if (start) {    // Start here.
		Game_object* owner = start->get_outermost();
		cx                 = owner->get_cx();
		cy                 = owner->get_cy();
		if (upwards) {
			Recursive_object_iterator_backwards next(start);
			while ((obj = next.get_next()) != nullptr) {
				if (obj->get_shapenum() == shapenum
					&& (frnum == c_any_framenum || obj->get_framenum() == frnum)
					&& (qual == c_any_qual || obj->get_quality() == qual)) {
					break;
				}
			}
		} else {
			Recursive_object_iterator next(start);
			while ((obj = next.get_next()) != nullptr) {
				if (obj->get_shapenum() == shapenum
					&& (frnum == c_any_framenum || obj->get_framenum() == frnum)
					&& (qual == c_any_qual || obj->get_quality() == qual)) {
					break;
				}
			}
		}
	}
	while (!obj) {           // Not found yet?
		cx += dir;           // Next chunk.
		if (cx == stop) {    // Past (either) end?
			cy += dir;
			if (cy == stop) {
				break;    // All done.
			}
			cx -= dir * c_num_chunks;
		}
		Map_chunk* chunk = get_chunk(cx, cy);
		// Make sure objs. are read.
		ensure_chunk_read(cx, cy);
		if (upwards) {
			Recursive_object_iterator_backwards next(chunk->get_objects());
			while ((obj = next.get_next()) != nullptr) {
				if (obj->get_shapenum() == shapenum
					&& (frnum == c_any_framenum || obj->get_framenum() == frnum)
					&& (qual == c_any_qual || obj->get_quality() == qual)) {
					break;
				}
			}
		} else {
			Recursive_object_iterator next(chunk->get_objects());
			while ((obj = next.get_next()) != nullptr) {
				if (obj->get_shapenum() == shapenum
					&& (frnum == c_any_framenum || obj->get_framenum() == frnum)
					&& (qual == c_any_qual || obj->get_quality() == qual)) {
					break;
				}
			}
		}
	}
	return obj;
}

/*
 *  Create a 192x192 map shape.
 */

void Game_map::create_minimap(
		Shape* minimaps, const unsigned char* chunk_pixels) {
	std::vector<unsigned char> pixels(c_num_chunks * c_num_chunks);
	for (int cy = 0; cy < c_num_chunks; ++cy) {
		const int yoff = cy * c_num_chunks;
		for (int cx = 0; cx < c_num_chunks; ++cx) {
			const int chunk_num = terrain_map[cx][cy];
			pixels[yoff + cx]   = chunk_pixels[chunk_num];
		}
	}
	if (num >= minimaps->get_num_frames()) {
		minimaps->resize(num + 1);
	}
	minimaps->set_frame(
			std::make_unique<Shape_frame>(
					pixels.data(), c_num_chunks, c_num_chunks, 0, 0, true),
			num);
}

/*
 *  Create a 192x192 map shape for each map and write out
 *  "patch/minimaps.vga".
 *
 *  Output: false if failed.
 */

bool Game_map::write_minimap() {
	char msg[80];
	// A pixel for each possible chunk.
	const int      num_chunks   = chunk_terrains->size();
	auto           chunk_pixels = std::make_unique<unsigned char[]>(num_chunks);
	unsigned char* ptr          = chunk_pixels.get();
	Game_window*   gwin         = Game_window::get_instance();
	Palette        pal;
	// Ensure that all terrain is loaded:
	get_all_terrain();
	pal.set(PALETTE_DAY, 100, false);
	Effects_manager* eman = gwin->get_effects();
	eman->center_text("Encoding chunks");
	gwin->paint();
	gwin->show();
	for (auto* ter : *chunk_terrains) {
		Image_buffer8* ibuf    = ter->get_rendered_flats();
		unsigned char* terbits = ibuf->get_bits();
		const int      w       = ibuf->get_width();
		const int      h       = ibuf->get_height();
		unsigned long  r       = 0;
		unsigned long  g       = 0;
		unsigned long  b       = 0;
		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < w; ++x) {
				r += pal.get_red(*terbits);
				g += pal.get_green(*terbits);
				b += pal.get_blue(*terbits);
				++terbits;
			}
		}
		r /= w * h;
		b /= w * h;
		g /= w * h;
		Write1(ptr, pal.find_color(r, g, b));
	}
	eman->remove_text_effects();
	const vector<Game_map*>& maps  = gwin->get_maps();
	const int                nmaps = maps.size();
	Shape                    shape;
	for (int i = 0; i < nmaps; ++i) {
		snprintf(msg, sizeof(msg), "Creating minimap %d", i);
		eman->center_text(msg);
		gwin->paint();
		gwin->show();
		maps[i]->create_minimap(&shape, chunk_pixels.get());
		eman->remove_text_effects();
	}
	OFileDataSource mfile(PATCH_MINIMAPS);    // May throw exception.
	Flex_writer     writer(mfile, "Written by Exult", 1);
	writer.write_object(shape);
	gwin->set_all_dirty();
	return true;
}

/*
 *  Do a cache out. (x, y) is the center.
 *  If x == -1, cache out whole map.
 */

void Game_map::cache_out(int cx, int cy) {
	int  sx = cx / c_chunks_per_schunk;
	int  sy = cy / c_chunks_per_schunk;
	bool chunk_flags[12][12]{};

#ifdef DEBUG
	if (cx == -1) {
		std::cout << "Want to cache out entire map #" << get_num() << std::endl;
	} else {
		std::cout << "Want to cache out around super chunk: " << (sy * 12 + sx)
				  << " = " << sx << ", " << sy << std::endl;
	}
#endif

	// We cache out all but the 9 directly around the pov
	if (cx != -1 && cy != -1) {
		chunk_flags[(sy + 11) % 12][(sx + 11) % 12] = true;
		chunk_flags[(sy + 11) % 12][sx]             = true;
		chunk_flags[(sy + 11) % 12][(sx + 1) % 12]  = true;

		chunk_flags[sy][(sx + 11) % 12] = true;
		chunk_flags[sy][sx]             = true;
		chunk_flags[sy][(sx + 1) % 12]  = true;

		chunk_flags[(sy + 1) % 12][(sx + 11) % 12] = true;
		chunk_flags[(sy + 1) % 12][sx]             = true;
		chunk_flags[(sy + 1) % 12][(sx + 1) % 12]  = true;
	}
	for (sy = 0; sy < 12; sy++) {
		for (sx = 0; sx < 12; sx++) {
			if (chunk_flags[sy][sx]) {
				continue;
			}

			const int schunk = sy * 12 + sx;
			if (schunk_read[schunk] && !schunk_modified[schunk]) {
				cache_out_schunk(schunk);
			}
		}
	}
}

void Game_map::cache_out_schunk(int schunk) {
	// Get abs. chunk coords.
	const int  scy = 16 * (schunk / 12);
	const int  scx = 16 * (schunk % 12);
	int        cy;
	int        cx;
	const bool save_map_modified     = map_modified;
	const bool save_terrain_modified = chunk_terrains_modified;

	if (schunk_modified[schunk]) {
		return;    // NEVER cache out modified chunks.
	}
	// Our vectors
	Game_object_vector removes;
	Actor_vector       actors;

	int buf_size = 0;

#ifdef DEBUG
	std::cout << "Killing superchunk: " << schunk << std::endl;
#endif
	// Go through chunks and get all the items
	for (cy = 0; cy < 16; cy++) {
		for (cx = 0; cx < 16; cx++) {
			const int size = get_chunk_unsafe(scx + cx, scy + cy)
									 ->get_obj_actors(removes, actors);

			if (size < 0) {
#ifdef DEBUG
				std::cerr << "Failed attempting to kill superchunk"
						  << std::endl;
#endif
				return;
			}
			buf_size += size + 2;
		}
	}

	schunk_read[schunk] = false;
	++caching_out;

#ifdef DEBUG
	std::cout << "Buffer size of " << buf_size
			  << " bytes required to store super chunk" << std::endl;
#endif

	// Clear old (this shouldn't happen)
	if (schunk_cache[schunk]) {
		delete[] schunk_cache[schunk];
		schunk_cache[schunk]       = nullptr;
		schunk_cache_sizes[schunk] = -1;
	}

	// Create new
	schunk_cache[schunk]       = new char[buf_size];
	schunk_cache_sizes[schunk] = buf_size;

	OBufferDataSpan ds(schunk_cache[schunk], schunk_cache_sizes[schunk]);

	write_ireg_objects(schunk, &ds);

#ifdef DEBUG
	std::cout << "Wrote " << ds.getPos() << " bytes" << std::endl;
#endif

	// Now remove the objects
	for (auto* remove : removes) {
		remove->delete_contents();
		remove->remove_this();
	}

	// Now disable the actors
	for (auto* actor : actors) {
		actor->cache_out();
	}

	// Go through chunks and finish up
	for (cy = 0; cy < 16; cy++) {
		for (cx = 0; cx < 16; cx++) {
			get_chunk_unsafe(scx + cx, scy + cy)->kill_cache();
		}
	}
	// Removing objs. sets these flags.
	schunk_modified[schunk] = false;
	map_modified            = save_map_modified;
	chunk_terrains_modified = save_terrain_modified;
	--caching_out;
}
