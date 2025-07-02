#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

#include <cstdint>

#include "glib.h"

#define TRACK_BLOCK 192618l
#define SANITY_CHECK 1
#define PROFILING
// #define DEBUG
#include "../../dataStructure/hashtable/hashtable.h"
#include "../../include/libCacheSim/prefetchAlgo/tsPrefetchus.h"
#include "glibconfig.h"

// ***********************************************************************
// ****                                                               ****
// ****               helper function declarations                    ****
// ****                                                               ****
// ***********************************************************************
const char *tsPrefetchus_default_params() {
  return "sequential=OBL, block-size=4096, history=Mithril, merge-params-range=1";
}

static void set_tsPrefetchus_default_init_params(tsPrefetchus_init_params_t *tsPrefetchus_init_params) {
  strcpy(tsPrefetchus_init_params->sequential_prefetcher_name, "OBL");
  tsPrefetchus_init_params->block_size = 4096;
  tsPrefetchus_init_params->merge_params_range = 1;
  tsPrefetchus_init_params->lr_update_interval = 1000000;
  strcpy(tsPrefetchus_init_params->history_prefetcher_name, "Mithril");
}

static void tsPrefetchus_parse_init_params(const char *cache_specific_params,
                                           tsPrefetchus_init_params_t *tsPrefetchus_init_params) {
  char *params_str = strdup(cache_specific_params);

  while (params_str != NULL && params_str[0] != '\0') {
    char *key = strsep((char **)&params_str, "=");
    char *value = strsep((char **)&params_str, ",");
    while (params_str != NULL && *params_str == ' ') {
      params_str++;
    }
    if (strcasecmp(key, "sequential") == 0) {
      strcpy(tsPrefetchus_init_params->sequential_prefetcher_name, value);
    } else if (strcasecmp(key, "block-size") == 0) {
      tsPrefetchus_init_params->block_size = atoi(value);
    } else if (strcasecmp(key, "history") == 0) {
      strcpy(tsPrefetchus_init_params->history_prefetcher_name, value);
    } else if (strcasecmp(key, "merge-params-range") == 0) {
      tsPrefetchus_init_params->merge_params_range = atoi(value);
    } else if (strcasecmp(key, "lr-update-interval") == 0) {
      tsPrefetchus_init_params->lr_update_interval = atoi(value);
    } else {
      ERROR("tsPrefetchus does not have parameter %s\n", key);
      printf("default params: %s\n", tsPrefetchus_default_params());
      exit(1);
    }
  }
}

