/*
 * pro1x-music-sink -- music through a deep-buffer sink on the F(x)tec Pro1X.
 *
 * PulseAudio cannot use timer-based scheduling on the Pro1X's sound card
 * (the DSP's PCMs are batch devices), so the UCM sink's 20 ms periods wake
 * the CPU 50 times a second while anything plays. Kernels with patches
 * 0034/0035 have a second playback front end, MultiMedia4 (hw:Pro1X,8),
 * which the DSP mixes with the UCM sink's MultiMedia1, with periods of up
 * to 640 ms (docs/audio.md, "Music with a deep buffer"). This PulseAudio
 * client of the session
 *
 *   - loads a sink there, pro1x_music: 2 periods of 640 ms, about 1.5
 *     wake-ups a second, with the intended roles ROLES (default: x-maemo,
 *     the media player's mafw-gst-renderer, and music), so that
 *     module-intended-roles creates such streams on it;
 *   - gives it the default sink's volume and mute whenever they change:
 *     the volume keys and the status menu set the default sink's volume;
 *   - keeps the default output: a new stream with one of ROLES moves to
 *     pro1x_music only from the default sink while that is the phone's own
 *     (the same ALSA card), and from pro1x_music to the default sink while
 *     that is another one (Bluetooth headphones);
 *   - unloads the sink when it is stopped (SIGTERM, SIGINT).
 *
 * Nothing is loaded on a kernel without MultiMedia4 (no FILE), or with
 * /etc/pulse/pro1x-deep-buffer-off. Without this program running there is
 * no such sink and everything plays as before.
 *
 *   pro1x-music-sink [-v] [-p BYTES] [-d DEVICE] [-x FILE] [-a PROPS] [ROLE...]
 *       -v          what it does, on stderr
 *       -p BYTES    the period (default 122880, 640 ms; a multiple of 1920,
 *                   the DSP's 480-frame step): the longest time until a
 *                   pause or a volume change is heard is twice that
 *       -d DEVICE   the PCM (default hw:Pro1X,8)
 *       -x FILE     only if FILE exists (default /proc/asound/Pro1X/pcm8p)
 *       -a PROPS    more sink properties (tests: "alsa.card=0")
 *   runs in the X session (Xsession.post/48pro1x-music-sink)
 *
 * Build: gcc -O2 -Wall -o pro1x-music-sink pro1x-music-sink.c $(pkg-config --cflags --libs libpulse)
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pulse/pulseaudio.h>

#define MUSIC_SINK "pro1x_music"
#define OFF_FILE "/etc/pulse/pro1x-deep-buffer-off"
/* 2 periods, by default of 640 ms of 48 kHz stereo S16 (kernel patch 0035;
 * a kernel with the old limit of 65536 bytes gives 4 of 320 ms) */
#define SINK_ARGS "sink_name=" MUSIC_SINK " rate=48000 format=s16le channels=2 fragments=2"

static const char *default_roles[] = { "x-maemo", "music", NULL };
static const char **roles = default_roles;
static const char *device = "hw:Pro1X,8";
static const char *check_file = "/proc/asound/Pro1X/pcm8p";
static const char *extra_props = "";
static unsigned long period_bytes = 122880;
static int verbose;

static pa_mainloop *ml;
static pa_context *ctx;
static int stopping;

/* the module we loaded, the music sink, and the default sink as last seen */
static uint32_t module_idx = PA_INVALID_INDEX;
static int loading;
static uint32_t music_idx = PA_INVALID_INDEX;
static char music_card[32];
static pa_channel_map music_map;
static pa_cvolume music_vol;
static int music_mute = -1;

static char *default_name;
static uint32_t default_idx = PA_INVALID_INDEX;
static char default_card[32];
static pa_channel_map default_map;
static pa_cvolume default_vol;
static int default_mute = -1;

#define LOG(...) do { if (verbose) { fprintf(stderr, "pro1x-music-sink: " __VA_ARGS__); \
	fputc('\n', stderr); } } while (0)

static void drop(pa_operation *o)
{
	if (o)
		pa_operation_unref(o);
}

static int our_role(const char *role)
{
	int i;

	if (!role)
		return 0;
	for (i = 0; roles[i]; i++)
		if (!strcmp(roles[i], role))
			return 1;
	return 0;
}

/* the default sink is the phone's own: one of the music sink's card */
static int default_is_ours(void)
{
	return music_idx != PA_INVALID_INDEX && default_idx != PA_INVALID_INDEX &&
	       default_idx != music_idx && music_card[0] && !strcmp(music_card, default_card);
}

