/*
   GSPLUS - Advanced Apple IIGS Emulator Environment
   Based on the KEGS emulator written by Kent Dickey
   See COPYRIGHT.txt for Copyright information
   See LICENSE.txt for license (GPL v2)
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
 Modified for the GSport emulator by Christopher G. Mason 03/2014
 Extensively rewritten to provide full emulation of the Apple ImageWriter II
 and LQ printers.

 Information used to write this emulator was provided by
 Apple's "ImageWriter II Technical Reference Manual"
 ISBN# 0-201-17766-8
 and
 Apple's "ImageWriter LQ Reference Manual"
 ISBN# 0-201-17751-X
 */

#include "imagewriter.h"
#include <math.h>
#include "support.h"

//#include "png.h"
//#pragma comment( lib, "libpng.lib" )
//#pragma comment (lib, "zdll.lib" )

static Imagewriter* defaultImagewriter = NULL;

static FILE *textPrinterFile = NULL;
#ifdef WIN32
const char* const textPrinterFileName = ".\\printer.txt";
#else
const char* const textPrinterFileName = "./printer.txt";
#endif

#define PARAM16(I) (params[I+1]*256+params[I])
#define PIXX ((Bitu)floor(curX*dpi+0.5))
#define PIXY ((Bitu)floor(curY*dpi+0.5))
//These ugly defines are so we can convert multibyte parameters from strings into some nice ints.
#define paramc(I) (params[I]-'0')
#define PARAM2(I) (paramc(I)*10+paramc(I+1))
#define PARAM3(I) (paramc(I)*100+paramc(I+1)*10+paramc(I+2))
#define PARAM4(I) (paramc(I)*1000+paramc(I+1)*100+paramc(I+2)*10+paramc(I+3))

static Bitu printer_timout;
static bool timeout_dirty;
static const char* document_path;
extern "C" char* g_imagewriter_fixed_font;
extern "C" char* g_imagewriter_prop_font;
extern "C" int iw_scc_write;
#include "iw_charmaps.h"

#define SWITCHA_CHARSET_MASK    0x07
#define SWITCHA_CHARSET_US      0x00
#define SWITCHA_CHARSET_IT      0x01
#define SWITCHA_CHARSET_DK      0x02
#define SWITCHA_CHARSET_UK      0x03
#define SWITCHA_CHARSET_DE      0x04
#define SWITCHA_CHARSET_SE      0x05
#define SWITCHA_CHARSET_FR      0x06
#define SWITCHA_CHARSET_ES      0x07

#define SWITCHA_PERFORATIONSKIP 0x10
#define SWITCHA_LFAFTERCR       0x80

#ifdef HAVE_SDL
void Imagewriter::FillPalette(Bit8u redmax, Bit8u greenmax, Bit8u bluemax, Bit8u colorID, SDL_Palette* pal)
{
	float red=redmax/30.9;
	float green=greenmax/30.9;
	float blue=bluemax/30.9;

	Bit8u colormask=colorID<<=5;

	for(int i = 0; i < 32;i++) {
		pal->colors[i+colormask].r=255-(red*(float)i);
		pal->colors[i+colormask].g=255-(green*(float)i);
		pal->colors[i+colormask].b=255-(blue*(float)i);
	}
}
#endif // HAVE_SDL

Imagewriter::Imagewriter(Bit16u dpi, Bit16u paperSize, Bit16u bannerSize, char* output, bool multipageOutput)
{
#ifdef HAVE_SDL
	if (FT_Init_FreeType(&FTlib))
	{
		page = NULL;
	}
	else
	{
		SDL_Init(SDL_INIT_EVERYTHING);
		this->output = output;
		this->multipageOutput = multipageOutput;
		this->port = port;

		if (bannerSize)
		{
			defaultPageWidth = ((Real64)paperSizes[0][0]/(Real64)72);
			defaultPageHeight = (((Real64)paperSizes[0][1]*bannerSize)/(Real64)72);
			dpi = 144;
		}
		else
		{
			if (paperSize >= N_PAPER_SIZES)
			{
				printf("Printer: unsupported paper size %d\n", paperSize);
				paperSize = 0;
			}
			defaultPageWidth = ((Real64)paperSizes[paperSize][0]/(Real64)72);
			defaultPageHeight = ((Real64)paperSizes[paperSize][1]/(Real64)72);
		}
		this->dpi = dpi;
		// Create page
		page = SDL_CreateRGBSurface(
						SDL_SWSURFACE,
						(Bitu)(defaultPageWidth*dpi),
						(Bitu)(defaultPageHeight*dpi),
						8,
						0,
						0,
						0,
						0);

		// Set a grey palette
		SDL_Palette* palette = page->format->palette;

		for (Bitu i=0; i<32; i++)
		{
			palette->colors[i].r =255;
			palette->colors[i].g =255;
			palette->colors[i].b =255;
		}

		// 0 = all white needed for logic 000
		FillPalette(  0,   0,   0, 1, palette);
		// 1 = magenta* 001
		FillPalette(  0, 255,   0, 1, palette);
		// 2 = cyan*    010
		FillPalette(255,   0,   0, 2, palette);
		// 3 = "violet" 011
		FillPalette(255, 255,   0, 3, palette);
		// 4 = yellow*  100
		FillPalette(  0,   0, 255, 4, palette);
		// 5 = red      101
		FillPalette(  0, 255, 255, 5, palette);
		// 6 = green    110
		FillPalette(255,   0, 255, 6, palette);
		// 7 = black    111
		FillPalette(255, 255, 255, 7, palette);

		// 0 = all white needed for logic 000
		/*FillPalette(  0,   0,   0, 1, palette);
		// 1 = yellow*  100 IW
		FillPalette(  0,   0, 255, 1, palette);
		// 2 = magenta* 001 IW
		FillPalette(  0, 255,   0, 2, palette);
		// 3 = cyan*    010 IW
		FillPalette(255,   0,   0, 3, palette);
		// 4 = red      101 IW
		FillPalette(  0, 255, 255, 4, palette);
		// 5 = green    110 IW
		FillPalette(255,   0, 255, 5, palette);
		// 6 = "violet" 011 IW
		FillPalette(255, 255,   0, 6, palette);
		// 7 = black    111
		FillPalette(255, 255, 255, 7, palette);*/

		// yyyxxxxx bit pattern: yyy=color xxxxx = intensity: 31=max
		// Printing colors on top of each other ORs them and gets the
		// correct resulting color.
		// i.e. magenta on blank page yyy=001
		// then yellow on magenta 001 | 100 = 101 = red

		color=COLOR_BLACK;

		curFont = NULL;
		charRead = false;
		autoFeed = false;
		outputHandle = NULL;

		resetPrinter();
		//Only initialize native printer here if multipage output is off. That way the user doesn't get prompted every page.
		if (strcasecmp(output, "printer") == 0 && !multipageOutput)
		{
#if defined (WIN32)
			// Show Print dialog to obtain a printer device context

			ShowCursor(1);
			PRINTDLG pd;
			pd.lStructSize = sizeof(PRINTDLG);
			pd.hDevMode = (HANDLE) NULL;
			pd.hDevNames = (HANDLE) NULL;
			pd.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE;
			pd.hwndOwner = NULL;
			pd.hDC = (HDC) NULL;
			pd.nFromPage = 0xFFFF;
			pd.nToPage = 0xFFFF;
			pd.nMinPage = 1;
			pd.nMaxPage = 0xFFFF;
			pd.nCopies = 1;
			pd.hInstance = NULL;
			pd.lCustData = 0L;
			pd.lpfnPrintHook = (LPPRINTHOOKPROC) NULL;
			pd.lpfnSetupHook = (LPSETUPHOOKPROC) NULL;
			pd.lpPrintTemplateName = (LPCSTR) NULL;
			pd.lpSetupTemplateName = (LPCSTR)  NULL;
			pd.hPrintTemplate = (HANDLE) NULL;
			pd.hSetupTemplate = (HANDLE) NULL;
			if(!PrintDlg(&pd))
			{
				//If user presses cancel, warn them with a dialog and switch output to bitmap files
				this->output = "bmp";
				MessageBox(NULL,"You did not select a printer.\nAll printer output will be saved as bitmap files.\nTo select a printer, press F4 and select 'Reset Virtual ImageWriter'",NULL,MB_ICONEXCLAMATION);
			}
			printerDC = pd.hDC;
			ShowCursor(0);
#endif // WIN32
		}
	}
#endif // HAVE_SDL
#ifndef HAVE_SDL
		this->output = output;
		this->multipageOutput = multipageOutput;
#endif // !HAVE_SDL
};

void Imagewriter::resetPrinterHard()
{
#ifdef HAVE_SDL
	charRead = false;
	resetPrinter();
#endif // HAVE_SDL
}

void Imagewriter::resetPrinter()
{
#ifdef HAVE_SDL
		printRes = 0;
		color=COLOR_BLACK;
		curX = curY = 0.0;
		ESCSeen = false;
		FSSeen = false;
		ESCCmd = 0;
		numParam = neededParam = 0;
		topMargin = 0.0;
		leftMargin = 0.25; //Most all Apple II software including GS/OS assume a 1/4 inch margin on an Imagewriter
		rightMargin = pageWidth = defaultPageWidth;
		bottomMargin = pageHeight = defaultPageHeight;
		lineSpacing = (Real64)1/6;
		cpi = 12.0;
		printRes = 2;
		definedUnit = 96;
		curCharTable = 1;
		style = STYLE_BASE;
		score = SCORE_NONE;
		extraIntraSpace = 0.0;
		printUpperContr = true;
		bitGraph.remBytes = 0;
		densk = 0;
		densl = 1;
		densy = 2;
		densz = 3;
		charTables[0] = 0; // Italics
		charTables[1] = charTables[2] = charTables[3] = 437;
		multipoint = false;
		multiPointSize = 0.0;
		multicpi = 0.0;
		hmi = -1.0;
		switcha = SWITCHA_CHARSET_US;
		switchb = ' ';
		numPrintAsChar = 0;
		LQtypeFace = fixed;
		verticalDot = 0;
		selectCodepage(charTables[curCharTable]);

		updateFont();
		updateSwitch();

#endif // HAVE_SDL
		newPage(false,true);
#ifdef HAVE_SDL

		// Default tabs => Each eight characters
		/*for (Bitu i=0;i<32;i++)
			horiztabs[i] = i*8*(1/(Real64)cpi);*/
		numHorizTabs = 0;

		numVertTabs = 0;
#endif // HAVE_SDL
}


