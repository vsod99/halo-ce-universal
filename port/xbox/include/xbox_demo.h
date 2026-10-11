/*
XBOX_DEMO.H

A demo of recorded play, to time and profile builds by (port/xbox/src/
xbox_demo.c).
*/

#ifndef XBOX_DEMO_H
#define XBOX_DEMO_H

/* whether frames run a fixed time: a demo records or plays (main.c) */
int xbox_demo_fixed_frames(void);
/* each frame, before the user interface: starts the level a demo plays */
void xbox_demo_frame(void);
/* a core save just made or loaded (main.c): a recording loads its save at
once and keeps the random seeds, which the save leaves out; the demo played
takes them back */
void xbox_demo_core_saved(void);
void xbox_demo_core_loaded(void);
/* the milliseconds a held button is timed by (input_xbox.c,
input_abstraction.c): the system's, or while a demo records or plays its
fixed frames' time, which comes out alike every time */
long xbox_demo_milliseconds(void);

#ifdef XINPUT_GAMEPAD_BACK
/* the first controller as read (XInputGetState): kept, or replaced by the
demo's; Back with both sticks clicked starts or ends a recording */
void xbox_demo_gamepad(XINPUT_GAMEPAD *pad);
#endif

#endif
