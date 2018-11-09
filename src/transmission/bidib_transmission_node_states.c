/*
 *
 * Copyright (C) 2017 University of Bamberg, Software Technologies Research Group
 * <https://www.uni-bamberg.de/>, <http://www.swt-bamberg.de/>
 *
 * This file is part of the BiDiB library (libbidib), used to communicate with
 * BiDiB <www.bidib.org> systems over a serial connection. This library was
 * developed as part of Nicolas Gross’ student project.
 *
 * libbidib is licensed under the GNU GENERAL PUBLIC LICENSE (Version 3), see
 * the LICENSE file at the project's top-level directory for details or consult
 * <http://www.gnu.org/licenses/>.
 *
 * libbidib is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or any later version.
 *
 * libbidib is a RESEARCH PROTOTYPE and distributed WITHOUT ANY WARRANTY, without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU General Public License for more details.
 *
 * The following people contributed to the conception and realization of the
 * present libbidib (in alphabetic order by surname):
 *
 * - Nicolas Gross <https://github.com/nicolasgross>
 *
 */

#include <glib.h>
#include <stdlib.h>
#include <syslog.h>
#include <pthread.h>
#include <memory.h>
#include <time.h>

#include "bidib_transmission_intern.h"
#include "../../include/bidib.h"


#define RESPONSE_QUEUE_EXPIRATION_SECS 2

pthread_mutex_t bidib_node_state_table_mutex;

static GHashTable *node_state_table = NULL;


void bidib_node_state_table_init() {
	node_state_table = g_hash_table_new(g_str_hash, g_str_equal);
}

static void bidib_node_state_add_response(unsigned char type, t_bidib_node_state *state,
                                          int message_max_resp, unsigned int action_id) {
	if (message_max_resp > 0) {
		state->current_max_respond += message_max_resp;
		t_bidib_response_queue_entry *response = malloc(sizeof(t_bidib_response_queue_entry));
		response->type = type;
		response->creation_time = time(NULL);
		response->action_id = action_id;
		g_queue_push_tail(state->response_queue, response);
	}
}

static void bidib_node_state_add_message(unsigned char *addr_stack, unsigned char type,
                                         unsigned char *message, t_bidib_node_state *state,
                                         unsigned int action_id) {
	t_bidib_message_queue_entry *message_entry = malloc(sizeof(t_bidib_message_queue_entry));
	message_entry->type = type;
	memcpy(message_entry->addr, addr_stack, 4);
	message_entry->message = malloc(sizeof(unsigned char) * (message[0] + 1));
	memcpy(message_entry->message, message, message[0] + 1);
	message_entry->action_id = action_id;
	g_queue_push_tail(state->message_queue, message_entry);
	syslog(LOG_DEBUG, "Enqueued type: 0x%02x to: 0x%02x 0x%02x 0x%02x 0x%02x action id: %d",
	       type, addr_stack[0], addr_stack[1], addr_stack[2], addr_stack[3], action_id);
}

static t_bidib_node_state *bidib_node_query(unsigned char *addr_stack) {
	t_bidib_node_state *state = g_hash_table_lookup(node_state_table, addr_stack);
	// Check whether node is already registered in the table
	if (state == NULL) {
		state = malloc(sizeof(t_bidib_node_state));
		memcpy(state->addr, addr_stack, 4);
		state->receive_seqnum = 0x01;
		state->send_seqnum = 0x01;
		state->stall = false;
		state->current_max_respond = 0;
		state->stall_affected_nodes_queue = g_queue_new();
		state->response_queue = g_queue_new();
		state->message_queue = g_queue_new();
		g_hash_table_insert(node_state_table, state->addr, state);
		syslog(LOG_DEBUG, "%s 0x%02x 0x%02x 0x%02x 0x%02x", "Add to node state table: ",
		       addr_stack[0], addr_stack[1], addr_stack[2], addr_stack[3]);
	}
	return state;
}

static int bidib_node_stall_queue_entry_equals(t_bidib_stall_queue_entry *elem,
                                               t_bidib_node_state *searched) {
	if (elem->addr[0] == searched->addr[0] && elem->addr[1] == searched->addr[1] &&
	    elem->addr[2] == searched->addr[2] && elem->addr[3] == searched->addr[3]) {
		return 0;
	}
	return 1;
}

