/*
 * SSLsplit - transparent SSL/TLS interception
 * Copyright (c) 2009-2016, Daniel Roethlisberger <daniel@roe.ch>
 * All rights reserved.
 * http://www.roe.ch/SSLsplit
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "pxythrmgr.h"

#include "sys.h"
#include "log.h"
#include "pxyconn.h"

#include <string.h>
#include <event2/bufferevent.h>
#include <pthread.h>
#include <assert.h>
#include <sys/param.h>

/*
 * Proxy thread manager: manages the connection handling worker threads
 * and the per-thread resources (i.e. event bases).  The load is shared
 * across num_cpu * 2 connection handling threads, using the number of
 * currently assigned connections as the sole metric.
 *
 * The attach and detach functions are thread-safe.
 */

static void
pxy_thrmgr_get_thr_expired_conns(pxy_thr_ctx_t *tctx, pxy_conn_ctx_t **expired_conns)
{
	*expired_conns = NULL;

	if (tctx->conns) {
		time_t now = time(NULL);

		pxy_conn_ctx_t *ctx = tctx->conns;
		while (ctx) {
			unsigned long elapsed_time = now - ctx->atime;
			if (elapsed_time > tctx->thrmgr->opts->conn_idle_timeout) {
				ctx->next_expired = *expired_conns;
				*expired_conns = ctx;
			}
			ctx = ctx->next;
		}

		if (tctx->thrmgr->opts->statslog) {
			char *src_addr = NULL;
			char *dst_addr = NULL;

			ctx = *expired_conns;
			while (ctx) {
				src_addr = NULL;
				if (ctx->srchost_str && ctx->srcport_str) {
					if (asprintf(&src_addr, ", src_addr=%s:%s", ctx->srchost_str, ctx->srcport_str) < 0) {
						goto leave;
					}
				}

				dst_addr = NULL;
				if (ctx->dsthost_str && ctx->dstport_str) {
					if (asprintf(&dst_addr, ", dst_addr=%s:%s", ctx->dsthost_str, ctx->dstport_str) < 0) {
						goto leave;
					}
				}

#ifdef DEBUG_PROXY
				log_dbg_level_printf(LOG_DBG_MODE_FINEST, "pxy_thrmgr_get_expired_conns: thr=%d, fd=%d, child_fd=%d, time=%lld%s%s\n",
						ctx->thr->thridx, ctx->fd, ctx->child_fd, (long int) now - ctx->atime, STRORNONE(src_addr), STRORNONE(dst_addr));
#endif /* DEBUG_PROXY */

				char *msg;
				if (asprintf(&msg, "EXPIRED: thr=%d, time=%lld%s%s\n", 
						ctx->thr->thridx, (long int) now - ctx->atime, STRORNONE(src_addr), STRORNONE(dst_addr)) < 0) {
					goto leave;
				}

				if (src_addr) {
					free(src_addr);
					src_addr = NULL;
				}
				if (dst_addr) {
					free(dst_addr);
					dst_addr = NULL;
				}

				if (log_conn(msg) == -1) {
					log_err_level_printf(LOG_WARNING, "Expired conn logging failed\n");
				}
				free(msg);

				ctx = ctx->next_expired;
			}
leave:
			if (src_addr) {
				free(src_addr);
			}
			if (dst_addr) {
				free(dst_addr);
			}
		}
	}
}

static evutil_socket_t
pxy_thrmgr_print_child(pxy_conn_child_ctx_t *child_ctx, unsigned int parent_idx, evutil_socket_t max_fd)
{
	assert(child_ctx != NULL);

	// @attention No need to log child stats
#ifdef DEBUG_PROXY
	char *msg;
	if (asprintf(&msg, "CHILD CONN: thr=%d, id=%d, pid=%u, src=%d, dst=%d, c=%d-%d\n", 
			child_ctx->parent->thr->thridx, child_ctx->idx, parent_idx, child_ctx->src_fd, child_ctx->dst_fd, child_ctx->src.closed, child_ctx->dst.closed) < 0) {
		return max_fd;
	}
	log_dbg_level_printf(LOG_DBG_MODE_FINEST, "pxy_thrmgr_print_child: %s", msg);
	free(msg);
#endif /* DEBUG_PROXY */

	if (child_ctx->next) {
		max_fd = pxy_thrmgr_print_child(child_ctx->next, parent_idx, max_fd);
	}

	return MAX(max_fd, MAX(child_ctx->src_fd, child_ctx->dst_fd));
}

