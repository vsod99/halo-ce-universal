/*
NSPIRE_LINK.C

Files to and from a TI-Nspire over USB (a CX II too, which TiLP's libraries
do not know), through libnspire (github.com/Vogtinator/libnspire). Built by
tools/nspire_link.sh:

	nspire_link ls /documents
	nspire_link put build/nspire/halo.tns /documents/halo.tns
	nspire_link get /documents/halo_log.txt.tns ~/Downloads/halo_log.txt.tns
	nspire_link rm /halo_b30_save.tns
	nspire_link shot screen.ppm
	nspire_link key 00fd00 [0d1000...]   (TiLP's KEYNSP_ codes: ascii, code, state)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nspire.h"

static int fail(const char *what, int error)
{
	fprintf(stderr, "nspire_link: %s: %s\n", what, nspire_strerror(error));
	return 1;
}

static int list(nspire_handle_t *handle, const char *path)
{
	struct nspire_dir_info *info;
	unsigned long index;
	int error = nspire_dirlist(handle, path, &info);

	if (error)
		return fail(path, error);
	for (index = 0; index < info->num; index++)
	{
		const struct nspire_dir_item *item = &info->items[index];

		printf("%10lu  %s%s\n", item->size, item->name, item->type == NSPIRE_DIR ? "/" : "");
	}
	nspire_dirlist_free(info);
	return 0;
}

static int read_once(const char *remote, void **data, size_t *size);

/* libnspire's own (not in its public headers) */
int service_connect(nspire_handle_t *handle, uint16_t sid);
int service_disconnect(nspire_handle_t *handle);
int data_write(nspire_handle_t *handle, void *ptr, size_t maxlen);

/* the screen, as a PPM */
static int shot(nspire_handle_t *handle, const char *local)
{
	struct nspire_image *image;
	FILE *file;
	unsigned long index, count;
	int error = nspire_screenshot(handle, &image);

	if (error)
		return fail("screenshot", error);
	file = fopen(local, "wb");
	if (!file)
	{
		perror(local);
		free(image);
		return 1;
	}
	fprintf(file, "P6\n%u %u\n255\n", image->width, image->height);
	count = (unsigned long)image->width * image->height;
	for (index = 0; index < count; index++)
	{
		unsigned char rgb[3];

		if (image->bpp == 16)
		{
			unsigned pixel = image->data[index * 2] | image->data[index * 2 + 1] << 8;

			rgb[0] = (pixel >> 11 & 31) * 255 / 31;
			rgb[1] = (pixel >> 5 & 63) * 255 / 63;
			rgb[2] = (pixel & 31) * 255 / 31;
		}
		else
		{
			unsigned char grey = image->bpp == 8 ? image->data[index] :
				(image->data[index / 2] >> (index & 1 ? 0 : 4) & 15) * 17;

			rgb[0] = rgb[1] = rgb[2] = grey;
		}
		fwrite(rgb, 1, 3, file);
	}
	fclose(file);
	printf("screen %ux%u at %u bits to %s\n", image->width, image->height, image->bpp, local);
	free(image);
	return 0;
}

/* a key, as the keypresses service (0x4042) takes it (TiLP's
nsp_cmd_s_key): TiLP's KEYNSP_ code, six hex digits of ascii, key code and
state */
static int key(nspire_handle_t *handle, const char *code)
{
	unsigned long value = strtoul(code, NULL, 16);
	unsigned char start[4] = { 0x01, 0x00, 0x00, 0x80 };
	unsigned char packet[26] = { 0 };
	int error = service_connect(handle, 0x4042);

	if (error)
		return fail("keypresses service", error);
	/* (the command byte first, then the data) */
	packet[1 + 3] = 0x08;
	packet[1 + 4] = 0x02;
	packet[1 + 5] = (unsigned char)(value >> 16);
	packet[1 + 7] = (unsigned char)(value >> 8);
	packet[1 + 23] = (unsigned char)value;
	error = data_write(handle, start, sizeof(start));
	if (!error)
		error = data_write(handle, packet, sizeof(packet));
	service_disconnect(handle);
	if (error)
		return fail(code, error);
	printf("key %s sent\n", code);
	return 0;
}