static bool bidib_node_stall_ready(unsigned char *addr_stack) {
	unsigned char addr_cpy[4];
	memcpy(addr_cpy, addr_stack, 4);
	while (addr_cpy[0] != 0x00) {
		t_bidib_node_state *state = g_hash_table_lookup(node_state_table, addr_cpy);
		if (state != NULL && state->stall &&
		    !g_queue_find(state->stall_affected_nodes_queue,
		                  bidib_node_stall_queue_entry_equals)) {
			t_bidib_stall_queue_entry *stall_entry = malloc(
					sizeof(t_bidib_stall_queue_entry));
			memcpy(stall_entry->addr, addr_stack, 4);
			g_queue_push_tail(state->stall_affected_nodes_queue, stall_entry);
			return false;
		}
		for (int i = 2; i >= 0; i--) {
			if (addr_cpy[i] != 0x00) {
				addr_cpy[i] = 0x00;
				break;
			}
		}
	}
	return true;
}

bool bidib_node_try_send(unsigned char *addr_stack, unsigned char type,
                         unsigned char *message, unsigned int action_id) {
	pthread_mutex_lock(&bidib_node_state_table_mutex);
	t_bidib_node_state *state = bidib_node_query(addr_stack);
	int max_response = bidib_response_info[type][1];
	bool status;
	if (bidib_node_stall_ready(addr_stack) && g_queue_is_empty(state->message_queue) &&
	    state->current_max_respond + max_response <= 48) {
		// Node is ready
		bidib_node_state_add_response(type, state, max_response, action_id);
		status = true;
	} else {
		// Node is not ready
		bidib_node_state_add_message(addr_stack, type, message, state, action_id);
		status = false;
	}
	syslog(LOG_DEBUG, "Used output buffer for 0x%02x 0x%02x 0x%02x 0x%02x is %d byte",
	       addr_stack[0], addr_stack[1], addr_stack[2], addr_stack[3],
	       state->current_max_respond);
	pthread_mutex_unlock(&bidib_node_state_table_mutex);
	return status;
}

static void bidib_node_try_queued_messages(t_bidib_node_state *state) {
	while (bidib_node_stall_ready((unsigned char *) state->addr) &&
	       !g_queue_is_empty(state->message_queue)) {
		t_bidib_message_queue_entry *queued_msg = g_queue_peek_head(state->message_queue);
		if (state->current_max_respond + bidib_response_info[queued_msg->type][1] <= 48) {
			// capacity sufficient -> send messages
			bidib_node_state_add_response(queued_msg->type, state,
			                              bidib_response_info[queued_msg->type][1],
			                              queued_msg->action_id);
			bidib_add_to_buffer(queued_msg->message);
			syslog(LOG_DEBUG, "Dequeued type: 0x%02x to: 0x%02x 0x%02x 0x%02x 0x%02x "
					       "action id: %d",
			       queued_msg->type, state->addr[0], state->addr[1], state->addr[2],
			       state->addr[3], queued_msg->action_id);
			g_queue_pop_head(state->message_queue);
			free(queued_msg->message);
			free(queued_msg);
		} else {
			break;
		}
	}
	bidib_flush();
}

unsigned int bidib_node_state_update(unsigned char *addr_stack, unsigned char response_type) {
	unsigned int action_id = 0;
	pthread_mutex_lock(&bidib_node_state_table_mutex);
	t_bidib_node_state *state = g_hash_table_lookup(node_state_table, addr_stack);

	if (state != NULL && !g_queue_is_empty(state->response_queue)) {
		// node in table and awaiting answers
		t_bidib_response_queue_entry *response = g_queue_peek_head(state->response_queue);
		time_t current_time = time(NULL);
		for (int i = 2; i <= bidib_response_info[response->type][0]; i++) {
			if (bidib_response_info[response->type][i] == response_type) {
				// awaited answer matches message -> extend free capacity
				g_queue_pop_head(state->response_queue);
				state->current_max_respond -= bidib_response_info[response->type][1];
				action_id = response->action_id;
				free(response);
				bidib_node_try_queued_messages(state);
				break;
			} else if (difftime(current_time, response->creation_time) >=
			           RESPONSE_QUEUE_EXPIRATION_SECS) {
				// remove response queue entries older than x seconds
				syslog(LOG_ERR,
				       "Response from: 0x%02x 0x%02x 0x%02x 0x%02x to type: 0x%02x "
						       "with action id: %d expected but not received",
				       addr_stack[0], addr_stack[1], addr_stack[2], addr_stack[3],
				       response->type, response->action_id);
				g_queue_pop_head(state->response_queue);
				state->current_max_respond -= bidib_response_info[response->type][1];
				free(response);
				if (!g_queue_is_empty(state->response_queue)) {
					response = g_queue_peek_head(state->response_queue);
				} else {
					break;
				}
			}
		}
		syslog(LOG_DEBUG, "Used output buffer for 0x%02x 0x%02x 0x%02x 0x%02x is %d byte",
		       addr_stack[0], addr_stack[1], addr_stack[2], addr_stack[3],
		       state->current_max_respond);
	}
	pthread_mutex_unlock(&bidib_node_state_table_mutex);
	return action_id;
}

