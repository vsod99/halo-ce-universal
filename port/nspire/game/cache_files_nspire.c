/*
CACHE_FILES_NSPIRE.C

The cache file loader of the TI-Nspire port, in place of
source/cache/cache_files_windows.c and cache_files_decompress_windows.c.

The Xbox copies each map from the DVD to a cache partition, decompressing
it, and reads the tag data, structure BSPs, bitmaps and sounds from there
asynchronously. The calculator has one converted map next to the program
(tools/nspire_map.py writes it: <name>.map.tns), nothing to copy, and no
second thread to read on, so every "precache" is already done and every
read finishes before it returns.

A read of the tag data or of a structure BSP into the tag cache does not
read at all: the pager (port/nspire/src/nspire_paging.c) learns where each
of the region's deflated blocks is in the file, and reads and inflates one
when the game first touches it. The pager's pool of resident blocks is made
when the tag data is first read, from what the heap has left then, less a
margin for the game's own allocations.

Bitmaps come from the file's pixel data: the converter made each small
and pointed its header there. Sound data is not in the file; reads of it
find zeros.
*/

/* ---------- headers */

#include "cseries/cseries.h"
#include "cseries/errors.h"
#include "cache/cache_files.h"
#include "cache/texture_cache.h"
#include "tag_files/files.h"
#include "tag_files/tag_files.h"
#include "rasterizer/rasterizer.h"

#include <xtl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the converted map is opened by its calculator path, not an Xbox one
(port/linux/include/stdio.h translates those) */
#undef fopen
/* the blocks are the pager's to free (nspire_paging.c): the C library's
malloc and free, not the game's debug allocator (cseries.h) */
#undef malloc
#undef free

/* ---------- constants */

enum
{
	NSPIRE_MAP_VERSION = 2,
	NSPIRE_MAP_BLOCK_SIZE = 0x4000,
	MAXIMUM_REGIONS = 32,

	REGION_TAG_DATA = 0,
	REGION_STRUCTURE_BSP = 1,

	/* heap kept free for the game's own allocations when the pager's pool
	is made */
	HEAP_MARGIN = 0x200000,
};

#define NSPIRE_MAP_MAGIC 0x686E7370UL /* 'hnsp', stored as the bytes "psnh" as Halo's 'head' is */

/* ---------- structures */

struct cache_file_tag_instance;

struct cache_file_tag_header
{
	struct cache_file_tag_instance *tag_instances;
	long scenario_tag_index;
	unsigned long checksum;
	long tag_count;
	long vertex_buffer_count;
	D3DVertexBuffer *vertex_buffers;
	long index_buffer_count;
	D3DIndexBuffer *index_buffers;
	unsigned long signature;
};

struct cache_file_structure_bsp_header
{
	void *base_address;
	long vertex_buffer_count;
	D3DVertexBuffer *vertex_buffers;
	long lightmap_vertex_buffer_count;
	D3DVertexBuffer *lightmap_vertex_buffers;
	unsigned long signature;
};

struct cache_file_header
{
	unsigned long header_signature;
	long version;
	long file_length;
	byte reservedC[4];
	long tag_data_offset;
	long tag_data_size;
	byte reserved18[8];
	char name[0x20];
	char build[0x20];
	short scenario_type;
	short pad62;
	unsigned long checksum;
	byte reserved68[0x794];
	unsigned long footer_signature;
};

/* tools/nspire_map.py */
struct nspire_map_header
{
	unsigned long magic;
	unsigned long version;
	unsigned long block_size;
	unsigned long region_count;
	unsigned long pixel_data_offset;
	unsigned long pixel_data_size;
};

struct nspire_map_region
{
	unsigned long kind;
	unsigned long file_offset;
	unsigned long size;
	unsigned long address;
	unsigned long first_block_address;
	unsigned long block_count;
	unsigned long sizes_offset;
	unsigned long data_offset;
};

typedef char verify_cache_file_header_size[
	sizeof(struct cache_file_header) == 0x800 ? 1 : -1];