Imagewriter::~Imagewriter(void)
{
#ifdef HAVE_SDL
	finishMultipage();
	if (page != NULL)
	{
		SDL_FreeSurface(page);
		page = NULL;
		FT_Done_FreeType(FTlib);
	}
#if defined (WIN32)
	DeleteDC(printerDC);
#endif
#endif // HAVE_SDL
};

#ifdef HAVE_SDL
void Imagewriter::selectCodepage(Bit16u cp)
{
	Bit16u *mapToUse = NULL;

	switch(cp)
	{
	case 0: // Italics, use cp437
	case 437:
		mapToUse = (Bit16u*)&cp437Map;
		break;
	default:
		//LOG(LOG_MISC,LOG_WARN)("Unsupported codepage %i. Using CP437 instead.", cp);
		mapToUse = (Bit16u*)&cp437Map;
	}

	for (int i=0; i<256; i++)
		curMap[i] = mapToUse[i];
}
#endif // HAVE_SDL

#ifdef HAVE_SDL
void Imagewriter::updateFont()
{
	//	char buffer[1000];
	if (curFont != NULL)
		FT_Done_Face(curFont);

	char* fontName;

	switch (LQtypeFace)
	{
	case fixed:
		fontName = g_imagewriter_fixed_font;
		break;
	case prop:
		fontName = g_imagewriter_prop_font;
		break;
	default:
		fontName = g_imagewriter_fixed_font;
	}

	if (FT_New_Face(FTlib, fontName, 0, &curFont))
	{

		printf("Unable to load font %s\n", fontName);
		//LOG_MSG("Unable to load font %s", fontName);
		curFont = NULL;
	}

	Real64 horizPoints = 10;
	Real64 vertPoints = 10;
	if (!multipoint)
	{
		actcpi = cpi;

		if (!(style & STYLE_CONDENSED)) {
			horizPoints *= 10.0/cpi;
			//vertPoints *= 10.0/cpi;
		}

		if (!(style & STYLE_PROP)) {
			if ((cpi == 10.0) && (style & STYLE_CONDENSED)) {
				actcpi = 17.14;
				horizPoints *= 10.0/17.14;
			}
			if ((cpi == 12.0) && (style & STYLE_CONDENSED)) {
				actcpi = 20.0;
				horizPoints *= 10.0/20.0;
				vertPoints *= 10.0/12.0;
			}
		} else if (style & STYLE_CONDENSED) horizPoints /= 2.0;


		if ((style & STYLE_DOUBLEWIDTH)) {
			actcpi /= 2.0;
			horizPoints *= 2.0;
		}
	} else { // multipoint true
		actcpi = multicpi;
		horizPoints = vertPoints = multiPointSize;
	}

	if (style & STYLE_SUPERSCRIPT || style & STYLE_SUBSCRIPT || style & STYLE_HALFHEIGHT) {
		//horizPoints *= 2.0/3.0;
		vertPoints *= 2.0/3.0;
		//actcpi /= 2.0/3.0;
	}

	FT_Set_Char_Size(curFont, (Bit16u)horizPoints*64, (Bit16u)vertPoints*64, dpi, dpi);

	if (style & STYLE_ITALICS || charTables[curCharTable] == 0)
	{
		FT_Matrix  matrix;
		matrix.xx = 0x10000L;
		matrix.xy = (FT_Fixed)(0.20 * 0x10000L);
		matrix.yx = 0;
		matrix.yy = 0x10000L;
		FT_Set_Transform(curFont, &matrix, 0);
	}
}


void Imagewriter::updateSwitch()
{
	//Set international character mappping (Switches A-1 to A3)
	int charmap = (switcha & SWITCHA_CHARSET_MASK);
	curMap[0x23] = intCharSets[charmap][0];
	curMap[0x40] = intCharSets[charmap][1];
	curMap[0x5b] = intCharSets[charmap][2];
	curMap[0x5c] = intCharSets[charmap][3];
	curMap[0x5d] = intCharSets[charmap][4];
	curMap[0x60] = intCharSets[charmap][5];
	curMap[0x7b] = intCharSets[charmap][6];
	curMap[0x7c] = intCharSets[charmap][7];
	curMap[0x7d] = intCharSets[charmap][8];
	curMap[0x7e] = intCharSets[charmap][9];

	if (switcha & SWITCHA_PERFORATIONSKIP)
	{
		topMargin = 0.25;
		bottomMargin = pageHeight - 0.25;
	}
	else
	{
		topMargin = 0.0;
		bottomMargin = pageHeight - 0.0;
	}

	//MSB control (Switch B-6)
	if (!(switchb & 32))
	{
		msb = 255;
	}
	else msb = 0;
}
void Imagewriter::slashzero(Bit16u penX, Bit16u penY)
{
		FT_Face slashFont = curFont;
		FT_UInt slashindex = FT_Get_Char_Index(slashFont, curMap[0x2f]);
		FT_Load_Glyph(slashFont, slashindex, FT_LOAD_DEFAULT);
		FT_Render_Glyph(slashFont->glyph, FT_RENDER_MODE_NORMAL);
		blitGlyph(slashFont->glyph->bitmap, penX, penY, false);
		blitGlyph(slashFont->glyph->bitmap, penX+1, penY, true);
		if (style & STYLE_BOLD) {
			blitGlyph(slashFont->glyph->bitmap, penX+1, penY, true);
			blitGlyph(slashFont->glyph->bitmap, penX+2, penY, true);
			blitGlyph(slashFont->glyph->bitmap, penX+3, penY, true);
		}
}
#endif // HAVE_SDL

