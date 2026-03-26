/*
 GSPLUS - Advanced Apple IIGS Emulator Environment
 Copyright (C) 2010 - 2011 by GSport contributors

 Copyright (C) 2010 by GSport contributors

 Based on the KEGS emulator written by and Copyright (C) 2003 Kent Dickey

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 2 of the License, or (at your
 option) any later version.

 This program is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 for more details.

 You should have received a copy of the GNU General Public License along
 with this program; if not, write to the Free Software Foundation, Inc.,
 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

/*
 *  Copyright (C) 2002-2004  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 Modified for the GSport emulator by Christopher G. Mason 02/2014
 Extensively rewritten to provide full emulation of the Apple ImageWriter II
 printer.

 Information used to write this emulator was provided by
 Apple's "ImageWriter II Technical Reference Manual"
 ISBN# 0-201-17766-8

 Apple's "ImageWriter LQ Reference Manual"
 ISBN# 0-201-17751-X
 */

#if !defined __IMAGEWRITER_H
#define __IMAGEWRITER_H
#ifdef __cplusplus
#ifdef C_LIBPNG
#include <png.h>
#endif

#include <stdio.h>

#ifdef HAVE_SDL
#include "SDL.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#endif // HAVE_SDL

#if defined (WIN32)
#include <windows.h>
#include <winspool.h>
#endif

#define STYLE_BASE 0x00
#define STYLE_PROP 0x01
#define STYLE_CONDENSED 0x02
#define STYLE_BOLD 0x04
#define STYLE_DOUBLESTRIKE 0x08
#define STYLE_DOUBLEWIDTH 0x10
#define STYLE_ITALICS 0x20
#define STYLE_UNDERLINE 0x40
#define STYLE_SUPERSCRIPT 0x80
#define STYLE_SUBSCRIPT 0x100
#define STYLE_HALFHEIGHT 0x200

#define SCORE_NONE 0x00
#define SCORE_SINGLE 0x01
#define SCORE_DOUBLE 0x02
#define SCORE_SINGLEBROKEN 0x05
#define SCORE_DOUBLEBROKEN 0x06

#define QUALITY_DRAFT 0x01
#define QUALITY_LQ 0x02

#define COLOR_BLACK 7<<5
typedef unsigned Bitu;
typedef signed Bits;
typedef unsigned char Bit8u;
typedef signed char Bit8s;
typedef unsigned short Bit16u;
typedef signed short Bit16s;
typedef unsigned long Bit32u;
typedef signed long Bit32s;
typedef double Real64;
#if defined(_MSC_VER)
typedef unsigned __int64 Bit64u;
typedef signed __int64 Bit64s;
#else
typedef unsigned long long int Bit64u;
typedef signed long long int Bit64s;
#endif
enum IWTypeface
{
	fixed = 0,
	prop = 1
};

typedef struct {
	Bitu codepage;
	const Bit16u* map;
} IWCHARMAP;


class Imagewriter {
public:

	Imagewriter (Bit16u dpi, Bit16u paperSize, Bit16u bannerSize, char* output, bool multipageOutput);
	virtual ~Imagewriter();

	// Process one character sent to virtual printer
	void printChar(Bit8u ch);

	// Hard Reset (like switching printer off and on)
	void resetPrinterHard();

	// Set Autofeed value
	void setAutofeed(bool feed);

	// Get Autofeed value
	bool getAutofeed();

	// True if printer is unable to process more data right now (do not use printChar)
	bool isBusy();

	// True if the last sent character was received
	bool ack();

	// Manual formfeed
	void formFeed();

#ifdef HAVE_SDL
	// Returns true if the current page is blank
	bool isBlank();
#endif // HAVE_SDL

private:

	// Resets the printer to the factory settings
	void resetPrinter();

	// Clears page. If save is true, saves the current page to a bitmap
	void newPage(bool save, bool resetx);

	// Closes a multipage document
	void finishMultipage();

	// Output current page
	void outputPage();

#ifdef HAVE_SDL
	// used to fill the color "sub-pallettes"
	void FillPalette(Bit8u redmax, Bit8u greenmax, Bit8u bluemax, Bit8u colorID,
							SDL_Palette* pal);

	// Checks if given char belongs to a command and process it. If false, the character
	// should be printed
	bool processCommandChar(Bit8u ch);

	// Reload font. Must be called after changing dpi, style or cpi
	void updateFont();

	// Reconfigures printer parameters after changing soft-switches with ESC Z and ESC D
	void updateSwitch();

	// Overprints a slash over zero if softswitch B-1 is set
	void slashzero(Bit16u penX, Bit16u penY);

	// Blits the given glyph on the page surface. If add is true, the values of bitmap are
	// added to the values of the pixels in the page
	void blitGlyph(FT_Bitmap bitmap, Bit16u destx, Bit16u desty, bool add);

	// Draws an anti-aliased line from (fromx, y) to (tox, y). If broken is true, gaps are included
	void drawLine(Bitu fromx, Bitu tox, Bitu y, bool broken);

	// Setup the bitGraph structure
	void setupBitImage(Bit8u dens, Bit16u numCols);