static void
pxy_thrmgr_print_thr_info(pxy_thr_ctx_t *tctx)
{
#ifdef DEBUG_PROXY
	log_dbg_level_printf(LOG_DBG_MODE_FINEST, "pxy_thrmgr_print_thr_info: thr=%d, load=%lu\n", tctx->thridx, tctx->load);
#endif /* DEBUG_PROXY */

	unsigned int idx = 1;
	evutil_socket_t max_fd = 0;
	time_t max_atime = 0;
	time_t max_ctime = 0;

#ifdef DEBUG_PROXY
	char *lmsg = NULL;
#endif /* DEBUG_PROXY */
	char *smsg = NULL;

	char *src_addr = NULL;
	char *dst_addr = NULL;

	if (tctx->conns) {
		time_t now = time(NULL);

		pxy_conn_ctx_t *ctx = tctx->conns;
		while (ctx) {
			time_t atime = now - ctx->atime;
			time_t ctime = now - ctx->ctime;
			
			src_addr = NULL;
			if (ctx->srchost_str && ctx->srcport_str) {
				if (asprintf(&src_addr, ", src_addr=%s:%s", ctx->srchost_str, ctx->srcport_str) < 0) {
					goto leave;
				}
			}
			dst_addr = NULL;
			if (ctx->dsthost_str && ctx->dstport_str) {
				if (asprintf(&dst_addr, ", dst_addr=%s:%s", ctx->dsthost_str, ctx->dstport_str) < 0) {
					goto leave;
				}
			}

#ifdef DEBUG_PROXY
			if (asprintf(&lmsg, "PARENT CONN: thr=%d, id=%u, fd=%d, child_fd=%d, dst=%d, srv_dst=%d, child_src=%d, child_dst=%d, p=%d-%d-%d c=%d-%d, ce=%d cc=%d, at=%lld ct=%lld%s%s\n",
					tctx->thridx, idx, ctx->fd, ctx->child_fd, ctx->dst_fd, ctx->srv_dst_fd, ctx->child_src_fd, ctx->child_dst_fd,
					ctx->src.closed, ctx->dst.closed, ctx->srv_dst.closed, ctx->children ? ctx->children->src.closed : 0, ctx->children ? ctx->children->dst.closed : 0,
					ctx->children ? 1:0, ctx->child_count, atime, ctime, STRORNONE(src_addr), STRORNONE(dst_addr)) < 0) {
				goto leave;
			}
			log_dbg_level_printf(LOG_DBG_MODE_FINEST, "pxy_thrmgr_print_thr_info: %s", lmsg);
			free(lmsg);
			lmsg = NULL;
#endif /* DEBUG_PROXY */

			// @attention Report idle connections only, i.e. the conns which have been idle since the last time we checked for expired conns
			if (atime >= tctx->thrmgr->opts->expired_conn_check_period) {
				if (asprintf(&smsg, "IDLE: thr=%d, id=%u, ce=%d cc=%d, at=%lld ct=%lld%s%s\n",
						tctx->thridx, idx, ctx->children ? 1:0, ctx->child_count, atime, ctime, STRORNONE(src_addr), STRORNONE(dst_addr)) < 0) {
					goto leave;
				}

				if (log_conn(smsg) == -1) {
					log_err_level_printf(LOG_WARNING, "Idle conn logging failed\n");
				}
				free(smsg);
				smsg = NULL;
			}

			if (src_addr) {
				free(src_addr);
				src_addr = NULL;
			}
			if (dst_addr) {
				free(dst_addr);
				dst_addr = NULL;
			}

			max_fd = MAX(max_fd, MAX(ctx->fd, MAX(ctx->child_fd, MAX(ctx->dst_fd, MAX(ctx->srv_dst_fd, MAX(ctx->child_src_fd, ctx->child_dst_fd))))));
			max_atime = MAX(max_atime, atime);
			max_ctime = MAX(max_ctime, ctime);

			if (ctx->children) {
				max_fd = pxy_thrmgr_print_child(ctx->children, idx, max_fd);
			}

			idx++;
			ctx = ctx->next;
		}
	}

	if (asprintf(&smsg, "STATS: thr=%d, mld=%lu, mfd=%d, mat=%lld, mct=%lld, iib=%llu, iob=%llu, eib=%llu, eob=%llu, swm=%lu, uwm=%lu, to=%lu, err=%lu, si=%u\n",
			tctx->thridx, tctx->max_load, tctx->max_fd, max_atime, max_ctime, tctx->intif_in_bytes, tctx->intif_out_bytes, tctx->extif_in_bytes, tctx->extif_out_bytes,
			tctx->set_watermarks, tctx->unset_watermarks, tctx->timedout_conns, tctx->errors, tctx->stats_idx) < 0) {
		goto leave;
	}

#ifdef DEBUG_PROXY
	log_dbg_level_printf(LOG_DBG_MODE_FINEST, "pxy_thrmgr_print_thr_info: %s", smsg);
#endif /* DEBUG_PROXY */

	if (log_stats(smsg) == -1) {
		log_err_level_printf(LOG_WARNING, "Stats logging failed\n");
	}
	free(smsg);
	smsg = NULL;

	tctx->stats_idx++;

	tctx->timedout_conns = 0;
	tctx->errors = 0;
	tctx->set_watermarks = 0;
	tctx->unset_watermarks = 0;

	tctx->intif_in_bytes = 0;
	tctx->intif_out_bytes = 0;
	tctx->extif_in_bytes = 0;
	tctx->extif_out_bytes = 0;

	// Reset these stats with the current values (do not reset to 0 directly, there may be active conns)
	tctx->max_fd = max_fd;
	tctx->max_load = tctx->load;

leave:
	if (src_addr) {
		free(src_addr);
	}
	if (dst_addr) {
		free(dst_addr);
	}
#ifdef DEBUG_PROXY
	if (lmsg) {
		free(lmsg);
	}
#endif /* DEBUG_PROXY */
	if (smsg) {
		free(smsg);
	}
}

