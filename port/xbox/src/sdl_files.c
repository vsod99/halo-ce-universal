/*
SDL_FILES.C

The SDL file functions the shared platform units call
(port/xbox/include/SDL3/SDL.h), over the Xbox's POSIX layer (nxdk_posix.c).
*/

#include "posix.h"

#include <SDL3/SDL.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *SDL_GetBasePath(void)
{
	return "E:/halo/";
}

void SDL_free(void *memory)
{
	free(memory);
}

void *SDL_LoadFile(const char *file, size_t *datasize)
{
	struct posix_file_information information;
	int descriptor = open(file, O_RDONLY);
	unsigned char *data;
	size_t size, done = 0;

	if (descriptor < 0)
		return NULL;
	if (posix_fstat(descriptor, &information) != 0 || information.size_high)
	{
		close(descriptor);
		return NULL;
	}
	size = information.size_low;
	/* (one more byte, NUL, as SDL's) */
	data = malloc(size + 1);
	while (data && done < size)
	{
		ssize_t count = read(descriptor, data + done, size - done);

		if (count <= 0)
		{
			free(data);
			data = NULL;
			break;
		}
		done += (size_t)count;
	}
	close(descriptor);
	if (!data)
		return NULL;
	data[size] = 0;
	if (datasize)
		*datasize = size;
	return data;
}

bool SDL_SaveFile(const char *file, const void *data, size_t datasize)
{
	int descriptor = open(file, O_WRONLY | O_CREAT | O_TRUNC);
	size_t done = 0;

	if (descriptor < 0)
		return false;
	while (done < datasize)
	{
		ssize_t count = write(descriptor, (const unsigned char *)data + done, datasize - done);

		if (count <= 0)
			break;
		done += (size_t)count;
	}
	return close(descriptor) == 0 && done == datasize;
}

/* ---------- listing */

struct glob
{
	char **names;
	int count;
	int capacity;
};

static int ends_with(const char *name, const char *suffix)
{
	size_t name_length = strlen(name);
	size_t suffix_length = strlen(suffix);

	return name_length >= suffix_length && !_stricmp(name + name_length - suffix_length, suffix);
}

static void glob_add(struct glob *glob, const char *name)
{
	if (glob->count == glob->capacity)
	{
		int capacity = glob->capacity ? glob->capacity * 2 : 16;
		char **names = realloc(glob->names, capacity * sizeof(*names));

		if (!names)
			return;
		glob->names = names;
		glob->capacity = capacity;
	}
	glob->names[glob->count] = strdup(name);
	if (glob->names[glob->count])
		glob->count++;
}

/* the names in folder ending in suffix ("" for every folder), each after
prefix */
static void glob_folder(struct glob *glob, const char *folder, const char *prefix, const char *suffix, int folders)
{
	void *directory = posix_directory_open(folder);
	char name[256];

	if (!directory)
		return;
	while (posix_directory_next(directory, name, sizeof(name)))
	{
		char path[1024];
		struct posix_file_information information;
		int is_folder;

		snprintf(path, sizeof(path), "%s/%s", folder, name);
		is_folder = posix_stat(path, &information) == 0 && (information.flags & _posix_file_is_directory);
		if (folders ? is_folder : (!is_folder && ends_with(name, suffix)))
		{
			snprintf(path, sizeof(path), "%s%s", prefix, name);
			glob_add(glob, path);
		}
	}
	posix_directory_close(directory);
}

char **SDL_GlobDirectory(const char *path, const char *pattern, SDL_GlobFlags flags, int *count)
{
	struct glob glob = { 0 };
	struct glob folders = { 0 };
	const char *slash = strchr(pattern, '/');
	/* the pattern's file part: "*.xml" */
	const char *suffix = (slash ? slash + 1 : pattern) + 1;
	char **result;
	char *text;
	size_t bytes;
	int index;

	(void)flags;
	if (slash)
	{
		/* "*" "/" "*.xml": the files of each folder in path */
		glob_folder(&folders, path, "", "", 1);
		for (index = 0; index < folders.count; index++)
		{
			char folder[1024];
			char prefix[300];

			snprintf(folder, sizeof(folder), "%s/%s", path, folders.names[index]);
			snprintf(prefix, sizeof(prefix), "%s/", folders.names[index]);
			glob_folder(&glob, folder, prefix, suffix, 0);
			free(folders.names[index]);
		}
		free(folders.names);
	}
	else
	{
		glob_folder(&glob, path, "", suffix, 0);
	}

	/* as SDL: the pointers, a NULL, and the names, in one block */
	bytes = (glob.count + 1) * sizeof(char *);
	for (index = 0; index < glob.count; index++)
		bytes += strlen(glob.names[index]) + 1;
	result = malloc(bytes);
	if (result)
	{
		text = (char *)(result + glob.count + 1);
		for (index = 0; index < glob.count; index++)
		{
			result[index] = text;
			strcpy(text, glob.names[index]);
			text += strlen(text) + 1;
		}
		result[glob.count] = NULL;
	}
	for (index = 0; index < glob.count; index++)
		free(glob.names[index]);
	free(glob.names);
	if (count)
		*count = result ? glob.count : 0;
	return result;
}
