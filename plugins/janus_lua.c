/*! \file   janus_lua.c
 * \author Lorenzo Miniero <lorenzo@meetecho.com>
 * \copyright GNU General Public License v3
 * \brief  Janus Lua plugin
 * \details  This is a plugin that implements a simple bridge to Lua
 * scripts. While the plugin implements low level stuff like media
 * manipulation, routing, recording, etc., all the logic is demanded
 * to an external Lua script. This means that the C code exposes functions
 * to the Lua script (e.g., to dictate what to do with media, whether
 * recording should be done, sending PLIs, etc.), while Lua exposes
 * functions to be notified by the C code about important events (e.g.,
 * new users, WebRTC state, incoming messages, etc.).
 *
 * Considering the C code and the Lua script will need some sort of
 * "contract" in order to be able to properly interact with each other,
 * the interface (as in method names) must be consistent, but the logic
 * in the Lua script can be completely customized, so that it fits
 * whatever requirement one has (e.g., something like the EchoTest, or
 * something like the VideoRoom).
 *
 * \section luaapi Lua interfaces
 *
 * Every Lua script that wants to implement a Janus plugin must provide
 * the following functions as callbacks:
 * 
 * - \c init(): called when janus_lua.c is initialized;
 * - \c destroy(): called when janus_lua.c is deinitialized (Janus shutting down);
 * - \c createSession(): called when a new user attaches to the Janus Lua plugin;
 * - \c destroySession(): called when an attached user detaches from the Janus Lua plugin;
 * - \c querySession(): called when an Admin API query for a specific user gets to the Janus Lua plugin;
 * - \c handleMessage(): called when a user sends a message to the Janus Lua plugin;
 * - \c setupMedia(): called when a users's WebRTC PeerConnection goes up;
 * - \c hangupMedia(): called when a users's WebRTC PeerConnection goes down;
 * - \c resumeScheduler(): called by the C scheduler to resume coroutines.
 *
 * While \c init() expects a path to a config file (which you can ignore if
 * unneeded), and \c destroy() and \c resumeScheduler() don't need any
 * argument, all other functions expect at the very least a numeric session
 * identifier, that will uniquely address a user in the plugin. Such a
 * value is created dynamically by the C code, and so all the Lua script
 * needs to do is track it as a unique session identifier when handling
 * requests and pushing responses/events/actions towards the C code.
 * Refer to the existing examples (e.g., \c echotest.lua) to see the
 * exact signature for all the above callbacks.
 *
 * \note Notice that, along the above mentioned callbacks, Lua scripts
 * can also implement functions like \c incomingRtp() \c incomingRtcp()
 * and \c incomingData() to handle those packets directly, instead of
 * letting the C code worry about relaying/processing them. While it might
 * make sense to handle incoming data channel messages with \c incomingData()
 * though, the performance impact of directly processing and manipulating
 * RTP an RTCP packets is probably too high, and so their usage is currently
 * discouraged.
 *
 * \section capi C interfaces
 *
 * Just as the Lua script needs to expose callbacks that the C code can
 * invoke, the C code exposes methods as Lua functions accessible from
 * the Lua script. This includes means to push events, configure how
 * media should be routed without handling each packet in Lua, sending
 * RTCP feedback, start/stop recording and so on.
 *
 * The following are the functions the C code exposes:
 *
 * - \c pushEvent(): push an event to the user via Janus API;
 * - \c notifyEvent(): send an event to Event Handlers;
 * - \c closePc(): force the closure of a PeerConnection;
 * - \c configureMedium(): specify whether audio/video/data can be received/sent;
 * - \c addRecipient(): specify which user should receive a user's media;
 * - \c removeRecipient(): specify which user should not receive a user's media anymore;
 * - \c setBitrate(): specify the bitrate to force on a user via REMB feedback;
 * - \c setPliFreq(): specify how often the plugin should send a PLI to this user;
 * - \c sendPli(): send a PLI (keyframe request);
 * - \c startRecording(): start recording audio, video and or data for a user;
 * - \c stopRecording(): start recording audio, video and or data for a user;
 * - \c pokeScheduler(): notify the C code that there's a coroutine to resume.
 *
 * As anticipated in the previous section, almost all these methods also
 * expect the unique session identifier to address a specific user in the
 * plugin. This is true for all the above methods expect \c pokeScheduler()
 * which, together with Lua's \c resumeScheduler(), will be clearer in
 * the next section.
 *
 * \section coroutines Lua/C coroutines scheduler
 *
 * Lua is a single threaded environment. While it has a concept similar
 * to threads called coroutines, these are not threads as known in C.
 * In order to allow for an easy to implement asynchronous behaviour in
 * Lua scripts, you can leverage a scheduler implemented in the C code.
 *
 * More specifically, when the plugin starts a dedicated thread is devoted
 * to the only purpose of acting as a scheduler for Lua coroutines. This
 * means that, whenever this C scheduler is awaken, it will call the
 * \c resumeScheduler() function in the Lua script, thus allowing the
 * Lua script to execute one or more pending coroutines. The C scheduler
 * only acts when triggered, which means it's up to the Lua script to
 * tell it when to wake up: this is possible via the \c pokeScheduler()
 * function, which does nothing more than sending a simple signal to the
 * C scheduler to wake it up. As such, it's easy for the Lua script to
 * implement asynchronous behaviour, e.g.:
 *
 * 1. Lua script needs to do something asynchronously;
 * 2. Lua script creates coroutine, and takes note of it somewhere;
 * 3. Lua script calls \c pokeScheduler();
 * 4. C code sends signal to the thread acting as a scheduler;
 * 5. when the scheduling thread wakes up, it calls \c resumeScheduler();
 * 6. Lua script resumes the previously queued coroutine.
 *
 * This simple mechanism is what the sample Lua scripts provided in this
 * repo use, for instance, to handle incoming messages asynchronously,
 * so you can refer to those to have an idea of how it can be used.
 *
 * \note You can implement asynchronous behaviour any way you want, and
 * you're not required to use this C scheduler. Anyway, you must implement
 * a method called \c resumeScheduler() anyway, as the C code checks for
 * its presence and fails if it's not there. If you don't need it, just
 * create an empty function that does nothing and you'll be fine.
 *
\endverbatim
 *
 * \ingroup plugins
 * \ingroup luapapi
 * \ref plugins
 * \ref luapapi
 */

#include <jansson.h>

/* Session definition and hashtable */
#include "janus_lua_data.h"
/* Extra/custom C hooks and code */
#include "janus_lua_extra.h"


/* Plugin information */
#define JANUS_LUA_VERSION			1
#define JANUS_LUA_VERSION_STRING	"0.0.1"
#define JANUS_LUA_DESCRIPTION		"A custom plugin for the Lua framework."
#define JANUS_LUA_NAME				"Janus Lua plugin"
#define JANUS_LUA_AUTHOR			"Meetecho s.r.l."
#define JANUS_LUA_PACKAGE			"janus.plugin.lua"

/* Plugin methods */
janus_plugin *create(void);
int janus_lua_init(janus_callbacks *callback, const char *config_path);
void janus_lua_destroy(void);
int janus_lua_get_api_compatibility(void);
int janus_lua_get_version(void);
const char *janus_lua_get_version_string(void);
const char *janus_lua_get_description(void);
const char *janus_lua_get_name(void);
const char *janus_lua_get_author(void);
const char *janus_lua_get_package(void);
void janus_lua_create_session(janus_plugin_session *handle, int *error);
struct janus_plugin_result *janus_lua_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep);
void janus_lua_setup_media(janus_plugin_session *handle);
void janus_lua_incoming_rtp(janus_plugin_session *handle, int video, char *buf, int len);
void janus_lua_incoming_rtcp(janus_plugin_session *handle, int video, char *buf, int len);
void janus_lua_incoming_data(janus_plugin_session *handle, char *buf, int len);
void janus_lua_slow_link(janus_plugin_session *handle, int uplink, int video);
void janus_lua_hangup_media(janus_plugin_session *handle);
void janus_lua_destroy_session(janus_plugin_session *handle, int *error);
json_t *janus_lua_query_session(janus_plugin_session *handle);

