/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdqueue.h"
#include "apr_atomic.h"

typedef struct recycled_pool
{
    apr_pool_t *pool;
    struct recycled_pool *next;
} recycled_pool;

struct fd_queue_info_t
{
    apr_int32_t idlers;      /**
                                  * 0 or positive: number of idle worker threads
                              * negative: number of threads blocked waiting
                              *           for an idle worker
                              */
    apr_thread_mutex_t *idlers_mutex;
    apr_thread_cond_t *wait_for_idler;
    int terminated;
    int max_idlers;
    recycled_pool *recycled_pools;
};

static apr_status_t queue_info_cleanup(void *data_)
{
    fd_queue_info_t *qi = data_;
    apr_thread_cond_destroy(qi->wait_for_idler);
    apr_thread_mutex_destroy(qi->idlers_mutex);

    /* Clean up any pools in the recycled list */
    for (;;) {
        struct recycled_pool *first_pool = qi->recycled_pools;
        if (first_pool == NULL) {
            break;
        }
        if (apr_atomic_casptr
            ((volatile void **) &(qi->recycled_pools), first_pool->next,
             first_pool) == first_pool) {
            apr_pool_destroy(first_pool->pool);
        }
    }

    return APR_SUCCESS;
}

apr_status_t ap_queue_info_create(fd_queue_info_t ** queue_info,
                                  apr_pool_t * pool, int max_idlers)
{
    apr_status_t rv;
    fd_queue_info_t *qi;

    qi = apr_pcalloc(pool, sizeof(*qi));

    rv = apr_thread_mutex_create(&qi->idlers_mutex, APR_THREAD_MUTEX_DEFAULT,
                                 pool);
    if (rv != APR_SUCCESS) {
        return rv;
    }
    rv = apr_thread_cond_create(&qi->wait_for_idler, pool);
    if (rv != APR_SUCCESS) {
        return rv;
    }
    qi->recycled_pools = NULL;
    qi->max_idlers = max_idlers;
    apr_pool_cleanup_register(pool, qi, queue_info_cleanup,
                              apr_pool_cleanup_null);

    *queue_info = qi;

    return APR_SUCCESS;
}

apr_status_t ap_queue_info_set_idle(fd_queue_info_t * queue_info,
                                    apr_pool_t * pool_to_recycle)
{
    apr_status_t rv;
    int prev_idlers;

    ap_push_pool(queue_info, pool_to_recycle);

    /* Atomically increment the count of idle workers */
    prev_idlers = apr_atomic_inc32(&(queue_info->idlers));

    /* If other threads are waiting on a worker, wake one up */
    if (prev_idlers < 0) {
        rv = apr_thread_mutex_lock(queue_info->idlers_mutex);
        if (rv != APR_SUCCESS) {
            AP_DEBUG_ASSERT(0);
            return rv;
        }
        rv = apr_thread_cond_signal(queue_info->wait_for_idler);
        if (rv != APR_SUCCESS) {
            apr_thread_mutex_unlock(queue_info->idlers_mutex);
            return rv;
        }
        rv = apr_thread_mutex_unlock(queue_info->idlers_mutex);
        if (rv != APR_SUCCESS) {
            return rv;
        }
    }

    return APR_SUCCESS;
}

apr_status_t ap_queue_info_wait_for_idler(fd_queue_info_t * queue_info)
{
    apr_status_t rv;
    int prev_idlers;

    /* Atomically decrement the idle worker count, saving the old value */
    prev_idlers = apr_atomic_add32(&(queue_info->idlers), -1);

    /* Block if there weren't any idle workers */
    if (prev_idlers <= 0) {
        rv = apr_thread_mutex_lock(queue_info->idlers_mutex);
        if (rv != APR_SUCCESS) {
            AP_DEBUG_ASSERT(0);
            apr_atomic_inc32(&(queue_info->idlers));    /* back out dec */
            return rv;
        }
        /* Re-check the idle worker count to guard against a
         * race condition.  Now that we're in the mutex-protected
         * region, one of two things may have happened:
         *   - If the idle worker count is still negative, the
         *     workers are all still busy, so it's safe to
         *     block on a condition variable.
         *   - If the idle worker count is non-negative, then a
         *     worker has become idle since the first check
         *     of queue_info->idlers above.  It's possible
         *     that the worker has also signaled the condition
         *     variable--and if so, the listener missed it
         *     because it wasn't yet blocked on the condition
         *     variable.  But if the idle worker count is
         *     now non-negative, it's safe for this function to
         *     return immediately.
         *
         *     A negative value in queue_info->idlers tells how many
         *     threads are waiting on an idle worker.
         */
        if (queue_info->idlers < 0) {
            rv = apr_thread_cond_wait(queue_info->wait_for_idler,
                                      queue_info->idlers_mutex);
            if (rv != APR_SUCCESS) {
                apr_status_t rv2;
                AP_DEBUG_ASSERT(0);
                rv2 = apr_thread_mutex_unlock(queue_info->idlers_mutex);
                if (rv2 != APR_SUCCESS) {
                    return rv2;
                }
                return rv;
            }
        }
        rv = apr_thread_mutex_unlock(queue_info->idlers_mutex);
        if (rv != APR_SUCCESS) {
            return rv;
        }
    }

    if (queue_info->terminated) {
        return APR_EOF;
    }
    else {
        return APR_SUCCESS;
    }
}


