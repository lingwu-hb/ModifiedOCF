/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "engine_rd.h"
#include <signal.h>
#include "../concurrency/ocf_concurrency.h"
#include "../metadata/metadata.h"
#include "../ocf_cache_priv.h"
#include "../ocf_def_priv.h"
#include "../ocf_request.h"
#include "../utils/utils_cache_line.h"
#include "../utils/utils_debug.h"
#include "../utils/utils_history_hash.h"
#include "../utils/utils_io.h"
#include "../utils/utils_user_part.h"
#include "cache_engine.h"
#include "engine_bf.h"
#include "engine_common.h"
#include "engine_inv.h"
#include "engine_pt.h"
#include "ocf/ocf.h"

#define OCF_ENGINE_DEBUG_IO_NAME "rd"
#include "engine_debug.h"

static env_atomic total_requests;
static env_atomic cache_write_requests;

static void _ocf_read_generic_hit_complete(struct ocf_request* req, int error) {
    struct ocf_alock* c = ocf_cache_line_concurrency(
        req->cache);

    if (error)
        req->error |= error;

    if (req->error)
        inc_fallback_pt_error_counter(req->cache);

    /* Handle callback-caller race to let only one of the two complete the
     * request. Also, complete original request only if this is the last
     * sub-request to complete
     */
    if (env_atomic_dec_return(&req->req_remaining) == 0) {
        OCF_DEBUG_RQ(req, "HIT completion");

        if (req->error) {
            ocf_core_stats_cache_error_update(req->core, OCF_READ);
            ocf_engine_push_req_front_pt(req);
        } else {
            ocf_req_unlock(c, req);

            /* Complete request */
            req->complete(req, req->error);

            /* Free the request at the last point
             * of the completion path
             */
            ocf_req_put(req);
        }
    }
}

static void _ocf_read_generic_miss_complete(struct ocf_request* req, int error) {
    struct ocf_cache* cache = req->cache;

    if (error)
        req->error = error;

    /* Handle callback-caller race to let only one of the two complete the
     * request. Also, complete original request only if this is the last
     * sub-request to complete
     */
    if (env_atomic_dec_return(&req->req_remaining) == 0) {
        OCF_DEBUG_RQ(req, "MISS completion");

        if (req->error) {
            /*
             * --- Do not submit this request to write-back-thread.
             * Stop it here ---
             */
            req->complete(req, req->error);

            req->info.core_error = 1;
            ocf_core_stats_core_error_update(req->core, OCF_READ);

            ctx_data_free(cache->owner, req->cp_data);
            req->cp_data = NULL;

            /* Invalidate metadata */
            ocf_engine_invalidate(req);

            return;
        }

        /* Copy pages to copy vec, since this is the one needed
         * by the above layer
         */
        ctx_data_cpy(cache->owner, req->cp_data, req->data, 0, 0,
                     req->byte_length);

        /* Complete request */
        req->complete(req, req->error);

        ocf_engine_backfill(req);
    }
}

void ocf_read_generic_submit_hit(struct ocf_request* req) {
    env_atomic_set(&req->req_remaining, ocf_engine_io_count(req));

    ocf_submit_cache_reqs(req->cache, req, OCF_READ, 0, req->byte_length,
                          ocf_engine_io_count(req), _ocf_read_generic_hit_complete);
}

static inline void _ocf_read_generic_submit_miss(struct ocf_request* req) {
    struct ocf_cache* cache = req->cache;
    int ret;

    env_atomic_set(&req->req_remaining, 1);

    req->cp_data = ctx_data_alloc(cache->owner,
                                  BYTES_TO_PAGES(req->byte_length));
    if (!req->cp_data)
        goto err_alloc;

    ret = ctx_data_mlock(cache->owner, req->cp_data);
    if (ret)
        goto err_alloc;

    /* Submit read request to core device. */
    ocf_submit_volume_req(&req->core->volume, req,
                          _ocf_read_generic_miss_complete);

    return;

err_alloc:
    _ocf_read_generic_miss_complete(req, -OCF_ERR_NO_MEM);
}

