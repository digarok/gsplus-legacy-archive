/*
   GSPLUS - Advanced Apple IIGS Emulator Environment
   Based on the KEGS emulator written by Kent Dickey
   See COPYRIGHT.txt for Copyright information
   See LICENSE.txt for license (GPL v2)
 */

// fps shiz
unsigned int lastTime = 0, currentTime, frames;

// @todo: mouse clip bugs.. great western shootout. Paint 8/16.   still in win32
#include "SDL.h"
#include "SDL_image.h"
#include "glog.h"

#include <stdbool.h>
#include <time.h>
#include <stdlib.h>
#include <signal.h>
#include <libgen.h>  // just for basename :P
#include <string.h>
#include "defc.h"
#ifdef HAVE_ICON    // Currently a flag because not supported outside of SDL builds.  Looking at full solution.
  #include "icongs.h"
#endif
#ifdef _WIN32
  #include <unistd.h>
#endif
// BITMASKS
#define ShiftMask       1
#define ControlMask     4
#define LockMask        2

#include "adb_keycodes.h"

int g_use_shmem = 0;

int g_num_check_input_calls = 0;
int g_check_input_flush_rate = 2;
int g_win_status_debug = 0;                     // Current visibility of status lines.
int g_win_status_debug_request = 0;     // Desired visibility of status lines.
int g_screen_mdepth = 0;
int kb_shift_control_state = 0;

void debuginfo_renderer(SDL_Renderer *r);
void x_take_screenshot(); // screenshot stuff
void x_grabmouse();
int g_screenshot_requested = 0; // DB to know if we want to save a screenshot
extern char g_config_gsplus_name[];
extern char g_config_gsplus_screenshot_dir[];
int screenshot_index = 0;  // allows us to save time by not scanning from 0 each time
char screenshot_filename[256];

extern int g_fullscreen;  // only checked at start if set via CLI, otherwise it's set via function call x_full_screen()
extern int g_grabmouse;
extern int g_highdpi;
extern int g_borderless;
extern int g_resizeable;
extern int g_noaspect;
extern int g_novsync;
extern int g_nohwaccel;
extern int g_fullscreen_desktop;
extern int g_scanline_simulator;
extern int g_startx;
extern int g_starty;
extern int g_startw;
extern int g_starth;
extern int g_screen_depth;
extern int g_quit_sim_now;
extern int g_border_sides_refresh_needed;
extern int g_border_special_refresh_needed;
extern int g_status_refresh_needed;
extern int g_lores_colors[];
extern int g_a2vid_palette;
extern int g_installed_full_superhires_colormap;
extern char *g_status_ptrs[MAX_STATUS_LINES];
extern word32 g_a2_screen_buffer_changed;
extern word32 g_full_refresh_needed;
extern word32 g_palette_8to1624[256];
extern word32 g_a2palette_8to1624[256];
extern Kimage g_mainwin_kimage;
extern const char g_gsplus_version_str[];       // version string for title bar

SDL_Window *window;                    // Declare a pointer
SDL_Renderer *renderer;
SDL_Texture *texture;
SDL_Texture *overlay_texture; // This is used for scanline simulation. Could be more in future (HUD).
Uint32 *overlay_pixels;

static char *g_clipboard = NULL;  // clipboard variables
static size_t g_clipboard_pos = 0;

void dev_video_init_sdl();
void handle_sdl_key_event(SDL_Event event);
void check_input_events_sdl();
void handle_sdl_mouse_event(SDL_Event event);

