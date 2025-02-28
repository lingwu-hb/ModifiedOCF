/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "engine_rd.h"
#include "../concurrency/ocf_concurrency.h"
#include "../metadata/metadata.h"
#include "../ocf_cache_priv.h"
#include "../ocf_def_priv.h"
#include "../ocf_request.h"
#include "../utils/utils_cache_line.h"
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

/* 定义哈希表大小 */
#define HISTORY_HASH_SIZE 1024
#define TIME_THRESHOLD 2  // 设定请求次数阈值
#define MAX_HISTORY 100   // 最大历史请求数

/* 哈希表节点结构 */
struct history_node {
    struct ocf_request* req;
    struct history_node* next;
};
typedef struct history_node history_node_t;

/* 哈希表 */
static history_node_t* history_hash[HISTORY_HASH_SIZE];
static int history_count = 0;

/* 计算哈希值 */
static unsigned int calc_hash(uint64_t addr, int core_id) {
    return (unsigned int)((addr ^ core_id) % HISTORY_HASH_SIZE);
}

/* 在哈希表中查找请求 */
static bool find_request_in_history(uint64_t addr, int core_id) {
    unsigned int hash = calc_hash(addr, core_id);
    struct history_node* node = history_hash[hash];

    while (node) {
        if (node->req && node->req->ioi.io.addr == addr &&
            ocf_core_get_id(node->req->core) == core_id) {
            return true;  // 找到匹配的请求
        }
        node = node->next;
    }

    return false;  // 未找到匹配的请求
}

/* 添加请求到哈希表 */
static void add_request_to_history(struct ocf_request* req) {
    unsigned int hash = calc_hash(req->ioi.io.addr, ocf_core_get_id(req->core));
    history_node_t* new_node = env_malloc(sizeof(history_node_t), ENV_MEM_NORMAL);

    if (!new_node)
        return;

    new_node->req = req;
    new_node->next = history_hash[hash];
    history_hash[hash] = new_node;
    history_count++;

    /* 如果超过最大历史数量，清理最旧的记录 */
    if (history_count > MAX_HISTORY) {
        /* 找到并移除最旧的记录 */
        for (int i = 0; i < HISTORY_HASH_SIZE; i++) {
            if (history_hash[i]) {
                struct history_node* temp = history_hash[i];
                history_hash[i] = temp->next;
                env_free(temp);
                history_count--;
                break;
            }
        }
    }
}

static env_atomic_t total_requests = 0;
static env_atomic_t cache_write_requests = 0;

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

    ocf_io_start(&req->ioi.io);

    if (env_atomic_read(&cache->pending_read_misses_list_blocked)) {
        /* There are conditions to bypass IO */
        req->force_pt = true;
        ocf_get_io_if(ocf_cache_mode_pt)->read(req);
        return 0;
    }

    /* Get OCF request - increase reference counter */
    ocf_req_get(req);

    /* Set resume call backs */
    req->io_if = &_io_if_read_generic_resume;
    req->engine_cbs = &_rd_engine_callbacks;

    lock = ocf_engine_prepare_clines(req);

    bool is_in_history = find_request_in_history(req->ioi.io.addr, ocf_core_get_id(req->core));
    if (!is_in_history) {  // 如果历史 IO 中没有找到，则将请求添加到历史 IO 中，直接 pass-thru
        add_request_to_history(req);
        ocf_req_clear(req);
        req->force_pt = true;
        ocf_get_io_if(ocf_cache_mode_pt)->read(req);
    } else {  // 如果历史 IO 中找到了，则按照正常的流程执行 IO 操作
        if (!ocf_req_test_mapping_error(req)) {
            if (lock >= 0) {
                if (lock != OCF_LOCK_ACQUIRED) {
                    /* Lock was not acquired, need to wait for resume */
                    OCF_DEBUG_RQ(req, "NO LOCK");
                } else {
                    // 尝试往缓存中写入的 IO 操作
                    /* 增加缓存写入计数并打印信息 */
                    env_atomic_inc(&cache_write_requests);
                    OCF_DEBUG_RQ(req, "[Cache Write] Address: %llu, Core: %u, Total: %d, Cache: %d, Ratio: %d%%",
                                 req->ioi.io.addr,
                                 ocf_core_get_id(req->core),
                                 env_atomic_read(&total_requests),
                                 env_atomic_read(&cache_write_requests),
                                 env_atomic_read(&cache_write_requests) * 100 / env_atomic_read(&total_requests));

                    /* Lock was acquired can perform IO */
                    _ocf_read_generic_do(req);
                }
            } else {
                OCF_DEBUG_RQ(req, "LOCK ERROR %d", lock);
                req->complete(req, lock);
                ocf_req_put(req);
            }
        } else {
            ocf_req_clear(req);
            req->force_pt = true;
            ocf_get_io_if(ocf_cache_mode_pt)->read(req);
        }
    }

    /* 打印当前统计信息 */
    OCF_DEBUG_RQ(req, "[Stats] Total Requests: %d, Cache Writes: %d, Cache Write Ratio: %d%%",
                 env_atomic_read(&total_requests),
                 env_atomic_read(&cache_write_requests),
                 env_atomic_read(&cache_write_requests) * 100 / env_atomic_read(&total_requests));

    /* Put OCF request - decrease reference counter */
    ocf_req_put(req);

    return 0;
}