void ap_push_pool(fd_queue_info_t * queue_info,
                                    apr_pool_t * pool_to_recycle)
{
    /* If we have been given a pool to recycle, atomically link
     * it into the queue_info's list of recycled pools
     */
    if (pool_to_recycle) {
        struct recycled_pool *new_recycle;
        new_recycle = (struct recycled_pool *) apr_palloc(pool_to_recycle,
                                                          sizeof
                                                          (*new_recycle));
        new_recycle->pool = pool_to_recycle;
        for (;;) {
            new_recycle->next = queue_info->recycled_pools;
            if (apr_atomic_casptr
                ((volatile void **) &(queue_info->recycled_pools),
                 new_recycle, new_recycle->next) == new_recycle->next) {
                break;
            }
        }
    }
}

void ap_pop_pool(apr_pool_t ** recycled_pool, fd_queue_info_t * queue_info)
{
    /* Atomically pop a pool from the recycled list */

    /* This function is safe only as long as it is single threaded because
     * it reaches into the queue and accesses "next" which can change.
     * We are OK today because it is only called from the listener thread.
     * cas-based pushes do not have the same limitation - any number can
     * happen concurrently with a single cas-based pop.
     */

    *recycled_pool = NULL;


    /* Atomically pop a pool from the recycled list */
    for (;;) {
        struct recycled_pool *first_pool = queue_info->recycled_pools;
        if (first_pool == NULL) {
            break;
        }
        if (apr_atomic_casptr
            ((volatile void **) &(queue_info->recycled_pools),
             first_pool->next, first_pool) == first_pool) {
            *recycled_pool = first_pool->pool;
            break;
        }
    }
}

apr_status_t ap_queue_info_term(fd_queue_info_t * queue_info)
{
    apr_status_t rv;
    rv = apr_thread_mutex_lock(queue_info->idlers_mutex);
    if (rv != APR_SUCCESS) {
        return rv;
    }
    queue_info->terminated = 1;
    apr_thread_cond_broadcast(queue_info->wait_for_idler);
    return apr_thread_mutex_unlock(queue_info->idlers_mutex);
}

/**
 * Detects when the fd_queue_t is full. This utility function is expected
 * to be called from within critical sections, and is not threadsafe.
 */
#define ap_queue_full(queue) ((queue)->nelts == (queue)->bounds)

/**
 * Detects when the fd_queue_t is empty. This utility function is expected
 * to be called from within critical sections, and is not threadsafe.
 */
#define ap_queue_empty(queue) ((queue)->nelts == 0)

/**
 * Callback routine that is called to destroy this
 * fd_queue_t when its pool is destroyed.
 */
static apr_status_t ap_queue_destroy(void *data)
{
    fd_queue_t *queue = data;

    /* Ignore errors here, we can't do anything about them anyway.
     * XXX: We should at least try to signal an error here, it is
     * indicative of a programmer error. -aaron */
    apr_thread_cond_destroy(queue->not_empty);
    apr_thread_mutex_destroy(queue->one_big_mutex);

    return APR_SUCCESS;
}

/**
 * Initialize the fd_queue_t.
 */