	// Process a character that is part of bit image. Must be called iff bitGraph.remBytes > 0.
	void printBitGraph(Bit8u ch);

	// Copies the codepage mapping from the constant array to CurMap
	void selectCodepage(Bit16u cp);

	// Prints out a byte using ASCII85 encoding (only outputs something every four bytes). When b>255, closes the ASCII85 string
	void fprintASCII85(FILE* f, Bit16u b);

	// Returns value of the num-th pixel (couting left-right, top-down) in a safe way
	Bit8u getPixel(Bit32u num);
	Bit8u getxyPixel(Bit32u x,Bit32u y);

	FT_Library FTlib;					// FreeType2 library used to render the characters

	SDL_Surface* page;					// Surface representing the current page
	FT_Face curFont;					// The font currently used to render characters
	Bit8u color;
	Bit8u switcha;						//Imagewriter softswitch A
	Bit8u switchb;						//Imagewriter softswitch B

	Real64 curX, curY;					// Position of the print head (in inch)

	Bit16u dpi;							// dpi of the page
	Bit16u ESCCmd;						// ESC-command that is currently processed
	bool ESCSeen;						// True if last read character was an ESC (0x1B)
	bool FSSeen;						// True if last read character was an FS (0x1C) (IBM commands)
	Bit8u numParam, neededParam;		// Numbers of parameters already read/needed to process command

	Bit8u params[20];					// Buffer for the read params
	Bit16u style;						// Style of font (see STYLE_* constants)
	Real64 cpi, actcpi;					// CPI value set by program and the actual one (taking in account font types)
	Bit8u score;						// Score for lines (see SCORE_* constants)
	Bit8u verticalDot;					// Vertical dot shift for Imagewriter LQ modes

	Real64 topMargin, bottomMargin, rightMargin, leftMargin;	// Margins of the page (in inch)
	Real64 pageWidth, pageHeight;								// Size of page (in inch)
	Real64 defaultPageWidth, defaultPageHeight;					// Default size of page (in inch)
	Real64 lineSpacing;											// Size of one line (in inch)

	Real64 horiztabs[32];				// Stores the set horizontal tabs (in inch)
	Bit8u numHorizTabs;					// Number of configured tabs

	Real64 verttabs[16];				// Stores the set vertical tabs (in inch)
	Bit8u numVertTabs;					// Number of configured tabs

	Bit8u curCharTable;					// Currently used char table und charset
	Bit8u printQuality;					// Print quality (see QUALITY_* constants)
	Bit8u printRes;						// Graphics resolution
	IWTypeface LQtypeFace;				// Typeface used in LQ printing mode

	Real64 extraIntraSpace;				// Extra space between two characters (set by program, in inch)

	bool charRead;						// True if a character was read since the printer was last initialized
	bool autoFeed;						// True if a LF should automatically added after a CR
	bool printUpperContr;				// True if the upper command characters should be printed

	struct bitGraphicParams				// Holds information about printing bit images
	{
		Bit16u horizDens, vertDens;		// Density of image to print (in dpi)
		bool adjacent;					// Print adjacent pixels? (ignored)
		Bit8u bytesColumn;				// Bytes per column
		Bit16u remBytes;				// Bytes left to read before image is done
		Bit8u column[6];				// Bytes of the current and last column
		Bit8u readBytesColumn;			// Bytes read so far for the current column
	} bitGraph;

	Bit8u densk, densl, densy, densz;	// Image density modes used in ESC K/L/Y/Z commands

	Bit16u curMap[256];					// Currently used ASCII => Unicode mapping
	Bit16u charTables[4];				// Charactertables

	Real64 definedUnit;					// Unit used by some ESC/P2 commands (negative => use default)

	bool multipoint;					// If multipoint mode is enabled
	Real64 multiPointSize;				// Point size of font in multipoint mode
	Real64 multicpi;					// CPI used in multipoint mode

	Real64 hmi;							// Horizontal motion index (in inch; overrides CPI settings)

	Bit16u numPrintAsChar;				// Number of bytes to print as characters (even when normally control codes)

#if defined (WIN32)
	HDC printerDC;						// Win32 printer device
#endif
	int port;							// SCC Port

#endif // HAVE_SDL
	Bit8u msb;							// MSB mode

	char* output;						// Output method selected by user
	void* outputHandle;					// If not null, additional pages will be appended to the given handle
	bool multipageOutput;				// If true, all pages are combined to one file/print job etc. until the "eject page" button is pressed
	Bit16u multiPageCounter;			// Current page (when printing multipages)

	Bit8u ASCII85Buffer[4];				// Buffer used in ASCII85 encoding
	Bit8u ASCII85BufferPos;				// Position in ASCII85 encode buffer
	Bit8u ASCII85CurCol;				// Columns printed so far in the current lines
};

#endif

//Interfaces to C code
#ifdef __cplusplus
extern "C"
{
#else
#include <stdbool.h>
typedef unsigned char Bit8u;
#endif

void imagewriter_init(int pdpi, int ppaper, int banner, char* poutput, bool mpage);
void imagewriter_loop(Bit8u pchar);
void imagewriter_close();
void imagewriter_feed();
#ifdef __cplusplus
}
#endif

#endif
