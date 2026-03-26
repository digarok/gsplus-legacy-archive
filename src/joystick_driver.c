/*
   GSPLUS - Advanced Apple IIGS Emulator Environment
   Based on the KEGS emulator written by Kent Dickey
   See COPYRIGHT.txt for Copyright information
   See LICENSE.txt for license (GPL v2)
 */

#include "defc.h"
#include "glog.h"

#ifdef __linux__
# include <linux/joystick.h>
# include <sys/time.h>
#endif

#ifdef _WIN32
# include <windows.h>
# include <mmsystem.h>
# include <time.h>
#endif

#ifdef HAVE_SDL
# include "SDL.h"
//@todo: multiple joysticks/more buttons/button config
//static SDL_GameController *joy0, *joy1;
SDL_GameController *gGameController = NULL;
#endif



extern int g_joystick_native_type1;             /* in paddles.c */
extern int g_joystick_native_type2;             /* in paddles.c */
extern int g_joystick_native_type;              /* in paddles.c */
extern int g_joystick_type;
extern int g_paddle_buttons;
extern int g_paddle_val[];


const char *g_joystick_dev = "/dev/input/js0";  /* default joystick dev file */
#define MAX_JOY_NAME    128

int g_joystick_native_fd = -1;
int g_joystick_num_axes = 0;
int g_joystick_num_buttons = 0;
int g_joystick_number = 0;          // SDL2
int g_joystick_x_axis = 0;          // SDL_CONTROLLER_AXIS_LEFTX
int g_joystick_y_axis = 1;          // SDL_CONTROLLER_AXIS_LEFTY
int g_joystick_button_0 = 0;        // SDL_CONTROLLER_BUTTON_A
int g_joystick_button_1 = 1;        // SDL_CONTROLLER_BUTTON_B

int g_joystick_x2_axis = 2;         // SDL_CONTROLLER_AXIS_RIGHTX
int g_joystick_y2_axis = 3;         // SDL_CONTROLLER_AXIS_RIGHTY
int g_joystick_button_2 = 2;        // SDL_CONTROLLER_BUTTON_X
int g_joystick_button_3 = 3;        // SDL_CONTROLLER_BUTTON_Y
#define JOY2SUPPORT


#if defined(HAVE_SDL) && !defined(JOYSTICK_DEFINED)
# define JOYSTICK_DEFINED
// Gamepad
void joystick_init()      {
  int i;
  if( SDL_Init( SDL_INIT_GAMECONTROLLER ) < 0 ) {
    glogf( "SDL could not initialize gamepad! SDL Error: %s", SDL_GetError() );
  } else {
    glog("SDL2 gamepad initialized");
  }
  if (SDL_NumJoysticks()<1) {
    glog("No gamepads detected");
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
  } else {
    gGameController = SDL_GameControllerOpen( g_joystick_number );
    if( gGameController == NULL ) {
      glogf( "Warning: Unable to open gamepad! SDL Error: %s", SDL_GetError() );
    }
  }
  g_joystick_native_type = 2;
  g_joystick_native_type1 = 2;
  g_joystick_native_type2 = -1;
  for(i = 0; i < 4; i++) {
    g_paddle_val[i] = 180;
  }
  g_joystick_type = JOYSTICK_TYPE_NATIVE_1;
  SDL_GameControllerUpdate();
  joystick_update(0.0);
}

