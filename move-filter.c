#include "move-transition.h"
#include "obs-frontend-api.h"
#include <util/dstr.h>
#include <util/darray.h>
#include <util/threading.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Mstcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define closesocket(s) close(s)
#endif

struct udp_server {
	int port;
	SOCKET socket;
	pthread_t thread;
	bool finished;
	DARRAY(struct move_filter *) start_triggers;
	DARRAY(struct move_filter *) stop_triggers;
};

static DARRAY(struct udp_server *) udp_servers;
static pthread_mutex_t udp_servers_mutex;
static bool udp_shutdown;

struct udp_trigger {
	obs_source_t *source;
	bool start;
};

static void udp_servers_reap(void);
static void udp_update_trigger_locked(struct move_filter *filter, int port, bool start);

bool is_move_filter(const char *filter_id)
{
	if (!filter_id)
		return false;
	return strcmp(filter_id, MOVE_SOURCE_FILTER_ID) == 0 || strcmp(filter_id, MOVE_SOURCE_SWAP_FILTER_ID) == 0 ||
	       strcmp(filter_id, MOVE_VALUE_FILTER_ID) == 0 || strcmp(filter_id, MOVE_AUDIO_VALUE_FILTER_ID) == 0 ||
	       strcmp(filter_id, MOVE_ACTION_FILTER_ID) == 0 || strcmp(filter_id, MOVE_AUDIO_ACTION_FILTER_ID) == 0 ||
	       strcmp(filter_id, MOVE_DIRECTSHOW_FILTER_ID) == 0;
}

void move_filter_init(struct move_filter *move_filter, obs_source_t *source, void (*move_start)(void *data))
{
	move_filter->source = source;
	move_filter->move_start_hotkey = OBS_INVALID_HOTKEY_ID;
	move_filter->move_hold_hotkey = OBS_INVALID_HOTKEY_ID;
	move_filter->move_start = move_start;
}

void move_filter_destroy(struct move_filter *move_filter)
{
	pthread_mutex_lock(&udp_servers_mutex);
	udp_update_trigger_locked(move_filter, 0, true);
	udp_update_trigger_locked(move_filter, 0, false);
	pthread_mutex_unlock(&udp_servers_mutex);
	udp_servers_reap();

	bfree(move_filter->filter_name);
	bfree(move_filter->simultaneous_move_name);
	bfree(move_filter->next_move_name);
	da_free(move_filter->filters_done);

	if (move_filter->move_start_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(move_filter->move_start_hotkey);
	if (move_filter->move_hold_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(move_filter->move_hold_hotkey);
}

void move_filter_start_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct move_filter *move_filter = data;

	if (move_filter->next_move_on != NEXT_MOVE_ON_HOTKEY || !move_filter->next_move_name ||
	    !strlen(move_filter->next_move_name)) {
		move_filter_start(move_filter);
		return;
	}
	if (!move_filter->filters_done.num) {
		move_filter_start(move_filter);
		da_push_back(move_filter->filters_done, &move_filter->source);
		return;
	}

	if (move_filter->moving && obs_source_enabled(move_filter->source) && move_filter->next_move_name &&
	    strcmp(move_filter->next_move_name, NEXT_MOVE_REVERSE) != 0) {
		move_filter->moving = false;
		if (move_filter->enabled_match_moving)
			obs_source_set_enabled(move_filter->source, false);
	}

	char *next_move_name = move_filter->next_move_name;
	obs_source_t *filter = move_filter->source;
	obs_source_t *parent = obs_filter_get_parent(filter);
	long long next_move_on = move_filter->next_move_on;
	size_t i = 0;
	while (i < move_filter->filters_done.num) {
		if (!next_move_name || !strlen(next_move_name)) {
			move_filter_start(move_filter);
			move_filter->filters_done.num = 0;
			da_push_back(move_filter->filters_done, &move_filter->source);
			return;
		}
		if (next_move_on != NEXT_MOVE_ON_HOTKEY) {
			da_push_back(move_filter->filters_done, &filter);
		} else if (strcmp(next_move_name, NEXT_MOVE_REVERSE) == 0 && !obs_source_removed(filter) &&
			   is_move_filter(obs_source_get_unversioned_id(filter))) {
			move_filter_start(obs_obj_get_data(filter));
			move_filter->filters_done.num = 0;
			return;
		}
		filter = obs_source_get_filter_by_name(parent, next_move_name);
		if (!filter && move_filter->get_alternative_filter) {
			filter = move_filter->get_alternative_filter(move_filter, next_move_name);
		}

		if (filter) {
			if (!obs_source_removed(filter) && is_move_filter(obs_source_get_unversioned_id(filter))) {
				struct move_filter *filter_data = obs_obj_get_data(filter);
				if (filter_data->moving && obs_source_enabled(filter_data->source) &&
				    (filter_data->reverse || !filter_data->next_move_name ||
				     strcmp(filter_data->next_move_name, NEXT_MOVE_REVERSE) != 0)) {
					filter_data->moving = false;
					if (filter_data->enabled_match_moving)
						obs_source_set_enabled(filter_data->source, false);
				}
				parent = obs_filter_get_parent(filter);
				next_move_name = filter_data->next_move_name;
				next_move_on = filter_data->next_move_on;
			} else {
				obs_source_release(filter);
				move_filter_start(move_filter);
				move_filter->filters_done.num = 0;
				da_push_back(move_filter->filters_done, &move_filter->source);
				return;
			}
			obs_source_release(filter);
		}

		i++;
	}
	for (i = 0; i < move_filter->filters_done.num; i++) {
		if (move_filter->filters_done.array[i] == filter) {
			move_filter_start(move_filter);
			move_filter->filters_done.num = 0;
			da_push_back(move_filter->filters_done, &move_filter->source);
			return;
		}
	}
	if (!obs_source_removed(filter) && is_move_filter(obs_source_get_unversioned_id(filter))) {
		move_filter_start(obs_obj_get_data(filter));
	}
	da_push_back(move_filter->filters_done, &filter);
}

void move_filter_hold_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct move_filter *move_filter = data;
	if (pressed) {
		move_filter_start(move_filter);
		move_filter->holding = true;
		return;
	}
	move_filter->running_duration = (float)(move_filter->start_delay + move_filter->duration) / 1000.0f;

	move_filter->holding = false;
}

