/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef UTILS_HISTORY_HASH_H_
#define UTILS_HISTORY_HASH_H_

#include <stdlib.h>  // 添加标准库头文件，用于 malloc 和 free
#include "../ocf_request.h"

/* 常用位运算宏定义 */
#define PAGE_ALIGN_DOWN(addr) ((addr) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(addr) (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGES_IN_REQ(start, end) (((end) - (start)) / PAGE_SIZE + 1)

/**
 * @file utils_history_hash.h
 * @brief OCF历史IO哈希表实现
 */

/* 哈希表节点结构 */
struct history_node {
    uint64_t addr;                  // 4K 块地址
    uint32_t core_id;               // 核心ID
    struct history_node* next;      // 哈希表链表的下一个节点
    struct history_node* prev_lru;  // LRU链表的前一个节点
    struct history_node* next_lru;  // LRU链表的下一个节点
    uint64_t timestamp;             // 添加时间戳，用于LRU策略
    uint32_t access_count;          // 访问次数，用于统计热点数据
};
typedef struct history_node history_node_t;

// 哈希表数组占用：
// 哈希表大小：1,073,741,824 个指针
// 每个指针占 8 字节
// 总占用：1,073,741,824 × 8 = 8,589,934,592 字节 ≈ 8.59 GB
// 历史节点占用：
// 最大节点数：100,000,000 个节点
// 每个节点占 56 字节（考虑内存对齐）
// 总占用：100,000,000 × 56 = 5,600,000,000 字节 ≈ 5.6 GB

/* 定义哈希表大小 */
// TODO 测一下哈希表充分大的情况，看看效果
// 64MB 的哈希表，1 亿个节点
#define INITIAL_HASH_SIZE 67108864
#define MIN_HASH_SIZE 67108864
#define MAX_HASH_SIZE 67108864
#define HASH_RESIZE_THRESHOLD 0.6
// 30%的4K块命中才算请求命中
#define HISTORY_HIT_RATIO_THRESHOLD 0.3  
// 初始最大历史 4K 块数
#define INITIAL_MAX_HISTORY 100000000
#define MIN_MAX_HISTORY 100000000
#define MAX_MAX_HISTORY 100000000

/* 添加LRU链表头尾节点 */
static history_node_t* lru_head = NULL;  // 最近访问的节点
static history_node_t* lru_tail = NULL;  // 最早访问的节点

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

/* 线程安全锁 */
// static env_spinlock history_lock;

/**
 * @brief 初始化历史IO哈希表
 *
 * @param ocf_ctx OCF上下文
 * @return int 0表示成功，非0表示失败
 */
int ocf_history_hash_init(struct ocf_ctx* ocf_ctx);

/**
 * @brief 在哈希表中查找 4K 块
 *
 * @param addr 4K 块地址
 * @param core_id 核心ID
 *
 * @retval true 找到匹配的 4K 块
 * @retval false 未找到匹配的 4K 块
 */
bool ocf_history_hash_find(uint64_t addr, int core_id);

/**
 * @brief 添加未命中的4K块地址到哈希表
 *
 * @param addr 4K块地址
 * @param core_id 核心ID
 */
void ocf_history_hash_add_addr(uint64_t addr, int core_id);

/**
 * @brief 添加请求到哈希表
 *
 * @param req OCF请求
 */
void ocf_history_hash_add_req(struct ocf_request* req);

/**
 * @brief 打印哈希表统计信息
 */
void ocf_history_hash_print_stats(void);

/**
 * @brief 打印最终统计信息和推荐参数
 */
void ocf_history_hash_print_final_stats(void);

/**
 * @brief 清理哈希表资源
 */
void ocf_history_hash_cleanup(void);

/**
 * 检查缓存是否已满
 * 
 * 通过调用 OCF 缓存监控接口检查缓存使用情况
 * 
 * @param cache OCF缓存实例
 * @return 如果缓存已满返回true，否则返回false
 */
bool ocf_is_cache_full(ocf_cache_t cache);

/**
 * 设置缓存满阈值
 * 
 * @param threshold 新的阈值(0-100)
 */
void ocf_set_cache_full_threshold(uint32_t threshold);

#endif /* UTILS_HISTORY_HASH_H_ */