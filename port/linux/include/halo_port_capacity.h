/*
HALO_PORT_CAPACITY.H

Memory capacity of the native builds (Windows, Linux, Android), sized for the
session limits in halo_port_limits.h, which includes this file. The Xbox
sizes are given in parentheses below.

Every machine in a session must be built with the same values: the
distributed netcode names objects and players by their datum index, the
same on every machine (port/linux/game/network_objects.c tracks
MAXIMUM_TRACKED_OBJECTS = HALO_PORT_MAXIMUM_OBJECTS_PER_MAP objects, and a
client's own objects take the upper half of the object array).
*/

#ifndef __HALO_PORT_CAPACITY_H
#define __HALO_PORT_CAPACITY_H

/* ---------- game state

The Xbox game state is 0x345000 bytes at 0x80061000 and ends where the tag
cache begins (0x803A6000). Cache files are linked to that tag cache address,
so the game state cannot grow in place. The native builds put a 16 MB game
state above the tag cache (which ends at 0x819A6000), inside the Xbox memory
window (0x80000000-0x88000000, port/linux/src/platform.h) and below everything
the window hands out top-down (texture and sound caches, Direct3D resources).

The CPU part holds about 13.6 MB of pools at the sizes below (the Xbox pools
fill 3,165,260 of its 0x305000 bytes); the GPU part holds only the decal
vertices, as on the Xbox. */

#ifdef HALO_NSPIRE
/* The TI-Nspire (port/nspire/README.md) has 64 MB in all and plays one
campaign level on one machine: the Xbox's own capacities, and the game state
where the Xbox has it, just below the tag cache. */

#define HALO_PORT_GAME_STATE_BASE_ADDRESS 0x80061000
#define HALO_PORT_GAME_STATE_CPU_SIZE 0x305000
#define HALO_PORT_GAME_STATE_GPU_SIZE 0x40000
#define HALO_PORT_GAME_STATE_SIZE (HALO_PORT_GAME_STATE_CPU_SIZE+HALO_PORT_GAME_STATE_GPU_SIZE)

#define HALO_PORT_MAXIMUM_OBJECTS_PER_MAP 2048
#define HALO_PORT_OBJECT_MEMORY_POOL_SIZE 0x100000
#define HALO_PORT_MAXIMUM_CLUSTER_REFERENCES 2048
#define HALO_PORT_MAXIMUM_RENDERED_OBJECTS 256
/* objects narrower than this, in pixels of the game's 640-wide screen, are
not drawn (source/render/render_objects.c) */
#define NSPIRE_MINIMUM_OBJECT_PIXELS 20.f
#define NSPIRE_MINIMUM_SCENERY_PIXELS 160.f
/* scenery this large (bounding radius, in world units of ten feet) is a
landmark: held to NSPIRE_MINIMUM_OBJECT_PIXELS instead */
#define NSPIRE_LANDMARK_RADIUS 2.f
/* objects drawn narrower than this (of 640) are animated every other tick,
as those not drawn are (source/objects/objects.c) */
#define NSPIRE_SMALL_OBJECT_PIXELS 64
/* a model's size in pixels times this picks its detail level
(source/models/models.c): the world is drawn 160 wide, a quarter of the 640
the levels were chosen for, and half that again for speed (models' draws
were 40% of a frame in the beach battle) */
#define NSPIRE_MODEL_DETAIL_SCALE 0.125f
/* game ticks run per drawn frame (source/game/game_time.c): 1 is a stopgap
that halves the game's speed for a quicker frame; 2 or more once ticks
are cheaper (port/nspire/README.md, "Compromises") */
#define NSPIRE_TICKS_PER_FRAME 1
#define HALO_PORT_MAXIMUM_CACHED_OBJECT_RENDER_STATES 256
#define HALO_PORT_MAXIMUM_AREA_OF_EFFECT_OBJECTS 64
#define HALO_PORT_MAXIMUM_LISTED_OBJECTS_PER_MAP 128
#define HALO_PORT_MAXIMUM_EFFECTS 256
#define HALO_PORT_MAXIMUM_EFFECT_LOCATIONS 512
#define HALO_PORT_MAXIMUM_PARTICLES 1024
#define HALO_PORT_MAXIMUM_PARTICLE_SYSTEMS 64
#define HALO_PORT_MAXIMUM_SYSTEM_PARTICLES 512
#define HALO_PORT_MAXIMUM_CONTRAILS 256
#define HALO_PORT_MAXIMUM_CONTRAIL_POINTS 1024
#define HALO_PORT_MAXIMUM_LIGHTS_PER_MAP 896
#define HALO_PORT_MAXIMUM_GAME_LOOPING_SOUNDS 1024

