/*
XBOX_LOG.H

Log lines from an Xbox program to the development loop (tools/xbox_dev.py):
COM2 of the debug kits' SuperIO chip, which xemu emulates
(-device lpc47m157), and the screen.
*/

#ifndef __XBOX_LOG_H
#define __XBOX_LOG_H

#include <stdarg.h>

/* the line tools/xbox_dev.py stops a run at, with success */
#define XBOX_LOG_DONE_MARKER "== XBOX DONE =="

/* finds and configures the serial port; returns whether there is one (a
retail console has none: then lines go to the screen only) */
int xbox_log_initialize(int to_screen);
/* lines also go to the screen (debugPrint), once the program has set a
video mode */
void xbox_log_to_screen(int to_screen);
void xbox_log(const char *format, ...);
void xbox_vlog(const char *format, va_list arguments);
/* text as it is (newlines as the serial port's CR LF) */
void xbox_log_write(const char *text);
/* ends the run: prints the done marker */
void xbox_log_done(void);

#endif /* __XBOX_LOG_H */
