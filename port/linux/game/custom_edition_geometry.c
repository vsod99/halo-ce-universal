/*
CUSTOM_EDITION_GEOMETRY.C

The model and structure BSP geometry of Halo Custom Edition maps, made into
the geometry this build draws (custom_edition_cache.h).

Custom Edition keeps geometry as Halo PC draws it. A model's parts hold no
vertices: their strips and uncompressed vertices lie in the map's model data,
which no tag holds, and when a model's parts have local nodes, each vertex
names its nodes through a table of its part's. A structure BSP's materials
hold uncompressed environment and lightmap vertices. This build draws
compressed vertices only, from Direct3D buffers that Xbox caches carry ready
made, so every part and material has its vertices compressed by the game's
own rasterizer_geometry_compress_vertices and its buffers made by the game's
own rasterizer_vertex_buffer_new and rasterizer_triangle_buffer_new. The
strips need no change. Against the Xbox maps of build 2276, whose models have
this build's layout, the models the two versions of Blood Gulch share have
the same strips and positions, and their compressed texture coordinates,
node indices and node weights are what the compressor makes of the Custom
Edition vertices (docs/custom_edition_caches.md).
*/

/* ---------- headers */

#include "cseries.h"
#include "cseries/cseries_windows.h"
#include "errors.h"
#include "tag_files/tag_groups.h"
#include "models/model_definitions.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_model_types.h"
#include "structures/structure_bsp_definitions.h"
#include "cache_file_formats.h"
#include "custom_edition_cache.h"
#include "memory/data.h"
#include "memory/lruv_cache.h"

#include <math.h>
#include <stdlib.h>
#include <xtl.h>
#ifdef HALO_XBOX
#include "cache/physical_memory_map.h"
#include "cache/texture_cache.h"
#endif

/* ---------- constants */

enum
{
	GBXMODEL_GROUP_TAG = 'mod2',
};

enum
{
	/* a gbxmodel's parts have node tables of their own (OpenSauce
	model_definitions.hpp, gbxmodel_definition::parts_have_local_nodes) */
	_gbxmodel_parts_have_local_nodes_bit = 1,
};

enum
{
	/* the part's vertices name nodes through its node table: set on exactly
	the parts that have one in every map examined (models.c) */
	_model_geometry_part_local_nodes_bit = 1,
};

/* rasterizer_geometry_compress_vertices clamps normals and the like to
[-1, 1] and asserts the packed vector is within 0.01 of the original: a
component this far past 1 stays within that after the packing's rounding
(every vector of the maps examined is within 1.0001) */
#define MAXIMUM_COMPRESSIBLE_COMPONENT 1.005f

/* the original Xbox's buffers are drawn in place, from headers of their
own (geometry_vertex_buffer_new); the other builds' are the game's */
#ifdef HALO_XBOX
#define GEOMETRY_HEADER_SIZE 12
#define GEOMETRY_VERTEX_BUFFER_NEW geometry_vertex_buffer_new
#define GEOMETRY_TRIANGLE_BUFFER_NEW geometry_strip_buffer_new
#else
#define GEOMETRY_VERTEX_BUFFER_NEW rasterizer_vertex_buffer_new
#define GEOMETRY_TRIANGLE_BUFFER_NEW rasterizer_triangle_buffer_new
#endif

/* ---------- structures */

/* this build's model geometry and part, as models.c and rasterizer.c each
define them for themselves (no header declares them) */
struct model_geometry
{
	unsigned long flags;
	/* (the tag's padding) on the original Xbox, a geometry read from the
	map when drawn: GEOMETRY_CACHE_SIGNATURE, and its block in the geometry
	cache, or NONE */
	unsigned long cache_signature;
	long cache_block_index;
	byte pad[0x18];
	struct tag_block parts;
};

/* the nodes a part's vertices name, by their place among them */
struct part_nodes
{
	byte count;
	byte nodes[MAXIMUM_NODES_PER_MODEL_GEOMETRY_PART];
};

/* Where a part read when drawn (on the original Xbox) has its vertices and
strip in the map, kept in the room of its tag's empty blocks, whose counts
stay 0 */
struct part_source
{
	long empty_uncompressed_vertex_count;
	/* from the start of the model data, and of its strips */
	unsigned long vertex_offset;
	unsigned long strip_offset;
	long empty_compressed_vertex_count;
	/* the nodes the map's vertices are named again through, or NULL when
	they stay as they are: the model's nodes by their place among the
	part's own when `nodes_made_local`, else the reverse */
	struct part_nodes const *nodes;
	unsigned long nodes_made_local;
	long empty_triangle_count;
	unsigned long unused[2];
};

struct model_geometry_part
{
	unsigned long flags;
	short shader_index;
	char previous_part_index;
	char next_part_index;
	short centroid_primary_node_index;
	short centroid_secondary_node_index;
	real centroid_primary_node_weight;
	real centroid_secondary_node_weight;
	real_point3d centroid;
	union
	{
		/* its uncompressed vertices, compressed vertices and triangles:
		none, as in Xbox caches (the game draws from the buffers alone) */
		struct tag_block blocks[3];
		struct part_source source;
	} data;
	struct triangle_buffer triangle_buffer;
	struct vertex_buffer vertex_buffer;
};

/* A model part as Custom Edition caches hold it (OpenSauce
model_definitions.hpp, gbxmodel_geometry_part). Where this build keeps its
buffers it keeps the kind, length and place of its strip and vertices in the
model data (cache_file_formats.c checked they lie within it), and after them
the table its vertices' node indices go through when the model's parts have
local nodes. */
struct custom_edition_model_part
{
	unsigned long flags;
	short shader_index;
	char previous_part_index;
	char next_part_index;
	short centroid_primary_node_index;
	short centroid_secondary_node_index;
	real centroid_primary_node_weight;
	real centroid_secondary_node_weight;
	real_point3d centroid;
	struct tag_block uncompressed_vertices;
	struct tag_block compressed_vertices;
	struct tag_block triangles;
	short strip_type;
	word pad1;
	long strip_triangle_count;
	unsigned long strip_offset;
	unsigned long unused1;
	short vertex_type;
	word pad2;
	long vertex_count;
	unsigned long unused2[2];
	unsigned long vertex_offset;
	byte pad3[3];
	byte local_node_count;
	byte local_node_indices[MAXIMUM_NODES_PER_MODEL_GEOMETRY_PART];
	word pad4;
};

typedef char verify_model_geometry_size[
	sizeof(struct model_geometry) == 0x30 ? 1 : -1];
typedef char verify_model_geometry_part_size[
	sizeof(struct model_geometry_part) == 0x68 ? 1 : -1];
typedef char verify_model_geometry_parts_offset[
	offsetof(struct model_geometry, parts) == 0x24 ? 1 : -1];
typedef char verify_part_source_size[
	sizeof(struct part_source) == 3 * sizeof(struct tag_block) &&
	offsetof(struct part_source, empty_compressed_vertex_count) == sizeof(struct tag_block) &&
	offsetof(struct part_source, empty_triangle_count) == 2 * sizeof(struct tag_block) ? 1 : -1];
typedef char verify_custom_edition_model_part_size[
	sizeof(struct custom_edition_model_part) == 0x84 ? 1 : -1];
typedef char verify_custom_edition_model_part_vertex_offset[
	offsetof(struct custom_edition_model_part, vertex_offset) == 0x64 ? 1 : -1];
typedef char verify_structure_material_size[
	sizeof(struct structure_material) == 0x100 ? 1 : -1];

/* A part of a model with more nodes than the renderer skins at once
(RASTERIZER_MAXIMUM_NODES_PER_MODEL - 1): its vertices name the part's own
nodes, and before it is drawn the renderer is given those nodes' matrices
alone (rasterizer_model_part_skinning, by its vertex buffer). Custom Edition
models of so many nodes have local nodes, at most
MAXIMUM_NODES_PER_MODEL_GEOMETRY_PART to a part. */
struct part_palette
{
	struct vertex_buffer const *vertex_buffer;
	struct part_nodes nodes;
};

/* what the models of a map need, counted before any is converted */
struct model_geometry_totals
{
	long part_count;
	long vertex_count;
	long strip_index_count;
	long largest_part_vertex_count;
	/* the parts of models of more nodes than the renderer skins at once */
	long many_node_part_count;
	/* the parts of the others whose vertices name their own nodes */
	long local_node_part_count;
};

struct custom_edition_geometry_globals
{
	/* the model parts that have buffers, and the compressed vertices and
	strips their buffers were made from */
	struct model_geometry_part **model_parts;
	long model_part_count;
	byte *model_geometry;
	boolean model_geometry_contiguous;
	/* the parts of the models of many nodes, in the order they were converted */
	struct part_palette *palettes;
	long palette_count;
#ifdef HALO_XBOX
	/* the original Xbox's geometry read from the map when drawn: the cache
	(lruv_cache.c), whose blocks' addresses count from `geometry_cache_base`,
	and whether the texture cache lent its memory; where the model data
	lies in the map; the nodes of the parts whose vertices name their own,
	of models drawn with all their nodes; and counts for the log */
	struct lruv_cache *geometry_cache;
	byte *geometry_bsp_headers;
	unsigned long geometry_bsp_headers_size;
	byte *geometry_cache_base;
	unsigned long geometry_cache_bytes;
	boolean geometry_cache_lent;
	unsigned long model_data_offset;
	unsigned long model_index_data_offset;
	struct part_nodes *part_nodes;
	long part_nodes_count;
	long geometry_cache_ticks;
	long geometries_read;
	unsigned long geometry_bytes_read;
	long geometries_not_drawn;
	long geometries_resident;
	unsigned long geometry_bytes_resident;
#endif

