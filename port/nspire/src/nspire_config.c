/*
NSPIRE_CONFIG.C

The native ports' settings (port/linux/src/port_config.h) for the Nspire
port, which has no config.toml: fixed values. The data and save folders are
the one the program runs from, and everything a calculator does not have
(sound, network, frames between ticks) is off. Any setting not listed is
false, 0 or empty.
*/

#include "platform.h"
#include "port_config.h"
#include "nspire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct nspire_setting
{
	const char *name;
	const char *value;
};

static const struct nspire_setting settings[] =
{
	{ "display.vsync", "false" },
	{ "display.interpolation", "false" },
	{ "display.direct_camera", "false" },
	{ "audio.enabled", "false" },
	{ "audio.volume", "0.0" },
	{ "network.online", "false" },
	{ "network.join_from_clipboard", "false" },
	{ "network.allow_upnp", "false" },
	{ "update.auto", "false" },
	{ "debug.gpu_trace_frame", "-1" },
};

static const char *setting_text(const char *name)
{
	static char saves[300];
	unsigned long index;

	if (!strcmp(name, "paths.data"))
		return nspire_program_directory();
	/* the game's drives z:, u: and t: become folders; kept out of the way */
	if (!strcmp(name, "paths.saves"))
	{
		if (!saves[0])
			snprintf(saves, sizeof(saves), "%s/halo_saves", nspire_program_directory());
		return saves;
	}
	for (index = 0; index < sizeof(settings) / sizeof(settings[0]); index++)
	{
		if (!strcmp(settings[index].name, name))
			return settings[index].value;
	}
	return "";
}

int config_boolean(const char *name)
{
	const char *text = setting_text(name);

	return !strcmp(text, "true");
}

long config_integer(const char *name)
{
	return strtol(setting_text(name), NULL, 10);
}

double config_real(const char *name)
{
	return strtod(setting_text(name), NULL);
}

const char *config_string(const char *name)
{
	return setting_text(name);
}

int config_write_boolean(const char *name, int value)
{
	(void)name;
	(void)value;
	return 0;
}