/* The tag cache is paged in from compressed blocks (port/nspire/src/
nspire_paging.c) and takes no memory of its own. The texture cache holds the
converted map's small textures (tools/nspire_map.py); there is no sound. */
#define HALO_PORT_TEXTURE_CACHE_SIZE 0x200000
#define HALO_PORT_SOUND_CACHE_SIZE 0x40000

#else

#define HALO_PORT_GAME_STATE_BASE_ADDRESS 0x81A00000 /* (0x80061000) */
#define HALO_PORT_GAME_STATE_CPU_SIZE 0xFC0000 /* (0x305000) */
#define HALO_PORT_GAME_STATE_GPU_SIZE 0x40000 /* (0x40000) */
#define HALO_PORT_GAME_STATE_SIZE (HALO_PORT_GAME_STATE_CPU_SIZE+HALO_PORT_GAME_STATE_GPU_SIZE)

/* ---------- objects */

#define HALO_PORT_MAXIMUM_OBJECTS_PER_MAP 8192 /* (2048) */
#define HALO_PORT_OBJECT_MEMORY_POOL_SIZE 0x800000 /* (0x100000) */
/* each of the two reference lists of every cluster partition (collideable
objects, noncollideable objects, lights) */
#define HALO_PORT_MAXIMUM_CLUSTER_REFERENCES 8192 /* (2048) */
#define HALO_PORT_MAXIMUM_RENDERED_OBJECTS 1024 /* (256) */
#define HALO_PORT_MAXIMUM_CACHED_OBJECT_RENDER_STATES 1024 /* (256) */
/* objects one explosion can damage */
#define HALO_PORT_MAXIMUM_AREA_OF_EFFECT_OBJECTS 256 /* (64) */
/* object references shared by all script object lists */
#define HALO_PORT_MAXIMUM_LISTED_OBJECTS_PER_MAP 1024 /* (128) */

/* ---------- effects, particles, lights and sounds */

#define HALO_PORT_MAXIMUM_EFFECTS 2048 /* (256) */
#define HALO_PORT_MAXIMUM_EFFECT_LOCATIONS 4096 /* (512) */
#define HALO_PORT_MAXIMUM_PARTICLES 8192 /* (1024) */
#define HALO_PORT_MAXIMUM_PARTICLE_SYSTEMS 256 /* (64) */
#define HALO_PORT_MAXIMUM_SYSTEM_PARTICLES 4096 /* (512) */
#define HALO_PORT_MAXIMUM_CONTRAILS 1024 /* (256) */
#define HALO_PORT_MAXIMUM_CONTRAIL_POINTS 8192 /* (1024) */
#define HALO_PORT_MAXIMUM_LIGHTS_PER_MAP 4096 /* (896) */
#define HALO_PORT_MAXIMUM_GAME_LOOPING_SOUNDS 4096 /* (1024) */

#define HALO_PORT_TEXTURE_CACHE_SIZE 0x1600000 /* (0x1600000) */
#define HALO_PORT_SOUND_CACHE_SIZE 0x400000 /* (0x400000) */

#endif /* HALO_NSPIRE */

#endif /* __HALO_PORT_CAPACITY_H */
