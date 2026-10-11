/*
XBOX_DEMO.H

A demo of recorded play, to time and profile builds by (port/xbox/src/
xbox_demo.c).
*/

#ifndef XBOX_DEMO_H
#define XBOX_DEMO_H

/* whether frames run a fixed time: a demo records or plays (main.c) */
int xbox_demo_fixed_frames(void);
/* each frame, before the user interface: starts a recording once its save
is made, and the level a demo plays */
void xbox_demo_frame(void);

#ifdef XINPUT_GAMEPAD_BACK
/* the first controller as read (XInputGetState): kept, or replaced by the
demo's; Back with both sticks clicked starts or ends a recording */
void xbox_demo_gamepad(XINPUT_GAMEPAD *pad);
#endif

#endif
