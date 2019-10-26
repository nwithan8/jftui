#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <locale.h>
#include <mpv/client.h>

#include "shared.h"
#include "net.h"
#include "json.h"
#include "config.h"
#include "disk.h"


// workaround for mpv bug #3988
#if MPV_CLIENT_API_VERSION <= MPV_MAKE_VERSION(1,24)
#define JF_MPV_SET_OPTPROP mpv_set_option
#define JF_MPV_SET_OPTPROP_STRING mpv_set_option_string
#else
#define JF_MPV_SET_OPTPROP mpv_set_property
#define JF_MPV_SET_OPTPROP_STRING mpv_set_property_string
#endif


////////// GLOBAL VARIABLES //////////
jf_options g_options;
jf_global_state g_state;
mpv_handle *g_mpv_ctx = NULL;
//////////////////////////////////////


////////// STATIC FUNCTIONS //////////
static inline void jf_mpv_version_check(void);
static void jf_print_usage(void);
static inline void jf_missing_arg(const char *arg);
static mpv_handle *jf_mpv_context_new(void);
static inline void jf_mpv_event_dispatch(const mpv_event *event);

// playback_ticks refers to segment referred by id
static void jf_update_progress_remote(const char *id, int64_t playback_ticks);


// Updates playback progress marker of the currently playing item on the server.
// Detects if we moved across split-file parts since the last such update and
// marks parts previous to current as played, next to current as unplayed (so
// that the item only has one overall progress marker on the server).
//
// Parameters:
//  - playback_ticks: current position in Jellyfin ticks, referring to the
//    whole merged file in case of split-part.
static void jf_now_playing_update_progress(const int64_t playback_ticks);

static inline void jf_align_subtitle(const int64_t sid);
//////////////////////////////////////


////////// PROGRAM TERMINATION //////////
// Note: the signature and description of this function are in shared.h
void jf_exit(int sig)
{
	// some of this is not async-signal-safe
	// but what's the worst that can happen, a crash? :^)
	g_state.state = sig == JF_EXIT_SUCCESS ? JF_STATE_USER_QUIT : JF_STATE_FAIL;
	if (sig == SIGABRT) {
		perror("FATAL");
	}
	jf_disk_clear();
	jf_net_clear();
	mpv_terminate_destroy(g_mpv_ctx);
	_exit(sig == JF_EXIT_SUCCESS ? EXIT_SUCCESS : EXIT_FAILURE);
}
/////////////////////////////////////////


////////// STARTUP STUFF //////////
static inline void jf_mpv_version_check(void)
{
	unsigned long mpv_version = mpv_client_api_version();
	if (mpv_version < MPV_MAKE_VERSION(1,24)) {
		fprintf(stderr, "FATAL: found libmpv version %lu.%lu, but 1.24 or greater is required.\n",
				mpv_version >> 16, mpv_version & 0xFFFF);
		jf_exit(JF_EXIT_FAILURE);
	}
	// future proofing
	if (mpv_version >= MPV_MAKE_VERSION(2,0)) {
		fprintf(stderr, "Warning: found libmpv version %lu.%lu, but jftui expects 1.xx. mpv will probably not work.\n",
				mpv_version >> 16, mpv_version & 0xFFFF);
	}
}


static void jf_print_usage() {
	printf("Usage:\n");
	printf("\t--help\n");
	printf("\t--version\n");
	printf("\t--config-dir <directory> (default: $XDG_CONFIG_HOME/jftui)\n");
	printf("\t--runtime-dir <directory> (default: $XDG_DATA_HOME/jftui)\n");
	printf("\t--login.\n");
	printf("\t--no-check-updates\n");
}


static inline void jf_missing_arg(const char *arg)
{
	fprintf(stderr, "FATAL: missing parameter for argument %s\n", arg);
	jf_print_usage();
}
///////////////////////////////////


