/*
P2P_SANITIZE.C

The text kept of the ids a joining machine tells its host (its hardware id
and Discord user: port/linux/game/network_distributed.c), as p2p.h
declares: what every port's host checks a joiner's against, with or without
internet play (the original Xbox has none yet).
*/

#include "p2p.h"

/* the text kept of a Discord user's id or name: of the characters allowed
(the rest left out), no longer than the size (and ended) */
void p2p_discord_sanitize(char *destination, int size, const char *source, int name)
{
	int length = 0;

	for (; source && *source && length < size - 1; source++)
	{
		char character = *source;

		if ((character >= '0' && character <= '9') ||
			(name && ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
				character == '_' || character == '.' || character == '-')))
		{
			destination[length++] = character;
		}
	}
	destination[length] = 0;
}

/* lowercase hex digits only, P2P_HARDWARE_ID_BYTES' worth */
void p2p_hardware_id_sanitize(char *destination, int size, const char *source)
{
	int length = 0;

	for (; source && *source && length < size - 1 && length < 2 * P2P_HARDWARE_ID_BYTES; source++)
	{
		char character = *source >= 'A' && *source <= 'F' ? *source - 'A' + 'a' : *source;

		if ((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))
			destination[length++] = character;
	}
	if (size > 0)
		destination[length] = 0;
}