apr_status_t ap_queue_init(fd_queue_t * queue, int queue_capacity,
                           apr_pool_t * a)
{
    int i;
    apr_status_t rv;

    if ((rv = apr_thread_mutex_create(&queue->one_big_mutex,
                                      APR_THREAD_MUTEX_DEFAULT,
                                      a)) != APR_SUCCESS) {
        return rv;
    }
    if ((rv = apr_thread_cond_create(&queue->not_empty, a)) != APR_SUCCESS) {
        return rv;
    }

    queue->data = apr_palloc(a, queue_capacity * sizeof(fd_queue_elem_t));
    queue->bounds = queue_capacity;
    queue->nelts = 0;

    /* Set all the sockets in the queue to NULL */
    for (i = 0; i < queue_capacity; ++i)
        queue->data[i].sd = NULL;

    apr_pool_cleanup_register(a, queue, ap_queue_destroy,
                              apr_pool_cleanup_null);

    return APR_SUCCESS;
}

/**
 * Push a new socket onto the queue.
 *
 * precondition: ap_queue_info_wait_for_idler has already been called
 *               to reserve an idle worker thread
 */
apr_status_t ap_queue_push(fd_queue_t * queue, apr_socket_t * sd,
                           conn_state_t * cs, apr_pool_t * p)
{
    fd_queue_elem_t *elem;
    apr_status_t rv;

    if ((rv = apr_thread_mutex_lock(queue->one_big_mutex)) != APR_SUCCESS) {
        return rv;
    }

    AP_DEBUG_ASSERT(!queue->terminated);
    AP_DEBUG_ASSERT(!ap_queue_full(queue));

    elem = &queue->data[queue->nelts];
    elem->sd = sd;
    elem->cs = cs;
    elem->p = p;
    queue->nelts++;

    apr_thread_cond_signal(queue->not_empty);

    if ((rv = apr_thread_mutex_unlock(queue->one_big_mutex)) != APR_SUCCESS) {
        return rv;
    }

    return APR_SUCCESS;
}

/**
 * Retrieves the next available socket from the queue. If there are no
 * sockets available, it will block until one becomes available.
 * Once retrieved, the socket is placed into the address specified by
 * 'sd'.
 */
apr_status_t ap_queue_pop(fd_queue_t * queue, apr_socket_t ** sd,
                          conn_state_t ** cs, apr_pool_t ** p)
{
    fd_queue_elem_t *elem;
    apr_status_t rv;

    if ((rv = apr_thread_mutex_lock(queue->one_big_mutex)) != APR_SUCCESS) {
        return rv;
    }

    /* Keep waiting until we wake up and find that the queue is not empty. */
    if (ap_queue_empty(queue)) {
        if (!queue->terminated) {
            apr_thread_cond_wait(queue->not_empty, queue->one_big_mutex);
        }
        /* If we wake up and it's still empty, then we were interrupted */
        if (ap_queue_empty(queue)) {
            rv = apr_thread_mutex_unlock(queue->one_big_mutex);
            if (rv != APR_SUCCESS) {
                return rv;
            }
            if (queue->terminated) {
                return APR_EOF; /* no more elements ever again */
            }
            else {
                return APR_EINTR;
            }
        }
    }

    elem = &queue->data[--queue->nelts];
    *sd = elem->sd;
    *cs = elem->cs;
    *p = elem->p;
#ifdef AP_DEBUG
    elem->sd = NULL;
    elem->p = NULL;
#endif /* AP_DEBUG */

    rv = apr_thread_mutex_unlock(queue->one_big_mutex);
    return rv;
}

apr_status_t ap_queue_interrupt_all(fd_queue_t * queue)
{
    apr_status_t rv;

    if ((rv = apr_thread_mutex_lock(queue->one_big_mutex)) != APR_SUCCESS) {
        return rv;
    }
    apr_thread_cond_broadcast(queue->not_empty);
    return apr_thread_mutex_unlock(queue->one_big_mutex);
}

apr_status_t ap_queue_term(fd_queue_t * queue)
{
    apr_status_t rv;

    if ((rv = apr_thread_mutex_lock(queue->one_big_mutex)) != APR_SUCCESS) {
        return rv;
    }
    /* we must hold one_big_mutex when setting this... otherwise,
     * we could end up setting it and waking everybody up just after a
     * would-be popper checks it but right before they block
     */
    queue->terminated = 1;
    if ((rv = apr_thread_mutex_unlock(queue->one_big_mutex)) != APR_SUCCESS) {
        return rv;
    }
    return ap_queue_interrupt_all(queue);
}