	/* the structure BSP whose materials have buffers, and the compressed
	vertices those were made from */
	struct structure_bsp *structure_bsp;
	byte *structure_bsp_vertices;
	boolean structure_bsp_vertices_contiguous;
};

#ifdef HALO_XBOX
typedef char verify_geometry_header_size[
	sizeof(D3DVertexBuffer) == GEOMETRY_HEADER_SIZE && sizeof(D3DIndexBuffer) == GEOMETRY_HEADER_SIZE ? 1 : -1];
#endif

/* ---------- globals */

static struct custom_edition_geometry_globals custom_edition_geometry_globals;

/* ---------- private code */

#ifdef HALO_XBOX
/* ---------- the original Xbox's room for geometry

The original Xbox (port/xbox) has a few MB of heap and of contiguous memory
free in a map: too little for a Custom Edition map's model data, read whole
by the other builds (up to 54 MB in the maps examined), and for its
geometry twice over (the compressed vertices, then the Direct3D buffers'
copies of them). The geometry goes instead in the Custom Edition tag cache,
between the tags and the lowest structure BSP, which is the Xbox tag
cache's own contiguous pages (port/xbox/src/nxdk_memory.c), had at their
Xbox tag cache address so that the GPU reads them; and the buffers are
drawn from it in place, as an Xbox cache's are (geometry_vertex_buffer_new).
The models' geometry is made first, at the room's start. A structure BSP's
vertices are compressed where they lie in the BSP, at the top of the tag
cache, which is one run of the GPU's memory with the tag cache's pages
(xbox_custom_edition_tag_cache_gpu_bytes); only its buffers' headers follow
the models' geometry, in room kept for them, so that loading a BSP, which
the game cannot survive failing, never runs out of it. While the models are
converted no structure BSP is loaded, so each part's vertices and strip are
read from the map to the top of the tag cache (part_data), and compressed
there. */
#define GEOMETRY_ALIGNMENT 0x1000UL
/* (in a Custom Edition part's padding: its vertices name the model's nodes,
which model_local_nodes_make gave it its own of) */
#define PART_NODES_MADE_HERE 1
/* a permutation's levels of detail: super low, low, medium, high, super high */
#define MEDIUM_LEVEL_OF_DETAIL 2
/* (the game's model geometry block holds no more) */
#define MAXIMUM_GEOMETRIES_PER_MODEL_FOR_REDUCTION 256

/* (port/xbox/src/nxdk_memory.c) */
unsigned long xbox_custom_edition_tag_cache_gpu_bytes(void);

static struct
{
	/* the geometry's room, at the Xbox tag cache's address */
	byte *start;
	byte *end;
	byte *models_end;
	/* the buffers' headers, after the geometry allocated last */
	unsigned long *headers;
	unsigned long *headers_end;
	/* where a part's vertices and strip are read and its vertices
	compressed from, room for the largest part's, at the top of the Custom
	Edition tag cache while the models are converted */
	struct model_vertex_uncompressed *part_vertices;
	struct model_vertex_uncompressed *part_scratch;
	word *part_strip;
} geometry_room;

/* the Xbox tag cache's address of an offset into the Custom Edition tag
cache, whose pages are the tag cache's and the ones after them */
static byte *geometry_room_address(
	unsigned long offset)
{
	return (byte *)physical_memory_get_tag_cache_base_address() + MIN(offset, xbox_custom_edition_tag_cache_gpu_bytes());
}

/* room for the headers of the buffers of any structure BSP's materials */
static unsigned long geometry_room_bsp_headers_size(
	struct custom_edition_load_report const *report)
{
	return (unsigned long)report->structure_bsp_materials_checked * 2 * GEOMETRY_HEADER_SIZE + 16;
}

/* the room's end once the models are converted: the lowest structure BSP */
static unsigned long geometry_room_limit(
	struct custom_edition_load_report const *report)
{
	return report->lowest_structure_bsp_address ?
		report->lowest_structure_bsp_address - CUSTOM_EDITION_TAG_CACHE_ADDRESS :
		report->tag_cache_bytes;
}

/* Sets the room for the models' geometry, below the room for the largest
part of the models of `tag_cache` (`loaded_bytes` of it in use); FALSE,
logged, when there is none. */
static boolean geometry_room_begin(
	byte *tag_cache,
	unsigned long loaded_bytes,
	struct custom_edition_load_report const *report)
{
	unsigned long start = (loaded_bytes + GEOMETRY_ALIGNMENT - 1) & ~(GEOMETRY_ALIGNMENT - 1);
	unsigned long largest_vertex_count = 0;
	unsigned long largest_strip_index_count = 0;
	unsigned long part_bytes;
	unsigned long part_offset;
	struct model *model;
	int32_t tag_index = NONE;

	while ((model = custom_edition_cache_tag_next(tag_cache, loaded_bytes, GBXMODEL_GROUP_TAG, sizeof(*model), &tag_index)) != NULL)
	{
		long geometry_index;

		for (geometry_index = 0; geometry_index < model->geometries.count; geometry_index++)
		{
			struct model_geometry const *geometry = TAG_BLOCK_GET_ELEMENT(
				&model->geometries,
				geometry_index,
				struct model_geometry);
			long part_index;

			for (part_index = 0; part_index < geometry->parts.count; part_index++)
			{
				struct custom_edition_model_part const *part = TAG_BLOCK_GET_ELEMENT(
					&geometry->parts,
					part_index,
					struct custom_edition_model_part);

				largest_vertex_count = MAX(largest_vertex_count, (unsigned long)part->vertex_count);
				largest_strip_index_count = MAX(largest_strip_index_count, (unsigned long)part->strip_triangle_count + 2);
			}
		}
	}
	/* (the loader checked every part's counts against the model data) */
	part_bytes = 2 * largest_vertex_count * sizeof(struct model_vertex_uncompressed) +
		largest_strip_index_count * sizeof(word) + 16;
	part_offset = (report->tag_cache_bytes - MIN(part_bytes, report->tag_cache_bytes)) & ~15UL;
	if (part_bytes > report->tag_cache_bytes || part_offset < start)
	{
		error(_error_silent, "custom edition: no room to read a model part of %lu vertices", largest_vertex_count);
		return FALSE;
	}
	geometry_room.part_vertices = (struct model_vertex_uncompressed *)(tag_cache + part_offset);
	geometry_room.part_scratch = geometry_room.part_vertices + largest_vertex_count;
	geometry_room.part_strip = (word *)(geometry_room.part_scratch + largest_vertex_count);
	geometry_room.start = geometry_room_address(start);
	geometry_room.models_end = geometry_room.start;
	geometry_room.end = geometry_room_address(MIN(part_offset,
		geometry_room_limit(report) - MIN(geometry_room_bsp_headers_size(report), geometry_room_limit(report))));

	return TRUE;
}

/* the room's bytes for the models (none when the tags reach past it) */
static unsigned long geometry_room_models_bytes(
	void)
{
	return geometry_room.end > geometry_room.start ? (unsigned long)(geometry_room.end - geometry_room.start) : 0;
}

/* (port_config.c) */
int config_boolean(char const *name);

/* the compressed bytes of the parts of `geometry` (as geometry_allocate
takes them, with their buffers' headers) */
static unsigned long geometry_bytes(
	struct model_geometry const *geometry)
{
	unsigned long bytes = 0;
	long part_index;

	for (part_index = 0; part_index < geometry->parts.count; part_index++)
	{
		struct custom_edition_model_part const *part = TAG_BLOCK_GET_ELEMENT(
			&geometry->parts,
			part_index,
			struct custom_edition_model_part);

		bytes += part->vertex_count * rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_model_compressed) +
			(part->strip_triangle_count + 2) * sizeof(word) + 2 * GEOMETRY_HEADER_SIZE;
	}

	return bytes;
}

