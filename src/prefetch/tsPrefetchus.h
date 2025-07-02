#ifndef TSPREFETCHUS_H
#define TSPREFETCHUS_H

#include <cstdint>
#include <unordered_map>

#include "../prefetchAlgo.h"
#include "AMP.h"
#include "Leap.h"
#include "Mithril.h"
#include "Mithril_adapt.h"
#include "Mithril_l.h"
#include "OBL.h"
#include "PG.h"
#include "PG_adapt.h"
#include "PG_l.h"

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

#endif