static bool udp_socket_retry(void)
{
#ifdef _WIN32
	int error = WSAGetLastError();
	return error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSAEMSGSIZE;
#else
	return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

/* Called with the registry locked; closing precedes publishing finished so a
 * replacement listener can bind the same port immediately. */
static void udp_server_finish_locked(struct udp_server *server)
{
	closesocket(server->socket);
	server->socket = INVALID_SOCKET;
	server->finished = true;
}

static void *udp_server_thread(void *data)
{
	os_set_thread_name("move_udp_server_thread");
	struct udp_server *server = data;
	DARRAY(struct udp_trigger) triggers;
	da_init(triggers);

	for (;;) {
		pthread_mutex_lock(&udp_servers_mutex);
		if (udp_shutdown || (!server->start_triggers.num && !server->stop_triggers.num)) {
			udp_server_finish_locked(server);
			pthread_mutex_unlock(&udp_servers_mutex);
			break;
		}
		pthread_mutex_unlock(&udp_servers_mutex);

		fd_set read_set;
		FD_ZERO(&read_set);
		FD_SET(server->socket, &read_set);
		struct timeval timeout = {0, 100000};
#ifdef _WIN32
		int ready = select(0, &read_set, NULL, NULL, &timeout);
#else
		int ready = select(server->socket + 1, &read_set, NULL, NULL, &timeout);
#endif
		if (!ready)
			continue;
		if (ready < 0) {
			if (udp_socket_retry())
				continue;
			blog(LOG_ERROR, "[Move Transition] Error waiting for UDP port %i", server->port);
			pthread_mutex_lock(&udp_servers_mutex);
			udp_server_finish_locked(server);
			pthread_mutex_unlock(&udp_servers_mutex);
			break;
		}

		char buf[512];
		int received = recvfrom(server->socket, buf, sizeof(buf), 0, NULL, NULL);
		if (received < 0) {
			if (udp_socket_retry())
				continue;
			blog(LOG_ERROR, "[Move Transition] Error receiving on UDP port %i", server->port);
			pthread_mutex_lock(&udp_servers_mutex);
			udp_server_finish_locked(server);
			pthread_mutex_unlock(&udp_servers_mutex);
			break;
		}
		if ((size_t)received == sizeof(buf))
			continue;
		buf[received] = 0;

		/* A retained source keeps the filter alive after unlocking. Callbacks and
		 * releases must run outside this mutex: they may update/destroy filters. */
		pthread_mutex_lock(&udp_servers_mutex);
		if (!udp_shutdown) {
			for (size_t i = 0; i < server->start_triggers.num; i++) {
				obs_source_t *source = obs_source_get_ref(server->start_triggers.array[i]->source);
				if (source) {
					struct udp_trigger trigger = {source, true};
					da_push_back(triggers, &trigger);
				}
			}
			for (size_t i = 0; i < server->stop_triggers.num; i++) {
				obs_source_t *source = obs_source_get_ref(server->stop_triggers.array[i]->source);
				if (source) {
					struct udp_trigger trigger = {source, false};
					da_push_back(triggers, &trigger);
				}
			}
		}
		pthread_mutex_unlock(&udp_servers_mutex);
		for (size_t i = 0; i < triggers.num; i++) {
			struct udp_trigger *trigger = &triggers.array[i];
			obs_source_t *source = trigger->source;
			if (!obs_source_removed(source)) {
				obs_data_t *settings = obs_source_get_settings(source);
				const char *packet = obs_data_get_string(settings, trigger->start ? S_START_TRIGGER_UDP_PACKET
										       : S_STOP_TRIGGER_UDP_PACKET);
				struct move_filter *filter = obs_obj_get_data(source);
				if (filter && (!packet[0] || strcmp(packet, buf) == 0)) {
					if (trigger->start)
						move_filter_start(filter);
					else
						move_filter_stop(filter);
				}
				obs_data_release(settings);
			}
			obs_source_release(source);
		}
		triggers.num = 0;
	}
	da_free(triggers);
	return NULL;
}

static void udp_server_free(struct udp_server *server)
{
	da_free(server->start_triggers);
	da_free(server->stop_triggers);
	bfree(server);
}

static void udp_servers_reap(void)
{
	DARRAY(struct udp_server *) finished;
	da_init(finished);
	pthread_mutex_lock(&udp_servers_mutex);
	for (size_t i = udp_servers.num; i > 0; i--) {
		struct udp_server *server = udp_servers.array[i - 1];
		if (server->finished) {
			da_push_back(finished, &server);
			da_erase(udp_servers, i - 1);
		}
	}
	pthread_mutex_unlock(&udp_servers_mutex);
	for (size_t i = 0; i < finished.num; i++) {
		pthread_join(finished.array[i]->thread, NULL);
		udp_server_free(finished.array[i]);
	}
	da_free(finished);
}

/* The caller holds the registry lock until it has added the first trigger. */
static struct udp_server *udp_server_create_locked(int port)
{
	if (udp_shutdown || port < 1 || port > 65535)
		return NULL;
	SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET) {
		blog(LOG_ERROR, "[Move Transition] Error creating UDP socket for port %i", port);
		return NULL;
	}
#ifndef _WIN32
	if (sock >= FD_SETSIZE)
		goto fail;
#endif
	struct sockaddr_in address = {0};
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)port);
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(sock, (struct sockaddr *)&address, sizeof(address)) != 0)
		goto fail;