////////// MISCELLANEOUS GARBAGE //////////
static mpv_handle *jf_mpv_context_new()
{
	mpv_handle *ctx;
	int mpv_flag_yes = 1;
	char *x_emby_token;

	assert((ctx = mpv_create()) != NULL);
	JF_MPV_ASSERT(JF_MPV_SET_OPTPROP(ctx, "config-dir", MPV_FORMAT_STRING, &g_state.config_dir));
	JF_MPV_ASSERT(JF_MPV_SET_OPTPROP(ctx, "config", MPV_FORMAT_FLAG, &mpv_flag_yes));
	JF_MPV_ASSERT(JF_MPV_SET_OPTPROP(ctx, "osc", MPV_FORMAT_FLAG, &mpv_flag_yes));
	JF_MPV_ASSERT(JF_MPV_SET_OPTPROP(ctx, "input-default-bindings", MPV_FORMAT_FLAG, &mpv_flag_yes));
	JF_MPV_ASSERT(JF_MPV_SET_OPTPROP(ctx, "input-vo-keyboard", MPV_FORMAT_FLAG, &mpv_flag_yes));
	JF_MPV_ASSERT(JF_MPV_SET_OPTPROP(ctx, "input-terminal", MPV_FORMAT_FLAG, &mpv_flag_yes));
	JF_MPV_ASSERT(JF_MPV_SET_OPTPROP(ctx, "terminal", MPV_FORMAT_FLAG, &mpv_flag_yes));
	assert((x_emby_token = jf_concat(2, "x-emby-token: ", g_options.token)) != NULL);
	JF_MPV_ASSERT(JF_MPV_SET_OPTPROP_STRING(ctx, "http-header-fields", x_emby_token));
	free(x_emby_token);
	JF_MPV_ASSERT(mpv_observe_property(ctx, 0, "time-pos", MPV_FORMAT_INT64));
	JF_MPV_ASSERT(mpv_observe_property(ctx, 0, "sid", MPV_FORMAT_INT64));

	JF_MPV_ASSERT(mpv_initialize(ctx));

	return ctx;
}


static void jf_update_progress_remote(const char *id, int64_t playback_ticks)
{
	char *progress_post;

	progress_post = jf_json_generate_progress_post(id, playback_ticks);
	jf_net_request("/sessions/playing/progress",
			JF_REQUEST_ASYNC_DETACH,
			JF_HTTP_POST,
			progress_post);
	free(progress_post);
}


static void jf_now_playing_update_progress(const int64_t playback_ticks)
{
	size_t i, last_part, current_part;
	int64_t accounted_ticks, current_tick_offset;

    // single-part items are blissfully simple and I lament my toil elsewise
    if (g_state.now_playing->children_count <= 1) {
		jf_update_progress_remote(g_state.now_playing->id, playback_ticks);
	    g_state.now_playing->playback_ticks = playback_ticks;
        return;
    }

    // split-part: figure out part number of current pos and last update
    accounted_ticks = 0;
    current_tick_offset = 0;
    for (i = 0; i < g_state.now_playing->children_count; i++) {
        if (accounted_ticks <= playback_ticks) {
            if (playback_ticks < accounted_ticks + g_state.now_playing->children[i]->runtime_ticks) {
                current_part = i;
            } else {
                current_tick_offset += g_state.now_playing->children[i]->runtime_ticks;
            }
        }
        if (accounted_ticks <= g_state.now_playing->playback_ticks
                && g_state.now_playing->playback_ticks < accounted_ticks + g_state.now_playing->children[i]->runtime_ticks) {
            last_part = i;
        }
        accounted_ticks += g_state.now_playing->children[i]->runtime_ticks;
    }

    // update progress of current part and record last update
    jf_update_progress_remote(g_state.now_playing->children[current_part]->id,
            playback_ticks - current_tick_offset);
    g_state.now_playing->playback_ticks = playback_ticks;
    
    // check if moved across parts and in case update
    if (last_part == current_part) return;
    for (i = 0; i < g_state.now_playing->children_count; i++) {
        if (i < current_part) {
            jf_menu_mark_played(g_state.now_playing->children[i]);
        } else if (i > current_part) {
            jf_menu_mark_unplayed(g_state.now_playing->children[i]);
        }
    }
}