/*
 * Recurring timer event to prevent the event loops from exiting when
 * they run out of events.
 */
static void
pxy_thrmgr_timer_cb(UNUSED evutil_socket_t fd, UNUSED short what,
                    UNUSED void *arg)
{
	pxy_thr_ctx_t *ctx = arg;

#ifdef DEBUG_PROXY
	log_dbg_level_printf(LOG_DBG_MODE_FINEST, "pxy_thrmgr_timer_cb: thr=%d, load=%lu, to=%u\n", ctx->thridx, ctx->load, ctx->timeout_count);
#endif /* DEBUG_PROXY */
	pxy_conn_ctx_t *expired = NULL;
	pxy_thrmgr_get_thr_expired_conns(ctx, &expired);

	if (expired) {
#ifdef DEBUG_PROXY
		time_t now = time(NULL);
#endif /* DEBUG_PROXY */
		while (expired) {
			pxy_conn_ctx_t *next = expired->next_expired;

#ifdef DEBUG_PROXY
			log_dbg_level_printf(LOG_DBG_MODE_FINE, "pxy_thrmgr_timer_cb: Delete timed out conn thr=%d, fd=%d, child_fd=%d, at=%lld ct=%lld\n",
					expired->thr->thridx, expired->fd, expired->child_fd, (long int) now - expired->atime, (long int) now - expired->ctime);
#endif /* DEBUG_PROXY */
			pxy_conn_free(expired, 1);
			ctx->timedout_conns++;

			expired = next;
		}
	}
	
	// @attention Print thread info only if stats logging is enabled, if disabled debug logs are not printed either
	if (ctx->thrmgr->opts->statslog) {
		ctx->timeout_count++;
		if (ctx->timeout_count * ctx->thrmgr->opts->expired_conn_check_period >= ctx->thrmgr->opts->stats_period * ctx->thrmgr->opts->expired_conn_check_period) {
			ctx->timeout_count = 0;
			pxy_thrmgr_print_thr_info(ctx);
		}
	}
}

/*
 * Thread entry point; runs the event loop of the event base.
 * Does not exit until the libevent loop is broken explicitly.
 */
static void *
pxy_thrmgr_thr(void *arg)
{
	pxy_thr_ctx_t *ctx = arg;
	struct timeval timer_delay = {ctx->thrmgr->opts->expired_conn_check_period, 0};
	struct event *ev;

	ev = event_new(ctx->evbase, -1, EV_PERSIST, pxy_thrmgr_timer_cb, ctx);
	if (!ev)
		return NULL;
	evtimer_add(ev, &timer_delay);
	ctx->running = 1;
	event_base_dispatch(ctx->evbase);
	event_free(ev);

	return NULL;
}

/*
 * Create new thread manager but do not start any threads yet.
 * This gets called before forking to background.
 */
pxy_thrmgr_ctx_t *
pxy_thrmgr_new(opts_t *opts)
{
	pxy_thrmgr_ctx_t *ctx;

	if (!(ctx = malloc(sizeof(pxy_thrmgr_ctx_t))))
		return NULL;
	memset(ctx, 0, sizeof(pxy_thrmgr_ctx_t));

	ctx->opts = opts;
	ctx->num_thr = 2 * sys_get_cpu_cores();
	return ctx;
}

