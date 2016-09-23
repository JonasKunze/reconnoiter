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
#include <mtev_cluster_messaging.h>
#include <mtev_atomic.h>
#include <mtev_listener.h>
#include <mtev_str.h>

#include <uuid/uuid.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>

#include "noit_mtev_bridge.h"
#include "noit_check.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#define CLUSTER_NAME "noit"

static mtev_atomic64_t largest_seq = 0;
static mtev_boolean initializing = mtev_true;

typedef struct {
  uuid_t uuid;
  char *name;
  mtev_boolean check_deleted;
} history_entry_t;

typedef struct {
  uint8_t version;
  int64_t last_seen_revision;
} request_hdr_t;

static void
on_check_updated(noit_check_t *check) {
  // replace my_revision with check->conf_seq if this is larger than my_revision
  while(largest_seq < check->config_seq && mtev_atomic_cas64(&largest_seq, check->config_seq, largest_seq) != largest_seq) {
  }
}

static mtev_hook_return_t
on_check_updated_listener(void *closure, noit_check_t *check) {
  assert(initializing == mtev_false && "noit_cluster_init must be called after all checks are loaded!");

  if(check->config_seq <= largest_seq) {
    char uuid_str[UUID_PRINTABLE_STRING_LENGTH];
    uuid_unparse(check->checkid, uuid_str);
    mtevL(noit_error, "Saw a check with a sequence smaller or equal then the largest one ever seen: %s\n",
              uuid_str);
    return MTEV_HOOK_CONTINUE;
  }

  on_check_updated(check);

  return MTEV_HOOK_CONTINUE;
}

static int
noit_poller_cb(noit_check_t * check, void *closure) {
  on_check_updated(check);
  return 1;
}

static mtev_hook_return_t
handle_request(void *closure, eventer_t e, const char* data, uint data_length) {
  assert(data_length == sizeof(request_hdr_t));
  request_hdr_t *request = (request_hdr_t*) data;
  int msg_len;

  mtevL(noit_notice, "Recieved request: %"PRId64"\n", request->last_seen_revision);

  xmlDocPtr doc = noit_generate_checks_xml_doc(request->last_seen_revision + 1);

  xmlChar *msg;
  xmlDocDumpMemory(doc, &msg, &msg_len);
  if(doc)
    xmlFreeDoc(doc);

  if(mtev_cluster_messaging_send_response(e, (char*)msg, msg_len, free) == 0) {
    mtevL(noit_error, "Unable to send cluster response: %s\n", msg);
  } else {
    mtevL(noit_notice, "Sent response: %s\n", msg);
  }

  return MTEV_HOOK_CONTINUE;
}

static mtev_hook_return_t
handle_response(void* closure, eventer_t e, const void *data, uint data_len) {
  xmlDocPtr doc;

  mtevL(noit_notice, "Received response : %s\n", (char*)data);

  doc = xmlReadMemory(data, data_len, "checks.xml", NULL, 0);
  if (doc == NULL) {
      mtevL(noit_error, "Failed to parse cluster message response: %s\n", (char*)data);
  } else {

  }

  return MTEV_HOOK_CONTINUE;
}

static mtev_hook_return_t
on_node_updated(void *closure, mtev_cluster_node_changes_t node_change,
    mtev_cluster_node_t *node, mtev_cluster_t *cluster,
    struct timeval old_boot_time) {
  //int64_t *other_revision = node->payload;
  if(mtev_cluster_is_that_me(node) == mtev_true) {
    return MTEV_HOOK_CONTINUE;
  }

  mtevL(noit_notice, "Other node changed: %d!!!!!!!!!\n", node_change);

  if(node_change & (MTEV_CLUSTER_NODE_REBOOTED /*| MTEV_CLUSTER_NODE_CHANGED_PAYLOAD*/)) {
    eventer_t connection = mtev_cluster_messaging_connect(node);

    if(connection) {
      request_hdr_t *request = calloc(1, sizeof(request_hdr_t));
      request->version = 123;
      request->last_seen_revision = largest_seq;

      int *closure = malloc(sizeof(int));
      *closure = 1234;
      mtev_cluster_messaging_send_request(connection, (char*)request, sizeof(*request), free,
          handle_response, closure);
      mtevL(noit_notice, "Sent request: %" PRId64 "\n", request->last_seen_revision);
    } else {
      mtevL(noit_notice, "Unable to connect to %d\n", node->data_port);
    }
  }
  return MTEV_HOOK_CONTINUE;
}

void
noit_cluster_init() {
  mtev_cluster_t *cluster;
  mtev_cluster_init();
  if(mtev_cluster_enabled() == mtev_true) {
    mtevL(noit_notice, "Initializing noit cluster\n");
    check_updated_hook_register("cluster-check-update-listener",
        on_check_updated_listener, NULL);
    check_deleted_hook_register("cluster-check-delete-listener",
        on_check_updated_listener, NULL);
    mtev_cluster_handle_node_update_hook_register("cluster-topology-listener",
        on_node_updated, NULL);

    cluster = mtev_cluster_by_name(CLUSTER_NAME);
    if(cluster == NULL) {
      mtevL(noit_error, "Unable to find cluster %s in the config files\n",
          CLUSTER_NAME);
      exit(1);
    }
    assert(
        mtev_cluster_set_heartbeat_payload(cluster, 1, 1, (void*) &largest_seq,
            sizeof(largest_seq)));
  } else {
    mtevL(noit_notice, "Didn't find any cluster in the config files\n");
  }

  mtev_cluster_messaging_init(CLUSTER_NAME);
  mtev_cluster_messaging_request_hook_register("cluster-messaging-listener",
      handle_request, NULL);

  noit_poller_do(noit_poller_cb, NULL);
  initializing = mtev_false;
}