static inline void jf_align_subtitle(const int64_t sid)
{
    int64_t track_count, track_id, playback_ticks, sub_delay;
    size_t i;
    long long offset_ticks;
    int success, is_external;
    bool is_sub;
    char num[3];
    char *track_type, *tmp;

    if (g_state.now_playing->children_count <= 1) return;

    // look for right track
	if (mpv_get_property(g_mpv_ctx, "track-list/count", MPV_FORMAT_INT64, &track_count) != 0) return;
    i = 0; // track-numbers are 0-based
    while (true) {
        if ((int64_t)i >= track_count) return;
        success = snprintf(num, 3, "%ld", i);
        if (success < 0 || success >= 3) {
            i++;
            continue;
        }
        tmp = jf_concat(3, "track-list/", num, "/id");
	    success = mpv_get_property(g_mpv_ctx, tmp, MPV_FORMAT_INT64, &track_id);
        free(tmp);
        if (success != 0) {
            i++;
            continue;
        }
        tmp = jf_concat(3, "track-list/", num, "/type");
	    success = mpv_get_property(g_mpv_ctx, tmp, MPV_FORMAT_STRING, &track_type);
        free(tmp);
        if (success != 0) {
            i++;
            continue;
        }
        is_sub = strcmp(track_type, "sub") == 0;
        mpv_free(track_type);
        if (track_id == sid && is_sub) break;
        i++;
    }

    // check if external
    tmp = jf_concat(3, "track-list/", num, "/external");
    success = mpv_get_property(g_mpv_ctx, tmp, MPV_FORMAT_FLAG, &is_external);
    free(tmp);
    if (success != 0) {
        fprintf(stderr, 
                "Warning: could not align subtitle track to split-file: mpv_get_property (external): %s.\n",
                mpv_error_string(success));
        return;
    }
    if (is_external) {
        // compute offset
        success = mpv_get_property(g_mpv_ctx, "time-pos", MPV_FORMAT_INT64, &playback_ticks);
        if (success != 0) {
            fprintf(stderr, 
                    "Warning: could not align subtitle track to split-file: mpv_get_property (time-pos): %s.\n",
                    mpv_error_string(success));
            return;
        }
        playback_ticks = JF_SECS_TO_TICKS(playback_ticks);
        offset_ticks = 0;
        i = 0;
        while (i < g_state.now_playing->children_count
                && offset_ticks + g_state.now_playing->children[i]->runtime_ticks <= playback_ticks) {
            offset_ticks += g_state.now_playing->children[i]->runtime_ticks;
            i++;
        }
        sub_delay = JF_TICKS_TO_SECS(offset_ticks);

        // apply
        success = mpv_set_property(g_mpv_ctx, "sub-delay", MPV_FORMAT_INT64, &sub_delay);
        if (success != 0) {
            fprintf(stderr,
                    "Warning: could not align subtitle track to split-file: mpv_set_property: %s.\n",
                    mpv_error_string(success));
        }
    } else {
        // internal are graciously aligned by EDL protocol: 0 offset
        sub_delay = 0;
        success = mpv_set_property(g_mpv_ctx, "sub-delay", MPV_FORMAT_INT64, &sub_delay);
        if (success != 0) {
            fprintf(stderr,
                    "Warning: could not align subtitle track to split-file: mpv_set_property: %s.\n",
                    mpv_error_string(success));
        }
    }
}