static void set_tsPrefetchus_params(tsPrefetchus_params_t *tsPrefetchus_params,
                                    tsPrefetchus_init_params_t *tsPrefetchus_init_params, uint64_t cache_size) {
  if (strcasecmp(tsPrefetchus_init_params->sequential_prefetcher_name, "OBL") == 0) {
    tsPrefetchus_params->sequential_prefetcher =
        create_OBL_prefetcher(NULL, cache_size);  // TODO: prefetcher init_paramsm
    OBL_params_t *OBL_params = (OBL_params_t *)tsPrefetchus_params->sequential_prefetcher->params;
    OBL_params->block_size = tsPrefetchus_init_params->block_size;
  } else if (strcasecmp(tsPrefetchus_init_params->sequential_prefetcher_name, "AMP") == 0) {
    tsPrefetchus_params->sequential_prefetcher =
        create_AMP_prefetcher(NULL, cache_size);  // TODO: prefetcher init_paramsm
    AMP_params_t *AMP_params = (AMP_params_t *)tsPrefetchus_params->sequential_prefetcher->params;
    AMP_params->block_size = tsPrefetchus_init_params->block_size;
  } else if (strcasecmp(tsPrefetchus_init_params->sequential_prefetcher_name, "Leap") == 0) {
    tsPrefetchus_params->sequential_prefetcher =
        create_Leap_prefetcher(NULL, cache_size);  // TODO: prefetcher init_paramsm
    Leap_params_t *Leap_params = (Leap_params_t *)tsPrefetchus_params->sequential_prefetcher->params;
    Leap_params->block_size = tsPrefetchus_init_params->block_size;
  } else {
    ERROR("sequential prefetcher does not support %s\n", tsPrefetchus_init_params->sequential_prefetcher_name);
    printf("default params: %s\n", tsPrefetchus_default_params());
    exit(1);
  }

  if (strcasecmp(tsPrefetchus_init_params->history_prefetcher_name, "Mithril") == 0) {
    tsPrefetchus_params->history_prefetcher = create_Mithril_prefetcher(NULL, cache_size);
    Mithril_params_t *Mithril_params = (Mithril_params_t *)tsPrefetchus_params->history_prefetcher->params;
    Mithril_params->block_size = tsPrefetchus_init_params->block_size;
    // TODO: 在 create_Mithril_prefetcher 后不起作用了
    // Mithril_params->min_threshold_metadata_size = tsPrefetchus_init_params->history_min_threshold_metadata_size;
    // Mithril_params->max_threshold_metadata_size = tsPrefetchus_init_params->history_max_threshold_metadata_size;
  } else if (strcasecmp(tsPrefetchus_init_params->history_prefetcher_name, "PG") == 0) {
    tsPrefetchus_params->history_prefetcher = create_PG_prefetcher(NULL, cache_size);
    PG_params_t *PG_params = (PG_params_t *)tsPrefetchus_params->history_prefetcher->params;
    PG_params->block_size = tsPrefetchus_init_params->block_size;
  } else if (strcasecmp(tsPrefetchus_init_params->history_prefetcher_name, "Mithril-l") == 0) {
    tsPrefetchus_params->history_prefetcher = create_Mithril_l_prefetcher(NULL, cache_size);
    Mithril_l_params_t *Mithril_l_params = (Mithril_l_params_t *)tsPrefetchus_params->history_prefetcher->params;
    Mithril_l_params->block_size = tsPrefetchus_init_params->block_size;
  } else if (strcasecmp(tsPrefetchus_init_params->history_prefetcher_name, "Mithril-adapt") == 0) {
    tsPrefetchus_params->history_prefetcher = create_Mithril_adapt_prefetcher(NULL, cache_size);
    Mithril_adapt_params_t *Mithril_adapt_params =
        (Mithril_adapt_params_t *)tsPrefetchus_params->history_prefetcher->params;
    Mithril_adapt_params->block_size = tsPrefetchus_init_params->block_size;
    // TODO: 在 create_Mithril_adapt_prefetcher 后不起作用了
    // Mithril_adapt_params->min_threshold_metadata_size =
    // tsPrefetchus_init_params->history_min_threshold_metadata_size; Mithril_adapt_params->max_threshold_metadata_size
    // = tsPrefetchus_init_params->history_max_threshold_metadata_size;
  } else if (strcasecmp(tsPrefetchus_init_params->history_prefetcher_name, "PG-l") == 0) {
    tsPrefetchus_params->history_prefetcher = create_PG_l_prefetcher(NULL, cache_size);
    PG_l_params_t *PG_l_params = (PG_l_params_t *)tsPrefetchus_params->history_prefetcher->params;
    PG_l_params->block_size = tsPrefetchus_init_params->block_size;
  } else if (strcasecmp(tsPrefetchus_init_params->history_prefetcher_name, "PG-adapt") == 0) {
    tsPrefetchus_params->history_prefetcher = create_PG_adapt_prefetcher(NULL, cache_size);
    PG_adapt_params_t *PG_adapt_params = (PG_adapt_params_t *)tsPrefetchus_params->history_prefetcher->params;
    PG_adapt_params->block_size = tsPrefetchus_init_params->block_size;
  } else {
    ERROR("history prefetcher does not support %s\n", tsPrefetchus_init_params->history_prefetcher_name);
    printf("default params: %s\n", tsPrefetchus_default_params());
    exit(1);
  }

  // tsPrefetchus_params->w_sequential_prefetcher = tsPrefetchus_params->w_history_prefetcher = 1;
  tsPrefetchus_params->lr = 0.001;
  tsPrefetchus_params->lr_previous = 0;
  // tsPrefetchus_params->lr_update_interval = cache_size / 4096;  //  #objs can be filled
  tsPrefetchus_params->lr_update_interval = tsPrefetchus_init_params->lr_update_interval;
  tsPrefetchus_params->num_hit = tsPrefetchus_params->hit_rate_prev = tsPrefetchus_params->unlearn_count = 0;
  tsPrefetchus_params->block_size = tsPrefetchus_init_params->block_size;
  tsPrefetchus_params->merge_params_range = tsPrefetchus_init_params->merge_params_range;
}