/* pro1x_music to the default sink's volume and mute */
static void mirror(void)
{
	pa_cvolume v;

	if (!default_is_ours() || !pa_cvolume_valid(&default_vol) || !music_map.channels)
		return;
	v = default_vol;
	pa_cvolume_remap(&v, &default_map, &music_map);
	if (!pa_cvolume_equal(&v, &music_vol)) {
		LOG("volume %u%% to " MUSIC_SINK, (unsigned)((pa_cvolume_max(&v) * 100 + PA_VOLUME_NORM / 2) /
							      PA_VOLUME_NORM));
		drop(pa_context_set_sink_volume_by_index(ctx, music_idx, &v, NULL, NULL));
		music_vol = v;
	}
	if (default_mute >= 0 && default_mute != music_mute) {
		drop(pa_context_set_sink_mute_by_index(ctx, music_idx, default_mute, NULL, NULL));
		music_mute = default_mute;
	}
}

static void input_cb(pa_context *c, const pa_sink_input_info *i, int eol, void *data)
{
	const char *role;

	(void)data;
	if (eol || !i || music_idx == PA_INVALID_INDEX || default_idx == PA_INVALID_INDEX)
		return;
	role = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_ROLE);
	if (!our_role(role))
		return;
	if (default_is_ours() && i->sink == default_idx) {
		LOG("stream %u (%s) to " MUSIC_SINK, i->index, role);
		drop(pa_context_move_sink_input_by_index(c, i->index, music_idx, NULL, NULL));
	} else if (!default_is_ours() && i->sink == music_idx && default_idx != music_idx) {
		LOG("stream %u (%s) to the default sink %s", i->index, role, default_name);
		drop(pa_context_move_sink_input_by_index(c, i->index, default_idx, NULL, NULL));
	}
}

static void copy_card(char *dst, size_t n, const pa_proplist *p)
{
	const char *c = pa_proplist_gets(p, "alsa.card");

	snprintf(dst, n, "%s", c ? c : "");
}

static void sink_cb(pa_context *c, const pa_sink_info *i, int eol, void *data)
{
	int scan = 0;

	(void)data;
	if (eol || !i)
		return;
	if (!strcmp(i->name, MUSIC_SINK)) {
		if (music_idx != i->index) {
			LOG(MUSIC_SINK " is sink %u", i->index);
			scan = 1;
		}
		music_idx = i->index;
		copy_card(music_card, sizeof(music_card), i->proplist);
		music_map = i->channel_map;
		music_vol = i->volume;
		music_mute = i->mute;
	}
	if (default_name && !strcmp(i->name, default_name)) {
		if (default_idx != i->index)
			scan = 1;
		default_idx = i->index;
		copy_card(default_card, sizeof(default_card), i->proplist);
		default_map = i->channel_map;
		default_vol = i->volume;
		default_mute = i->mute;
	}
	mirror();
	/* both known now, or one changed: the streams there already */
	if (scan)
		drop(pa_context_get_sink_input_info_list(c, input_cb, NULL));
}

static void server_cb(pa_context *c, const pa_server_info *i, void *data)
{
	(void)data;
	if (!i)
		return;
	if (default_name && i->default_sink_name && !strcmp(default_name, i->default_sink_name))
		return;
	free(default_name);
	default_name = i->default_sink_name ? strdup(i->default_sink_name) : NULL;
	default_idx = PA_INVALID_INDEX;
	default_card[0] = 0;
	LOG("default sink %s", default_name ? default_name : "(none)");
	if (default_name)
		drop(pa_context_get_sink_info_by_name(c, default_name, sink_cb, NULL));
}

static void loaded_cb(pa_context *c, uint32_t idx, void *data)
{
	(void)data;
	loading = 0;
	if (idx == PA_INVALID_INDEX) {
		LOG("module-alsa-sink on %s did not load: %s", device, pa_strerror(pa_context_errno(c)));
		return;
	}
	module_idx = idx;
	LOG("module-alsa-sink %u on %s", idx, device);
	if (stopping)
		drop(pa_context_unload_module(c, idx, NULL, NULL));
}

/* the sink, unless it is there or must not be */
static void music_info_cb(pa_context *c, const pa_sink_info *i, int eol, void *data)
{
	char args[1024], intended[256] = "";
	int k;

	if (i) {
		sink_cb(c, i, eol, data);
		return;
	}
	/* eol < 0: no such sink */
	if (eol > 0 || music_idx != PA_INVALID_INDEX || loading || module_idx != PA_INVALID_INDEX)
		return;
	if (access(check_file, F_OK) < 0) {
		LOG("no %s: no deep-buffer front end in this kernel", check_file);
		return;
	}
	if (access(OFF_FILE, F_OK) == 0) {
		LOG(OFF_FILE " is there: off");
		return;
	}
	for (k = 0; roles[k]; k++)
		snprintf(intended + strlen(intended), sizeof(intended) - strlen(intended), "%s%s",
			 k ? " " : "", roles[k]);
	snprintf(args, sizeof(args), "device=%s " SINK_ARGS " fragment_size=%lu sink_properties=\""
		 "device.description='Music (deep buffer)' device.intended_roles='%s' %s\"",
		 device, period_bytes, intended, extra_props);
	LOG("loading module-alsa-sink %s", args);
	loading = 1;
	drop(pa_context_load_module(c, "module-alsa-sink", args, loaded_cb, NULL));
}

