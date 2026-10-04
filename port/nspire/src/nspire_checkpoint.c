/*
NSPIRE_CHECKPOINT.C

The game's checkpoints on the TI-Nspire (source/saved games/
game_state_xbox.c). The Xbox wrote the whole game state, 3.3 MB, to
z:\savegame.bin at every checkpoint; the calculator's flash takes over a
minute and a half for that. Most of the game state is unused space, all
zeros, so a checkpoint is kept in memory with its runs of zero words
counted rather than stored, and only when there is no memory for it goes to
a file (halo_checkpoint.tns next to the program), in the same form.

The form is a series of runs, each a word: its length in words shifted left
one, with bit 0 set for a run of zeros; the words of a run of other words
follow it.

When the game quits (esc held), the last checkpoint is kept in
halo_<level>_save.tns, in the same form, and the next start goes back to it
once the level has loaded (game_state_nspire_resume in source/saved games/
game_state.c): the cutscene that opens a level need not be sat through
again.
*/

#include "nspire.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECKPOINT_FILE "halo_checkpoint.tns"
/* runs of fewer zero words than this are kept as words */
#define MINIMUM_ZERO_RUN 4

static struct
{
	unsigned long *data;
	unsigned long words;
	int in_file;
	int valid;
	/* made since the save file was last written or read */
	int unsaved;
} checkpoint;

/* the OS's file calls copy through this, not straight from the game's memory
(they fault on the paged memory window, which only the game's own code may
touch) */
static unsigned char bounce[32 * 1024];

/* the encoded size in words of a buffer, and the encoding into output or
file when one is given (0 in *failed when the file could not be
written) */
static unsigned long encode_to(const unsigned long *input, unsigned long count, unsigned long *output, FILE *file,
	int *failed)
{
	unsigned long index = 0, written = 0;

	while (index < count)
	{
		unsigned long run = 0;

		while (index + run < count && !input[index + run])
			run++;
		if (run >= MINIMUM_ZERO_RUN || index + run == count)
		{
			unsigned long header = (run << 1) | 1;

			if (output)
				output[written] = header;
			if (file && fwrite(&header, sizeof(header), 1, file) != 1)
				*failed = 1;
			written++;
			index += run;
			continue;
		}
		/* other words, up to the next run of zeros long enough to count */
		run = 0;
		while (index + run < count)
		{
			unsigned long zeros = 0;

			while (zeros < MINIMUM_ZERO_RUN && index + run + zeros < count && !input[index + run + zeros])
				zeros++;
			if (zeros == MINIMUM_ZERO_RUN || (zeros && index + run + zeros == count))
				break;
			run += zeros ? zeros : 1;
		}
		if (output)
		{
			output[written] = run << 1;
			memcpy(output + written + 1, input + index, run * sizeof(unsigned long));
		}
		if (file)
		{
			unsigned long header = run << 1;

			if (fwrite(&header, sizeof(header), 1, file) != 1 ||
				fwrite(input + index, sizeof(unsigned long), run, file) != run)
			{
				*failed = 1;
			}
		}
		written += 1 + run;
		index += run;
	}
	return written;
}

static unsigned long encode(const unsigned long *input, unsigned long count, unsigned long *output)
{
	return encode_to(input, count, output, NULL, NULL);
}

static int decode(const unsigned long *input, unsigned long words, unsigned long *output, unsigned long count)
{
	unsigned long index = 0, read = 0;

	while (read < words)
	{
		unsigned long header = input[read++], run = header >> 1;

		if (index + run > count)
			return 0;
		if (header & 1)
		{
			memset(output + index, 0, run * sizeof(unsigned long));
		}
		else
		{
			if (read + run > words)
				return 0;
			memcpy(output + index, input + read, run * sizeof(unsigned long));
			read += run;
		}
		index += run;
	}
	return index == count;
}

static void checkpoint_path(char *path, unsigned long size)
{
	snprintf(path, size, "%s/%s", nspire_program_directory(), CHECKPOINT_FILE);
}