void joystick_update(double dcycs)      {
  if (gGameController) {
    SDL_GameControllerUpdate();
    g_paddle_val[0] = (int)SDL_GameControllerGetAxis(gGameController, g_joystick_x_axis);   // default is 0
    g_paddle_val[1] = (int)SDL_GameControllerGetAxis(gGameController, g_joystick_y_axis);   // default is 1
    g_paddle_val[2] = (int)SDL_GameControllerGetAxis(gGameController, g_joystick_x2_axis);  // default is 2
    g_paddle_val[3] = (int)SDL_GameControllerGetAxis(gGameController, g_joystick_y2_axis);  // default is 3

    // D-pad will override thumb stick values.
    if (SDL_GameControllerGetButton(gGameController, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) {
      g_paddle_val[0] = -32768;
    }
    if (SDL_GameControllerGetButton(gGameController, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
      g_paddle_val[0] = 32767;
    }
    if (SDL_GameControllerGetButton(gGameController, SDL_CONTROLLER_BUTTON_DPAD_UP)) {
      g_paddle_val[1] = -32768;
    }
    if (SDL_GameControllerGetButton(gGameController, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) {
      g_paddle_val[1] = 32767;
    }

    // These are the face buttons (xbox:A,B,X,Y ps:x,o,square,triangle)
    if (SDL_GameControllerGetButton(gGameController, g_joystick_button_0)) {
      g_paddle_buttons = g_paddle_buttons | 1;
    } else {
      g_paddle_buttons = g_paddle_buttons & (~1);
    }
    if (SDL_GameControllerGetButton(gGameController, g_joystick_button_1)) {
      g_paddle_buttons = g_paddle_buttons | 2;
    } else {
      g_paddle_buttons = g_paddle_buttons & (~2);
    }
    if (SDL_GameControllerGetButton(gGameController, g_joystick_button_2)) {
      g_paddle_buttons = g_paddle_buttons | 4;
    } else {
      g_paddle_buttons = g_paddle_buttons & (~4);
    }
    if (SDL_GameControllerGetButton(gGameController, g_joystick_button_3)) {
      g_paddle_buttons = g_paddle_buttons | 8;
    } else {
      g_paddle_buttons = g_paddle_buttons & (~8);
    }

    // The trigger/shoulder buttons are mapped to help right thumb stick usage.
    // Otherwise someone would need to remove the thumb to press a face button.  All these
    // logically OR with the face buttons.  The face buttons control if a button is released.

    // Note that the intent of these changes (shoulder/trigger/right stick on a D-pad) are to
    // enable novel experiences that historical games did not support.  For example, Robotron-type
    // games could have used two joysticks but didn't.  A new game could take advantage of this.

    // It might be better for historical games usage for the emulator to support two concurrent
    // controllers in the menu system than one "super controller".
    if (SDL_GameControllerGetAxis(gGameController, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384) {
      g_paddle_buttons = g_paddle_buttons | 1;
    }
    if (SDL_GameControllerGetAxis(gGameController, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16384) {
      g_paddle_buttons = g_paddle_buttons | 2;
    }
    if (SDL_GameControllerGetButton(gGameController, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) {
      g_paddle_buttons = g_paddle_buttons | 4;
    }
    if (SDL_GameControllerGetButton(gGameController, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) {
      g_paddle_buttons = g_paddle_buttons | 8;
    }

    paddle_update_trigger_dcycs(dcycs);
  }
}

void joystick_update_buttons()      {
}

void joystick_shut() {
  SDL_GameControllerClose( gGameController );
  gGameController = NULL;
}
#endif





#if defined(__linux__) && !defined(JOYSTICK_DEFINED)
# define JOYSTICK_DEFINED
void joystick_init()      {
  char joy_name[MAX_JOY_NAME];
  int version;
  int fd;
  int i;

  fd = open(g_joystick_dev, O_RDONLY | O_NONBLOCK);
  if(fd < 0) {
    printf("Unable to open joystick dev file: %s, errno: %d\n",
           g_joystick_dev, errno);
    printf("Defaulting to mouse joystick\n");
    return;
  }

  strcpy(&joy_name[0], "Unknown Joystick");
  version = 0x800;

  ioctl(fd, JSIOCGNAME(MAX_JOY_NAME), &joy_name[0]);
  ioctl(fd, JSIOCGAXES, &g_joystick_num_axes);
  ioctl(fd, JSIOCGBUTTONS, &g_joystick_num_buttons);
  ioctl(fd, JSIOCGVERSION, &version);

  printf("Detected joystick: %s [%d axes, %d buttons vers: %08x]\n",
         joy_name, g_joystick_num_axes, g_joystick_num_buttons,
         version);

  g_joystick_native_type1 = 1;
  g_joystick_native_type2 = -1;
  g_joystick_native_fd = fd;
  for(i = 0; i < 4; i++) {
    g_paddle_val[i] = 32767;
  }
  g_paddle_buttons = 0xc;

  joystick_update(0.0);
}

/* joystick_update_linux() called from paddles.c.  Update g_paddle_val[] */
/*  and g_paddle_buttons with current information */
void joystick_update(double dcycs)      {
  struct js_event js;           /* the linux joystick event record */
  int mask;
  int val;
  int num;
  int type;
  int ret;
  int len;
  int i;

  /* suck up to 20 events, then give up */
  len = sizeof(struct js_event);
  for(i = 0; i < 20; i++) {
    ret = read(g_joystick_native_fd, &js, len);
    if(ret != len) {
      /* just get out */
      break;
    }
    type = js.type & ~JS_EVENT_INIT;
    val = js.value;
    num = js.number & 3;                        /* clamp to 0-3 */
    switch(type) {
      case JS_EVENT_BUTTON:
        mask = 1 << num;
        if(val) {
          val = mask;
        }
        g_paddle_buttons = (g_paddle_buttons & ~mask) | val;
        break;
      case JS_EVENT_AXIS:
        /* val is -32767 to +32767 */
        g_paddle_val[num] = val;
        break;
    }
  }

//	if(i > 0) {
//	Note from Dave Schmenk: paddle_update_trigger_dcycles(dcycs) always has to be called to keep the triggers current.
  paddle_update_trigger_dcycs(dcycs);
//	}
}

void joystick_update_buttons()      {
}
#endif /* LINUX */

#if defined(_WIN32) && !defined(JOYSTICK_DEFINED)
# define JOYSTICK_DEFINED
void joystick_init()      {
  JOYINFO info;
  JOYCAPS joycap;
  MMRESULT ret1, ret2;
  int i;

  //	Check that there is a joystick device
  if(joyGetNumDevs() <= 0) {
    glog("No joystick hardware detected");
    g_joystick_native_type1 = -1;
    g_joystick_native_type2 = -1;
    return;
  }

  g_joystick_native_type1 = -1;
  g_joystick_native_type2 = -1;

  //	Check that at least joystick 1 or joystick 2 is available
  ret1 = joyGetPos(JOYSTICKID1, &info);
  ret2 = joyGetDevCaps(JOYSTICKID1, &joycap, sizeof(joycap));
  if(ret1 == JOYERR_NOERROR && ret2 == JOYERR_NOERROR) {
    g_joystick_native_type1 = JOYSTICKID1;
    printf("Joystick #1 = %s\n", joycap.szPname);
    g_joystick_native_type = JOYSTICKID1;
  }
  ret1 = joyGetPos(JOYSTICKID2, &info);
  ret2 = joyGetDevCaps(JOYSTICKID2, &joycap, sizeof(joycap));
  if(ret1 == JOYERR_NOERROR && ret2 == JOYERR_NOERROR) {
    g_joystick_native_type2 = JOYSTICKID2;
    printf("Joystick #2 = %s\n", joycap.szPname);
    if(g_joystick_native_type < 0) {
      g_joystick_native_type = JOYSTICKID2;
    }
  }

  if (g_joystick_native_type1<0 && g_joystick_native_type2 <0) {
    glog("No joystick is attached");
    return;
  }

  for(i = 0; i < 4; i++) {
    g_paddle_val[i] = 32767;
  }
  g_paddle_buttons = 0xc;

  joystick_update(0.0);
}

void joystick_update(double dcycs)      {
  JOYCAPS joycap;
  JOYINFO info;
  UINT id;
  MMRESULT ret1, ret2;

  id = g_joystick_native_type;

  ret1 = joyGetDevCaps(id, &joycap, sizeof(joycap));
  ret2 = joyGetPos(id, &info);
  if(ret1 == JOYERR_NOERROR && ret2 == JOYERR_NOERROR) {
    g_paddle_val[0] = (info.wXpos - joycap.wXmin) * 32768 /
                      (joycap.wXmax - joycap.wXmin);
    g_paddle_val[1] = (info.wYpos - joycap.wYmin) * 32768 /
                      (joycap.wYmax - joycap.wYmin);
    if(info.wButtons & JOY_BUTTON1) {
      g_paddle_buttons = g_paddle_buttons | 1;
    } else {
      g_paddle_buttons = g_paddle_buttons & (~1);
    }
    if(info.wButtons & JOY_BUTTON2) {
      g_paddle_buttons = g_paddle_buttons | 2;
    } else {
      g_paddle_buttons = g_paddle_buttons & (~2);
    }
    paddle_update_trigger_dcycs(dcycs);
  }
}

void joystick_update_buttons()      {
  JOYINFOEX info;
  UINT id;

  id = g_joystick_native_type;

  info.dwSize = sizeof(JOYINFOEX);
  info.dwFlags = JOY_RETURNBUTTONS;
  if(joyGetPosEx(id, &info) == JOYERR_NOERROR) {
    if(info.dwButtons & JOY_BUTTON1) {
      g_paddle_buttons = g_paddle_buttons | 1;
    } else {
      g_paddle_buttons = g_paddle_buttons & (~1);
    }
    if(info.dwButtons & JOY_BUTTON2) {
      g_paddle_buttons = g_paddle_buttons | 2;
    } else {
      g_paddle_buttons = g_paddle_buttons & (~2);
    }
  }
}
#endif





#ifndef JOYSTICK_DEFINED
/* stubs for the routines */
void joystick_init()      {
  g_joystick_native_type1 = -1;
  g_joystick_native_type2 = -1;
  g_joystick_native_type = -1;
}

void joystick_update(double dcycs)      {
  int i;

  for(i = 0; i < 4; i++) {
    g_paddle_val[i] = 32767;
  }
  g_paddle_buttons = 0xc;
}

void joystick_update_buttons()      {
}

void joystick_shut() {
}
#endif
