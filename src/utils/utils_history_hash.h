/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef UTILS_HISTORY_HASH_H_
#define UTILS_HISTORY_HASH_H_

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

/**
 * @brief 初始化历史IO哈希表
 * 
 * @param ocf_ctx OCF上下文
 * @return int 0表示成功，非0表示失败
 */
int ocf_history_hash_init(struct ocf_ctx* ocf_ctx);

/**
 * @brief 在哈希表中查找请求
 *
 * @param addr 请求地址
 * @param core_id 核心ID
 *
 * @retval true 找到匹配的请求
 * @retval false 未找到匹配的请求
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

#endif /* UTILS_HISTORY_HASH_H_ */