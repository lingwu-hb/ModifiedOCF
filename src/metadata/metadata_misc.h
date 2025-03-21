/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __METADATA_MISC_H__
#define __METADATA_MISC_H__

/* Hash function intentionally returns consecutive (modulo @hash_table_entries)
 * values for consecutive @core_line_num. This way it is trivial to sort all
 * core lines within a single request in ascending hash value order. This kind
 * of sorting is required to assure that (future) hash bucket metadata locks are
 * always acquired in fixed order, eliminating the risk of dead locks.
 */
/* req->map[i].hash 表示当前请求的第 i 个 4K 块在 cache 中的位置
 * 给连续的地址块赋连续的哈希值，这样后续获取锁的时候也是连续获取的，不会形成死锁
 */
static inline ocf_cache_line_t ocf_metadata_hash_func(ocf_cache_t cache,
                                                      uint64_t core_line_num,
                                                      ocf_core_id_t core_id) {
    const unsigned int entries = cache->device->hash_table_entries;

    /* TODO：entries 指 hash_table_entries，即 hash 表的桶数
     *       这里 hash 表的桶数具体指多少？
     */
    return (ocf_cache_line_t)((core_line_num + (core_id * (entries / 32))) % entries);
}

void ocf_metadata_remove_cache_line(struct ocf_cache* cache,
                                    ocf_cache_line_t cache_line);

void ocf_metadata_sparse_cache_line(struct ocf_cache* cache,
                                    ocf_cache_line_t cache_line);

int ocf_metadata_sparse_range(struct ocf_cache* cache, int core_id, uint64_t start_byte, uint64_t end_byte);

#endif /* __METADATA_MISC_H__ */