#ifdef _WIN32
	u_long nonblocking = 1;
	if (ioctlsocket(sock, FIONBIO, &nonblocking) != 0)
		goto fail;
#else
	int flags = fcntl(sock, F_GETFL, 0);
	if (flags == -1 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1)
		goto fail;
#endif
	struct udp_server *server = bzalloc(sizeof(*server));
	server->port = port;
	server->socket = sock;
	if (pthread_create(&server->thread, NULL, udp_server_thread, server) != 0) {
		bfree(server);
		goto fail;
	}
	da_push_back(udp_servers, &server);
	return server;

fail:
	blog(LOG_ERROR, "[Move Transition] Could not start UDP listener on port %i", port);
	closesocket(sock);
	return NULL;
}

static void udp_update_trigger_locked(struct move_filter *filter, int port, bool start)
{
	struct udp_server *target = NULL;
	for (size_t i = 0; i < udp_servers.num; i++) {
		struct udp_server *server = udp_servers.array[i];
		if (!udp_shutdown && !server->finished && server->port == port) {
			target = server;
			continue;
		}
		if (start)
			da_erase_item(server->start_triggers, &filter);
		else
			da_erase_item(server->stop_triggers, &filter);
	}
	if (!port || udp_shutdown)
		return;
	if (!target)
		target = udp_server_create_locked(port);
	if (!target)
		return;
	if (start) {
		if (da_find(target->start_triggers, &filter, 0) == DARRAY_INVALID)
			da_push_back(target->start_triggers, &filter);
	} else {
		if (da_find(target->stop_triggers, &filter, 0) == DARRAY_INVALID)
			da_push_back(target->stop_triggers, &filter);
	}
}

