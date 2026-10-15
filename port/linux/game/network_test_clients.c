/*
NETWORK_TEST_CLIENTS.C

Test clients inside a network test's host (debug.network_test_clients):
machines that join the host's game in progress over this machine's own
loopback, with the game's messages as a client sends them, so that a host
with many machines can be measured on one (its processor, and what it sends
each machine) without the machines. The host's code is the same as with real
clients: it accepts each, sends it the game's settings and every object,
takes its player's input and sends it the game every tick.

Each client is a stream and a datagram socket of the game's transport
(transport_endpoint_winsock.c), which send and read the game's messages as
a client's connection does (network_connection.c: each message whole, its
header in network byte order), with a buffer of 8 KB where a game's client
connection holds 384 KB of queues (fifteen of those would not fit beside an
Xbox's map). Its sockets are bound to an address of its own, 127.0.0.2 and
on: the host tells machines apart by their addresses, and sends each its
datagrams at its address's client port (xnet.c binds the client's sockets
to the address, and lets the datagram ones share the port with this
machine's own client; the Xbox makes each address an interface of lwIP's,
nxdk_net.c). It asks to join, adds one
player, says it has loaded when the host starts it, asks for the host's
objects, and from then on sends its player's input every tick: walking and
strafing in circles, turning, firing every three seconds, jumping every
five, throwing a grenade every seven (as debug.test_input's bot plays a
controller). It reads everything the host sends it and counts it, but loads
nothing, predicts nothing and reports no hits: the host simulates its player
from the input alone.

Every ten seconds the log has how many are in the game and what the host
sent them, on average each.

Called from network_test_update every frame (network_test.c).
*/

#include "cseries.h"
#include "cseries/cseries_windows.h"
#include "bungie_net/common/message_header.h"
#include "bungie_net/network/transport.h"
#include "bungie_net/network/transport_address_constants.h"
#include "bungie_net/network/transport_endpoint.h"
#include "networking/network_connection.h"
#include "networking/network_messages.h"
#include "networking/network_game_protocol.h"
#include "networking/network_game_globals.h"
#include "networking/network_server_manager.h"
#include "game/game.h"
#include "game/players.h"
#include "game/player_queues_new.h"
#include "scenario/scenario.h"
#include "units/units.h"
#include "network_distributed.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* the platform layer's (port/linux/src) */
long config_integer(char const *name);
void platform_log(char const *format, ...);
void xnet_test_client_bind_address(unsigned long address);

enum
{
	MAXIMUM_TEST_CLIENTS = 30,
	/* the first's address: 127.0.0.2 (127.0.0.1 is this machine's own client's) */
	FIRST_TEST_CLIENT_ADDRESS = IPV4_LOOPBACK_ADDRESS + 1,
	/* (the game's own, network_client_manager.c) */
	MAXIMUM_MACHINE_NAME_LENGTH = 32,
	MAXIMUM_NETWORK_MESSAGE_SIZE = HALO_PORT_MAXIMUM_NETWORK_MESSAGE_SIZE,
	/* the host's game running this long before the first joins, and a
	second between them */
	TEST_CLIENT_FIRST_TICKS = 3 * TICKS_PER_SECOND,
	TEST_CLIENT_INTERVAL_TICKS = TICKS_PER_SECOND,
	/* the host's objects asked for again until they come, at most this often */
	TEST_CLIENT_READY_TICKS = 5 * TICKS_PER_SECOND,
	TEST_CLIENT_REPORT_TICKS = 10 * TICKS_PER_SECOND,
	/* a connection the host sends nothing for this long is gone */
	TEST_CLIENT_TIMEOUT = 15000,
	/* the game's message kinds (message_header.h's packet; the distributed
	netcode's data, network_distributed.c) */
	TEST_MESSAGE_TYPE_DATA = 2,
	/* (network_client_message_handler.c's) */
	TEST_MESSAGE_VERSION = HALO_PORT_NETWORK_GAME_MESSAGE_VERSION,
	TEST_PACKET_CLASS_PREGAME = 2,
	/* (network_connection.c's) */
	TEST_TRANSPORT_UDP = 0x11,
	TEST_TRANSPORT_TCP = 0x12,
	/* what came over the stream and is not yet a whole message: two of the
	largest */
	TEST_STREAM_BUFFER_SIZE = 2 * MAXIMUM_NETWORK_MESSAGE_SIZE,
};