int g_num_a2_keycodes = 0;
int a2_key_to_sdlkeycode[][3] = {
  { kVK_Escape,             SDLK_ESCAPE,    0 },
  { kVK_F1,                 SDLK_F1,        0 },
  { kVK_F2,                 SDLK_F2,        0 },
  { kVK_F3,                 SDLK_F3,        0 },
  { kVK_F4,                 SDLK_F4,        0 },
  { kVK_F5,                 SDLK_F5,        0 },
  { kVK_F6,                 SDLK_F6,        0 },
  { kVK_F7,                 SDLK_F7,        0 },
  { kVK_F8,                 SDLK_F8,        0 },
  { kVK_F9,                 SDLK_F9,        0 },
  { kVK_F10,                SDLK_F10,       0 },
  { kVK_F11,                SDLK_F11,       0 },
  { kVK_F12,                SDLK_F12,       0 },
  { kVK_F13,                SDLK_F13,       0 },
  { kVK_F14,                SDLK_F14,       0 },
  { kVK_F15,                SDLK_F15,       0 },
  { kVK_F16,                SDLK_F16,       0 },
  { kVK_F17,                SDLK_F17,       0 },
  { kVK_F18,                SDLK_F18,       0 },
  { kVK_F19,                SDLK_F19,       0 },
  { kVK_F20,                SDLK_F20,       0 },
  { kVK_Reset,              SDLK_PAUSE,     0 },
  { kVK_ANSI_Grave,         SDLK_BACKQUOTE, '~' },
  { kVK_ANSI_1,             SDLK_1,         '!' },
  { kVK_ANSI_2,             SDLK_2,         '@' },
  { kVK_ANSI_3,             SDLK_3,         '#' },
  { kVK_ANSI_4,             SDLK_4,         '$' },
  { kVK_ANSI_5,             SDLK_5,         '%' },
  { kVK_ANSI_6,             SDLK_6,         '^' },
  { kVK_ANSI_7,             SDLK_7,         '&' },
  { kVK_ANSI_8,             SDLK_8,         '*' },
  { kVK_ANSI_9,             SDLK_9,         '(' },
  { kVK_ANSI_0,             SDLK_0,         ')' },
  { kVK_ANSI_Minus,         SDLK_MINUS, SDLK_UNDERSCORE },
  { kVK_ANSI_Equal,         SDLK_EQUALS, SDLK_PLUS },
  { kVK_Delete,             SDLK_BACKSPACE, 0 },
  { kVK_Help,               SDLK_INSERT,    0 },       /* Help? XK_Help  */
  { kVK_Home,               SDLK_HOME,      0 },
  { kVK_PageUp,             SDLK_PAGEUP,    0 },
  { kVK_Tab,                SDLK_TAB,       0 },
  { kVK_ANSI_Q,             SDLK_q,         'Q' },
  { kVK_ANSI_W,             SDLK_w,         'W' },
  { kVK_ANSI_E,             SDLK_e,         'E' },
  { kVK_ANSI_R,             SDLK_r,         'R' },
  { kVK_ANSI_T,             SDLK_t,         'T' },
  { kVK_ANSI_Y,             SDLK_y,         'Y' },
  { kVK_ANSI_U,             SDLK_u,         'U' },
  { kVK_ANSI_I,             SDLK_i,         'I' },
  { kVK_ANSI_O,             SDLK_o,         'O' },
  { kVK_ANSI_P,             SDLK_p,         'P' },
  { kVK_ANSI_LeftBracket,   SDLK_LEFTBRACKET, '{' },
  { kVK_ANSI_RightBracket,  SDLK_RIGHTBRACKET, '}' },
  { kVK_ANSI_Backslash,     SDLK_BACKSLASH, '|' },    /* backslash, bar */
  { kVK_ForwardDelete,      SDLK_DELETE,    0 },
  { kVK_End,                SDLK_END,       0 },
  { kVK_PageDown,           SDLK_PAGEDOWN,  0 },
  { kVK_ANSI_A,             SDLK_a,         'A' },
  { kVK_ANSI_S,             SDLK_s,         'S' },
  { kVK_ANSI_D,             SDLK_d,         'D' },
  { kVK_ANSI_F,             SDLK_f,         'F' },
  { kVK_ANSI_G,             SDLK_g,         'G' },
  { kVK_ANSI_H,             SDLK_h,         'H' },
  { kVK_ANSI_J,             SDLK_j,         'J' },
  { kVK_ANSI_K,             SDLK_k,         'K' },
  { kVK_ANSI_L,             SDLK_l,         'L' },
  { kVK_ANSI_Semicolon,     SDLK_SEMICOLON, SDLK_COLON },
  { kVK_ANSI_Quote,         SDLK_QUOTE,     SDLK_QUOTEDBL },
  { kVK_Return,             SDLK_RETURN,    0 },
  { kVK_Shift,              SDLK_LSHIFT,    0 },
  { kVK_RightShift,         SDLK_RSHIFT,    0 },
  { kVK_ANSI_Z,             SDLK_z,         'Z' },
  { kVK_ANSI_X,             SDLK_x,         'X' },
  { kVK_ANSI_C,             SDLK_c,         'C' },
  { kVK_ANSI_V,             SDLK_v,         'V' },
  { kVK_ANSI_B,             SDLK_b,         'B' },
  { kVK_ANSI_N,             SDLK_n,         'N' },
  { kVK_ANSI_M,             SDLK_m,         'M' },
  { kVK_ANSI_Comma,         SDLK_COMMA,     SDLK_LESS },
  { kVK_ANSI_Period,        SDLK_PERIOD,    SDLK_GREATER },
  { kVK_ANSI_Slash,         SDLK_SLASH,     SDLK_QUESTION },

  { kVK_CapsLock,           SDLK_CAPSLOCK,  0 },

  { kVK_Control,            SDLK_LCTRL,     0 },
  { kVK_RightControl,       SDLK_RCTRL,     0 },
  #if defined(__APPLE__)
  { kVK_Option,             SDLK_LALT,      0 },         /* Command */
  { kVK_RightOption,        SDLK_RALT,      0 },         /* Command */
  { kVK_Command,            SDLK_LGUI,      0 },         /* Option */
  { kVK_RightCommand,       SDLK_RGUI,      0 },         /* Option */
  #else
  { kVK_Option,             SDLK_LGUI,      0 },         /* Option */
  { kVK_RightOption,        SDLK_RGUI,      0 },         /* Option */
  { kVK_Command,            SDLK_LALT,      0 },         /* Command */
  { kVK_RightCommand,       SDLK_RALT,      0 },         /* Command */
  #endif
  { kVK_Space,              SDLK_SPACE,     0 },
  { kVK_LeftArrow,          SDLK_LEFT,      0 },
  { kVK_DownArrow,          SDLK_DOWN,      0 },
  { kVK_RightArrow,         SDLK_RIGHT,     0 },
  { kVK_UpArrow,            SDLK_UP,        0 },

  { kVK_ANSI_Keypad0,       SDLK_KP_0,      0 },
  { kVK_ANSI_Keypad1,       SDLK_KP_1,      0 },
  { kVK_ANSI_Keypad2,       SDLK_KP_2,      0 },
  { kVK_ANSI_Keypad3,       SDLK_KP_3,      0 },
  { kVK_ANSI_Keypad4,       SDLK_KP_4,      0 },
  { kVK_ANSI_Keypad5,       SDLK_KP_5,      0 },
  { kVK_ANSI_Keypad6,       SDLK_KP_6,      0 },
  { kVK_ANSI_Keypad7,       SDLK_KP_7,      0 },
  { kVK_ANSI_Keypad8,       SDLK_KP_8,      0 },
  { kVK_ANSI_Keypad9,       SDLK_KP_9,      0 },

  { kVK_ANSI_KeypadMinus,   SDLK_KP_MINUS,  0 },
  { kVK_ANSI_KeypadPlus,    SDLK_KP_PLUS,   0 },
  { kVK_ANSI_KeypadEquals,  SDLK_KP_EQUALS, 0 }, /* Note XK_Home alias! XK_Home */
  { kVK_ANSI_KeypadDivide,  SDLK_KP_DIVIDE, 0 },
  { kVK_ANSI_KeypadMultiply,SDLK_KP_MULTIPLY, 0 },
  { kVK_ANSI_KeypadDecimal, SDLK_KP_PERIOD, 0 },
  { kVK_ANSI_KeypadEnter,   SDLK_KP_ENTER,  0 },
  { kVK_ANSI_KeypadClear,   SDLK_NUMLOCKCLEAR, 0 }, /* Clear, XK_Clear */

  { -1, -1, -1 }
};