/* When the models of `tag_cache` do not fit the room at their full detail
and game.custom_edition_reduce_detail allows it, draws every permutation's
high and super high levels of detail with its medium geometry, and gives the
geometries no permutation names then no parts, so that they are not
converted. (custom_edition_cache_measure_models measured the same before the
map was let run.) Returns the bytes the models' geometry then takes. */
static unsigned long custom_edition_models_reduce(
	byte *tag_cache,
	unsigned long loaded_bytes)
{
	unsigned long room = geometry_room_models_bytes();
	unsigned long all_bytes = 0;
	unsigned long reduced_bytes = 0;
	long reduced_geometry_count = 0;
	struct model *model;
	int32_t tag_index = NONE;

	while ((model = custom_edition_cache_tag_next(tag_cache, loaded_bytes, GBXMODEL_GROUP_TAG, sizeof(*model), &tag_index)) != NULL)
	{
		long geometry_index;

		for (geometry_index = 0; geometry_index < model->geometries.count; geometry_index++)
		{
			all_bytes += geometry_bytes(TAG_BLOCK_GET_ELEMENT(&model->geometries, geometry_index, struct model_geometry));
		}
	}
	if (all_bytes <= room || !config_boolean("game.custom_edition_reduce_detail"))
	{
		return all_bytes;
	}
	tag_index = NONE;
	while ((model = custom_edition_cache_tag_next(tag_cache, loaded_bytes, GBXMODEL_GROUP_TAG, sizeof(*model), &tag_index)) != NULL)
	{
		byte used[MAXIMUM_GEOMETRIES_PER_MODEL_FOR_REDUCTION];
		long region_index;
		long geometry_index;

		/* (the loader checked every index a permutation holds) */
		if (model->geometries.count > MAXIMUM_GEOMETRIES_PER_MODEL_FOR_REDUCTION)
			continue;
		csmemset(used, 0, sizeof(used));
		for (region_index = 0; region_index < model->regions.count; region_index++)
		{
			struct model_region *region = TAG_BLOCK_GET_ELEMENT(&model->regions, region_index, struct model_region);
			long permutation_index;

			for (permutation_index = 0; permutation_index < region->permutations.count; permutation_index++)
			{
				struct model_region_permutation *permutation = TAG_BLOCK_GET_ELEMENT(
					&region->permutations,
					permutation_index,
					struct model_region_permutation);
				short medium = permutation->geometry_indices[MEDIUM_LEVEL_OF_DETAIL];
				long level;

				if (medium >= 0 && medium < model->geometries.count)
				{
					for (level = MEDIUM_LEVEL_OF_DETAIL + 1; level < NUMBEROF(permutation->geometry_indices); level++)
						permutation->geometry_indices[level] = medium;
				}
				for (level = 0; level < NUMBEROF(permutation->geometry_indices); level++)
				{
					short geometry = permutation->geometry_indices[level];

					if (geometry >= 0 && geometry < model->geometries.count)
						used[geometry] = TRUE;
				}
			}
		}
		for (geometry_index = 0; geometry_index < model->geometries.count; geometry_index++)
		{
			struct model_geometry *geometry = TAG_BLOCK_GET_ELEMENT(&model->geometries, geometry_index, struct model_geometry);

			if (!used[geometry_index])
			{
				geometry->parts.count = 0;
				reduced_geometry_count++;
			}
			reduced_bytes += geometry_bytes(geometry);
		}
	}
	error(_error_silent, "custom edition: the models' 0x%lX bytes of geometry do not fit the Xbox's 0x%lX: drawn at medium detail at most, 0x%lX bytes (%ld geometries left out)",
		all_bytes, room, reduced_bytes, reduced_geometry_count);

	return reduced_bytes;
}

/* the models converted: the room reaches to the lowest structure BSP */
static void geometry_room_models_done(
	struct custom_edition_load_report const *report)
{
	geometry_room.end = geometry_room_address(geometry_room_limit(report));
	geometry_room.part_vertices = NULL;
	geometry_room.part_scratch = NULL;
	geometry_room.part_strip = NULL;

	return;
}

/* (`models`: the models' geometry, else a structure BSP's; and room for
`buffer_count` buffers' headers after it) */
static void *geometry_allocate(
	unsigned long size,
	boolean models,
	long buffer_count,
	boolean *contiguous)
{
	byte *first = models ? geometry_room.start : geometry_room.models_end;
	unsigned long headers_size = (unsigned long)buffer_count * GEOMETRY_HEADER_SIZE;
	byte *geometry = NULL;

	*contiguous = FALSE;
	size = (size + 15) & ~15UL;
	if (first && first <= geometry_room.end && headers_size <= (unsigned long)(geometry_room.end - first) &&
		size <= (unsigned long)(geometry_room.end - first) - headers_size)
	{
		geometry = first;
		geometry_room.headers = (unsigned long *)(first + size);
		geometry_room.headers_end = (unsigned long *)(first + size + headers_size);
		if (models)
			geometry_room.models_end = first + size + headers_size;
	}
	else
	{
		error(_error_silent, "custom edition: no room for 0x%lX bytes of geometry (0x%lX between the tags and the structure BSPs)",
			size + headers_size, first && first <= geometry_room.end ? (unsigned long)(geometry_room.end - first) : 0UL);
	}

	return geometry;
}

static void geometry_free(
	void *geometry,
	boolean contiguous)
{
	/* (the room's: had again by the next map or structure BSP) */
	(void)geometry;
	(void)contiguous;
}

/* the next header geometry_allocate made room for */
static void *geometry_header_next(
	void)
{
	unsigned long *header = geometry_room.headers;

	assert(header && header + GEOMETRY_HEADER_SIZE / sizeof(*header) <= geometry_room.headers_end);
	geometry_room.headers = header + GEOMETRY_HEADER_SIZE / sizeof(*header);

	return header;
}

/* `vertex_buffer` drawn from `vertices`, in place: a header without
D3DCOMMON_D3DCREATED, which Release never frees (d3d8_resources.c) */
static boolean geometry_vertex_buffer_new(
	struct vertex_buffer *vertex_buffer,
	long vertex_type,
	long count,
	void const *vertices,
	long buffer_size)
{
	D3DVertexBuffer *header = geometry_header_next();

	(void)buffer_size;
	header->Common = D3DCOMMON_TYPE_VERTEXBUFFER | 1;
	header->Data = 0;
	header->Lock = 0;
	IDirect3DVertexBuffer8_Register(header, (void *)vertices);
	vertex_buffer->type = (short)vertex_type;
	vertex_buffer->count = count;
	vertex_buffer->offset = 0;
	vertex_buffer->base_address = (void *)vertices;
	vertex_buffer->hardware_format = header;

	return TRUE;
}

/* `triangle_buffer` drawn from the strip `strip`, in place (index buffers'
Data is their memory's own address on the Xbox: d3d8_resources.c) */
static boolean geometry_strip_buffer_new(
	struct triangle_buffer *triangle_buffer,
	short triangle_type,
	long triangle_count,
	void const *strip)
{
	D3DIndexBuffer *header = geometry_header_next();

	header->Common = D3DCOMMON_TYPE_INDEXBUFFER | 1;
	header->Data = (unsigned long)strip;
	header->Lock = 0;
	triangle_buffer->type = triangle_type;
	triangle_buffer->count = triangle_count;
	triangle_buffer->base_address = (void *)strip;
	triangle_buffer->hardware_format = header;

	return TRUE;
}

/* Compresses `count` vertices of `type` (an uncompressed type) from `source`
to `destination` of `compressed_type`, a chunk at a time through a buffer:
`destination` may be `source` itself, as the compressed vertices are smaller
and each chunk is read before any of it is written. */
static void geometry_compress_vertices(
	long type,
	long compressed_type,
	long count,
	byte *destination,
	byte const *source)
{
	static byte chunk[0x4000];
	long size = rasterizer_geometry_get_vertex_size(type);
	long compressed_size = rasterizer_geometry_get_vertex_size(compressed_type);
	long chunk_count = (long)sizeof(chunk) / size;
	long first;

	assert(compressed_size <= size);
	for (first = 0; first < count; first += chunk_count)
	{
		long number = MIN(chunk_count, count - first);

		csmemcpy(chunk, source + first * size, number * size);
		rasterizer_geometry_compress_vertices(
			type,
			number,
			destination + first * compressed_size,
			number * compressed_size,
			chunk,
			number * size);
	}

	return;
}

/* ---------- the original Xbox's geometry read when drawn

A map whose models' geometry does not fit the room, even at medium detail
when custom_edition_models_reduce makes it so, has each model geometry (a
permutation's level of detail) read from the map, compressed and given
buffers when it is first drawn, as Halo PC reads its own models' vertices
into a cache when they are drawn: into a cache of the game's own kind
(lruv_cache.c, the texture cache's), whose blocks go once their room is
wanted and the GPU has drawn from them. The cache is the room, after the
room kept for the structure BSPs' headers, when that is large enough; else
pages the texture cache lends (texture_cache_lend_memory). A geometry the
cache has no room for, every block in the way drawn this frame or not yet by
the GPU, is not drawn this frame (custom_edition_model_geometry_ready). */
#define GEOMETRY_CACHE_SIGNATURE 0x67656F63UL /* 'geoc' */
#define GEOMETRY_CACHE_PAGE_SIZE_BITS 12
#define GEOMETRY_CACHE_MINIMUM_ROOM 0x300000UL
#define GEOMETRY_CACHE_LENT_BYTES 0x400000UL
/* frames between the cache's lines in the log (30 s at 30 fps) */
#define GEOMETRY_CACHE_LOG_FRAMES 900

static unsigned long geometry_cache_round(
	unsigned long size)
{
	return (size + 15) & ~15UL;
}

/* the bytes of the block of `geometry`: its buffers' headers, then each
part's compressed vertices and strip */
static unsigned long geometry_cache_bytes(
	struct model_geometry const *geometry)
{
	unsigned long bytes = geometry_cache_round((unsigned long)geometry->parts.count * 2 * GEOMETRY_HEADER_SIZE);
	long part_index;

	for (part_index = 0; part_index < geometry->parts.count; part_index++)
	{
		struct model_geometry_part const *part = TAG_BLOCK_GET_ELEMENT(
			&geometry->parts,
			part_index,
			struct model_geometry_part);

		bytes += geometry_cache_round((unsigned long)part->vertex_buffer.count *
				rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_model_compressed)) +
			geometry_cache_round((unsigned long)(part->triangle_buffer.count + 2) * sizeof(word));
	}

	return bytes;
}

static struct model_geometry *geometry_cache_block_geometry(
	long block_index)
{
	struct lruv_cache_block const *block = datum_get(
		custom_edition_geometry_globals.geometry_cache->blocks,
		block_index);

	return (struct model_geometry *)block->user_data;
}

/* whether the GPU has yet to draw from the block's vertices */
static boolean geometry_cache_locked_block_proc(
	long block_index)
{
	struct model_geometry const *geometry = geometry_cache_block_geometry(block_index);
	long part_index;

	for (part_index = 0; part_index < geometry->parts.count; part_index++)
	{
		struct model_geometry_part const *part = TAG_BLOCK_GET_ELEMENT(
			&geometry->parts,
			part_index,
			struct model_geometry_part);

		if (part->vertex_buffer.hardware_format &&
			IDirect3DVertexBuffer8_IsBusy((D3DVertexBuffer *)part->vertex_buffer.hardware_format))
		{
			return TRUE;
		}
	}

	return FALSE;
}