enum
{
	_test_client_unused,
	_test_client_joining,
	_test_client_accepted,
	_test_client_loaded,
	_test_client_failed,
};

/* the messages, as the game's own units have them (network_client_manager.c) */
struct test_message_client_join_game_request
{
	wchar_t machine_name[MAXIMUM_MACHINE_NAME_LENGTH];
	byte join_token[0x10];
	char hardware_id[0x20];
};

struct test_message_server_machine_accepted
{
	long random_seed;
	short machine_index;
	byte __padding6[2];
};

struct test_message_client_loaded
{
	long opaque;
};

/* (network_distributed.c's) */
struct test_distributed_player_input
{
	byte player_index;
	byte structure_bsp_index;
	byte pad[2];
	long tick;
	long host_time;
	struct player_action action;
	unsigned short control_flags[DISTRIBUTED_INPUT_HISTORY];
};

typedef char test_distributed_player_input_size_assert[
	sizeof(struct test_distributed_player_input) == 12 + 0x20 + 2 * DISTRIBUTED_INPUT_HISTORY ? 1 : -1];

struct test_client
{
	short state;
	short machine_index;
	/* its player's absolute index on the host, once it is in; else NONE */
	long player_index;
	unsigned long address;
	struct transport_endpoint *stream;
	struct transport_endpoint *datagrams;
	byte *stream_buffer;
	word stream_used;
	/* when the host last sent it anything (system_milliseconds) */
	unsigned long heard_time;
	long ready_time;
	boolean synchronized;
	long input_tick;
	/* the buttons of its ticks, newest first */
	unsigned short control_flags[DISTRIBUTED_INPUT_HISTORY];
	/* what the host sent it */
	long stream_bytes;
	long stream_messages;
	long datagram_bytes;
	long datagram_count;
};

static struct
{
	boolean checked;
	short count;
	/* the game they joined (the host's game time starts over with each) */
	boolean running;
	long started_time;
	long report_time;
	long reported_stream_bytes;
	long reported_datagram_bytes;
	struct test_client clients[MAXIMUM_TEST_CLIENTS];
} test_clients;

/* ---------- sending */

/* "<prefix> <number>", the client's number from 1, in the wide characters
of a name on the wire, ended */
static void test_client_name(
	wchar_t *name,
	short length,
	char const *prefix,
	short client_index)
{
	char text[MAXIMUM_MACHINE_NAME_LENGTH];
	short index;

	snprintf(text, sizeof(text), "%s %d", prefix, client_index + 1);
	for (index = 0; index < length - 1 && text[index]; index++)
		name[index] = (wchar_t)text[index];
	name[index] = 0;
}

/* a message to the host over the client's stream (reliable) or in a
datagram, its header in network byte order (a copy's) */
static boolean test_client_write(
	struct test_client *client,
	void const *message,
	word size,
	boolean reliable)
{
	word buffer[MAXIMUM_NETWORK_MESSAGE_SIZE / sizeof(word)];

	if (size > sizeof(buffer) || (!reliable && size > DATAGRAM_MAXIMUM_SIZE))
		return FALSE;
	csmemcpy(buffer, message, size);
	byte_swap_message_header(buffer, _byte_order_network);
	/* (a stream's messages are a few dozen bytes, written whole or the
	client is done) */
	return write_endpoint(reliable ? client->stream : client->datagrams, buffer, size) == size || !reliable;
}

static boolean test_client_send_game_message(
	struct test_client *client,
	enum network_game_message_type type,
	void const *message,
	short size)
{
	message_header *encoded = create_network_game_message(type, message, size);

	return encoded && test_client_write(client, encoded, (word)GET_MESSAGE_SIZE(*encoded), TRUE);
}

static void test_client_fill_header(
	struct distributed_message_header *header,
	byte type,
	short count,
	word size)
{
	header->type = type;
	header->count = (byte)count;
	header->game_time = game_time_get();
	header->header = 0;
	build_message_header(&header->header, size, TEST_MESSAGE_TYPE_DATA, 0);
}

/* the host's objects, please (network_objects_client_tick's ready) */
static void test_client_send_ready(
	struct test_client *client)
{
	struct distributed_message_header message;

	test_client_fill_header(&message, _distributed_message_client_ready, 0, (word)sizeof(message));
	test_client_write(client, &message, (word)sizeof(message), TRUE);
	client->ready_time = game_time_get();
}

