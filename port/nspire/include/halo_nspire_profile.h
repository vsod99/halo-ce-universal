/*
HALO_NSPIRE_PROFILE.H

Timing the game's parts on the TI-Nspire (port/nspire/src/nspire_profile.c):
NSPIRE_PROFILE_BEGIN and _END around a call add its time to a section,
which is logged every 4 frames. Nothing on the other ports. Included by
halo_linux_prefix.h.
*/

#ifndef __HALO_NSPIRE_PROFILE_H
#define __HALO_NSPIRE_PROFILE_H

/* the sections (nspire_profile.c names them) */
enum
{
	_nspire_profile_input,
	_nspire_profile_player_control,
	_nspire_profile_game_ticks,
	_nspire_profile_director,
	_nspire_profile_render,
	_nspire_profile_present,
	_nspire_profile_units,
	_nspire_profile_ai,
	_nspire_profile_effects,
	_nspire_profile_first_person_weapons,
	_nspire_profile_scripts,
	_nspire_profile_objects,
	_nspire_profile_hud,
	_nspire_profile_other_updates,
	_nspire_profile_object_type,
	_nspire_profile_object_damage,
	_nspire_profile_object_node_matrices,
	_nspire_profile_object_functions,
	_nspire_profile_object_lights,
	_nspire_profile_object_postprocess,
	_nspire_profile_vertices,
	_nspire_profile_pixels,
	_nspire_profile_vertex_fetch,
	_nspire_profile_vertex_program,
	_nspire_profile_vertex_finish,
	_nspire_profile_project,
	_nspire_profile_triangle_setup,
	_nspire_profile_spans,
	_nspire_profile_draws,
	_nspire_profile_render_visibility,
	_nspire_profile_render_sky,
	_nspire_profile_render_level,
	_nspire_profile_render_objects,
	_nspire_profile_render_level_passes,
	_nspire_profile_render_effects,
	_nspire_profile_render_transparent,
	_nspire_profile_render_hud,
	_nspire_profile_render_other,
	_nspire_profile_object_render_lighting,
	_nspire_profile_object_render_models,
	_nspire_profile_model_node_matrices,
	_nspire_profile_object_render_draws,
	_nspire_profile_object_render_find,
	_nspire_profile_object_render_first_person,
	_nspire_profile_update_bipeds,
	_nspire_profile_update_vehicles,
	_nspire_profile_update_weapons,
	_nspire_profile_update_other_types,
	_nspire_profile_node_matrices_unseen,
	_nspire_profile_node_matrices_unseen_other,
	_nspire_profile_setup_planes,
	_nspire_profile_setup_levels,
	_nspire_profile_setup_edges,
	_nspire_profile_draw_setup,
	_nspire_profile_setup_state,
	_nspire_profile_setup_cache,
	_nspire_profile_setup_part,
	_nspire_profile_setup_memo,
	_nspire_profile_part_find,
	_nspire_profile_part_run,
	_nspire_profile_part_project,
	_nspire_profile_biped_unit_part,
	_nspire_profile_biped_part,
	_nspire_profile_biped_turning,
	_nspire_profile_biped_moving,
	_nspire_profile_biped_state,
	_nspire_profile_biped_animation,
	_nspire_profile_unit_aiming,
	_nspire_profile_unit_weapon,
	_nspire_profile_unit_dialogue,
	_nspire_profile_unit_illumination,
	_nspire_profile_ai_perception,
	_nspire_profile_ai_decision,
	_nspire_profile_ai_action,
	_nspire_profile_ai_movement,
	_nspire_profile_ai_combat,
	_nspire_profile_ai_encounters,
	_nspire_profile_ai_input,
	_nspire_profile_ai_perception_refresh,
	_nspire_profile_ai_danger_zone,
	_nspire_profile_ai_prop_position,
	_nspire_profile_ai_prop_status,
	_nspire_profile_passes_lights,
	_nspire_profile_passes_decals,
	_nspire_profile_passes_diffuse,
	_nspire_profile_passes_specular,
	_nspire_profile_passes_transparent,
	NUMBER_OF_NSPIRE_PROFILE_SECTIONS
};

#ifdef HALO_NSPIRE
void nspire_profile_begin(long section);
/* whether an object's model was drawn in the last two frames
(source/render/render_objects.c) */
int nspire_object_seen(long object_index);
/* whether it was drawn smaller than NSPIRE_SMALL_OBJECT_PIXELS of 640 */
int nspire_object_small(long object_index);
void nspire_profile_end(long section);
/* (NSPIRE_REPLAY: tools/nspire_replay's build, timing by its instruction
count through nspire_ticks, defined on its command line, ahead of the
prefix header that brings this one in) */
#ifdef NSPIRE_REPLAY
#define NSPIRE_PROFILE_BEGIN(section) nspire_profile_begin(section)
#define NSPIRE_PROFILE_END(section) nspire_profile_end(section)
#else
/* (inline, as the game opens and closes sections for each object every
tick: the timer nspire_ticks counts by, 32768 Hz, counting down, read
straight; the draws and the models' sections, which the report tells
apart, through the functions) */
extern unsigned long nspire_profile_started[NUMBER_OF_NSPIRE_PROFILE_SECTIONS];
extern unsigned long long nspire_profile_totals[NUMBER_OF_NSPIRE_PROFILE_SECTIONS];
#define NSPIRE_PROFILE_TIMER (*(volatile unsigned long *)0x900D0024)
#define NSPIRE_PROFILE_SPECIAL(section) \
	((section) == _nspire_profile_draws || (section) == _nspire_profile_object_render_models)
#define NSPIRE_PROFILE_BEGIN(section) do { \
	if (NSPIRE_PROFILE_SPECIAL(section)) nspire_profile_begin(section); \
	else nspire_profile_started[section] = NSPIRE_PROFILE_TIMER; } while (0)
#define NSPIRE_PROFILE_END(section) do { \
	if (NSPIRE_PROFILE_SPECIAL(section)) nspire_profile_end(section); \
	else nspire_profile_totals[section] += nspire_profile_started[section] - NSPIRE_PROFILE_TIMER; } while (0)
#endif
#else
#define NSPIRE_PROFILE_BEGIN(section)
#define NSPIRE_PROFILE_END(section)
#endif

#endif
