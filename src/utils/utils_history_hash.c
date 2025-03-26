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
#include "utils_debug.h"

/* 初始化哈希表 */
int ocf_history_hash_init(struct ocf_ctx* ocf_ctx) {
    if (history_hash == NULL) {
        history_hash = env_malloc(sizeof(history_node_t*) * current_hash_size, ENV_MEM_NORMAL);
        if (history_hash) {
            memset(history_hash, 0, sizeof(history_node_t*) * current_hash_size);
        }

        // 移除内存池的创建和初始化
        return history_hash ? 0 : -OCF_ERR_NO_MEM;
    }
    return 0;
}

/* 计算哈希值 */
static unsigned int calc_hash(uint64_t addr, int core_id) {
    // 使用更好的哈希算法 - MurmurHash3
    // 该算法具有更好的分布特性，可以减少哈希冲突
    uint64_t aligned_addr = PAGE_ALIGN_DOWN(addr);
    uint64_t h = aligned_addr;
    
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    
    // 加入 core_id 作为哈希因子
    h = h ^ ((uint64_t)core_id << 32 | core_id);
    // 使用位运算取模
    return (unsigned int)(h & (current_hash_size - 1));
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

    // env_spinlock_lock(&history_lock);  // 注释掉锁

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

    // env_spinlock_unlock(&history_lock);  // 注释掉锁
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

    OCF_DEBUG_HISTORY("[Hash Resize] New hash size: %u, History count: %d, Max history: %d\n",
                      current_hash_size, history_count, max_history)
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
            OCF_DEBUG_HISTORY("[History Adjust] Increasing max history to %d (hit ratio: %.2f%%)\n",
                              max_history, hit_ratio * 100);
        }
        // 命中率高，可以减少历史记录容量
        else if (hit_ratio > 0.7 && max_history > MIN_MAX_HISTORY && history_count < max_history / 2) {
            max_history = max_history / 2;
            if (max_history < MIN_MAX_HISTORY)
                max_history = MIN_MAX_HISTORY;
            OCF_DEBUG_HISTORY("[History Adjust] Decreasing max history to %d (hit ratio: %.2f%%)\n",
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

            // 使用直接释放替代内存池释放
            free(node);
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

    // env_spinlock_lock(&history_lock);  // 注释掉锁

    // 检查是否已存在
    unsigned int hash = calc_hash(aligned_addr, core_id);
    history_node_t* node = history_hash[hash];

    int depth = 0;
    while (node) {
        depth++;
        if (node->addr == aligned_addr && node->core_id == core_id) {
            // 已存在，更新访问信息
            node->access_count++;
            node->timestamp = current_timestamp++;
            add_to_lru_head(node);
            // env_spinlock_unlock(&history_lock);  // 注释掉锁
            return;
        }
        node = node->next;
    }
    printf("Depth: %d\n", depth);

    // 使用malloc直接分配内存，替代内存池分配
    history_node_t* new_node = (history_node_t*)malloc(sizeof(history_node_t));
    if (!new_node) {
        OCF_DEBUG_HISTORY("Failed to allocate memory for new history node");
        // env_spinlock_unlock(&history_lock);  // 注释掉锁
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

    // env_spinlock_unlock(&history_lock);  // 注释掉锁
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

    OCF_DEBUG_HISTORY("[Hash Stats] Size: %u, Count: %d, Max: %d, Load: %.2f%%, Hit Ratio: %.2f%%, Collisions: %llu\n",
                      current_hash_size, history_count, max_history,
                      load_factor * 100, hit_ratio, collision_count);
}

/* 打印最终统计信息和推荐参数 */
void ocf_history_hash_print_final_stats(void) {
    // 打印哈希表统计信息
    ocf_history_hash_print_stats();
    
    // 打印最佳配置建议
    float hit_ratio = (hit_count + miss_count > 0) ? ((float)hit_count / (hit_count + miss_count)) : 0;
    OCF_DEBUG_HISTORY("[Final Stats] Total: %llu, Hit: %llu, Miss: %llu, Ratio: %.2f%%\n",
                      hit_count + miss_count, hit_count, miss_count, hit_ratio * 100);
}

/**
 * @brief 执行二次准入检查
 * 
 * 检查请求中所有4K块在历史记录中的命中情况，并根据命中率决定是否允许缓存。
 * 如果命中率低于阈值，则将请求的所有4K块添加到历史记录中，并返回false表示不应缓存。
 * 
 * @param req OCF请求对象
 * @return bool true表示通过二次准入检查，应该被缓存；false表示未通过，应直接PT
 */
bool ocf_history_check_second_chance(struct ocf_request* req) {
    if (!history_hash) {
        ocf_history_hash_init(NULL);
    }

    /* 计算页面对齐的地址和总页数 */
    uint64_t start_addr = PAGE_ALIGN_DOWN(req->ioi.io.addr);
    uint64_t end_addr = PAGE_ALIGN_DOWN(req->ioi.io.addr + req->ioi.io.bytes - 1);
    uint64_t total_pages = PAGES_IN_REQ(start_addr, end_addr);
    uint64_t hit_pages = 0;
    int core_id = ocf_core_get_id(req->core);

    /* 检查历史记录中的命中情况 */
    for (uint64_t curr_addr = start_addr; curr_addr <= end_addr; curr_addr += PAGE_SIZE) {
        if (ocf_history_hash_find(curr_addr, core_id)) {
            hit_pages++;
        }
    }
    
    /* 如果历史命中率低于阈值，添加4K块到历史记录并返回false */
    if ((float)hit_pages / total_pages < HISTORY_HIT_RATIO_THRESHOLD) {
        OCF_DEBUG_IO("PT, History miss", req);

        // 将当前请求涉及到的所有4K块都尝试添加到历史记录中
        // 因为考虑到要更新LRU链表，所有需要都进行尝试添加
        for (uint64_t curr_addr = start_addr; curr_addr <= end_addr; curr_addr += PAGE_SIZE) {
            ocf_history_hash_add_addr(curr_addr, core_id);
        }

        return false; // 未通过二次准入检查
    }

    return true; // 通过二次准入检查
}

/* 清理哈希表资源 */
void ocf_history_hash_cleanup(void) {
    // env_spinlock_lock(&history_lock);

    // 释放哈希表内存
    if (history_hash) {
        for (unsigned int i = 0; i < current_hash_size; i++) {
            history_node_t* node = history_hash[i];
            while (node) {
                history_node_t* next = node->next;
                // 使用直接释放替代内存池释放
                free(node);
                node = next;
            }
        }
        env_free(history_hash);
        history_hash = NULL;
    }

    // 重置LRU链表指针
    lru_head = NULL;
    lru_tail = NULL;

    // 不再需要销毁内存池
    // env_spinlock_unlock(&history_lock);
}