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

#include <uuid/uuid.h>

#include "noit_mtev_bridge.h"
#include "noit_check.h"

#define CLUSTER_NAME "ponies"

static mtev_atomic64_t my_revision = 0;

static mtev_hook_return_t
on_check_updated(noit_check_t *check) {
  mtev_atomic_inc64(&my_revision);
  return MTEV_HOOK_CONTINUE;
}

static mtev_hook_return_t
on_node_updated(void *closure, mtev_cluster_node_t *updated_node, mtev_cluster_t *cluster,
    struct timeval old_boot_time) {
  int64_t *other_revision = updated_node->payload;

  return MTEV_HOOK_CONTINUE;
}

void
noit_cluster_init() {
  mtev_cluster_t *cluster;
  mtev_cluster_init();
  if (mtev_cluster_enabled() == mtev_true) {
    mtevL(noit_notice, "Initializing noit cluster\n");
    check_updated_hook_register("cluster-check-listener", on_check_updated, NULL);
    mtev_cluster_handle_node_update_hook_register("cluster-topology-listener", on_node_updated, NULL);

    cluster = mtev_cluster_by_name(CLUSTER_NAME);
    if (cluster == NULL) {
      mtevL(noit_error, "Unable to find cluster %s in the config files\n", CLUSTER_NAME);
      exit(1);
    }
    mtev_cluster_enable_payload(cluster, (void*)&my_revision, sizeof(my_revision));
  } else {
    mtevL(noit_notice, "Didn't find any cluster in the config files\n");
  }
}
