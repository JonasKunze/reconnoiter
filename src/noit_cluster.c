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

typedef struct {
  mtev_cluster_node_t node;
  const char* outbuff;
  uint send_size;
  uint write_sofar;
} send_job_t;

typedef struct {
  request_hdr_t next_hdr;
  uint8_t read_so_far;
} request_ctx_t;

static request_ctx_t*
request_ctx_alloc() {
  return calloc(1, sizeof(request_ctx_t));
}

static void
request_ctx_free(void* data) {
  request_ctx_t *ctx = data;
  free(ctx);
}

static request_hdr_t
request_hdr_new() {
  request_hdr_t hdr;
  hdr.version = 0;
  return hdr;
}

static void
ntoh_request_hdr(request_hdr_t *hdr) {

}

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
static mtev_boolean
read_next_hdr(eventer_t e, request_ctx_t *ctx) {
  int len;

  len = e->opset->read(e->fd, ((char*)&ctx->next_hdr) + ctx->read_so_far, sizeof(request_hdr_t) - ctx->read_so_far, &newmask, e);
  if(len == 0 || (len < 0 && errno != EAGAIN)) {
    eventer_remove_fd(e->fd);
    e->opset->close(e->fd, &newmask, e);
    return 0;
  }

  if(len > 0) {
    ctx->read_so_far += len;

    if(ctx->read_so_far == sizeof(request_hdr_t)) {
      ctx->read_so_far = 0;
      ntoh_request_hdr(&ctx->next_hdr);
      return mtev_true;
    }
  }

  return mtev_false;
}

static int
handle_check_request(eventer_t e, int mask, void *closure,
    struct timeval *now) {
  acceptor_closure_t *ac = closure;
  request_ctx_t *ctx = ac->service_ctx;

  if(mask & EVENTER_EXCEPTION) {
    /* Exceptions cause us to simply snip the connection */

    /* This removes the log feed which is important to do before calling close */
    eventer_remove_fd(e->fd);
    e->opset->close(e->fd, &newmask, e);
    return 0;
  }

  if(!ac->service_ctx) {
    ctx = ac->service_ctx = request_ctx_alloc();
    ac->service_ctx_free = request_ctx_free;
  }

  if(read_next_hdr(e, ctx) == mtev_true) {

  }

  mtevL(noit_notice, "Bytes read: %d\n", ctx->read_so_far);

  return newmask | EVENTER_EXCEPTION;
}

static int
mtev_cluster_send_connection_complete(eventer_t e, int mask, void *closure,
    struct timeval *now) {
  int rv;
  int write_mask = EVENTER_EXCEPTION;
  send_job_t *job = closure;

  while((rv = e->opset->write(e->fd,
      job->outbuff + job->write_sofar, job->send_size - job->write_sofar, &write_mask, e)) > 0) {
    job->write_sofar += rv;
    if(job->write_sofar == job->send_size) break;
  }
  if(rv < 0) {
    return -1;
  }
  eventer_remove_fd(e->fd);
  e->opset->close(e->fd, &newmask, e);
  eventer_free(e);
  return 0;
}

static int
mtev_cluster_send(const mtev_cluster_node_t *node, const char *data, uint data_len) {

  int fd, rv;
  eventer_t e;
  union {
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
  } addr;
  addr.addr6 = node->addr.addr6;
  addr.addr4.sin_port = htons(node->data_port);
  fd = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
  rv = connect(fd, (struct sockaddr*)&addr, node->address_len);
  if(rv == -1) return -1;

  send_job_t *job = malloc(sizeof(send_job_t));
  job->node = *node;
  job->outbuff = data;
  job->send_size = data_len;

  /* Register a handler for connection completion */
  e = eventer_alloc();
  e->fd = fd;
  e->mask = EVENTER_READ | EVENTER_WRITE | EVENTER_EXCEPTION;
  e->callback = mtev_cluster_send_connection_complete;
  e->closure = job;
  eventer_add(e);

  return 1;
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
        mtev_cluster_enable_payload(cluster, (void*) &my_revision,
            sizeof(my_revision)));

    check_history = jl_array_list_new(free_history_entry);
  } else {
    mtevL(noit_notice, "Didn't find any cluster in the config files\n");
  }

  eventer_name_callback("noit_cluster_network", handle_check_request);


  mtev_cluster_node_t *nodes[10];
  mtev_cluster_get_nodes(cluster, nodes, 10, mtev_false);

  mtev_cluster_send(nodes[0], "asdf", 4);

}