static void event_cb(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *data)
{
	unsigned f = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK, k = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

	(void)data;
	switch (f) {
	case PA_SUBSCRIPTION_EVENT_SERVER:
		drop(pa_context_get_server_info(c, server_cb, NULL));
		break;
	case PA_SUBSCRIPTION_EVENT_SINK:
		if (k == PA_SUBSCRIPTION_EVENT_REMOVE) {
			if (idx == music_idx) {
				LOG(MUSIC_SINK " gone");
				music_idx = PA_INVALID_INDEX;
				music_card[0] = 0;
				music_map.channels = 0;
				music_mute = -1;
			}
			if (idx == default_idx)
				default_idx = PA_INVALID_INDEX;
		} else {
			drop(pa_context_get_sink_info_by_index(c, idx, sink_cb, NULL));
		}
		break;
	case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
		/* new streams only: one that someone moves later stays there */
		if (k == PA_SUBSCRIPTION_EVENT_NEW)
			drop(pa_context_get_sink_input_info(c, idx, input_cb, NULL));
		break;
	case PA_SUBSCRIPTION_EVENT_MODULE:
		if (k == PA_SUBSCRIPTION_EVENT_REMOVE && idx == module_idx)
			module_idx = PA_INVALID_INDEX;
		break;
	}
}

static void state_cb(pa_context *c, void *data)
{
	(void)data;
	switch (pa_context_get_state(c)) {
	case PA_CONTEXT_READY:
		LOG("connected");
		pa_context_set_subscribe_callback(c, event_cb, NULL);
		drop(pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SERVER | PA_SUBSCRIPTION_MASK_SINK |
					  PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_MODULE,
					  NULL, NULL));
		drop(pa_context_get_server_info(c, server_cb, NULL));
		drop(pa_context_get_sink_info_by_name(c, MUSIC_SINK, music_info_cb, NULL));
		break;
	case PA_CONTEXT_FAILED:
	case PA_CONTEXT_TERMINATED:
		pa_mainloop_quit(ml, 1);
		break;
	default:
		break;
	}
}

static void unloaded_cb(pa_context *c, int success, void *data)
{
	(void)c; (void)success; (void)data;
	pa_mainloop_quit(ml, 0);
}

static void signal_cb(pa_mainloop_api *api, pa_signal_event *e, int sig, void *data)
{
	(void)api; (void)e; (void)data;
	LOG("signal %d", sig);
	stopping = 1;
	if (ctx && pa_context_get_state(ctx) == PA_CONTEXT_READY && module_idx != PA_INVALID_INDEX) {
		pa_operation *o = pa_context_unload_module(ctx, module_idx, unloaded_cb, NULL);

		if (o) {
			pa_operation_unref(o);
			return;
		}
	}
	pa_mainloop_quit(ml, 0);
}

int main(int argc, char **argv)
{
	int o, ret = 0;

	while ((o = getopt(argc, argv, "vp:d:x:a:")) != -1) {
		switch (o) {
		case 'v': verbose = 1; break;
		case 'p': period_bytes = strtoul(optarg, NULL, 0); break;
		case 'd': device = optarg; break;
		case 'x': check_file = optarg; break;
		case 'a': extra_props = optarg; break;
		default:
			fprintf(stderr, "usage: %s [-v] [-p BYTES] [-d DEVICE] [-x FILE] [-a PROPS] [ROLE...]\n",
				argv[0]);
			return 2;
		}
	}
	if (optind < argc)
		roles = (const char **)&argv[optind];	/* argv ends with NULL */

	ml = pa_mainloop_new();
	pa_signal_init(pa_mainloop_get_api(ml));
	pa_signal_new(SIGTERM, signal_cb, NULL);
	pa_signal_new(SIGINT, signal_cb, NULL);
	signal(SIGPIPE, SIG_IGN);
	/* PulseAudio not up yet (NOFAIL waits) or restarted: connect again */
	while (!stopping) {
		module_idx = music_idx = default_idx = PA_INVALID_INDEX;
		loading = 0;
		music_card[0] = default_card[0] = 0;
		music_map.channels = 0;
		music_mute = default_mute = -1;
		free(default_name);
		default_name = NULL;
		ctx = pa_context_new(pa_mainloop_get_api(ml), "pro1x-music-sink");
		pa_context_set_state_callback(ctx, state_cb, NULL);
		if (pa_context_connect(ctx, NULL, PA_CONTEXT_NOFAIL, NULL) >= 0)
			pa_mainloop_run(ml, &ret);
		pa_context_disconnect(ctx);
		pa_context_unref(ctx);
		ctx = NULL;
		if (!stopping) {
			LOG("disconnected");
			sleep(5);
		}
	}
	pa_signal_done();
	pa_mainloop_free(ml);
	return 0;
}