bool move_filter_udp_init(void)
{
	da_init(udp_servers);
	udp_shutdown = false;
	if (pthread_mutex_init(&udp_servers_mutex, NULL) != 0)
		return false;
#ifdef _WIN32
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
		pthread_mutex_destroy(&udp_servers_mutex);
		return false;
	}
#endif
	return true;
}

void move_filter_udp_shutdown(void)
{
	DARRAY(struct udp_server *) servers;
	da_init(servers);
	pthread_mutex_lock(&udp_servers_mutex);
	udp_shutdown = true;
	da_move(servers, udp_servers);
	pthread_mutex_unlock(&udp_servers_mutex);
	/* Workers observe shutdown within the select timeout, even if a wakeup
	 * datagram could not be sent. Never join while holding their mutex. */
	for (size_t i = 0; i < servers.num; i++) {
		pthread_join(servers.array[i]->thread, NULL);
		udp_server_free(servers.array[i]);
	}
	da_free(servers);
}

void move_filter_udp_free(void)
{
	move_filter_udp_shutdown();
	obs_wait_for_destroy_queue();
	pthread_mutex_destroy(&udp_servers_mutex);
#ifdef _WIN32
	WSACleanup();
#endif
}

void move_filter_update(struct move_filter *move_filter, obs_data_t *settings)
{
	const char *filter_name = obs_source_get_name(move_filter->source);
	if (!move_filter->filter_name || strcmp(move_filter->filter_name, filter_name) != 0) {
		bfree(move_filter->filter_name);
		move_filter->filter_name = bstrdup(filter_name);
		if (move_filter->move_start_hotkey != OBS_INVALID_HOTKEY_ID) {
			obs_hotkey_unregister(move_filter->move_start_hotkey);
			move_filter->move_start_hotkey = OBS_INVALID_HOTKEY_ID;
		}
		if (move_filter->move_hold_hotkey != OBS_INVALID_HOTKEY_ID) {
			obs_hotkey_unregister(move_filter->move_hold_hotkey);
			move_filter->move_hold_hotkey = OBS_INVALID_HOTKEY_ID;
		}
	}
	obs_source_t *parent = obs_filter_get_parent(move_filter->source);
	if (parent && move_filter->filter_name) {
		if (move_filter->move_start_hotkey == OBS_INVALID_HOTKEY_ID) {
			move_filter->move_start_hotkey = obs_hotkey_register_source(
				parent, move_filter->filter_name, move_filter->filter_name, move_filter_start_hotkey, move_filter);
		}
		if (move_filter->move_hold_hotkey == OBS_INVALID_HOTKEY_ID) {
			struct dstr hotkey_name = {0};
			dstr_init_copy(&hotkey_name, move_filter->filter_name);
			dstr_cat(&hotkey_name, " ");
			dstr_cat(&hotkey_name, obs_module_text("Hold"));
			move_filter->move_hold_hotkey = obs_hotkey_register_source(parent, hotkey_name.array, hotkey_name.array,
										   move_filter_hold_hotkey, move_filter);
			dstr_free(&hotkey_name);
		}
	}
	move_filter->enabled_match_moving = obs_data_get_bool(settings, S_ENABLED_MATCH_MOVING);
	if (move_filter->enabled_match_moving && !move_filter->moving && obs_source_enabled(move_filter->source))
		move_filter_start(move_filter);

	move_filter->custom_duration = obs_data_get_bool(settings, S_CUSTOM_DURATION);
	if (move_filter->custom_duration)
		move_filter->duration = obs_data_get_int(settings, S_DURATION);
	move_filter->start_delay = obs_data_get_int(settings, S_START_DELAY);
	move_filter->end_delay = obs_data_get_int(settings, S_END_DELAY);

	move_filter->easing = obs_data_get_int(settings, S_EASING_MATCH);
	move_filter->easing_function = obs_data_get_int(settings, S_EASING_FUNCTION_MATCH);

	udp_servers_reap();
	pthread_mutex_lock(&udp_servers_mutex);
	move_filter->start_trigger = (uint32_t)obs_data_get_int(settings, S_START_TRIGGER);
	move_filter->stop_trigger = (uint32_t)obs_data_get_int(settings, S_STOP_TRIGGER);
	int start_port = 0, stop_port = 0;
	if (move_filter->start_trigger == START_TRIGGER_UDP) {
		start_port = (int)obs_data_get_int(settings, S_START_TRIGGER_UDP_PORT);
		if (!start_port)
			start_port = 3000;
	}
	if (move_filter->stop_trigger == START_TRIGGER_UDP) {
		stop_port = (int)obs_data_get_int(settings, S_STOP_TRIGGER_UDP_PORT);
		if (!stop_port)
			stop_port = 3000;
	}
	udp_update_trigger_locked(move_filter, start_port, true);
	udp_update_trigger_locked(move_filter, stop_port, false);
	pthread_mutex_unlock(&udp_servers_mutex);

	const char *simultaneous_move_name = obs_data_get_string(settings, S_SIMULTANEOUS_MOVE);
	if (!move_filter->simultaneous_move_name || strcmp(move_filter->simultaneous_move_name, simultaneous_move_name) != 0) {
		bfree(move_filter->simultaneous_move_name);
		move_filter->simultaneous_move_name = bstrdup(simultaneous_move_name);
	}

	const char *next_move_name = obs_data_get_string(settings, S_NEXT_MOVE);
	if (!move_filter->next_move_name || strcmp(move_filter->next_move_name, next_move_name) != 0) {
		bfree(move_filter->next_move_name);
		move_filter->next_move_name = bstrdup(next_move_name);
		move_filter->reverse = false;
	}
	move_filter->next_move_on = obs_data_get_int(settings, S_NEXT_MOVE_ON);
}

