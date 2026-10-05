/*
XBOX_PLATFORM.C

The desktop ports' hooks the game calls (port/linux/src/sdl_platform.c has
them there), as the Xbox answers them, and what it leaves out or does not
have yet:

- the high-res HUD and text (port/linux/src/hud_hires.c, text_hires.c),
  which the Xbox leaves out (port/xbox/README.md): no textures, no fonts;
- internet play (port/linux/src/p2p*.c), the plan's fourth phase: off, so
  the game plays on the LAN alone (p2p_sanitize.c is shared), and the
  Winsock layer (port/linux/src/xnet.c) finds no peers; the identifier an
  XNADDR carries is the console's Ethernet address, as a retail Xbox's is.
*/

#include "platform.h"
#include "halo_ui_pointer.h"
#include "hud_hires.h"
#include "p2p.h"
#include "nxdk_platform.h"
#include "text_hires.h"

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

/* the scoreboard's mouse wheel and page keys */
void platform_scoreboard_scroll(int open, long *notches, long *pages)
{
	(void)open;
	*notches = 0;
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

/* ---------- internet play: off */

int p2p_join_invite(const char *text)
{
	(void)text;
	return 0;
}

int p2p_peer_address(const unsigned char *identifier, unsigned long *address)
{
	(void)identifier;
	(void)address;
	return 0;
}

unsigned long p2p_peer_endpoint_address(unsigned long virtual_address)
{
	(void)virtual_address;
	return 0;
}

void p2p_set_hosting_allowed(int allowed)
{
	(void)allowed;
}

int p2p_invite_link(char *link, int size)
{
	if (size > 0)
		link[0] = 0;
	return 0;
}

void p2p_set_game_player_counts(int count, int maximum)
{
	(void)count;
	(void)maximum;
}

void p2p_set_hosting_public(int public)
{
	(void)public;
}

void p2p_set_game_listing(const char *name, const char *map, const char *gametype, int engine_type, int open,
	int in_progress, int has_teams)
{
	(void)name;
	(void)map;
	(void)gametype;
	(void)engine_type;
	(void)open;
	(void)in_progress;
	(void)has_teams;
}

void p2p_lobby_browse(int on)
{
	(void)on;
}

void p2p_lobby_refresh(void)
{
}

int p2p_lobby_games(struct p2p_listing *games, int maximum_count)
{
	(void)games;
	(void)maximum_count;
	return 0;
}

void p2p_lobby_mark_failed(const unsigned char *identifier)
{
	(void)identifier;
}

void p2p_discord_identity(char *id, int id_size, char *name, int name_size)
{
	if (id_size > 0)
		id[0] = 0;
	if (name_size > 0)
		name[0] = 0;
}

/* none yet: the plan's is a keyed hash of the EEPROM's serial number and
the MAC address */
void p2p_hardware_id(char *hex, int size)
{
	if (size > 0)
		hex[0] = 0;
}

/* the Winsock layer's (xnet.c): no address is a peer's */

void p2p_initialize(unsigned long local_address)
{
	(void)local_address;
}

const unsigned char *p2p_identifier(void)
{
	return xbox_net_ethernet_address();
}

int p2p_outgoing(int stream, int socket, unsigned long *address, unsigned short *port)
{
	(void)stream;
	(void)socket;
	(void)address;
	(void)port;
	return 0;
}

int p2p_incoming(int stream, unsigned long *address, unsigned short *port)
{
	(void)stream;
	(void)address;
	(void)port;
	return 0;
}

int p2p_broadcast_targets(unsigned short port, unsigned long *addresses, unsigned short *ports, int maximum_count)
{
	(void)port;
	(void)addresses;
	(void)ports;
	(void)maximum_count;
	return 0;
}

int p2p_send_datagram(unsigned short source_port, unsigned long address, unsigned short port, const void *data,
	int size)
{
	(void)source_port;
	(void)address;
	(void)port;
	(void)data;
	(void)size;
	return 0;
}

int p2p_broadcast_datagram(unsigned short source_port, unsigned short port, const void *data, int size)
{
	(void)source_port;
	(void)port;
	(void)data;
	(void)size;
	return 0;
}

void p2p_socket_port(int socket, int stream, int listening, unsigned short port)
{
	(void)socket;
	(void)stream;
	(void)listening;
	(void)port;
}

void p2p_port_taken(int stream, unsigned short port)
{
	(void)stream;
	(void)port;
}

void p2p_socket_closed(int socket, unsigned short datagram_port)
{
	(void)socket;
	(void)datagram_port;
}