/* the block's geometry goes, once the GPU has drawn from it, to be read
again when next drawn */
static void geometry_cache_delete_block_proc(
	long block_index)
{
	struct custom_edition_geometry_globals *globals = &custom_edition_geometry_globals;
	struct model_geometry *geometry = geometry_cache_block_geometry(block_index);
	long part_index;

	for (part_index = 0; part_index < geometry->parts.count; part_index++)
	{
		struct model_geometry_part *part = TAG_BLOCK_GET_ELEMENT(
			&geometry->parts,
			part_index,
			struct model_geometry_part);

		if (part->vertex_buffer.hardware_format)
			IDirect3DVertexBuffer8_BlockUntilNotBusy((D3DVertexBuffer *)part->vertex_buffer.hardware_format);
		part->vertex_buffer.base_address = NULL;
		part->vertex_buffer.hardware_format = NULL;
		part->triangle_buffer.base_address = NULL;
		part->triangle_buffer.hardware_format = NULL;
	}
	geometry->cache_block_index = NONE;
	globals->geometries_resident--;
	globals->geometry_bytes_resident -= geometry_cache_bytes(geometry);

	return;
}

/* Reads the vertices of `part` from the map, a chunk at a time, compressed
to `vertices`, naming the nodes the renderer skins it with, and its strip to
`strip`. */
static void geometry_cache_part_read(
	struct model_geometry_part const *part,
	byte *vertices,
	word *strip)
{
	static struct model_vertex_uncompressed chunk[0x4000 / sizeof(struct model_vertex_uncompressed)];
	struct custom_edition_geometry_globals const *globals = &custom_edition_geometry_globals;
	struct part_source const *source = &part->data.source;
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_model_compressed);
	long vertex_count = part->vertex_buffer.count;
	long strip_index_count = part->triangle_buffer.count + 2;
	long first;
	long index;

	for (first = 0; first < vertex_count; first += (long)NUMBEROF(chunk))
	{
		long number = MIN((long)NUMBEROF(chunk), vertex_count - first);

		custom_edition_cache_read(
			NONE,
			(long)(globals->model_data_offset + source->vertex_offset + (unsigned long)first * sizeof(*chunk)),
			number * (long)sizeof(*chunk),
			chunk);
		if (source->nodes)
		{
			for (index = 0; index < number; index++)
			{
				long slot;

				for (slot = 0; slot < 2; slot++)
				{
					short *node = &chunk[index].nodes[slot];

					if (source->nodes_made_local)
					{
						short local = 0;

						while (local < source->nodes->count && source->nodes->nodes[local] != *node)
							local++;
						*node = local < source->nodes->count ? local : 0;
					}
					else
					{
						*node = *node >= 0 && *node < source->nodes->count ? source->nodes->nodes[*node] : 0;
					}
				}
			}
		}
		rasterizer_geometry_compress_vertices(
			_rasterizer_vertex_type_model_uncompressed,
			number,
			vertices + first * vertex_size,
			number * vertex_size,
			chunk,
			number * (long)sizeof(*chunk));
	}
	custom_edition_cache_read(
		NONE,
		(long)(globals->model_data_offset + globals->model_index_data_offset + source->strip_offset),
		strip_index_count * (long)sizeof(*strip),
		strip);
	/* (checked when the map was loaded: a map changed since names vertex 0) */
	for (index = 0; index < strip_index_count; index++)
	{
		if (strip[index] >= vertex_count)
			strip[index] = 0;
	}

	return;
}

/* Reads `geometry` into a block of the cache, its parts' buffers there;
FALSE when the cache has no room for it this frame. */
static boolean geometry_cache_load(
	struct model_geometry *geometry)
{
	struct custom_edition_geometry_globals *globals = &custom_edition_geometry_globals;
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_model_compressed);
	unsigned long bytes = geometry_cache_bytes(geometry);
	unsigned long headers_size = geometry_cache_round((unsigned long)geometry->parts.count * 2 * GEOMETRY_HEADER_SIZE);
	long block_index = lruv_block_new(globals->geometry_cache, (long)bytes);
	struct lruv_cache_block *block;
	byte *data;
	long part_index;

	if (block_index == NONE)
	{
		globals->geometries_not_drawn++;
		return FALSE;
	}
	block = datum_get(globals->geometry_cache->blocks, block_index);
	block->user_data = (long)geometry;
	data = globals->geometry_cache_base + (unsigned long)lruv_block_get_address(globals->geometry_cache, block_index);
	geometry_room.headers = (unsigned long *)data;
	geometry_room.headers_end = (unsigned long *)(data + headers_size);
	data += headers_size;
	for (part_index = 0; part_index < geometry->parts.count; part_index++)
	{
		struct model_geometry_part *part = TAG_BLOCK_GET_ELEMENT(
			&geometry->parts,
			part_index,
			struct model_geometry_part);
		long vertex_count = part->vertex_buffer.count;
		long triangle_count = part->triangle_buffer.count;
		byte *vertices = data;
		word *strip = (word *)(data + geometry_cache_round((unsigned long)vertex_count * vertex_size));

		data = (byte *)strip + geometry_cache_round((unsigned long)(triangle_count + 2) * sizeof(word));
		geometry_cache_part_read(part, vertices, strip);
		geometry_vertex_buffer_new(&part->vertex_buffer, _rasterizer_vertex_type_model_compressed, vertex_count, vertices, 0);
		geometry_strip_buffer_new(&part->triangle_buffer, _triangle_buffer_type_precompiled_strip, triangle_count, strip);
	}
	geometry->cache_block_index = block_index;
	globals->geometries_read++;
	globals->geometry_bytes_read += bytes;
	globals->geometries_resident++;
	globals->geometry_bytes_resident += bytes;

	return TRUE;
}

/* Makes the cache, between the tags and the lowest structure BSP when there
is room enough, else in pages the texture cache lends; FALSE, logged, when
it cannot be had. The structure BSPs' buffers' headers, which the processor
alone reads, are kept room at its start (geometry_cache_bsp_headers_room),
so that the tags may reach almost to the lowest structure BSP. */
static boolean geometry_cache_new(
	struct custom_edition_load_report const *report)
{
	struct custom_edition_geometry_globals *globals = &custom_edition_geometry_globals;
	unsigned long headers_size = (geometry_room_bsp_headers_size(report) + GEOMETRY_ALIGNMENT - 1) & ~(GEOMETRY_ALIGNMENT - 1);
	byte *first = geometry_room.start;
	byte *last = geometry_room_address(geometry_room_limit(report));
	byte *memory;
	unsigned long bytes;
	long page_count;

	globals->model_data_offset = report->model_data_offset;
	globals->model_index_data_offset = report->model_index_data_offset;
	if (last > first && (unsigned long)(last - first) >= headers_size + GEOMETRY_CACHE_MINIMUM_ROOM)
	{
		memory = first;
		bytes = (unsigned long)(last - first) & ~((1UL << GEOMETRY_CACHE_PAGE_SIZE_BITS) - 1);
		globals->geometry_cache_lent = FALSE;
	}
	else
	{
		bytes = headers_size + GEOMETRY_CACHE_LENT_BYTES;
		memory = texture_cache_lend_memory((long)bytes);
		globals->geometry_cache_lent = memory != NULL;
	}
	globals->geometry_bsp_headers = memory;
	globals->geometry_bsp_headers_size = headers_size;
	globals->geometry_cache_base = memory ? memory + headers_size : NULL;
	globals->geometry_cache_bytes = bytes - headers_size;
	page_count = (long)(globals->geometry_cache_bytes >> GEOMETRY_CACHE_PAGE_SIZE_BITS);
	globals->geometry_cache = globals->geometry_cache_base ?
		lruv_new(
			"custom edition geometry",
			page_count,
			GEOMETRY_CACHE_PAGE_SIZE_BITS,
			page_count,
			geometry_cache_delete_block_proc,
			geometry_cache_locked_block_proc) :
		NULL;
	if (!globals->geometry_cache)
	{
		error(_error_silent, "custom edition: no memory for a cache of model geometry");
		return FALSE;
	}
	globals->geometry_cache_ticks = 0;
	globals->geometries_read = 0;
	globals->geometry_bytes_read = 0;
	globals->geometries_not_drawn = 0;
	globals->geometries_resident = 0;
	globals->geometry_bytes_resident = 0;

	return TRUE;
}

/* the models converted: the structure BSPs' headers go in the room kept for
them at the cache's start */
static void geometry_cache_bsp_headers_room(
	void)
{
	struct custom_edition_geometry_globals const *globals = &custom_edition_geometry_globals;

	geometry_room.models_end = globals->geometry_bsp_headers;
	geometry_room.end = globals->geometry_bsp_headers + globals->geometry_bsp_headers_size;

	return;
}

static void geometry_cache_dispose(
	void)
{
	struct custom_edition_geometry_globals *globals = &custom_edition_geometry_globals;

	if (globals->geometry_cache)
	{
		lruv_flush(globals->geometry_cache);
		lruv_delete(globals->geometry_cache);
		globals->geometry_cache = NULL;
	}
	if (globals->geometry_cache_lent)
	{
		texture_cache_lend_memory(0);
		globals->geometry_cache_lent = FALSE;
	}
	globals->geometry_cache_base = NULL;
	globals->geometry_bsp_headers = NULL;
	globals->geometry_bsp_headers_size = 0;
	if (globals->part_nodes)
	{
		free(globals->part_nodes);
		globals->part_nodes = NULL;
	}
	globals->part_nodes_count = 0;

	return;
}
#endif