/* Plugin setup */
static janus_plugin janus_lua_plugin =
	JANUS_PLUGIN_INIT (
		.init = janus_lua_init,
		.destroy = janus_lua_destroy,

		.get_api_compatibility = janus_lua_get_api_compatibility,
		.get_version = janus_lua_get_version,
		.get_version_string = janus_lua_get_version_string,
		.get_description = janus_lua_get_description,
		.get_name = janus_lua_get_name,
		.get_author = janus_lua_get_author,
		.get_package = janus_lua_get_package,
		
		.create_session = janus_lua_create_session,
		.handle_message = janus_lua_handle_message,
		.setup_media = janus_lua_setup_media,
		.incoming_rtp = janus_lua_incoming_rtp,
		.incoming_rtcp = janus_lua_incoming_rtcp,
		.incoming_data = janus_lua_incoming_data,
		.slow_link = janus_lua_slow_link,
		.hangup_media = janus_lua_hangup_media,
		.destroy_session = janus_lua_destroy_session,
		.query_session = janus_lua_query_session,
	);

/* Plugin creator */
janus_plugin *create(void) {
	JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_LUA_NAME);
	return &janus_lua_plugin;
}

/* Useful stuff */
volatile gint initialized = 0, stopping = 0;
janus_callbacks *gateway = NULL;

/* Lua stuff */
lua_State *state = NULL;
janus_mutex lua_mutex = JANUS_MUTEX_INITIALIZER;
static const char *lua_functions[] = {
	"init", "destroy", "resumeScheduler",
	"createSession", "destroySession", "querySession",
	"handleMessage",
	"setupMedia", "hangupMedia"
};
static uint lua_funcsize = sizeof(lua_functions)/sizeof(*lua_functions);
static gboolean has_incoming_rtp = FALSE;
static gboolean has_incoming_rtcp = FALSE;
static gboolean has_incoming_data = FALSE;
/* Lua C scheduler (for coroutines) */
static GThread *scheduler_thread;
static void *janus_lua_scheduler(void *data);
static GAsyncQueue *events = NULL;
typedef enum janus_lua_event {
	janus_lua_event_none = 0,
	janus_lua_event_resume,		/* Resume one or more pending coroutines */
	janus_lua_event_exit		/* Break the scheduler loop */
} janus_lua_event;


/* janus_lua_session is defined in janus_lua_data.h, but it's managed here */
GHashTable *sessions, *ids;
janus_mutex sessions_mutex = JANUS_MUTEX_INITIALIZER;

static void janus_lua_session_destroy(janus_lua_session *session) {
	if(session && g_atomic_int_compare_and_exchange(&session->destroyed, 0, 1)) {
		janus_refcount_decrease(&session->ref);
	}
}

static void janus_lua_session_free(const janus_refcount *session_ref) {
	janus_lua_session *session = janus_refcount_containerof(session_ref, janus_lua_session, ref);
	/* Remove the reference to the core plugin session */
	janus_refcount_decrease(&session->handle->ref);
	/* This session can be destroyed, free all the resources */
	g_hash_table_remove(ids, GUINT_TO_POINTER(session->id));
	janus_recorder_free(session->arc);
	janus_recorder_free(session->vrc);
	janus_recorder_free(session->drc);
	g_free(session);
}

/* Packet data and routing */
typedef struct janus_lua_rtp_relay_packet {
	rtp_header *data;
	gint length;
	gboolean is_video;
	uint32_t timestamp;
	uint16_t seq_number;
} janus_lua_rtp_relay_packet;
static void janus_lua_relay_rtp_packet(gpointer data, gpointer user_data);
static void janus_lua_relay_data_packet(gpointer data, gpointer user_data);


/* Helper struct to address outgoing notifications, e.g., involving PeerConnections */
typedef enum janus_lua_async_event_type {
	janus_lua_async_event_type_none = 0,
	janus_lua_async_event_type_pushevent,
	janus_lua_async_event_type_closepc
} janus_lua_async_event_type;
typedef struct janus_lua_async_event {
	janus_lua_session *session;			/* Who this event is for */
	janus_lua_async_event_type type;	/* What this event is about */
	char *transaction;					/* Notification transaction, if any */
	json_t *event;						/* Content of the notification, if any */
	json_t *jsep;						/* Content of JSEP SDP, if any */
} janus_lua_async_event;
/* Helper thread to push events that need to be asynchronous, e.g., for those
 * that would keep the Lua state busy longer than usual and cause delays,
 * or those that might actually result in a deadlock if done synchronously */
static void *janus_lua_async_event_helper(void *data) {
	janus_lua_async_event *asev = (janus_lua_async_event *)data;
	if(asev == NULL)
		return NULL;
	if(asev->type == janus_lua_async_event_type_pushevent) {
		/* Send the event */
		gateway->push_event(asev->session->handle, &janus_lua_plugin, asev->transaction, asev->event, asev->jsep);
	} else if(asev->type == janus_lua_async_event_type_closepc) {
		/* Close the PeerConnection */
		gateway->close_pc(asev->session->handle);
	}
	json_decref(asev->event);
	json_decref(asev->jsep);
	g_free(asev->transaction);
	janus_refcount_decrease(&asev->session->ref);
	g_free(asev);
	return NULL;
}


/* Methods that we expose to the Lua script */
static int janus_lua_method_pokescheduler(lua_State *s) {
	/* This method allows the Lua script to poke the scheduler and have it wake up ASAP */
	g_async_queue_push(events, GUINT_TO_POINTER(janus_lua_event_resume));
	lua_pushnumber(s, 0);
	return 1;
}

static int janus_lua_method_pushevent(lua_State *s) {
	/* Get the arguments from the provided state */
	int n = lua_gettop(s);
	if(n != 4) {
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 4)", n);
		lua_pushnumber(s, -1);
		return 1;
	}
	guint32 id = lua_tonumber(s, 1);
	const char *transaction = lua_tostring(s, 2);
	const char *event_text = lua_tostring(s, 3);
	const char *jsep_text = lua_tostring(s, 4);
	/* Parse the event/jsep strings to Jansson objects */
	json_error_t error;
	json_t *event = json_loads(event_text, 0, &error);
	if(!event) {
		JANUS_LOG(LOG_ERR, "JSON error: on line %d: %s", error.line, error.text);
		lua_pushnumber(s, -1);
		return 1;
	}
	json_t *jsep = NULL;
	if(jsep_text != NULL) {
		jsep = json_loads(jsep_text, 0, &error);
		if(!jsep) {
			JANUS_LOG(LOG_ERR, "JSON error: on line %d: %s", error.line, error.text);
			json_decref(event);
			lua_pushnumber(s, -1);
			return 1;
		}
	}
	/* Find the session */
	janus_mutex_lock(&sessions_mutex);
	janus_lua_session *session = g_hash_table_lookup(ids, GUINT_TO_POINTER(id));
	if(session == NULL || g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&sessions_mutex);
		json_decref(event);
		if(jsep)
			json_decref(jsep);
		lua_pushnumber(s, -1);
		return 1;
	}
	janus_refcount_increase(&session->ref);
	janus_mutex_unlock(&sessions_mutex);
	/* If there's an SDP attached, create a thread to send the event asynchronously:
	 * sending it here would keep the locked Lua state busy much longer than intended */
	if(jsep != NULL) {
		janus_lua_async_event *asev = g_malloc0(sizeof(janus_lua_async_event));
		asev->session = session;
		asev->type = janus_lua_async_event_type_pushevent;
		asev->transaction = transaction ? g_strdup(transaction) : NULL;
		asev->event = event;
		asev->jsep = jsep;
		GError *error = NULL;
		g_thread_try_new("lua pushevent", janus_lua_async_event_helper, asev, &error);
		if(error != NULL) {
			JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the Lua pushevent thread...\n",
				error->code, error->message ? error->message : "??");
			json_decref(event);
			json_decref(jsep);
			g_free(asev->transaction);
			janus_refcount_decrease(&session->ref);
			g_free(asev);
		}
		/* Return a success/error right away */
		lua_pushnumber(s, error ? 1 : 0);
		return 1;
	}
	/* No SDP, send the event now */
	int res = gateway->push_event(session->handle, &janus_lua_plugin, transaction, event, NULL);
	janus_refcount_decrease(&session->ref);
	json_decref(event);
	lua_pushnumber(s, res);
	return 1;
}