int main(int argc, char **argv) {
  return gsplusmain(argc, argv);
}


const char *byte_to_binary(int x) {
  static char b[9];
  b[0] = '\0';

  int z;
  for (z = 128; z > 0; z >>= 1)
  {
    strcat(b, ((x & z) == z) ? "1" : "0");
  }

  return b;
}


// Queries the Screen to see if set to Fullscreen or Not
// @return SDL_FALSE when windowed, SDL_TRUE when fullscreen
SDL_bool IsFullScreen(SDL_Window *win) {
  Uint32 flags = SDL_GetWindowFlags(win);
  if (flags & SDL_WINDOW_FULLSCREEN) {
    return SDL_TRUE; // return SDL_TRUE if fullscreen
  }
  return SDL_FALSE; // Return SDL_FALSE if windowed
}



void dev_video_init() {
  word32 lores_col;

  // build keycode map ??
  g_num_a2_keycodes = 0;
  int i;
  int keycode;

  for(i = 0; i < 0x7f; i++) {
    keycode = a2_key_to_sdlkeycode[i][0];
    if(keycode < 0) {
      g_num_a2_keycodes = i;
      break;
    } else if(keycode > 0x7f) {
      glogf("a2_key_to_xsym[%d] = %02x!\n", i, keycode);
      exit(2);
    }
  }

  // This actually creates our window
  dev_video_init_sdl();

  // @todo DANGER DANGER.  HARD CODING THESE.. there was logic for stepping values in xdriver
  g_screen_depth = 24;
  g_screen_mdepth =32;
  video_get_kimages();
  video_get_kimage(&g_mainwin_kimage, 0, g_screen_depth,g_screen_mdepth);

  for(i = 0; i < 256; i++) {
    //g_xcolor_a2vid_array[i].pixel = i;
    lores_col = g_lores_colors[i & 0xf];
    video_update_color_raw(i, lores_col);
    g_a2palette_8to1624[i] = g_palette_8to1624[i];
  }

}