#ifdef HAVE_SDL
bool Imagewriter::processCommandChar(Bit8u ch)
{
	if (ESCSeen || FSSeen)
	{
		ESCCmd = ch;
		if(FSSeen) ESCCmd |= 0x800;
		ESCSeen = FSSeen = false;
		numParam = 0;

		switch (ESCCmd) {
		case 0x21: // Select bold font											(ESC !) IW
		case 0x22: // Cancel bold font											(ESC ") IW
		case 0x24: // Cancel MSB control and Mousetext							(ESC $) IW
		case 0x2b: // custom char width is 8 dots								(ESC -) IW
		case 0x2e: // custom char width is 8 dots								(ESC +) IW
		case 0x30: // Clear all tabs											(ESC 0) IW
		case 0x31: // Insert 1 intercharacter spaces							(ESC 1) IW
		case 0x32: // Insert 2 intercharacter spaces							(ESC 2) IW
		case 0x33: // Insert 3 intercharacter spaces							(ESC 3) IW
		case 0x34: // Insert 4 intercharacter spaces							(ESC 4) IW
		case 0x35: // Insert 5 intercharacter spaces							(ESC 5) IW
		case 0x36: // Insert 6 intercharacter spaces							(ESC 6) IW
		case 0x3c: // bidirectional mode (one line)								(ESC <) IW
		case 0x3e: // Unidirectional mode (one line)							(ESC >) IW
		case 0x3f: // Send ID string											(ESC ?) IW
		case 0x41: // Select 1/6-inch line spacing								(ESC A) IW
		case 0x42: // Select 1/8-inch line spacing								(ESC B) IW
		case 0x45: // 12 cpi, 96 dpi graphics									(ESC E) IW
		case 0x4d: // Same as ESC a2											(ESC M) IW
		case 0x4e: // 10 cpi, 80 dpi graphics									(ESC N) IW
		case 0x4f: // Disable paper-out detector								(ESC O) IW
		case 0x50: // Proportional, 160 dpi graphics							(ESC P) IW
		case 0x51: // 17 cpi, 136 dpi graphics									(ESC Q) IW
		case 0x57: // Cancel halfheight printing								(ESC W) IW
		case 0x58: // Turn underline on											(ESC X) IW
		case 0x59: // Turn underline off										(ESC Y) IW
		case 0x63: // Initialize printer										(ESC c) IW
		case 0x65: // 13.4 cpi, 107 dpi graphics								(ESC e) IW
		case 0x66: // Select forward feed mode									(ESC f) IW
		case 0x6b: // Select optional font										(ESC k) IW LQ
		case 0x6d: // Same as ESC a0											(ESC m) IW
		case 0x6e: // 9 cpi, 72 dpi graphics									(ESC n) IW
		case 0x6f: // Enable paper-out detector									(ESC o) IW
		case 0x70: // Proportional, 144 dpi graphics							(ESC p) IW
		case 0x72: // Select reverse feed mode									(ESC r) IW
		case 0x71: // 15 cpi, 120 dpi graphics									(ESC q) IW
		case 0x77: // Select halfheight printing								(ESC w) IW
		case 0x78: // Select superscript printing								(ESC x) IW
		case 0x79: // Select subscript printing									(ESC y) IW
		case 0x7a: // Cancel superscript/subscript printing						(ESC z) IW
			neededParam = 0;
			break;
		case 0x3d: // Internal font ID											(ESC = n) IW LQ
		case 0x40: // Select output bin											(ESC @ n) IW LQ
		case 0x4b: // Select printing color										(ESC K n) IW
		case 0x61: // Select font												(ESC a n) IW
		case 0x6c: // Insert CR before LF and FF								(ESC l n) IW
		case 0x73: // Set intercharacter space									(ESC s n) IW
		case 0x74: // Shift printing downward n/216 inch						(ESC t n) IW LQ
		case 0x833: // Feed n lines of blank space								(US n) IW
			neededParam = 1;
			break;
		case 0x44: // Set soft switches to closed (on)= 1						(ESC D nn) IW
		case 0x54: // Distance between lines to be nn/144 inch					(ESC T nn) IW
		case 0x5a: // Set soft switches to open (off) = 0						(ESC Z nn) IW
			neededParam = 2;
			break;
		case 0x4c: // Set left margin at column nnn								(ESC L nnn) IW
		case 0x67: // Print graphics for next nnn * 8 databytes					(ESC g nnn) IW
		case 0x75: // Add one tab stop at nnn									(ESC u nnn) IW
			neededParam = 3;
			break;
		case 0x28: // Set horizontal tabs											(ESC ( nnn,) IW
			numHorizTabs = 0;
		case 0x29: // Delete horizontal tabs										(ESC ) nnn,) IW
		case 0x43: // Print hi-res graphics for next nnnn*3 databytes				(ESC C nnnn) IW LQ
		case 0x47: // Print graphics for next nnnn databytes						(ESC G nnnn) IW
		case 0x46: // Place printhead nnnn dots from left margin					(ESC F nnnn) IW
		case 0x48: // Set pagelength to nnnn/144									(ESC H nnnn) IW
		case 0x53: // Print graphics for next nnnn databytes						(ESC S nnnn) IW
		case 0x52: // Repeat character c nnn times									(ESC R nnn c) IW
		case 0x68: // Place printhead nnnn hi-res dots from left margin				(ESC h nnnn) IW LQ
			neededParam = 4;
			break;
		case 0x56: //Repeat Print nnnn repetitions of dot column c					(ESC V nnnn c) IW
			neededParam = 5;
			msb = 255;
			break;
		case 0x55: //Repeat Print nnnn repetitions of hi-res dot column abc			(ESC U nnnn abc) IW LQ
			neededParam = 7;
			msb = 255;
			break;
		case 0x27: // Select user-defined set									(ESC ')
		case 0x49: // Define user-defined characters							(ESC I)
			//LOG(LOG_MISC,LOG_ERROR)("User-defined characters not supported!");
			return true;
		default:
			/*LOG_MSG("PRINTER: Unknown command %c (%02Xh) %c , unable to skip parameters.",
				(ESCCmd & 0x800)?"FS":"ESC",ESCCmd, ESCCmd);*/

			neededParam = 0;
			ESCCmd = 0;
			return true;
		}

		if (neededParam > 0)
			return true;
	}

	if (numParam < neededParam)
	{
		params[numParam++] = ch;

		if (numParam < neededParam)
			return true;
	}
	if (ESCCmd != 0)
	{
		switch (ESCCmd)
		{
		case 0x19: // Control paper loading/ejecting (ESC EM)
			// We are not really loading paper, so most commands can be ignored
			if (params[0] == 'R')
				newPage(true,false); // TODO resetx?
			break;
		case 0x73: // Set intercharacter space (ESC s) IW
			if (style & STYLE_PROP)
			{
				extraIntraSpace = (Real64)paramc(0);
				updateFont();
			}
			break;
		case 0x46: // Set absolute horizontal print position (ESC F nnnn) IW
			{
			int x = 0;
			//convert any leading spaces in parameters to zeros
			while (x < 4)
			{
				if (params[x] == ' ') params[x] = '0';
				x++;
			}
				Real64 unitSize = definedUnit;
				if (unitSize < 0)
					unitSize = (Real64)72.0;

				Real64 newX = leftMargin + ((Real64)PARAM4(0)/unitSize);
				if (newX <= rightMargin)
					curX = newX;
			}
			break;
		case 0x68: // Set absolute horizontal hi-res print position (ESC h nnnn) IW LQ
			{
			int x = 0;
			//convert any leading spaces in parameters to zeros
			while (x < 4)
			{
				if (params[x] == ' ') params[x] = '0';
				x++;
			}
				Real64 unitSize = definedUnit*2;
				if (unitSize < 0)
					unitSize = (Real64)72.0;

				Real64 newX = leftMargin + ((Real64)PARAM4(0)/unitSize);
				if (newX <= rightMargin)
					curX = newX;
			}
			break;
		case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36:// Insert 1-6 intercharacter spaces  (ESC 1 to 6) IW
			{
				if (style & STYLE_PROP) //This function only works in proportional mode
				{
					Real64 unitSize = definedUnit;
					if (unitSize < 0)
						unitSize = (Real64)72.0;

					Real64 newX = ((Real64)(ESCCmd-'0')/unitSize);
					if (newX <= rightMargin)
						curX = newX;
				}
				break;
			}
		case 0x47: case 0x53: // Print graphics (ESC G nnnn) IW
			{
			int x = 0;
			printRes &= ~8;
			//convert any leading spaces in parameters to zeros
			while (x < 4)
			{
				if (params[x] == ' ') params[x] = '0';
				x++;
			}
			setupBitImage(printRes, PARAM4(0));
			break;
			}
		case 0x43: // Print hi-res graphics (ESC G nnnn) IW LQ
			{
			int x = 0;
			printRes |= 8;
			//convert any leading spaces in parameters to zeros
			while (x < 4)
			{
				if (params[x] == ' ') params[x] = '0';
				x++;
			}
			setupBitImage(printRes, PARAM4(0));
			break;
			}
		case 0x67: // Print graphics (ESC g nnn) IW
			{
			int x = 0;
			printRes &= ~8;
			//convert any leading spaces in parameters to zeros
			while (x < 4)
			{
				if (params[x] == ' ') params[x] = '0';
				x++;
			}
			setupBitImage(printRes, (PARAM3(0)*8));
			break;
			}
		case 0x56: //Repeat Print nnnn repetitions of dot column byte c (ESC V nnnn c)IW
			{
			int x = 0;
			printRes &= ~8;
			//convert any leading spaces in parameters to zeros
			while (x < 4)
			{
				if (params[x] == ' ') params[x] = '0';
				x++;
			}
			x = 0;
			while ( x < PARAM4(0))
			{
			setupBitImage(printRes, 1);
			printBitGraph(params[4]);
			x++;
			}
			msb = 0;
			break;
			}
		case 0x55: //Repeat Print nnnn repetitions of hi-res dot column byte abc (ESC U nnnn abc) IW LQ
			{
			int x = 0;
			printRes |= 8;
			//convert any leading spaces in parameters to zeros
			while (x < 4)
			{
				if (params[x] == ' ') params[x] = '0';
				x++;
			}
			x = 0;
			while ( x < PARAM4(0))
			{
			setupBitImage(printRes, 1);
			printBitGraph(params[4]);
			printBitGraph(params[5]);
			printBitGraph(params[6]);
			x++;
			}
			msb = 0;
			break;
			}
		case 0x74: // Shift printing downward n/216 inch						(ESC t n) IW LQ
			{
				verticalDot = paramc(0);
				break;
			}
		case 0x6e: // 9 cpi, 72/144 dpi graphics								(ESC n) IW
			cpi = 9.0;
			style &= (0xffff - STYLE_PROP);
			printRes = 0;
			definedUnit = 72;
			extraIntraSpace = 0.0;
			LQtypeFace = fixed;
			updateFont();
			break;
		case 0x4e: // 10 cpi, 80/160 dpi graphics								(ESC N) IW
			cpi = 10.0;
			printRes = 1;
			style &= (0xffff - STYLE_PROP);
			definedUnit = 80;
			extraIntraSpace = 0.0;
			LQtypeFace = fixed;
			updateFont();
			break;
		case 0x45: // 12 cpi, 96/192 dpi graphics								(ESC E) IW
			cpi = 12.0;
			printRes = 2;
			style &= (0xffff - STYLE_PROP);
			definedUnit = 96;
			extraIntraSpace = 0.0;
			LQtypeFace = fixed;
			updateFont();
			break;
		case 0x65: // 13.4 cpi, 107/216 dpi graphics							(ESC e) IW
			cpi = 13.4;
			printRes = 3;
			style &= (0xffff - STYLE_PROP);
			definedUnit = 107;
			extraIntraSpace = 0.0;
			LQtypeFace = fixed;
			updateFont();
			break;
		case 0x71: // 15 cpi, 120/240 dpi graphics								(ESC q) IW
			cpi = 15;
			printRes = 4;
			style &= (0xffff - STYLE_PROP);
			definedUnit = 120;
			extraIntraSpace = 0.0;
			LQtypeFace = fixed;
			updateFont();
			break;
		case 0x51: // 17 cpi, 136/272 dpi graphics								(ESC Q) IW
			cpi = 17;
			printRes = 5;
			style &= (0xffff - STYLE_PROP);
			definedUnit = 136;
			extraIntraSpace = 0.0;
			LQtypeFace = fixed;
			updateFont();
			break;
		case 0x70: // Proportional, 144/288 dpi graphics						(ESC p) IW
			style |= STYLE_PROP;
			cpi = 10;
			//printQuality = QUALITY_LQ;
			printRes = 6;
			definedUnit = 144;
			LQtypeFace = prop;
			updateFont();
			break;
		case 0x50: // Proportional, 160/320 dpi graphics						(ESC P) IW
			style |= STYLE_PROP;
			cpi = 12;
			//printQuality = QUALITY_LQ;
			printRes = 7;
			definedUnit = 160;
			LQtypeFace = prop;
			updateFont();
			break;
		case 0x54: // Set n/144-inch line spacing (ESC T nn) IW
			lineSpacing = (Real64)PARAM2(0)/144;
			break;
		case 0x59: // Turn underline off (ESC Y) IW
			style &= ~STYLE_UNDERLINE;
			updateFont();
			break;
		case 0x58: // Turn underline on (ESC X) IW
			style |= STYLE_UNDERLINE;
			score = SCORE_SINGLE;
			updateFont();
			break;
		case 0x42: // Select 1/8-inch line spacing (ESC B) IW
			lineSpacing = (Real64)1/8;
			break;
		case 0x41: // Select 1/6-inch line spacing (ESC A) IW
			lineSpacing = (Real64)1/6;
			break;
		case 0x3c: case 0x3e: // Unidirectional mode (one line) (ESC <)
			// We don't have a print head, so just ignore this
			break;
		case 0x63: // Initialize printer (ESC c) IW
			resetPrinter();
			break;
		case 0x48: // Set page length in lines (ESC H nnnn) IW
			{
			int x = 0;
			//convert any leading spaces in parameters to zeros
			while (x < 4)
			{
				if (params[x] == ' ') params[x] = '0';
				x++;
			}
			pageHeight = (Real64)PARAM4(0)/144;
			// trigger margins computation
			updateSwitch();
			break;
			}
		case 0x21: // Select bold font (ESC !) IW
			style |= STYLE_BOLD;
			updateFont();
			break;
		case 0x22: // Cancel bold font (ESC ") IW
			style &= ~STYLE_BOLD;
			updateFont();
			break;
		case 0x78: // Select superscript printing (ESC x) IW
			style |= STYLE_SUPERSCRIPT;
			updateFont();
			break;
		case 0x79: // Select subscript printing (ESC y) IW
			style |= STYLE_SUBSCRIPT;
			updateFont();
			break;
		case 0x7a: // Cancel superscript/subscript printing (ESC z) IW
			style &= 0xFFFF - STYLE_SUPERSCRIPT - STYLE_SUBSCRIPT;
			updateFont();
			break;
		case 0x77: // Select halfheight printing (ESC w) IW
			style |= STYLE_HALFHEIGHT;
			updateFont();
			break;
		case 0x57: // Cancel halfheight printing (ESC W) IW
			style &= ~STYLE_HALFHEIGHT;
			updateFont();
			break;
		case 0x72: // Reverse paper feed (ESC r) IW
			{
				printf("Reverse Feed\n");
				if(lineSpacing > 0) lineSpacing *= -1;
				break;
			}
		case 0x66: // Forward paper feed (ESC f) IW
			{
				if(lineSpacing < 0) lineSpacing *= -1;
				break;
			}
		case 0x61: // Select typeface (ESC a n) IW worry about this later
			break;
		case 0x6d: case 0x4d: //Same as ESC a n commands
			break;
		case 0x4c: // Set left margin (ESC L nnn) IW
			{
			int x = 0;
			//convert any leading spaces in parameters to zeros
			while (x < 4)
			{
				if (params[x] == ' ') params[x] = '0';
				x++;
			}
			leftMargin =  (Real64)(PARAM3(0)-1.0) / (Real64)cpi;
			if (curX < leftMargin)
				curX = leftMargin;
			break;
			}
		case 0x4b: // Select printing color (ESC K) IW

			switch (paramc(0))
			{
			case 0: break;
			case 1: params[0] = 4; break;
			case 2: params[0] = 1; break;
			case 3: params[0] = 2; break;
			case 4: params[0] = 5; break;
			case 5: params[0] = 6; break;
			case 6: params[0] = 3; break;
			}
			if(paramc(0)==0) color = COLOR_BLACK;
			else color = params[0]<<5;
			break;
		case 0x3d: // Internal font ID (ESC = n) IW LQ
			//Ignore for now
			break;
		case 0x3f: //Send ID string to computer (ESC ?) IW
			//insert SCC send code here
			printf("Sending ID String\n");
			iw_scc_write = true;
			break;
		case 0x52: // Repeat character c for nnn times (ESC R nnn c) IW
			{
			int x = 0;
			//convert any leading spaces in parameters to zeros
			while (x < 4)
			{
				if (params[x] == ' ') params[x] = '0';
				x++;
			}
			x = 0;
			ESCCmd = 0;
			while (x < PARAM3(0))
			{
				printChar(params[3]);
				x++;
			}
			break;
			}
		case 0x30: //Clear all tabs
			numHorizTabs = 0;
			break;
		case 0x28: // Set horizontal tabs										(ESC ( nnn,) IW
			{
			int x = 0;
			//convert any leading spaces in parameters to zeros
			while (x < 4)
			{
				if (params[x] == ' ') params[x] = '0';
				x++;
			}
			if (params[3] == '.' || (numHorizTabs>0 && horiztabs[numHorizTabs-1] > (Real64)PARAM3(0)*(1/(Real64)cpi)))
			{
				horiztabs[numHorizTabs++] = (Real64)PARAM3(0)*(1/(Real64)cpi);
				//printf("Adding tab %d, and end\n",PARAM3(0));
				//printf("Number of Tabs:%d\n",numHorizTabs);
			}
			else if (params[3] == ',' && numHorizTabs < 32)
			{
				horiztabs[numHorizTabs++] = (Real64)PARAM3(0)*(1/(Real64)cpi);
				numParam = 0;
				neededParam = 4;
				//printf("Adding tab %d, plus more\n", PARAM3(0));
				//printf("Number of Tabs:%d\n",numHorizTabs);
				return true;
			}
			x = 0;
			break;
			}
		case 0x29: // Delete horizontal tabs										(ESC ) nnn,) IW
			{
			int x = 0;
			//convert any leading spaces in parameters to zeros
			while (x < 4)
			{
				if (params[x] == ' ') params[x] = '0';
				x++;
			}
			x = 0;
			while (x < numHorizTabs)
			{
				if (horiztabs[x] == (Real64)PARAM3(0)*(1/(Real64)cpi))
				{	printf("Tab Found %d\n",PARAM3(0));
					horiztabs[x] = 0;
				}
				x++;
			}

			if (params[3] == '.')
			{
				printf("Deleting tab %d, and end\n",PARAM3(0));
			}
			else if (params[3] == ',')
			{
				numParam = 0;
				neededParam = 4;
				//printf("Deleting tab %d, plus more\n", PARAM3(0));
				return true;
			}
			x = 0;
			break;
			}
		case 0x5a: // Set switches to open (off)								(ESC Z nn) IW
			//printf ("switcha is: %x switchb is: %x\n",switcha,switchb);
			//printf ("(Setting to 0) param 0 is: %x param 1 is: %x\n",params[0],params[1]);
			switcha &= ~params[0];
			switchb &= ~params[1];
			//printf ("switcha is now: %x switchb is now: %x\n",switcha,switchb);
			updateSwitch();
			break;
		case 0x44: // Set switches to closed (on)								(ESC D nn) IW
			//printf ("switcha is: %x switchb is: %x\n",switcha,switchb);
			//printf ("(Setting to 1) param 0 is: %x param 1 is: %x\n",params[0],params[1]);
			switcha |= params[0];
			switchb |= params[1];
			//printf ("switcha is now: %x switchb is now: %x\n",switcha,switchb);
			updateSwitch();
			break;
		case 0x75: // Add one tab stop at nnn									(ESC u nnn) IW
			{
			bool haveStop = false;
			int lastEmpty;
			//If the list is full, we assume there are no empty spaces to fill until the list is scanned
			if (numHorizTabs == 32) lastEmpty = 33;
			else lastEmpty = numHorizTabs;
			int x = 0;
			//convert any leading spaces in parameters to zeros
			while (x < 4)
			{
				if (params[x] == ' ') params[x] = '0';
				x++;
			}
			x = 0;
			//see if we have the tab stop already on the list and check for any deleted entries to reuse
			while (x < numHorizTabs)
			{
				if (horiztabs[x] == (Real64)PARAM3(0)*(1/(Real64)cpi))
				{
					//printf("We have this tab already! at list entry: %d\n", x);
					haveStop = true;
				}
				if (horiztabs[x] == 0) lastEmpty = x;
				//printf("at list entry: %d\n", x);
				x++;
			}
			if (!haveStop && lastEmpty < 33)
			{
				//printf("Adding tab %d\n", PARAM3(0));
				horiztabs[lastEmpty] = (Real64)PARAM3(0)*(1/(Real64)cpi);
				if (lastEmpty == numHorizTabs) numHorizTabs++; //only increase if we don't reuse an empty tab entry
				//printf("Number of Tabs:%d\n",numHorizTabs);
			}
			}
			break;
		case 0x833: // Feed n lines of blank space								(US n) IW
			{
			int x = 0;
			while (x < paramc(0))
			{
				curY += lineSpacing;
				if (curY > bottomMargin - lineSpacing)
					newPage(true,false);
				x++;
			}
			}
		break;
		default:
			if (ESCCmd < 0x100);
				//LOG(LOG_MISC,LOG_WARN)
				//LOG_MSG("PRINTER: Skipped unsupported command ESC %c (%02X)", ESCCmd, ESCCmd);
			else;
				//LOG(LOG_MISC,LOG_WARN)
				//LOG_MSG("PRINTER: Skipped unsupported command ESC ( %c (%02X)", ESCCmd-0x200, ESCCmd-0x200);
		}

		ESCCmd = 0;
		return true;
	}

	switch (ch)
	{
	case 0x00:  // NUL is ignored by the printer
		return true;
	case 0x07:  // Beeper (BEL)
		// BEEEP!
		return true;
	case 0x08:	// Backspace (BS)
		{
			Real64 newX = curX - (1/(Real64)actcpi);
			if (hmi > 0)
				newX = curX - hmi;
			if (newX >= leftMargin)
				curX = newX;
		}
		return true;
	case 0x09:	// Tab horizontally (HT)
		{
			// Find tab right to current pos
			Real64 moveTo = -1;
			for (Bit8u i=0; i<numHorizTabs; i++)
				if (horiztabs[i] > curX)
					moveTo = horiztabs[i];
			// Nothing found => Ignore
			if (moveTo > 0 && moveTo < rightMargin)
				curX = moveTo;
		}
		return true;
	case 0x0b:	// Tab vertically (VT)
		if (numVertTabs == 0) // All tabs cancelled => Act like CR
			curX = leftMargin;
		else if (numVertTabs == 255) // No tabs set since reset => Act like LF
		{
			curX = leftMargin;
			curY += lineSpacing;
			if (curY > bottomMargin - lineSpacing)
				newPage(true,false);
		}
		else
		{
			// Find tab below current pos
			Real64 moveTo = -1;
			for (Bit8u i=0; i<numVertTabs; i++)
				if (verttabs[i] > curY)
					moveTo = verttabs[i];

			// Nothing found => Act like FF
			if (moveTo > bottomMargin - lineSpacing || moveTo < 0)
				newPage(true,false);
			else
				curY = moveTo;
		}
		return true;
	case 0x0c:		// Form feed (FF)
		newPage(true,true);
		return true;
	case 0x0d:		// Carriage Return (CR)
		curX = leftMargin;
		if ((switcha & SWITCHA_LFAFTERCR)) curY += lineSpacing; // If switch A-8 is set, send a LF after CR
		if (!autoFeed)
			return true;
	case 0x0a:		// Line feed
		//curX = leftMargin;
		curY += lineSpacing;
		if (curY > bottomMargin - lineSpacing)
			newPage(true,false);
		return true;
	case 0x0e:		//Select double width printing (SO) IW
		style |= STYLE_DOUBLEWIDTH;
		updateFont();
		return true;
	case 0x0f:		// Dis-select double width printing (SI) IW
		style &= ~STYLE_DOUBLEWIDTH;
		updateFont();
		return true;
	case 0x11:		// Select printer (DC1)
		// Ignore
		return true;
	case 0x12:		// Cancel condensed printing (DC2)
		hmi = -1;
		style &= ~STYLE_CONDENSED;
		updateFont();
		return true;
	case 0x13:		// Deselect printer (DC3)
		// Ignore
		return true;
	case 0x14:		// Cancel double-width printing (one line) (DC4)
		return true;
	case 0x18:		// Cancel line (CAN)
		return true;
	case 0x1b:		// ESC
		ESCSeen = true;
		return true;
	case 0x1f:		// unit seperator (US) Feed 1 to 15 line commands
		FSSeen = true;
		return true;
	default:
		return false;
	}

	return false;
}
#endif // HAVE_SDL

