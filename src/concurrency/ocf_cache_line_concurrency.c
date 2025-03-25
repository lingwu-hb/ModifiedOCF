/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "../ocf_priv.h"
#include "../ocf_request.h"
#include "../utils/utils_alock.h"
#include "../utils/utils_cache_line.h"
#include "ocf_concurrency.h"

static bool ocf_cl_lock_line_needs_lock(struct ocf_alock* alock,
                                        struct ocf_request* req,
                                        unsigned index) {
    /* Remapped cachelines are assigned cacheline lock individually
     * during eviction
     */

    return req->map[index].status != LOOKUP_MISS &&
           req->map[index].status != LOOKUP_REMAPPED;
}

static bool ocf_cl_lock_line_is_acting(struct ocf_alock* alock,
                                       struct ocf_request* req,
                                       unsigned index) {
    return req->map[index].status != LOOKUP_MISS;
}

static ocf_cache_line_t ocf_cl_lock_line_get_entry(
    struct ocf_alock* alock,
    struct ocf_request* req,
    unsigned index) {
    return req->map[index].coll_idx;
}

/**
 * @brief 尝试快速获取请求中所有缓存行的锁
 *
 * 该函数遍历请求中的所有缓存行，尝试立即（非阻塞）获取锁。
 * 如果任何一个缓存行无法立即获取锁，将释放所有已获取的锁并返回失败。
 *
 * @param alock - 缓存行锁控制结构
 * @param req - OCF 请求结构
 * @param rw - 锁类型（OCF_READ 或 OCF_WRITE）
 * 
 * @return OCF_LOCK_ACQUIRED - 成功获取所有需要的锁
 *         OCF_LOCK_NOT_ACQUIRED - 无法获取至少一个锁
 */
static int ocf_cl_lock_line_fast(struct ocf_alock* alock,
                                 struct ocf_request* req,
                                 int rw) {
    int32_t i;                       // 循环计数器，用于遍历请求中的所有缓存行
    ocf_cache_line_t entry;          // 当前处理的缓存行索引
    int ret = OCF_LOCK_ACQUIRED;     // 返回值，默认为成功获取锁

    // 遍历请求中的所有缓存行
    for (i = 0; i < req->core_line_count; i++) {
        // 检查当前缓存行是否需要锁定
        // 在某些情况下（如MISS或REMAPPED状态），不需要锁定
        if (!ocf_cl_lock_line_needs_lock(alock, req, i)) {
            /* nothing to lock */
            continue;
        }

        // 获取当前缓存行的碰撞索引（collision index）
        entry = ocf_cl_lock_line_get_entry(alock, req, i);
        // 确保该缓存行尚未被锁定（开发断言）
        ENV_BUG_ON(ocf_alock_is_index_locked(alock, req, i));

        // 根据请求类型尝试获取不同类型的锁
        if (rw == OCF_WRITE) {
            // 尝试获取写锁（互斥锁）
            if (ocf_alock_trylock_entry_wr(alock, entry)) {
                /* cache entry locked */
                // 成功获取锁，标记该缓存行已锁定
                ocf_alock_mark_index_locked(alock, req, i, true);
            } else {
                /* Not possible to lock all cachelines */
                // 无法获取写锁，设置返回值为未获取锁
                ret = OCF_LOCK_NOT_ACQUIRED;
                break;  // 立即退出循环，不再尝试锁定其他缓存行
            }
        } else {
            // 尝试获取读锁（共享锁），但只有在缓存行闲置时才能获取
            if (ocf_alock_trylock_entry_rd_idle(alock, entry)) {
                /* cache entry locked */
                // 成功获取锁，标记该缓存行已锁定
                ocf_alock_mark_index_locked(alock, req, i, true);
            } else {
                /* Not possible to lock all cachelines */
                // 无法获取读锁，设置返回值为未获取锁
                ret = OCF_LOCK_NOT_ACQUIRED;
                break;  // 立即退出循环，不再尝试锁定其他缓存行
            }
        }
    }

    /* Check if request is locked */
    // 如果无法获取所有锁，需要释放已获取的锁
    if (ret == OCF_LOCK_NOT_ACQUIRED) {
        /* Request is not locked, discard acquired locks */
        // 从当前缓存行开始，向前释放所有已获取的锁
        for (; i >= 0; i--) {
            // 跳过不需要锁定的缓存行
            if (!ocf_cl_lock_line_needs_lock(alock, req, i))
                continue;

            // 获取缓存行索引
            entry = ocf_cl_lock_line_get_entry(alock, req, i);

            // 检查该缓存行是否已被锁定
            if (ocf_alock_is_index_locked(alock, req, i)) {
                // 根据锁类型释放对应的锁
                if (rw == OCF_WRITE) {
                    // 释放写锁
                    ocf_alock_unlock_one_wr(alock, entry);
                } else {
                    // 释放读锁
                    ocf_alock_unlock_one_rd(alock, entry);
                }
                // 标记该缓存行未锁定
                ocf_alock_mark_index_locked(alock, req, i, false);
            }
        }
    }

    // 返回锁定结果：OCF_LOCK_ACQUIRED 或 OCF_LOCK_NOT_ACQUIRED
    return ret;
}

