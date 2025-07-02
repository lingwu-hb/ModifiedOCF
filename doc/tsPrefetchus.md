# tsPrefetchus Implementation

## TODO List

### 1. 多臂老虎机代码提取
1. 提取多臂老虎机的代码作为一个独立的可编译的文件夹，放在SPDK根目录下
  1. 清理掉模拟器，抽取多臂赌博机核心算法为PrefetchUS，对外抽象成五个接口：
    das_prefetchus_init
    das_prefetchus_predict
    das_prefetchus_cleanup
    das_prefetchus_find,
    das_prefetchus_evict 
  2. 原项目混合使用C，C++， 且预取算法和模拟器耦合在一起（直接调用模拟的Cache参数），需要修改代码，解耦依赖，先让剥离后的代码能够重新编译出 libprefetchus.so
  3. 让原Cache模拟器成为影子数据结构，只维护元数据，不真正存储Cache（参数多，这个比较繁琐）
  4. 修正算法配置，与我们的SSD介质的Cache相匹配
    1. Cache Size = 4k
    2. ...
2. 把libPrefetchus.so装进SPDK，应该要修改SPDK的makefile，跑通编译
3. 在ocf_core_volume_submit_io 调用，测试返回的预取IO建议是否合理
4. 跑测试，看一下会不会遇到多线程死锁。

## 问题
### 接口函数
1. 多臂老虎机在缓存命中和缓存驱逐这两种情况下，都会对其权重进行动态更新。但是我们需要调用 ocf_engine_traverse(req)，才能知道缓存是否命中，涉及到锁的问题，需要整理代码逻辑。同时对于缓存驱逐的情况也要进行后续调用。
2. 缓存驱逐相关代码：ocf_engine_prepare_clines -> ocf_prepare_clines_miss -> ocf_engine_remap -> [等等复杂的驱逐逻辑]，最终会到达 ocf_lru_req_clines函数，在此处执行更新处理。缓存驱逐需要知道被驱逐的数据块是什么情况被准入缓存，因此需要增加多种元数据。包括但不限于，prefetch_flag（是否是通过预取机制加入缓存的。），trigger_block（触发预取的原始对象ID）。

暴露出三个接口：ts_handle_prefetch, ts_handle_find, ts_handle_evict
1. ts_handle_prefetch
在ocf_core_volume_submit_io 调用，测试返回的预取IO建议
2. ts_handle_find
ocf_engine_traverse() 等涉及到查询请求是否命中缓存的情况都可以调用，用缓存命中情况对预取器的参数进行在线调整
3. ts_handle_evict
（可选）当 ocf 涉及到缓存驱逐时，考虑增加被驱逐块元数据信息，然后根据被驱逐情况在线更新预取器参数。

### 模块化
模块化问题，新的模块以什么方法，放置在哪里？ ----> 涉及到编译和结构化相关问题
考虑在 spdk/ocf/src 目录下新增 prefetch 文件夹，所需要 c 文件和对应的头文件都放置于此。
1. 直接添加代码即可，理论上不需要修改 makefile文件
2. 可能需要在 module/bdev/ocf 下增加部分与预取器相关的初始化工作

### tsPrefetcher
这部分主要包括以下内容
1. 预取器本身工作所需要的元数据和数据结构
2. 与 ocf 相比，需要增加的元数据
```c
typedef struct tsPrefetchus_init_params {
  char sequential_prefetcher_name[64];
  char history_prefetcher_name[64];
  int32_t block_size;
  int history_min_threshold_metadata_size;
  int history_max_threshold_metadata_size;
  int merge_params_range;
  uint64_t lr_update_interval;
} tsPrefetchus_init_params_t;

typedef struct tsPrefetchus_params {
  prefetcher_t* sequential_prefetcher;
  prefetcher_t* history_prefetcher;

  int merge_params_range;

  std::unordered_map<uint64_t, double> w_sequential_prefetcher;  // Weight for sequential prefetcher
  std::unordered_map<uint64_t, double> w_history_prefetcher;     // Weight for history prefetcher
  double lr;                                                     // learning rate
  double lr_previous;                                            // previous learning rate
  uint64_t lr_update_interval;                                   // lr update interval

  int64_t num_hit;
  double hit_rate_prev;
  uint8_t unlearn_count;
  uint8_t no_use_count;
  int32_t block_size;
} tsPrefetchus_params_t;
```

### tsPrefetchus_prefetch