//static void PRINTER_EventHandler(Bitu param);

void Imagewriter::newPage(bool save, bool resetx)
{
	//PIC_RemoveEvents(PRINTER_EventHandler);
	if(printer_timout) timeout_dirty=false;

#ifdef HAVE_SDL
	if (save)
		outputPage();

	if(resetx) curX=leftMargin;
	curY = topMargin;

	SDL_Rect rect;
	rect.x = 0;
	rect.y = 0;
	rect.w = page->w;
	rect.h = page->h;
	SDL_FillRect(page, &rect, SDL_MapRGB(page->format, 255, 255, 255));

	/*for(int i = 0; i < 256; i++)
	{
        *((Bit8u*)page->pixels+i)=i;
	}*/
#endif // HAVE_SDL
	if (strcasecmp(output, "text") == 0) { /* Text file */
		if (textPrinterFile) {
			fclose(textPrinterFile);
			textPrinterFile = NULL;
		}
	}
}

void Imagewriter::printChar(Bit8u ch)
{
#ifdef HAVE_SDL

	charRead = true;
	if (page == NULL) return;
// Apply MSB if desired, but only if we aren't printing graphics!
	if (msb != 255) {
		if (!bitGraph.remBytes) ch &= 0x7F;
	}
#endif // HAVE_SDL
	if (strcasecmp(output, "text") == 0) {
		if (!textPrinterFile) {
			textPrinterFile = fopen(textPrinterFileName,"ab");
		}
		fprintf(textPrinterFile,"%c",ch);
		fflush(textPrinterFile);
		return;
	}
#ifdef HAVE_SDL

	// Are we currently printing a bit graphic?
	if (bitGraph.remBytes > 0) {
		printBitGraph(ch);
		return;
	}
	// Print everything?
	if (numPrintAsChar > 0) numPrintAsChar--;
	else if (processCommandChar(ch)) return;

	// Do not print if no font is available
	if (!curFont) return;
	if(ch==0x1) ch=0x20;

	// Find the glyph for the char to render
	FT_UInt index = FT_Get_Char_Index(curFont, curMap[ch]);

	// Load the glyph
	FT_Load_Glyph(curFont, index, FT_LOAD_DEFAULT);


	// Render a high-quality bitmap
	FT_Render_Glyph(curFont->glyph, FT_RENDER_MODE_NORMAL);

	Bit16u penX = PIXX + curFont->glyph->bitmap_left;
	Bit16u penY = PIXY - curFont->glyph->bitmap_top + curFont->size->metrics.ascender/64;

	//if (style & STYLE_SUBSCRIPT) penY += curFont->glyph->bitmap.rows / 2;
	//if (style & STYLE_HALFHEIGHT) penY += curFont->glyph->bitmap.rows / 4;
	if (style & STYLE_SUBSCRIPT) penY += 20;
	if (style & STYLE_SUPERSCRIPT) penY -= 10;
	if (style & STYLE_HALFHEIGHT) penY += 15;

	// Copy bitmap into page
	SDL_LockSurface(page);

	blitGlyph(curFont->glyph->bitmap, penX, penY, false);
	blitGlyph(curFont->glyph->bitmap, penX+1, penY, true);

	// Bold => Print the glyph a second time one pixel to the right
	// or be a bit more bold...
	if (style & STYLE_BOLD) {
		blitGlyph(curFont->glyph->bitmap, penX+1, penY, true);
		blitGlyph(curFont->glyph->bitmap, penX+2, penY, true);
		blitGlyph(curFont->glyph->bitmap, penX+3, penY, true);
	}
	SDL_UnlockSurface(page);

	// For line printing
	Bit16u lineStart = PIXX;
	// Print a slashed zero if the softswitch B-1 is set
	if(switchb & 1 && ch=='0') slashzero(penX,penY);
	// advance the cursor to the right
	Real64 x_advance;
	if (style &	STYLE_PROP)
		x_advance = (Real64)((Real64)(curFont->glyph->advance.x)/(Real64)(dpi*64));
	else {
		x_advance = 1/(Real64)actcpi;
	}
	x_advance += extraIntraSpace;
    curX += x_advance;

	// Draw lines if desired
	if ((score != SCORE_NONE) && (style &
		(STYLE_UNDERLINE)))
	{
		// Find out where to put the line
		Bit16u lineY = PIXY;
		double height = (curFont->size->metrics.height>>6); // TODO height is fixed point madness...

		if (style & STYLE_UNDERLINE) lineY = PIXY + (Bit16u)(height*0.9);

		drawLine(lineStart, PIXX, lineY, score==SCORE_SINGLEBROKEN || score==SCORE_DOUBLEBROKEN);

		// draw second line if needed
		if ((score == SCORE_DOUBLE)||(score == SCORE_DOUBLEBROKEN))
			drawLine(lineStart, PIXX, lineY + 5, score==SCORE_SINGLEBROKEN || score==SCORE_DOUBLEBROKEN);
	}
	// If the next character would go beyond the right margin, line-wrap.
	if((curX + x_advance) > rightMargin) {
		curX = leftMargin;
		curY += lineSpacing;
		if (curY > bottomMargin - lineSpacing) newPage(true,false);
	}
#endif // HAVE_SDL
}