/*
 * Start the thread manager and associated threads.
 * This must be called after forking.
 *
 * Returns -1 on failure, 0 on success.
 */
int
pxy_thrmgr_run(pxy_thrmgr_ctx_t *ctx)
{
	int idx = -1, dns = 0;

	dns = opts_has_dns_spec(ctx->opts);

	if (pthread_mutex_init(&ctx->mutex, NULL)) {
		log_dbg_printf("Failed to initialize mutex\n");
		goto leave;
	}

	if (!(ctx->thr = malloc(ctx->num_thr * sizeof(pxy_thr_ctx_t*)))) {
		log_dbg_printf("Failed to allocate memory\n");
		goto leave;
	}
	memset(ctx->thr, 0, ctx->num_thr * sizeof(pxy_thr_ctx_t*));

	for (idx = 0; idx < ctx->num_thr; idx++) {
		if (!(ctx->thr[idx] = malloc(sizeof(pxy_thr_ctx_t)))) {
			log_dbg_printf("Failed to allocate memory\n");
			goto leave;
		}
		memset(ctx->thr[idx], 0, sizeof(pxy_thr_ctx_t));
		ctx->thr[idx]->evbase = event_base_new();
		if (!ctx->thr[idx]->evbase) {
			log_dbg_printf("Failed to create evbase %d\n", idx);
			goto leave;
		}
		if (dns) {
			/* only create dns base if we actually need it later */
			ctx->thr[idx]->dnsbase = evdns_base_new(
			                         ctx->thr[idx]->evbase, 1);
			if (!ctx->thr[idx]->dnsbase) {
				log_dbg_printf("Failed to create dnsbase %d\n",
				               idx);
				goto leave;
			}
		}
		ctx->thr[idx]->load = 0;
		ctx->thr[idx]->running = 0;
		ctx->thr[idx]->conns = NULL;
		ctx->thr[idx]->thridx = idx;
		ctx->thr[idx]->timeout_count = 0;
		ctx->thr[idx]->thrmgr = ctx;
	}

	log_dbg_printf("Initialized %d connection handling threads\n",
	               ctx->num_thr);

	for (idx = 0; idx < ctx->num_thr; idx++) {
		if (pthread_create(&ctx->thr[idx]->thr, NULL,
		                   pxy_thrmgr_thr, ctx->thr[idx]))
			goto leave_thr;
		while (!ctx->thr[idx]->running) {
			sched_yield();
		}
	}

	log_dbg_printf("Started %d connection handling threads\n",
	               ctx->num_thr);

	return 0;

leave_thr:
	idx--;
	while (idx >= 0) {
		pthread_cancel(ctx->thr[idx]->thr);
		pthread_join(ctx->thr[idx]->thr, NULL);
		idx--;
	}
	idx = ctx->num_thr - 1;

leave:
	while (idx >= 0) {
		if (ctx->thr[idx]) {
			if (ctx->thr[idx]->dnsbase) {
				evdns_base_free(ctx->thr[idx]->dnsbase, 0);
			}
			if (ctx->thr[idx]->evbase) {
				event_base_free(ctx->thr[idx]->evbase);
			}
			free(ctx->thr[idx]);
		}
		idx--;
	}
	pthread_mutex_destroy(&ctx->mutex);
	if (ctx->thr) {
		free(ctx->thr);
		ctx->thr = NULL;
	}
	return -1;
}

/*
 * Destroy the event manager and stop all threads.
 */
void
pxy_thrmgr_free(pxy_thrmgr_ctx_t *ctx)
{
	pthread_mutex_destroy(&ctx->mutex);
	if (ctx->thr) {
		for (int idx = 0; idx < ctx->num_thr; idx++) {
			event_base_loopbreak(ctx->thr[idx]->evbase);
			sched_yield();
		}
		for (int idx = 0; idx < ctx->num_thr; idx++) {
			pthread_join(ctx->thr[idx]->thr, NULL);
		}
		for (int idx = 0; idx < ctx->num_thr; idx++) {
			if (ctx->thr[idx]->dnsbase) {
				evdns_base_free(ctx->thr[idx]->dnsbase, 0);
			}
			if (ctx->thr[idx]->evbase) {
				event_base_free(ctx->thr[idx]->evbase);
			}
			free(ctx->thr[idx]);
		}
		free(ctx->thr);
	}
	free(ctx);
}