typedef char verify_nspire_map_region_size[
	sizeof(struct nspire_map_region) == 0x20 ? 1 : -1];

/* ---------- port/nspire/src */

void nspire_log(const char *format, ...);
const char *nspire_program_directory(void);
unsigned long nspire_paging_allocate_pool(unsigned long bytes);
void nspire_paging_set_block(unsigned long address, unsigned long file_offset, unsigned long size);
int nspire_paging_set_source(const char *path);
void nspire_paging_drop(unsigned long address, unsigned long size);
unsigned long nspire_memory_heap_free(void);

/* ---------- globals */

static struct
{
	FILE *file;
	char name[32];
	struct cache_file_header header;
	unsigned long region_count;
	struct nspire_map_region regions[MAXIMUM_REGIONS];
	/* the structure BSP whose blocks the pager has, or NONE */
	long loaded_structure_bsp;
	boolean pool_allocated;
	boolean open;
	unsigned long zero_reads;
	unsigned long pixel_data_offset;
	unsigned long pixel_data_size;
} nspire_cache;

/* ---------- private code */

static boolean nspire_map_file_open(
	char const *map_name)
{
	char path[300];
	struct nspire_map_header header;

	if (nspire_cache.file && !strcmp(nspire_cache.name, map_name))
		return TRUE;
	if (nspire_cache.file)
	{
		fclose(nspire_cache.file);
		nspire_cache.file = NULL;
	}
	snprintf(path, sizeof(path), "%s/%s.map.tns", nspire_program_directory(), map_name);
	nspire_cache.file = fopen(path, "rb");
	if (!nspire_cache.file)
	{
		nspire_log("cannot open %s", path);
		return FALSE;
	}
	if (fread(&header, sizeof(header), 1, nspire_cache.file) != 1 ||
		header.magic != NSPIRE_MAP_MAGIC ||
		header.version != NSPIRE_MAP_VERSION ||
		header.block_size != NSPIRE_MAP_BLOCK_SIZE ||
		header.region_count > MAXIMUM_REGIONS ||
		fread(&nspire_cache.header, sizeof(nspire_cache.header), 1, nspire_cache.file) != 1 ||
		fread(nspire_cache.regions, sizeof(struct nspire_map_region), header.region_count, nspire_cache.file) !=
			header.region_count)
	{
		nspire_log("%s is not a converted map (tools/nspire_map.py)", path);
		fclose(nspire_cache.file);
		nspire_cache.file = NULL;
		return FALSE;
	}
	if (!nspire_paging_set_source(path))
		nspire_log("the pager cannot open %s", path);
	nspire_cache.region_count = header.region_count;
	nspire_cache.pixel_data_offset = header.pixel_data_offset;
	nspire_cache.pixel_data_size = header.pixel_data_size;
	nspire_cache.loaded_structure_bsp = NONE;
	csstrncpy(nspire_cache.name, map_name, sizeof(nspire_cache.name) - 1);
	nspire_log("map %s: %lu regions, tag data %ld KB", path, nspire_cache.region_count,
		nspire_cache.header.tag_data_size / 1024);
	return TRUE;
}

static struct nspire_map_region *nspire_region_find(
	long offset,
	long size,
	void *buffer)
{
	unsigned long index;

	for (index = 0; index < nspire_cache.region_count; index++)
	{
		struct nspire_map_region *region = &nspire_cache.regions[index];

		if (region->file_offset == (unsigned long)offset && region->address == (unsigned long)buffer &&
			(unsigned long)size >= region->size)
		{
			return region;
		}
	}
	return NULL;
}

/* the pool: what the heap has left, less a margin for the game */
static void nspire_pool_allocate(
	void)
{
	unsigned long available, pool;

	if (nspire_cache.pool_allocated)
		return;
	available = nspire_memory_heap_free();
	pool = available > HEAP_MARGIN ? available - HEAP_MARGIN : 0;
	nspire_log("heap free %lu KB; pager pool %lu KB", available / 1024, pool / 1024);
	if (pool < 64 * NSPIRE_MAP_BLOCK_SIZE)
		error(_error_silent, "not enough memory for the pager (%lu KB free)", available / 1024);
	nspire_paging_allocate_pool(pool);
	nspire_cache.pool_allocated = TRUE;
}