#ifdef HAVE_SDL
void Imagewriter::blitGlyph(FT_Bitmap bitmap, Bit16u destx, Bit16u desty, bool add) {
	for (Bitu y=0; y<bitmap.rows; y++) {
		for (Bitu x=0; x<bitmap.width; x++) {
			// Read pixel from glyph bitmap
			Bit8u source = *(bitmap.buffer + x + y*bitmap.pitch);

			// Ignore background and don't go over the border
			if (source > 0 && (destx+x < page->w) && (desty+y < page->h) ) {
				Bit8u* target = (Bit8u*)page->pixels + (x+destx) + (y+desty)*page->pitch;
				source>>=3;

				if (add) {
					if (((*target)&0x1f )+ source > 31) *target |= (color|0x1f);
					else {
						*target += source;
						*target |= color;
					}
				}
				else *target = source|color;
			}
		}
	}
}

void Imagewriter::drawLine(Bitu fromx, Bitu tox, Bitu y, bool broken)
{
	SDL_LockSurface(page);

	Bitu breakmod = dpi / 15;
	Bitu gapstart = (breakmod * 4)/5;

	// Draw anti-aliased line
	for (Bitu x=fromx; x<=tox; x++)
	{
		// Skip parts if broken line or going over the border
		if ((!broken || (x%breakmod <= gapstart)) && (x < page->w))
		{
			if (y > 0 && (y-1) < page->h)
				*((Bit8u*)page->pixels + x + (y-1)*page->pitch) = 240;
			if (y < page->h)
				*((Bit8u*)page->pixels + x + y*page->pitch) = !broken?255:240;
			if (y+1 < page->h)
				*((Bit8u*)page->pixels + x + (y+1)*page->pitch) = 240;
		}
	}
	SDL_UnlockSurface(page);
}

void Imagewriter::setAutofeed(bool feed) {
	autoFeed = feed;
}

bool Imagewriter::getAutofeed() {
	return autoFeed;
}

bool Imagewriter::isBusy() {
	// We're never busy
	return false;
}

bool Imagewriter::ack() {
	// Acknowledge last char read
	if(charRead) {
		charRead=false;
		return true;
	}
	return false;
}