static inline void jf_mpv_event_dispatch(const mpv_event *event)
{
	int64_t playback_ticks;
	int mpv_flag_yes = 1, mpv_flag_no = 0;

#ifdef JF_DEBUG
// 	printf("DEBUG: event: %s\n", mpv_event_name(event->event_id));
#endif
	switch (event->event_id) {
		case MPV_EVENT_CLIENT_MESSAGE:
			// playlist controls
			if (((mpv_event_client_message *)event->data)->num_args > 0) {
				if (strcmp(((mpv_event_client_message *)event->data)->args[0],
							"jftui-playlist-next") == 0) {
					jf_menu_playlist_forward();
				} else if (strcmp(((mpv_event_client_message *)event->data)->args[0],
							"jftui-playlist-prev") == 0) {
					jf_menu_playlist_backward();
				}
			}
			break;
		case MPV_EVENT_END_FILE:
			// tell server file playback stopped so it won't keep accruing progress
			playback_ticks =
				mpv_get_property(g_mpv_ctx, "time-pos", MPV_FORMAT_INT64, &playback_ticks) == 0 ?
				JF_SECS_TO_TICKS(playback_ticks) : g_state.now_playing->playback_ticks;
			jf_now_playing_update_progress(playback_ticks);
			// move to next item in playlist, if any
			if (((mpv_event_end_file *)event->data)->reason == MPV_END_FILE_REASON_EOF) {
				if (jf_menu_playlist_forward()) {
					g_state.state = JF_STATE_PLAYBACK_NAVIGATING;
				}
			}
			break;
		case MPV_EVENT_SEEK:
			// syncing to user progress marker
			if (g_state.state == JF_STATE_PLAYBACK_START_MARK) {
				JF_MPV_ASSERT(mpv_set_property_string(g_mpv_ctx, "start", "none"));
				// ensure parent playback ticks refer to merged item
				playback_ticks =
					mpv_get_property(g_mpv_ctx, "time-pos", MPV_FORMAT_INT64, &playback_ticks) == 0 ?
					JF_SECS_TO_TICKS(playback_ticks) : 0;
				g_state.now_playing->playback_ticks = playback_ticks;
				g_state.state = JF_STATE_PLAYBACK;
				break;
			}
            // no need to update progress as a time-pos event gets fired
            // immediately after
			break;
		case MPV_EVENT_PROPERTY_CHANGE:
            if (((mpv_event_property *)event->data)->format == MPV_FORMAT_NONE) break;
			if (strcmp("time-pos", ((mpv_event_property *)event->data)->name) == 0) {
                // event valid, check if need to update the server
                playback_ticks = JF_SECS_TO_TICKS(*(int64_t *)((mpv_event_property *)event->data)->data);
                if (llabs(playback_ticks - g_state.now_playing->playback_ticks) < JF_SECS_TO_TICKS(10)) break;
                // good for update; note this will also start a playback session if none are there
                jf_now_playing_update_progress(playback_ticks);
            } else if (strcmp("sid", ((mpv_event_property *)event->data)->name) == 0) {
                // subtitle track change, go and see if we need to align for split-part
                jf_align_subtitle(*(int64_t *)((mpv_event_property *)event->data)->data);
            }
			break;
		case MPV_EVENT_IDLE:
			if (g_state.state == JF_STATE_PLAYBACK_NAVIGATING) {
				// digest idle event while we move to the next track
				g_state.state = JF_STATE_PLAYBACK;
			} else {
				// go into UI mode
				g_state.state = JF_STATE_MENU_UI;
				JF_MPV_ASSERT(mpv_set_property(g_mpv_ctx, "terminal", MPV_FORMAT_FLAG, &mpv_flag_no));
				while (g_state.state == JF_STATE_MENU_UI) jf_menu_ui();
				JF_MPV_ASSERT(mpv_set_property(g_mpv_ctx, "terminal", MPV_FORMAT_FLAG, &mpv_flag_yes));
			}
			break;
		case MPV_EVENT_SHUTDOWN:
			// tell jellyfin playback stopped
			// NB we can't call mpv_get_property because mpv core has aborted!
			jf_now_playing_update_progress(g_state.now_playing->playback_ticks);
			// it is unfortunate, but the cleanest way to handle this case
			// (which is when mpv receives a "quit" command)
			// is to comply and create a new context
			mpv_terminate_destroy(g_mpv_ctx);
			g_mpv_ctx = jf_mpv_context_new();
			break;
		default:
			// no-op on everything else
			break;
	}
}
///////////////////////////////////////////