void do_icon() {
  #ifdef HAVE_ICON
  //surface = SDL_CreateRGBSurfaceFrom(pixels,w,h,depth,pitch,rmask,gmask,bmask,amask);
  int size = 256;           // icon size
  SDL_Surface *surface;     // declare an SDL_Surface to be filled in with pixel data from an image file
  surface = SDL_CreateRGBSurfaceFrom(icon_pixels,size,size,32,size*4,0xff000000,0x00ff0000,0x0000ff00,0x000000ff);

  // The icon is attached to the window pointer
  SDL_SetWindowIcon(window, surface);
  // ...and the surface containing the icon pixel data is no longer required.
  SDL_FreeSurface(surface);
  #endif
}


// Initialize our SDL window and texture
void dev_video_init_sdl() {
  SDL_Init(SDL_INIT_VIDEO);              // Initialize SDL2

  #if defined __APPLE__
  extern void fix_mac_menu();
  fix_mac_menu();
  #endif

  // Create an application window with the following settings:
  char window_title[32];
  sprintf(window_title, "GSplus v%-6s", g_gsplus_version_str);
  int startx = SDL_WINDOWPOS_UNDEFINED;
  int starty = SDL_WINDOWPOS_UNDEFINED;
  if (g_startx != WINDOWPOS_UNDEFINED) { startx = g_startx; }
  if (g_starty != WINDOWPOS_UNDEFINED) { starty = g_starty; }
  int more_flags = 0;
  // check for CLI fullscreen
  if (g_fullscreen) {
    more_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
  }
  if (g_highdpi) {
    more_flags |= SDL_WINDOW_ALLOW_HIGHDPI;
  }
  if (g_borderless) {
    more_flags |= SDL_WINDOW_BORDERLESS;
  }
  if (g_resizeable) {
    more_flags |= SDL_WINDOW_RESIZABLE;
  }

  window = SDL_CreateWindow(
    window_title,                   // window title (GSport vX.X)
    startx,
    starty,
    g_startw,                       // width, in pixels
    g_starth,                       // height, in pixels
    SDL_WINDOW_OPENGL               // flags - see below
    | more_flags
    );


  // Check that the window was successfully created
  if (window == NULL) {
    // In the case that the window could not be made...
    glogf("Could not create window: %s", SDL_GetError());
    //@todo die, i guess
  } else {
    glog("SDL2 graphics initialized");
  }

  // SET WINDOW ICON
  do_icon();

  int renderer_hints = 0;
  if (!g_novsync) {
    renderer_hints |= SDL_RENDERER_PRESENTVSYNC;
  }
  if (!g_nohwaccel) {
    renderer_hints |= SDL_RENDERER_ACCELERATED;
  }
  renderer = SDL_CreateRenderer(window, -1, renderer_hints);
  debuginfo_renderer(renderer);

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");  // make the scaled rendering look smoother.
  // SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");  // make the scaled rendering look smoother.
  if (!g_noaspect) {
    SDL_RenderSetLogicalSize(renderer, BASE_WINDOW_WIDTH, X_A2_WINDOW_HEIGHT);
  }

  texture = SDL_CreateTexture(renderer,
                              SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING,
                              BASE_WINDOW_WIDTH, X_A2_WINDOW_HEIGHT);
  // The window is open: could enter program loop here (see SDL_PollEvent())
  //overlay test
  overlay_texture = SDL_CreateTexture(renderer,
                                      SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      BASE_WINDOW_WIDTH,
                                      X_A2_WINDOW_HEIGHT);

  SDL_SetTextureBlendMode(overlay_texture, SDL_BLENDMODE_BLEND);
  overlay_pixels = malloc(BASE_WINDOW_WIDTH*X_A2_WINDOW_HEIGHT*sizeof(Uint32));
  Uint32 pixelARGB = 0x33000000;  // default "low grey"
  if (overlay_pixels) {
    if (g_scanline_simulator > 0) {
      pixelARGB = (int)(g_scanline_simulator*2.56) << 24;
    }
    for (int y=0; y<X_A2_WINDOW_HEIGHT; y++) {
      for (int x=0; x<BASE_WINDOW_WIDTH; x++) {

        if (y%2 == 1) {
          overlay_pixels[(y*BASE_WINDOW_WIDTH)+x] = pixelARGB;
        }
      }
    }
  }
  SDL_Rect dstrect;
  dstrect.x = 0;
  dstrect.y = 0;
  dstrect.w = BASE_WINDOW_WIDTH;
  dstrect.h = X_A2_WINDOW_HEIGHT;
  int pitch = BASE_WINDOW_WIDTH;


  // UPDATE A RECT OF THE APPLE II SCREEN TEXTURE
  SDL_UpdateTexture(overlay_texture, &dstrect, overlay_pixels, pitch*sizeof(Uint32) );

  // Turn off host mouse cursor
  SDL_ShowCursor(SDL_DISABLE);
}


