/*
NSPIRE_PROFILE.C

Where the Nspire port's frames go: the main loop (source/main/main.c)
times its parts, and every 4 frames the average of each is logged
(d3d8_soft.c calls nspire_profile_report at present).
*/

#include "nspire.h"
#include "halo_nspire_profile.h"

#define SECTION_COUNT NUMBER_OF_NSPIRE_PROFILE_SECTIONS

static const char *const section_names[SECTION_COUNT] =
{
	"input", "player control", "game ticks", "director", "render", "present",
	"  units", "  ai", "  effects", "  first person weapons", "  scripts", "  objects", "  hud",
	"  other updates",
	"    object type update", "    object damage", "    object node matrices", "    object functions",
	"    object lights", "    object postprocess",
	"  rasterizer vertices", "  rasterizer triangles",
	"    vertex fetch", "    vertex program", "    vertex clip codes", "    projection", "    triangle setup",
	"    pixel spans",
	"  draws (all the rasterizer's work)",
	"  render: visibility", "  render: sky", "  render: level", "  render: objects", "  render: level passes",
	"  render: effects", "  render: transparent", "  render: hud and ui", "  render: the rest",
	"    objects: lighting", "    objects: models (draws included)", "      models: node matrices", "      models: draws",
	"    objects: finding them", "    objects: first person weapon",
	"      type update: bipeds", "      type update: vehicles", "      type update: weapons",
	"      type update: the rest", "      node matrices of bipeds and weapons not drawn lately",
	"      node matrices of other objects not drawn lately",
	"        setup: planes", "        setup: texture levels", "        setup: edges",
	"    draw setup (state, combiners, samplers, caches)",
	"        draw setup: state", "        draw setup: vertex cache", "        draw setup: part culling", "        draw setup: pass memo",
	"            part: bounds", "            part: program", "            part: projection",
	"          bipeds: unit_update", "          bipeds: biped_update", "            biped: turning", "            biped: moving",
	"            biped: dead, airborne, landing, slipping", "            biped: animation",
	"            unit: aiming and looking", "            unit: weapon", "            unit: dialogue", "            unit: illumination",
	"      ai: input and perception", "      ai: situation, emotion, decisions", "      ai: actions", "      ai: movement and looking",
	"      ai: combat", "      ai: encounters",
	"        ai: input", "        ai: perception refresh (timesliced)", "        ai: danger zone",
	"        ai: targets' positions", "        ai: targets' status (line of sight)",
	"    passes: shadows and lights", "    passes: decals", "    passes: diffuse texture",
	"    passes: specular and reflections", "    passes: transparent and fog",
};

static unsigned long long started[SECTION_COUNT];
/* the watched value (nspire_widget_error_watch) checked at every section's
beginning and end: when it goes bad, the two checks either side are logged */
static int watch_was_sound = 1, watch_reports;
static long watch_last_section = -1;
static int watch_last_was_end;

extern short *nspire_widget_error_watch(void);

static int watch_sound(void)
{
	short value = *nspire_widget_error_watch();

	return value == -1 || (value >= 0 && value < 64);
}

static void watch_check(long section, int end)
{
	int sound = watch_sound();

	if (watch_was_sound && !sound && watch_reports < 24)
	{
		watch_reports++;
		nspire_log("deferred error code spoilt (%d) between %s of %s and %s of %s", *nspire_widget_error_watch(),
			watch_last_was_end ? "the end" : "the start",
			watch_last_section >= 0 ? section_names[watch_last_section] : "(nothing)", end ? "the end" : "the start",
			section_names[section]);
	}
	watch_was_sound = sound;
	watch_last_section = section;
	watch_last_was_end = end;
}
/* each section's time (halo_nspire_profile.h's macros add to it straight),
and when the inline sections began, in the timer's own count */
unsigned long long nspire_profile_totals[SECTION_COUNT];
unsigned long nspire_profile_started[SECTION_COUNT];
#define totals nspire_profile_totals
static unsigned char section_open[SECTION_COUNT];

void nspire_profile_begin(long section)
{
	if (section >= 0 && section < SECTION_COUNT)
	{
		started[section] = nspire_ticks();
		section_open[section] = 1;
		watch_check(section, 0);
	}
}

void nspire_profile_end(long section)
{
	if (section >= 0 && section < SECTION_COUNT)
	{
		unsigned long long elapsed = nspire_ticks() - started[section];

		totals[section] += elapsed;
		section_open[section] = 0;
		watch_check(section, 1);
		/* the draws made while drawing the objects' models, to tell them from the engine's own work */
		if (section == _nspire_profile_draws && section_open[_nspire_profile_object_render_models])
			totals[_nspire_profile_object_render_draws] += elapsed;
	}
}

/* logs each part's average over the frames since the last report, in ms */
void nspire_profile_report(unsigned long frames)
{
	long section;

	if (!frames)
		return;
	for (section = 0; section < SECTION_COUNT; section++)
	{
		nspire_log("  %s: %lu ms", section_names[section],
			(unsigned long)(totals[section] * 1000ULL / 32768ULL / frames));
		totals[section] = 0;
	}
}
