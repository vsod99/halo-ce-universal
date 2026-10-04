/*
POSIX_VIDEO.C

The calculator's screen for the Nspire port, through nSDL: a 320x240 RGB565
surface that the software renderer (d3d8_soft.c) draws into directly, text
in nSDL's font for the status line, and the flip to the LCD (nSDL goes
through Ndless's lcd_blit, so the CX II's 240x320 panels work too).

nSDL's headers bring in Ndless's own (its BOOL is not the Xbox SDK's), so
this file is compiled as the posix_*.c are, with the C library's headers
only (tools/nspire_build.py), behind the small interface in nspire.h.
*/

#include <SDL/SDL.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

static SDL_Surface *screen;
static nSDL_Font *font;

static void video_dispose(void)
{
	if (font)
		nSDL_FreeFont(font);
	font = NULL;
	if (screen)
		SDL_Quit();
	screen = NULL;
}

/* the screen back to the OS (for its message boxes, which nSDL's video
mode would hide: the calculator then seemed frozen, waiting for a key);
nspire_video_initialize takes it again */
void nspire_video_release(void)
{
	video_dispose();
}

int nspire_video_initialize(void)
{
	if (screen)
		return 1;
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
		return 0;
	screen = SDL_SetVideoMode(SCREEN_WIDTH, SCREEN_HEIGHT, 16, SDL_SWSURFACE);
	if (!screen || screen->format->BitsPerPixel != 16 || screen->pitch != SCREEN_WIDTH * 2)
	{
		SDL_Quit();
		screen = NULL;
		return 0;
	}
	font = nSDL_LoadFont(NSDL_FONT_THIN, 255, 255, 0);
	atexit(video_dispose);
	return 1;
}

/* the screen's RGB565 pixels, row after row */
unsigned short *nspire_video_pixels(void)
{
	return screen ? (unsigned short *)screen->pixels : NULL;
}

void nspire_video_text(int x, int y, const char *text)
{
	if (screen && font)
		nSDL_DrawString(screen, font, x, y, "%s", text);
}

void nspire_video_present(void)
{
	if (screen)
		SDL_Flip(screen);
}

/* whether this is a CX II (nspire_main.c's clock log): Ndless's is_cx2,
here where its headers are */
int nspire_is_cx2(void)
{
	return is_cx2;
}