static void 
pxy_thrmgr_remove_conn(pxy_conn_ctx_t *node, pxy_conn_ctx_t **head)
{
	assert(node != NULL);
	assert(*head != NULL);

#ifdef DEBUG_PROXY
	log_dbg_level_printf(LOG_DBG_MODE_FINEST, "pxy_thrmgr_remove_conn: Removing conn, id=%llu, fd=%d, child_fd=%d\n", node->id, node->fd, node->child_fd);
#endif /* DEBUG_PROXY */
	
	// @attention We may get multiple conns with the same fd combinations, so fds cannot uniquely define a conn; hence the need for unique ids.
    if (node->id == (*head)->id) {
        *head = (*head)->next;
        return;
    }

    pxy_conn_ctx_t *current = (*head)->next;
    pxy_conn_ctx_t *previous = *head;
    while (current != NULL && previous != NULL) {
        if (node->id == current->id) {
            previous->next = current->next;
            return;
        }
        previous = current;
        current = current->next;
    }
}

/*
 * Attach a new connection to a thread.  Chooses the thread with the fewest
 * currently active connections, returns the appropriate event bases.
 * Returns the index of the chosen thread (for passing to _detach later).
 * This function cannot fail.
 */
void
pxy_thrmgr_attach(pxy_conn_ctx_t *ctx)
{
#ifdef DEBUG_PROXY
	log_dbg_level_printf(LOG_DBG_MODE_FINEST, "pxy_thrmgr_attach: ENTER\n");
#endif /* DEBUG_PROXY */

	int thridx = 0;
	size_t minload;

	pxy_thrmgr_ctx_t *tmctx = ctx->thrmgr;
	pthread_mutex_lock(&tmctx->mutex);

	minload = tmctx->thr[thridx]->load;
#ifdef DEBUG_THREAD
	log_dbg_printf("===> Proxy connection handler thread status:\n"
	               "thr[%d]: %zu\n", thridx, minload);
#endif /* DEBUG_THREAD */
	for (int idx = 1; idx < tmctx->num_thr; idx++) {
#ifdef DEBUG_THREAD
		log_dbg_printf("thr[%d]: %zu\n", idx, tmctx->thr[idx]->load);
#endif /* DEBUG_THREAD */
		if (minload > tmctx->thr[idx]->load) {
			minload = tmctx->thr[idx]->load;
			thridx = idx;
		}
	}

	ctx->thr = tmctx->thr[thridx];
	ctx->thr->load++;
	ctx->thr->max_load = MAX(ctx->thr->max_load, ctx->thr->load);

	pthread_mutex_unlock(&tmctx->mutex);

	ctx->evbase = ctx->thr->evbase;
	ctx->dnsbase = ctx->thr->dnsbase;

	ctx->next = ctx->thr->conns;
	ctx->thr->conns = ctx;

	// @attention We are running on the thrmgr thread, do not call conn thread functions here.
	//pxy_thrmgr_print_thr_info(ctx->thr);

#ifdef DEBUG_THREAD
	log_dbg_printf("thridx: %d\n", thridx);
#endif /* DEBUG_THREAD */
}

void
pxy_thrmgr_attach_child(pxy_conn_ctx_t *ctx)
{
#ifdef DEBUG_PROXY
	log_dbg_level_printf(LOG_DBG_MODE_FINEST, "pxy_thrmgr_attach_child: ENTER\n");
#endif /* DEBUG_PROXY */

	pthread_mutex_lock(&ctx->thrmgr->mutex);
	ctx->thr->load++;
	ctx->thr->max_load = MAX(ctx->thr->max_load, ctx->thr->load);
	pthread_mutex_unlock(&ctx->thrmgr->mutex);
}

/*
 * Detach a connection from a thread by index.
 * This function cannot fail.
 */
void
pxy_thrmgr_detach(pxy_conn_ctx_t *ctx)
{
	assert(ctx->children == NULL);

	pthread_mutex_lock(&ctx->thrmgr->mutex);
	ctx->thr->load--;
	pxy_thrmgr_remove_conn(ctx, &ctx->thr->conns);
	pthread_mutex_unlock(&ctx->thrmgr->mutex);
}

void
pxy_thrmgr_detach_child(pxy_conn_ctx_t *ctx)
{
#ifdef DEBUG_PROXY
	log_dbg_level_printf(LOG_DBG_MODE_FINEST, "pxy_thrmgr_detach_child: ENTER\n");
#endif /* DEBUG_PROXY */

	pthread_mutex_lock(&ctx->thrmgr->mutex);
	ctx->thr->load--;
	pthread_mutex_unlock(&ctx->thrmgr->mutex);
}

/* vim: set noet ft=c: */