void ts_update_lr(cache_t *cache) {
  tsPrefetchus_params_t *tsPrefetchus_params = (tsPrefetchus_params_t *)(cache->prefetcher->params);
  // Here: num_hit is the number of hits; reset to 0 when update_lr is called.
  double hit_rate_current = (double)tsPrefetchus_params->num_hit / tsPrefetchus_params->lr_update_interval;
  double delta_hit_rate = hit_rate_current - tsPrefetchus_params->hit_rate_prev;
  double delta_lr = tsPrefetchus_params->lr - tsPrefetchus_params->lr_previous;

  tsPrefetchus_params->lr_previous = tsPrefetchus_params->lr;
  tsPrefetchus_params->hit_rate_prev = hit_rate_current;

  if (delta_lr != 0) {
    int sign;
    // Intuition: If hit rate is decreasing (delta hit rate < 0)
    // Learning rate is positive (delta_lr > 0)
    // sign = -1 => decrease learning rate;
    if (delta_hit_rate / delta_lr > 0) {
      sign = 1;
    } else {
      sign = -1;
    }
    // If hit rate > learning rate
    // sign positive, increase learning rate;

    // Repo has this part: not included in the original paper.
    // if self.learning_rate >= 1:
    // self.learning_rate = 0.9
    // elif self.learning_rate <= 0.001:
    // self.learning_rate = 0.005
    if (tsPrefetchus_params->lr + sign * fabs(tsPrefetchus_params->lr * delta_lr) > 0.001) {
      tsPrefetchus_params->lr = tsPrefetchus_params->lr + sign * fabs(tsPrefetchus_params->lr * delta_lr);
    } else {
      tsPrefetchus_params->lr = 0.001;
    }
    tsPrefetchus_params->unlearn_count = 0;
  } else {
    if (hit_rate_current == 0 || delta_hit_rate <= 0) {
      tsPrefetchus_params->unlearn_count += 1;
    } else if (tsPrefetchus_params->unlearn_count >= 10) {
      tsPrefetchus_params->unlearn_count = 0;
      tsPrefetchus_params->lr =
          0.001 + ((double)(next_rand() % 10)) / 1000;  // learning rate chooses randomly between 10-3 & 10e-2
    }
  }
  // printf("learning rate: %f\n", tsPrefetchus_params->lr);
  tsPrefetchus_params->num_hit = 0;
}

// ***********************************************************************
// ****                                                               ****
// ****                     prefetcher interfaces                     ****
// ****                                                               ****
// ****   create, free, clone, handle_find, handle_evict, prefetch    ****
// ***********************************************************************
prefetcher_t *create_tsPrefetchus_prefetcher(const char *init_params, uint64_t cache_size);

/**
 sequential_prefetcher and history_prefetcher are called to handle find

 @param cache the cache struct
 @param req the request containing the request
 @return
*/
static void tsPrefetchus_handle_find(cache_t *cache, const ocf_request *req) {
  tsPrefetchus_params_t *tsPrefetchus_params = (tsPrefetchus_params_t *)(cache->prefetcher->params);

  tsPrefetchus_params->history_prefetcher->handle_find(cache, req);
  tsPrefetchus_params->sequential_prefetcher->handle_find(cache, req);

  // 执行到此函数，说明必定该请求必定命中缓存
  tsPrefetchus_params->num_hit++;
  obj_id_t trigger_block = req->trigger_block;
  if (trigger_block) {
    if (req->prefetch_flag == 1) {
      uint64_t merge_place = (uint64_t)trigger_block / tsPrefetchus_params->merge_params_range;
      tsPrefetchus_params->w_sequential_prefetcher[merge_place] *=
          exp(tsPrefetchus_params->lr);  // increase weight_sequential_prefetcher
      if (tsPrefetchus_params->w_sequential_prefetcher[merge_place] > 1) {
        tsPrefetchus_params->w_sequential_prefetcher[merge_place] = 1;
      }
    } else if (req->prefetch_flag == 2) {
      tsPrefetchus_params->w_history_prefetcher[trigger_block] *=
          exp(tsPrefetchus_params->lr);  // increase weight_history_prefetcher
      if (tsPrefetchus_params->w_history_prefetcher[trigger_block] > 1) {
        tsPrefetchus_params->w_history_prefetcher[trigger_block] = 1;
      }
    }
  }
  // now update in LRU_find
  req->prefetch_flag = 0;  // one flag just be used once (TODO: can test is or not)


  if (req->clock_time > 86400 && cache->n_req % tsPrefetchus_params->lr_update_interval == 0) {
    ts_update_lr(tsPrefetchus_params);
    // printf("learning rate: %f\n", tsPrefetchus_params->lr);
  }
}