/* sent, then read back in a connection of its own to check it arrived whole */
static int put(nspire_handle_t *handle, const char *local, const char *remote)
{
	FILE *file = fopen(local, "rb");
	long size;
	void *data;
	int error;

	if (!file)
	{
		perror(local);
		return 1;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = malloc(size ? size : 1);
	if (!data || fread(data, 1, size, file) != (size_t)size)
	{
		fprintf(stderr, "nspire_link: could not read %s\n", local);
		fclose(file);
		return 1;
	}
	fclose(file);
	error = nspire_file_write(handle, remote, data, size);
	if (error)
	{
		free(data);
		return fail(remote, error);
	}
	{
		void *back;
		size_t back_size;

		error = read_once(remote, &back, &back_size);
		if (error || back_size != (size_t)size || memcmp(back, data, size))
		{
			fprintf(stderr, "nspire_link: %s did not read back as sent\n", remote);
			free(back);
			free(data);
			return 1;
		}
		free(back);
	}
	free(data);
	printf("sent %s to %s (%ld bytes, read back alike)\n", local, remote, size);
	return 0;
}

/* a file read in its own connection (reading several in one has come back
with one shifted after its first packet): data and size, or an error */
static int read_once(const char *remote, void **data, size_t *size)
{
	nspire_handle_t *handle;
	struct nspire_dir_item item;
	int error = nspire_init(&handle);

	*data = NULL;
	*size = 0;
	if (error)
		return error;
	error = nspire_attr(handle, remote, &item);
	if (!error)
	{
		*data = malloc(item.size ? item.size : 1);
		if (!*data)
			error = NSPIRE_ERR_NOMEM;
		else
			error = nspire_file_read(handle, remote, *data, item.size, size);
		if (!error && *size != item.size)
			error = NSPIRE_ERR_INVALPKT;
	}
	nspire_free(handle);
	if (error)
	{
		free(*data);
		*data = NULL;
	}
	return error;
}

/* read twice and kept only when both agree (up to four tries) */
static int get(const char *remote, const char *local)
{
	void *first = NULL, *second = NULL;
	size_t first_size = 0, second_size = 0;
	FILE *file;
	int attempt, error = 0;

	for (attempt = 0; attempt < 4; attempt++)
	{
		error = read_once(remote, &first, &first_size);
		if (!error)
			error = read_once(remote, &second, &second_size);
		if (!error && first_size == second_size && !memcmp(first, second, first_size))
			break;
		if (!error)
			fprintf(stderr, "nspire_link: %s: two reads differ, reading again\n", remote);
		free(first);
		free(second);
		first = second = NULL;
	}
	free(second);
	if (attempt == 4)
	{
		if (error)
			return fail(remote, error);
		fprintf(stderr, "nspire_link: %s: no two reads agreed\n", remote);
		return 1;
	}
	file = fopen(local, "wb");
	if (!file || fwrite(first, 1, first_size, file) != first_size)
	{
		perror(local);
		free(first);
		return 1;
	}
	fclose(file);
	free(first);
	printf("received %s to %s (%lu bytes, read twice alike)\n", remote, local, (unsigned long)first_size);
	return 0;
}

int main(int argc, char **argv)
{
	nspire_handle_t *handle;
	int error, result = 0, index;

	if (argc < 3 || (strcmp(argv[1], "ls") && strcmp(argv[1], "rm") && strcmp(argv[1], "shot") &&
		strcmp(argv[1], "key") && (argc < 4 || argc % 2)))
	{
		fprintf(stderr, "usage: %s ls PATH | rm REMOTE... | shot LOCAL.ppm | key CODE... | put LOCAL REMOTE [LOCAL REMOTE...] | get REMOTE LOCAL [REMOTE LOCAL...]\n",
			argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "get"))
	{
		for (index = 2; index + 1 < argc && !result; index += 2)
			result = get(argv[index], argv[index + 1]);
		return result;
	}
	error = nspire_init(&handle);
	if (error)
		return fail("connecting", error);
	if (!strcmp(argv[1], "ls"))
		result = list(handle, argv[2]);
	else if (!strcmp(argv[1], "rm"))
	{
		for (index = 2; index < argc && !result; index++)
		{
			error = nspire_file_delete(handle, argv[index]);
			if (error)
				result = fail(argv[index], error);
			else
				printf("deleted %s\n", argv[index]);
		}
	}
	else if (!strcmp(argv[1], "shot"))
		result = shot(handle, argv[2]);
	else if (!strcmp(argv[1], "key"))
	{
		for (index = 2; index < argc && !result; index++)
			result = key(handle, argv[index]);
	}
	else if (!strcmp(argv[1], "put"))
	{
		for (index = 2; index + 1 < argc && !result; index += 2)
			result = put(handle, argv[index], argv[index + 1]);
	}
	else
	{
		fprintf(stderr, "nspire_link: unknown command %s\n", argv[1]);
		result = 2;
	}
	nspire_free(handle);
	return result;
}