static int janus_lua_method_notifyevent(lua_State *s) {
	/* Get the arguments from the provided state */
	int n = lua_gettop(s);
	if(n != 2) {
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 2)", n);
		lua_pushnumber(s, -1);
		return 1;
	}
	guint32 id = lua_tonumber(s, 1);
	const char *event_text = lua_tostring(s, 2);
	/* Parse the event/jsep strings to Jansson objects */
	json_error_t error;
	json_t *event = json_loads(event_text, 0, &error);
	if(!event) {
		JANUS_LOG(LOG_ERR, "JSON error: on line %d: %s", error.line, error.text);
		lua_pushnumber(s, -1);
		return 1;
	}
	/* Find the session (optional) */
	janus_mutex_lock(&sessions_mutex);
	janus_lua_session *session = g_hash_table_lookup(ids, GUINT_TO_POINTER(id));
	if(session != NULL)
		janus_refcount_increase(&session->ref);
	janus_mutex_unlock(&sessions_mutex);
	/* Notify the event */
	gateway->notify_event(&janus_lua_plugin, session ? session->handle : NULL, event);
	if(session != NULL)
		janus_refcount_decrease(&session->ref);
	lua_pushnumber(s, 0);
	return 1;
}

static int janus_lua_method_closepc(lua_State *s) {
	/* Get the arguments from the provided state */
	int n = lua_gettop(s);
	if(n != 1) {
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 1)", n);
		lua_pushnumber(s, -1);
		return 1;
	}
	guint32 id = lua_tonumber(s, 1);
	/* Find the session */
	janus_mutex_lock(&sessions_mutex);
	janus_lua_session *session = g_hash_table_lookup(ids, GUINT_TO_POINTER(id));
	if(session == NULL || g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&sessions_mutex);
		lua_pushnumber(s, -1);
		return 1;
	}
	janus_refcount_increase(&session->ref);
	janus_mutex_unlock(&sessions_mutex);
	/* We call close_pc from a thread, instead of calling it from here directly.
	 * In fact, a call to close_pc will result in the core invoking hangup_media
	 * synchronously, so from the same thread that originated the close_pc call.
	 * Since hangup_media tries to lock the Lua state mutex, in order to notify
	 * the Lua script, doing this without a thread would result in a deadlock. */
	janus_lua_async_event *asev = g_malloc0(sizeof(janus_lua_async_event));
	asev->session = session;
	asev->type = janus_lua_async_event_type_closepc;
	GError *error = NULL;
	g_thread_try_new("lua closepc", janus_lua_async_event_helper, asev, &error);
	if(error != NULL) {
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the Lua closepc thread...\n",
			error->code, error->message ? error->message : "??");
		janus_refcount_decrease(&session->ref);
		g_free(asev);
	}
	/* Return a success/error right away */
	lua_pushnumber(s, error ? 1 : 0);
	return 1;
}

static int janus_lua_method_configuremedium(lua_State *s) {
	/* Get the arguments from the provided state */
	int n = lua_gettop(s);
	if(n != 4) {
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 4)", n);
		lua_pushnumber(s, -1);
		return 1;
	}
	guint32 id = lua_tonumber(s, 1);
	const char *medium = lua_tostring(s, 2);
	const char *direction = lua_tostring(s, 3);
	int enabled = lua_toboolean(s, 4);
	/* Find the session */
	janus_mutex_lock(&sessions_mutex);
	janus_lua_session *session = g_hash_table_lookup(ids, GUINT_TO_POINTER(id));
	if(session == NULL || g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&sessions_mutex);
		lua_pushnumber(s, -1);
		return 1;
	}
	janus_refcount_increase(&session->ref);
	janus_mutex_unlock(&sessions_mutex);
	/* Modify the session media property */
	if(medium && direction) {
		if(!strcasecmp(medium, "audio")) {
			if(!strcasecmp(direction, "in")) {
				session->accept_audio = enabled ? TRUE : FALSE;
			} else {
				session->send_audio = enabled ? TRUE : FALSE;
			}
		} else if(!strcasecmp(medium, "video")) {
			if(!strcasecmp(direction, "in")) {
				session->accept_video = enabled ? TRUE : FALSE;
			} else {
				session->send_video = enabled ? TRUE : FALSE;
			}
		} else if(!strcasecmp(medium, "data")) {
			if(!strcasecmp(direction, "in")) {
				session->accept_data = enabled ? TRUE : FALSE;
			} else {
				session->send_data = enabled ? TRUE : FALSE;
			}
		}
	}
	janus_refcount_decrease(&session->ref);
	lua_pushnumber(s, 0);
	return 1;
}

static int janus_lua_method_addrecipient(lua_State *s) {
	/* Get the arguments from the provided state */
	int n = lua_gettop(s);
	if(n != 2) {
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 2)", n);
		lua_pushnumber(s, -1);
		return 1;
	}
	guint32 id = lua_tonumber(s, 1);
	guint32 rid = lua_tonumber(s, 2);
	/* Find the sessions */
	janus_mutex_lock(&sessions_mutex);
	janus_lua_session *session = g_hash_table_lookup(ids, GUINT_TO_POINTER(id));
	if(session == NULL || g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&sessions_mutex);
		lua_pushnumber(s, -1);
		return 1;
	}
	janus_refcount_increase(&session->ref);
	janus_lua_session *recipient = g_hash_table_lookup(ids, GUINT_TO_POINTER(rid));
	if(recipient == NULL || g_atomic_int_get(&recipient->destroyed)) {
		janus_refcount_decrease(&session->ref);
		janus_mutex_unlock(&sessions_mutex);
		lua_pushnumber(s, -1);
		return 1;
	}
	janus_refcount_increase(&recipient->ref);
	/* Add to the list of recipients */
	janus_mutex_lock(&session->recipients_mutex);
	janus_mutex_unlock(&sessions_mutex);
	if(g_slist_find(session->recipients, recipient) == NULL) {
		janus_refcount_increase(&session->ref);
		janus_refcount_increase(&recipient->ref);
		session->recipients = g_slist_append(session->recipients, recipient);
	}
	janus_mutex_unlock(&session->recipients_mutex);
	/* Done */
	janus_refcount_decrease(&session->ref);
	janus_refcount_decrease(&recipient->ref);
	lua_pushnumber(s, 0);
	return 1;
}

