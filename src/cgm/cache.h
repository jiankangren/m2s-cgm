/*
 * cache.h
 *
 *  Created on: Nov 25, 2014
 *      Author: stardica
 */


#ifndef CACHE_H_
#define CACHE_H_

#include <cgm/cgm.h>

//star todo add prefetching and coalescing

extern struct str_map_t cache_policy_map;
extern struct str_map_t cache_block_state_map;

enum cache_policy_t{

	cache_policy_invalid = 0,
	cache_policy_lru,
	cache_policy_fifo,
	cache_policy_random
};

enum cache_block_state_t{

	cache_block_invalid = 0,
	cache_block_noncoherent,
	cache_block_modified,
	cache_block_owned,
	cache_block_exclusive,
	cache_block_shared
};

struct cache_block_t{

	struct cache_block_t *way_next;
	struct cache_block_t *way_prev;

	int tag;
	int transient_tag;
	int way;
	int prefetched;

	enum cache_block_state_t state;
};

struct cache_set_t{

	int id;

	struct cache_block_t *way_head;
	struct cache_block_t *way_tail;
	struct cache_block_t *blocks;
};


struct cache_t{

	//star >> my added elements.
	char * name;
	int id;

	//cache configuration settings
	unsigned int num_sets;
	unsigned int block_size;
	unsigned int assoc;
	unsigned int num_ports;

	//enum cache_policy_t policy;
	const char *policy;
	unsigned int latency;
	unsigned int mshr_size;
	unsigned int directory_latency;

	//cache data
	struct cache_set_t *sets;
	unsigned int block_mask;
	int log_block_size;

	struct list_t *Rx_queue;
	//struct list_t *Tx_queue;

	//access record
	struct list_t *cache_accesses;
	//struct prefetcher_t *prefetcher;

};

extern int QueueSize;


//CPU caches
extern struct cache_t *l1_i_caches;
extern struct cache_t *l1_d_caches;
extern struct cache_t *l2_caches;

extern struct cache_t *l3_caches;
/*extern struct cache_t *l3_s0_cache;
extern struct cache_t *l3_s1_cache;
extern struct cache_t *l3_s2_cache;
extern struct cache_t *l3_s3_cache;*/

//GPU caches
extern struct cache_t *l1_v_caches;
extern struct cache_t *l1_s_caches;
extern struct cache_t *gpu_l2_caches;
extern struct cache_t *lds_units;


//function prototypes
void cache_init(void);
int i_cache_ctrl(int id, enum cgm_access_kind_t task);


//from m2s
void cache_decode_address(struct cache_t *cache, unsigned int addr, int *set_ptr, int *tag_ptr, unsigned int *offset_ptr);
int cache_find_block(struct cache_t *cache, unsigned int addr, int *set_ptr, int *pway, int *state_ptr);
void cache_set_block(struct cache_t *cache, int set, int way, int tag, int state);
void cache_get_block(struct cache_t *cache, int set, int way, int *tag_ptr, int *state_ptr);

void cache_access_block(struct cache_t *cache, int set, int way);
int cache_replace_block(struct cache_t *cache, int set);
void cache_set_transient_tag(struct cache_t *cache, int set, int way, int tag);


#endif /*CACHE_H_*/
