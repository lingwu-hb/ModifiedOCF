/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef UTILS_HISTORY_HASH_H_
#define UTILS_HISTORY_HASH_H_

#include "../ocf_request.h"

/**
 * @file utils_history_hash.h
 * @brief OCF历史IO哈希表实现
 */

/* 哈希表节点结构 */
struct history_node {
    struct ocf_request* req;
    struct history_node* next;
    uint64_t timestamp;     // 添加时间戳，用于LRU策略
    uint32_t access_count;  // 访问次数，用于统计热点数据
};
typedef struct history_node history_node_t;

/**
 * @brief 初始化历史IO哈希表
 */
void ocf_history_hash_init(void);

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
 * @brief 添加请求到哈希表
 *
 * @param req OCF请求
 */
void ocf_history_hash_add(struct ocf_request* req);

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