static int janus_lua_method_removerecipient(lua_State *s) {
	/* Get the arguments from the provided state */
	int n = lua_gettop(s);
	if(n != 2) {
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 2)", n);
		lua_pushnumber(s, -1);
		return 1;
	}
	guint32 id = lua_tonumber(s, 1);
	guint32 rid = lua_tonumber(s, 2);
	/* Find the sessions */
	janus_mutex_lock(&sessions_mutex);
	janus_lua_session *session = g_hash_table_lookup(ids, GUINT_TO_POINTER(id));
	if(session == NULL) {
		janus_mutex_unlock(&sessions_mutex);
		lua_pushnumber(s, -1);
		return 1;
	}
	janus_refcount_increase(&session->ref);
	janus_lua_session *recipient = g_hash_table_lookup(ids, GUINT_TO_POINTER(rid));
	if(recipient == NULL) {
		janus_refcount_decrease(&session->ref);
		janus_mutex_unlock(&sessions_mutex);
		lua_pushnumber(s, -1);
		return 1;
	}
	janus_refcount_increase(&recipient->ref);
	/* Remove from the list of recipients */
	janus_mutex_lock(&session->recipients_mutex);
	janus_mutex_unlock(&sessions_mutex);
	gboolean unref = FALSE;
	if(g_slist_find(session->recipients, recipient) != NULL) {
		session->recipients = g_slist_remove(session->recipients, recipient);
		unref = TRUE;
	}
	janus_mutex_unlock(&session->recipients_mutex);
	if(unref) {
		janus_refcount_decrease(&session->ref);
		janus_refcount_decrease(&recipient->ref);
	}
	/* Done */
	janus_refcount_decrease(&session->ref);
	janus_refcount_decrease(&recipient->ref);
	lua_pushnumber(s, 0);
	return 1;
}

static int janus_lua_method_setbitrate(lua_State *s) {
	/* Get the arguments from the provided state */
	int n = lua_gettop(s);
	if(n != 2) {
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 2)", n);
		lua_pushnumber(s, -1);
		return 1;
	}
	guint32 id = lua_tonumber(s, 1);
	guint32 bitrate = lua_tonumber(s, 2);
	/* Find the session */
	janus_mutex_lock(&sessions_mutex);
	janus_lua_session *session = g_hash_table_lookup(ids, GUINT_TO_POINTER(id));
	if(session == NULL || g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&sessions_mutex);
		lua_pushnumber(s, -1);
		return 1;
	}
	janus_refcount_increase(&session->ref);
	janus_mutex_unlock(&sessions_mutex);
	session->bitrate = bitrate;
	/* Send a REMB right away too, if the PeerConnection is up */
	if(session->bitrate > 0 && g_atomic_int_get(&session->started)) {
		char rtcpbuf[24];
		janus_rtcp_remb((char *)(&rtcpbuf), 24, session->bitrate);
		gateway->relay_rtcp(session->handle, 1, rtcpbuf, 24);
	}
	/* Done */
	janus_refcount_decrease(&session->ref);
	lua_pushnumber(s, 0);
	return 1;
}

static int janus_lua_method_setplifreq(lua_State *s) {
	/* Get the arguments from the provided state */
	int n = lua_gettop(s);
	if(n != 2) {
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 2)", n);
		lua_pushnumber(s, -1);
		return 1;
	}
	guint32 id = lua_tonumber(s, 1);
	guint16 pli_freq = lua_tonumber(s, 2);
	/* Find the session */
	janus_mutex_lock(&sessions_mutex);
	janus_lua_session *session = g_hash_table_lookup(ids, GUINT_TO_POINTER(id));
	if(session == NULL || g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&sessions_mutex);
		lua_pushnumber(s, -1);
		return 1;
	}
	janus_refcount_increase(&session->ref);
	janus_mutex_unlock(&sessions_mutex);
	session->pli_freq = pli_freq;
	/* Done */
	janus_refcount_decrease(&session->ref);
	lua_pushnumber(s, 0);
	return 1;
}

static int janus_lua_method_sendpli(lua_State *s) {
	/* Get the arguments from the provided state */
	int n = lua_gettop(s);
	if(n != 1) {
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 1)", n);
		lua_pushnumber(s, -1);
		return 1;
	}
	guint32 id = lua_tonumber(s, 1);
	/* Find the session */
	janus_mutex_lock(&sessions_mutex);
	janus_lua_session *session = g_hash_table_lookup(ids, GUINT_TO_POINTER(id));
	if(session == NULL || g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&sessions_mutex);
		lua_pushnumber(s, -1);
		return 1;
	}
	janus_refcount_increase(&session->ref);
	janus_mutex_unlock(&sessions_mutex);
	/* Send a PLI */
	session->pli_latest = janus_get_monotonic_time();
	char rtcpbuf[12];
	janus_rtcp_pli((char *)&rtcpbuf, 12);
	JANUS_LOG(LOG_HUGE, "Sending PLI to session %"SCNu32"\n", session->id);
	gateway->relay_rtcp(session->handle, 1, rtcpbuf, 12);
	/* Done */
	janus_refcount_decrease(&session->ref);
	lua_pushnumber(s, 0);
	return 1;
}

static int janus_lua_method_relayrtp(lua_State *s) {
	/* Get the arguments from the provided state */
	int n = lua_gettop(s);
	if(n != 4) {
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 4)", n);
		lua_pushnumber(s, -1);
		return 1;
	}
	guint32 id = lua_tonumber(s, 1);
	int is_video = lua_toboolean(s, 2);
	const char *payload = lua_tostring(s, 3);
	int len = lua_tonumber(s, 4);
	if(!payload || len < 1) {
		JANUS_LOG(LOG_ERR, "Invalid payload\n");
		lua_pushnumber(s, -1);
		return 1;
	}
	/* Find the session */
	janus_mutex_lock(&sessions_mutex);
	janus_lua_session *session = g_hash_table_lookup(ids, GUINT_TO_POINTER(id));
	if(session == NULL || g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&sessions_mutex);
		lua_pushnumber(s, -1);
		return 1;
	}
	janus_mutex_unlock(&sessions_mutex);
	/* Send the RTP packet */
	gateway->relay_rtp(session->handle, is_video, (char *)payload, len);
	lua_pushnumber(s, 0);
	return 1;
}

static int janus_lua_method_relayrtcp(lua_State *s) {
	/* Get the arguments from the provided state */
	int n = lua_gettop(s);
	if(n != 4) {
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 4)", n);
		lua_pushnumber(s, -1);
		return 1;
	}
	guint32 id = lua_tonumber(s, 1);
	int is_video = lua_toboolean(s, 2);
	const char *payload = lua_tostring(s, 3);
	int len = lua_tonumber(s, 4);
	if(!payload || len < 1) {
		JANUS_LOG(LOG_ERR, "Invalid payload\n");
		lua_pushnumber(s, -1);
		return 1;
	}
	/* Find the session */
	janus_mutex_lock(&sessions_mutex);
	janus_lua_session *session = g_hash_table_lookup(ids, GUINT_TO_POINTER(id));
	if(session == NULL || g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&sessions_mutex);
		lua_pushnumber(s, -1);
		return 1;
	}
	janus_mutex_unlock(&sessions_mutex);
	/* Send the RTCP packet */
	gateway->relay_rtcp(session->handle, is_video, (char *)payload, len);
	lua_pushnumber(s, 0);
	return 1;
}

