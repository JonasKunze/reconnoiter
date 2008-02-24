/*
 * Copyright (c) 2007, OmniTI Computer Consulting, Inc.
 * All rights reserved.
 */

#ifndef _NOIT_CHECK_H
#define _NOIT_CHECK_H

#include "noit_defines.h"

#include <uuid/uuid.h>
#include <netinet/in.h>

#include "eventer/eventer.h"
#include "utils/noit_hash.h"
#include "noit_conf.h"
#include "noit_console.h"

/*
 * Checks:
 *  attrs:
 *   UUID
 *   host (target)
 *   check (module)
 *   name (identifying the check to the user if
 *         multiple checks of the same module are specified)
 *   config (params for the module)
 *   period (ms)
 *   timeout (ms)
 *  transient:
 *   eventer_t (fire)
 *   stats_t [inprogress, last]
 *   closure
 */

#define NP_RUNNING  0x00000001
#define NP_KILLED   0x00000002
#define NP_DISABLED 0x00000004
#define NP_UNCONFIG 0x00000008

#define NP_UNKNOWN '0'             /* stats_t.{available,state} */
#define NP_AVAILABLE 'A'           /* stats_t.available */
#define NP_UNAVAILABLE 'U'         /* stats_t.available */
#define NP_BAD 'B'                 /* stats_t.state */
#define NP_GOOD 'G'                /* stats_t.state */

typedef enum {
  METRIC_GUESS = '0',
  METRIC_INT32 = 'i',
  METRIC_UINT32 = 'I',
  METRIC_INT64 = 'l',
  METRIC_UINT64 = 'L',
  METRIC_DOUBLE = 'n',
  METRIC_STRING = 's'
} metric_type_t;

typedef struct {
  char *metric_name;
  metric_type_t metric_type;
  union {
    double *n;
    int32_t *i;
    u_int32_t *I;
    int64_t *l;
    u_int64_t *L;
    char *s;
    void *vp; /* used for clever assignments */
  } metric_value;
} metric_t;

typedef struct {
  struct timeval whence;
  int8_t available;
  int8_t state;
  u_int32_t duration;
  char *status;
  noit_hash_table metrics;
} stats_t;

typedef struct dep_list {
  struct noit_check *check;
  struct dep_list *next;
} dep_list_t;

typedef struct noit_check {
  uuid_t checkid;
  int8_t target_family;
  union {
    struct in_addr addr;
    struct in6_addr addr6;
  } target_addr;
  char *target;
  char *module;
  char *name;
  noit_hash_table *config;
  char *oncheck;               /* target`name of the check that fires us */
  u_int32_t period;            /* period of checks in milliseconds */
  u_int32_t timeout;           /* timeout of check in milliseconds */
  u_int32_t flags;             /* NP_KILLED, NP_RUNNING */

  dep_list_t *causal_checks;
  eventer_t fire_event;
  struct timeval last_fire_time;
  struct {
    stats_t current;
    stats_t previous;
  } stats;
  u_int32_t generation;        /* This can roll, we don't care */
  void *closure;
} noit_check_t;

#define NOIT_CHECK_LIVE(a) ((a)->fire_event != NULL)
#define NOIT_CHECK_DISABLED(a) ((a)->flags & NP_DISABLED)
#define NOIT_CHECK_CONFIGURED(a) (((a)->flags & NP_UNCONFIG) == 0)
#define NOIT_CHECK_RUNNING(a) ((a)->flags & NP_RUNNING)
#define NOIT_CHECK_KILLED(a) ((a)->flags & NP_KILLED)

API_EXPORT(void) noit_poller_init();
API_EXPORT(void) noit_poller_reload(const char *xpath); /* NULL for all */
API_EXPORT(void) noit_poller_process_checks(const char *xpath);
API_EXPORT(void) noit_poller_make_causal_map();

API_EXPORT(void)
  noit_check_fake_last_check(noit_check_t *check,
                             struct timeval *lc, struct timeval *_now);
API_EXPORT(int) noit_check_max_initial_stutter();

API_EXPORT(int)
  noit_poller_schedule(const char *target,
                       const char *module,
                       const char *name,
                       noit_hash_table *config,
                       u_int32_t period,
                       u_int32_t timeout,
                       const char *oncheck,
                       int flags,
                       uuid_t in,
                       uuid_t out);

API_EXPORT(int)
  noit_check_update(noit_check_t *new_check,
                    const char *target,
                    const char *name,
                    noit_hash_table *config,
                    u_int32_t period,
                    u_int32_t timeout,
                    const char *oncheck,
                    int flags);

API_EXPORT(int)
  noit_poller_deschedule(uuid_t in);

API_EXPORT(noit_check_t *)
  noit_poller_lookup(uuid_t in);

API_EXPORT(noit_check_t *)
  noit_poller_lookup_by_name(char *target, char *name);

API_EXPORT(void)
  noit_check_stats_clear(stats_t *s);

struct _noit_module;
API_EXPORT(void)
  noit_check_set_stats(struct _noit_module *self, noit_check_t *check,
                        stats_t *newstate);

API_EXPORT(void)
  noit_stats_set_metric(stats_t *, char *, metric_type_t, void *);

/* These are from noit_check_log.c */
API_EXPORT(void) noit_check_log_check(noit_check_t *check);
API_EXPORT(void) noit_check_log_status(noit_check_t *check);
API_EXPORT(void) noit_check_log_metrics(noit_check_t *check);

#endif