/* The vertices and strip of the part `part`, as model_local_nodes_make
leaves them: in `model_data`, the model data the report describes, read
whole; or on the original Xbox, which has no room for it, read from the map
(geometry_room) and their nodes made the part's own again. */
static void part_data(
	struct custom_edition_model_part const *part,
	struct custom_edition_load_report const *report,
	byte *model_data,
	struct model_vertex_uncompressed **vertices,
	word **strip)
{
#ifdef HALO_XBOX
	(void)model_data;
	custom_edition_cache_read(
		NONE,
		(long)(report->model_data_offset + part->vertex_offset),
		part->vertex_count * (long)sizeof(**vertices),
		geometry_room.part_vertices);
	custom_edition_cache_read(
		NONE,
		(long)(report->model_data_offset + report->model_index_data_offset + part->strip_offset),
		(part->strip_triangle_count + 2) * (long)sizeof(**strip),
		geometry_room.part_strip);
	if (part->pad3[0] == PART_NODES_MADE_HERE)
	{
		long vertex_index;

		for (vertex_index = 0; vertex_index < part->vertex_count; vertex_index++)
		{
			long slot;

			for (slot = 0; slot < 2; slot++)
			{
				short *node = &geometry_room.part_vertices[vertex_index].nodes[slot];
				short local = 0;

				while (local < part->local_node_count && part->local_node_indices[local] != *node)
					local++;
				*node = local < part->local_node_count ? local : 0;
			}
		}
	}
	*vertices = geometry_room.part_vertices;
	*strip = geometry_room.part_strip;
#else
	*vertices = (struct model_vertex_uncompressed *)(model_data + part->vertex_offset);
	*strip = (word *)(model_data + report->model_index_data_offset + part->strip_offset);
#endif

	return;
}


static boolean vector_compressible(
	real_vector3d const *vector)
{
	/* a NaN fails every comparison */
	return fabs(vector->i) <= MAXIMUM_COMPRESSIBLE_COMPONENT &&
		fabs(vector->j) <= MAXIMUM_COMPRESSIBLE_COMPONENT &&
		fabs(vector->k) <= MAXIMUM_COMPRESSIBLE_COMPONENT;
}

/* Whether this build can draw the part `part` of `model` (`part_count`
parts in its geometry) from `vertices` and `strip`: every index its fields,
strip and vertices hold names what exists, and every vector compresses.
Node indices are checked as the model data holds them, local to the part
when the model's parts have local nodes. */
static boolean custom_edition_model_part_verify(
	struct model const *model,
	struct custom_edition_model_part const *part,
	long part_count,
	struct model_vertex_uncompressed const *vertices,
	word const *strip)
{
	boolean local_nodes = TEST_FLAG(model->flags, _gbxmodel_parts_have_local_nodes_bit);
	long node_limit = local_nodes ? part->local_node_count : model->nodes.count;
	long strip_index;
	long vertex_index;
	long node_index;

	if (part->shader_index < 0 || part->shader_index >= model->shaders.count ||
		part->centroid_primary_node_index < 0 || part->centroid_primary_node_index >= model->nodes.count ||
		part->centroid_secondary_node_index < 0 || part->centroid_secondary_node_index >= model->nodes.count ||
		part->previous_part_index < NONE || part->previous_part_index >= part_count ||
		part->next_part_index < NONE || part->next_part_index >= part_count ||
		(local_nodes && part->local_node_count == 0))
	{
		return FALSE;
	}
	if (local_nodes)
	{
		for (node_index = 0; node_index < part->local_node_count; node_index++)
		{
			if (part->local_node_indices[node_index] >= model->nodes.count)
			{
				return FALSE;
			}
		}
	}
	for (strip_index = 0; strip_index < part->strip_triangle_count + 2; strip_index++)
	{
		if (strip[strip_index] >= part->vertex_count)
		{
			return FALSE;
		}
	}
	for (vertex_index = 0; vertex_index < part->vertex_count; vertex_index++)
	{
		struct model_vertex_uncompressed const *vertex = &vertices[vertex_index];

		if (vertex->nodes[0] < 0 || vertex->nodes[0] >= node_limit ||
			vertex->nodes[1] < 0 || vertex->nodes[1] >= node_limit ||
			!vector_compressible(&vertex->normal) ||
			!vector_compressible(&vertex->binormal) ||
			!vector_compressible(&vertex->tangent))
		{
			return FALSE;
		}
	}

	return TRUE;
}

/* whether the model has more nodes than the renderer skins at once, and so
is drawn a part's own nodes at a time */
static boolean model_has_many_nodes(
	struct model const *model)
{
	return model->nodes.count >= RASTERIZER_MAXIMUM_NODES_PER_MODEL;
}

/* The nodes the part's vertices name, at most
MAXIMUM_NODES_PER_MODEL_GEOMETRY_PART of them, into `nodes`: their count, or
NONE when there are more, or a vertex names a node the model lacks. */
static long part_nodes_used(
	struct model const *model,
	struct custom_edition_model_part const *part,
	struct model_vertex_uncompressed const *vertices,
	byte nodes[MAXIMUM_NODES_PER_MODEL_GEOMETRY_PART])
{
	long count = 0;
	long vertex_index;

	for (vertex_index = 0; vertex_index < part->vertex_count; vertex_index++)
	{
		long slot;

		for (slot = 0; slot < 2; slot++)
		{
			short node = vertices[vertex_index].nodes[slot];
			long index;

			if (node < 0 || node >= model->nodes.count)
				return NONE;
			for (index = 0; index < count && nodes[index] != node; index++)
				;
			if (index == count)
			{
				if (count == MAXIMUM_NODES_PER_MODEL_GEOMETRY_PART)
					return NONE;
				nodes[count++] = (byte)node;
			}
		}
	}

	return count;
}

/* Halo PC skins a model's nodes all at once; this build's renderer at most
RASTERIZER_MAXIMUM_NODES_PER_MODEL - 1, so a model of more is drawn a part's
own nodes at a time (local nodes). A model of more whose parts have none is
given them here: each part the nodes its vertices name, its vertices naming
them by their place there. FALSE, with nothing changed, when a part's
vertices name more than one part holds. */
static boolean model_local_nodes_make(
	struct model *model,
	struct custom_edition_load_report const *report,
	byte *model_data)
{
	long pass;

	/* (every part checked before any is changed) */
	for (pass = 0; pass < 2; pass++)
	{
		long geometry_index;

		for (geometry_index = 0; geometry_index < model->geometries.count; geometry_index++)
		{
			struct model_geometry const *geometry = TAG_BLOCK_GET_ELEMENT(
				&model->geometries,
				geometry_index,
				struct model_geometry);
			long part_index;

			for (part_index = 0; part_index < geometry->parts.count; part_index++)
			{
				struct custom_edition_model_part *part = TAG_BLOCK_GET_ELEMENT(
					&geometry->parts,
					part_index,
					struct custom_edition_model_part);
				struct model_vertex_uncompressed *vertices;
				word *strip;
				byte nodes[MAXIMUM_NODES_PER_MODEL_GEOMETRY_PART];
				long count;
				long vertex_index;

				part_data(part, report, model_data, &vertices, &strip);
				count = part_nodes_used(model, part, vertices, nodes);

				if (count == NONE)
					return FALSE;
				if (pass == 0)
					continue;
				for (vertex_index = 0; vertex_index < part->vertex_count; vertex_index++)
				{
					long slot;

					for (slot = 0; slot < 2; slot++)
					{
						short local = 0;

						while (nodes[local] != vertices[vertex_index].nodes[slot])
							local++;
						vertices[vertex_index].nodes[slot] = local;
					}
				}
				part->local_node_count = (byte)count;
				csmemcpy(part->local_node_indices, nodes, (size_t)count);
#ifdef HALO_XBOX
				/* (the vertices just changed are a copy: part_data makes
				them the part's own again whenever it reads them) */
				part->pad3[0] = PART_NODES_MADE_HERE;
#endif
			}
		}
	}
	SET_FLAG(model->flags, _gbxmodel_parts_have_local_nodes_bit, TRUE);

	return TRUE;
}

/* Gives each part of `model` that names a shader past the model's shaders
the model's last one, rather than the map refused: bigass_v3's oak tree has
geometries left from a third shader the tag no longer has, which no region
permutation draws, beside the same ones drawn with its leaves (its last
shader). Halo PC reads such a part's shader from past the model's
shaders. A model with no shaders is refused. */
static void custom_edition_model_part_shaders_bound(
	struct model *model,
	char const *name)
{
	long bound = 0;
	long geometry_index;

	if (model->shaders.count < 1)
		return;
	for (geometry_index = 0; geometry_index < model->geometries.count; geometry_index++)
	{
		struct model_geometry const *geometry = TAG_BLOCK_GET_ELEMENT(
			&model->geometries,
			geometry_index,
			struct model_geometry);
		long part_index;

		for (part_index = 0; part_index < geometry->parts.count; part_index++)
		{
			struct custom_edition_model_part *part = TAG_BLOCK_GET_ELEMENT(
				&geometry->parts,
				part_index,
				struct custom_edition_model_part);

			if (part->shader_index < 0 || part->shader_index >= model->shaders.count)
			{
				part->shader_index = (short)(model->shaders.count - 1);
				bound++;
			}
		}
	}
	if (bound)
	{
		error(_error_silent, "custom edition: %ld parts of the model '%s' name shaders it has not got, and are given its last",
			bound, name);
	}

	return;
}