/**
 * @brief 尝试获取请求中所有缓存行的锁（慢速路径）
 *
 * 该函数为锁获取的"慢速路径"，当快速路径无法立即获取所有锁时使用。
 * 它会将请求添加到每个缓存行的等待队列中，以便在锁可用时异步通知。
 * 与fast版本不同，它不要求立即获取锁，而是安排请求在锁可用时继续执行。
 *
 * @param alock - 缓存行锁控制结构
 * @param req - OCF 请求结构
 * @param rw - 锁类型（OCF_READ 或 OCF_WRITE）
 * @param cmpl - 锁获取完成时的回调函数
 * 
 * @return 0 - 成功将请求添加到所有必要的等待队列
 *         -OCF_ERR_NO_MEM - 内存不足，无法将请求添加到等待队列
 */
static int ocf_cl_lock_line_slow(struct ocf_alock* alock,
                                 struct ocf_request* req,
                                 int rw,
                                 ocf_req_async_lock_cb cmpl) {
    int32_t i;                  // 循环计数器，用于遍历请求中的所有缓存行
    ocf_cache_line_t entry;     // 当前处理的缓存行索引
    int ret = 0;                // 返回值，默认为成功

    // 遍历请求中的所有缓存行
    for (i = 0; i < req->core_line_count; i++) {
        // 检查当前缓存行是否需要锁定
        // 如果不需要锁定（例如MISS或REMAPPED状态），则减少等待计数并跳过
        if (!ocf_cl_lock_line_needs_lock(alock, req, i)) {
            /* nothing to lock */
            // 减少请求的锁等待计数，因为这个缓存行不需要锁定
            env_atomic_dec(&req->lock_remaining);
            continue;
        }

        // 获取当前缓存行的碰撞索引
        entry = ocf_cl_lock_line_get_entry(alock, req, i);
        // 确保该缓存行尚未被锁定（开发断言）
        ENV_BUG_ON(ocf_alock_is_index_locked(alock, req, i));

        // 根据请求类型尝试获取不同类型的锁，并加入等待队列
        if (rw == OCF_WRITE) {
            // 尝试获取写锁或将请求添加到等待队列
            if (!ocf_alock_lock_one_wr(alock, entry, cmpl, req, i)) {
                /* lock not acquired and not added to wait list */
                // 无法添加到等待队列（通常是内存分配失败）
                ret = -OCF_ERR_NO_MEM;
                goto err;  // 跳转到错误处理代码
            }
        } else {
            // 尝试获取读锁或将请求添加到等待队列
            if (!ocf_alock_lock_one_rd(alock, entry, cmpl, req, i)) {
                /* lock not acquired and not added to wait list */
                // 无法添加到等待队列（通常是内存分配失败）
                ret = -OCF_ERR_NO_MEM;
                goto err;  // 跳转到错误处理代码
            }
        }
    }

    // 成功将请求添加到所有必要的等待队列
    return ret;

err:
    // 错误处理：从当前位置开始，移除已添加的等待项
    for (; i >= 0; i--) {
        // 跳过不需要锁定的缓存行
        if (!ocf_cl_lock_line_needs_lock(alock, req, i))
            continue;

        // 获取缓存行索引
        entry = ocf_cl_lock_line_get_entry(alock, req, i);
        // 从等待队列中移除请求，或释放已获取的锁
        ocf_alock_waitlist_remove_entry(alock, req, i, entry, rw);
    }

    // 返回错误码
    return ret;
}