/* the scripted player's input at the tick: different for each client */
static void test_client_action(
	short client_index,
	long tick,
	struct player_action *action)
{
	double t = (double)tick / TICKS_PER_SECOND + client_index * 1.7;
	double yaw = fmod(t * 0.4 + client_index, 2.0 * _pi);
	unsigned long control_flags = 0;

	if (fmod(t, 5.0) < 0.1)
		control_flags |= FLAG(_unit_control_jump_bit);
	if (fmod(t, 7.0) < 0.2)
		control_flags |= FLAG(_unit_control_throw_grenade_bit);
	csmemset(action, 0, sizeof(*action));
	action->control_flags = control_flags;
	action->desired_facing.yaw = (real)yaw;
	action->desired_facing.pitch = (real)(sin(t * 0.3) * 0.3);
	action->throttle.i = (real)sin(t * 0.9);
	action->throttle.j = (real)(cos(t * 0.6 + client_index) * 0.6);
	action->primary_trigger = fmod(t, 3.0) < 0.3 ? 1.0f : 0.0f;
	action->desired_weapon_index = NONE;
	action->desired_grenade_index = NONE;
	action->desired_zoom_level = NONE;
}

/* its player's input of the host's latest tick, with the buttons of the
ticks before it, once a tick (unreliable) */
static void test_client_send_input(
	short client_index,
	struct test_client *client)
{
	struct
	{
		struct distributed_message_header header;
		struct test_distributed_player_input input;
	} message;
	long tick = game_time_get();
	short index;

	if (client->player_index == NONE || tick <= client->input_tick)
		return;
	csmemset(&message, 0, sizeof(message));
	test_client_action(client_index, tick, &message.input.action);
	/* (the ticks since the last, a tick's buttons a slot, newest first) */
	for (index = 0; index < DISTRIBUTED_INPUT_HISTORY && client->input_tick + index < tick; index++)
	{
		csmemmove(client->control_flags + 1, client->control_flags,
			(DISTRIBUTED_INPUT_HISTORY - 1) * sizeof(client->control_flags[0]));
		client->control_flags[0] = (unsigned short)message.input.action.control_flags;
	}
	client->input_tick = tick;
	message.input.player_index = (byte)client->player_index;
	message.input.structure_bsp_index = (byte)global_structure_bsp_index_get();
	message.input.tick = tick;
	message.input.host_time = tick;
	csmemcpy(message.input.control_flags, client->control_flags, sizeof(message.input.control_flags));
	test_client_fill_header(&message.header, _distributed_message_player_inputs, 1, (word)sizeof(message));
	test_client_write(client, &message, (word)sizeof(message), FALSE);
}

/* ---------- receiving */

/* whether a distributed message (or a batch's) says the host's objects
are all sent */
static boolean test_client_objects_synchronized(
	byte const *message,
	word size)
{
	struct distributed_message_header header;
	word offset;

	if (size < sizeof(header))
		return FALSE;
	csmemcpy(&header, message, sizeof(header));
	if (header.type != _distributed_message_batch)
		return header.type == _distributed_message_objects_synchronized;
	/* (each a length, then the message from its type on) */
	for (offset = sizeof(header); offset + sizeof(word) + 1 <= size;)
	{
		word length;

		csmemcpy(&length, message + offset, sizeof(word));
		offset += sizeof(word);
		if (!length || length > size - offset)
			break;
		if (message[offset] == _distributed_message_objects_synchronized)
			return TRUE;
		offset += length;
	}
	return FALSE;
}

