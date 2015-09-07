/*
 * cgm-struct.h
 *
 *  Created on: Sep 6, 2015
 *      Author: stardica
 */


#ifndef CGMSTRUCT_H_
#define CGMSTRUCT_H_

#include <lib/util/list.h>

#include <cgm/tasking.h>
#include <cgm/directory.h>

enum cgm_cache_block_state_t{

	cgm_cache_block_invalid = 0,
	cgm_cache_block_noncoherent,
	cgm_cache_block_modified,
	cgm_cache_block_owned,
	cgm_cache_block_exclusive,
	cgm_cache_block_shared,
	cgm_cache_block_transient,
	cgm_cache_block_null,
	cgm_cache_block_state_num
};

enum cache_waylist_enum{

	cache_waylist_head,
	cache_waylist_tail
};


enum protocol_kind_t {

	cgm_protocol_mesi = 0,
	cgm_protocol_moesi,
	cgm_protocol_gmesi,
	cgm_protocol_non_coherent,
	num_cgm_protocol_types
};

enum cgm_access_kind_t {

	cgm_access_invalid = 0,
	cgm_access_fetch,
	cgm_access_load,
	cgm_access_store,
	cgm_access_nc_store,
	cgm_access_nc_load,
	cgm_access_store_v,
	cgm_access_load_s,
	cgm_access_load_v,
	cgm_access_prefetch,
	cgm_access_gets, //get shared
	cgm_access_gets_i,
	cgm_access_get, //get specific to d caches
	cgm_access_get_fwd,
	cgm_access_get_fwd_nack,
	cgm_access_getx_fwd,
	cgm_access_getx_fwd_nack,
	cgm_access_getx_fwd_ack,
	cgm_access_getx_fwd_inval_ack,
	cgm_access_gets_s, //get shared specific to s caches
	cgm_access_gets_v, //get shared specific to v caches
	cgm_access_getx, //get exclusive (or get with intent to write)
	cgm_access_inv,  //invalidation request
	cgm_access_inv_ack,
	cgm_access_upgrade, //upgrade request
	cgm_access_upgrade_ack,
	cgm_access_downgrade, //downgrade request
	cgm_access_downgrade_ack,
	cgm_access_downgrade_nack,
	cgm_access_mc_get,	//request sent to system agent/memory controller
	cgm_access_mc_put,	//reply from system agent/memory controller
	cgm_access_put_clnx, //put block in clean exclusive state
	cgm_access_putx, //
	cgm_access_puts, //request for write back of cache block in shared state.
	cgm_access_puto, //request for write back of cache block in owned state.
	cgm_access_puto_shared, //request for write back of cache block in owned state but other sharers of the block exist.
	cgm_access_unblock, //message to unblock next cache level/directory for blocking protocols.
	cgm_access_retry,
	cgm_access_fetch_retry,
	cgm_access_load_retry,
	cgm_access_store_retry,
	cgm_access_write_back,
	cgm_access_retry_i,//not used
	num_access_types
};

struct cgm_packet_t{

	char *name;

	//star todo clean this up when the simulator is done.
	enum cgm_access_kind_t access_type;
	enum cgm_access_kind_t l1_access_type;
	enum cgm_access_kind_t cpu_access_type;
	enum cgm_access_kind_t gpu_access_type;

	int l1_cache_id;
	char *l2_cache_name;
	int l2_cache_id;
	int gpu_cache_id;

	//access data
	long long access_id;
	unsigned int address;
	int set;
	int tag;
	int way;
	unsigned int offset;
	int size;
	int coalesced;

	//for evictions, write backs, downgrades
	int flush_pending;
	int downgrade;
	int downgrade_pending;
	int downgrade_ack;
	int inval;
	int inval_pending;
	int inval_ack;


	int l1_victim_way;
	int l2_victim_way;
	int l3_victim_way;

	//for protocol messages
	enum cgm_cache_block_state_t cache_block_state;

	//for routing
	char *src_name;
	int src_id;
	char *dest_name;
	int dest_id;

	//for m2s CPU and GPU
	struct linked_list_t *event_queue;
	int *witness_ptr;
	void *data;

	//stats
	long long start_cycle;
	long long end_cycle;
};

struct cgm_packet_status_t{

	//used for global memory list
	enum cgm_access_kind_t access_type;
	unsigned int address;
	long long access_id;
	int in_flight;
};

struct cache_block_t{

	struct cache_block_t *way_next;
	struct cache_block_t *way_prev;

	int tag;
	int set;
	int transient_tag;
	int way;
	int prefetched;
	int flush_pending;
	unsigned int address;

	enum cgm_cache_block_state_t state;
	enum cgm_cache_block_state_t transient_state;