```c
void tsPrefetchus_prefetch(cache_t *cache, const request_t *req) {
  // 初始化两种专家的权重，每个请求都有单独的权重值
  if (!tsPrefetchus_params->w_history_prefetcher.count(req->obj_id)) {
    tsPrefetchus_params->w_history_prefetcher[req->obj_id] = 1;
  }
  uint64_t merge_place = (uint64_t)req->obj_id / tsPrefetchus_params->merge_params_range;
  if (!tsPrefetchus_params->w_sequential_prefetcher.count(merge_place)) {
    tsPrefetchus_params->w_sequential_prefetcher[merge_place] = 1;
  }
  
  request_t *new_req = my_malloc(request_t);
  new_req->obj_size = tsPrefetchus_params->block_size;
  new_req->trigger_block = req->obj_id;

  r = ((double)(next_rand() % 100)) / 100.0;
  if (r < tsPrefetchus_params->w_history_prefetcher[req->obj_id]) {
    GList *prefetch_list = tsPrefetchus_params->history_prefetcher->get_prefetch_list(cache, req);
    for (GList *node = prefetch_list; node; node = node->next) {
      new_req->obj_id = GPOINTER_TO_INT(node->data);
      new_req->prefetch_flag = 2;  // this obj is prefetched by seq prefetcher
      /* ocf 中不考虑这部分
      if (cache->find(cache, new_req, false)) {
        continue;
      }
      while (cache->get_occupied_byte(cache) + tsPrefetchus_params->block_size + cache->obj_md_size >
             cache->cache_size) {
        cache->evict(cache, req);
      }
      cache->insert(cache, new_req); */
    }
    g_list_free(prefetch_list);
  }
  if (r < tsPrefetchus_params->w_sequential_prefetcher[merge_place]) {
    GList *prefetch_list = tsPrefetchus_params->sequential_prefetcher->get_prefetch_list(cache, req);
    for (GList *node = prefetch_list; node; node = node->next) {
      new_req->obj_id = GPOINTER_TO_INT(node->data);
      new_req->prefetch_flag = 1;  // this obj is prefetched by seq prefetcher
      // **** 和上面一致，略去
    }
    g_list_free(prefetch_list);
  }
  free_request(new_req);
}
```
重要数据结构
1. 对于每个请求，都分别保存了对两种预取专家的不同权重
unordered_map 可以用 c 库中的 GHashTable 代替，从而避开 cpp 编译错误
2. tsPrefetchus_params->block_size
3. 两种专家需要提供 get_prefetch_list() 函数，返回预取的请求。

### tsPrefetchus_handle_find
```c
static void tsPrefetchus_handle_find(cache_t *cache, const request_t *req, bool hit) {
  tsPrefetchus_params->history_prefetcher->handle_find(cache, req, hit);
  tsPrefetchus_params->sequential_prefetcher->handle_find(cache, req, hit);

  cache_obj_t *cache_obj = hashtable_find(cache->hashtable, req);
  if (!cache_obj) {  // cache->prefetcher->base_miss++;
  } else {
    tsPrefetchus_params->num_hit++;
    obj_id_t trigger_block = cache_obj->trigger_block;
    if (trigger_block) {
      if (cache_obj->prefetch_flag == 1) {
        uint64_t merge_place = (uint64_t)trigger_block / tsPrefetchus_params->merge_params_range;
        tsPrefetchus_params->w_sequential_prefetcher[merge_place] *=
            exp(tsPrefetchus_params->lr);  // increase weight_sequential_prefetcher
      } else if (cache_obj->prefetch_flag == 2) {
        tsPrefetchus_params->w_history_prefetcher[trigger_block] *=
            exp(tsPrefetchus_params->lr);  // increase weight_history_prefetcher
      }
    }
    // now update in LRU_find
    cache_obj->prefetch_flag = 0;  // one flag just be used once (TODO: can test is or not)
  }

  if (req->clock_time > 86400 && cache->n_req % tsPrefetchus_params->lr_update_interval == 0) {
    ts_update_lr(cache); // 需要 cache->prefetcher->params，所以入参为 cache
  }
}
1. tsPrefetchus_params->num_hit
2. cache_obj->trigger_block
3. cache_obj->prefetch_flag
4. tsPrefetchus_params->lr
5. tsPrefetchus_params->lr_update_interval

### tsPrefetchus_handle_evict
void tsPrefetchus_handle_evict(cache_t *cache, const request_t *check_req) {
  if (check_req->trigger_block) {
    if (check_req->prefetch_flag == 1) {
      uint64_t merge_place = (uint64_t)trigger_block / tsPrefetchus_params->merge_params_range;
      tsPrefetchus_params->w_sequential_prefetcher[merge_place] *=
          exp(-tsPrefetchus_params->lr);  // decrease weight_sequential_prefetcher
    } else if (check_req->prefetch_flag == 2) {
      tsPrefetchus_params->w_history_prefetcher[trigger_block] *=
          exp(-tsPrefetchus_params->lr);  // decrease weight_history_prefetcher
    }
  }
  tsPrefetchus_params->history_prefetcher->handle_evict(cache, check_req);
  tsPrefetchus_params->sequential_prefetcher->handle_evict(cache, check_req);
}
```
1. cache_obj->trigger_block
2. tsPrefetchus_params->lr
> !!! 考虑移植代码的过程中，哪些数据结构可以继承，尽量少改动。尤其是两个子专家预取系统。

### 并发问题
Ocf 中，ocf_engine_hndl_req 函数直接将请求塞入队列即返回。由于异步处理的存在，对缓存进行处理需要加锁。



