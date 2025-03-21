# IO 流程



```c++
ocf_core_volume_submit_io
    -> ocf_engine_hndl_req
    	-> ocf_engine_push_req_back // 将 req 放入到 req->io_queue 中
```

```c++
// 不断对 queue 中的请求进行处理
void ocf_queue_run(ocf_queue_t q)
{
	unsigned char step = 0;

	OCF_CHECK_NULL(q);

	while (env_atomic_read(&q->io_no) > 0) {
        // 从 q 中拿出一个 req，出队然后执行 req->io_if->read 进行读取
		ocf_queue_run_single(q); 
        // 每隔 128 个请求让出 CPU，避免长时间占用
		OCF_COND_RESCHED(step, 128);
	}
}
```

## read_generic 流程



## read_pt 流程



TODO：分析这两者关于锁机制的一些细节



# 锁机制分析

首先尝试快速获取锁，如果快速获取锁失败，再慢速获取锁。

```c
int ocf_alock_lock_rd(struct ocf_alock *alock,
		struct ocf_request *req, ocf_req_async_lock_cb cmpl)
{
	int lock, status;

	ENV_BUG_ON(env_atomic_read(&req->lock_remaining));
	req->alock_rw = OCF_READ;

	lock = alock->cbs->lock_entries_fast(alock, req, OCF_READ);

	if (lock != OCF_LOCK_ACQUIRED) {
		env_mutex_lock(&alock->lock);

		ENV_BUG_ON(env_atomic_read(&req->lock_remaining));
		ENV_BUG_ON(!cmpl);

		env_atomic_inc(&alock->waiting);
		env_atomic_set(&req->lock_remaining, req->core_line_count);
		env_atomic_inc(&req->lock_remaining);

		status = alock->cbs->lock_entries_slow(alock, req, OCF_READ, cmpl);
		if (!status) {
			if (env_atomic_dec_return(&req->lock_remaining) == 0) {
				lock = OCF_LOCK_ACQUIRED;
				env_atomic_dec(&alock->waiting);
			}
		} else {
			env_atomic_set(&req->lock_remaining, 0);
			env_atomic_dec(&alock->waiting);
			lock = status;
		}
		env_mutex_unlock(&alock->lock);
	}

	return lock;
}
```

TODO：快速获取锁 和 慢速获取锁 流程上的区别？相关变量的具体含义？





# OCF 内部哈希表实现

有时间可以研究一下其是如何设计缓存哈希表，以及其是如何使用锁进行并发管理的。

```c
ocf_cache_line_t ocf_metadata_get_hash(struct ocf_cache *cache,
		ocf_cache_line_t index)
{
	struct ocf_metadata_ctrl *ctrl
		= (struct ocf_metadata_ctrl *) cache->metadata.priv;

	return *(ocf_cache_line_t *)ocf_metadata_raw_rd_access(cache,
			&(ctrl->raw_desc[metadata_segment_hash]), index);
}
```







# 缓存相关变量



```c
struct ocf_req_info {
    /* 请求的统计计数器 */
    unsigned int hit_no;      // 缓存命中次数
    unsigned int invalid_no;  // 无效缓存行数量
    unsigned int re_part_no;  // 需要重新分区的数量
    unsigned int seq_no;      // 顺序访问的数量
    unsigned int insert_no;   // 插入操作的数量

    /* 脏数据相关标志 */
    uint32_t dirty_all;      // 请求中脏缓存行的数量
    uint32_t dirty_any;      // 标记是否有任何脏数据

    /* 状态标志位 */
    // flush_metadata: 1 - 元数据需要刷新的情况
    uint32_t flush_metadata : 1;    
    /* 触发条件:
     * - 写操作完成后需要更新元数据
     * - 分区调整后需要持久化新的映射关系
     * - 系统关闭前需要保存元数据状态
     */

    // mapping_error: 1 - 映射错误标志
    uint32_t mapping_error : 1;     
    /* 触发条件:
     * - hash表冲突无法解决
     * - 内存分配失败
     * - 映射表损坏
     */

    // cleaning_required: 1 - 需要清理的情况
    uint32_t cleaning_required : 1;  
    /* 触发条件:
     * - 分区空间不足需要驱逐数据
     * - 脏数据过多需要回写
     * - 系统触发周期性清理
     */

    // core_error: 1 - core设备IO错误
    uint32_t core_error : 1;        
    /* 触发条件:
     * - 读写core设备失败
     * - 设备离线
     * - 设备超时
     */

    // cleaner_cache_line_lock: 1 - 清理器锁
    uint32_t cleaner_cache_line_lock : 1;  
    /* 触发条件:
     * - 清理进程需要锁定cache line
     * - 防止清理过程中的并发访问
     */

    // internal: 1 - 内部请求标志
    uint32_t internal : 1;          
    /* 触发条件:
     * - 系统内部维护操作
     * - 元数据更新
     * - 清理操作
     */



};
```

## 处理机制

```c
static void ocf_engine_process_request(struct ocf_request *req) {
    // 检查是否有映射错误
    if (req->info.mapping_error) {
        // 转为直通模式处理
        ocf_get_io_if(ocf_cache_mode_pt)->read(req);
        return;
    }

    // 检查是否需要清理
    if (req->info.cleaning_required) {
        // 触发清理流程
        ocf_engine_clean(req);
        return;
    }

    // 检查是否有core错误
    if (req->info.core_error) {
        // 错误处理
        req->complete(req, req->error);
        return;
    }

    // 检查是否需要刷新元数据
    if (req->info.flush_metadata) {
        // 刷新元数据
        ocf_metadata_flush(req->cache);
    }
}
```