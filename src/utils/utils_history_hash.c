/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "utils_history_hash.h"
#include <signal.h>
#include <stdio.h>
#include "../ocf_cache_priv.h"
#include "../ocf_def_priv.h"
#include "../ocf_request.h"

/* 定义哈希表大小 */
#define INITIAL_HASH_SIZE 1024
#define MIN_HASH_SIZE 512
#define MAX_HASH_SIZE 65536
#define HASH_RESIZE_THRESHOLD 0.75  // 哈希表负载因子阈值
#define TIME_THRESHOLD 2            // 设定请求次数阈值
#define INITIAL_MAX_HISTORY 100     // 初始最大历史请求数
#define MIN_MAX_HISTORY 50
#define MAX_MAX_HISTORY 10000

/* 哈希表 */
static history_node_t** history_hash = NULL;
static unsigned int current_hash_size = INITIAL_HASH_SIZE;
static int history_count = 0;
static int max_history = INITIAL_MAX_HISTORY;
static uint64_t current_timestamp = 0;
static uint64_t hit_count = 0;        // 命中次数
static uint64_t miss_count = 0;       // 未命中次数
static uint64_t collision_count = 0;  // 哈希冲突次数
static uint64_t longest_chain = 0;    // 最长链长度

/* 初始化哈希表 */
void ocf_history_hash_init(void) {
    if (history_hash == NULL) {
        history_hash = env_malloc(sizeof(history_node_t*) * current_hash_size, ENV_MEM_NORMAL);
        if (history_hash) {
            memset(history_hash, 0, sizeof(history_node_t*) * current_hash_size);
        }
    }
}

/* 计算哈希值 */
static unsigned int calc_hash(uint64_t addr, int core_id) {
    return (unsigned int)((addr ^ core_id) % current_hash_size);
}

/* 在哈希表中查找请求 */
bool ocf_history_hash_find(uint64_t addr, int core_id) {
    if (!history_hash) {
        ocf_history_hash_init();
        return false;
    }

    unsigned int hash = calc_hash(addr, core_id);
    struct history_node* node = history_hash[hash];
    struct history_node* prev = NULL;
    uint32_t chain_length = 0;

    while (node) {
        chain_length++;
        if (node->req && node->req->ioi.io.addr == addr &&
            ocf_core_get_id(node->req->core) == core_id) {
            // 更新访问信息
            node->access_count++;
            node->timestamp = current_timestamp++;

            // 将热点数据移到链表前端（如果不是第一个节点）
            if (prev) {
                prev->next = node->next;
                node->next = history_hash[hash];
                history_hash[hash] = node;
            }

            hit_count++;
            return true;  // 找到匹配的请求
        }
        prev = node;
        node = node->next;
    }

    // 更新最长链长度统计
    if (chain_length > longest_chain) {
        longest_chain = chain_length;
    }

    if (chain_length > 1) {
        collision_count++;
    }

    miss_count++;
    return false;  // 未找到匹配的请求
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
            unsigned int new_hash_val = (unsigned int)((node->req->ioi.io.addr ^ ocf_core_get_id(node->req->core)) % new_size);

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

/* 清理最不常用的历史记录 */
static void cleanup_lru_history(void) {
    if (history_count <= max_history)
        return;

    // 找到访问时间最早的节点
    history_node_t* lru_node = NULL;
    history_node_t** lru_prev = NULL;
    uint64_t min_timestamp = UINT64_MAX;

    for (unsigned int i = 0; i < current_hash_size; i++) {
        history_node_t** prev = &history_hash[i];
        history_node_t* node = history_hash[i];

        while (node) {
            if (node->timestamp < min_timestamp) {
                min_timestamp = node->timestamp;
                lru_node = node;
                lru_prev = prev;
            }
            prev = &node->next;
            node = node->next;
        }
    }

    // 移除找到的LRU节点
    if (lru_node && lru_prev) {
        *lru_prev = lru_node->next;
        env_free(lru_node);
        history_count--;
    }
}

/* 添加请求到哈希表 */
void ocf_history_hash_add(struct ocf_request* req) {
    if (!history_hash) {
        ocf_history_hash_init();
    }

    unsigned int hash = calc_hash(req->ioi.io.addr, ocf_core_get_id(req->core));
    history_node_t* new_node = env_malloc(sizeof(history_node_t), ENV_MEM_NORMAL);

    if (!new_node)
        return;

    /* 头插法 */
    new_node->req = req;
    new_node->next = history_hash[hash];
    new_node->timestamp = current_timestamp++;
    new_node->access_count = 1;

    history_hash[hash] = new_node;
    history_count++;

    /* 如果超过最大历史数量，清理最不常用的记录 */
    if (history_count > max_history) {
        cleanup_lru_history();
    }

    /* 检查是否需要调整哈希表大小 */
    check_and_resize_hash_table();
}

/* 打印哈希表统计信息 */
void ocf_history_hash_print_stats(void) {
    float hit_ratio = (hit_count + miss_count > 0) ? ((float)hit_count / (hit_count + miss_count) * 100) : 0;
    float load_factor = (float)history_count / current_hash_size;

    printf("[Hash Stats] Size: %u, Count: %d, Max: %d, Load: %.2f%%, Hit Ratio: %.2f%%, Collisions: %llu\n",
           current_hash_size, history_count, max_history,
           load_factor * 100, hit_ratio, collision_count);
}

/* 打印最终统计信息和推荐参数 */
void ocf_history_hash_print_final_stats(void) {
    printf("\n=== Final Hash Table Statistics ===\n");
    printf("Hash Table Size: %u\n", current_hash_size);
    printf("Max History: %d\n", max_history);
    printf("Current History Count: %d\n", history_count);
    printf("Collision Count: %llu\n", collision_count);
    printf("Longest Chain: %llu\n", longest_chain);
    printf("===========================\n\n");

    // 输出最优哈希表参数建议
    printf("=== Recommended Parameters ===\n");
    printf("#define HISTORY_HASH_SIZE %u\n", current_hash_size);
    printf("#define MAX_HISTORY %d\n", max_history);
    printf("=============================\n\n");
}

/* 清理哈希表资源 */
void ocf_history_hash_cleanup(void) {
    // 释放哈希表内存
    if (history_hash) {
        for (unsigned int i = 0; i < current_hash_size; i++) {
            history_node_t* node = history_hash[i];
            while (node) {
                history_node_t* next = node->next;
                env_free(node);
                node = next;
            }
        }
        env_free(history_hash);
        history_hash = NULL;
    }
}

/* 信号处理函数 */
static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        ocf_history_hash_print_final_stats();
    }
}

/* 注册退出处理函数 */
static void __attribute__((constructor)) init_history_hash_module(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    atexit(ocf_history_hash_print_final_stats);
}