/* a message the host sent the client (its header in host byte order):
counted; its acceptance, its start and the host's objects all sent acted on */
static boolean test_client_handle_message(
	short client_index,
	struct test_client *client,
	word *message,
	word size,
	boolean datagram)
{
	if (datagram)
	{
		client->datagram_bytes += size;
		client->datagram_count++;
	}
	else
	{
		client->stream_bytes += size;
		client->stream_messages++;
	}
	switch (GET_MESSAGE_TYPE(message[0]))
	{
	case _message_type_packet:
		switch (((byte *)message)[size - 1])
		{
		case _message_server_machine_accepted:
			if (client->state == _test_client_joining)
			{
				struct test_message_server_machine_accepted accepted;
				short packet_type = _message_server_machine_accepted;
				short packet_version = TEST_MESSAGE_VERSION;
				short encoded_size = (short)(size - sizeof(word));

				if (!decode_network_game_message(&accepted, message + 1, &encoded_size, &packet_type,
					&packet_version, TEST_PACKET_CLASS_PREGAME))
				{
					return FALSE;
				}
				client->machine_index = accepted.machine_index;
				client->state = _test_client_accepted;
				/* (its one player, joining the game in progress: the
				host queues it, then starts the machine) */
				{
					struct network_player player;

					csmemset(&player, 0, sizeof(player));
					test_client_name(player.name, NUMBEROF(player.name), "Test", client_index);
					player.primary_color_index = (short)(client_index % 18);
					player.icon_index = NONE;
					player.machine_index = (char)client->machine_index;
					player.controller_index = 0;
					player.team_index = NONE;
					player.player_list_index = NONE;
					if (!test_client_send_game_message(client, _message_client_add_player_request_pregame,
						&player, sizeof(player)))
					{
						return FALSE;
					}
				}
				platform_log("test client %d: joined as machine #%d", client_index + 1, client->machine_index);
			}
			break;
		case _message_server_machine_rejected:
			platform_log("test client %d: refused by the host", client_index + 1);
			return FALSE;
		case _message_server_begin_game:
			if (client->state == _test_client_accepted)
			{
				struct test_message_client_loaded loaded = { 0 };

				if (!test_client_send_game_message(client, _message_client_loaded, &loaded, sizeof(loaded)))
					return FALSE;
				client->state = _test_client_loaded;
				test_client_send_ready(client);
				platform_log("test client %d: loaded", client_index + 1);
			}
			break;
		default:
			break;
		}
		break;
	case TEST_MESSAGE_TYPE_DATA:
		if (!client->synchronized &&
			test_client_objects_synchronized((byte const *)message, size))
		{
			client->synchronized = TRUE;
			platform_log("test client %d: has the host's objects (%ld KB so far)", client_index + 1,
				(client->stream_bytes + client->datagram_bytes) / 1024);
		}
		break;
	default:
		break;
	}
	return TRUE;
}

/* the size of the message whose header (in network byte order) begins at
data; 0 if it cannot be one */
static word test_client_message_size(
	byte const *data)
{
	message_header header;
	word size;

	csmemcpy(&header, data, sizeof(header));
	byte_swap_message_header(&header, _byte_order_host);
	size = (word)GET_MESSAGE_SIZE(header);
	if (GET_MESSAGE_FLAGS(header) || size < sizeof(message_header) + sizeof(byte) ||
		size > MAXIMUM_NETWORK_MESSAGE_SIZE)
	{
		return 0;
	}
	return size;
}

/* everything the host sent the client since the last frame: the stream's
whole messages, and the datagrams (one message each); FALSE once the stream
is lost or broken */
static boolean test_client_receive(
	short client_index,
	struct test_client *client)
{
	word message[MAXIMUM_NETWORK_MESSAGE_SIZE / sizeof(word)];
	long result;

	for (;;)
	{
		word offset = 0;

		result = read_endpoint(client->stream, client->stream_buffer + client->stream_used,
			TEST_STREAM_BUFFER_SIZE - client->stream_used);
		if (result == _transport_result_operation_would_block)
			break;
		if (result <= 0)
			return FALSE;
		client->heard_time = system_milliseconds();
		client->stream_used += (word)result;
		while (client->stream_used - offset >= sizeof(message_header))
		{
			word size = test_client_message_size(client->stream_buffer + offset);

			if (!size)
				return FALSE;
			if (client->stream_used - offset < size)
				break;
			csmemcpy(message, client->stream_buffer + offset, size);
			byte_swap_message_header(message, _byte_order_host);
			offset += size;
			if (!test_client_handle_message(client_index, client, message, size, FALSE))
				return FALSE;
		}
		csmemmove(client->stream_buffer, client->stream_buffer + offset, client->stream_used - offset);
		client->stream_used -= offset;
	}
	for (;;)
	{
		result = read_endpoint(client->datagrams, message, sizeof(message));
		if (result == _transport_result_operation_would_block)
			break;
		/* (an empty one, or a fault: nothing) */
		if (result <= 0)
			break;
		client->heard_time = system_milliseconds();
		if (test_client_message_size((byte const *)message) != result)
			continue;
		byte_swap_message_header(message, _byte_order_host);
		if (!test_client_handle_message(client_index, client, message, (word)result, TRUE))
			return FALSE;
	}
	return TRUE;
}

