/*
 * Copyright (c) 2016, Circonus, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name Circonus, Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this
 *       software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <mtev_cluster.h>
#include <mtev_atomic.h>
#include <mtev_arraylist.h>
#include <mtev_listener.h>

#include <uuid/uuid.h>
#include <errno.h>
#include <stdint.h>

#include "noit_mtev_bridge.h"
#include "noit_check.h"

#define CLUSTER_NAME "noit"

static mtev_atomic64_t my_revision = 0;

static struct jl_array_list *check_history;
static int newmask = EVENTER_READ | EVENTER_EXCEPTION;

typedef struct {
  uuid_t uuid;
  char *name;
  mtev_boolean check_deleted;
} history_entry_t;

typedef struct {
  uint8_t version;
  uuid_t requester_id;
  struct timeval last_seen_boot_time;
  int64_t last_seen_revision;
} request_hdr_t;

static void
free_history_entry(void* data) {
  history_entry_t *entry;
  entry = data;
  if(entry->name)
    free(entry->name);
  free(entry);
}

static void
add_history_entry(noit_check_t *check, mtev_boolean check_deleted) {
  history_entry_t *entry = malloc(sizeof(history_entry_t));
  jl_array_list_add(check_history, entry);

  memcpy(&entry->uuid, &check->checkid, sizeof(check->checkid));
  entry->name = strdup(check->name);
  entry->check_deleted = check_deleted;
}

static mtev_hook_return_t
on_check_updated(void *closure, noit_check_t *check) {
  add_history_entry(check, mtev_false);
  mtev_atomic_inc64(&my_revision);
  return MTEV_HOOK_CONTINUE;
}

static mtev_hook_return_t
on_check_deleted(void *closure, noit_check_t *check) {
  add_history_entry(check, mtev_true);
  mtev_atomic_inc64(&my_revision);
  return MTEV_HOOK_CONTINUE;
}

static mtev_hook_return_t
on_node_updated(void *closure,
    mtev_cluster_node_t *updated_node, mtev_cluster_t *cluster,
    struct timeval old_boot_time) {
  //int64_t *other_revision = updated_node->payload;

  return MTEV_HOOK_CONTINUE;
}

static int
handle_check_request(void *closure, eventer_t e, const char* data, uint data_length) {
  mtevL(noit_notice, "Received cluster message: %s\n", data);
  int mask;
  char *msg = "hello world!";

  return mtev_cluster_messaging_send_response(e, msg, strlen(msg), NULL);
}

static int
handle_response(eventer_t e, const char *data, uint data_len, void *closure) {
  mtev_cluster_messaging_send_request(e, "asd", 3, NULL, handle_response);
  mtevL(noit_notice, "Received response : %s\n", data);
  return 0;
}

void
noit_cluster_init() {
  mtev_cluster_t *cluster;
  mtev_cluster_init();
  if(mtev_cluster_enabled() == mtev_true) {
    mtevL(noit_notice, "Initializing noit cluster\n");
    check_updated_hook_register("cluster-check-update-listener",
        on_check_updated, NULL);
    check_deleted_hook_register("cluster-check-delete-listener",
        on_check_deleted, NULL);
    mtev_cluster_handle_node_update_hook_register("cluster-topology-listener",
        on_node_updated, NULL);

    cluster = mtev_cluster_by_name(CLUSTER_NAME);
    if(cluster == NULL) {
      mtevL(noit_error, "Unable to find cluster %s in the config files\n",
          CLUSTER_NAME);
      exit(1);
    }
    assert(
        mtev_cluster_set_heartbeat_payload(cluster, 1, 1, (void*) &my_revision,
            sizeof(my_revision)));

    check_history = jl_array_list_new(free_history_entry);
  } else {
    mtevL(noit_notice, "Didn't find any cluster in the config files\n");
  }

  mtev_cluster_messaging_init(CLUSTER_NAME);
  mtev_cluster_messaging_received_hook_register("cluster-messaging-listener",
      handle_check_request, NULL);

  mtev_cluster_node_t *nodes[10];
  int cluster_size = mtev_cluster_get_nodes(cluster, nodes, 10, mtev_false);

  eventer_t connection = mtev_cluster_messaging_connect(nodes[0]);
  if(connection)
    mtev_cluster_messaging_send_request(connection, "asd", 3, NULL, handle_response);
  else
    mtevL(noit_notice, "Unable to connect to %d\n", nodes[0]->data_port);
}