/* tells the pager where the region's blocks are in the file */
static boolean nspire_region_page(
	struct nspire_map_region const *region)
{
	unsigned long *sizes = malloc(region->block_count * sizeof(unsigned long));
	unsigned long index, offset = region->data_offset;

	if (!sizes)
		return FALSE;
	fseek(nspire_cache.file, region->sizes_offset, SEEK_SET);
	if (fread(sizes, sizeof(unsigned long), region->block_count, nspire_cache.file) != region->block_count)
	{
		free(sizes);
		return FALSE;
	}
	for (index = 0; index < region->block_count; index++)
	{
		nspire_paging_set_block(region->first_block_address + index * NSPIRE_MAP_BLOCK_SIZE, offset, sizes[index]);
		offset += sizes[index];
	}
	free(sizes);
	return TRUE;
}

/* ---------- public code */

void tags_header_register_vertex_and_index_buffers(
	struct cache_file_tag_header *header)
{
	short index;

	for (index = 0; index < header->vertex_buffer_count; index++)
	{
		D3DVertexBuffer *vertex_buffer = &header->vertex_buffers[index];

		vertex_buffer->Common = D3DCOMMON_TYPE_VERTEXBUFFER | 1;
		IDirect3DVertexBuffer8_Register(vertex_buffer, NULL);
	}

	for (index = 0; index < header->index_buffer_count; index++)
	{
		D3DIndexBuffer *index_buffer = &header->index_buffers[index];

		index_buffer->Common = D3DCOMMON_TYPE_INDEXBUFFER | 1;
	}

	return;
}

void tags_header_deregister_vertex_and_index_buffers(
	struct cache_file_tag_header *header)
{
	(void)header;
	return;
}

void structure_bsp_header_register_vertex_buffers(
	struct cache_file_structure_bsp_header *header)
{
	short index;

	for (index = 0; index < header->vertex_buffer_count; index++)
	{
		D3DVertexBuffer *vertex_buffer = &header->vertex_buffers[index];

		vertex_buffer->Common = D3DCOMMON_TYPE_VERTEXBUFFER | 1;
		IDirect3DVertexBuffer8_Register(vertex_buffer, NULL);
	}

	for (index = 0; index < header->lightmap_vertex_buffer_count; index++)
	{
		D3DVertexBuffer *vertex_buffer = &header->lightmap_vertex_buffers[index];

		vertex_buffer->Common = D3DCOMMON_TYPE_VERTEXBUFFER | 1;
		IDirect3DVertexBuffer8_Register(vertex_buffer, NULL);
	}

	return;
}

void structure_bsp_header_deregister_vertex_buffers(
	struct cache_file_structure_bsp_header *header)
{
	(void)header;
	return;
}

void cache_files_initialize(
	void)
{
	csmemset(&nspire_cache, 0, sizeof(nspire_cache));
	nspire_cache.loaded_structure_bsp = NONE;
	return;
}

void cache_files_dispose(
	void)
{
	if (nspire_cache.file)
		fclose(nspire_cache.file);
	nspire_cache.file = NULL;
	return;
}

/* ---------- precaching: the converted map is always there */

void cache_files_precache_set_priority(
	boolean blocking)
{
	(void)blocking;
	return;
}

boolean cache_files_precache_in_progress(
	void)
{
	return FALSE;
}

boolean cache_files_precache_is_copying_map(
	const char *map_name)
{
	(void)map_name;
	return FALSE;
}