void Imagewriter::setupBitImage(Bit8u dens, Bit16u numCols) {
	switch (dens)
	{
	case 0:
		bitGraph.horizDens = 72;
		bitGraph.vertDens = 72;
		bitGraph.adjacent = true;
		bitGraph.bytesColumn = 1;
		break;
	case 1:
		bitGraph.horizDens = 80;
		bitGraph.vertDens = 72;
		bitGraph.adjacent = true;
		bitGraph.bytesColumn = 1;
		break;
	case 2:
		bitGraph.horizDens = 96;
		bitGraph.vertDens = 72;
		bitGraph.adjacent = true;
		bitGraph.bytesColumn = 1;
		break;
	case 3:
		bitGraph.horizDens = 107;
		bitGraph.vertDens = 72;
		bitGraph.adjacent = true;
		bitGraph.bytesColumn = 1;
		break;
	case 4:
		bitGraph.horizDens = 120;
		bitGraph.vertDens = 72;
		bitGraph.adjacent = true;
		bitGraph.bytesColumn = 1;
		break;
	case 5:
		bitGraph.horizDens = 136;
		bitGraph.vertDens = 72;
		bitGraph.adjacent = true;
		bitGraph.bytesColumn = 1;
		break;
	case 6:
		bitGraph.horizDens = 144;
		bitGraph.vertDens = 72;
		bitGraph.adjacent = true;
		bitGraph.bytesColumn = 1;
		break;
	case 7:
		bitGraph.horizDens = 160;
		bitGraph.vertDens = 72;
		bitGraph.adjacent = true;
		bitGraph.bytesColumn = 1;
		break;
	//Imagewriter LQ resolutions
	case 8:
		bitGraph.horizDens = 144;
		bitGraph.vertDens = 216;
		bitGraph.adjacent = true;
		bitGraph.bytesColumn = 3;
		break;
	case 9:
		bitGraph.horizDens = 160;
		bitGraph.vertDens = 216;
		bitGraph.adjacent = true;
		bitGraph.bytesColumn = 3;
		break;
	case 10:
		bitGraph.horizDens = 192;
		bitGraph.vertDens = 216;
		bitGraph.adjacent = true;
		bitGraph.bytesColumn = 3;
		break;
	case 11:
		bitGraph.horizDens = 216;
		bitGraph.vertDens = 216;
		bitGraph.adjacent = true;
		bitGraph.bytesColumn = 3;
		break;
	case 12:
		bitGraph.horizDens = 240;
		bitGraph.vertDens = 216;
		bitGraph.adjacent = true;
		bitGraph.bytesColumn = 3;
		break;
	case 13:
		bitGraph.horizDens = 272;
		bitGraph.vertDens = 216;
		bitGraph.adjacent = true;
		bitGraph.bytesColumn = 3;
		break;
	case 14:
		bitGraph.horizDens = 288;
		bitGraph.vertDens = 216;
		bitGraph.adjacent = true;
		bitGraph.bytesColumn = 3;
		break;
	case 15:
		bitGraph.horizDens = 320;
		bitGraph.vertDens = 216;
		bitGraph.adjacent = true;
		bitGraph.bytesColumn = 3;
		break;
	default:
		//break;
		printf("PRINTER: Unsupported bit image density");
	}

	bitGraph.remBytes = numCols * bitGraph.bytesColumn;
	bitGraph.readBytesColumn = 0;
}

void Imagewriter::printBitGraph(Bit8u ch)
{
	bitGraph.column[bitGraph.readBytesColumn++] = ch;
	bitGraph.remBytes--;

	// Only print after reading a full column
	if (bitGraph.readBytesColumn < bitGraph.bytesColumn)
		return;

	Real64 oldY = curY;
	SDL_LockSurface(page);

	// When page dpi is greater than graphics dpi, the drawn pixels get "bigger"
	Bitu pixsizeX=1;
	Bitu pixsizeY=1;
	if(bitGraph.adjacent) {
		pixsizeX = dpi/bitGraph.horizDens > 0? dpi/bitGraph.horizDens : 1;
		if(dpi%bitGraph.horizDens && bitGraph.horizDens < dpi)
		{
			if(PIXX%(bitGraph.horizDens*8) || (PIXX == 0)) //Primative scaling function
			{
				pixsizeX++;
			}
		}
		pixsizeY = dpi/bitGraph.vertDens > 0? dpi/bitGraph.vertDens : 1;
		if(bitGraph.vertDens == 216)
		{
			if(PIXY%(bitGraph.vertDens*8) || (PIXY == 0)) //Primative scaling function
			{
				pixsizeY++;
			}
		}
	}
	if ((printRes > 7) && (verticalDot != 0)) //for ESC t
	{
		curY += (Real64)verticalDot/(Real64)bitGraph.vertDens;
	}
	// TODO figure this out for 360dpi mode in windows

//	Bitu pixsizeX = dpi/bitGraph.horizDens > 0? dpi/bitGraph.horizDens : 1;
//	Bitu pixsizeY = dpi/bitGraph.vertDens > 0? dpi/bitGraph.vertDens : 1;

	for (Bitu i=0; i<bitGraph.bytesColumn; i++) // for each byte
	{
		for (Bitu j=1; j<256; j<<=1) { // for each bit
			if (bitGraph.column[i] & j) {
				for (Bitu xx=0; xx<pixsizeX; xx++)
					for (Bitu yy=0; yy<pixsizeY; yy++) {
						if (((PIXX + xx) < page->w) && ((PIXY + yy) < page->h))
							*((Bit8u*)page->pixels + (PIXX+xx) + (PIXY+yy)*page->pitch) |= (color|0x1F);
					}
			} // else white pixel
			curY += (Real64)1/(Real64)bitGraph.vertDens; // TODO line wrap?
		}
	}
	SDL_UnlockSurface(page);

	curY = oldY;
	bitGraph.readBytesColumn = 0;

	// Advance to the left
	curX += (Real64)1/(Real64)bitGraph.horizDens;
}
#endif // HAVE_SDL

void Imagewriter::formFeed()
{
#ifdef HAVE_SDL
	// Don't output blank pages
	newPage(!isBlank(),true);
	finishMultipage();
#endif // HAVE_SDL
}

#ifdef HAVE_SDL
static void findNextName(char* front, char* ext, char* fname)
{
	document_path = "";
	Bitu i = 1;
	Bitu slen = strlen(document_path);
	if(slen>(200-15)) {
		fname[0]=0;
		return;
	}
	FILE *test = NULL;
	do
	{
		strcpy(fname, document_path);
		printf("%s",fname);
#ifdef WIN32
		const char* const pathstring = ".\\%s%d%s";
#else
		const char* const pathstring = "./%s%d%s";
#endif
		sprintf(fname+strlen(fname), pathstring, front,i++,ext);
		test = fopen(fname, "rb");
		if (test != NULL)
			fclose(test);
	}
	while (test != NULL );
}

