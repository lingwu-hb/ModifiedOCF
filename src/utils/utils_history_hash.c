/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "utils_history_hash.h"
#include <signal.h>
#include <stdio.h>
#include "../../inc/ocf.h"  // 添加必要的头文件
#include "../ocf_cache_priv.h"
#include "../ocf_def_priv.h"
#include "../ocf_request.h"

/* 初始化哈希表 */
int ocf_history_hash_init(struct ocf_ctx* ocf_ctx) {
    if (history_hash == NULL) {
        history_hash = env_malloc(sizeof(history_node_t*) * current_hash_size, ENV_MEM_NORMAL);
        if (history_hash) {
            memset(history_hash, 0, sizeof(history_node_t*) * current_hash_size);
        }
        env_spinlock_init(&history_lock);

        // 创建内存池
        history_node_pool = env_mpool_create(
            sizeof(history_node_t),  // 节点大小
            0,                       // 不需要额外空间
            ENV_MEM_NORMAL,          // 内存类型
            1,                       // 最大大小
            false,                   // 不需要零初始化
            NULL,                    // 没有限制
            "history_node",          // 池名称
            true                     // 允许扩展
        );

        if (!history_node_pool) {
            env_free(history_hash);
            history_hash = NULL;
            return -OCF_ERR_NO_MEM;
        }
    }
    return 0;
}

/* 计算哈希值 */
static unsigned int calc_hash(uint64_t addr, int core_id) {
    // 将地址按4K对齐
    uint64_t aligned_addr = PAGE_ALIGN_DOWN(addr);
    return (unsigned int)((aligned_addr ^ core_id) % current_hash_size);
}

/* 将节点添加到LRU链表头部 */
static void add_to_lru_head(history_node_t* node) {
    if (!node)
        return;

    // 从当前位置断开连接
    if (node->prev_lru)
        node->prev_lru->next_lru = node->next_lru;

    if (node->next_lru)
        node->next_lru->prev_lru = node->prev_lru;

    // 如果节点是尾节点，更新尾指针
    if (lru_tail == node)
        lru_tail = node->prev_lru;

    // 添加到头部
    node->prev_lru = NULL;
    node->next_lru = lru_head;

    if (lru_head)
        lru_head->prev_lru = node;

    lru_head = node;

    // 如果尾节点为空，这是唯一的节点
    if (!lru_tail)
        lru_tail = node;
}

/* 从LRU链表中移除节点 */
static void remove_from_lru(history_node_t* node) {
    if (!node)
        return;

    // 更新前后节点的链接
    if (node->prev_lru)
        node->prev_lru->next_lru = node->next_lru;
    else
        lru_head = node->next_lru;  // 节点是头节点

    if (node->next_lru)
        node->next_lru->prev_lru = node->prev_lru;
    else
        lru_tail = node->prev_lru;  // 节点是尾节点
}

/* 在哈希表中查找 4K 块 */
bool ocf_history_hash_find(uint64_t addr, int core_id) {
    if (!history_hash) {
        ocf_history_hash_init(NULL);
        return false;
    }

    // 将地址按4K对齐
    uint64_t aligned_addr = PAGE_ALIGN_DOWN(addr);
    bool found = false;

    env_spinlock_lock(&history_lock);

    unsigned int hash = calc_hash(aligned_addr, core_id);
    history_node_t* node = history_hash[hash];
    history_node_t* prev = NULL;
    uint32_t chain_length = 0;

    while (node) {
        chain_length++;
        // 检查4K对齐的地址和核心ID是否匹配
        if (node->addr == aligned_addr && node->core_id == core_id) {
            // 更新访问信息
            node->access_count++;
            node->timestamp = current_timestamp++;

            // 将节点移动到LRU链表头部
            add_to_lru_head(node);

            // 将热点数据移到链表前端（哈希链表）
            if (prev) {
                prev->next = node->next;
                node->next = history_hash[hash];
                history_hash[hash] = node;
            }

            hit_count++;
            found = true;
            break;
        }
        prev = node;
        node = node->next;
    }

    // 更新统计信息
    if (chain_length > longest_chain) {
        longest_chain = chain_length;
    }
    if (chain_length > 1) {
        collision_count++;
    }
    if (!found) {
        miss_count++;
    }

    env_spinlock_unlock(&history_lock);
    return found;
}