void bidib_node_update_stall(unsigned char *addr_stack, unsigned char stall_status) {
	pthread_mutex_lock(&bidib_node_state_table_mutex);
	t_bidib_node_state *state = bidib_node_query(addr_stack);
	if (stall_status == 0x00) {
		state->stall = false;
		syslog(LOG_WARNING, "Stall inactive for: 0x%02x 0x%02x 0x%02x 0x%02x",
		       addr_stack[0], addr_stack[1], addr_stack[2], addr_stack[3]);
		t_bidib_stall_queue_entry *elem;
		while (!g_queue_is_empty(state->stall_affected_nodes_queue)) {
			elem = g_queue_pop_head(state->stall_affected_nodes_queue);
			t_bidib_node_state *waiting_node_state = g_hash_table_lookup(
					node_state_table, elem->addr);
			if (waiting_node_state != NULL) {
				bidib_node_try_queued_messages(waiting_node_state);
			}
			free(elem);
		}
	} else {
		state->stall = true;
		syslog(LOG_WARNING, "Stall active for: 0x%02x 0x%02x 0x%02x 0x%02x",
		       addr_stack[0], addr_stack[1], addr_stack[2], addr_stack[3]);
	}
	pthread_mutex_unlock(&bidib_node_state_table_mutex);
}

static unsigned char bidib_get_and_incr_seqnum(unsigned char *seqnum) {
	if (*seqnum == 255) {
		*seqnum = 0x01;
		return 255;
	}
	return (*seqnum)++;
}

unsigned char bidib_node_state_get_and_incr_receive_seqnum(unsigned char *addr_stack) {
	pthread_mutex_lock(&bidib_node_state_table_mutex);
	t_bidib_node_state *state = bidib_node_query(addr_stack);
	unsigned char seqnum = bidib_get_and_incr_seqnum(&state->receive_seqnum);
	pthread_mutex_unlock(&bidib_node_state_table_mutex);
	return seqnum;
}

unsigned char bidib_node_state_get_and_incr_send_seqnum(unsigned char *addr_stack) {
	pthread_mutex_lock(&bidib_node_state_table_mutex);
	t_bidib_node_state *state = bidib_node_query(addr_stack);
	unsigned char seqnum = bidib_get_and_incr_seqnum(&state->send_seqnum);
	pthread_mutex_unlock(&bidib_node_state_table_mutex);
	return seqnum;
}

void bidib_node_state_set_receive_seqnum(unsigned char *addr_stack, unsigned char seqnum) {
	pthread_mutex_lock(&bidib_node_state_table_mutex);
	t_bidib_node_state *state = bidib_node_query(addr_stack);
	state->receive_seqnum = seqnum;
	pthread_mutex_unlock(&bidib_node_state_table_mutex);
}

void bidib_node_state_table_reset(void) {
	pthread_mutex_lock(&bidib_node_state_table_mutex);
	GHashTableIter iter;
	unsigned char *key;
	t_bidib_node_state *value;
	g_hash_table_iter_init(&iter, node_state_table);
	while (g_hash_table_iter_next(&iter, (gpointer) &key, (gpointer) &value)) {
		t_bidib_stall_queue_entry *elem0;
		if (value != NULL) {
			while (!g_queue_is_empty(value->stall_affected_nodes_queue)) {
				elem0 = g_queue_pop_head(value->stall_affected_nodes_queue);
				free(elem0);
			}
			g_queue_free(value->stall_affected_nodes_queue);
			t_bidib_response_queue_entry *elem1;
			while (!g_queue_is_empty(value->response_queue)) {
				elem1 = g_queue_pop_head(value->response_queue);
				free(elem1);
			}
			g_queue_free(value->response_queue);
			t_bidib_message_queue_entry *elem2;
			while (!g_queue_is_empty(value->message_queue)) {
				elem2 = g_queue_pop_head(value->message_queue);
				free(elem2->message);
				free(elem2);
			}
			g_queue_free(value->message_queue);
			free(value);
			g_hash_table_iter_remove(&iter);
		}
	}
	pthread_mutex_unlock(&bidib_node_state_table_mutex);
	syslog(LOG_INFO, "%s", "Node state table reset");
}

void bidib_node_state_table_free(void) {
	if (node_state_table != NULL) {
		bidib_node_state_table_reset();
		g_hash_table_destroy(node_state_table);
	}
	syslog(LOG_INFO, "%s", "Node state table freed");
}
