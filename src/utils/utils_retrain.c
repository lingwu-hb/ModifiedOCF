/*
 * Copyright(c) 2012-2020 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "utils_retrain.h"
#include "ocf/ocf_das.h"
#include "ocf/ocf_stats.h"
#include "ocf/ocf_core.h"
#include "../engine/engine_debug.h"

#include <math.h>
#include <stdio.h>

// 添加全局变量，存储上一次的缓存命中率
static double previous_hit_ratio = -1.0; // -1表示首次运行，尚未收集数据

// 计算缓存命中率的函数
static double calculate_hit_ratio(struct ocf_stats_requests *reqs)
{
    // 获取总请求数和命中请求数
    uint64_t total_requests = reqs->rd_total.value + reqs->wr_total.value;
    uint64_t hit_requests = reqs->rd_hits.value + reqs->wr_hits.value;
    
    if (total_requests == 0) {
        return 0.0;
    }
    
    return (double)hit_requests / total_requests;
}

// 检查缓存命中率变化并决定是否重训练模型
void check_hit_ratio_and_retrain(ocf_core_t core)
{
    printf(" --- retrain triggered --- \n");
    struct ocf_stats_requests reqs = {0};
    int status;
    
    // 获取统计信息
    status = ocf_stats_collect_core(core, NULL, &reqs, NULL, NULL);
    if (status) {
        OCF_DEBUG_PARAM(cache, "Failed to collect stats for core %s: %d\n", core_name, status);
        return;
    }
    
    // 计算当前缓存命中率
    double current_hit_ratio = calculate_hit_ratio(&reqs);
    
    // 如果不是首次收集数据，则比较两个周期的命中率变化
    if (previous_hit_ratio >= 0) {
        double ratio_change = fabs(current_hit_ratio - previous_hit_ratio);
        
        OCF_DEBUG_PARAM(cache, "OCF hit ratio: previous=%lf, current=%lf, change=%lf\n", 
                previous_hit_ratio, current_hit_ratio, ratio_change);
        
        // 如果命中率变化超过10%，触发模型重训练
        if (ratio_change > 0.1) {
            OCF_DEBUG_PARAM(cache, "Hit ratio change (%lf) exceeded threshold, triggering model retraining\n", ratio_change);
            das_retrain_classifier();
        }
    }
    
    // 保存当前命中率，用于下一次比较
    previous_hit_ratio = current_hit_ratio;
}