void move_filter_start(struct move_filter *move_filter)
{
	move_filter->move_start(move_filter);
}

bool move_filter_start_internal(struct move_filter *move_filter)
{
	if (!move_filter->custom_duration)
		move_filter->duration = obs_frontend_get_transition_duration();
	if (move_filter->moving && !move_filter->holding && obs_source_enabled(move_filter->source)) {
		if (move_filter->next_move_on == NEXT_MOVE_ON_HOTKEY && move_filter->next_move_name &&
		    strcmp(move_filter->next_move_name, NEXT_MOVE_REVERSE) == 0) {
			move_filter->reverse = !move_filter->reverse;
			move_filter->running_duration =
				(float)(move_filter->duration + move_filter->start_delay + move_filter->end_delay) / 1000.0f -
				move_filter->running_duration;
		}
		return false;
	}
	move_filter->running_duration = 0.0f;
	move_filter->moving = true;

	if (move_filter->enabled_match_moving && !obs_source_enabled(move_filter->source)) {
		move_filter->enabled = true;
		obs_source_set_enabled(move_filter->source, true);
	}

	if (move_filter->simultaneous_move_name && strlen(move_filter->simultaneous_move_name) &&
	    (!move_filter->filter_name || strcmp(move_filter->filter_name, move_filter->simultaneous_move_name) != 0)) {
		obs_source_t *parent = obs_filter_get_parent(move_filter->source);
		if (parent) {
			obs_source_t *filter = obs_source_get_filter_by_name(parent, move_filter->simultaneous_move_name);
			if (!filter && move_filter->get_alternative_filter) {
				filter = move_filter->get_alternative_filter(move_filter, move_filter->simultaneous_move_name);
			}

			if (filter) {
				if (!obs_source_removed(filter) && is_move_filter(obs_source_get_unversioned_id(filter))) {
					move_filter_start(obs_obj_get_data(filter));
				}
				obs_source_release(filter);
			}
		}
	}
	return true;
}

void move_filter_stop(struct move_filter *move_filter)
{
	if (move_filter->holding)
		return;
	move_filter->moving = false;
	if (move_filter->enabled_match_moving && obs_source_enabled(move_filter->source)) {
		obs_source_set_enabled(move_filter->source, false);
	}
}