/* 重新调整哈希表大小 */
static void resize_hash_table(unsigned int new_size) {
    history_node_t** new_hash = env_malloc(sizeof(history_node_t*) * new_size, ENV_MEM_NORMAL);
    if (!new_hash)
        return;

    memset(new_hash, 0, sizeof(history_node_t*) * new_size);

    // 将旧哈希表中的节点重新分配到新哈希表
    for (unsigned int i = 0; i < current_hash_size; i++) {
        history_node_t* node = history_hash[i];
        while (node) {
            history_node_t* next = node->next;
            unsigned int new_hash_val = (unsigned int)((node->addr ^ node->core_id) % new_size);

            // 插入到新哈希表
            node->next = new_hash[new_hash_val];
            new_hash[new_hash_val] = node;

            node = next;
        }
    }

    // 释放旧哈希表并更新
    env_free(history_hash);
    history_hash = new_hash;
    current_hash_size = new_size;

    printf("[Hash Resize] New hash size: %u, History count: %d, Max history: %d\n",
           current_hash_size, history_count, max_history);
}

/* 检查是否需要调整哈希表大小 */
static void check_and_resize_hash_table(void) {
    float load_factor = (float)history_count / current_hash_size;

    // 负载因子过高，扩大哈希表
    if (load_factor > HASH_RESIZE_THRESHOLD && current_hash_size < MAX_HASH_SIZE) {
        unsigned int new_size = current_hash_size * 2;
        if (new_size > MAX_HASH_SIZE)
            new_size = MAX_HASH_SIZE;

        resize_hash_table(new_size);
    }
    // 负载因子过低，缩小哈希表
    else if (load_factor < (HASH_RESIZE_THRESHOLD / 2) && current_hash_size > MIN_HASH_SIZE &&
             history_count > 0) {
        unsigned int new_size = current_hash_size / 2;
        if (new_size < MIN_HASH_SIZE)
            new_size = MIN_HASH_SIZE;

        resize_hash_table(new_size);
    }

    // 根据命中率调整最大历史数
    if (hit_count + miss_count > 1000) {
        float hit_ratio = (float)hit_count / (hit_count + miss_count);

        // 命中率低，增加历史记录容量
        if (hit_ratio < 0.3 && max_history < MAX_MAX_HISTORY) {
            max_history = max_history * 2;
            if (max_history > MAX_MAX_HISTORY)
                max_history = MAX_MAX_HISTORY;
            printf("[History Adjust] Increasing max history to %d (hit ratio: %.2f%%)\n",
                   max_history, hit_ratio * 100);
        }
        // 命中率高，可以减少历史记录容量
        else if (hit_ratio > 0.7 && max_history > MIN_MAX_HISTORY && history_count < max_history / 2) {
            max_history = max_history / 2;
            if (max_history < MIN_MAX_HISTORY)
                max_history = MIN_MAX_HISTORY;
            printf("[History Adjust] Decreasing max history to %d (hit ratio: %.2f%%)\n",
                   max_history, hit_ratio * 100);
        }

        // 重置统计
        hit_count = 0;
        miss_count = 0;
    }
}

/* 清理最不常用的历史记录 - 优化版 */
static void cleanup_lru_history(void) {
    if (history_count <= max_history || !lru_tail)
        return;

    // 直接获取LRU链表中的尾节点
    history_node_t* lru_node = lru_tail;

    // 从LRU链表中移除
    remove_from_lru(lru_node);

    // 从哈希表中移除
    unsigned int hash = calc_hash(lru_node->addr, lru_node->core_id);
    history_node_t* node = history_hash[hash];
    history_node_t* prev = NULL;

    while (node) {
        if (node == lru_node) {
            // 处理冲突链表的前后节点连接
            if (prev) {
                // 如果不是链表头节点,更新前一个节点的next指针
                prev->next = node->next;
            } else {
                // 如果是链表头节点,更新哈希表槽位指针
                history_hash[hash] = node->next;
            }

            // 使用内存池释放节点
            env_mpool_del(history_node_pool, node, 1);
            history_count--;
            break;
        }
        prev = node;
        node = node->next;
    }
}