static int ocf_cl_lock_line_check_fast(struct ocf_alock* alock,
                                       struct ocf_request* req,
                                       int rw) {
    int32_t i;                       // 循环计数器，用于遍历请求中的所有缓存行
    ocf_cache_line_t entry;          // 当前处理的缓存行索引
    int ret = OCF_LOCK_ACQUIRED;     // 返回值，默认为成功获取锁

    // 遍历请求中的所有缓存行
    for (i = 0; i < req->core_line_count; i++) {
        // 检查当前缓存行是否需要锁定
        // 在某些情况下（如MISS或REMAPPED状态），不需要锁定
        if (!ocf_cl_lock_line_needs_lock(alock, req, i)) {
            /* nothing to lock */
            continue;
        }

        // 获取当前缓存行的碰撞索引（collision index）
        entry = ocf_cl_lock_line_get_entry(alock, req, i);
        // 确保该缓存行尚未被锁定（开发断言）
        ENV_BUG_ON(ocf_alock_is_index_locked(alock, req, i));

        // 如果缓存行未命中，则跳过
        if(req->map[i].status != LOOKUP_HIT) {
            continue;
        }

        // 根据请求类型尝试获取不同类型的锁
        if (rw == OCF_WRITE) {
            // 尝试获取写锁（互斥锁）
            if (ocf_alock_trylock_entry_wr(alock, entry)) {
                /* cache entry locked */
                // 成功获取锁，标记该缓存行已锁定
                ocf_alock_mark_index_locked(alock, req, i, true);
            } else {
                /* Not possible to lock all cachelines */
                // 无法获取写锁，设置返回值为未获取锁
                ret = OCF_LOCK_NOT_ACQUIRED;
                break;  // 立即退出循环，不再尝试锁定其他缓存行
            }
        } else {
            // 尝试获取读锁（共享锁），但只有在缓存行闲置时才能获取
            if (ocf_alock_trylock_entry_rd_idle(alock, entry)) {
                /* cache entry locked */
                // 成功获取锁，标记该缓存行已锁定
                ocf_alock_mark_index_locked(alock, req, i, true);
            } else {
                /* Not possible to lock all cachelines */
                // 无法获取读锁，设置返回值为未获取锁
                ret = OCF_LOCK_NOT_ACQUIRED;
                break;  // 立即退出循环，不再尝试锁定其他缓存行
            }
        }
    }

    
    /* Request is not locked, discard acquired locks */
    // 从当前缓存行开始，向前释放所有已获取的锁
    for (; i >= 0; i--) {
        // 跳过不需要锁定的缓存行
        if (!ocf_cl_lock_line_needs_lock(alock, req, i))
            continue;

        // 获取缓存行索引
        entry = ocf_cl_lock_line_get_entry(alock, req, i);

        if(req->map[i].status != LOOKUP_HIT) {
            continue;
        }

        // 检查该缓存行是否已被锁定
        if (ocf_alock_is_index_locked(alock, req, i)) {
            // 根据锁类型释放对应的锁
            if (rw == OCF_WRITE) {
                // 释放写锁
                ocf_alock_unlock_one_wr(alock, entry);
            } else {
                // 释放读锁
                ocf_alock_unlock_one_rd(alock, entry);
            }
            // 标记该缓存行未锁定
            ocf_alock_mark_index_locked(alock, req, i, false);
        }
    }
    
    // 返回锁定结果：OCF_LOCK_ACQUIRED 或 OCF_LOCK_NOT_ACQUIRED
    return ret;
}

static struct ocf_alock_lock_cbs ocf_cline_conc_cbs = {
    .lock_entries_fast = ocf_cl_lock_line_fast,
    .lock_entries_check_fast = ocf_cl_lock_line_check_fast,
    .lock_entries_slow = ocf_cl_lock_line_slow};

bool ocf_cache_line_try_lock_rd(struct ocf_alock* alock,
                                ocf_cache_line_t line) {
    return ocf_alock_trylock_one_rd(alock, line);
}

void ocf_cache_line_unlock_rd(struct ocf_alock* alock, ocf_cache_line_t line) {
    ocf_alock_unlock_one_rd(alock, line);
}

bool ocf_cache_line_try_lock_wr(struct ocf_alock* alock,
                                ocf_cache_line_t line) {
    return ocf_alock_trylock_entry_wr(alock, line);
}

void ocf_cache_line_unlock_wr(struct ocf_alock* alock,
                              ocf_cache_line_t line) {
    ocf_alock_unlock_one_wr(alock, line);
}