// Copy a rect to our SDL window
void sdl_push_kimage(Kimage *kimage_ptr, int destx, int desty, int srcx, int srcy, int width, int height) {

  byte *src_ptr;
  int pixel_size = 4;
  src_ptr = kimage_ptr->data_ptr + (srcy * kimage_ptr->width_act + srcx) * pixel_size;

  SDL_Rect dstrect;
  dstrect.x = destx;
  dstrect.y = desty;
  dstrect.w = width;
  dstrect.h = height;
  int pitch = 640;
  if (width < 560) {
    pitch = EFF_BORDER_WIDTH;
    // seems to be the correct value, but would like clarification
    pitch = BORDER_WIDTH+72;
  }
  SDL_UpdateTexture(texture, &dstrect, src_ptr, pitch*4 );

  // We now call the render step seperately in sdl_present_buffer once per frame
  // SDL picks up the buffer and waits for VBLANK to send it
}


void set_refresh_needed() {
  g_a2_screen_buffer_changed = -1;
  g_full_refresh_needed = -1;

  g_border_sides_refresh_needed = 1;
  g_border_special_refresh_needed = 1;
  g_status_refresh_needed = 1;
}


void x_get_kimage(Kimage *kimage_ptr) {
  byte *data;
  int width;
  int height;
  int depth;

  width = kimage_ptr->width_req;
  height = kimage_ptr->height;
  depth = kimage_ptr->depth;
  // this might be too big!!! I had it at depth/3 but it segfaults
  data = malloc(width*height*(depth/4));
  kimage_ptr->data_ptr = data;
}


void check_input_events() {
  check_input_events_sdl();
}


void check_input_events_sdl() {
  SDL_Event event;

  while (SDL_PollEvent(&event)) {
    /* Check all window events (mostly for Fullscreen) */
    if (event.type == SDL_WINDOWEVENT) {
      set_refresh_needed();
    }
    switch( event.type ) {
      case SDL_KEYDOWN:
      case SDL_KEYUP:
        handle_sdl_key_event(event);
        break;
      case SDL_MOUSEMOTION:
      case SDL_MOUSEBUTTONUP:
      case SDL_MOUSEBUTTONDOWN:
        handle_sdl_mouse_event(event);
        break;
      case SDL_QUIT:
        xdriver_end();
        my_exit(1);
        break;
      case SDL_DROPFILE:
      {
        char *file = event.drop.file;
        cfg_inspect_maybe_insert_file(file, 0);
        SDL_free(file);
      }
      break;
      default:
        break;
    }
  }
}