static void tsPrefetchus_handle_insert(cache_t *cache, const request_t *req) {
  tsPrefetchus_params_t *tsPrefetchus_params = (tsPrefetchus_params_t *)(cache->prefetcher->params);
  if (tsPrefetchus_params->history_prefetcher->handle_insert) {
    tsPrefetchus_params->history_prefetcher->handle_insert(cache, req);
  }

  if (tsPrefetchus_params->sequential_prefetcher->handle_insert) {
    tsPrefetchus_params->sequential_prefetcher->handle_insert(cache, req);
  }
}

/**
 sequential_prefetcher and history_prefetcher are called to handle evict

 @param cache the cache struct
 @param req the request containing the request
 @return
*/
void tsPrefetchus_handle_evict(cache_t *cache, const request_t *check_req) {
  tsPrefetchus_params_t *tsPrefetchus_params = (tsPrefetchus_params_t *)(cache->prefetcher->params);
  obj_id_t trigger_block = check_req->trigger_block;

  if (trigger_block) {
    // the obj is prefetched to cache but not be accessed
    if (check_req->prefetch_flag == 1) {
      uint64_t merge_place = (uint64_t)trigger_block / tsPrefetchus_params->merge_params_range;
      tsPrefetchus_params->w_sequential_prefetcher[merge_place] *=
          exp(-tsPrefetchus_params->lr);  // decrease weight_sequential_prefetcher
      if (tsPrefetchus_params->w_sequential_prefetcher[merge_place] < 0.1) {
        tsPrefetchus_params->w_sequential_prefetcher[merge_place] = 0.1;
      }
    } else if (check_req->prefetch_flag == 2) {
      tsPrefetchus_params->w_history_prefetcher[trigger_block] *=
          exp(-tsPrefetchus_params->lr);  // decrease weight_history_prefetcher
      if (tsPrefetchus_params->w_history_prefetcher[trigger_block] < 0.1) {
        tsPrefetchus_params->w_history_prefetcher[trigger_block] = 0.1;
      }
    }
  }
  tsPrefetchus_params->history_prefetcher->handle_evict(cache, check_req);
  tsPrefetchus_params->sequential_prefetcher->handle_evict(cache, check_req);
}

/**
 prefetch some objects which are from `_PG_get_prefetch_list`

 @param cache the cache struct
 @param req the request containing the request
 @return
 */