extern void move_filter_ended(struct move_filter *move_filter)
{
	if (move_filter->enabled_match_moving &&
	    (move_filter->reverse || move_filter->next_move_on == NEXT_MOVE_ON_HOTKEY || !move_filter->next_move_name ||
	     strcmp(move_filter->next_move_name, NEXT_MOVE_REVERSE) != 0) &&
	    obs_source_enabled(move_filter->source)) {
		obs_source_set_enabled(move_filter->source, false);
	}

	if (move_filter->next_move_on == NEXT_MOVE_ON_END && move_filter->next_move_name && strlen(move_filter->next_move_name) &&
	    (!move_filter->filter_name || strcmp(move_filter->filter_name, move_filter->next_move_name) != 0)) {
		if (strcmp(move_filter->next_move_name, NEXT_MOVE_REVERSE) == 0) {
			move_filter->reverse = !move_filter->reverse;
			if (move_filter->reverse)
				move_filter_start(move_filter);
		} else {
			obs_source_t *parent = obs_filter_get_parent(move_filter->source);
			if (parent) {
				obs_source_t *filter = obs_source_get_filter_by_name(parent, move_filter->next_move_name);

				if (!filter && move_filter->get_alternative_filter) {
					filter = move_filter->get_alternative_filter(move_filter, move_filter->next_move_name);
				}

				if (filter) {
					if (!obs_source_removed(filter) && is_move_filter(obs_source_get_unversioned_id(filter))) {
						move_filter_start(obs_obj_get_data(filter));
					}

					obs_source_release(filter);
				}
			}
		}
	} else if (move_filter->next_move_on == NEXT_MOVE_ON_HOTKEY && move_filter->next_move_name &&
		   strcmp(move_filter->next_move_name, NEXT_MOVE_REVERSE) == 0) {
		move_filter->reverse = !move_filter->reverse;
	}
}

float get_eased(float f, long long easing, long long easing_function);

bool move_filter_tick(struct move_filter *move_filter, float seconds, float *tp)
{
	if (move_filter->filter_name &&
	    (move_filter->move_start_hotkey == OBS_INVALID_HOTKEY_ID || move_filter->move_hold_hotkey == OBS_INVALID_HOTKEY_ID)) {
		obs_source_t *parent = obs_filter_get_parent(move_filter->source);
		if (parent && move_filter->move_start_hotkey == OBS_INVALID_HOTKEY_ID) {
			move_filter->move_start_hotkey = obs_hotkey_register_source(
				parent, move_filter->filter_name, move_filter->filter_name, move_filter_start_hotkey, move_filter);
		}
		if (parent && move_filter->move_hold_hotkey == OBS_INVALID_HOTKEY_ID) {
			struct dstr hotkey_name = {0};
			dstr_init_copy(&hotkey_name, move_filter->filter_name);
			dstr_cat(&hotkey_name, " ");
			dstr_cat(&hotkey_name, obs_module_text("Hold"));
			move_filter->move_hold_hotkey = obs_hotkey_register_source(parent, hotkey_name.array, hotkey_name.array,
										   move_filter_hold_hotkey, move_filter);
			dstr_free(&hotkey_name);
		}
	}

	const bool enabled = obs_source_enabled(move_filter->source);
	if (move_filter->enabled != enabled) {
		if (enabled && (move_filter->start_trigger == START_TRIGGER_ENABLE ||
				(move_filter->enabled_match_moving && !move_filter->moving)))
			move_filter_start(move_filter);
		if (enabled && move_filter->stop_trigger == START_TRIGGER_ENABLE)
			move_filter_stop(move_filter);

		move_filter->enabled = enabled;
	}
	if (move_filter->enabled_match_moving && enabled != move_filter->moving) {
		if (enabled) {
			move_filter_start(move_filter);
		} else {
			move_filter_stop(move_filter);
		}
	}
	if (!move_filter->moving || !enabled)
		return false;

	move_filter->running_duration += seconds;
	if (move_filter->running_duration * 1000.0f < (move_filter->reverse ? move_filter->end_delay : move_filter->start_delay)) {
		return false;
	}
	if (move_filter->holding) {
		move_filter_start(move_filter);
		move_filter->running_duration = (float)(move_filter->start_delay + move_filter->duration) / 1000.0f;

		*tp = 1.0f;
		return true;
	}

	if (move_filter->running_duration * 1000.0f >=
	    (float)(move_filter->start_delay + move_filter->duration + move_filter->end_delay)) {
		move_filter->moving = false;
	}
	if (!move_filter->duration) {
		*tp = 1.0f;
		return true;
	}
	float t = (move_filter->running_duration * 1000.0f -
		   (float)(move_filter->reverse ? move_filter->end_delay : move_filter->start_delay)) /
		  (float)move_filter->duration;
	if (t >= 1.0f) {
		t = 1.0f;
	}
	if (move_filter->reverse) {
		t = 1.0f - t;
	}
	t = get_eased(t, move_filter->easing, move_filter->easing_function);
	*tp = t;
	return true;
}