	//each block has it's own directory (unsigned char)
	union directory_t directory_entry;
	int data_type;

	//for error checking
	long long transient_access_id;
};

struct cache_set_t{

	int id;

	struct cache_block_t *way_head;
	struct cache_block_t *way_tail;
	struct cache_block_t *blocks;

};

enum cache_type_enum{

	l1_i_cache_t,
	l1_d_cache_t,
	l2_cache_t,
	l3_cache_t,
	gpu_s_cache_t,
	gpu_v_cache_t,
	gpu_l2_cache_t
};

enum cache_policy_t{

	cache_policy_invalid = 0,
	cache_policy_lru,
	cache_policy_fifo,
	cache_policy_random,
	cache_policy_num
};

struct cache_t{

	//star >> my added elements.
	char *name;
	int id;

	enum cache_type_enum cache_type;

	//this is so the cache can advance itself
	eventcount *ec_ptr;

	//cache configuration settings
	unsigned int num_slices;
	unsigned int num_sets;
	unsigned int block_size;
	unsigned int assoc;
	unsigned int num_ports;
	enum cache_policy_t policy;
	char * policy_type;
	int slice_type;
	int bus_width;

	//cache data
	struct cache_set_t *sets;
	unsigned int block_mask;
	int log_block_size;
	unsigned int set_mask;
	int log_set_size;

	//mshr control links
	int mshr_size;
	struct mshr_t *mshrs;

	//outstanding request table
	int **ort;
	struct list_t *ort_list;
	int max_coal;

	//cache queues
	//star todo rewrite all of this queues should be inboxes
	//buffers are internal buffers
	struct list_t *Rx_queue_top;
	struct list_t *Rx_queue_bottom;
	struct list_t *Tx_queue_top;
	struct list_t *Tx_queue_bottom;
	struct list_t *Coherance_Tx_queue;
	struct list_t *Coherance_Rx_queue;
	struct list_t *retry_queue;
	struct list_t *write_back_buffer;
	struct list_t *pending_request_buffer;
	struct list_t *next_queue;
	struct list_t *last_queue;

	//io ctrl
	eventcount volatile *cache_io_up_ec;
	task *cache_io_up_tasks;

	eventcount volatile *cache_io_down_ec;
	task *cache_io_down_tasks;

	//physical characteristics
	unsigned int latency;
	unsigned int wire_latency;
	unsigned int directory_latency;

	//directory bit vectors for coherence
	unsigned int dir_latency;
	union directory_t **dir;
	unsigned int share_mask;

	//L1 I cache protocol virtual functions
	void (*l1_i_fetch)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*l1_i_write_block)(struct cache_t *cache, struct cgm_packet_t *message_packet);

	//L1 D cache protocol virtual functions
	void (*l1_d_load)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*l1_d_store)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	int (*l1_d_write_block)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*l1_d_downgrade)(struct cache_t *cache, struct cgm_packet_t *message_packet);

	//L2 cache protocol virtual functions
	void (*l2_gets)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*l2_get)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*l2_downgrade_ack)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*l2_get_fwd)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*l2_getx_fwd)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*l2_getx_fwd_inval_ack)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*l2_inval_ack)(struct cache_t *cache, struct cgm_packet_t *message_packet);

	int (*l2_write_block)(struct cache_t *cache, struct cgm_packet_t *message_packet);

	//L3 cache protocol virtual functions
	void (*l3_gets)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*l3_get)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*l3_downgrade_ack)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*l3_downgrade_nack)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*l3_getx_fwd_nack)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*l3_getx_fwd_ack)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*l3_write_block)(struct cache_t *cache, struct cgm_packet_t *message_packet);

	//GPU S cache protocol virtual functions
	void (*gpu_s_load)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*gpu_s_put)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*gpu_s_retry)(struct cache_t *cache, struct cgm_packet_t *message_packet);

	//GPU V cache protocol virtual functions
	void (*gpu_v_load)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*gpu_v_store)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*gpu_v_put)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*gpu_v_retry)(struct cache_t *cache, struct cgm_packet_t *message_packet);

	//GPU L2 cache protocol virtual functions
	void (*gpu_l2_get)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*gpu_l2_put)(struct cache_t *cache, struct cgm_packet_t *message_packet);
	void (*gpu_l2_retry)(struct cache_t *cache, struct cgm_packet_t *message_packet);


	//statistics
	long long fetches;
	long long loads;
	long long stores;
	long long hits;
	long long invalid_hits;
	long long misses;
	long long upgrade_misses;
	long long retries;
	long long coalesces;
	long long mshr_entires;
	long long stalls;
	unsigned int *fetch_address_history;
	unsigned int *load_address_history;
	unsigned int *store_address_history;
};


#endif /* CGMSTRUCT_H_ */