////////// MAIN LOOP //////////
int main(int argc, char *argv[])
{
	// VARIABLES
	int i;
	char *config_path;
	jf_reply *reply, *reply_alt;


	// SIGNAL HANDLERS
	{
		struct sigaction sa;
		sa.sa_handler = jf_exit;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sa.sa_sigaction = NULL;
		assert(sigaction(SIGABRT, &sa, NULL) == 0);
		assert(sigaction(SIGINT, &sa, NULL) == 0);
		// for the sake of multithreaded libcurl
		sa.sa_handler = SIG_IGN;
		assert(sigaction(SIGPIPE, &sa, NULL) == 0);
	}
	//////////////////


	// LIBMPV VERSION CHECK
	// required for "osc" option
	jf_mpv_version_check();
	///////////////////////


	// SETUP OPTIONS
	jf_options_init();
	////////////////


	// SETUP GLOBAL STATE
	g_state = (jf_global_state){ 0 };
	assert((g_state.session_id = jf_generate_random_id(0)) != NULL);
	/////////////////////


	// COMMAND LINE ARGUMENTS
	i = 0;
	while (++i < argc) {
		if (strcmp(argv[i], "--help") == 0) {
			jf_print_usage();
			jf_exit(JF_EXIT_SUCCESS);
		} else if (strcmp(argv[i], "--config-dir") == 0) {
			if (++i >= argc) {
				jf_missing_arg("--config-dir");
				jf_exit(JF_EXIT_FAILURE);
			}
			assert((g_state.config_dir = strdup(argv[i])) != NULL);
		} else if (strcmp(argv[i], "--runtime-dir") == 0) {
			if (++i >= argc) {
				jf_missing_arg("--runtime-dir");
				jf_exit(JF_EXIT_FAILURE);
			}
			assert((g_state.runtime_dir = strdup(argv[i])) != NULL);
		} else if (strcmp(argv[i], "--login") == 0) {
			g_state.state = JF_STATE_STARTING_LOGIN;
		} else if (strcmp(argv[i], "--no-check-updates") == 0) {
			g_options.check_updates = false;
		} else if (strcmp(argv[i], "--version") == 0) {
			printf("%s\n", g_options.version);
			jf_exit(JF_EXIT_SUCCESS);
		} else {
			fprintf(stderr, "FATAL: unrecognized argument %s.\n", argv[i]);
			jf_print_usage();
			jf_exit(JF_EXIT_FAILURE);
		}
	}
	/////////////////////////
	

	// SETUP DISK
	// apply runtime directory location default unless there was user override
	if (g_state.runtime_dir == NULL
			&& (g_state.runtime_dir = jf_disk_get_default_runtime_dir()) == NULL) {
		fprintf(stderr, "FATAL: could not acquire runtime directory location. $HOME could not be read and --runtime-dir was not passed.\n");
		jf_exit(JF_EXIT_FAILURE);
	}
	jf_disk_init();
	/////////////


	// READ AND PARSE CONFIGURATION FILE
	// apply config directory location default unless there was user override
	if (g_state.config_dir == NULL
			&& (g_state.config_dir = jf_config_get_default_dir()) == NULL) {
		fprintf(stderr, "FATAL: could not acquire configuration directory location. $HOME could not be read and --config-dir was not passed.\n");
		jf_exit(JF_EXIT_FAILURE);
	}
	// get expected location of config file
	config_path = jf_concat(2, g_state.config_dir, "/settings");

	// check config file exists
	if (access(config_path, F_OK) == 0) {
		// it's there: read it
		jf_config_read(config_path);
		if (strcmp(g_options.version, JF_VERSION) < 0) {
			printf("Attention: jftui was updated from the last time it was run. Check the changelog on Github.\n");
			free(g_options.version);
			assert((g_options.version = strdup(JF_VERSION)) != NULL);
		}
		// if fundamental fields are missing (file corrupted for some reason)
		if (g_options.server == NULL
				|| g_options.userid == NULL
				|| g_options.token == NULL) {
			if (! jf_menu_user_ask_yn("Error: settings file missing fundamental fields. Would you like to go through manual configuration?")) {
				jf_exit(JF_EXIT_SUCCESS);
			}
			free(g_options.server);
			free(g_options.userid);
			free(g_options.token);
			g_state.state = JF_STATE_STARTING_FULL_CONFIG;
		}
	} else if (errno == ENOENT) {
		// it's not there
		if (! jf_menu_user_ask_yn("Settings file not found. Would you like to configure jftui?")) {
			jf_exit(JF_EXIT_SUCCESS);
		}
		g_state.state = JF_STATE_STARTING_FULL_CONFIG;
	} else {
		fprintf(stderr, "FATAL: access for settings file at location %s: %s.\n",
			config_path, strerror(errno));
		jf_exit(JF_EXIT_FAILURE);
	}
	////////////////////////////////////
	

	// UPDATE CHECK
	// it runs asynchronously while we do other stuff
	if (g_options.check_updates) {
		reply_alt = jf_net_request(NULL, JF_REQUEST_CHECK_UPDATE, JF_HTTP_GET, NULL);
	}
	///////////////


	// INTERACTIVE CONFIG
	if (g_state.state == JF_STATE_STARTING_FULL_CONFIG) {
		jf_config_ask_user();
	} else if (g_state.state == JF_STATE_STARTING_LOGIN) {
		jf_config_ask_user_login();
	}

	// save to disk
	if (g_state.state == JF_STATE_STARTING_FULL_CONFIG
			|| g_state.state == JF_STATE_STARTING_LOGIN) {
		if (jf_config_write(config_path)) {
			printf("Please restart to apply the new settings.\n");
			jf_exit(JF_EXIT_SUCCESS);
		} else {
			fprintf(stderr, "FATAL: Configuration failed.\n");
			jf_exit(JF_EXIT_FAILURE);
		}
	} else {
		// we don't consider a failure to save config fatal during normal startup
		jf_config_write(config_path);
		free(config_path);
	}
	/////////////////////
	

	// SERVER NAME
	// this doubles up as a check for connectivity and correct login parameters
	reply = jf_net_request("/system/info", JF_REQUEST_IN_MEMORY, JF_HTTP_GET, NULL);
	if (JF_REPLY_PTR_HAS_ERROR(reply)) {
		fprintf(stderr, "FATAL: could not reach server: %s.\n", jf_reply_error_string(reply));
		jf_exit(JF_EXIT_FAILURE);
	}
	jf_json_parse_server_info_response(reply->payload);
	jf_reply_free(reply);
	//////////////
	

	// SETUP MENU
	jf_menu_init();
	/////////////////


	// SETUP MPV
	if (setlocale(LC_NUMERIC, "C") == NULL) {
		fprintf(stderr, "Warning: could not set numeric locale to sane standard. mpv might refuse to work.\n");
	}
	g_mpv_ctx = jf_mpv_context_new();
	////////////


	// resolve update check
	if (g_options.check_updates) {
		jf_net_await(reply_alt);
		if (JF_REPLY_PTR_HAS_ERROR(reply_alt)) {
			fprintf(stderr, "Warning: could not fetch latest version info: %s.\n",
					jf_reply_error_string(reply_alt));
		} else if (strcmp(JF_VERSION, reply_alt->payload) < 0) {
			printf("Attention: jftui v%s is available for update.\n",
					reply_alt->payload);
		}
		jf_reply_free(reply_alt);
	}
	///////////////////////


	////////// MAIN LOOP //////////
	while (true) {
		switch (g_state.state) {
			case JF_STATE_USER_QUIT:
				jf_exit(JF_EXIT_SUCCESS);
				break;
			case JF_STATE_FAIL:
				jf_exit(JF_EXIT_FAILURE);
				break;
			default:
				jf_mpv_event_dispatch(mpv_wait_event(g_mpv_ctx, -1));
		}
	}
	///////////////////////////////


	// never reached
	jf_exit(JF_EXIT_SUCCESS);
}
///////////////////////////////