bool move_filter_start_button(obs_properties_t *props, obs_property_t *property, void *data)
{
	struct move_filter *move_filter = data;
	move_filter_start(move_filter);
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	return false;
}

void prop_list_add_move_filter(obs_source_t *parent, obs_source_t *child, void *data)
{
	UNUSED_PARAMETER(parent);
	if (!is_move_filter(obs_source_get_unversioned_id(child)))
		return;
	obs_property_t *p = (obs_property_t *)data;
	const char *name = obs_source_get_name(child);
	obs_property_list_add_string(p, name, name);
}

bool move_filter_start_trigger_changed(void *priv, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(property);
	obs_property_t *port = obs_properties_get(props, S_START_TRIGGER_UDP_PORT);
	obs_property_t *packet = obs_properties_get(props, S_START_TRIGGER_UDP_PACKET);
	bool udp = obs_data_get_int(settings, S_START_TRIGGER) == START_TRIGGER_UDP;
	if (obs_property_visible(port) == udp && obs_property_visible(packet) == udp)
		return false;
	obs_property_set_visible(port, udp);
	obs_property_set_visible(packet, udp);
	return true;
}

bool move_filter_stop_trigger_changed(void *priv, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(property);
	obs_property_t *port = obs_properties_get(props, S_STOP_TRIGGER_UDP_PORT);
	obs_property_t *packet = obs_properties_get(props, S_STOP_TRIGGER_UDP_PACKET);
	bool udp = obs_data_get_int(settings, S_STOP_TRIGGER) == START_TRIGGER_UDP;
	if (obs_property_visible(port) == udp && obs_property_visible(packet) == udp)
		return false;
	obs_property_set_visible(port, udp);
	obs_property_set_visible(packet, udp);
	return true;
}