/* ---------- the clients */

static void test_client_delete(
	struct test_client *client)
{
	if (client->stream)
	{
		disconnect_endpoint(client->stream);
		delete_transport_endpoint(client->stream);
	}
	if (client->datagrams)
		delete_transport_endpoint(client->datagrams);
	if (client->stream_buffer)
		debug_free(client->stream_buffer, __FILE__, __LINE__);
	client->stream = NULL;
	client->datagrams = NULL;
	client->stream_buffer = NULL;
}

static void test_client_fail(
	short client_index,
	struct test_client *client,
	char const *reason)
{
	platform_log("test client %d: %s", client_index + 1, reason);
	test_client_delete(client);
	client->state = _test_client_failed;
}

/* the client's stream and datagram socket, from its own address (the
datagrams' at the client port, where the host sends them), and its join
request */
static void test_client_start(
	short client_index,
	struct test_client *client)
{
	struct test_message_client_join_game_request request;
	struct transport_address server_address;
	struct transport_address local_address;
	unsigned long address = FIRST_TEST_CLIENT_ADDRESS + client_index;
	boolean connected;

	csmemset(client, 0, sizeof(*client));
	client->machine_index = NONE;
	client->player_index = NONE;
	client->ready_time = NONE;
	client->input_tick = NONE;
	client->address = address;
	/* (the host's answers come from 127.0.0.1 where every loopback address
	is one interface's; on the Xbox, from the address's own interface, which
	the client then talks to) */
	csmemset(&server_address, 0, sizeof(server_address));
#ifdef HALO_XBOX
	server_address.address.long_words[0] = address;
#else
	server_address.address.long_words[0] = IPV4_LOOPBACK_ADDRESS;
#endif
	server_address.address_length = IPV4_ADDRESS_LENGTH;
	server_address.port = NETWORK_GAME_SERVER_PORT;
	/* (in network byte order) */
	xnet_test_client_bind_address(((address >> 24) & 0xFF) | ((address >> 8) & 0xFF00) | ((address & 0xFF00) << 8) |
		((address & 0xFF) << 24));
	csmemset(&local_address, 0, sizeof(local_address));
	local_address.address_length = IPV4_ADDRESS_LENGTH;
	local_address.port = NETWORK_GAME_CLIENT_PORT;
	client->stream_buffer = debug_malloc(TEST_STREAM_BUFFER_SIZE, FALSE, __FILE__, __LINE__);
	client->stream = create_transport_endpoint(TEST_TRANSPORT_TCP);
	client->datagrams = create_transport_endpoint(TEST_TRANSPORT_UDP);
	connected = client->stream_buffer && client->stream && client->datagrams &&
		bind_endpoint(client->datagrams, &local_address) == _transport_error_none &&
		set_endpoint_blocking(client->datagrams, FALSE) == _transport_error_none &&
		connect_endpoint(client->datagrams, &server_address) == _transport_error_none &&
		connect_endpoint(client->stream, &server_address) == _transport_error_none &&
		set_endpoint_blocking(client->stream, FALSE) == _transport_error_none;
	xnet_test_client_bind_address(0);
	if (!connected)
	{
		test_client_fail(client_index, client, "could not connect to the host");
		return;
	}
	client->heard_time = system_milliseconds();
	csmemset(&request, 0, sizeof(request));
	test_client_name(request.machine_name, MAXIMUM_MACHINE_NAME_LENGTH, "Test client", client_index);
	network_game_generate_join_game_token(request.join_token);
	/* (a hardware id of its own: the host logs and bans by it) */
	snprintf(request.hardware_id, sizeof(request.hardware_id), "7e57c11e%024x", client_index + 1);
	client->state = _test_client_joining;
	if (!test_client_send_game_message(client, _message_client_join_game_request, &request, sizeof(request)))
	{
		test_client_fail(client_index, client, "could not send its join request");
		return;
	}
	platform_log("test client %d: joining from 127.0.0.%lu", client_index + 1, address & 0xFF);
}

/* the client's player on the host, once the host has added it */
static long test_client_find_player(
	struct test_client *client)
{
	struct data_iterator iterator;
	struct player_datum *player;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (player->network_player_data.machine_index == client->machine_index)
			return DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
	}
	return NONE;
}

