//
//  an OBL module that supports sequential prefetching for block storage. Each
//  object (logical block address) should be uniform in size.
//
//
//  OBL.c
//  libCacheSim
//
//  Created by Zhelong on 24/1/29.
//  Copyright © 2024 Zhelong. All rights reserved.
//
#include "../../include/libCacheSim/prefetchAlgo/OBL.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

#include "../../dataStructure/hashtable/hashtable.h"
#include "../../include/libCacheSim/prefetchAlgo.h"
#include "../../include/libCacheSim/prefetchAlgo/Prefetchus.h"
#include "glib.h"
// #define DEBUG

#ifdef __cplusplus
extern "C" {
#endif

// ***********************************************************************
// ****                                                               ****
// ****               helper function declarations                    ****
// ****                                                               ****
// ***********************************************************************

const char *OBL_default_params(void) { return "block-size=4096, sequential-confidence-k=4"; }

void set_OBL_default_init_params(OBL_init_params_t *init_params) {
  init_params->block_size = 4096;
  init_params->sequential_confidence_k = 4;
}

void OBL_parse_init_params(const char *cache_specific_params, OBL_init_params_t *init_params) {
  char *params_str = strdup(cache_specific_params);

  while (params_str != NULL && params_str[0] != '\0') {
    char *key = strsep((char **)&params_str, "=");
    char *value = strsep((char **)&params_str, ",");
    while (params_str != NULL && *params_str == ' ') {
      params_str++;
    }
    if (strcasecmp(key, "block-size") == 0) {
      init_params->block_size = atoi(value);
    } else if (strcasecmp(key, "sequential-confidence-k") == 0) {
      init_params->sequential_confidence_k = atoi(value);
    } else {
      ERROR("OBL does not have parameter %s\n", key);
      printf("default params: %s\n", OBL_default_params());
      exit(1);
    }
  }
}

void set_OBL_params(OBL_params_t *OBL_params, OBL_init_params_t *init_params, uint64_t cache_size) {
  OBL_params->block_size = init_params->block_size;
  OBL_params->sequential_confidence_k = init_params->sequential_confidence_k;
  OBL_params->do_prefetch = false;
  if (OBL_params->sequential_confidence_k <= 0) {
    printf("sequential_confidence_k should be positive\n");
    exit(1);
  }
  OBL_params->prev_access_block = (obj_id_t *)malloc(OBL_params->sequential_confidence_k * sizeof(obj_id_t));
  for (int i = 0; i < OBL_params->sequential_confidence_k; i++) {
    OBL_params->prev_access_block[i] = UINT64_MAX;
  }
  OBL_params->curr_idx = 0;
}

/**************************************************************************
 **                      prefetcher interfaces
 **
 ** create, free, clone, handle_find, handle_insert, handle_evict, prefetch
 **************************************************************************/
prefetcher_t *create_OBL_prefetcher(const char *init_params, uint64_t cache_size);
/**
 check if the previous access is sequential. If true, set do_prefetch to true.

@param cache the cache struct
@param req the request containing the request
@return
*/
void OBL_handle_find(cache_t *cache, const request_t *req, bool hit) {
  OBL_params_t *OBL_params;
  if (strcasecmp(cache->prefetcher->name, "OBL") == 0) {
    OBL_params = (OBL_params_t *)(cache->prefetcher->params);

    cache_obj_t *cache_obj = hashtable_find(cache->hashtable, req);
    if (!cache_obj) {
      cache->prefetcher->base_miss++;
    } else if (cache_obj->prefetch_flag) {
      cache->prefetcher->prefetch_hit++;
      cache->prefetcher->base_miss++;
      cache_obj->prefetch_flag = 0;
    }
  } else {
    Prefetchus_params_t *prefetchus_params = (Prefetchus_params_t *)(cache->prefetcher->params);
    OBL_params = (OBL_params_t *)(prefetchus_params->sequential_prefetcher->params);
  }
  int32_t sequential_confidence_k = OBL_params->sequential_confidence_k;

  // printf("%ld %d\n", req->obj_size, OBL_params->block_size);
  assert(req->obj_size == OBL_params->block_size);
  bool flag = true;
  for (int i = 0; i < sequential_confidence_k; i++) {
    if (OBL_params->prev_access_block[(OBL_params->curr_idx + 1 + i) % sequential_confidence_k] !=
        req->obj_id - sequential_confidence_k + i) {
      flag = false;
      break;
    }
  }
  OBL_params->do_prefetch = flag;
  OBL_params->curr_idx = (OBL_params->curr_idx + 1) % sequential_confidence_k;
  OBL_params->prev_access_block[OBL_params->curr_idx] = req->obj_id;
}

void OBL_handle_insert(cache_t *cache, const request_t *req) { return; }

void OBL_handle_evict(cache_t *cache, const request_t *req) { return; }

/**
 prefetch next block if the previous access is sequential

 @param cache the cache struct
 @param req the request containing the request
 @return
 */
void OBL_prefetch(cache_t *cache, const request_t *req) {
  OBL_params_t *OBL_params;
  if (strcasecmp(cache->prefetcher->name, "OBL") == 0) {
    OBL_params = (OBL_params_t *)(cache->prefetcher->params);
  } else {
    Prefetchus_params_t *prefetchus_params = (Prefetchus_params_t *)(cache->prefetcher->params);
    OBL_params = (OBL_params_t *)(prefetchus_params->sequential_prefetcher->params);
  }

  if (req->offset_end && OBL_params->do_prefetch) {  // 一次逻辑 io 结束后才会进行预取
    OBL_params->do_prefetch = false;
    request_t *new_req = new_request();
    new_req->obj_size = OBL_params->block_size;
    new_req->obj_id = req->obj_id + 1;
    if (cache->find(cache, new_req, false)) {
      free_request(new_req);
      return;
    }
    // if (req->clock_time > 86400) {
    //   new_req->prefetch_flag = 1;  // this obj is prefetched by seq prefetcher
    // } else {
    //   new_req->prefetch_flag = 0;
    // }
    while (cache->get_occupied_byte(cache) + OBL_params->block_size + cache->obj_md_size > cache->cache_size) {
      cache->evict(cache, req);
    }
    if (strcasecmp(cache->prefetcher->name, "OBL") == 0) {
      cache->prefetcher->total_prefetch++;
      new_req->prefetch_flag = 1;
    }
    cache->insert(cache, new_req);
    free_request(new_req);
  }
}

GList *OBL_get_prefetch_list(cache_t *cache, const request_t *req) {
  OBL_params_t *OBL_params;
  if (strcasecmp(cache->prefetcher->name, "OBL") == 0) {
    OBL_params = (OBL_params_t *)(cache->prefetcher->params);
  } else {
    Prefetchus_params_t *prefetchus_params = (Prefetchus_params_t *)(cache->prefetcher->params);
    OBL_params = (OBL_params_t *)(prefetchus_params->sequential_prefetcher->params);
  }

  GList *prefetch_list = NULL;

  if (req->offset_end && OBL_params->do_prefetch) {  // 一次逻辑 io 结束后才会进行预取
    OBL_params->do_prefetch = false;
    request_t *new_req = new_request();
    new_req->obj_size = OBL_params->block_size;
    new_req->obj_id = req->obj_id + 1;
    if (cache->find(cache, new_req, false)) {
      free_request(new_req);
      return prefetch_list;
    }
    // while (cache->get_occupied_byte(cache) + OBL_params->block_size + cache->obj_md_size > cache->cache_size) {
    //   cache->evict(cache, req);
    // }
    prefetch_list = g_list_append(prefetch_list, GINT_TO_POINTER(new_req->obj_id));
    free_request(new_req);
  }
  return prefetch_list;
}

void free_OBL_prefetcher(prefetcher_t *prefetcher) {
  OBL_params_t *OBL_params = (OBL_params_t *)prefetcher->params;
  free(OBL_params->prev_access_block);

  my_free(sizeof(OBL_params_t), OBL_params);
  if (prefetcher->init_params) {
    free(prefetcher->init_params);
  }
  my_free(sizeof(prefetcher_t), prefetcher);
}

prefetcher_t *clone_OBL_prefetcher(prefetcher_t *prefetcher, uint64_t cache_size) {
  return create_OBL_prefetcher(prefetcher->init_params, cache_size);
}

prefetcher_t *create_OBL_prefetcher(const char *init_params, uint64_t cache_size) {
  OBL_init_params_t *OBL_init_params = my_malloc(OBL_init_params_t);
  memset(OBL_init_params, 0, sizeof(OBL_init_params_t));

  set_OBL_default_init_params(OBL_init_params);
  if (init_params != NULL) {
    OBL_parse_init_params(init_params, OBL_init_params);
  }

  OBL_params_t *OBL_params = my_malloc(OBL_params_t);
  set_OBL_params(OBL_params, OBL_init_params, cache_size);

  prefetcher_t *prefetcher = (prefetcher_t *)my_malloc(prefetcher_t);
  memset(prefetcher, 0, sizeof(prefetcher_t));
  prefetcher->params = OBL_params;
  prefetcher->get_prefetch_list = OBL_get_prefetch_list;
  prefetcher->prefetch = OBL_prefetch;
  prefetcher->handle_find = OBL_handle_find;
  prefetcher->handle_insert = OBL_handle_insert;
  prefetcher->handle_evict = OBL_handle_evict;
  prefetcher->free = free_OBL_prefetcher;
  prefetcher->clone = clone_OBL_prefetcher;
  strcpy(prefetcher->name, "OBL");
  if (init_params) {
    prefetcher->init_params = strdup(init_params);
  }

  my_free(sizeof(OBL_init_params_t), OBL_init_params);
  return prefetcher;
}