int sdl_keysym_to_a2code(int keysym, int is_up) {
  int i;

  if(keysym == 0) {
    return -1;
  }

  if((keysym == SDLK_LSHIFT) || (keysym == SDLK_RSHIFT)) {
    if(is_up) {
      kb_shift_control_state &= ~ShiftMask;
    } else {
      kb_shift_control_state |= ShiftMask;
    }
  }
  if(keysym == SDLK_CAPSLOCK) {
    if(is_up) {
      kb_shift_control_state &= ~LockMask;
    } else {
      kb_shift_control_state |= LockMask;
    }
  }
  if((keysym == SDLK_LCTRL) || (keysym == SDLK_RCTRL)) {
    if(is_up) {
      kb_shift_control_state &= ~ControlMask;
    } else {
      kb_shift_control_state |= ControlMask;
    }
  }

  /* Look up Apple 2 keycode */
  for(i = g_num_a2_keycodes - 1; i >= 0; i--) {
    if((keysym == a2_key_to_sdlkeycode[i][1]) ||
       (keysym == a2_key_to_sdlkeycode[i][2])) {
      return a2_key_to_sdlkeycode[i][0];
    }
  }

  return -1;
}


void handle_sdl_key_event(SDL_Event event) {
  int state_xor;
  int state = 0;
  int is_up;

  int mod = event.key.keysym.mod;

  // simulate xmask style here
  // @todo: this can probably all be refactored now that X is gone
  //state = state & (ControlMask | LockMask | ShiftMask);

  // when mod key is first press, comes as event, otherwise just a modifier
  if( mod & KMOD_LCTRL  || mod & KMOD_RCTRL ||
      event.type == (SDL_KEYDOWN && (event.key.keysym.sym == SDLK_LCTRL || event.key.keysym.sym == SDLK_RCTRL))) {
    state = state | ControlMask;
  }
  if( (mod & KMOD_LSHIFT)  || (mod & KMOD_RSHIFT) ||
      event.type == (SDL_KEYDOWN && (event.key.keysym.sym == SDLK_LSHIFT || event.key.keysym.sym == SDLK_RSHIFT))) {
    state = state | ShiftMask;
  }
  if( mod & KMOD_CAPS) {
    state = state | LockMask;
  }

  state_xor = kb_shift_control_state ^ state;
  is_up = 0;
  if(state_xor & ControlMask) {
    is_up = ((state & ControlMask) == 0);
    adb_physical_key_update(0x36, is_up);
  }
  if(state_xor & LockMask) {
    is_up = ((state & LockMask) == 0);
    adb_physical_key_update(0x39, is_up);
  }
  if(state_xor & ShiftMask) {
    is_up = ((state & ShiftMask) == 0);
    adb_physical_key_update(0x38, is_up);
  }

  kb_shift_control_state = state;

  is_up = 0;
  int a2code;
  if (event.type == SDL_KEYUP) {
    is_up = 1;
  }
  switch( event.key.keysym.sym ) {
    case SDLK_F11:
      if (kb_shift_control_state & ShiftMask) {  // SHIFT+F11
        if (!is_up) {
          if (g_scanline_simulator) {
            glog("Enable scanline simulator");
            g_scanline_simulator = 0;
          } else {
            glog("Disable scanline simulator");
            g_scanline_simulator = 1;
          }
          set_refresh_needed();   // make sure user sees it right away
        }
      } else {
        if (!is_up) {
          if (!IsFullScreen(window)) {
            glog("Enable fullscreen");
            SDL_SetWindowGrab(window, true);
            SDL_SetRelativeMouseMode(true);

            Uint32 fullmode = g_fullscreen_desktop ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN;
            SDL_SetWindowFullscreen(window, fullmode);
          } else {
            glog("Disable fullscreen");
            SDL_SetWindowFullscreen(window, 0);
            SDL_SetWindowGrab(window, false);
            SDL_SetWindowSize(window, g_startw, g_starth);
            SDL_SetRelativeMouseMode(false);

          }
        }
      }
      break;
    default:
      a2code = sdl_keysym_to_a2code(event.key.keysym.sym, is_up);
      if(a2code >= 0) {
        adb_physical_key_update(a2code, is_up);
      }
      break;
  }
}