/* Whether this build can draw `model`: every part must pass
custom_edition_model_part_verify, and a model of more nodes than the
renderer skins at once must have local nodes (each part few enough). Adds
what its parts need to `totals`. */
static boolean custom_edition_model_verify(
	struct model const *model,
	char const *name,
	struct custom_edition_load_report const *report,
	byte *model_data,
	struct model_geometry_totals *totals)
{
	long geometry_index;

	if (model->nodes.count < 1 || model->nodes.count > MAXIMUM_NODES_PER_MODEL ||
		(model_has_many_nodes(model) && !TEST_FLAG(model->flags, _gbxmodel_parts_have_local_nodes_bit)))
	{
		error(
			_error_silent,
			"custom edition: the model '%s' has %ld nodes; this build draws models of 1 to %d, or up to %d whose parts have local nodes",
			name,
			model->nodes.count,
			RASTERIZER_MAXIMUM_NODES_PER_MODEL - 1,
			MAXIMUM_NODES_PER_MODEL);
		return FALSE;
	}
	for (geometry_index = 0; geometry_index < model->geometries.count; geometry_index++)
	{
		struct model_geometry const *geometry = TAG_BLOCK_GET_ELEMENT(
			&model->geometries,
			geometry_index,
			struct model_geometry);
		long part_index;

		for (part_index = 0; part_index < geometry->parts.count; part_index++)
		{
			struct custom_edition_model_part const *part = TAG_BLOCK_GET_ELEMENT(
				&geometry->parts,
				part_index,
				struct custom_edition_model_part);
			struct model_vertex_uncompressed *vertices;
			word *strip;

			part_data(part, report, model_data, &vertices, &strip);
			if (!custom_edition_model_part_verify(
				model,
				part,
				geometry->parts.count,
				vertices,
				strip))
			{
				error(
					_error_silent,
					"custom edition: part %ld of geometry %ld of the model '%s' names a node, vertex, shader or part that does not exist, or has a vector this build cannot compress",
					part_index,
					geometry_index,
					name);
				return FALSE;
			}
			totals->part_count++;
			if (model_has_many_nodes(model))
				totals->many_node_part_count++;
			else if (TEST_FLAG(model->flags, _gbxmodel_parts_have_local_nodes_bit))
				totals->local_node_part_count++;
			totals->vertex_count += part->vertex_count;
			totals->strip_index_count += part->strip_triangle_count + 2;
			totals->largest_part_vertex_count = MAX(totals->largest_part_vertex_count, part->vertex_count);
		}
	}

	return TRUE;
}

/* The model's node for a node the part `part` names by its index among its
local nodes; an index past them is left as it is. */
static short part_model_node(
	struct custom_edition_model_part const *part,
	short node_index)
{
	if (node_index >= 0 && node_index < part->local_node_count)
	{
		return part->local_node_indices[node_index];
	}

	return node_index;
}

/* `part` this build's part for the Custom Edition part `source`, without
buffers yet */
static void model_part_header_convert(
	struct model_geometry_part *part,
	struct custom_edition_model_part const *source)
{
	part->flags = source->flags & ~FLAG(_model_geometry_part_local_nodes_bit);
	part->shader_index = source->shader_index;
	part->previous_part_index = source->previous_part_index;
	part->next_part_index = source->next_part_index;
	part->centroid_primary_node_index = source->centroid_primary_node_index;
	part->centroid_secondary_node_index = source->centroid_secondary_node_index;
	part->centroid_primary_node_weight = source->centroid_primary_node_weight;
	part->centroid_secondary_node_weight = source->centroid_secondary_node_weight;
	part->centroid = source->centroid;
	/* empty, as in Xbox caches (whatever the map held: the game draws from
	the buffers alone) */
	csmemset(&part->data, 0, sizeof(part->data));
	csmemset(&part->triangle_buffer, 0, sizeof(part->triangle_buffer));
	csmemset(&part->vertex_buffer, 0, sizeof(part->vertex_buffer));

	return;
}

#ifdef HALO_XBOX
/* Makes `part` this build's part for the Custom Edition part `source`, its
geometry read from the map when drawn (geometry_cache_load): buffers of its
counts without memory, and where its vertices and strip lie, their nodes
named again through `nodes` (part_source). */
static void custom_edition_model_part_defer(
	struct model_geometry_part *part,
	struct custom_edition_model_part const *source,
	struct part_nodes const *nodes,
	boolean nodes_made_local)
{
	model_part_header_convert(part, source);
	part->data.source.vertex_offset = source->vertex_offset;
	part->data.source.strip_offset = source->strip_offset;
	part->data.source.nodes = nodes;
	part->data.source.nodes_made_local = nodes_made_local;
	part->vertex_buffer.type = _rasterizer_vertex_type_model_compressed;
	part->vertex_buffer.count = source->vertex_count;
	part->triangle_buffer.type = _triangle_buffer_type_precompiled_strip;
	part->triangle_buffer.count = source->strip_triangle_count;

	return;
}
#endif

/* Makes `part` this build's part for the Custom Edition part `source`,
compressing its vertices (by way of `scratch`, room for all of them) to
`vertices` and copying its strip to `strip`, and gives it buffers. Its
vertices name the model's nodes when local_nodes, else as they are. */
static boolean custom_edition_model_part_convert(
	struct model_geometry_part *part,
	struct custom_edition_model_part const *source,
	boolean local_nodes,
	struct model_vertex_uncompressed const *source_vertices,
	word const *source_strip,
	struct model_vertex_uncompressed *scratch,
	struct model_vertex_compressed *vertices,
	word *strip)
{
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_model_compressed);
	long uncompressed_vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_model_uncompressed);
	long strip_index_count = source->strip_triangle_count + 2;

	csmemcpy(scratch, source_vertices, source->vertex_count * uncompressed_vertex_size);
	if (local_nodes)
	{
		long vertex_index;

		for (vertex_index = 0; vertex_index < source->vertex_count; vertex_index++)
		{
			short *nodes = scratch[vertex_index].nodes;

			nodes[0] = source->local_node_indices[nodes[0]];
			nodes[1] = source->local_node_indices[nodes[1]];
		}
	}
	rasterizer_geometry_compress_vertices(
		_rasterizer_vertex_type_model_uncompressed,
		source->vertex_count,
		vertices,
		source->vertex_count * vertex_size,
		scratch,
		source->vertex_count * uncompressed_vertex_size);
	csmemcpy(strip, source_strip, strip_index_count * sizeof(*strip));
	model_part_header_convert(part, source);

	return GEOMETRY_VERTEX_BUFFER_NEW(
			&part->vertex_buffer,
			_rasterizer_vertex_type_model_compressed,
			source->vertex_count,
			vertices,
			source->vertex_count * vertex_size) &&
		GEOMETRY_TRIANGLE_BUFFER_NEW(
			&part->triangle_buffer,
			_triangle_buffer_type_precompiled_strip,
			source->strip_triangle_count,
			strip);
}

/* Converts every part of `model`, repacking each geometry's parts from
Custom Edition's size to this build's in place, and records them in the
globals; the geometry goes to `*vertices` and `*strips`, which advance, or
on the original Xbox is read when drawn when `on_demand`. */
static boolean custom_edition_model_convert(
	struct model *model,
	struct custom_edition_load_report const *report,
	byte *model_data,
	struct model_vertex_uncompressed *scratch,
	struct model_vertex_compressed **vertices,
	word **strips,
	boolean on_demand)
{
	struct custom_edition_geometry_globals *globals = &custom_edition_geometry_globals;
	boolean local_nodes = TEST_FLAG(model->flags, _gbxmodel_parts_have_local_nodes_bit);
	/* (the renderer skins with the model's nodes, or a part's own when the
	model has more than it skins at once) */
	boolean part_palettes = model_has_many_nodes(model);
	long geometry_index;

	for (geometry_index = 0; geometry_index < model->geometries.count; geometry_index++)
	{
		struct model_geometry *geometry = TAG_BLOCK_GET_ELEMENT(
			&model->geometries,
			geometry_index,
			struct model_geometry);
		long part_index;

#ifdef HALO_XBOX
		geometry->cache_signature = on_demand && geometry->parts.count ? GEOMETRY_CACHE_SIGNATURE : 0;
		geometry->cache_block_index = NONE;
#endif
		/* a part is never written over a Custom Edition part not yet read:
		this build's parts are smaller, and each is read first */
		for (part_index = 0; part_index < geometry->parts.count; part_index++)
		{
			struct custom_edition_model_part source = *TAG_BLOCK_GET_ELEMENT(
				&geometry->parts,
				part_index,
				struct custom_edition_model_part);
			struct model_geometry_part *part = TAG_BLOCK_GET_ELEMENT(
				&geometry->parts,
				part_index,
				struct model_geometry_part);
			struct model_vertex_uncompressed *source_vertices = NULL;
			word *source_strip = NULL;
			struct part_nodes const *nodes = NULL;
			boolean nodes_made_local = FALSE;

			if (!on_demand)
				part_data(&source, report, model_data, &source_vertices, &source_strip);
			globals->model_parts[globals->model_part_count++] = part;
			/* a part with local nodes names its centroid's nodes among them
			too, and the renderer places a transparent part by them */
			if (local_nodes)
			{
				source.centroid_primary_node_index = part_model_node(&source, source.centroid_primary_node_index);
				source.centroid_secondary_node_index = part_model_node(&source, source.centroid_secondary_node_index);
			}
			if (part_palettes)
			{
				struct part_palette *palette = &globals->palettes[globals->palette_count++];

				palette->vertex_buffer = &part->vertex_buffer;
				palette->nodes.count = source.local_node_count;
				csmemcpy(palette->nodes.nodes, source.local_node_indices, sizeof(palette->nodes.nodes));
#ifdef HALO_XBOX
				/* (the map's vertices name the model's nodes: part_data) */
				if (source.pad3[0] == PART_NODES_MADE_HERE)
				{
					nodes = &palette->nodes;
					nodes_made_local = TRUE;
				}
#endif
			}
#ifdef HALO_XBOX
			else if (on_demand && local_nodes)
			{
				struct part_nodes *own = &globals->part_nodes[globals->part_nodes_count++];

				own->count = source.local_node_count;
				csmemcpy(own->nodes, source.local_node_indices, sizeof(own->nodes));
				nodes = own;
			}
			if (on_demand)
			{
				custom_edition_model_part_defer(part, &source, nodes, nodes_made_local);
				continue;
			}
#else
			(void)nodes;
			(void)nodes_made_local;
#endif
			if (!custom_edition_model_part_convert(
				part,
				&source,
				local_nodes && !part_palettes,
				source_vertices,
				source_strip,
				scratch,
				*vertices,
				*strips))
			{
				return FALSE;
			}
			*vertices += source.vertex_count;
			*strips += source.strip_triangle_count + 2;
		}
	}
	/* the parts' node indices are now the model's, or their own as the
	palettes say: the game's code knows nothing of local nodes */
	model->flags &= ~FLAG(_gbxmodel_parts_have_local_nodes_bit);

	return TRUE;
}