void move_filter_properties(struct move_filter *move_filter, obs_properties_t *ppts)
{
	obs_property_t *p = obs_properties_add_int(ppts, S_START_DELAY, obs_module_text("StartDelay"), 0, 10000000, 100);
	obs_property_int_set_suffix(p, "ms");

	obs_properties_t *duration = obs_properties_create();

	p = obs_properties_add_int(duration, S_DURATION, "", 0, 10000000, 100);
	obs_property_int_set_suffix(p, "ms");

	p = obs_properties_add_group(ppts, S_CUSTOM_DURATION, obs_module_text("CustomDuration"), OBS_GROUP_CHECKABLE, duration);

	p = obs_properties_add_int(ppts, S_END_DELAY, obs_module_text("EndDelay"), 0, 10000000, 100);
	obs_property_int_set_suffix(p, "ms");

	p = obs_properties_add_list(ppts, S_EASING_MATCH, obs_module_text("Easing"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	prop_list_add_easings(p);

	p = obs_properties_add_list(ppts, S_EASING_FUNCTION_MATCH, obs_module_text("EasingFunction"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	prop_list_add_easing_functions(p);

	p = obs_properties_add_bool(ppts, S_ENABLED_MATCH_MOVING, obs_module_text("EnabledMatchMoving"));

	p = obs_properties_add_list(ppts, S_START_TRIGGER, obs_module_text("StartTrigger"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(p, obs_module_text("StartTrigger.None"), START_TRIGGER_NONE);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.Activate"), START_TRIGGER_ACTIVATE);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.Deactivate"), START_TRIGGER_DEACTIVATE);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.Show"), START_TRIGGER_SHOW);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.Hide"), START_TRIGGER_HIDE);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.Enable"), START_TRIGGER_ENABLE);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.Load"), START_TRIGGER_LOAD);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.MoveMatch"), START_TRIGGER_MOVE_MATCH);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.MoveIn"), START_TRIGGER_MOVE_IN);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.MoveOut"), START_TRIGGER_MOVE_OUT);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.Udp"), START_TRIGGER_UDP);

	obs_property_set_modified_callback2(p, move_filter_start_trigger_changed, move_filter);

	obs_properties_add_int(ppts, S_START_TRIGGER_UDP_PORT, obs_module_text("UdpPort"), 1, 65535, 1);
	obs_properties_add_text(ppts, S_START_TRIGGER_UDP_PACKET, obs_module_text("UdpPacket"), OBS_TEXT_DEFAULT);

	p = obs_properties_add_list(ppts, S_STOP_TRIGGER, obs_module_text("StopTrigger"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(p, obs_module_text("StopTrigger.None"), START_TRIGGER_NONE);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.Activate"), START_TRIGGER_ACTIVATE);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.Deactivate"), START_TRIGGER_DEACTIVATE);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.Show"), START_TRIGGER_SHOW);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.Hide"), START_TRIGGER_HIDE);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.Enable"), START_TRIGGER_ENABLE);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.MoveMatch"), START_TRIGGER_MOVE_MATCH);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.MoveIn"), START_TRIGGER_MOVE_IN);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.MoveOut"), START_TRIGGER_MOVE_OUT);
	obs_property_list_add_int(p, obs_module_text("StartTrigger.Udp"), START_TRIGGER_UDP);

	obs_property_set_modified_callback2(p, move_filter_stop_trigger_changed, move_filter);

	obs_properties_add_int(ppts, S_STOP_TRIGGER_UDP_PORT, obs_module_text("UdpPort"), 1, 65535, 1);
	obs_properties_add_text(ppts, S_STOP_TRIGGER_UDP_PACKET, obs_module_text("UdpPacket"), OBS_TEXT_DEFAULT);

	obs_source_t *parent = obs_filter_get_parent(move_filter->source);

	p = obs_properties_add_list(ppts, S_SIMULTANEOUS_MOVE, obs_module_text("SimultaneousMove"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("SimultaneousMove.None"), "");
	if (parent)
		obs_source_enum_filters(parent, prop_list_add_move_filter, p);

	p = obs_properties_add_list(ppts, S_NEXT_MOVE, obs_module_text("NextMove"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("NextMove.None"), "");
	obs_property_list_add_string(p, obs_module_text("NextMove.Reverse"), NEXT_MOVE_REVERSE);
	if (parent)
		obs_source_enum_filters(parent, prop_list_add_move_filter, p);

	p = obs_properties_add_list(ppts, S_NEXT_MOVE_ON, obs_module_text("NextMoveOn"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("NextMoveOn.End"), NEXT_MOVE_ON_END);
	obs_property_list_add_int(p, obs_module_text("NextMoveOn.Hotkey"), NEXT_MOVE_ON_HOTKEY);

	obs_properties_add_button2(ppts, "move_filter_start", obs_module_text("Start"), move_filter_start_button, move_filter);
	obs_properties_add_text(ppts, "plugin_info", PLUGIN_INFO, OBS_TEXT_INFO);
}

void move_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_ENABLED_MATCH_MOVING, true);
	obs_data_set_default_int(settings, S_DURATION, 300);
	obs_data_set_default_int(settings, S_START_TRIGGER_UDP_PORT, 3000);
	obs_data_set_default_int(settings, S_STOP_TRIGGER_UDP_PORT, 3000);
}

void move_filter_activate(void *data)
{
	struct move_filter *move_filter = data;
	if (move_filter->start_trigger == START_TRIGGER_ACTIVATE)
		move_filter_start(move_filter);
	if (move_filter->stop_trigger == START_TRIGGER_ACTIVATE)
		move_filter_stop(move_filter);
}

void move_filter_deactivate(void *data)
{
	struct move_filter *move_filter = data;
	if (move_filter->start_trigger == START_TRIGGER_DEACTIVATE)
		move_filter_start(move_filter);
	if (move_filter->stop_trigger == START_TRIGGER_DEACTIVATE)
		move_filter_stop(move_filter);
}

void move_filter_show(void *data)
{
	struct move_filter *move_filter = data;
	if (move_filter->start_trigger == START_TRIGGER_SHOW)
		move_filter_start(move_filter);
	if (move_filter->stop_trigger == START_TRIGGER_SHOW)
		move_filter_stop(move_filter);
}

void move_filter_hide(void *data)
{
	struct move_filter *move_filter = data;
	if (move_filter->start_trigger == START_TRIGGER_HIDE)
		move_filter_start(move_filter);
	if (move_filter->stop_trigger == START_TRIGGER_HIDE)
		move_filter_stop(move_filter);
}