/* whether the map's converted file is there, without opening it for the
pager: precaching runs while the last map is still in use (the main menu's,
whose tag data the next map's would replace under it); cache_file_open
changes maps, once the last is disposed of */
static boolean nspire_map_file_exists(
	char const *map_name)
{
	char path[300];
	FILE *file;

	if (nspire_cache.file && !strcmp(nspire_cache.name, map_name))
		return TRUE;
	snprintf(path, sizeof(path), "%s/%s.map.tns", nspire_program_directory(), map_name);
	file = fopen(path, "rb");
	if (!file)
	{
		nspire_log("cannot open %s", path);
		return FALSE;
	}
	fclose(file);
	return TRUE;
}

boolean cache_files_precache_map_loaded(
	const char *map_name)
{
	return nspire_map_file_exists(tag_name_strip_path(map_name));
}

boolean cache_files_precache_map_begin(
	const char *map_name,
	boolean copy_map)
{
	(void)copy_map;
	return nspire_map_file_exists(tag_name_strip_path(map_name));
}

short cache_files_precache_map_status(
	real *progress)
{
	if (progress)
		*progress = 1.0f;
	return _cached_map_file_success;
}

void cache_files_precache_map_end(
	void)
{
	return;
}

void cache_files_precache_map_queue_end(
	void)
{
	return;
}

/* ---------- reading */

void cache_file_promote_read(
	short request_index)
{
	(void)request_index;
	return;
}

void cache_file_block_until_not_busy(
	void)
{
	return;
}

void cache_file_close(
	void)
{
	nspire_cache.open = FALSE;
	return;
}

boolean cache_file_open(
	const char *scenario_name,
	struct cache_file_header *header)
{
	match_assert("c:\\halo\\SOURCE\\cache\\cache_files_nspire.c", __LINE__, scenario_name && header);
	if (!nspire_map_file_open(scenario_name))
		return FALSE;
	csmemcpy(header, &nspire_cache.header, sizeof(struct cache_file_header));
	nspire_cache.open = TRUE;
	return TRUE;
}

short cache_file_read(
	long tag_index,
	long offset,
	long size,
	void *buffer,
	boolean *completion_flag_reference,
	boolean blocking)
{
	struct nspire_map_region *region = nspire_region_find(offset, size, buffer);

	(void)tag_index;
	(void)blocking;
	match_assert("c:\\halo\\SOURCE\\cache\\cache_files_nspire.c", __LINE__,
		nspire_cache.open && buffer && completion_flag_reference && offset >= 0);

	if (region)
	{
		if (region->kind == REGION_STRUCTURE_BSP)
		{
			long index = region - nspire_cache.regions;

			/* the BSP before this one leaves the pager first */
			if (nspire_cache.loaded_structure_bsp != NONE && nspire_cache.loaded_structure_bsp != index)
			{
				struct nspire_map_region *previous = &nspire_cache.regions[nspire_cache.loaded_structure_bsp];

				nspire_paging_drop(previous->address, previous->size);
			}
			nspire_cache.loaded_structure_bsp = index;
		}
		if (!nspire_region_page(region))
			error(_error_silent, "couldn't page in the region at 0x%08lx", region->address);
		/* before the game touches the tag data */
		nspire_pool_allocate();
		nspire_log("paged region at 0x%08lx: %lu KB in %lu blocks", region->address, region->size / 1024,
			region->block_count);
	}
	else if ((unsigned long)offset >= nspire_cache.pixel_data_offset &&
		(unsigned long)offset + (unsigned long)size <= nspire_cache.pixel_data_offset + nspire_cache.pixel_data_size)
	{
		/* a bitmap's pixels (tools/nspire_map.py pointed it here) */
		fseek(nspire_cache.file, offset, SEEK_SET);
		if (fread(buffer, 1, size, nspire_cache.file) != (size_t)size)
			error(_error_silent, "couldn't read %ld bytes of pixels at %ld", size, offset);
	}
	else
	{
		/* sounds are not in the converted map */
		csmemset(buffer, 0, size);
		if (nspire_cache.zero_reads++ < 16)
			nspire_log("read of %ld bytes at %ld (tag %ld) answered with zeros", size, offset, tag_index);
	}

	*completion_flag_reference = TRUE;
	return 0;
}