static void test_clients_stop(
	void)
{
	short index;

	for (index = 0; index < test_clients.count; index++)
	{
		test_client_delete(&test_clients.clients[index]);
		test_clients.clients[index].state = _test_client_unused;
	}
	test_clients.running = FALSE;
}

/* every ten seconds: the clients in the game, and what the host sent them
in that time, each on average */
static void test_clients_report(
	void)
{
	long stream_bytes = 0;
	long datagram_bytes = 0;
	long datagrams = 0;
	short in_game = 0;
	short failed = 0;
	short index;
	long elapsed = game_time_get() - test_clients.report_time;

	for (index = 0; index < test_clients.count; index++)
	{
		struct test_client *client = &test_clients.clients[index];

		stream_bytes += client->stream_bytes;
		datagram_bytes += client->datagram_bytes;
		datagrams += client->datagram_count;
		if (client->state == _test_client_loaded && client->player_index != NONE)
			in_game++;
		if (client->state == _test_client_failed)
			failed++;
	}
	if (elapsed > 0 && in_game)
	{
		long seconds_tenths = elapsed * 10 / TICKS_PER_SECOND;

		platform_log("test clients: %d of %d in the game (%d failed); the host sent each %ld kbit/s over its "
			"connection, %ld kbit/s in datagrams (%ld datagrams a second to them all since they began)", in_game, test_clients.count,
			failed, (stream_bytes - test_clients.reported_stream_bytes) * 8 * 10 / 1024 / seconds_tenths / in_game,
			(datagram_bytes - test_clients.reported_datagram_bytes) * 8 * 10 / 1024 / seconds_tenths / in_game,
			datagrams * TICKS_PER_SECOND / MAX(1, game_time_get() - test_clients.started_time));
	}
	test_clients.reported_stream_bytes = stream_bytes;
	test_clients.reported_datagram_bytes = datagram_bytes;
	test_clients.report_time = game_time_get();
}

void network_test_clients_update(
	void)
{
	short index;
	boolean hosting;

	if (!test_clients.checked)
	{
		test_clients.checked = TRUE;
		test_clients.count = (short)PIN(config_integer("debug.network_test_clients"), 0, MAXIMUM_TEST_CLIENTS);
	}
	if (!test_clients.count)
		return;
	/* (the host's game running; with the next game, joined again) */
	hosting = game_in_progress() && game_connection() == _game_connection_network_server &&
		global_network_game_server_get() != NULL;
	if (test_clients.running && (!hosting || game_time_get() < test_clients.started_time))
		test_clients_stop();
	if (!hosting)
		return;
	if (!test_clients.running)
	{
		test_clients.running = TRUE;
		test_clients.started_time = game_time_get();
		test_clients.report_time = game_time_get();
		test_clients.reported_stream_bytes = 0;
		test_clients.reported_datagram_bytes = 0;
	}

	for (index = 0; index < test_clients.count; index++)
	{
		struct test_client *client = &test_clients.clients[index];

		if (client->state == _test_client_unused &&
			game_time_get() - test_clients.started_time >= TEST_CLIENT_FIRST_TICKS + index * TEST_CLIENT_INTERVAL_TICKS)
		{
			test_client_start(index, client);
		}
		if (client->state == _test_client_unused || client->state == _test_client_failed)
			continue;
		if (!test_client_receive(index, client))
		{
			test_client_fail(index, client, "its stream from the host was lost or broken");
			continue;
		}
		if (system_milliseconds() - client->heard_time > TEST_CLIENT_TIMEOUT)
		{
			test_client_fail(index, client, "the host sent it nothing for 15 seconds");
			continue;
		}
		if (client->state != _test_client_loaded)
			continue;
		if (client->player_index == NONE)
		{
			client->player_index = test_client_find_player(client);
			if (client->player_index != NONE)
				platform_log("test client %d: playing as player #%ld", index + 1, client->player_index);
		}
		/* (the host's objects asked for again until they come, as a client
		asks) */
		if (!client->synchronized && game_time_get() - client->ready_time >= TEST_CLIENT_READY_TICKS)
			test_client_send_ready(client);
		test_client_send_input(index, client);
	}

	if (game_time_get() - test_clients.report_time >= TEST_CLIENT_REPORT_TICKS)
		test_clients_report();
}