static int _ocf_read_generic_do(struct ocf_request* req) {
    if (ocf_engine_is_miss(req) && req->alock_rw == OCF_READ) {
        /* Miss can be handled only on write locks.
         * Need to switch to PT
         */
        OCF_DEBUG_RQ(req, "Switching to PT");
        ocf_read_pt_do(req);
        return 0;
    }

    /* Get OCF request - increase reference counter */
    ocf_req_get(req);

    if (ocf_engine_is_miss(req)) {
        if (req->info.dirty_any) {
            ocf_hb_req_prot_lock_rd(req);

            /* Request is dirty need to clean request */
            ocf_engine_clean(req);

            ocf_hb_req_prot_unlock_rd(req);

            /* We need to clean request before processing, return */
            ocf_req_put(req);

            return 0;
        }

        ocf_hb_req_prot_lock_wr(req);

        /* Set valid status bits map */
        ocf_set_valid_map_info(req);

        ocf_hb_req_prot_unlock_wr(req);
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

    OCF_DEBUG_RQ(req, "Submit");

    /* Submit IO */
    if (ocf_engine_is_hit(req))
        ocf_read_generic_submit_hit(req);
    else
        // 由于前面已经分配好了缓存空间，数据直接读取到了缓存空间中！
        _ocf_read_generic_submit_miss(req);

    /* Update statistics */
    ocf_engine_update_request_stats(req);
    ocf_engine_update_block_stats(req);

    /* Put OCF request - decrease reference counter */
    ocf_req_put(req);

    return 0;
}

static const struct ocf_io_if _io_if_read_generic_resume = {
    .read = _ocf_read_generic_do,
    .write = _ocf_read_generic_do,
};

static const struct ocf_engine_callbacks _rd_engine_callbacks =
    {
        .resume = ocf_engine_on_resume,
};

int ocf_read_generic(struct ocf_request* req) {
    int lock = OCF_LOCK_NOT_ACQUIRED;
    struct ocf_cache* cache = req->cache;
    env_atomic_inc(&total_requests);

    ocf_io_start(&req->ioi.io);

    if (env_atomic_read(&cache->pending_read_misses_list_blocked)) {
        /* 如果有待处理的读取未命中，直接bypass到PT模式 */
        req->force_pt = true;
        ocf_get_io_if(ocf_cache_mode_pt)->read(req);
        return 0;
    }

    /* 增加引用计数 */
    ocf_req_get(req);

    /* 设置回调函数 */
    req->io_if = &_io_if_read_generic_resume;
    req->engine_cbs = &_rd_engine_callbacks;


    /* 使用宏定义计算页面对齐的地址和总页数 */
    uint64_t start_addr = PAGE_ALIGN_DOWN(req->ioi.io.addr);
    uint64_t end_addr = PAGE_ALIGN_DOWN(req->ioi.io.addr + req->ioi.io.bytes - 1);
    uint64_t total_pages = PAGES_IN_REQ(start_addr, end_addr);
    uint64_t hit_pages = 0;

    /* 检查历史记录中的命中情况 */
    for (uint64_t curr_addr = start_addr; curr_addr <= end_addr; curr_addr += PAGE_SIZE) {
        if (ocf_history_hash_find(curr_addr, ocf_core_get_id(req->core))) {
            hit_pages++;
        }
    }

    /* 每1000个请求输出一次统计信息 */
    if (env_atomic_read(&total_requests) % 1000 == 0) {
        // ocf_history_hash_print_stats();
    }

    // 输出当前请求的相关信息
    printf("\e[31m[INFO]\e[0m addr: %lx, size: %lx, start_addr: %lx, end_addr: %lx\n", req->ioi.io.addr, req->ioi.io.bytes, start_addr, end_addr);
    printf("hit_pages: %d, total_pages: %d, hit_ratio: %f\n", hit_pages, total_pages, (float)hit_pages / total_pages);

    // 判断缓存使用情况，缓存占满了才启用二次准入，否则不启用
    bool cache_full = ocf_is_cache_full(req->cache);

    /* 如果历史命中率低于阈值，添加的 4K 块到历史记录并直接PT */
    if ((float)hit_pages / total_pages < HISTORY_HIT_RATIO_THRESHOLD && cache_full) {
        OCF_DEBUG_IO("PT, History miss", req);

        // 将当前请求涉及到的所有 4K 块都尝试添加到历史记录中
        // 因为考虑到要更新 LRU 链表，所有需要都进行尝试添加
        // need to mantain: 理论上命中的块不需要添加，此处为了方便，没有重整代码
        for (uint64_t curr_addr = start_addr; curr_addr <= end_addr; curr_addr += PAGE_SIZE) {
            ocf_history_hash_add_addr(curr_addr, ocf_core_get_id(req->core));
        }

        ocf_req_clear(req);
        req->force_pt = true;
        ocf_get_io_if(ocf_cache_mode_pt)->read(req);
        ocf_req_put(req);
        return 0;
    }

    /* 准备缓存行 */
    lock = ocf_engine_prepare_clines(req);

    if (!ocf_req_test_mapping_error(req)) {
        if (lock >= 0) {
            if (lock == OCF_LOCK_ACQUIRED) {
                /* 增加缓存写入计数并打印信息 */
                OCF_DEBUG_IO("Write Cache", req);
                env_atomic_inc(&cache_write_requests);
                /* 执行IO操作 */
                _ocf_read_generic_do(req);
            } else {
                /* 未获取到锁，需要等待恢复 */
                OCF_DEBUG_RQ(req, "NO LOCK");
                OCF_DEBUG_IO("NO Lock", req);
            }
        } else {
            OCF_DEBUG_RQ(req, "LOCK ERROR %d", lock);
            OCF_DEBUG_IO("Lock Error", req);
            req->complete(req, lock);
            ocf_req_put(req);
        }
    } else {
        OCF_DEBUG_IO("PT, Map error", req);
        ocf_req_clear(req);
        req->force_pt = true;
        ocf_get_io_if(ocf_cache_mode_pt)->read(req);
    }

    /* 减少引用计数 */
    ocf_req_put(req);

    return 0;
}