/* 添加未命中的 4K 块到哈希表 */
void ocf_history_hash_add_addr(uint64_t addr, int core_id) {
    if (!history_hash) {
        ocf_history_hash_init(NULL);
    }

    // 将地址按 4K 对齐
    uint64_t aligned_addr = PAGE_ALIGN_DOWN(addr);

    env_spinlock_lock(&history_lock);

    // 检查是否已存在
    unsigned int hash = calc_hash(aligned_addr, core_id);
    history_node_t* node = history_hash[hash];

    while (node) {
        if (node->addr == aligned_addr && node->core_id == core_id) {
            // 已存在，更新访问信息
            node->access_count++;
            node->timestamp = current_timestamp++;
            add_to_lru_head(node);
            env_spinlock_unlock(&history_lock);
            return;
        }
        node = node->next;
    }

    // 从内存池分配新节点
    history_node_t* new_node = env_mpool_new(history_node_pool, 1);
    if (!new_node) {
        OCF_DEBUG_LOG("Failed to allocate memory for new history node");
        env_spinlock_unlock(&history_lock);
        return;
    }

    /* 初始化新节点 */
    new_node->addr = aligned_addr;
    new_node->core_id = core_id;
    new_node->next = history_hash[hash];
    new_node->timestamp = current_timestamp++;
    new_node->access_count = 1;
    new_node->prev_lru = NULL;
    new_node->next_lru = NULL;

    /* 添加到哈希表 */
    history_hash[hash] = new_node;

    /* 添加到LRU链表头部 */
    add_to_lru_head(new_node);

    history_count++;

    /* 如果超过最大历史数量，清理最不常用的记录 */
    if (history_count > max_history) {
        cleanup_lru_history();
    }

    /* 检查是否需要调整哈希表大小 */
    check_and_resize_hash_table();

    env_spinlock_unlock(&history_lock);
}

/* 添加请求到哈希表 */
// TODO：这个函数可以废弃了
void ocf_history_hash_add_req(struct ocf_request* req) {
    if (!req || !history_hash) {
        if (!history_hash)
            ocf_history_hash_init(NULL);
        return;
    }

    uint64_t start_addr = PAGE_ALIGN_DOWN(req->ioi.io.addr);
    uint64_t end_addr = PAGE_ALIGN_DOWN(req->ioi.io.addr + req->ioi.io.bytes - 1);

    // 遍历每个4K块，检查是否在历史记录中，只添加未命中的块
    for (uint64_t curr_addr = start_addr; curr_addr <= end_addr; curr_addr += PAGE_SIZE) {
        // 判断是否已存在于哈希表中
        if (!ocf_history_hash_find(curr_addr, ocf_core_get_id(req->core))) {
            // 添加未命中的块
            ocf_history_hash_add_addr(curr_addr, ocf_core_get_id(req->core));
        }
    }
}

/* 打印哈希表统计信息 */
void ocf_history_hash_print_stats(void) {
    float hit_ratio = (hit_count + miss_count > 0) ? ((float)hit_count / (hit_count + miss_count) * 100) : 0;
    float load_factor = (float)history_count / current_hash_size;

    printf("[Hash Stats] Size: %u, Count: %d, Max: %d, Load: %.2f%%, Hit Ratio: %.2f%%, Collisions: %llu\n",
           current_hash_size, history_count, max_history,
           load_factor * 100, hit_ratio, collision_count);
}

/* 清理哈希表资源 */
void ocf_history_hash_cleanup(void) {
    env_spinlock_lock(&history_lock);

    // 释放哈希表内存
    if (history_hash) {
        for (unsigned int i = 0; i < current_hash_size; i++) {
            history_node_t* node = history_hash[i];
            while (node) {
                history_node_t* next = node->next;
                // 使用内存池释放节点
                env_mpool_del(history_node_pool, node, 1);
                node = next;
            }
        }
        env_free(history_hash);
        history_hash = NULL;
    }

    // 重置LRU链表指针
    lru_head = NULL;
    lru_tail = NULL;

    // 销毁内存池
    if (history_node_pool) {
        env_mpool_destroy(history_node_pool);
        history_node_pool = NULL;
    }

    env_spinlock_unlock(&history_lock);
}