int nspire_checkpoint_save(const void *buffer, unsigned long size)
{
	unsigned long count = size / sizeof(unsigned long), words;
	unsigned long long start = nspire_ticks();
	unsigned long *data;

	words = encode(buffer, count, NULL);
	/* the last checkpoint goes first, to make room */
	free(checkpoint.data);
	checkpoint.data = NULL;
	checkpoint.valid = 0;
	data = malloc(words * sizeof(unsigned long));
	if (data)
	{
		encode(buffer, count, data);
		checkpoint.data = data;
		checkpoint.words = words;
		checkpoint.in_file = 0;
		checkpoint.valid = 1;
	}
	else
	{
		/* no room: to the file, written as it is encoded */
		char path[256];
		FILE *file;
		int failed = 0;

		checkpoint_path(path, sizeof(path));
		file = fopen(path, "wb");
		if (file)
		{
			encode_to(buffer, count, NULL, file, &failed);
			if (fclose(file) != 0)
				failed = 1;
		}
		checkpoint.words = words;
		checkpoint.in_file = 1;
		checkpoint.valid = file && !failed;
	}
	checkpoint.unsaved = checkpoint.valid;
	nspire_log("checkpoint: %lu KB of game state in %lu KB %s, %lu ms", size / 1024,
		checkpoint.words * sizeof(unsigned long) / 1024, checkpoint.in_file ? "in the file" : "in memory",
		(unsigned long)((nspire_ticks() - start) * 1000ULL / 32768ULL));
	{
		/* (a demo starts from this one: nspire_demo.c) */
		extern void nspire_demo_saved(void);

		nspire_demo_saved();
	}
	return checkpoint.valid;
}

int nspire_checkpoint_load(void *buffer, unsigned long size)
{
	unsigned long count = size / sizeof(unsigned long);
	int ok;
	unsigned long long start = nspire_ticks();

	if (!checkpoint.valid)
		return 0;
	if (!checkpoint.in_file)
	{
		ok = decode(checkpoint.data, checkpoint.words, buffer, count);
	}
	else
	{
		/* from the file, a run at a time */
		char path[256];
		FILE *file;
		unsigned long index = 0, read = 0, header, run;

		checkpoint_path(path, sizeof(path));
		file = fopen(path, "rb");
		ok = file != NULL;
		while (ok && read < checkpoint.words)
		{
			ok = fread(&header, sizeof(header), 1, file) == 1;
			read++;
			run = header >> 1;
			if (!ok || index + run > count)
			{
				ok = 0;
				break;
			}
			if (header & 1)
			{
				memset((unsigned long *)buffer + index, 0, run * sizeof(unsigned long));
			}
			else
			{
				ok = fread((unsigned long *)buffer + index, sizeof(unsigned long), run, file) == run;
				read += run;
			}
			index += run;
		}
		if (file)
			fclose(file);
		ok = ok && index == count;
	}
	nspire_log("checkpoint loaded (%s), %lu ms", ok ? "ok" : "failed",
		(unsigned long)((nspire_ticks() - start) * 1000ULL / 32768ULL));
	return ok;
}

void nspire_checkpoint_dispose(void)
{
	free(checkpoint.data);
	checkpoint.data = NULL;
	checkpoint.valid = 0;
}

extern const char *nspire_level_name(void);

extern int nspire_demo_playing(void);

static void save_path(char *path, unsigned long size)
{
	/* (a demo plays from its own: nspire_demo.c) */
	snprintf(path, size, "%s/halo_%s_save.tns", nspire_program_directory(),
		nspire_demo_playing() ? "demo" : nspire_level_name());
}

/* the checkpoint into a file of its own, leaving it as it is (a demo's
start: nspire_demo.c) */
int nspire_checkpoint_write(const char *path)
{
	unsigned long long start = nspire_ticks();
	unsigned long bytes = checkpoint.words * sizeof(unsigned long), done = 0;
	int file, from = -1, ok;

	if (!checkpoint.valid)
		return 0;
	if (checkpoint.in_file)
	{
		char from_path[256];

		checkpoint_path(from_path, sizeof(from_path));
		from = open(from_path, O_RDONLY);
		if (from < 0)
			return 0;
	}
	file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	ok = file >= 0;
	while (ok && done < bytes)
	{
		unsigned long chunk = bytes - done < sizeof(bounce) ? bytes - done : sizeof(bounce);

		if (from >= 0)
			ok = read(from, bounce, chunk) == (long)chunk;
		else
			memcpy(bounce, (const unsigned char *)checkpoint.data + done, chunk);
		ok = ok && write(file, bounce, chunk) == (long)chunk;
		done += chunk;
	}
	if (from >= 0)
		close(from);
	if (file >= 0 && close(file) != 0)
		ok = 0;
	if (!ok)
		remove(path);
	nspire_log("checkpoint %s %s, %lu ms", ok ? "written to" : "could not be written to", path,
		(unsigned long)((nspire_ticks() - start) * 1000ULL / 32768ULL));
	return ok;
}

