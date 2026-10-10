/*
XBOX_PLATFORM.C

The desktop ports' hooks the game calls (port/linux/src/sdl_platform.c has
them there), as the Xbox answers them, and what it leaves out or does not
have yet:

- the high-res HUD and text (port/linux/src/hud_hires.c, text_hires.c),
  which the Xbox leaves out (port/xbox/README.md): no textures, no fonts;
- voice chat (port/linux/src/voice_audio.c): no microphone, and no voices;
- Halo Custom Edition maps (port/linux/src/xbox_memory.c's tag cache for
  them): none, so the game plays the Xbox's own maps alone.
*/

#include "platform.h"
#include "halo_ui_pointer.h"
#include "hud_hires.h"
#include "nxdk_platform.h"
#include "text_hires.h"
#include "voice_audio.h"

#include <string.h>

/* ---------- messages, the clipboard and the display */

void platform_show_message(const char *title, const char *message)
{
	platform_log("%s: %s", title, message);
}

/* no clipboard: invites will be typed on the on-screen keyboard */
int platform_clipboard_get(char *text, int size)
{
	if (size > 0)
		text[0] = 0;
	return 0;
}

void platform_clipboard_set(const char *text)
{
	(void)text;
}

/* one video mode, which the renderer sets */
void platform_display_apply(void)
{
}

/* the maps come from the Halo disc or the hard disk (port/xbox/README.md):
nothing to copy them out of */
BOOL platform_offer_game_data(const char *destination)
{
	(void)destination;
	return FALSE;
}

/* the scoreboard's mouse wheel and page keys: none (a closing scoreboard
passes no counts: game_engine_scoreboard_closed) */
void platform_scoreboard_scroll(int open, long *notches, long *pages)
{
	(void)open;
	if (notches)
		*notches = 0;
	if (pages)
		*pages = 0;
}

/* a frame per game tick, as the Xbox drew it (the plan: no frames between
ticks) */
int halo_interpolation_enabled(void)
{
	return 0;
}

/* no mouse */
int halo_ui_pointer_update(int menus_active, struct halo_ui_pointer *pointer)
{
	(void)menus_active;
	(void)pointer;
	return 0;
}

int halo_scoreboard_pointer_update(int offered, struct halo_ui_pointer *pointer)
{
	(void)offered;
	(void)pointer;
	return 0;
}

/* the menus' choices of a desktop's displays, windows and sound devices:
none (port/linux/game/menu_tags.c) */
int platform_display_resolutions(long *widths, long *heights, int maximum)
{
	(void)widths;
	(void)heights;
	(void)maximum;
	return 0;
}

int platform_window_sizes(long *widths, long *heights, int maximum)
{
	(void)widths;
	(void)heights;
	(void)maximum;
	return 0;
}

int platform_audio_devices(int recording, char (*names)[128], int maximum)
{
	(void)recording;
	(void)names;
	(void)maximum;
	return 0;
}

/* ---------- Halo Custom Edition maps: none */

void *halo_custom_edition_tag_cache(void)
{
	return NULL;
}

void halo_custom_edition_texels_channels(const void *texels, unsigned char channel_order)
{
	(void)texels;
	(void)channel_order;
}

void halo_custom_edition_texels_forget(void)
{
}

/* ---------- voice chat: no microphone, and no voices */

int halo_push_to_talk_held(void)
{
	return 0;
}

int voice_audio_microphone(int open)
{
	(void)open;
	return 0;
}

int voice_audio_read_frame(float *frame)
{
	(void)frame;
	return 0;
}

float voice_audio_level(const float *frame)
{
	(void)frame;
	return 0.0f;
}

int voice_audio_encode(const float *frame, int bitrate, unsigned char *packet, int maximum)
{
	(void)frame;
	(void)bitrate;
	(void)packet;
	(void)maximum;
	return 0;
}

void voice_audio_play(int speaker, unsigned short sequence, const unsigned char *packet, int length, float gain,
	float pan)
{
	(void)speaker;
	(void)sequence;
	(void)packet;
	(void)length;
	(void)gain;
	(void)pan;
}

void voice_audio_forget(int speaker)
{
	(void)speaker;
}

void voice_audio_forget_all(void)
{
}

int voice_audio_speaking(int speaker)
{
	(void)speaker;
	return 0;
}

void voice_audio_set_volume(float volume)
{
	(void)volume;
}

void voice_audio_mix(float *output, unsigned long frames)
{
	(void)output;
	(void)frames;
}

/* ---------- the high-res HUD and text: left out */

long hud_hires_asset_count(void)
{
	return 0;
}

char const *hud_hires_asset_tag(long asset)
{
	(void)asset;
	return "";
}

long hud_hires_asset_bitmap(long asset)
{
	(void)asset;
	return -1;
}

long hud_hires_asset_fits(long asset, long width, long height)
{
	(void)asset;
	(void)width;
	(void)height;
	return 0;
}

/* (the menus' bitmaps: the renderer's, once it has one) */
unsigned int hud_hires_png_texture(const void *png, unsigned long size, unsigned long *levels)
{
	(void)png;
	(void)size;
	*levels = 0;
	return 0;
}

long text_hires_font(char const *tag_name, float cap_height, float oversample)
{
	(void)tag_name;
	(void)cap_height;
	(void)oversample;
	return -1;
}

int text_hires_covers(long font, unsigned long code)
{
	(void)font;
	(void)code;
	return 0;
}

int text_hires_glyph(long font, unsigned long code, struct text_hires_glyph *glyph)
{
	(void)font;
	(void)code;
	(void)glyph;
	return 0;
}

void text_hires_register_atlas(const unsigned long *texture, unsigned long width, unsigned long height)
{
	(void)texture;
	(void)width;
	(void)height;
}