int ocf_req_async_lock_rd(struct ocf_alock* alock,
                          struct ocf_request* req,
                          ocf_req_async_lock_cb cmpl) {
    return ocf_alock_lock_rd(alock, req, cmpl);
}

/**
 * @brief 对请求中的缓存行尝试获取读锁（只进行快速尝试）
 * @param alock - 缓存行锁
 * @param req - 请求
 * @note 该函数只尝试快速获取锁，如果无法立即获取锁，不会尝试等待
 * @return OCF_LOCK_ACQUIRED 如果所有锁都已获取；否则返回 -OCF_ERR_NO_LOCK
 */
int ocf_req_async_lock_rd_fast_only(struct ocf_alock* alock,
                                   struct ocf_request* req) {
    return ocf_alock_lock_rd_fast_only(alock, req) == OCF_LOCK_ACQUIRED? OCF_LOCK_ACQUIRED : -OCF_ERR_NO_LOCK;
}

/**
 * @brief 检查请求中 已映射 的缓存行是否可以获取写锁（只进行快速尝试，并不会真获取锁）
 * @param alock - 缓存行锁
 * @param req - 请求
 * @return true - 可以获取写锁，false - 不能获取写锁
 */
bool ocf_req_async_lock_wr_check_fast(struct ocf_alock* alock,
                            struct ocf_request* req) {
    return ocf_alock_lock_wr_check_fast(alock, req) == OCF_LOCK_ACQUIRED;
}

int ocf_req_async_lock_wr(struct ocf_alock* alock,
                          struct ocf_request* req,
                          ocf_req_async_lock_cb cmpl) {
    return ocf_alock_lock_wr(alock, req, cmpl);
}

void ocf_req_unlock_rd(struct ocf_alock* alock, struct ocf_request* req) {
    int32_t i;
    ocf_cache_line_t entry;

    for (i = 0; i < req->core_line_count; i++) {
        if (!ocf_cl_lock_line_is_acting(alock, req, i))
            continue;

        if (!ocf_alock_is_index_locked(alock, req, i))
            continue;

        entry = ocf_cl_lock_line_get_entry(alock, req, i);

        ocf_alock_unlock_one_rd(alock, entry);
        ocf_alock_mark_index_locked(alock, req, i, false);
    }
}

void ocf_req_unlock_wr(struct ocf_alock* alock, struct ocf_request* req) {
    int32_t i;
    ocf_cache_line_t entry;

    for (i = 0; i < req->core_line_count; i++) {
        if (!ocf_cl_lock_line_is_acting(alock, req, i))
            continue;

        if (!ocf_alock_is_index_locked(alock, req, i))
            continue;

        entry = ocf_cl_lock_line_get_entry(alock, req, i);

        ocf_alock_unlock_one_wr(alock, entry);
        ocf_alock_mark_index_locked(alock, req, i, false);
    }
}

void ocf_req_unlock(struct ocf_alock* alock, struct ocf_request* req) {
    if (req->alock_rw == OCF_WRITE)
        ocf_req_unlock_wr(alock, req);
    else
        ocf_req_unlock_rd(alock, req);
}

bool ocf_cache_line_are_waiters(struct ocf_alock* alock,
                                ocf_cache_line_t line) {
    return !ocf_alock_waitlist_is_empty(alock, line);
}

uint32_t ocf_cache_line_concurrency_suspended_no(struct ocf_alock* alock) {
    return ocf_alock_waitlist_count(alock);
}

#define ALLOCATOR_NAME_FMT "ocf_%s_cl_conc"
#define ALLOCATOR_NAME_MAX (sizeof(ALLOCATOR_NAME_FMT) + OCF_CACHE_NAME_SIZE)

int ocf_cache_line_concurrency_init(struct ocf_alock** self,
                                    unsigned num_clines,
                                    ocf_cache_t cache) {
    char name[ALLOCATOR_NAME_MAX];
    int ret;

    ret = snprintf(name, sizeof(name), ALLOCATOR_NAME_FMT,
                   ocf_cache_get_name(cache));
    if (ret < 0)
        return ret;
    if (ret >= ALLOCATOR_NAME_MAX)
        return -ENOSPC;

    return ocf_alock_init(self, num_clines, name, &ocf_cline_conc_cbs, cache);
}

void ocf_cache_line_concurrency_deinit(struct ocf_alock** self) {
    ocf_alock_deinit(self);
}

size_t ocf_cache_line_concurrency_size_of(ocf_cache_t cache) {
    return ocf_alock_size(cache->device->collision_table_entries);
}