static int janus_lua_method_relaydata(lua_State *s) {
	/* Get the arguments from the provided state */
	int n = lua_gettop(s);
	if(n != 3) {
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 2)", n);
		lua_pushnumber(s, -1);
		return 1;
	}
	guint32 id = lua_tonumber(s, 1);
	const char *payload = lua_tostring(s, 2);
	int len = lua_tonumber(s, 3);
	if(!payload || len < 1) {
		JANUS_LOG(LOG_ERR, "Invalid data\n");
		lua_pushnumber(s, -1);
		return 1;
	}
	/* Find the session */
	janus_mutex_lock(&sessions_mutex);
	janus_lua_session *session = g_hash_table_lookup(ids, GUINT_TO_POINTER(id));
	if(session == NULL || g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&sessions_mutex);
		lua_pushnumber(s, -1);
		return 1;
	}
	janus_refcount_increase(&session->ref);
	janus_mutex_unlock(&sessions_mutex);
	/* Send the RTP packet */
	gateway->relay_data(session->handle, (char *)payload, len);
	janus_refcount_decrease(&session->ref);
	lua_pushnumber(s, 0);
	return 1;
}

static int janus_lua_method_startrecording(lua_State *s) {
	/* Get the arguments from the provided state */
	int n = lua_gettop(s);
	if(n != 5 && n != 9 && n != 13) {
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 5, 9 or 13)", n);
		lua_pushnumber(s, -1);
		return 1;
	}
	guint32 id = lua_tonumber(s, 1);
	/* Find the session */
	janus_mutex_lock(&sessions_mutex);
	janus_lua_session *session = g_hash_table_lookup(ids, GUINT_TO_POINTER(id));
	if(session == NULL || g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&sessions_mutex);
		lua_pushnumber(s, -1);
		return 1;
	}
	janus_refcount_increase(&session->ref);
	janus_mutex_lock(&session->rec_mutex);
	janus_mutex_unlock(&sessions_mutex);
	/* Iterate on all arguments, to see what we're being asked to record */
	n--;
	int i = 1;
	janus_recorder *arc = NULL, *vrc = NULL, *drc = NULL;
	while(n > 0) {
		i++; n--;
		const char *type = lua_tostring(s, i);
		i++; n--;
		const char *codec = lua_tostring(s, i);
		i++; n--;
		const char *folder = lua_tostring(s, i);
		i++; n--;
		const char *filename = lua_tostring(s, i);
		janus_recorder *rc = janus_recorder_create(folder, codec, filename);
		if(rc == NULL) {
			JANUS_LOG(LOG_ERR, "Error creating '%s' recorder...\n", type);
			goto error;
		}
		if(!strcasecmp(type, "audio")) {
			if(arc != NULL || session->arc != NULL) {
				JANUS_LOG(LOG_ERR, "Duplicate audio recording\n");
				goto error;
			}
			arc = rc;
		} else if(!strcasecmp(type, "video")) {
			if(vrc != NULL || session->vrc != NULL) {
				JANUS_LOG(LOG_ERR, "Duplicate video recording\n");
				goto error;
			}
			vrc = rc;
		} else if(!strcasecmp(type, "data")) {
			if(drc != NULL || session->drc != NULL) {
				JANUS_LOG(LOG_ERR, "Duplicate data recording\n");
				goto error;
			}
			drc = rc;
		}
	}
	if(arc)
		session->arc = arc;
	if(vrc)
		session->vrc = vrc;
	if(drc)
		session->drc = drc;
	janus_refcount_decrease(&session->ref);
	goto done;

error:
	janus_recorder_free(arc);
	janus_recorder_free(vrc);
	janus_recorder_free(drc);
	janus_mutex_unlock(&session->rec_mutex);
	/* Something went wrong */
	janus_refcount_decrease(&session->ref);
	lua_pushnumber(s, -1);
	return 1;

done:
	janus_mutex_unlock(&session->rec_mutex);
	/* Done */
	lua_pushnumber(s, 0);
	return 1;
}

static int janus_lua_method_stoprecording(lua_State *s) {
	/* Get the arguments from the provided state */
	int n = lua_gettop(s);
	if(n != 2 && n != 3 && n != 4) {
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 2, 3 or 4)", n);
		lua_pushnumber(s, -1);
		return 1;
	}
	guint32 id = lua_tonumber(s, 1);
	/* Find the session */
	janus_mutex_lock(&sessions_mutex);
	janus_lua_session *session = g_hash_table_lookup(ids, GUINT_TO_POINTER(id));
	if(session == NULL || g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&sessions_mutex);
		lua_pushnumber(s, -1);
		return 1;
	}
	janus_refcount_increase(&session->ref);
	janus_mutex_lock(&session->rec_mutex);
	janus_mutex_unlock(&sessions_mutex);
	/* Iterate on all arguments, to see what which recording we're being asked to stop */
	n--;
	int i = 1;
	while(n > 0) {
		i++; n--;
		const char *type = lua_tostring(s, i);
		if(!strcasecmp(type, "audio")) {
			if(session->arc != NULL) {
				janus_recorder_close(session->arc);
				janus_recorder_free(session->arc);
				session->arc = NULL;
			}
		} else if(!strcasecmp(type, "video")) {
			if(session->vrc != NULL) {
				janus_recorder_close(session->vrc);
				janus_recorder_free(session->vrc);
				session->vrc = NULL;
			}
		} else if(!strcasecmp(type, "data")) {
			if(session->drc != NULL) {
				janus_recorder_close(session->drc);
				janus_recorder_free(session->drc);
				session->drc = NULL;
			}
		}
	}
	janus_mutex_unlock(&session->rec_mutex);
	/* Done */
	janus_refcount_decrease(&session->ref);
	lua_pushnumber(s, 0);
	return 1;
}