void tsPrefetchus_prefetch(ocf_cache_t cache, const ocf_request *req) {
  // TODO: 还是需要维护一个自己的 request 结构，方便内部处理
  // 需要包括prefetch_flag（是否是通过预取机制加入缓存的。），trigger_block（触发预取的原始对象ID）
  ts_request *ts_req = my_malloc(ts_request);
  convert_ocf_request_to_ts_request(req, ts_req);

  tsPrefetchus_params_t *tsPrefetchus_params = (tsPrefetchus_params_t *)(cache->prefetcher->params);

  // TODO：修改成 glib 的 hash 表
  if (!tsPrefetchus_params->w_history_prefetcher.count(req->ioi.io.addr)) {
    tsPrefetchus_params->w_history_prefetcher[req->ioi.io.addr] = 1;
  }
  uint64_t merge_place = (uint64_t)req->ioi.io.addr / tsPrefetchus_params->merge_params_range;
  if (!tsPrefetchus_params->w_sequential_prefetcher.count(merge_place)) {
    tsPrefetchus_params->w_sequential_prefetcher[merge_place] = 1;
  }

  double r;
  r = ((double)(next_rand() % 100)) / 100.0;
  ocf_request *new_req = my_malloc(ts_request);
  new_req->obj_size = tsPrefetchus_params->block_size;
  new_req->trigger_block = req->ioi.io.addr;
  // 关于推荐的预取 IO 数量问题，考虑降低至 1 个
  if (r < tsPrefetchus_params->w_history_prefetcher[req->ioi.io.addr]) {
    GList *prefetch_list = tsPrefetchus_params->history_prefetcher->get_prefetch_list(cache, req);
    for (GList *node = prefetch_list; node; node = node->next) {
      new_req->obj_id = GPOINTER_TO_INT(node->data);
      new_req->prefetch_flag = 2;  // this obj is prefetched by seq prefetcher
    }
    g_list_free(prefetch_list);
  }

  if (r < tsPrefetchus_params->w_sequential_prefetcher[merge_place]) {
    // tsPrefetchus_params->sequential_prefetcher->prefetch(cache, req);
    GList *prefetch_list = tsPrefetchus_params->sequential_prefetcher->get_prefetch_list(cache, req);
    for (GList *node = prefetch_list; node; node = node->next) {
      new_req->obj_id = GPOINTER_TO_INT(node->data);
      new_req->prefetch_flag = 1;  // this obj is prefetched by seq prefetcher
    }
    g_list_free(prefetch_list);
  }
  free_request(new_req);
  return prefetch_io;
}

void free_tsPrefetchus_prefetcher(prefetcher_t *prefetcher) {
  tsPrefetchus_params_t *tsPrefetchus_params = (tsPrefetchus_params_t *)(prefetcher->params);

  tsPrefetchus_params->sequential_prefetcher->free(tsPrefetchus_params->sequential_prefetcher);
  tsPrefetchus_params->history_prefetcher->free(tsPrefetchus_params->history_prefetcher);

  delete tsPrefetchus_params;
  if (prefetcher->init_params) {
    free(prefetcher->init_params);
  }
  my_free(sizeof(prefetcher_t), prefetcher);
}

prefetcher_t *clone_tsPrefetchus_prefetcher(prefetcher_t *prefetcher, uint64_t cache_size) {
  return create_tsPrefetchus_prefetcher((const char *)prefetcher->init_params, cache_size);
}

prefetcher_t *create_tsPrefetchus_prefetcher(const char *init_params, uint64_t cache_size) {
  tsPrefetchus_init_params_t *tsPrefetchus_init_params = my_malloc(tsPrefetchus_init_params_t);
  memset(tsPrefetchus_init_params, 0, sizeof(tsPrefetchus_init_params_t));

  set_tsPrefetchus_default_init_params(tsPrefetchus_init_params);
  if (init_params != NULL) {
    tsPrefetchus_parse_init_params(init_params, tsPrefetchus_init_params);
  }

  tsPrefetchus_params_t *tsPrefetchus_params = new tsPrefetchus_params_t();

  set_tsPrefetchus_params(tsPrefetchus_params, tsPrefetchus_init_params, cache_size);

  prefetcher_t *prefetcher = (prefetcher_t *)my_malloc(prefetcher_t);
  memset(prefetcher, 0, sizeof(prefetcher_t));
  prefetcher->params = tsPrefetchus_params;
  prefetcher->get_prefetch_list = NULL;
  // 核心就是 prefetch，find 和 evict，后续需要封装一下，提供给 ocf 使用！
  prefetcher->prefetch = tsPrefetchus_prefetch;
  prefetcher->handle_find = tsPrefetchus_handle_find;
  prefetcher->handle_insert = tsPrefetchus_handle_insert;
  prefetcher->handle_evict = tsPrefetchus_handle_evict;
  prefetcher->free = free_tsPrefetchus_prefetcher;
  prefetcher->clone = clone_tsPrefetchus_prefetcher;
  strcpy(prefetcher->name, "tsPrefetchus");
  if (init_params) {
    prefetcher->init_params = strdup(init_params);
  }

  my_free(sizeof(tsPrefetchus_init_params_t), tsPrefetchus_init_params);
  return prefetcher;
}