/* the last checkpoint into the save file, when it is newer than the file */
void nspire_checkpoint_persist(void)
{
	char path[256];
	unsigned long long start = nspire_ticks();
	int ok = 1;

	if (!checkpoint.valid || !checkpoint.unsaved)
		return;
	save_path(path, sizeof(path));
	if (checkpoint.in_file)
	{
		/* (already in a file, the one made when memory ran out) */
		char from[256];

		checkpoint_path(from, sizeof(from));
		remove(path);
		ok = rename(from, path) == 0;
		if (ok)
			checkpoint.valid = 0;
	}
	else
	{
		int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		unsigned long bytes = checkpoint.words * sizeof(unsigned long), done = 0;

		ok = file >= 0;
		while (ok && done < bytes)
		{
			unsigned long chunk = bytes - done < sizeof(bounce) ? bytes - done : sizeof(bounce);

			memcpy(bounce, (const unsigned char *)checkpoint.data + done, chunk);
			ok = write(file, bounce, chunk) == (long)chunk;
			done += chunk;
		}
		if (file >= 0 && close(file) != 0)
			ok = 0;
		if (!ok)
			remove(path);
	}
	checkpoint.unsaved = !ok;
	nspire_log("checkpoint %s %s, %lu ms", ok ? "saved to" : "could not be saved to", path,
		(unsigned long)((nspire_ticks() - start) * 1000ULL / 32768ULL));
}

/* the save file, if there is one, made the checkpoint (in memory) */
int nspire_checkpoint_resume(void)
{
	char path[256];
	int file;
	long bytes;
	unsigned long done = 0;
	unsigned long *data;

	save_path(path, sizeof(path));
	file = open(path, O_RDONLY);
	if (file < 0)
		return 0;
	bytes = lseek(file, 0, SEEK_END);
	lseek(file, 0, SEEK_SET);
	data = bytes > 0 && !(bytes & 3) ? malloc(bytes) : NULL;
	while (data && done < (unsigned long)bytes)
	{
		unsigned long chunk = bytes - done < sizeof(bounce) ? bytes - done : sizeof(bounce);

		if (read(file, bounce, chunk) != (long)chunk)
		{
			free(data);
			data = NULL;
			break;
		}
		memcpy((unsigned char *)data + done, bounce, chunk);
		done += chunk;
	}
	close(file);
	if (!data)
	{
		nspire_log("checkpoint: %s could not be read", path);
		return 0;
	}
	nspire_checkpoint_dispose();
	checkpoint.data = data;
	checkpoint.words = bytes / sizeof(unsigned long);
	checkpoint.in_file = 0;
	checkpoint.valid = 1;
	checkpoint.unsaved = 0;
	nspire_log("checkpoint: %lu KB read from %s", (unsigned long)bytes / 1024, path);
	return 1;
}

/* size bytes of the checkpoint's game state from offset (both whole
words), without decoding the rest: its header, to check before reverting */
int nspire_checkpoint_peek(unsigned long offset, void *out, unsigned long size)
{
	unsigned long first = offset / sizeof(unsigned long), count = size / sizeof(unsigned long);
	unsigned long index = 0, read = 0;
	unsigned long *output = out;

	if (!checkpoint.valid || checkpoint.in_file || (offset | size) & 3)
		return 0;
	while (read < checkpoint.words && index < first + count)
	{
		unsigned long header = checkpoint.data[read++], run = header >> 1, k;

		for (k = 0; k < run; k++, index++)
		{
			if (index >= first && index < first + count)
				output[index - first] = header & 1 ? 0 : checkpoint.data[read + k];
		}
		if (!(header & 1))
			read += run;
	}
	return index >= first + count;
}
