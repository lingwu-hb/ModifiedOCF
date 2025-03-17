// src/utils/utils_debug.h
#ifndef __UTILS_DEBUG_H__
#define __UTILS_DEBUG_H__

#include "../ocf_def_priv.h"

#if OCF_DEBUG_ENABLED

// 基础调试信息
#define OCF_DEBUG_LOG(fmt, ...) \
    printf("[Debug] " fmt "\n", ##__VA_ARGS__)

#define OCF_DEBUG_HISTORY(fmt, ...) \
    printf("[Debug] " fmt "\n", ##__VA_ARGS__)

// IO操作相关的调试信息
#define OCF_DEBUG_IO(type, req, ...)                                         \
    printf("[Debug] IO %-18s Address: %14llu, Size: %8uKB" __VA_ARGS__ "\n", \
           type, (req)->ioi.io.addr, (req)->ioi.io.bytes / 1024)

// 命中率统计信息
#define OCF_DEBUG_STATS(hit_pages, total_pages)                             \
    do {                                                                    \
        float hit_ratio = total_pages ? (float)hit_pages / total_pages : 0; \
        printf("[Debug] Request hit ratio: %.2f%% (%llu/%llu 4K blocks)\n", \
               hit_ratio * 100, hit_pages, total_pages);                    \
    } while (0)

// 分隔符
#define OCF_DEBUG_SEPARATOR(counter) \
    printf("\n====== %d ======\n", env_atomic_read(&counter))

#else

#define OCF_DEBUG_LOG(fmt, ...)
#define OCF_DEBUG_HISTORY(fmt, ...)
#define OCF_DEBUG_IO(type, req, ...)
#define OCF_DEBUG_STATS(hit_pages, total_pages)
#define OCF_DEBUG_SEPARATOR(counter)

#endif /* OCF_DEBUG_ENABLED */

#endif /* __UTILS_DEBUG_H__ */