static void structure_bsp_buffers_release(
	struct structure_bsp *structure_bsp)
{
	long lightmap_index;

	for (lightmap_index = 0; lightmap_index < structure_bsp->lightmaps.count; lightmap_index++)
	{
		struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(
			&structure_bsp->lightmaps,
			lightmap_index,
			struct structure_lightmap);
		long material_index;

		for (material_index = 0; material_index < lightmap->materials.count; material_index++)
		{
			struct structure_material *material = TAG_BLOCK_GET_ELEMENT(
				&lightmap->materials,
				material_index,
				struct structure_material);

			rasterizer_vertex_buffer_delete(&material->vertices);
			rasterizer_vertex_buffer_delete(&material->lightmap_vertices);
		}
	}

	return;
}

/* Gives `material` compressed vertices at `vertices` and buffers made from
them. Its uncompressed vertices (cache_file_formats.c checked their size and
place) stay where they are. */
/* geometry the renderer draws from: in the Xbox's contiguous memory, as the
game's own vertex and index buffers are (physical_memory_map.c), which the
renderer keeps on the GPU (d3d8_gl.c's mirror: anything outside it is sent
again at every draw); in the game's heap when that memory is spent */
#ifndef HALO_XBOX
static void *geometry_allocate(
	unsigned long size,
	boolean models,
	long buffer_count,
	boolean *contiguous)
{
	void *geometry = XPhysicalAlloc(size, (unsigned long)-1, 0, PAGE_READWRITE);

	(void)models;
	(void)buffer_count;

	*contiguous = geometry != NULL;
	if (!geometry)
	{
		error(_error_silent, "custom edition: 0x%lX bytes of geometry drawn from outside contiguous memory (slower)",
			size);
		geometry = system_malloc(size);
	}

	return geometry;
}

static void geometry_free(
	void *geometry,
	boolean contiguous)
{
	if (contiguous)
		XPhysicalFree(geometry);
	else
		system_free(geometry);
}
#endif

static boolean structure_material_convert(
	struct structure_material *material,
	byte *vertices)
{
	long vertex_count = material->vertices.count;
	long lightmap_vertex_count = material->lightmap_vertices.count;
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_compressed);
	long lightmap_vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_lightmap_compressed);
	byte *uncompressed_vertices = XBOX_POINTER(byte, material->uncompressed_vertex_data.address);
	byte *lightmap_vertices = vertices + vertex_count * vertex_size;
	boolean success = TRUE;

	/* the lightmap vertices follow the environment vertices, as object
	lights and the structure's point queries expect of the compressed ones */
	if (vertex_count)
	{
#ifdef HALO_XBOX
		/* (in place: the original Xbox's are compressed where they lie) */
		geometry_compress_vertices(
			_rasterizer_vertex_type_environment_uncompressed,
			_rasterizer_vertex_type_environment_compressed,
			vertex_count,
			vertices,
			uncompressed_vertices);
#else
		rasterizer_geometry_compress_vertices(
			_rasterizer_vertex_type_environment_uncompressed,
			vertex_count,
			vertices,
			vertex_count * vertex_size,
			uncompressed_vertices,
			vertex_count * rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_uncompressed));
#endif
		success = GEOMETRY_VERTEX_BUFFER_NEW(
			&material->vertices,
			_rasterizer_vertex_type_environment_compressed,
			vertex_count,
			vertices,
			vertex_count * vertex_size);
	}
	if (success && lightmap_vertex_count)
	{
#ifdef HALO_XBOX
		geometry_compress_vertices(
			_rasterizer_vertex_type_environment_lightmap_uncompressed,
			_rasterizer_vertex_type_environment_lightmap_compressed,
			lightmap_vertex_count,
			lightmap_vertices,
			uncompressed_vertices + vertex_count * rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_uncompressed));
#else
		rasterizer_geometry_compress_vertices(
			_rasterizer_vertex_type_environment_lightmap_uncompressed,
			lightmap_vertex_count,
			lightmap_vertices,
			lightmap_vertex_count * lightmap_vertex_size,
			uncompressed_vertices + vertex_count * rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_uncompressed),
			lightmap_vertex_count * rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_lightmap_uncompressed));
#endif
		success = GEOMETRY_VERTEX_BUFFER_NEW(
			&material->lightmap_vertices,
			_rasterizer_vertex_type_environment_lightmap_compressed,
			lightmap_vertex_count,
			lightmap_vertices,
			lightmap_vertex_count * lightmap_vertex_size);
	}
	material->compressed_vertex_data.size = vertex_count * vertex_size + lightmap_vertex_count * lightmap_vertex_size;
	material->compressed_vertex_data.address = XBOX_ADDRESS(vertices);

	return success;
}

/* ---------- public code */

boolean custom_edition_models_convert(
	byte *tag_cache,
	unsigned long loaded_bytes,
	struct custom_edition_load_report const *report,
	byte *model_data)
{
	struct custom_edition_geometry_globals *globals = &custom_edition_geometry_globals;
	struct model_geometry_totals totals = { 0, 0, 0, 0, 0, 0 };
	struct model_vertex_uncompressed *scratch;
	struct model_vertex_compressed *vertices = NULL;
	struct model *model;
	word *strips = NULL;
	int32_t tag_index = NONE;
	boolean success = TRUE;
	boolean on_demand = FALSE;
	boolean geometry_had;

	assert(!globals->model_parts && !globals->model_geometry);
#ifdef HALO_XBOX
	if (!geometry_room_begin(tag_cache, loaded_bytes, report))
		return FALSE;
	/* (with room for geometry_allocate's rounding) */
	on_demand = custom_edition_models_reduce(tag_cache, loaded_bytes) + 32 > geometry_room_models_bytes();
#endif
	/* (every model's local nodes are made before any model is verified:
	making them writes node indices into the model data, and parts of
	different models may name the same vertices there, so a model verified
	earlier could otherwise be drawn from vertices changed after its check) */
	while ((model = custom_edition_cache_tag_next(tag_cache, loaded_bytes, GBXMODEL_GROUP_TAG, sizeof(*model), &tag_index)) != NULL)
	{
		if (model_has_many_nodes(model) && !TEST_FLAG(model->flags, _gbxmodel_parts_have_local_nodes_bit) &&
			model->nodes.count <= MAXIMUM_NODES_PER_MODEL && model_local_nodes_make(model, report, model_data))
		{
			error(_error_silent, "custom edition: the model '%s', of %ld nodes, is drawn a part's nodes at a time",
				custom_edition_cache_tag_name(tag_cache, loaded_bytes, tag_index), model->nodes.count);
		}
	}
	tag_index = NONE;
	while ((model = custom_edition_cache_tag_next(tag_cache, loaded_bytes, GBXMODEL_GROUP_TAG, sizeof(*model), &tag_index)) != NULL)
	{
		custom_edition_model_part_shaders_bound(model, custom_edition_cache_tag_name(tag_cache, loaded_bytes, tag_index));
		if (!custom_edition_model_verify(
			model,
			custom_edition_cache_tag_name(tag_cache, loaded_bytes, tag_index),
			report,
			model_data,
			&totals))
		{
			return FALSE;
		}
	}

	globals->model_parts = malloc((totals.part_count + 1) * sizeof(*globals->model_parts));
	globals->palettes = malloc((totals.many_node_part_count + 1) * sizeof(*globals->palettes));
#ifdef HALO_XBOX
	if (on_demand)
	{
		globals->part_nodes = malloc((totals.local_node_part_count + 1) * sizeof(*globals->part_nodes));
		geometry_had = globals->part_nodes && geometry_cache_new(report);
	}
	else
#endif
	{
		globals->model_geometry = geometry_allocate(
			totals.vertex_count * rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_model_compressed) +
			totals.strip_index_count * sizeof(*strips) + 1,
			TRUE,
			totals.part_count * 2,
			&globals->model_geometry_contiguous);
		geometry_had = globals->model_geometry != NULL;
		vertices = (struct model_vertex_compressed *)globals->model_geometry;
		strips = (word *)(vertices + totals.vertex_count);
	}
#ifdef HALO_XBOX
	scratch = geometry_room.part_scratch;
#else
	scratch = malloc(totals.largest_part_vertex_count * sizeof(*scratch) + 1);
