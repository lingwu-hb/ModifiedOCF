* 缓存相关变量

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

* 处理机制

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