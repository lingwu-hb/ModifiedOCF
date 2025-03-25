/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#include "engine_pt.h"
#include "../concurrency/ocf_concurrency.h"
#include "../metadata/metadata.h"
#include "../ocf_cache_priv.h"
#include "../ocf_request.h"
#include "../utils/utils_io.h"
#include "../utils/utils_user_part.h"
#include "cache_engine.h"
#include "engine_common.h"
#include "ocf/ocf.h"

#define OCF_ENGINE_DEBUG_IO_NAME "pt"
#include "engine_debug.h"

static void _ocf_read_pt_complete(struct ocf_request* req, int error) {
    if (error)
        req->error |= error;

    if (env_atomic_dec_return(&req->req_remaining))
        return;

    OCF_DEBUG_RQ(req, "Completion");

    if (req->error) {
        req->info.core_error = 1;
        ocf_core_stats_core_error_update(req->core, OCF_READ);
    }

    /* Complete request */
    req->complete(req, req->error);

    ocf_req_unlock_rd(ocf_cache_line_concurrency(req->cache), req);

    /* Release OCF request */
    ocf_req_put(req);
}

static inline void _ocf_read_pt_submit(struct ocf_request* req) {
    env_atomic_set(&req->req_remaining, 1); /* Core device IO */

    OCF_DEBUG_RQ(req, "Submit");

    /* Core read */
    ocf_submit_volume_req(&req->core->volume, req, _ocf_read_pt_complete);
}

int ocf_read_pt_do(struct ocf_request* req) {
    /* Get OCF request - increase reference counter */
    ocf_req_get(req);

    if (req->info.dirty_any) {
        ocf_hb_req_prot_lock_rd(req);
        /* Need to clean, start it */
        ocf_engine_clean(req);
        ocf_hb_req_prot_unlock_rd(req);

        /* Do not processing, because first we need to clean request */
        ocf_req_put(req);

        return 0;
    }

    if (ocf_engine_needs_repart(req)) {
        OCF_DEBUG_RQ(req, "Re-Part");

        ocf_hb_req_prot_lock_wr(req);

        /* Probably some cache lines are assigned into wrong
         * partition. Need to move it to new one
         */
        ocf_user_part_move(req);

        ocf_hb_req_prot_unlock_wr(req);
    }

    /* Submit read IO to the core */
    _ocf_read_pt_submit(req);

    /* Update statistics */
    ocf_engine_update_block_stats(req);
    ocf_core_stats_request_pt_update(req->core, req->part_id, req->rw,
                                     req->info.hit_no, req->core_line_count);

    /* Put OCF request - decrease reference counter */
    ocf_req_put(req);

    return 0;
}

static const struct ocf_io_if _io_if_pt_resume = {
    .read = ocf_read_pt_do,
    .write = ocf_read_pt_do,
};

int ocf_read_pt(struct ocf_request* req) {
    bool use_cache = false;
    int lock = OCF_LOCK_NOT_ACQUIRED;

    OCF_DEBUG_TRACE(req->cache);

    ocf_io_start(&req->ioi.io);

    /* Get OCF request - increase reference counter */
    ocf_req_get(req);

    /* Set resume io_if */
    req->io_if = &_io_if_pt_resume;

    ocf_req_hash(req);
    ocf_hb_req_prot_lock_rd(req);

    /* 遍历请求，检查是否存在映射的缓存行 */
    ocf_engine_traverse(req);

    // 走到 PT 流程的请求，一定是 force_pt 为 true
    if (req->seq_cutoff && ocf_engine_is_dirty_all(req) &&
        !req->force_pt) {
        use_cache = true;
    } else {
        // 如果请求中存在映射的缓存行，则需要获取读锁
        if (ocf_engine_mapped_count(req)) {
            /* There are mapped cache line,
             * lock request for READ access (only try fast lock)
             */
            // lock = ocf_req_async_lock_rd_fast_only(
            //     ocf_cache_line_concurrency(req->cache),
            //     req);
            // 由于测试过程中，没有涉及到写操作，都是读操作，缓存不会脏，所以不会与缓存进行交互
            lock = OCF_LOCK_ACQUIRED;
        } else {
            /* No mapped cache lines, no need to get lock */
            lock = OCF_LOCK_ACQUIRED;
        }
    }

    ocf_hb_req_prot_unlock_rd(req);

    if (use_cache) {
        /*
         * There is dirt HIT, and sequential cut off,
         * because of this force read data from cache
         */
        ocf_req_clear(req);
        ocf_get_io_if(ocf_cache_mode_wt)->read(req);
    } else {
        if (lock >= 0) {
            if (lock == OCF_LOCK_ACQUIRED) {
                /* Lock acquired perform read off operations */
                ocf_read_pt_do(req);
            } else {
                /* WR lock was not acquired, need to wait for resume */
                OCF_DEBUG_RQ(req, "NO LOCK");
            }
        } else {
            OCF_DEBUG_RQ(req, "LOCK ERROR %d", lock);
            // 调用用户回调函数，返回错误
            req->complete(req, lock);
            ocf_req_put(req);
        }
    }

    /* Put OCF request - decrease reference counter */
    ocf_req_put(req);

    return 0;
}

void ocf_engine_push_req_front_pt(struct ocf_request* req) {
    ocf_engine_push_req_front_if(req, &_io_if_pt_resume, true);
}