void Imagewriter::outputPage()
{/*
	SDL_Surface *screen;
	screen = SDL_SetVideoMode(1024, 768, 16, SDL_DOUBLEBUF | SDL_RESIZABLE);
if (screen == NULL) {
	printf("Unable to set video mode: %s\n", SDL_GetError());
}
SDL_Surface *image;
SDL_LockSurface(page);
image = SDL_DisplayFormat(page);
SDL_UnlockSurface(page);
SDL_Rect src, dest;

src.x = 0;
src.y = 0;
src.w = image->w;
src.h = image->h;

dest.x = 100;
dest.y = 100;
dest.w = image->w;
dest.h = image->h;

SDL_BlitSurface(image, &src, screen, &dest);
SDL_Flip(screen);

SDL_Delay(2000);
SDL_FreeSurface(image);*/
	char fname[200];
	if (strcasecmp(output, "printer") == 0)
	{
#if defined (WIN32)
		if (multipageOutput && outputHandle == NULL)
		{
			ShowCursor(1);
			PRINTDLG pd;
			pd.lStructSize = sizeof(PRINTDLG);
			pd.hDevMode = (HANDLE) NULL;
			pd.hDevNames = (HANDLE) NULL;
			pd.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE;
			pd.hwndOwner = NULL;
			pd.hDC = (HDC) NULL;
			pd.nFromPage = 0xFFFF;
			pd.nToPage = 0xFFFF;
			pd.nMinPage = 1;
			pd.nMaxPage = 0xFFFF;
			pd.nCopies = 1;
			pd.hInstance = NULL;
			pd.lCustData = 0L;
			pd.lpfnPrintHook = (LPPRINTHOOKPROC) NULL;
			pd.lpfnSetupHook = (LPSETUPHOOKPROC) NULL;
			pd.lpPrintTemplateName = (LPCSTR) NULL;
			pd.lpSetupTemplateName = (LPCSTR)  NULL;
			pd.hPrintTemplate = (HANDLE) NULL;
			pd.hSetupTemplate = (HANDLE) NULL;
				if(!PrintDlg(&pd))
				{
					//If user clicks cancel, show warning dialog and force all output to bitmaps as failsafe.
					MessageBox(NULL,"You did not select a printer.\nAll output from this print job will be saved as bitmap files.",NULL,MB_ICONEXCLAMATION);
					findNextName((char *)"page", (char *)".bmp", &fname[0]);
					SDL_SaveBMP(page, fname); //Save first page as bitmap.
					outputHandle = printerDC;
					printerDC = NULL;
					ShowCursor(0);
					return;
				}
				else
				{
					//Create device context.
					printerDC = pd.hDC;
					ShowCursor(0);
				}
		}
		if (!printerDC) //Fall thru for subsequent pages if printer dialog was cancelled.
		{
			findNextName((char *)"page", (char *)".bmp", &fname[0]);
			SDL_SaveBMP(page, fname); //Save remaining pages.
			return;
		}
		Bit32u physW = GetDeviceCaps(printerDC, PHYSICALWIDTH);
		Bit32u physH = GetDeviceCaps(printerDC, PHYSICALHEIGHT);
		Bit16u printeroffsetW = GetDeviceCaps(printerDC, PHYSICALOFFSETX);  //printer x offset in actual pixels
		Bit16u printeroffsetH = GetDeviceCaps(printerDC, PHYSICALOFFSETY); //printer y offset in actual pixels
		Bit16u deviceDPIW = GetDeviceCaps(printerDC, LOGPIXELSX);
		Bit16u deviceDPIH = GetDeviceCaps(printerDC, LOGPIXELSY);
		Real64 physoffsetW = (Real64)printeroffsetW/deviceDPIW; //printer x offset in inches
		Real64 physoffsetH = (Real64)printeroffsetH/deviceDPIH; //printer y offset in inches
		Bit16u dpiW = page->w/defaultPageWidth; //Get currently set DPI of the emulated printer in an indirect way
		Bit16u dpiH = page->h/defaultPageHeight;
		Real64 soffsetW = physoffsetW*dpiW; //virtual page x offset in actual pixels
		Real64 soffsetH = physoffsetH*dpiH; //virtual page y offset in actual pixels
		HDC memHDC = CreateCompatibleDC(printerDC);
		BITMAPINFO *BitmapInfo;
		HBITMAP bitmap;

		// Start new printer job?
		if (outputHandle == NULL)
		{
			DOCINFO docinfo;
			docinfo.cbSize = sizeof(docinfo);
			docinfo.lpszDocName = "GSport Virtual ImageWriter";
			docinfo.lpszOutput = NULL;
			docinfo.lpszDatatype = NULL;
			docinfo.fwType = 0;

			StartDoc(printerDC, &docinfo);
			multiPageCounter = 1;
		}
		SDL_LockSurface(page);
		StartPage(printerDC);
        DWORD TotalSize;
        HGDIOBJ Prev;
        void* Pixels;
        BitmapInfo = (BITMAPINFO*)
          malloc (sizeof (BITMAPINFO)+255*sizeof (RGBQUAD));
        memset (BitmapInfo,0,sizeof (bitmap));
        BitmapInfo->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        BitmapInfo->bmiHeader.biWidth = page->w;
        BitmapInfo->bmiHeader.biHeight = -page->h;
        BitmapInfo->bmiHeader.biPlanes = 1;
        BitmapInfo->bmiHeader.biBitCount = page->format->BitsPerPixel;
        BitmapInfo->bmiHeader.biCompression = BI_RGB;
        BitmapInfo->bmiHeader.biSizeImage = page->h * page->pitch;
        BitmapInfo->bmiHeader.biXPelsPerMeter = 0;
        BitmapInfo->bmiHeader.biYPelsPerMeter = 0;
        BitmapInfo->bmiHeader.biClrUsed = page->format->palette->ncolors;
        BitmapInfo->bmiHeader.biClrImportant = 0;
        if (page->format->palette) {
          for (int I=0; I<page->format->palette->ncolors; I++) {
            BitmapInfo->bmiColors[I].rgbRed =
              (page->format->palette->colors+I)->r;
            BitmapInfo->bmiColors[I].rgbGreen =
              (page->format->palette->colors+I)->g;
            BitmapInfo->bmiColors[I].rgbBlue =
              (page->format->palette->colors+I)->b;
          }
        }
        memHDC = CreateCompatibleDC(printerDC);
        if (memHDC) {
          bitmap = CreateDIBSection(memHDC, BitmapInfo, DIB_RGB_COLORS,
                                    (&Pixels), NULL, 0);
          if (bitmap) {
            memcpy (Pixels, page->pixels,
		    BitmapInfo->bmiHeader.biSizeImage);
            Prev = SelectObject (memHDC, bitmap);
			StretchBlt(printerDC, 0, 0, physW, physH, memHDC, soffsetW, soffsetH, page->w, page->h, SRCCOPY);
            SelectObject (memHDC,Prev);
            DeleteObject (bitmap);
          }
        }
        free (BitmapInfo);
		SDL_UnlockSurface(page);
		EndPage(printerDC);

		if (multipageOutput)
		{
			multiPageCounter++;
			outputHandle = printerDC;
		}
		else
		{
			EndDoc(printerDC);
			outputHandle = NULL;
		}
		DeleteObject(bitmap);
		DeleteDC(memHDC);
#else
		//LOG_MSG("PRINTER: Direct printing not supported under this OS");
#endif
	}
#ifdef C_LIBPNG
	else if (strcasecmp(output, "png") == 0)
	{
		// Find a page that does not exists
		findNextName((char *)"page", (char *)".png", &fname[0]);

		png_structp png_ptr;
		png_infop info_ptr;
		png_bytep * row_pointers;
		png_color palette[256];
		Bitu i;

		/* Open the actual file */
		FILE * fp=fopen(fname,"wb");
		if (!fp)
		{
			//LOG(LOG_MISC,LOG_ERROR)("PRINTER: Can't open file %s for printer output", fname);
			return;
		}

		/* First try to alloacte the png structures */
		png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL,NULL, NULL);
		if (!png_ptr) return;
		info_ptr = png_create_info_struct(png_ptr);
		if (!info_ptr) {
			png_destroy_write_struct(&png_ptr,(png_infopp)NULL);
			return;
		}

		/* Finalize the initing of png library */
		png_init_io(png_ptr, fp);
		png_set_compression_level(png_ptr,Z_BEST_COMPRESSION);

		/* set other zlib parameters */
		png_set_compression_mem_level(png_ptr, 8);
		png_set_compression_strategy(png_ptr,Z_DEFAULT_STRATEGY);
		png_set_compression_window_bits(png_ptr, 15);
		png_set_compression_method(png_ptr, 8);
		png_set_compression_buffer_size(png_ptr, 8192);


		png_set_IHDR(png_ptr, info_ptr, page->w, page->h,
			8, PNG_COLOR_TYPE_PALETTE, PNG_INTERLACE_NONE,
			PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
		for (i=0;i<256;i++)
		{
			palette[i].red = page->format->palette->colors[i].r;
			palette[i].green = page->format->palette->colors[i].g;
			palette[i].blue = page->format->palette->colors[i].b;
		}
		png_set_PLTE(png_ptr, info_ptr, palette,256);
		png_set_packing(png_ptr);
		SDL_LockSurface(page);

		// Allocate an array of scanline pointers
		row_pointers = (png_bytep*)malloc(page->h*sizeof(png_bytep));
		for (i=0; i<page->h; i++)
			row_pointers[i] = ((Bit8u*)page->pixels+(i*page->pitch));

		// tell the png library what to encode.
		png_set_rows(png_ptr, info_ptr, row_pointers);

		// Write image to file
		png_write_png(png_ptr, info_ptr, 0, NULL);




		SDL_UnlockSurface(page);

		/*close file*/
		fclose(fp);

		/*Destroy PNG structs*/
		png_destroy_write_struct(&png_ptr, &info_ptr);

		/*clean up dynamically allocated RAM.*/
		free(row_pointers);
	}
#endif
	else if (strcasecmp(output, "colorps") == 0)
	{
		FILE* psfile = NULL;

		// Continue postscript file?
		if (outputHandle != NULL)
			psfile = (FILE*)outputHandle;

		// Create new file?
		if (psfile == NULL)
		{
			if (!multipageOutput)
				findNextName((char *)"page", (char *)".ps", &fname[0]);
			else
				findNextName((char *)"doc", (char *)".ps", &fname[0]);

			psfile = fopen(fname, "wb");
			if (!psfile)
			{
				//LOG(LOG_MISC,LOG_ERROR)("PRINTER: Can't open file %s for printer output", fname);
				return;
			}

			// Print header
			fprintf(psfile, "%%!PS-Adobe-3.0\n");
			fprintf(psfile, "%%%%Pages: (atend)\n");
			fprintf(psfile, "%%%%BoundingBox: 0 0 %i %i\n", (Bit16u)(defaultPageWidth*72), (Bit16u)(defaultPageHeight*72));
			fprintf(psfile, "%%%%Creator: GSport Virtual Printer\n");
			fprintf(psfile, "%%%%DocumentData: Clean7Bit\n");
			fprintf(psfile, "%%%%LanguageLevel: 2\n");
			fprintf(psfile, "%%%%EndComments\n");
			multiPageCounter = 1;
		}

		fprintf(psfile, "%%%%Page: %i %i\n", multiPageCounter, multiPageCounter);
		fprintf(psfile, "%i %i scale\n", (Bit16u)(defaultPageWidth*72), (Bit16u)(defaultPageHeight*72));
		fprintf(psfile, "%i %i 8 [%i 0 0 -%i 0 %i]\n", page->w, page->h, page->w, page->h, page->h);
		fprintf(psfile, "currentfile\n");
		fprintf(psfile, "/ASCII85Decode filter\n");
		fprintf(psfile, "/RunLengthDecode filter\n");
		fprintf(psfile, "false 3\n");
		fprintf(psfile, "colorimage\n");

		SDL_LockSurface(page);
		Bit8u * templine;
		templine = (Bit8u*) malloc(page->w*3);
		Bit32u x = 0;
		Bit32u numy = page->h;
		Bit32u numx = page->w;
		Bit32u numpix = page->w*3;
		Bit32u pix = 0;
		Bit32u currDot = 0;
		Bit32u y = 0;
		Bit32u plane = 0;
		Bit8u r, g, b;
		ASCII85BufferPos = ASCII85CurCol = 0;
		for (y=0; y < numy;y++)
		{
			currDot = 0;
			for (x = 0; x < numx; x++)
			{
				SDL_GetRGB(getxyPixel(x,y), page->format, &r, &g, &b);
				templine[currDot] = ~r; currDot++;
				templine[currDot] = ~g; currDot++;
				templine[currDot] = ~b; currDot++;
			}
			// Compress data using RLE
			pix = 0;
			while (pix < numpix)
			{
			if ((pix < numpix-2) && (templine[pix] == templine[pix+1]) && (templine[pix] == templine[pix+2]))
			{
				// Found three or more pixels with the same color
				Bit8u sameCount = 3;
				Bit8u col = templine[pix];
				while (sameCount < 128 && sameCount+pix < numpix && col == templine[pix+sameCount])
					sameCount++;

				fprintASCII85(psfile, 257-sameCount);
				fprintASCII85(psfile, 255-col);

				// Skip ahead
				pix += sameCount;
			}
			else
			{
				// Find end of heterogenous area
				Bit8u diffCount = 1;
				while (diffCount < 128 && diffCount+pix < numpix &&
					(
						   (diffCount+pix < numpix-2)
						|| (templine[pix+diffCount] != templine[pix+diffCount+1])
						|| (templine[pix+diffCount] != templine[pix+diffCount+2])
					))
					diffCount++;

				fprintASCII85(psfile, diffCount-1);
				for (Bit8u i=0; i<diffCount; i++)
					fprintASCII85(psfile, 255-templine[pix++]);
			}
			}
		}
		// Write EOD for RLE and ASCII85
		fprintASCII85(psfile, 128);
		fprintASCII85(psfile, 256);

		SDL_UnlockSurface(page);
		free(templine);

		fprintf(psfile, "showpage\n");

		if (multipageOutput)
		{
			multiPageCounter++;
			outputHandle = psfile;
		}
		else
		{
			fprintf(psfile, "%%%%Pages: 1\n");
			fprintf(psfile, "%%%%EOF\n");
			fclose(psfile);
			outputHandle = NULL;
		}
	}
	else if (strcasecmp(output, "ps") == 0)
	{
		FILE* psfile = NULL;
		printf("%d\n",getPixel(2));

		// Continue postscript file?
		if (outputHandle != NULL)
			psfile = (FILE*)outputHandle;

		// Create new file?
		if (psfile == NULL)
		{
			if (!multipageOutput)
				findNextName((char *)"page", (char *)".ps", &fname[0]);
			else
				findNextName((char *)"doc", (char *)".ps", &fname[0]);

			psfile = fopen(fname, "wb");
			if (!psfile)
			{
				//LOG(LOG_MISC,LOG_ERROR)("PRINTER: Can't open file %s for printer output", fname);
				return;
			}

			// Print header
			fprintf(psfile, "%%!PS-Adobe-3.0\n");
			fprintf(psfile, "%%%%Pages: (atend)\n");
			fprintf(psfile, "%%%%BoundingBox: 0 0 %i %i\n", (Bit16u)(defaultPageWidth*72), (Bit16u)(defaultPageHeight*72));
			fprintf(psfile, "%%%%Creator: GSport Virtual Printer\n");
			fprintf(psfile, "%%%%DocumentData: Clean7Bit\n");
			fprintf(psfile, "%%%%LanguageLevel: 2\n");
			fprintf(psfile, "%%%%EndComments\n");
			multiPageCounter = 1;
		}

		fprintf(psfile, "%%%%Page: %i %i\n", multiPageCounter, multiPageCounter);
		fprintf(psfile, "%i %i scale\n", (Bit16u)(defaultPageWidth*72), (Bit16u)(defaultPageHeight*72));
		fprintf(psfile, "%i %i 8 [%i 0 0 -%i 0 %i]\n", page->w, page->h, page->w, page->h, page->h);
		fprintf(psfile, "currentfile\n");
		fprintf(psfile, "/ASCII85Decode filter\n");
		fprintf(psfile, "/RunLengthDecode filter\n");
		fprintf(psfile, "image\n");

		SDL_LockSurface(page);

		Bit32u pix = 0;
		Bit32u numpix = page->h*page->w;
		ASCII85BufferPos = ASCII85CurCol = 0;

		while (pix < numpix)
		{
			// Compress data using RLE

			if ((pix < numpix-2) && (getPixel(pix) == getPixel(pix+1)) && (getPixel(pix) == getPixel(pix+2)))
			{
				// Found three or more pixels with the same color
				Bit8u sameCount = 3;
				Bit8u col = getPixel(pix);
				while (sameCount < 128 && sameCount+pix < numpix && col == getPixel(pix+sameCount))
					sameCount++;

				fprintASCII85(psfile, 257-sameCount);
				fprintASCII85(psfile, 255-col);

				// Skip ahead
				pix += sameCount;
			}
			else
			{
				// Find end of heterogenous area
				Bit8u diffCount = 1;
				while (diffCount < 128 && diffCount+pix < numpix &&
					(
						   (diffCount+pix < numpix-2)
						|| (getPixel(pix+diffCount) != getPixel(pix+diffCount+1))
						|| (getPixel(pix+diffCount) != getPixel(pix+diffCount+2))
					))
					diffCount++;

				fprintASCII85(psfile, diffCount-1);
				for (Bit8u i=0; i<diffCount; i++)
					fprintASCII85(psfile, 255-getPixel(pix++));
			}
		}

		// Write EOD for RLE and ASCII85
		fprintASCII85(psfile, 128);
		fprintASCII85(psfile, 256);

		SDL_UnlockSurface(page);

		fprintf(psfile, "showpage\n");

		if (multipageOutput)
		{
			multiPageCounter++;
			outputHandle = psfile;
		}
		else
		{
			fprintf(psfile, "%%%%Pages: 1\n");
			fprintf(psfile, "%%%%EOF\n");
			fclose(psfile);
			outputHandle = NULL;
		}
	}
	else
	{
		// Find a page that does not exists
		findNextName((char *)"page", (char *)".bmp", &fname[0]);
		SDL_SaveBMP(page, fname);
	}
}