/* Plugin implementation */
int janus_lua_init(janus_callbacks *callback, const char *config_path) {
	if(g_atomic_int_get(&stopping)) {
		/* Still stopping from before */
		return -1;
	}
	if(callback == NULL || config_path == NULL) {
		/* Invalid arguments */
		return -1;
	}

	/* Read configuration */
	char filename[255];
	g_snprintf(filename, 255, "%s/%s.cfg", config_path, JANUS_LUA_PACKAGE);
	JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
	janus_config *config = janus_config_parse(filename);
	if(config == NULL) {
		/* No config means no Lua script */
		JANUS_LOG(LOG_ERR, "Failed to load configuration file for Lua plugin...\n");
		return -1;
	}
	janus_config_print(config);
	char *lua_folder = NULL;
	janus_config_item *folder = janus_config_get_item_drilldown(config, "general", "path");
	if(folder && folder->value)
		lua_folder = g_strdup(folder->value);
	janus_config_item *script = janus_config_get_item_drilldown(config, "general", "script");
	if(script == NULL && script->value == NULL) {
		JANUS_LOG(LOG_ERR, "Missing script path in Lua plugin configuration...\n");
		janus_config_destroy(config);
		g_free(lua_folder);
		return -1;
	}
	char *lua_file = g_strdup(script->value);
	char *lua_config = NULL;
	janus_config_item *conf = janus_config_get_item_drilldown(config, "general", "config");
	if(conf && conf->value)
		lua_config = g_strdup(conf->value);
	janus_config_destroy(config);

	/* Initialize Lua */
	state = luaL_newstate();
	luaL_openlibs(state);

	if(lua_folder != NULL) {
		/* Add the script folder to the path, so that we can load other scripts from there */
		lua_getglobal(state, "package");
		lua_getfield(state, -1, "path");
		const char *cur_path = lua_tostring(state, -1);
		char new_path[1024];
		memset(new_path, 0, sizeof(new_path));
		g_snprintf(new_path, sizeof(new_path), "%s;%s/?.lua", cur_path, lua_folder);
		lua_pop(state, 1);
		lua_pushstring(state, new_path);
		lua_setfield(state, -2, "path");
		lua_pop(state, 1);
	}

	/* Register our functions */
	lua_register(state, "pokeScheduler", janus_lua_method_pokescheduler);
	lua_register(state, "pushEvent", janus_lua_method_pushevent);
	lua_register(state, "notifyEvent", janus_lua_method_notifyevent);
	lua_register(state, "closePc", janus_lua_method_closepc);
	lua_register(state, "configureMedium", janus_lua_method_configuremedium);
	lua_register(state, "addRecipient", janus_lua_method_addrecipient);
	lua_register(state, "removeRecipient", janus_lua_method_removerecipient);
	lua_register(state, "setBitrate", janus_lua_method_setbitrate);
	lua_register(state, "setPliFreq", janus_lua_method_setplifreq);
	lua_register(state, "sendPli", janus_lua_method_sendpli);
	lua_register(state, "relayRtp", janus_lua_method_relayrtp);
	lua_register(state, "relayRtcp", janus_lua_method_relayrtcp);
	lua_register(state, "relayData", janus_lua_method_relaydata);
	lua_register(state, "startRecording", janus_lua_method_startrecording);
	lua_register(state, "stopRecording", janus_lua_method_stoprecording);
	/* Register all extra functions, if any were added */
	janus_lua_register_extra_functions(state);

	/* Now load the script */
	int err = luaL_dofile(state, lua_file);
	if(err) {
		JANUS_LOG(LOG_ERR, "Error loading Lua script %s: %s\n", lua_file, lua_tostring(state, -1));
		lua_close(state);
		g_free(lua_folder);
		g_free(lua_file);
		return -1;
	}
	/* Make sure that all the functions we need are there */
	uint i=0;
	for(i=0; i<lua_funcsize; i++) {
		lua_getglobal(state, lua_functions[i]);
		if(lua_isfunction(state, lua_gettop(state)) == 0) {
			JANUS_LOG(LOG_ERR, "Function '%s' is missing in %s\n", lua_functions[i], lua_file);
			lua_close(state);
			g_free(lua_folder);
			g_free(lua_file);
			return -1;
		}
	}
	/* Some Lua functions are optional, e.g., those to directly handle RTP, RTCP and data,
	 * as those will typically be kept at a C level, with Lua only dictating the logic */
	lua_getglobal(state, "incomingRtp");
	if(lua_isfunction(state, lua_gettop(state)) != 0)
		has_incoming_rtp = TRUE;
	lua_getglobal(state, "incomingRtcp");
	if(lua_isfunction(state, lua_gettop(state)) != 0)
		has_incoming_rtcp = TRUE;
	lua_getglobal(state, "incomingData");
	if(lua_isfunction(state, lua_gettop(state)) != 0)
		has_incoming_data = TRUE;
	/* Init the Lua script, in case it's needed */
	lua_State *t = lua_newthread(state);
	lua_getglobal(t, "init");
	lua_pushstring(t, lua_config);
	lua_call(t, 1, 0);

	sessions = g_hash_table_new_full(NULL, NULL, NULL, (GDestroyNotify)janus_lua_session_destroy);
	ids = g_hash_table_new(NULL, NULL);
	events = g_async_queue_new();

	/* Launch the scheduler thread (which will be responsible for resuming asynchronous coroutines) */
	GError *error = NULL;
	scheduler_thread = g_thread_try_new("lua scheduler", janus_lua_scheduler, NULL, &error);
	if(error != NULL) {
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the Lua scheduler thread...\n",
			error->code, error->message ? error->message : "??");
		lua_close(state);
		g_free(lua_folder);
		g_free(lua_file);
		g_free(lua_config);
		return -1;
	}

	g_free(lua_folder);
	g_free(lua_file);
	g_free(lua_config);

	/* This is the callback we'll need to invoke to contact the gateway */
	gateway = callback;
	g_atomic_int_set(&initialized, 1);

	JANUS_LOG(LOG_INFO, "%s initialized!\n", JANUS_LUA_NAME);
	return 0;
}

void janus_lua_destroy(void) {
	if(!g_atomic_int_get(&initialized))
		return;
	g_atomic_int_set(&stopping, 1);

	g_async_queue_push(events, GUINT_TO_POINTER(janus_lua_event_exit));
	if(scheduler_thread != NULL) {
		g_thread_join(scheduler_thread);
		scheduler_thread = NULL;
	}

	/* Deinit the Lua script, in case it's needed */
	janus_mutex_lock(&lua_mutex);
	lua_State *t = lua_newthread(state);
	lua_getglobal(t, "destroy");
	lua_call(t, 0, 0);
	janus_mutex_unlock(&lua_mutex);

	janus_mutex_lock(&sessions_mutex);
	g_hash_table_destroy(sessions);
	sessions = NULL;
	g_hash_table_destroy(ids);
	ids = NULL;
	g_async_queue_unref(events);
	events = NULL;
	janus_mutex_unlock(&sessions_mutex);

	janus_mutex_lock(&lua_mutex);
	lua_close(state);
	state = NULL;
	janus_mutex_unlock(&lua_mutex);

	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);
	JANUS_LOG(LOG_INFO, "%s destroyed!\n", JANUS_LUA_NAME);
}

int janus_lua_get_api_compatibility(void) {
	/* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
	return JANUS_PLUGIN_API_VERSION;
}

int janus_lua_get_version(void) {
	return JANUS_LUA_VERSION;
}

const char *janus_lua_get_version_string(void) {
	return JANUS_LUA_VERSION_STRING;
}

const char *janus_lua_get_description(void) {
	return JANUS_LUA_DESCRIPTION;
}

const char *janus_lua_get_name(void) {
	return JANUS_LUA_NAME;
}

const char *janus_lua_get_author(void) {
	return JANUS_LUA_AUTHOR;
}

const char *janus_lua_get_package(void) {
	return JANUS_LUA_PACKAGE;
}

void janus_lua_create_session(janus_plugin_session *handle, int *error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}	
	janus_mutex_lock(&sessions_mutex);
	guint32 id = 0;
	while(id == 0) {
		id = janus_random_uint32();
		if(g_hash_table_lookup(ids, GUINT_TO_POINTER(id))) {
			id = 0;
			continue;
		}
	}
	JANUS_LOG(LOG_VERB, "Creating new Lua session %"SCNu32"...\n", id);
	janus_lua_session *session = (janus_lua_session *)g_malloc0(sizeof(janus_lua_session));
	session->handle = handle;
	session->id = id;
	janus_rtp_switching_context_reset(&session->rtpctx);
	g_atomic_int_set(&session->hangingup, 0);
	g_atomic_int_set(&session->destroyed, 0);
	janus_refcount_init(&session->ref, janus_lua_session_free);
	handle->plugin_handle = session;
	g_hash_table_insert(sessions, handle, session);
	g_hash_table_insert(ids, GUINT_TO_POINTER(session->id), session);
	janus_mutex_unlock(&sessions_mutex);

	/* Notify the Lua script */
	janus_mutex_lock(&lua_mutex);
	lua_State *t = lua_newthread(state);
	lua_getglobal(t, "createSession");
	lua_pushnumber(t, session->id);
	lua_call(t, 1, 0);
	janus_mutex_unlock(&lua_mutex);

	return;
}

