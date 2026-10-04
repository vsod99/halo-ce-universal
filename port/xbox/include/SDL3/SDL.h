/*
SDL.H

The Xbox has no SDL 3. The platform units it shares with the desktop ports
use SDL's files only (port/linux/src/port_config.c reads and writes
config.toml, menu_files.c reads and lists the menus folder): these are those
functions, for the Xbox (port/xbox/src/sdl_files.c).
*/

#ifndef __HALO_XBOX_SDL_H
#define __HALO_XBOX_SDL_H

#include <stdbool.h>
#include <stddef.h>

typedef unsigned int SDL_GlobFlags;

/* the folder of config.toml and menus/, with its separator: the hard disk's
E:/halo/ (D: may be a disc) */
const char *SDL_GetBasePath(void);
/* the whole file, which SDL_free frees, or NULL */
void *SDL_LoadFile(const char *file, size_t *datasize);
bool SDL_SaveFile(const char *file, const void *data, size_t datasize);
void SDL_free(void *memory);
/* the names under path that match pattern ("*.xml", or the same one folder
down), relative to path, in one block for SDL_free */
char **SDL_GlobDirectory(const char *path, const char *pattern, SDL_GlobFlags flags, int *count);

#endif /* __HALO_XBOX_SDL_H */