void handle_sdl_mouse_event(SDL_Event event) {
  int x, y;

  int scaledmotion = 0;
  if (scaledmotion) {
    x = event.motion.x * A2_WINDOW_WIDTH / g_startw;
    y = event.motion.y * A2_WINDOW_HEIGHT / g_starth;
  } else {
    x = event.motion.x - BASE_MARGIN_LEFT;
    y = event.motion.y - BASE_MARGIN_TOP;
  }

  switch (event.type) {
    case SDL_MOUSEMOTION:
      update_mouse_w_delta(x, y, 0, 0, event.motion.xrel, event.motion.yrel);
      break;
    case SDL_MOUSEBUTTONUP:
      update_mouse_w_delta(x, y, 0, event.motion.state &7, 0, 0);
      break;
    case SDL_MOUSEBUTTONDOWN:
      update_mouse_w_delta(x, y, event.motion.state &7, event.motion.state &7 , 0, 0);
      break;
  }
}


void x_push_kimage(Kimage *kimage_ptr, int destx, int desty, int srcx, int srcy, int width, int height) {
  sdl_push_kimage(kimage_ptr, destx, desty, srcx, srcy, width, height);
}


// called by src/sim65816.c
void x_dialog_create_gsport_conf(const char *str) {
  // Just write the config file already...
  config_write_config_gsplus_file();
}


void x_full_screen(int do_full) {
  if (do_full) {
    SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
  } else {
    SDL_SetWindowFullscreen(window, 0);
    SDL_SetWindowSize(window, BASE_WINDOW_WIDTH, X_A2_WINDOW_HEIGHT);
  }
}


int file_exists(char *fname) {
  if( access( fname, F_OK ) != -1 ) {
    return 1; // file exists
  } else {
    return 0; // file does not exist
  }
}


// This tries to determine the next screenshot name.
// It uses the config name as the basename.
void make_next_screenshot_filename() {
  char filepart[256];
  char filename[256];

  int available_filename = 0;
  while (!available_filename) {
    char *bn = basename(g_config_gsplus_name);
    // get location of '.'
    char *dotptr = strchr(bn, '.');
    int index = dotptr - bn;
    strncpy(filepart, bn, index);
    filepart[index] = '\0'; //terminator
    // handle trailing "/" vs no "/"
    char tchar = g_config_gsplus_screenshot_dir[strlen(g_config_gsplus_screenshot_dir) - 1];
    if (tchar == '/') {
      sprintf(filename, "%s%s%04d.png",g_config_gsplus_screenshot_dir,filepart,screenshot_index);
    } else {
      sprintf(filename, "%s/%s%04d.png",g_config_gsplus_screenshot_dir,filepart,screenshot_index);
    }
    screenshot_index++;
    if (!file_exists(filename)) {
      available_filename = 1;
    }
  }
  strcpy(screenshot_filename, filename);
}



// @todo: some error with writing data direct to png.  output is empty/transparent?
// workaround is this horrible hack of saving the bmp -> load bmp -> save png
void x_take_screenshot() {
  make_next_screenshot_filename();
  glogf("Taking screenshot - %s", screenshot_filename);
  SDL_Surface *sshot = SDL_CreateRGBSurface(0, BASE_WINDOW_WIDTH, X_A2_WINDOW_HEIGHT, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
  SDL_LockSurface(sshot);
  int read = SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, sshot->pixels, sshot->pitch);
  if (read != 0) {
    printf("READPXL FAIL!\n%s\n", SDL_GetError());
  }
  SDL_SaveBMP(sshot, "screenshot.bmp");
  SDL_UnlockSurface(sshot);
  SDL_FreeSurface(sshot);

  SDL_Surface *s = SDL_CreateRGBSurface(0, BASE_WINDOW_WIDTH, X_A2_WINDOW_HEIGHT, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
  if (s) {
    SDL_Surface * image = SDL_LoadBMP("screenshot.bmp");
    IMG_SavePNG(image, screenshot_filename);
    SDL_FreeSurface(image);
  }
  SDL_FreeSurface(s);
}


void clipboard_paste(void) {

  char *cp;

  if (g_clipboard) {
    free(g_clipboard);
    g_clipboard = NULL;
    g_clipboard_pos = 0;
  }

  cp =  SDL_GetClipboardText();
  if (!cp) return;

  g_clipboard = strdup(cp);
  g_clipboard_pos = 0;

  SDL_free(cp);
}


int clipboard_get_char(void) {
  char c;

  if (!g_clipboard)
    return 0;

  /* skip utf-8 characters. */
  do {
    c = g_clipboard[g_clipboard_pos++];
  } while (c & 0x80);

  /* windows -- skip the \n in \r\n. */
  if (c == '\r' && g_clipboard[g_clipboard_pos] == '\n')
    g_clipboard_pos++;

  /* everybody else -- convert \n to \r */
  if (c == '\n') c = '\r';

  if (c == 0) {
    free(g_clipboard);
    g_clipboard = NULL;
    g_clipboard_pos = 0;
    return 0;
  }

  return c | 0x80;
}

int x_show_alert(int is_fatal, const char *str) {
  if (str) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "GS+ Alert", str, NULL);
  }
  return 0;
}

