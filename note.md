# IO 流程

## ocf_core_volume_submit_io 

OCF 处理 IO 请求时，如果 cache 中能够命中，会走 `ocf_core_submit_io_fast` 直接读取 cache，然后返回。

如果 cache 读取失败，会走下面流程，将请求放入到 req->io_queue 队列中，后续定时对该队列进行处理。

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

最后 pop 出来之后，会执行 `req->io_if->read(req)` 函数接口进行处理。

不同的缓存模式，对应的接口函数 `req->io_if` 不同。WT 模式下，执行 `ocf_read_generic`，PT 模式下，执行 `ocf_read_pt` 函数。

## read_generic 



## read_pt 

首先检查一下当前请求在缓存中对应的情况

如果当前请求的数据在缓存中存在映射，需要获取读锁。



## 缓存行锁

缓存行锁针对的粒度为**缓存行**，并且使用了分级获取的方式

首先尝试快速获取锁，如果快速获取锁失败，再慢速获取锁。

快速获取是同步进行获取，不会阻塞，一旦有一个缓存行获取失败，直接退出。

慢速获取会尝试获取每一个锁，如果获取不到，将会考虑将其放置到等待队列，直到成功获取锁才执行回调函数。（代码片段如下）

```c
int ocf_alock_lock_rd(struct ocf_alock *alock,
		struct ocf_request *req, ocf_req_async_lock_cb cmpl)
{
	// 尝试快速获取锁
	lock = alock->cbs->lock_entries_fast(alock, req, OCF_READ);
	if (lock != OCF_LOCK_ACQUIRED) {
		// 快速获取失败，执行慢速获取
		status = alock->cbs->lock_entries_slow(alock, req, OCF_READ, cmpl);
	}
	return lock;
}
```

## 快速和慢速获取缓存行锁

需要注意以下两个变量的联系

* `req->alock_status`：uint8 类型，其数位保存当前请求需要的缓存行的锁状态。
* `req->cache->device.concurrency->cache_line->access`：原子类型变量，用于监控系统中所有缓存行锁的状态。

`req->cache->device->concurrency.cache_line` 就是 ocf_alock 类型的变量。后续的 alock 就是指代该全局变量

在 OCF (Open CAS Framework) 中，缓存行锁定机制涉及两个关键字段：`alock->access` 和 `req->alock_status`。这两个字段虽然都与缓存行锁定有关，但它们在系统中扮演着不同的角色。

### alock->access 字段

#### 定义与结构

alock->access 是一个原子变量数组，每个缓存行对应一个原子变量，用于表示缓存行的实际锁定状态。

```c
struct ocf_alock {
  env_atomic *access; *// 每个缓存行的锁状态*
  *// 其他字段...*
};
```

#### 功能

全局锁状态管理：记录**系统**中每个缓存行的实际锁定状态

原子操作：通过原子操作确保并发安全

锁状态值：

* `OCF_CACHE_LINE_ACCESS_IDLE (0)`：缓存行空闲

* `OCF_CACHE_LINE_ACCESS_ONE_RD (1)`：一个读锁

* \`>1`：多个读锁

* `OCF_CACHE_LINE_ACCESS_WR (INT_MAX)`：写锁

#### 使用方式

如在 `ocf_alock_trylock_entry_rd_idle` 函数中：

```c
env_atomic *access = &alock->access[entry];
int prev = env_atomic_cmpxchg(access, OCF_CACHE_LINE_ACCESS_IDLE, OCF_CACHE_LINE_ACCESS_ONE_RD);
```

这里使用原子比较交换操作尝试将缓存行从空闲状态更改为一个读锁状态。

### req->alock_status 字段

#### 定义与结构

`req->alock_status` 是一个请求特定的字段，是一个指向 uint8_t 数组的指针，用于跟踪**请求**中每个缓存行的锁定状态。

```c
struct ocf_request {
  *// 其他字段...*
  uint8_t* alock_status; *// 请求中每个缓存行的锁定状态*
  *// 其他字段...*
};
```

#### 功能

1. **请求级别跟踪**：记录特定请求中哪些缓存行已被锁定

2. 锁定标记：通常是简单的布尔值（0表示未锁定，非0表示已锁定），**无法知道锁的类型**

3. 请求完成时解锁：帮助在请求完成时知道哪些缓存行需要解锁

#### 使用方式

通过 `ocf_alock_mark_index_locked` 函数设置：

```c
void ocf_alock_mark_index_locked(struct ocf_alock *alock,
		struct ocf_request *req, unsigned index, bool locked)
{ // locked == true: 进行锁定; locked == false: 进行解锁
	if (locked) 
		env_bit_set(index, req->alock_status);
	else
		env_bit_clear(index, req->alock_status);
}
```

### 两者的区别

作用域不同：

`alock->access`：全局作用域，表示缓存系统中每个缓存行的实际锁状态

`req->alock_status`：请求作用域，仅跟踪特定请求中缓存行的锁定状态

数据类型不同：

`alock->access`：原子变量数组，支持复杂的原子操作

`req->alock_status`：简单的 uint8_t 数组，通常只存储布尔值

用途不同：

`alock->access`：实际执行锁定/解锁操作

`req->alock_status`：跟踪请求中哪些缓存行已被锁定，便于请求完成时解锁

### 两者的联系

协同工作：

当请求尝试锁定缓存行时，首先通过 `alock->access` 获取实际锁

锁定成功后，通过 `req->alock_status` 标记该请求已锁定此缓存行

锁定/解锁流程：

锁定流程：

1. 尝试通过 `alock->access` 获取锁

2. 成功后，设置 `req->alock_status` 中对应数位为 1

解锁流程：

1. 遍历 `req->alock_status` 找出已锁定的缓存行

2. 通过 `alock->access` 解锁这些缓存行

3. 设置 `req->alock_status` 中对应数位为 0

请求完成时的解锁：

```c
void ocf_req_unlock(struct ocf_alock *c, struct ocf_request *req) {
    // 根据 req->alock_rw 和 req->alock_status 决定如何解锁
    if (req->alock_rw == OCF_READ)
      ocf_req_unlock_rd(c, req);
    else if (req->alock_rw == OCF_WRITE)
      ocf_req_unlock_wr(c, req);
}
```



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