void Imagewriter::fprintASCII85(FILE* f, Bit16u b)
{
	if (b != 256)
	{
		if (b < 256)
			ASCII85Buffer[ASCII85BufferPos++] = (Bit8u)b;

		if (ASCII85BufferPos == 4 || b == 257)
		{
			Bit32u num = (Bit32u)ASCII85Buffer[0] << 24 | (Bit32u)ASCII85Buffer[1] << 16 | (Bit32u)ASCII85Buffer[2] << 8 | (Bit32u)ASCII85Buffer[3];

			// Deal with special case
			if (num == 0 && b != 257)
			{
				fprintf(f, "z");
				if (++ASCII85CurCol >= 79)
				{
					ASCII85CurCol = 0;
					fprintf(f, "\n");
				}
			}
			else
			{
				char buffer[5];
				for (Bit8s i=4; i>=0; i--)
				{
					buffer[i] = (Bit8u)((Bit32u)num % (Bit32u)85);
					buffer[i] += 33;
					num /= (Bit32u)85;
				}

				// Make sure a line never starts with a % (which may be mistaken as start of a comment)
				if (ASCII85CurCol == 0 && buffer[0] == '%')
					fprintf(f, " ");

				for (int i=0; i<((b != 257)?5:ASCII85BufferPos+1); i++)
				{
					fprintf(f, "%c", buffer[i]);
					if (++ASCII85CurCol >= 79)
					{
						ASCII85CurCol = 0;
						fprintf(f, "\n");
					}
				}
			}

			ASCII85BufferPos = 0;
		}

	}
	else // Close string
	{
		// Partial tupel if there are still bytes in the buffer
		if (ASCII85BufferPos > 0)
		{
			for (Bit8u i = ASCII85BufferPos; i < 4; i++)
				ASCII85Buffer[i] = 0;

			fprintASCII85(f, 257);
		}

		fprintf(f, "~");
		fprintf(f, ">\n");
	}
}

void Imagewriter::finishMultipage()
{
	if (outputHandle != NULL)
	{
		if (strcasecmp(output, "ps") == 0)
		{
			FILE* psfile = (FILE*)outputHandle;
			fprintf(psfile, "%%%%Pages: %i\n", multiPageCounter);
			fprintf(psfile, "%%%%EOF\n");
			fclose(psfile);
		}
		if (strcasecmp(output, "colorps") == 0)
		{
			FILE* psfile = (FILE*)outputHandle;
			fprintf(psfile, "%%%%Pages: %i\n", multiPageCounter);
			fprintf(psfile, "%%%%EOF\n");
			fclose(psfile);
		}
		else if (strcasecmp(output, "printer") == 0)
		{
#if defined (WIN32)
			EndDoc(printerDC);
#endif
		}
		outputHandle = NULL;
	}
}

bool Imagewriter::isBlank() {
	bool blank = true;
	SDL_LockSurface(page);

	for (Bit16u y=0; y<page->h; y++)
		for (Bit16u x=0; x<page->w; x++)
			if (*((Bit8u*)page->pixels + x + (y*page->pitch)) != 0)
				blank = false;

	SDL_UnlockSurface(page);
	return blank;
}

Bit8u Imagewriter::getxyPixel(Bit32u x,Bit32u y) {
	Bit8u *p;

	/* get the X/Y values within the bounds of this surface */
	if ((unsigned) x > (unsigned) page->w - 1u)
    x = (x < 0) ? 0 : page->w - 1;
	if ((unsigned) y > (unsigned) page->h - 1u)
    y = (y < 0) ? 0 : page->h - 1;

	/* Set a pointer to the exact location in memory of the pixel
     in question: */

	p = (Bit8u *) (((Bit8u *) page->pixels) +	/* Start at top of RAM */
		 (y * page->pitch) +	/* Go down Y lines */
						x);		/* Go in X pixels */


	/* Return the correctly-sized piece of data containing the
	* pixel's value (an 8-bit palette value, or a 16-, 24- or 32-bit
	* RGB value) */

	return (*p);
}
Bit8u Imagewriter::getPixel(Bit32u num) {
	Bit32u pixel = *((Bit8u*)page->pixels + (num % page->w) + ((num / page->w) * page->pitch));
	return *((Bit8u*)page->pixels + (num % page->w) + ((num / page->w) * page->pitch));
}
#endif // HAVE_SDL


//Interfaces to C code
extern "C" void imagewriter_init(int pdpi, int ppaper, int banner, char* poutput, bool mpage)
{
	if (defaultImagewriter != NULL) return; //if Imagewriter on this port is initialized, reuse it
	defaultImagewriter = new Imagewriter(pdpi, ppaper, banner, poutput, mpage);
}
extern "C" void imagewriter_loop(Bit8u pchar)
{
    if (defaultImagewriter == NULL) return;
	defaultImagewriter->printChar(pchar);
}

extern "C" void imagewriter_close()
{
	delete defaultImagewriter;
	defaultImagewriter = NULL;
}
extern "C" void imagewriter_feed()
{
	if(defaultImagewriter == NULL) return;
	defaultImagewriter->formFeed();
}