void janus_lua_destroy_session(janus_plugin_session *handle, int *error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}	
	janus_lua_session *session = (janus_lua_session *)handle->plugin_handle;
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		*error = -2;
		return;
	}
	guint32 id = session->id;
	JANUS_LOG(LOG_VERB, "Removing Lua session %"SCNu32"...\n", id);
	janus_mutex_lock(&sessions_mutex);
	g_hash_table_remove(sessions, handle);
	janus_mutex_unlock(&sessions_mutex);

	/* Notify the Lua script */
	janus_mutex_lock(&lua_mutex);
	lua_State *t = lua_newthread(state);
	lua_getglobal(t, "destroySession");
	lua_pushnumber(t, id);
	lua_call(t, 1, 0);
	janus_mutex_unlock(&lua_mutex);

	return;
}

json_t *janus_lua_query_session(janus_plugin_session *handle) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		return NULL;
	}	
	janus_lua_session *session = (janus_lua_session *)handle->plugin_handle;
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return NULL;
	}
	/* Ask the Lua script for information on this session */
	janus_mutex_lock(&lua_mutex);
	lua_State *t = lua_newthread(state);
	lua_getglobal(t, "querySession");
	lua_pushnumber(t, session->id);
	lua_call(t, 1, 1);
	const char *info = lua_tostring(t, -1);
	lua_pop(t, 1);
	/* We need a Jansson object */
	json_error_t error;
	json_t *json = json_loads(info, 0, &error);
	janus_mutex_unlock(&lua_mutex);
	if(!json) {
		JANUS_LOG(LOG_ERR, "JSON error: on line %d: %s", error.line, error.text);
		return NULL;
	}
	return json;
}

struct janus_plugin_result *janus_lua_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return janus_plugin_result_new(JANUS_PLUGIN_ERROR, g_atomic_int_get(&stopping) ? "Shutting down" : "Plugin not initialized", NULL);
	janus_lua_session *session = (janus_lua_session *)handle->plugin_handle;
	if(!session)
		return janus_plugin_result_new(JANUS_PLUGIN_ERROR, "No session associated with this handle", NULL);

	/* Processing the message is up to the Lua script: serialize the Jansson objects to strings */
	char *message_text = message ? json_dumps(message, JSON_INDENT(0) | JSON_PRESERVE_ORDER) : NULL;
	json_decref(message);
	char *jsep_text = jsep ? json_dumps(jsep, JSON_INDENT(0) | JSON_PRESERVE_ORDER) : NULL;
	json_decref(jsep);
	/* Invoke the script function */
	janus_mutex_lock(&lua_mutex);
	lua_State *t = lua_newthread(state);
	lua_getglobal(t, "handleMessage");
	lua_pushnumber(t, session->id);
	lua_pushstring(t, transaction);
	lua_pushstring(t, message_text);
	lua_pushstring(t, jsep_text);
	lua_call(t, 4, 2);
	int n = lua_gettop(t);
	if(n != 2) {
		janus_mutex_unlock(&lua_mutex);
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 2)", n);
		return janus_plugin_result_new(JANUS_PLUGIN_ERROR, "Lua error", NULL);
	}
	/* Check if this is a synchronous or asynchronous response */
	int res = (int)lua_tonumber(t, 1);
	const char *response = lua_tostring(t, 2);
	if(res < 0) {
		/* We got an error */
		janus_mutex_unlock(&lua_mutex);
		return janus_plugin_result_new(JANUS_PLUGIN_ERROR, response ? response : "Lua error", NULL);
	} else if(res == 0) {
		/* Synchronous response: we need a Jansson object */
		json_error_t error;
		json_t *json = json_loads(response, 0, &error);
		janus_mutex_unlock(&lua_mutex);
		if(!json) {
			JANUS_LOG(LOG_ERR, "JSON error: on line %d: %s\n", error.line, error.text);
			return janus_plugin_result_new(JANUS_PLUGIN_ERROR, "Lua error", NULL);
		}
		return janus_plugin_result_new(JANUS_PLUGIN_OK, NULL, json);
	}
	janus_mutex_unlock(&lua_mutex);
	/* If we got here, it's an asynchronous response */
	return janus_plugin_result_new(JANUS_PLUGIN_OK_WAIT, NULL, NULL);
}

void janus_lua_setup_media(janus_plugin_session *handle) {
	JANUS_LOG(LOG_INFO, "WebRTC media is now available\n");
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_lua_session *session = (janus_lua_session *)handle->plugin_handle;	
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(g_atomic_int_get(&session->destroyed))
		return;
	g_atomic_int_set(&session->hangingup, 0);
	g_atomic_int_set(&session->started, 1);
	session->pli_latest = janus_get_monotonic_time();

	/* Notify the Lua script */
	janus_mutex_lock(&lua_mutex);
	lua_State *t = lua_newthread(state);
	lua_getglobal(t, "setupMedia");
	lua_pushnumber(t, session->id);
	lua_call(t, 1, 0);
	janus_mutex_unlock(&lua_mutex);
}

void janus_lua_incoming_rtp(janus_plugin_session *handle, int video, char *buf, int len) {
	if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_lua_session *session = (janus_lua_session *)handle->plugin_handle;	
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(g_atomic_int_get(&session->destroyed) || g_atomic_int_get(&session->hangingup))
		return;
	/* Check if the Lua script wants to handle/manipulate RTP packets itself */
	if(has_incoming_rtp) {
		/* Yep, pass the data to the Lua script and return */
		janus_mutex_lock(&lua_mutex);
		lua_State *t = lua_newthread(state);
		lua_pushnumber(t, session->id);
		lua_pushboolean(t, video);
		lua_pushlstring(t, buf, len);
		lua_pushnumber(t, len);
		lua_call(t, 4, 0);
		janus_mutex_unlock(&lua_mutex);
		return;
	}
	/* Is this session allowed to send media? */
	if((video && !session->send_video) || (!video && !session->send_audio))
		return;
	/* Are we recording? */
	janus_recorder_save_frame(video ? session->vrc : session->arc, buf, len);
	/* Handle the packet */
	rtp_header *rtp = (rtp_header *)buf;
	janus_lua_rtp_relay_packet packet;
	packet.data = rtp;
	packet.length = len;
	packet.is_video = video;
	/* Backup the actual timestamp and sequence number set by the publisher, in case switching is involved */
	packet.timestamp = ntohl(packet.data->timestamp);
	packet.seq_number = ntohs(packet.data->seq_number);
	/* Relay to all recipients */
	janus_mutex_lock_nodebug(&session->recipients_mutex);
	g_slist_foreach(session->recipients, janus_lua_relay_rtp_packet, &packet);
	janus_mutex_unlock_nodebug(&session->recipients_mutex);

	/* Check if we need to send any PLI to this media source */
	if(video && session->pli_freq > 0) {
		/* We send a FIR every tot seconds, depending on what the Lua script configured */
		gint64 now = janus_get_monotonic_time();
		if((now-session->pli_latest) >= ((gint64)session->pli_freq*G_USEC_PER_SEC)) {
			session->pli_latest = now;
			char rtcpbuf[12];
			janus_rtcp_pli((char *)&rtcpbuf, 12);
			JANUS_LOG(LOG_HUGE, "Sending PLI to session %"SCNu32"\n", session->id);
			gateway->relay_rtcp(handle, 1, rtcpbuf, 12);
		}
	}
}