#endif
	if (!globals->model_parts || !geometry_had || !globals->palettes || !scratch)
	{
		error(_error_silent, "custom edition: out of memory for the geometry of %ld model parts", totals.part_count);
		/* (the game's free, debug_free, does not take NULL) */
#ifndef HALO_XBOX
		if (scratch)
			free(scratch);
#endif
		return FALSE;
	}

	tag_index = NONE;
	while (success &&
		(model = custom_edition_cache_tag_next(tag_cache, loaded_bytes, GBXMODEL_GROUP_TAG, sizeof(*model), &tag_index)) != NULL)
	{
		success = custom_edition_model_convert(model, report, model_data, scratch, &vertices, &strips, on_demand);
		if (!success)
		{
			error(
				_error_silent,
				"custom edition: cannot make the buffers of the model '%s'",
				custom_edition_cache_tag_name(tag_cache, loaded_bytes, tag_index));
		}
	}
#ifdef HALO_XBOX
	geometry_room_models_done(report);
	if (on_demand)
		geometry_cache_bsp_headers_room();
#else
	free(scratch);
#endif
	if (success)
	{
		custom_edition_cache_tags_regroup(tag_cache, loaded_bytes, GBXMODEL_GROUP_TAG, MODELS_GROUP_TAG);
		error(
			_error_silent,
			"custom edition: %ld model parts converted (%ld vertices compressed, %ld parts drawn with their own nodes)",
			totals.part_count,
			totals.vertex_count,
			totals.many_node_part_count);
#ifdef HALO_XBOX
		if (on_demand)
		{
			error(
				_error_silent,
				"custom edition: the models' geometry is read from the map when drawn, into a cache of %lu KB %s",
				globals->geometry_cache_bytes >> 10,
				globals->geometry_cache_lent ? "lent by the texture cache" : "beside the tags");
		}
#endif
	}

	return success;
}

short custom_edition_part_palette(
	struct vertex_buffer const *vertex_buffer,
	byte const **nodes)
{
	struct custom_edition_geometry_globals *globals = &custom_edition_geometry_globals;
	long index;

	for (index = 0; index < globals->palette_count; index++)
	{
		if (globals->palettes[index].vertex_buffer == vertex_buffer)
		{
			*nodes = globals->palettes[index].nodes.nodes;
			return globals->palettes[index].nodes.count;
		}
	}
	return 0;
}

unsigned char custom_edition_model_geometry_ready(
	struct model_geometry *geometry)
{
#ifdef HALO_XBOX
	struct custom_edition_geometry_globals *globals = &custom_edition_geometry_globals;

	if (!globals->geometry_cache || geometry->cache_signature != GEOMETRY_CACHE_SIGNATURE)
	{
		return TRUE;
	}
	if (geometry->cache_block_index != NONE)
	{
		lruv_block_touch(globals->geometry_cache, geometry->cache_block_index);
		return TRUE;
	}

	return geometry_cache_load(geometry);
#else
	(void)geometry;

	return TRUE;
#endif
}

void custom_edition_geometry_idle(
	void)
{
#ifdef HALO_XBOX
	struct custom_edition_geometry_globals *globals = &custom_edition_geometry_globals;

	if (!globals->geometry_cache)
	{
		return;
	}
	lruv_idle(globals->geometry_cache);
	if (++globals->geometry_cache_ticks % GEOMETRY_CACHE_LOG_FRAMES == 0 &&
		(globals->geometries_read || globals->geometries_not_drawn))
	{
		error(
			_error_silent,
			"custom edition: geometry cache: %ld geometries in %lu of its %lu KB; in the last %d frames %ld read (%lu KB), %ld not drawn for want of room",
			globals->geometries_resident,
			globals->geometry_bytes_resident >> 10,
			globals->geometry_cache_bytes >> 10,
			GEOMETRY_CACHE_LOG_FRAMES,
			globals->geometries_read,
			globals->geometry_bytes_read >> 10,
			globals->geometries_not_drawn);
		globals->geometries_read = 0;
		globals->geometry_bytes_read = 0;
		globals->geometries_not_drawn = 0;
	}
#endif

	return;
}

void custom_edition_models_dispose(
	void)
{
	struct custom_edition_geometry_globals *globals = &custom_edition_geometry_globals;
	long part_index;

#ifdef HALO_XBOX
	/* (the geometry read when drawn goes first, once the GPU has drawn it) */
	geometry_cache_dispose();
#endif
	for (part_index = 0; part_index < globals->model_part_count; part_index++)
	{
		rasterizer_triangle_buffer_delete(&globals->model_parts[part_index]->triangle_buffer);
		rasterizer_vertex_buffer_delete(&globals->model_parts[part_index]->vertex_buffer);
	}
	/* the game's free stops on NULL (cseries.h), and a map can fail before
	its models are converted */
	if (globals->model_parts)
	{
		free(globals->model_parts);
	}
	if (globals->model_geometry)
	{
		geometry_free(globals->model_geometry, globals->model_geometry_contiguous);
	}
	if (globals->palettes)
	{
		free(globals->palettes);
	}
	globals->model_parts = NULL;
	globals->model_part_count = 0;
	globals->model_geometry = NULL;
	globals->palettes = NULL;
	globals->palette_count = 0;

	return;
}

boolean custom_edition_structure_bsp_load(
	struct structure_bsp *structure_bsp)
{
	struct custom_edition_geometry_globals *globals = &custom_edition_geometry_globals;
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_compressed);
	long lightmap_vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_lightmap_compressed);
	unsigned long vertices_size = 0;
	unsigned long vertices_offset = 0;
	boolean success = TRUE;
	long material_count = 0;
	long lightmap_index;

	assert(!globals->structure_bsp);
	/* the Custom Edition buffer fields name nothing in this process: every
	material starts with none, so that a failure releases only what was
	made (cache_file_formats.c checked the counts and sizes) */
	for (lightmap_index = 0; lightmap_index < structure_bsp->lightmaps.count; lightmap_index++)
	{
		struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(
			&structure_bsp->lightmaps,
			lightmap_index,
			struct structure_lightmap);
		long material_index;

		for (material_index = 0; material_index < lightmap->materials.count; material_index++)
		{
			struct structure_material *material = TAG_BLOCK_GET_ELEMENT(
				&lightmap->materials,
				material_index,
				struct structure_material);
			long vertex_count = material->vertices.count;
			long lightmap_vertex_count = material->lightmap_vertices.count;

			csmemset(&material->vertices, 0, sizeof(material->vertices));
			csmemset(&material->lightmap_vertices, 0, sizeof(material->lightmap_vertices));
			material->vertices.type = _rasterizer_vertex_type_environment_compressed;
			material->vertices.count = vertex_count;
			material->lightmap_vertices.type = _rasterizer_vertex_type_environment_lightmap_compressed;
			material->lightmap_vertices.count = lightmap_vertex_count;
			vertices_size += vertex_count * vertex_size + lightmap_vertex_count * lightmap_vertex_size;
			material_count++;
		}
	}

	globals->structure_bsp = structure_bsp;
	/* (in Xbox memory either way: the material points at them by Xbox address) */
#ifdef HALO_XBOX
	/* (the headers alone: the vertices are compressed in place) */
	globals->structure_bsp_vertices = geometry_allocate(1,
		FALSE,
		material_count * 2,
		&globals->structure_bsp_vertices_contiguous);
#else
	globals->structure_bsp_vertices = geometry_allocate(vertices_size + 1,
		FALSE,
		material_count * 2,
		&globals->structure_bsp_vertices_contiguous);
#endif
	if (!globals->structure_bsp_vertices)
	{
		error(_error_silent, "custom edition: out of memory for 0x%lX bytes of structure BSP vertices", vertices_size);
		custom_edition_structure_bsp_unload();
		return FALSE;
	}
	for (lightmap_index = 0; success && lightmap_index < structure_bsp->lightmaps.count; lightmap_index++)
	{
		struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(
			&structure_bsp->lightmaps,
			lightmap_index,
			struct structure_lightmap);
		long material_index;

		for (material_index = 0; success && material_index < lightmap->materials.count; material_index++)
		{
			struct structure_material *material = TAG_BLOCK_GET_ELEMENT(
				&lightmap->materials,
				material_index,
				struct structure_material);

#ifdef HALO_XBOX
			/* (where its uncompressed vertices lie, at the Xbox tag cache's
			address, which the GPU reads) */
			success = structure_material_convert(material, geometry_room_address(
				(unsigned long)XBOX_POINTER(byte, material->uncompressed_vertex_data.address) - CUSTOM_EDITION_TAG_CACHE_ADDRESS));
#else
			success = structure_material_convert(material, globals->structure_bsp_vertices + vertices_offset);
#endif
			vertices_offset += material->compressed_vertex_data.size;
		}
	}
	if (success)
	{
		error(
			_error_silent,
			"custom edition: the structure BSP lit by bitmap tag 0x%08lX has its vertices compressed (0x%lX bytes)",
			(unsigned long)structure_bsp->lightmap_group.index,
			vertices_size);
	}
	else
	{
		error(_error_silent, "custom edition: cannot make the buffers of the structure BSP's materials");
		custom_edition_structure_bsp_unload();
	}

	return success;
}

void custom_edition_structure_bsp_unload(
	void)
{
	struct custom_edition_geometry_globals *globals = &custom_edition_geometry_globals;

	if (globals->structure_bsp)
	{
		structure_bsp_buffers_release(globals->structure_bsp);
		geometry_free(globals->structure_bsp_vertices, globals->structure_bsp_vertices_contiguous);
		globals->structure_bsp = NULL;
		globals->structure_bsp_vertices = NULL;
	}

	return;
}