void xdriver_end() {
  SDL_DestroyWindow(window);
  iwm_shut();
  // Clean up
  SDL_Quit();
}

// This will help us determine how well and which drivers are supported on
// different SDL platforms
void debuginfo_renderer(SDL_Renderer *r) {
  int n = SDL_GetNumRenderDrivers();
  glogf("**--- SDL DEBUG ------ (%i) drivers", n);
  for(int i = 0; i < n; i++) {
      SDL_RendererInfo info;
      SDL_GetRenderDriverInfo(i, &info);
      glogf("*   '%s'", info.name);
  }


  SDL_RendererInfo info = {0};
  if (SDL_GetRendererInfo(r,&info) == 0) {
    glogf("* SDL_RENDERER_SOFTWARE: %d", (info.flags & SDL_RENDERER_SOFTWARE) > 0   );
    glogf("* SDL_RENDERER_ACCELERATED: %d", (info.flags & SDL_RENDERER_ACCELERATED) > 0  );
    glogf("* SDL_RENDERER_PRESENTVSYNC: %d", (info.flags & SDL_RENDERER_PRESENTVSYNC) > 0   );
    glogf("* SDL_RENDERER_TARGETTEXTURE: %d", (info.flags & SDL_RENDERER_TARGETTEXTURE) > 0  );
    glogf("* active renderer -> '%s'", info.name);
  } else {
    glog("NO Renderinfo");
  }
}

// as this is triggered when new images were pushed to backing buffer,
// this is when we want to update frames.
// putting it in x_push_done means we can skip frames that weren't changed.
// the emulator will still run at whatever specified speed.  but this way,
// when running in faster 8mhz/unlimited modes, it won't be slowed down by
// forcing every draw at 60FPS sync.
void x_push_done() {
  void sdl_present_buffer();
  sdl_present_buffer();
}

void sdl_present_buffer() {
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, NULL, NULL);
  if (g_scanline_simulator) {
    SDL_RenderCopy(renderer, overlay_texture, NULL, NULL);
  }

  SDL_RenderPresent(renderer);
  if (g_screenshot_requested) {
    x_take_screenshot();
    g_screenshot_requested = 0;
  }
}

void x_grabmouse() {
  SDL_SetWindowGrab(window, g_grabmouse);
  SDL_SetRelativeMouseMode(g_grabmouse);
}

// BELOW ARE FUNCTIONS THAT ARE EITHER UNIMPLEMENTED, OR AR NOT RELEVANT TO
// THIS DRIVER.

// called by src/sim65816.c
void get_ximage(Kimage *kimage_ptr) { }
void x_toggle_status_lines() { }
void x_redraw_status_lines() { }
void x_hide_pointer(int do_hide) { }
void x_auto_repeat_on(int must) { }
void x_auto_repeat_off(int must) { }
void x_release_kimage(Kimage* kimage_ptr) { }
int x_calc_ratio(float x,float y) { return 1; }
void x_set_mask_and_shift(word32 x_mask, word32 *mask_ptr, int *shift_left_ptr, int *shift_right_ptr) { return; }
void x_update_color(int col_num, int red, int green, int blue, word32 rgb) { }
void x_update_physical_colormap() { }
void show_xcolor_array() { }