void janus_lua_incoming_rtcp(janus_plugin_session *handle, int video, char *buf, int len) {
	if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_lua_session *session = (janus_lua_session *)handle->plugin_handle;
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(g_atomic_int_get(&session->destroyed) || g_atomic_int_get(&session->hangingup))
		return;
	/* Check if the Lua script wants to handle/manipulate RTCP packets itself */
	if(has_incoming_rtcp) {
		/* Yep, pass the data to the Lua script and return */
		janus_mutex_lock(&lua_mutex);
		lua_State *t = lua_newthread(state);
		lua_getglobal(t, "incomingRtcp");
		lua_pushnumber(t, session->id);
		lua_pushboolean(t, video);
		lua_pushlstring(t, buf, len);
		lua_pushnumber(t, len);
		lua_call(t, 4, 0);
		janus_mutex_unlock(&lua_mutex);
		return;
	}
	/* If a REMB arrived, make sure we cap it to our configuration, and send it as a video RTCP */
	guint32 bitrate = janus_rtcp_get_remb(buf, len);
	if(bitrate > 0) {
		if(session->bitrate > 0) {
			char rtcpbuf[24];
			janus_rtcp_remb((char *)(&rtcpbuf), 24, session->bitrate);
			gateway->relay_rtcp(handle, 1, rtcpbuf, 24);
		} else {
			gateway->relay_rtcp(handle, 1, buf, len);
		}
		return;
	}
}

void janus_lua_incoming_data(janus_plugin_session *handle, char *buf, int len) {
	if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_lua_session *session = (janus_lua_session *)handle->plugin_handle;
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(g_atomic_int_get(&session->destroyed) || g_atomic_int_get(&session->hangingup))
		return;
	/* Check if the Lua script wants to handle/manipulate data channel packets itself */
	if(has_incoming_data) {
		/* Yep, pass the data to the Lua script and return */
		janus_mutex_lock(&lua_mutex);
		lua_State *t = lua_newthread(state);
		lua_getglobal(t, "incomingData");
		lua_pushnumber(t, session->id);
		lua_pushlstring(t, buf, len);
		lua_pushnumber(t, len);
		lua_call(t, 3, 0);
		janus_mutex_unlock(&lua_mutex);
		return;
	}
	/* Is this session allowed to send data? */
	if(!session->send_data)
		return;
	/* Are we recording? */
	janus_recorder_save_frame(session->drc, buf, len);
	/* Get a string out of the data */
	char *text = g_malloc0(len+1);
	if(text == NULL) {
		JANUS_LOG(LOG_FATAL, "Memory error!\n");
		return;
	}
	memcpy(text, buf, len);
	*(text+len) = '\0';
	JANUS_LOG(LOG_VERB, "Got a DataChannel message (%zu bytes) to forward: %s\n", strlen(text), text);
	/* Relay to all recipients */
	janus_mutex_lock_nodebug(&session->recipients_mutex);
	g_slist_foreach(session->recipients, janus_lua_relay_data_packet, text);
	janus_mutex_unlock_nodebug(&session->recipients_mutex);
	g_free(text);
}

void janus_lua_slow_link(janus_plugin_session *handle, int uplink, int video) {
	if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_lua_session *session = (janus_lua_session *)handle->plugin_handle;
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(g_atomic_int_get(&session->destroyed) || g_atomic_int_get(&session->hangingup))
		return;
	/* TODO Handle feedback depending on the logic the Lua script dictated */
}

void janus_lua_hangup_media(janus_plugin_session *handle) {
	JANUS_LOG(LOG_INFO, "No WebRTC media anymore\n");
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_lua_session *session = (janus_lua_session *)handle->plugin_handle;
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(g_atomic_int_get(&session->destroyed))
		return;
	if(g_atomic_int_add(&session->hangingup, 1))
		return;
	g_atomic_int_set(&session->started, 0);

	/* Reset the media properties */
	session->accept_audio = FALSE;
	session->accept_video = FALSE;
	session->accept_data = FALSE;
	session->send_audio = FALSE;
	session->send_video = FALSE;
	session->send_data = FALSE;
	session->bitrate = 0;
	session->pli_freq = 0;
	session->pli_latest = 0;
	janus_rtp_switching_context_reset(&session->rtpctx);

	/* Get rid of the recipients */
	janus_mutex_lock(&session->recipients_mutex);
	while(session->recipients) {
		janus_lua_session *recipient = (janus_lua_session *)session->recipients->data;
		session->recipients = g_slist_remove(session->recipients, recipient);
		janus_refcount_decrease(&session->ref);
		janus_refcount_decrease(&recipient->ref);
	}
	janus_mutex_unlock(&session->recipients_mutex);

	/* Notify the Lua script */
	janus_mutex_lock(&lua_mutex);
	lua_State *t = lua_newthread(state);
	lua_getglobal(t, "hangupMedia");
	lua_pushnumber(t, session->id);
	lua_call(t, 1, 0);
	janus_mutex_unlock(&lua_mutex);
}

/* Helpers to quickly relay RTP and data packets to the intended recipients */
static void janus_lua_relay_rtp_packet(gpointer data, gpointer user_data) {
	janus_lua_rtp_relay_packet *packet = (janus_lua_rtp_relay_packet *)user_data;
	if(!packet || !packet->data || packet->length < 1) {
		JANUS_LOG(LOG_ERR, "Invalid packet...\n");
		return;
	}
	janus_lua_session *session = (janus_lua_session *)data;
	if(!session || !session->handle || !g_atomic_int_get(&session->started)) {
		return;
	}
	
	/* Check if this recipient is willing/allowed to receive this medium */
	if((packet->is_video && !session->accept_video) || (!packet->is_video && !session->accept_audio)) {
		/* Nope, don't relay */
		return;
	}
	/* Fix sequence number and timestamp (publisher switching may be involved) */
	janus_rtp_header_update(packet->data, &session->rtpctx, packet->is_video, packet->is_video ? 4500 : 960);
	/* Send the packet */
	if(gateway != NULL)
		gateway->relay_rtp(session->handle, packet->is_video, (char *)packet->data, packet->length);
	/* Restore the timestamp and sequence number to what the publisher set them to */
	packet->data->timestamp = htonl(packet->timestamp);
	packet->data->seq_number = htons(packet->seq_number);

	return;
}

static void janus_lua_relay_data_packet(gpointer data, gpointer user_data) {
	janus_lua_session *session = (janus_lua_session *)data;
	if(!session || !session->handle || !g_atomic_int_get(&session->started) || !session->accept_data) {
		return;
	}
	char *text = (char *)user_data;
	if(gateway != NULL && text != NULL) {
		JANUS_LOG(LOG_VERB, "Forwarding DataChannel message (%zu bytes) to session %"SCNu32": %s\n",
			strlen(text), session->id, text);
		gateway->relay_data(session->handle, text, strlen(text));
	}
	return;
}

/* This is a scheduler thread: if we know there are coroutines to resume
 * in Lua (e.g., for asynchronous requests), we do that ourselves here */
static void *janus_lua_scheduler(void *data) {
	JANUS_LOG(LOG_VERB, "Joining Lua scheduler thread\n");
	janus_lua_event *event = NULL;
	/* Wait until there are events to process */
	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		event = g_async_queue_pop(events);
		if(event == GUINT_TO_POINTER(janus_lua_event_exit))
			break;
		if(event == GUINT_TO_POINTER(janus_lua_event_resume)) {
			/* There are coroutines to resume */
			janus_mutex_lock(&lua_mutex);
			lua_State *t = lua_newthread(state);
			lua_getglobal(t, "resumeScheduler");
			lua_call(t, 0, 0);
			janus_mutex_unlock(&lua_mutex);
		}
	}
	JANUS_LOG(LOG_VERB, "Leaving Lua scheduler thread\n");
	return NULL;
}
