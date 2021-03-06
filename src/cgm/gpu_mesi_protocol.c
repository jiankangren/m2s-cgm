
//////////////////////
/////GPU MESI protocol
//////////////////////

#include <cgm/protocol.h>


int gpu_core_id = 0;

void cgm_mesi_gpu_s_load(struct cache_t *cache, struct cgm_packet_t *message_packet){

	/*GPU S$ contains read only data that is established prior to kernel execution (during OS/driver configuration)
	it should be sufficient to charge a small latency and continue on for simulator purposes.*/

	GPU_PAUSE(cache->latency);

	message_packet = list_remove(cache->last_queue, message_packet);
	(*message_packet->witness_ptr)++;
	packet_destroy(message_packet);

	return;
}

void cgm_mesi_gpu_l1_v_load(struct cache_t *cache, struct cgm_packet_t *message_packet){

	/*printf("access_id %llu\n",message_packet->access_id);*/


	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	struct cgm_packet_t *write_back_packet = NULL;

	//get the status of the cache block
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	//search the WB buffer for the data if in WB the block is either in the E or M state so return
	write_back_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

	//update cache way list for cache replacement policies.
	if(*cache_block_hit_ptr == 1)
	{
		//make this block the MRU
		cgm_cache_update_waylist(&cache->sets[message_packet->set], cache->sets[message_packet->set].way_tail, cache_waylist_head);
	}

	//charge delay
	GPU_PAUSE(cache->latency);

	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_owned:
		case cgm_cache_block_noncoherent:
			fatal("cgm_mesi_gpu_l1_v_load(): Invalid block state on load hit as \"%s\"\n", str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr));
			break;

		//miss or invalid cache block states
		case cgm_cache_block_invalid:

			//block is in write back !!!!
			if(write_back_packet)
			{
				/*found the packet in the write back buffer
				data should not be in the rest of the cache*/
				assert(*cache_block_hit_ptr == 0);
				assert(write_back_packet->cache_block_state == cgm_cache_block_modified);
				assert(message_packet->set == write_back_packet->set && message_packet->tag == write_back_packet->tag);
				assert(message_packet->access_type == cgm_access_load || message_packet->access_type == cgm_access_load_retry);

				//see if we can write it back into the cache.
				write_back_packet->l1_victim_way = cgm_cache_get_victim_for_wb(cache, write_back_packet->set);

				//if not then we must coalesce
				if(write_back_packet->l1_victim_way == -1)
				{
					//Set and ways are all transient must coalesce
					cache_check_ORT(cache, message_packet);

					/*stats*/
					cache->CoalescePut++;

					if(message_packet->coalesced == 1)
						return;
					else
						fatal("cgm_mesi_gpu_load(): write failed to coalesce when all ways are transient...\n");
				}

				//we are writing in a block so evict the victim
				assert(write_back_packet->l1_victim_way >= 0 && write_back_packet->l1_victim_way < cache->assoc);

				//first evict the old block if it isn't invalid already
				if(cgm_cache_get_block_state(cache, write_back_packet->set, write_back_packet->l1_victim_way) != cgm_cache_block_invalid)
					cgm_L1_cache_evict_block(cache, write_back_packet->set, write_back_packet->l1_victim_way);

				//now set the block
				cgm_cache_set_block(cache, write_back_packet->set, write_back_packet->l1_victim_way, write_back_packet->tag, write_back_packet->cache_block_state);

				//free the write back
				write_back_packet = list_remove(cache->write_back_buffer, write_back_packet);
				packet_destroy(write_back_packet);

				GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s load wb hit id %llu state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, *cache_block_state_ptr, P_TIME);

				//check for retries on successful cache read...
				if(message_packet->access_type == cgm_access_load_retry || message_packet->coalesced == 1)
				{
					cache->MergeRetries++;

					//enter retry state.
					cache_coalesed_retry(cache, message_packet->tag, message_packet->set, message_packet->access_id);
				}

				/*stats*/
				cache->WbMerges++;
				message_packet->end_cycle = P_TIME;
				if(!message_packet->protocol_case)
					message_packet->protocol_case = L1_hit;

				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;

				cache_gpu_v_return(cache, message_packet);
				return;
			}
			else
			{
				//block isn't in the cache or in write back.

				//check ORT for coalesce
				cache_check_ORT(cache, message_packet);

				if(message_packet->coalesced == 1)
				{
					/*stats*/
					cache->CoalescePut++;

					GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s load miss coalesce ID %llu type %d state %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, *cache_block_state_ptr, P_TIME);

					return;
				}

				//add some routing/status data to the packet
				message_packet->access_type = cgm_access_get;
				message_packet->l1_access_type = cgm_access_get;
				message_packet->l1_cache_id = cache->id;

				//find victim
				message_packet->l1_victim_way = cgm_cache_get_victim(cache, message_packet->set, message_packet->tag);
				assert(cache->sets[message_packet->set].blocks[message_packet->l1_victim_way].directory_entry.entry_bits.pending != 1);

				/*	message_packet->l1_victim_way = cgm_cache_replace_block(cache, message_packet->set);*/
				assert(message_packet->l1_victim_way >= 0 && message_packet->l1_victim_way < cache->assoc);

				if(cgm_cache_get_block_state(cache, message_packet->set, message_packet->l1_victim_way) != cgm_cache_block_invalid)
					cgm_L1_cache_evict_block(cache, message_packet->set, message_packet->l1_victim_way);

				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;


			/*	/////////////////
				if((message_packet->address & cache->block_address_mask) == 0x08123380)
				warning("GPU vector $ id %d load id %llu blk addr 0x%08x!\n",
						cache->id, message_packet->access_id, (message_packet->address & cache->block_address_mask));
				fflush(stderr);
				/////////////////*/


				//transmit to L2
				cache_put_io_down_queue(cache, message_packet);

				GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s load miss id %llu state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, *cache_block_state_ptr, P_TIME);

			}

			break;

		//hit states
		case cgm_cache_block_modified:
		case cgm_cache_block_exclusive:
		case cgm_cache_block_shared:


			GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s load hit ID %llu type %d state %d cycle %llu\n",
					(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
					message_packet->access_type, *cache_block_state_ptr, P_TIME);


			//set the retry state and charge latency
			if(message_packet->access_type == cgm_access_load_retry || message_packet->coalesced == 1)
			{
				//enter retry state.
				cache_coalesed_retry(cache, message_packet->tag, message_packet->set, message_packet->access_id);
			}

			/*stats*/
			message_packet->end_cycle = P_TIME;

			if(!message_packet->protocol_case)
				message_packet->protocol_case = L1_hit;

			/*reset mp flags*/
			message_packet->coalesced = 0;
			message_packet->assoc_conflict = 0;

			cache_gpu_v_return(cache,message_packet);

			/*if((message_packet->address & cache->block_address_mask) == 0x08123380 && message_packet->access_id == 1199071)
			warning("GPU_1 L1 $ id %d load hit %llu phy address 0x%08x state %d\n",
						cache->id, message_packet->access_id,
						(message_packet->address & cache->block_address_mask),
						*cache_block_state_ptr);
			fflush(stderr);*/

			/*if((message_packet->address & cache->block_address_mask) == 0x08123380)
			fatal("GPU_2 L1 $ id %d load hit %llu phy address 0x%08x state %d\n",
						cache->id, message_packet->access_id,
						(message_packet->address & cache->block_address_mask),
						*cache_block_state_ptr);
			fflush(stderr);*/

			break;
	}

	return;
}

void cgm_mesi_gpu_l1_v_load_nack(struct cache_t *cache, struct cgm_packet_t *message_packet){

	int ort_row = 0;

	int set = 0;
	int tag = 0;
	unsigned int offset = 0;

	int *set_ptr = &set;
	int *tag_ptr = &tag;
	unsigned int *offset_ptr = &offset;

	//charge delay
	GPU_PAUSE(cache->latency);

	//probe the address for set, tag, and offset.
	cgm_cache_probe_address(cache, message_packet->address, set_ptr, tag_ptr, offset_ptr);

	GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s load_nack ID %llu type %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, P_TIME);

	//store the decoded address
	message_packet->tag = tag;
	message_packet->set = set;
	message_packet->offset = offset;

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);

	//do not set retry because this contains the coalesce set and tag.
	//check that there is an ORT entry for this address
	ort_row = ort_search(cache, message_packet->tag, message_packet->set);
	assert(ort_row < cache->mshr_size);

	/*clear the pending bit in the ort and retry the access*/
	ort_clear_pending_join_bit(cache, ort_row, message_packet->tag, message_packet->set);

	//add some routing/status data to the packet
	message_packet->access_type = cgm_access_get;

	cache_put_io_down_queue(cache, message_packet);

	/*stats*/
	mem_system_stats->l1_load_nack++;

	return;
}

void cgm_mesi_gpu_l1_v_getx_inval(struct cache_t *cache, struct cgm_packet_t *message_packet){

	//Getx invalidation request from L2 cache

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	struct cgm_packet_t *wb_packet;
	int ort_status = -1;

	//enum cgm_cache_block_state_t victim_trainsient_state;

	//charge the delay
	GPU_PAUSE(cache->latency);

	//get the block status
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s getx_inval ID %llu type %d state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->evict_id,
			message_packet->access_type, *cache_block_state_ptr, P_TIME);

	//check the ORT table is there an outstanding access for this block we are trying to evict?
	ort_status = ort_search(cache, message_packet->tag, message_packet->set);
	if(ort_status != cache->mshr_size)
	{
		/*yep there is so set the bit in the ort table to 0.
		 * When the put/putx comes kill it and try again...*/
		ort_set_pending_join_bit(cache, ort_status, message_packet->tag, message_packet->set);

		//warning("l1 conflict found ort set cycle %llu\n", P_TIME);
	}

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);

	//victim_trainsient_state = cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->l1_victim_way);

	//first check the cache for the block
	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_owned:
		case cgm_cache_block_noncoherent:
		fatal("cgm_mesi_gpu_l1_v_flush_block(): Invalid block state on flush hit %s \n", str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr));
			break;

		//if invalid check the WB buffer
		case cgm_cache_block_invalid:

				wb_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

				//found the block in the WB buffer
				if(wb_packet)
				{
					//BUG HERE??? isn't it possible for this to be exclusive?
					assert(wb_packet->cache_block_state == cgm_cache_block_modified);

					message_packet->size = packet_set_size(cache->block_size);
					message_packet->cache_block_state = cgm_cache_block_modified;

					//remove the block from the WB buffer
					wb_packet = list_remove(cache->write_back_buffer, wb_packet);
					packet_destroy(wb_packet);

					//set access type inval_ack
					message_packet->l1_cache_id = cache->id;
					message_packet->access_type = cgm_access_getx_inval_ack;

					//reply to the L2 cache
					cache_put_io_down_queue(cache, message_packet);
				}
				else
				{

					/*Dropped if exclusive or shared
					 Also WB may be in the pipe between L1 and L2 if Modified This will flush it out*/
					message_packet->size = HEADER_SIZE;
					message_packet->cache_block_state = cgm_cache_block_invalid;

					//set access type inval_ack
					message_packet->l1_cache_id = cache->id;
					message_packet->access_type = cgm_access_getx_inval_ack;

					//reply to the L2 cache
					cache_put_io_down_queue(cache, message_packet);
				}

			break;

		case cgm_cache_block_exclusive:
		case cgm_cache_block_shared:

			message_packet->size = HEADER_SIZE;
			message_packet->cache_block_state = cgm_cache_block_invalid;
			message_packet->l1_cache_id = cache->id;

			//invalidate the local block
			cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_invalid);

			//set access type inval_ack
			message_packet->l1_cache_id = cache->id;
			message_packet->access_type = cgm_access_getx_inval_ack;

			//reply to the L2 cache
			cache_put_io_down_queue(cache, message_packet);
			break;

		case cgm_cache_block_modified:
			//hit and its dirty send the ack and block down (sharing writeback) to the L2 cache.
			message_packet->size = packet_set_size(cache->block_size);
			message_packet->cache_block_state = cgm_cache_block_modified;

			//invalidate the local block
			cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_invalid);

			//set access type inval_ack
			message_packet->l1_cache_id = cache->id;
			message_packet->access_type = cgm_access_getx_inval_ack;

			//reply to the L2 cache
			cache_put_io_down_queue(cache, message_packet);

			break;

	}

	return;
}

void cgm_mesi_gpu_l1_v_downgrade(struct cache_t *cache, struct cgm_packet_t *message_packet){

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;
	int ort_status = -1;

	//enum cgm_cache_block_state_t block_trainsient_state;

	struct cgm_packet_t *write_back_packet = NULL;

	//charge the delay
	GPU_PAUSE(cache->latency);

	//get the block status
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	//search the WB buffer for the data
	write_back_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

	//block_trainsient_state = cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->way);

	//check the ORT table is there an outstanding access for this block we are trying to evict?
	ort_status = ort_search(cache, message_packet->tag, message_packet->set);
	if(ort_status != cache->mshr_size)
	{

		//seems ok
		/*fatal("%s downgrade to pending request, just check me addr 0x%08x\n",
				cache->name, (message_packet->address & cache->block_address_mask));*/

		/*yep there is so set the bit in the ort table to 0.
		 * When the put/putx comes kill it and try again...*/
		ort_set_pending_join_bit(cache, ort_status, message_packet->tag, message_packet->set);

		//warning("l1 conflict found on downgrade ort set cycle %llu\n", P_TIME);
	}

	/*if((*cache_block_hit_ptr == 1 && block_trainsient_state == cgm_cache_block_transient))
		warning("bug is here block 0x%08x %s downgrade ID %llu type %d state %d cycle %llu\n",
					(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, *cache_block_state_ptr, P_TIME);*/


	//assert((*cache_block_hit_ptr == 1 && block_trainsient_state != cgm_cache_block_transient) || (*cache_block_hit_ptr == 0));

	GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s GPU downgrade ID %llu type %d state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->downgrade_id,
			message_packet->access_type, *cache_block_state_ptr, P_TIME);

	assert(message_packet->size == HEADER_SIZE);

	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_noncoherent:
		case cgm_cache_block_owned:
			cgm_cache_dump_set(cache, message_packet->set);

			fatal("cgm_mesi_gpu_v_downgrade(): L1 id %d invalid block state on downgrade as %s set %d wat %d tag %d blk_addr 0x%08x\n",
				cache->id, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr), message_packet->set, message_packet->way, message_packet->tag, message_packet->address & cache->block_address_mask);
			break;

		case cgm_cache_block_invalid:

			assert(*cache_block_hit_ptr == 0);

			//check write back buffer
			if(write_back_packet)
			{
				/*found the packet in the write back buffer
				data should not be in the rest of the cache*/
				assert(write_back_packet->cache_block_state == cgm_cache_block_modified || write_back_packet->cache_block_state == cgm_cache_block_exclusive);

				if(write_back_packet->cache_block_state == cgm_cache_block_modified)
				{
					message_packet->size = packet_set_size(cache->block_size);
					message_packet->cache_block_state = cgm_cache_block_modified;
				}
				else
				{
					message_packet->size = HEADER_SIZE;
				}

				//clear the WB from the buffer
				write_back_packet = list_remove(cache->write_back_buffer, write_back_packet);
				packet_destroy(write_back_packet);

				message_packet->l1_cache_id = cache->id;

				//set the access type
				message_packet->access_type = cgm_access_downgrade_ack;

				//reply to the L2 cache
				cache_put_io_down_queue(cache, message_packet);

			}
			else
			{
				//if invalid and not in write back it was silently dropped
				message_packet->size = HEADER_SIZE;
				message_packet->cache_block_state = cgm_cache_block_invalid;

				//set the access type
				message_packet->access_type = cgm_access_downgrade_ack;

				//reply to the L2 cache
				cache_put_io_down_queue(cache, message_packet);
			}

			break;

		case cgm_cache_block_exclusive:
		case cgm_cache_block_modified:

			assert(!write_back_packet);
			assert(*cache_block_hit_ptr == 1);

			if(*cache_block_state_ptr == cgm_cache_block_exclusive)
			{
				//if E it is not dirty
				message_packet->size = HEADER_SIZE;
				message_packet->cache_block_state = cgm_cache_block_invalid;
			}
			else if(*cache_block_state_ptr == cgm_cache_block_modified)
			{
				//hit and its dirty send the ack and block down (sharing write back) to the L2 cache.
				message_packet->size = packet_set_size(cache->block_size);
				message_packet->cache_block_state = cgm_cache_block_modified;
			}

			//set the access type
			message_packet->access_type = cgm_access_downgrade_ack;

			//downgrade the local block
			cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_shared);

			//reply to the L2 cache
			cache_put_io_down_queue(cache, message_packet);
			break;

		case cgm_cache_block_shared:

			fatal("cgm_mesi_gpu_l1_v_downgrade(): block found shared on downgrade. This shouldn't occur\n");

			break;
	}

	/*stats*/
	cache->TotalDowngrades++;

	return;
}

void cgm_mesi_gpu_l1_v_store_nack(struct cache_t *cache, struct cgm_packet_t *message_packet){

	int ort_row = 0;

	int set = 0;
	int tag = 0;
	unsigned int offset = 0;

	int *set_ptr = &set;
	int *tag_ptr = &tag;
	unsigned int *offset_ptr = &offset;

	//charge delay
	GPU_PAUSE(cache->latency);

	//probe the address for set, tag, and offset.
	cgm_cache_probe_address(cache, message_packet->address, set_ptr, tag_ptr, offset_ptr);

	GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s store_nack ID %llu type %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, P_TIME);

	//store the decoded address
	message_packet->tag = tag;
	message_packet->set = set;
	message_packet->offset = offset;

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);

	//do not set retry because this contains the coalesce set and tag.
	//check that there is an ORT entry for this address
	ort_row = ort_search(cache, message_packet->tag, message_packet->set);
	assert(ort_row < cache->mshr_size);

	/*clear the pending bit in the ort and retry the access*/
	ort_clear_pending_join_bit(cache, ort_row, message_packet->tag, message_packet->set);

	//add some routing/status data to the packet
	message_packet->access_type = cgm_access_getx;

	cache_put_io_down_queue(cache, message_packet);

	/*stats*/
	mem_system_stats->l1_store_nack++;

	return;
}


void cgm_mesi_gpu_l1_v_store(struct cache_t *cache, struct cgm_packet_t *message_packet){

	/*printf("gpu v %d storing\n", cache->id);*/
	/*STOP;*/

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	struct cgm_packet_t *write_back_packet = NULL;

	//get the status of the cache block
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	//search the WB buffer for the data
	write_back_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

	//check this...
	if(write_back_packet)
	{
		assert(*cache_block_hit_ptr == 0);
		assert(write_back_packet->cache_block_state == cgm_cache_block_modified || write_back_packet->cache_block_state == cgm_cache_block_exclusive);

	}

	//update cache way list for cache replacement policies.
	if(*cache_block_hit_ptr == 1)
	{
		//make this block the MRU
		cgm_cache_update_waylist(&cache->sets[message_packet->set], cache->sets[message_packet->set].way_tail, cache_waylist_head);
	}

	//charge delay
	GPU_PAUSE(cache->latency);

	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_owned:
		case cgm_cache_block_noncoherent:
			fatal("cgm_mesi_gpu_l1_v_store(): Invalid block state on store hit %s \n", str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr));
			break;

		//miss or invalid cache block state
		case cgm_cache_block_invalid:

			//lock in write back for the cache line.
			if(write_back_packet)
			{
				/*found the packet in the write back buffer
				data should not be in the rest of the cache*/
				assert(*cache_block_hit_ptr == 0);
				assert(write_back_packet->cache_block_state == cgm_cache_block_modified || write_back_packet->cache_block_state == cgm_cache_block_exclusive);
				assert(message_packet->set == write_back_packet->set && message_packet->tag == write_back_packet->tag);
				assert(message_packet->access_type == cgm_access_store || message_packet->access_type == cgm_access_store_retry);

				//see if we can write it back into the cache.
				write_back_packet->l1_victim_way = cgm_cache_get_victim_for_wb(cache, write_back_packet->set);

				//if not then we must coalesce
				if(write_back_packet->l1_victim_way == -1)
				{
					//Set and ways are all transient must coalesce
					cache_check_ORT(cache, message_packet);

					/*stats*/
					cache->CoalescePut++;

					if(message_packet->coalesced == 1)
						return;
					else
						fatal("cgm_mesi_gpu_load(): write failed to coalesce when all ways are transient...\n");
				}

				//we are writing in a block so evict the victim
				assert(write_back_packet->l1_victim_way >= 0 && write_back_packet->l1_victim_way < cache->assoc);

				//first evict the old block if it isn't invalid already
				if(cgm_cache_get_block_state(cache, write_back_packet->set, write_back_packet->l1_victim_way) != cgm_cache_block_invalid)
					cgm_L1_cache_evict_block(cache, write_back_packet->set, write_back_packet->l1_victim_way);

				cgm_cache_set_block(cache, write_back_packet->set, write_back_packet->l1_victim_way, write_back_packet->tag, cgm_cache_block_modified);

				//free the write back
				write_back_packet = list_remove(cache->write_back_buffer, write_back_packet);
				packet_destroy(write_back_packet);

				GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s store wb hit id %llu state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, *cache_block_state_ptr, P_TIME);

				//check for retires on successful cache write...
				if(message_packet->access_type == cgm_access_store_retry || message_packet->coalesced == 1)
				{
					cache->MergeRetries++;

					//enter retry state.
					cache_coalesed_retry(cache, message_packet->tag, message_packet->set, message_packet->access_id);
				}

				/*stats*/
				cache->WbMerges++;
				message_packet->end_cycle = P_TIME;
				if(!message_packet->protocol_case)
					message_packet->protocol_case = L1_hit;

				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;

				cache_gpu_v_return(cache,message_packet);
				return;
			}
			else
			{
				//check ORT for coalesce
				cache_check_ORT(cache, message_packet);

				if(message_packet->coalesced == 1)
				{
					/*stats*/
					cache->CoalescePut++;

					GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s store miss coalesce ID %llu type %d state %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type,
							*cache_block_state_ptr, P_TIME);

					return;
				}

				//add some routing/status data to the packet
				message_packet->access_type = cgm_access_getx;
				message_packet->l1_access_type = cgm_access_getx;
				message_packet->l1_cache_id = cache->id;

				//find victim
				message_packet->l1_victim_way = cgm_cache_get_victim(cache, message_packet->set, message_packet->tag);
				assert(cache->sets[message_packet->set].blocks[message_packet->l1_victim_way].directory_entry.entry_bits.pending != 1);

				/*message_packet->l1_victim_way = cgm_cache_replace_block(cache, message_packet->set);*/
				assert(message_packet->l1_victim_way >= 0 && message_packet->l1_victim_way < cache->assoc);

				//evict the block if the data is valid
				if(cgm_cache_get_block_state(cache, message_packet->set, message_packet->l1_victim_way) != cgm_cache_block_invalid)
					cgm_L1_cache_evict_block(cache, message_packet->set, message_packet->l1_victim_way);

				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;

				//transmit to L2
				cache_put_io_down_queue(cache, message_packet);

				GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s store miss ID %llu type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
						message_packet->access_type, *cache_block_state_ptr, P_TIME);

			}

			break;

		case cgm_cache_block_shared:

			//this is an upgrade miss. However we assume the GPU dosn't perform upgrades
			//invalidate the line and send a Getx to L2

			/*star todo find a better way to do this.
			this is for a special case where a coalesced store
			can be pulled from the ORT and is an upgrade miss here
			at this point we want the access to be treated as a new miss
			so set coalesced to 0. Older packets in the ORT will stay in the ORT
			preserving order until the missing access returns with the upgrade.*/
			if(message_packet->coalesced == 1)
			{
				message_packet->coalesced = 0;

				/*warning("block 0x%08x %s coalesced store to a shared line ID %llu type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type,
						*cache_block_state_ptr, P_TIME);*/
			}

			//first check/update ORT for coalesce because this is a miss
			cache_check_ORT(cache, message_packet);

			if(message_packet->coalesced == 1)
			{
				/*stats*/
				cache->CoalescePut++;

				GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s upgrade miss store miss to shared line!! coalesce ID %llu type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type,
						*cache_block_state_ptr, P_TIME);

				return;
			}

			//Ok, so this is a new miss for the line
			//invalidate the line in the L1 cache
			cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_invalid);

			//set block transient state, process the upgraded but as a GetX
			cgm_cache_set_block_transient_state(cache, message_packet->set, message_packet->way, cgm_cache_block_transient);

			//keep the way of the block to upgrade (for debugging)
			message_packet->l1_victim_way = message_packet->way;

			message_packet->access_type = cgm_access_getx;
			message_packet->l1_access_type = cgm_access_getx;
			message_packet->l1_cache_id = cache->id;

			/*reset mp flags*/
			message_packet->coalesced = 0;
			message_packet->assoc_conflict = 0;

			//transmit upgrade request to L2
			cache_put_io_down_queue(cache, message_packet);

			GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s store missed on shared line sending GetX ID %llu type %d state %d cycle %llu\n",
					(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
					message_packet->access_type, *cache_block_state_ptr, P_TIME);

			/*if(message_packet->access_id ==  1422704)
				warning("Caught id %llu\n", message_packet->access_id);*/


			break;

		case cgm_cache_block_exclusive:
		case cgm_cache_block_modified:

			//set modified if current block state is exclusive
			if(*cache_block_state_ptr == cgm_cache_block_exclusive)
			{
				cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_modified);
			}

			//check for retry state
			if(message_packet->access_type == cgm_access_store_retry || message_packet->coalesced == 1)
			{
				//enter retry state.
				cache_coalesed_retry(cache, message_packet->tag, message_packet->set, message_packet->access_id);
			}

			GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s store hit ID %llu type %d state %d cycle %llu\n",
					(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
					message_packet->access_type, *cache_block_state_ptr, P_TIME);

			/*stats*/
			message_packet->end_cycle = P_TIME;
			if(!message_packet->protocol_case)
					message_packet->protocol_case = L1_hit;

			/*reset mp flags*/
			message_packet->coalesced = 0;
			message_packet->assoc_conflict = 0;

			cache_gpu_v_return(cache,message_packet);

			break;
	}

	return;
}



void cgm_mesi_gpu_l1_v_write_back(struct cache_t *cache, struct cgm_packet_t *message_packet){


	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;
	//struct cgm_packet_t *wb_packet;
	//enum cgm_cache_block_state_t block_trainsient_state;
	//int l3_map;
	//int error = 0;

	//charge the delay
	GPU_PAUSE(cache->latency);

	//we should only receive modified lines from L1 D cache
	assert(message_packet->cache_block_state == cgm_cache_block_modified);

	//get the state of the local cache block
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	//check for block transient state
	/*block_trainsient_state = cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->way);
	if(block_trainsient_state == cgm_cache_block_transient)
	{
		if potentially merging in cache the block better not be transient, check that the tags don't match
		if they don't match the block is missing from both the cache and wb buffer when it should not be

		//check that the tags don't match. This should not happen as the request should have been coalesced at L1 D.
		assert(message_packet->tag != cache->sets[message_packet->set].blocks[message_packet->way].tag);
	}*/

	GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s L1 wb sent id %llu state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->write_back_id,
			message_packet->cache_block_state, P_TIME);

	/*better not be in my cache*/
	assert(*cache_block_hit_ptr == 0);
	assert(message_packet->flush_pending == 0 && message_packet->L3_flush_join == 0);

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);

	/*stats*/
	cache->TotalWriteBackSent++;

	message_packet->size = packet_set_size(cache->block_size);

	cache_put_io_down_queue(cache, message_packet);

	return;
}

void cgm_mesi_gpu_l1_v_get_getx_fwd_inval(struct cache_t *cache, struct cgm_packet_t *message_packet){

	/*warning("l1 get getx inval\n");*/
	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	struct cgm_packet_t *write_back_packet = NULL;

	int ort_status = -1;

	//charge delay
	GPU_PAUSE(cache->latency);

	//get the block status
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	//search the WB buffer for the data
	write_back_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

	GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s getx fwd inval ID %llu type %d state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
			message_packet->access_type, *cache_block_state_ptr, P_TIME);

	//check the ORT table is there an outstanding access for this block we are trying to evict?
	ort_status = ort_search(cache, message_packet->tag, message_packet->set);
	if(ort_status != cache->mshr_size)
	{
		fatal("cgm_mesi_gpu_l1_v_get_getx_fwd_inval(): shoudn't be pending\n");

		/*fatal("here\n");*/
		/*yep there is so set the bit in the ort table to 0.
		 * When the put/putx comes kill it and try again...*/
		ort_set_pending_join_bit(cache, ort_status, message_packet->tag, message_packet->set);

		//warning("l1 conflict found ort set cycle %llu\n", P_TIME);
	}

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);


	/*if the block is in the cache it is not in the WB buffer
	if the block is dirty send down to L2 cache for merge*/
	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_owned:
		case cgm_cache_block_noncoherent:
		fatal("cgm_mesi_gpu_l1_v_get_getx_fwd_inval): block 0x%08x Invalid block state on flush hit %s ID %llu cycle %llu\n",
				(message_packet->address & cache->block_address_mask), str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr),
				message_packet->access_id, P_TIME);

			break;

		case cgm_cache_block_invalid:

			//check write back buffer
			if(write_back_packet)
			{
				/*found the packet in the write back buffer
				data should not be in the rest of the cache*/
				assert((write_back_packet->cache_block_state == cgm_cache_block_modified
						|| write_back_packet->cache_block_state == cgm_cache_block_exclusive)
						&& *cache_block_state_ptr == cgm_cache_block_invalid);

				if(write_back_packet->cache_block_state == cgm_cache_block_modified)
				{
					message_packet->size = packet_set_size(cache->block_size);
					message_packet->cache_block_state = cgm_cache_block_modified;
				}

				//clear the wb buffer
				write_back_packet = list_remove(cache->write_back_buffer, write_back_packet);
				packet_destroy(write_back_packet);

				message_packet->size = HEADER_SIZE;
				message_packet->cache_block_state = cgm_cache_block_invalid;

				//set access type
				message_packet->l1_cache_id = cache->id;
				message_packet->access_type = cgm_access_getx_fwd_inval_ack;

				//reply to the L2 cache
				cache_put_io_down_queue(cache, message_packet);

			}
			else
			{
				//if invalid it was silently dropped
				message_packet->size = HEADER_SIZE;
				message_packet->cache_block_state = cgm_cache_block_invalid;

				//set access type
				message_packet->l1_cache_id = cache->id;
				message_packet->access_type = cgm_access_getx_fwd_inval_ack;

				//reply to the L2 cache
				cache_put_io_down_queue(cache, message_packet);
			}
			break;

		case cgm_cache_block_exclusive:
		case cgm_cache_block_modified:
		case cgm_cache_block_shared:

			assert(*cache_block_hit_ptr == 1);

			if(*cache_block_state_ptr == cgm_cache_block_exclusive
					|| *cache_block_state_ptr == cgm_cache_block_shared)
			{
				//if E it is not dirty
				message_packet->size = HEADER_SIZE;
				message_packet->cache_block_state = cgm_cache_block_invalid;
			}
			else if(*cache_block_state_ptr == cgm_cache_block_modified)
			{
				//hit and its dirty send the ack and block down (sharing write back) to the L2 cache.
				message_packet->size = packet_set_size(cache->block_size);
				message_packet->cache_block_state = cgm_cache_block_modified;
			}

			//invalidate the local block
			cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_invalid);

			//set access type
			message_packet->l1_cache_id = cache->id;
			message_packet->access_type = cgm_access_getx_fwd_inval_ack;

			//reply to the L2 cache
			cache_put_io_down_queue(cache, message_packet);

			break;
	}

	/*stats*/
	cache->TotalGetxFwdInvals++;

	return;
}

void cgm_mesi_gpu_l1_v_gpu_flush(struct cache_t *cache, struct cgm_packet_t *message_packet){

	//CPU is flushing a block, flush and send down to L2 and L3/MC
	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	struct cgm_packet_t *wb_packet;
	int ort_status = -1;

	//enum cgm_cache_block_state_t victim_trainsient_state;

	//charge the delay
	GPU_PAUSE(cache->latency);

	//get the block status
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s GPU flush-flush block ID %llu type %d state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->evict_id,
			message_packet->access_type, *cache_block_state_ptr, P_TIME);

	//check the ORT table is there an outstanding access for this block we are trying to flush?
	ort_status = ort_search(cache, message_packet->tag, message_packet->set);
	if(ort_status != cache->mshr_size)
	{
		fatal("cgm_mesi_gpu_l1_v_gpu_flush(): ort conflict set\n");
		/*CPU is flushing blocks, but we are still waiting on the memory system to bring the block in
		the CPU is now trying to flush, stall until the block is brought and the progress is made*/
		//fatal("not sure about this\n");
	}

	//remove the block from the cache....
	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_owned:
		case cgm_cache_block_noncoherent:
		fatal("cgm_mesi_gpu_l1_v_gpu_flush(): Invalid block state on flush hit %s \n", str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr));
			break;

		//if invalid check the WB buffer
		case cgm_cache_block_invalid:

				wb_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

				//found the block in the WB buffer
				if(wb_packet)
				{
					assert(wb_packet->cache_block_state == cgm_cache_block_modified);

					message_packet->size = packet_set_size(cache->block_size);
					message_packet->cache_block_state = cgm_cache_block_modified;

					//remove the block from the WB buffer
					wb_packet = list_remove(cache->write_back_buffer, wb_packet);
					packet_destroy(wb_packet);

					//set access type inval_ack
					message_packet->access_type = cgm_access_gpu_flush_ack;

					cache_put_io_down_queue(cache, message_packet);
				}
				else
				{

					/*Dropped if exclusive or shared
					 Also WB may be in the pipe between L1 and L2 if Modified This will flush it out*/
					message_packet->size = HEADER_SIZE;
					message_packet->cache_block_state = cgm_cache_block_invalid;

					//set access type inval_ack
					message_packet->access_type = cgm_access_gpu_flush_ack;

					//reply to the L2 cache
					cache_put_io_down_queue(cache, message_packet);
				}

			break;

		case cgm_cache_block_exclusive:

			message_packet->size = HEADER_SIZE;
			message_packet->cache_block_state = cgm_cache_block_invalid;

			//invalidate the local block
			cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_invalid);

			//set access type inval_ack
			message_packet->access_type = cgm_access_gpu_flush_ack;

			//reply to the L2 cache
			cache_put_io_down_queue(cache, message_packet);

			break;

		case cgm_cache_block_modified:

			//hit and its dirty send the ack and block down to the L2 cache.
			message_packet->size = packet_set_size(cache->block_size);
			message_packet->cache_block_state = cgm_cache_block_modified;

			//invalidate the local block
			cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_invalid);

			//set access type inval_ack
			message_packet->access_type = cgm_access_gpu_flush_ack;

			//reply to the L2 cache
			cache_put_io_down_queue(cache, message_packet);

			break;

		case cgm_cache_block_shared:

			fatal("cgm_mesi_gpu_l1_v_gpu_flush(): shared in l1\n");

			break;

	}

	return;
}


void cgm_mesi_gpu_l1_v_flush_block(struct cache_t *cache, struct cgm_packet_t *message_packet){

	//Invalidation/eviction request from L2 cache

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	struct cgm_packet_t *wb_packet;
	int ort_status = -1;

	//enum cgm_cache_block_state_t victim_trainsient_state;

	//charge the delay
	GPU_PAUSE(cache->latency);

	//get the block status
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s GPU flush block (L1) ID %llu type %d state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->evict_id,
			message_packet->access_type, *cache_block_state_ptr, P_TIME);

	//check the ORT table is there an outstanding access for this block we are trying to evict?
	ort_status = ort_search(cache, message_packet->tag, message_packet->set);
	if(ort_status != cache->mshr_size)
	{
		/*yep there is so set the bit in the ort table to 0.
		 * When the put/putx comes kill it and try again...*/
		ort_set_pending_join_bit(cache, ort_status, message_packet->tag, message_packet->set);

		//warning("l1 conflict found ort set cycle %llu\n", P_TIME);
	}

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);

	//victim_trainsient_state = cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->l1_victim_way);

	//first check the cache for the block
	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_owned:
		case cgm_cache_block_noncoherent:
		fatal("cgm_mesi_gpu_l1_v_flush_block(): Invalid block state on flush hit %s \n", str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr));
			break;

		//if invalid check the WB buffer
		case cgm_cache_block_invalid:

				wb_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

				//found the block in the WB buffer
				if(wb_packet)
				{
					assert(wb_packet->cache_block_state == cgm_cache_block_modified);

					message_packet->size = packet_set_size(cache->block_size);
					message_packet->cache_block_state = cgm_cache_block_modified;

					//remove the block from the WB buffer
					wb_packet = list_remove(cache->write_back_buffer, wb_packet);
					packet_destroy(wb_packet);

					//set access type inval_ack
					message_packet->access_type = cgm_access_flush_block_ack;

					//reply to the L2 cache
					cache_put_io_down_queue(cache, message_packet);
				}
				else
				{

					//line is invalid and not in WB if shared at L2 do nothing.
					if(message_packet->cache_block_state == cgm_cache_block_shared)
					{
						message_packet = list_remove(cache->last_queue, message_packet);
						packet_destroy(message_packet);
					}
					else
					{

						assert(message_packet->cache_block_state == cgm_cache_block_modified
							|| message_packet->cache_block_state == cgm_cache_block_exclusive);

						/*Dropped if exclusive or shared
						 Also WB may be in the pipe between L1 and L2 if Modified This will flush it out*/
						message_packet->size = HEADER_SIZE;
						message_packet->cache_block_state = cgm_cache_block_invalid;

						//set access type inval_ack
						message_packet->access_type = cgm_access_flush_block_ack;

						//reply to the L2 cache
						cache_put_io_down_queue(cache, message_packet);
					}
				}

			break;

		case cgm_cache_block_exclusive:

			message_packet->size = HEADER_SIZE;
			message_packet->cache_block_state = cgm_cache_block_invalid;

			//invalidate the local block
			cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_invalid);

			//set access type inval_ack
			message_packet->access_type = cgm_access_flush_block_ack;

			//reply to the L2 cache
			cache_put_io_down_queue(cache, message_packet);
			break;

		case cgm_cache_block_modified:
			//hit and its dirty send the ack and block down (sharing writeback) to the L2 cache.
			message_packet->size = packet_set_size(cache->block_size);
			message_packet->cache_block_state = cgm_cache_block_modified;

			//invalidate the local block
			cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_invalid);

			//set access type inval_ack
			message_packet->access_type = cgm_access_flush_block_ack;

			//reply to the L2 cache
			cache_put_io_down_queue(cache, message_packet);

			break;

		case cgm_cache_block_shared:

			//invalidate the local block
			cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_invalid);

			message_packet = list_remove(cache->last_queue, message_packet);
			packet_destroy(message_packet);

			break;
	}

	/*stats*/
	cache->EvictInv++;

	return;
}


int cgm_mesi_gpu_l1_v_write_block(struct cache_t *cache, struct cgm_packet_t *message_packet){

	assert(cache->cache_type == gpu_v_cache_t);
	assert((message_packet->access_type == cgm_access_puts && message_packet->cache_block_state == cgm_cache_block_shared)
			|| (message_packet->access_type == cgm_access_put_clnx && message_packet->cache_block_state == cgm_cache_block_exclusive)
			|| (message_packet->access_type == cgm_access_putx && message_packet->cache_block_state == cgm_cache_block_modified));

	/*if(message_packet->access_id == 15509891)
		fatal("%s write block after flush cycle %llu\n", cache->name, P_TIME);*/


	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;
	enum cgm_cache_block_state_t victim_trainsient_state;

	int ort_status = -1;
	int ort_join_bit = -1;

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);

	cache_get_transient_block(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	if(*cache_block_hit_ptr != 1)
	{
		cgm_cache_dump_set(cache, message_packet->set);

		fatal("block 0x%08x %s write block didn't find transient block! ID %llu type %s state %d way %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
			str_map_value(&cgm_mem_access_strn_map, message_packet->access_type), message_packet->way, message_packet->cache_block_state, P_TIME);
		fflush(stderr);
	}

	if(message_packet->size != 72)
		fatal("block 0x%08x %s bad size ID %llu type %s state %d way %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
			str_map_value(&cgm_mem_access_strn_map, message_packet->access_type), message_packet->way, message_packet->cache_block_state, P_TIME);

	//assert(message_packet->size == 72);

	ort_status = ort_search(cache, message_packet->tag, message_packet->set);
	assert(ort_status < cache->mshr_size);

	//The block should be in the transient state.
	victim_trainsient_state = cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->l1_victim_way);

	//make sure victim way was correctly stored.
	assert(message_packet->l1_victim_way >= 0 && message_packet->l1_victim_way < cache->assoc);

	if(victim_trainsient_state != cgm_cache_block_transient)
	{
		cgm_cache_dump_set(cache, message_packet->set);

		unsigned int temp = message_packet->address;
		temp = temp & cache->block_address_mask;

		fatal("cgm_mesi_gpu_l1_v_write_block(): %s access_id %llu address 0x%08x blk_addr 0x%08x set %d tag %d way %d cycle %llu\n",
			cache->name, message_packet->access_id, message_packet->address, temp, message_packet->set, message_packet->tag, message_packet->l1_victim_way, P_TIME);
	}

	assert(victim_trainsient_state == cgm_cache_block_transient);

	/*special case check for a join
	if so kill this write block and retry the get/getx*/
	ort_join_bit = ort_get_pending_join_bit(cache, ort_status, message_packet->tag, message_packet->set);
	assert(ort_join_bit == 1 || ort_join_bit == 0);

	if(ort_join_bit == 0)
	{

		GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s write block conflict found retrying access ID %llu type %d state %d cycle %llu\n",
				(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type,
				message_packet->cache_block_state, P_TIME);

		/*we know that the victim is still transient*/

		/*clear the pending bit in the ort and retry the access*/
		ort_clear_pending_join_bit(cache, ort_status, message_packet->tag, message_packet->set);

		/*change the access type*/
		if(message_packet->l1_access_type == cgm_access_get)
		{
			message_packet->access_type = cgm_access_get;
		}
		else
		{

			//assert(message_packet->l1_access_type == cgm_access_getx || message_packet->l1_access_type == cgm_access_upgrade_ack);
			message_packet->access_type = cgm_access_getx;
		}

		if(message_packet->coalesced == 1)
			message_packet->coalesced = 0;


		/*change the size*/
		message_packet->size = HEADER_SIZE;

		//transmit to L2
		cache_put_io_down_queue(cache, message_packet);

		//warning("l1 write block caught conflict cycle %llu\n", P_TIME);

		return 0;
	}


	/*we are clear to write the block in*/
	ort_clear(cache, message_packet);


	GPUDEBUG(LEVEL == 1 || LEVEL == 3, "block 0x%08x %s write block ID %llu type %d state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
			message_packet->access_type, message_packet->cache_block_state, P_TIME);


	/*if((message_packet->address & cache->block_address_mask) == 0x08123380)
	warning("GPU L1 $ id %d write block %llu phy address 0x%08x state %d\n",
				cache->id, message_packet->access_id,
				(message_packet->address & cache->block_address_mask),
				message_packet->cache_block_state);
	fflush(stderr);*/

	//write the block
	cgm_cache_set_block(cache, message_packet->set, message_packet->l1_victim_way, message_packet->tag, message_packet->cache_block_state);

	//set retry state
	message_packet->access_type = cgm_cache_get_retry_state(message_packet->gpu_access_type);

	message_packet = list_remove(cache->last_queue, message_packet);
	list_enqueue(cache->retry_queue, message_packet);

	/*stats*/
	cache->TotalWriteBlocks++;

	return 1;
}


void cgm_mesi_gpu_l2_get_nack(struct cache_t *cache, struct cgm_packet_t *message_packet){

	int ort_row = 0;
	int conflict_bit = -1;

	int set = 0;
	int tag = 0;
	unsigned int offset = 0;
	/*int way = 0;*/
	//int l3_map = 0;

	int *set_ptr = &set;
	int *tag_ptr = &tag;
	unsigned int *offset_ptr = &offset;
	/*int *way_ptr = &way;*/

	/*should only occur if connected to l3*/
	assert(hub_iommu_connection_type == hub_to_l3);

	//charge delay
	GPU_PAUSE(cache->latency);

	//probe the address for set, tag, and offset.
	cgm_cache_probe_address(cache, message_packet->address, set_ptr, tag_ptr, offset_ptr);

	GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s get_nack ID %llu type %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
			message_packet->access_type, P_TIME);

	//store the decoded address
	message_packet->tag = tag;
	message_packet->set = set;
	message_packet->offset = offset;

	//do not set retry because this contains the coalesce set and tag.
	//check that there is an ORT entry for this address
	ort_row = ort_search(cache, message_packet->tag, message_packet->set);
	assert(ort_row < cache->mshr_size);
	conflict_bit = cache->ort[ort_row][2];


	//if the conflict bit is set in the ort reset it because this is the nack
	if(conflict_bit == 0)
	{
		cache->ort[ort_row][2] = 1;

		GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s get_nack caught the conflict bit in the ORT table ID %llu type %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
			message_packet->access_type, P_TIME);
	}


	message_packet->access_type = cgm_access_get;
	message_packet->cpu_access_type = cgm_access_load;
	message_packet->cache_block_state = cgm_cache_block_exclusive;
	message_packet->size = HEADER_SIZE;

	/*reset mp flags*/
	message_packet->coalesced = 0;
	message_packet->assoc_conflict = 0;

	//L3 should see the entire GPU as a single core.
	message_packet->l2_cache_id = gpu_core_id;
	message_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

	//transmit to L3
	cache_put_io_down_queue(cache, message_packet);


	return;
}



void cgm_mesi_gpu_l2_get(struct cache_t *cache, struct cgm_packet_t *message_packet){

	int ort_row = -1;

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	struct cgm_packet_t *write_back_packet = NULL;
	struct cgm_packet_t *pending_join = NULL;

	int sharers = 0;
	int owning_core = 0;
	int pending_bit = 0;// dirty_in_core;

	int num_cus = si_gpu_num_compute_units;

	struct cgm_packet_t *downgrade_packet = NULL;

	//charge delay
	GPU_PAUSE(cache->latency);

	//get the status of the cache block
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	//search the WB buffer for the data
	write_back_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

	//get number of sharers
	sharers = cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way);
	//check to see if access is from an already owning core
	owning_core = cgm_cache_is_owning_core(cache, message_packet->set, message_packet->way, message_packet->l1_cache_id);
	//check pending state
	pending_bit = cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way);
	//get block transient state
	//block_trainsient_state = cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->way);
	//assert(victim_trainsient_state == cgm_cache_block_transient);
	//dirty_in_core = cgm_cache_get_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);

	/*assumption block is never shared in GPU v caches*/
	//if(sharers > 1)
	//	fatal("cgm_mesi_gpu_l2_get(): sharers = %d\n", sharers);

	//update cache way list for cache replacement policies.
	if(*cache_block_hit_ptr == 1)
	{
		//make this block the MRU
		cgm_cache_update_waylist(&cache->sets[message_packet->set], cache->sets[message_packet->set].way_tail, cache_waylist_head);
	}

	if(pending_bit == 1 && *cache_block_hit_ptr == 1)
	{
		/*its possible for a get to come in from an owing core to and for the block to be pending
		this occurs if the owning core silently dropped the block and a get_fwd was processed
		just before the owning core's request comes in send the block back to the owning core
		a previously sent get_fwd will be joined at the owning core with this put/putx*/
		/*if(owning_core == 1)
		{

			assert(message_packet->coalesced == 0);

			GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s pending at L2 owning core PUT back to L2 id %llu state %d cycle %llu\n",
				(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, *cache_block_state_ptr, P_TIME);

			there should be only be the owning core in the directory and the pending bit set
			assert(sharers == 1 && owning_core == 1);

			The block MUST be in the E or M state
			assert(*cache_block_state_ptr == cgm_cache_block_exclusive || *cache_block_state_ptr == cgm_cache_block_modified);

			//update message status
			if(*cache_block_state_ptr == cgm_cache_block_exclusive)
			{
				message_packet->access_type = cgm_access_put_clnx;
			}
			else if(*cache_block_state_ptr == cgm_cache_block_modified)
			{
				message_packet->access_type = cgm_access_putx;
			}

			//get the cache block state
			message_packet->cache_block_state = *cache_block_state_ptr;

			//set message package size
			message_packet->size = packet_set_size(gpu_v_caches[message_packet->l1_cache_id].block_size);

			//don't change the directory entries, the downgrade ack will come back and clean things up.

			reset mp flags
			assert(message_packet->coalesced == 0);
			assert(message_packet->assoc_conflict == 0);

			//send the cache block out
			cache_put_io_up_queue(cache, message_packet);

			stats
			if(!message_packet->protocol_case)
				message_packet->protocol_case = L2_hit;

		}
		else
		{*/

			GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s pending at L2 get nacked back to L1 %d id %llu state %d cycle %llu\n",
				(message_packet->address & cache->block_address_mask), cache->name, message_packet->l1_cache_id, message_packet->access_id, *cache_block_state_ptr, P_TIME);

			/*Thrid party is looking for access to the block, but it is busy nack and let node retry later*/


			//check if the packet has coalesced accesses.
			if(message_packet->access_type == cgm_access_load_retry || message_packet->coalesced == 1)
			{
				//enter retry state.
				cache_coalesed_retry(cache, message_packet->tag, message_packet->set, message_packet->access_id);

				/*if(message_packet->access_id == 71992322)
					warning("l3 checking retries ID %llu\n", message_packet->access_id);*/
			}

			/*star todo find a better way to do this.
			this is for a special case where a coalesced access was pulled
			and is going to be nacked at this point we want the access to be
			treated as a new miss so set coalesced to 0*/
			if(message_packet->coalesced == 1)
			{
				message_packet->coalesced = 0;
			}


			//send the reply up as a NACK!
			message_packet->access_type = cgm_access_get_nack;

			//set message package size
			message_packet->size = HEADER_SIZE;

			/*reset mp flags*/
			assert(message_packet->coalesced == 0);
			assert(message_packet->assoc_conflict == 0);

			//send the reply
			cache_put_io_up_queue(cache, message_packet);

			/*stats*/
			mem_system_stats->l2_load_nack++;
		/*}*/

		return;

	}

	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_noncoherent:
		case cgm_cache_block_owned:
			fatal("cgm_mesi_l2_get(): L2 id %d Invalid block state on get as %s cycle %llu\n",
					cache->id, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr), P_TIME);
			break;

		case cgm_cache_block_invalid:

			//ZMG block is in the write back !!!!
			if(write_back_packet)
			{
				/*found the packet in the write back buffer
				data should not be in the rest of the cache*/
				assert(*cache_block_hit_ptr == 0);
				assert(write_back_packet->cache_block_state == cgm_cache_block_modified || write_back_packet->cache_block_state == cgm_cache_block_exclusive);
				assert(message_packet->set == write_back_packet->set && message_packet->tag == write_back_packet->tag);
				assert(message_packet->access_type == cgm_access_get || message_packet->access_type == cgm_access_load_retry);

				//see if we can write it back into the cache.
				write_back_packet->l2_victim_way = cgm_cache_get_victim_for_wb(cache, write_back_packet->set);

				/*special case nack the request back to L1 because L2 is still waiting on a a flush block ack*/
				if(write_back_packet->flush_pending == 1)
				{
					/*flush is pending, but we have a request for the block, nack it back to L1*/
					//send the reply up as a NACK!
					message_packet->access_type = cgm_access_get_nack;

					//set message package size
					message_packet->size = HEADER_SIZE;

					/*stats*/
					/*star todo add a stat for this*/

					GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s load nack wb pending flush ID %llu type %d state %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type,
							*cache_block_state_ptr, P_TIME);

					/*reset mp flags*/
					message_packet->coalesced = 0;
					message_packet->assoc_conflict = 0;

					cache_put_io_up_queue(cache, message_packet);

					//warning("nacking load back to L1, flush still pending\n");

					return;
				}


				//if not then we must coalesce
				if(write_back_packet->l2_victim_way == -1)
				{
					//Set and ways are all transient must coalesce
					cache_check_ORT(cache, message_packet);

					/*stats*/
					cache->CoalescePut++;

					if(message_packet->coalesced == 1)
					{
						return;
					}
					else
					{

						/*rare case here
						 * this only happens in caches that have a directory
						 * AND have LOTS of upper level clients
						 * nack the request back to requester and flush the WB out of the L2 cache*/

						/*FIXME*/
						/*this is a temporary fix and needs a better solution we should never really get into this state.*/

						warning("crazy train start get! block 0x%08x %s ID %llu type %d state %d cycle %llu\n",
								(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type,
								*cache_block_state_ptr, P_TIME);

						//clear the ORT entry that we just set sigh...
						ort_clear(cache, message_packet);

						/*ok we can't coalesce so we have to evict the write back and nack the request'*/

						/*flush is pending, but we have a request for the block, nack it back to L1*/
						//send the reply up as a NACK!
						message_packet->access_type = cgm_access_get_nack;

						//set message package size
						message_packet->size = HEADER_SIZE;

						/*stats*/
						/*star todo add a stat for this*/

						GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s load nack wb pending flush ID %llu type %d state %d cycle %llu\n",
								(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type,
								*cache_block_state_ptr, P_TIME);

						/*reset mp flags*/
						message_packet->coalesced = 0;
						message_packet->assoc_conflict = 0;

						cache_put_io_up_queue(cache, message_packet);

						//send the writeback on to main memory


						//verify that the block is out of L1
						int error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, write_back_packet->address);
						assert(error == 0);

						//verify that there is only one wb in L2 for this block.
						error = cache_search_wb_dup_packets(cache, write_back_packet->tag, write_back_packet->set);
						assert(error == 1); //error == 1 i.e only one wb packet and we are about to send it.

						if(write_back_packet->cache_block_state == cgm_cache_block_exclusive)
						{

							//GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s write back destroy ID %llu type %d cycle %llu\n",
							//		(message_packet->address & cache->block_address_mask), cache->name, message_packet->write_back_id, message_packet->access_type, P_TIME);


							/*stats*/
							cache->TotalWriteBackDropped++;

							write_back_packet = list_remove(cache->write_back_buffer, write_back_packet);
							packet_destroy(write_back_packet);
						}
						else
						{

							assert(write_back_packet->cache_block_state == cgm_cache_block_modified);

							//GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s write back sent (to L3) %llu type %d cycle %llu\n",
							//		(message_packet->address & cache->block_address_mask), cache->name, message_packet->write_back_id, message_packet->access_type, P_TIME);

							write_back_packet->size = packet_set_size(cache->block_size);

							if(hub_iommu_connection_type == hub_to_mc)
							{
								//message is going down to mc so its and mc_store
								write_back_packet->access_type = cgm_access_mc_store;

								write_back_packet->l2_cache_id = cache->id;
								write_back_packet->l2_cache_name = cache->name;

								SETROUTE(write_back_packet, cache, system_agent)

							}
							else
							{

								struct cache_t *l3_cache_ptr = NULL;

								l3_cache_ptr = cgm_l3_cache_map(write_back_packet->set);
								write_back_packet->l2_cache_id = cache->id;
								write_back_packet->l2_cache_name = cache->name;

								SETROUTE(write_back_packet, cache, l3_cache_ptr)
							}

							//send the write back on.
							write_back_packet = list_remove(cache->write_back_buffer, write_back_packet);
							assert(write_back_packet);

							list_enqueue(cache->Tx_queue_bottom, write_back_packet);
							advance(cache->cache_io_down_ec);

						}


						return;
					}
				}


				//if no conflicts merge the write back into the cache and process as a hit.
				assert(write_back_packet->L3_flush_join == 0);
				assert(write_back_packet->flush_pending == 0);

				//we are bringing a new block so evict the victim and flush the L1 copies
				assert(write_back_packet->l2_victim_way >= 0 && write_back_packet->l2_victim_way < cache->assoc);

				//first evict the old block if it isn't invalid already
				if(cgm_cache_get_block_state(cache, write_back_packet->set, write_back_packet->l2_victim_way) != cgm_cache_block_invalid)
					cgm_L2_cache_evict_block(cache, write_back_packet->set, write_back_packet->l2_victim_way,
							cgm_cache_get_num_shares(gpu, cache, write_back_packet->set, write_back_packet->l2_victim_way), 0, 0);

				//clear the old directory entry
				cgm_cache_clear_dir(cache,  write_back_packet->set, write_back_packet->l2_victim_way);

				//set the new directory entry
				cgm_cache_set_dir(cache, write_back_packet->set, write_back_packet->l2_victim_way, message_packet->l1_cache_id);

				//now set the block
				assert(write_back_packet->cache_block_state == cgm_cache_block_exclusive || write_back_packet->cache_block_state == cgm_cache_block_modified);
				cgm_cache_set_block(cache, write_back_packet->set, write_back_packet->l2_victim_way, write_back_packet->tag, write_back_packet->cache_block_state);

				//check for retries on successful cache read...
				if(message_packet->access_type == cgm_access_load_retry || message_packet->coalesced == 1)
				{
					cache->MergeRetries++;
					//enter retry state process all coalesced accesses
					cache_coalesed_retry(cache, message_packet->tag, message_packet->set, message_packet->access_id);
				}

				//set message size
				message_packet->size = packet_set_size(gpu_v_caches[message_packet->l1_cache_id].block_size); //this can be either L1 I or L1 D cache block size.
				message_packet->cache_block_state = write_back_packet->cache_block_state;

				//update message status
				if(message_packet->cache_block_state == cgm_cache_block_modified)
				{
					message_packet->access_type = cgm_access_putx;
				}
				else if(message_packet->cache_block_state == cgm_cache_block_exclusive)
				{
					message_packet->access_type = cgm_access_put_clnx;
				}
				else
				{
					fatal("cgm_mesi_l2_get(): invalid write back block state\n");
				}

				//free the write back
				write_back_packet = list_remove(cache->write_back_buffer, write_back_packet);
				packet_destroy(write_back_packet);

				GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s load wb hit id %llu state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, *cache_block_state_ptr, P_TIME);

				/*stats*/
				cache->WbMerges++;
				if(!message_packet->protocol_case)
					message_packet->protocol_case = L2_hit;

				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;

				cache_put_io_up_queue(cache, message_packet);
				return;
			}
			else
			{

				// A true miss

				message_packet->l2_victim_way = cgm_cache_search_way(cache, message_packet->set);
				//assert(cache->sets[message_packet->set].blocks[message_packet->l2_victim_way].directory_entry.entry_bits.pending != 1);

				//super rare case here, where this is a unique access so not coalesced, but there isn't a blk->way to make transient
				//this can happen if there is a pending bit set in the directory. No different than a pending conflict so nack this back for a retry
				//if not then we must coalesce
				if(message_packet->l2_victim_way == -1)
				{
					//nack here then everything after this is normal....

					//no room in the set for this request, usually because one or more blocks have some pending action.

					message_packet->access_type = cgm_access_get_nack;

					//set message package size
					message_packet->size = HEADER_SIZE;

					//warning("Nacking get access with no way back to v cache block 0x%08x %s load nack no way available flush ID %llu type %d state %d cycle %llu\n",
					//		(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type,
					//		*cache_block_state_ptr, P_TIME);

					GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s load nack no way available flush ID %llu type %d state %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type,
							*cache_block_state_ptr, P_TIME);

					/*reset mp flags*/
					message_packet->coalesced = 0;
					message_packet->assoc_conflict = 0;

					cache_put_io_up_queue(cache, message_packet);

					return;
				}
				else
				{
					//check ORT for coalesce
					cache_check_ORT(cache, message_packet);

					/*if((message_packet->address & cache->block_address_mask) == 0x08123380)
					warning("GPU L2 $ id %d coalesce %llu phy address 0x%08x state %d\n",
								cache->id, message_packet->access_id,
								(message_packet->address & cache->block_address_mask),
								message_packet->cache_block_state);
					fflush(stderr);*/

					if(message_packet->coalesced == 1)
					{
						/*stats*/
						cache->CoalescePut++;

						GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s load miss coalesce ID %llu type %d state %d cycle %llu\n",
								(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
								message_packet->access_type, *cache_block_state_ptr, P_TIME);

						return;
					}
				}

				//this is a unique access

				//set the transient state.
				assert(message_packet->l2_victim_way >= 0 && message_packet->l2_victim_way < cache->assoc);
				cgm_cache_set_victim(cache, message_packet->set, message_packet->l2_victim_way, message_packet->tag);

				//we are bringing a new block so evict the victim and flush the L1 copies
				if(cgm_cache_get_block_state(cache, message_packet->set, message_packet->l2_victim_way) != cgm_cache_block_invalid)
					cgm_L2_cache_evict_block(cache, message_packet->set, message_packet->l2_victim_way,
							cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->l2_victim_way), 0, 0);

				//clear the directory entry
				cgm_cache_clear_dir(cache, message_packet->set, message_packet->l2_victim_way);

				ort_row = ort_search(cache, message_packet->tag, message_packet->set);
				assert(ort_row < cache->mshr_size);

				if(cgm_cache_get_block_upgrade_pending_bit(cache, message_packet->set, message_packet->l2_victim_way) == 1
						|| ort_get_pending_join_bit(cache, ort_row, message_packet->tag, message_packet->set) == 0)
				{
					printf("\n");
					cgm_cache_dump_set(cache, message_packet->set);

					printf("\n");
					cache_dump_queue(cache->pending_request_buffer);

					printf("\n");

					ort_dump(cache);
					printf("\n");

					fatal("block 0x%08x %s load miss coalesce ID %llu type %d state %d set %d tag %d way %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
							message_packet->access_type, *cache_block_state_ptr, message_packet->set, message_packet->tag, message_packet->l2_victim_way, P_TIME);

				}


				assert(cgm_cache_get_block_upgrade_pending_bit(cache, message_packet->set, message_packet->l2_victim_way) == 0);

				//add some routing/status data to the packet
				//if(message_packet->access_type != cgm_access_get)
				//	fatal("block 0x%08x %s ID %llu type %d state %d cycle %llu\n",
				//			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
				//			message_packet->access_type, *cache_block_state_ptr, P_TIME);


				assert(message_packet->access_type == cgm_access_get || message_packet->access_type == cgm_access_load_retry);

				message_packet->size = HEADER_SIZE;

				if(hub_iommu_connection_type == hub_to_mc)
				{
					//message is going down to mc so its and mc_load
					message_packet->access_type = cgm_access_mc_load;
				}
				else
				{
					//message is going down to L3 so its a get
					message_packet->access_type = cgm_access_get;
					message_packet->cpu_access_type = cgm_access_load;
				}

				//message_packet->cache_block_state = cgm_cache_block_exclusive;

				//L3 should see the entire GPU as a single core.
				message_packet->l2_cache_id = gpu_core_id;
				message_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

				//printf("name %s\n", message_packet->l2_cache_name);

				GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s load miss ID %llu type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
						message_packet->access_type, *cache_block_state_ptr, P_TIME);

				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;

				/*if((message_packet->address & cache->block_address_mask) == 0x08123380)
				warning("GPU L2 $ id %d load going to L3 id %llu blk addr 0x%08x!\n",
						cache->id, message_packet->access_id, (message_packet->address & cache->block_address_mask));
				fflush(stderr);*/

				//transmit to L3
				cache_put_io_down_queue(cache, message_packet);
			}
			break;

		case cgm_cache_block_modified:
		case cgm_cache_block_exclusive:

			assert(sharers >= 0 && sharers <= num_cus);
			assert(owning_core >= 0 && owning_core <= 1);

			if(message_packet->access_type == cgm_access_load_retry || message_packet->coalesced == 1)
			{
				//enter retry state.
				cache_coalesed_retry(cache, message_packet->tag, message_packet->set, message_packet->access_id);

				/*check for a pending get_fwd or getx_fwd join*/
				pending_join = cache_search_pending_request_buffer(cache, (message_packet->address & cache->block_address_mask));

				if(pending_join)
				{
					pending_join->downgrade_pending--;

					if (pending_join->downgrade_pending == 0)
					{
						GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s pulled pending get/getx_fwd id %llu state %d cycle %llu\n",
								(message_packet->address & cache->block_address_mask), cache->name, pending_join->access_id, *cache_block_state_ptr, P_TIME);

						pending_join = list_remove(cache->pending_request_buffer, pending_join);
						list_enqueue(cache->retry_queue, pending_join);
						advance(cache->ec_ptr);
					}
				}
			}

			//if it is a new access (L3 retry) or a repeat access from an already owning core.
			if(sharers == 0 || owning_core == 1)
			{
				/*there should be only 1 core with the block*/
				if(owning_core == 1)
					assert(sharers == 1);

				GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s load hit zero or single shared ID %llu type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, *cache_block_state_ptr, P_TIME);

				//set the presence bit in the directory for the requesting core.
				cgm_cache_clear_dir(cache, message_packet->set, message_packet->way);

				cgm_cache_set_dir(cache, message_packet->set, message_packet->way, message_packet->l1_cache_id);

				//set message size
				message_packet->size = packet_set_size(gpu_v_caches[cache->id].block_size); //this can be either L1 I or L1 D cache block size.

				//update message status
				if(*cache_block_state_ptr == cgm_cache_block_modified)
				{
					message_packet->access_type = cgm_access_putx;
				}
				else if(*cache_block_state_ptr == cgm_cache_block_exclusive)
				{
					message_packet->access_type = cgm_access_put_clnx;
				}
				else if(*cache_block_state_ptr == cgm_cache_block_shared)
				{
					message_packet->access_type = cgm_access_puts;
				}

				/*this will send the block and block state up to the higher level cache.*/
				message_packet->cache_block_state = *cache_block_state_ptr;

				/*GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s load hit ID %llu type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
						message_packet->access_type, *cache_block_state_ptr, P_TIME);*/

				/*stats*/
				if(!message_packet->protocol_case)
						message_packet->protocol_case = L2_hit;

				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;

				/*if((message_packet->address & cache->block_address_mask) == 0x08123380)
				warning("GPU L2 $ id %d load hit %llu phy address 0x%08x state %d\n",
							cache->id, message_packet->access_id,
							(message_packet->address & cache->block_address_mask),
							message_packet->cache_block_state);
				fflush(stderr);*/

				cache_put_io_up_queue(cache, message_packet);

			}
			else if (sharers >= 1)
			{

				/*The connection between GPU L1 and L2 caches is a cross bar
				hold onto this request, downgrade the block in the holding core,
				join on the ack, in the mean time nack all other L1 request for this block.*/

				//block is E or M so there better be only 1 sharer
				assert(sharers == 1 && pending_bit == 0);

				//set the directory pending bit.
				cgm_cache_set_dir_pending_bit(cache, message_packet->set, message_packet->way);

				//flush the block out of the pending core...
				GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s load hit multi share (downgrade) ID %llu type %d pending_bit %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask),
						cache->name, message_packet->access_id,
						message_packet->access_type,
						cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way),
						*cache_block_state_ptr, P_TIME);

				//get the id of the owning core L2
				owning_core = cgm_cache_get_xown_core(gpu, cache, message_packet->set, message_packet->way);

				//down grade the the owning core...
				downgrade_packet = packet_create();
				init_downgrade_packet(downgrade_packet, message_packet->address);

				//flush the block out of the pending core...
				GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s issuing downgrade packet ID %llu type %d cycle %llu\n",
						(downgrade_packet->address & cache->block_address_mask), cache->name, downgrade_packet->downgrade_id, downgrade_packet->access_type, P_TIME);

				downgrade_packet->l1_cache_id = owning_core;

				/*printf("l2 flushing block id %llu cycle %llu\n", flush_packet->evict_id, P_TIME);*/
				list_enqueue(cache->Tx_queue_top, downgrade_packet);
				advance(cache->cache_io_up_ec);

				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;
				message_packet->downgrade = 1;

				/*shouldn't be another pending request for this block*/
				assert(cache_search_pending_request_buffer(cache, message_packet->address) == NULL);

				/*if((message_packet->address & cache->block_address_mask) == 0x08123380)
				warning("GPU L2 $ id %d load hit held downgrade block %llu phy address 0x%08x state %d\n",
							cache->id, message_packet->access_id,
							(message_packet->address & cache->block_address_mask),
							message_packet->cache_block_state);
				fflush(stderr);*/

				//drop the message packet into the pending request buffer
				message_packet = list_remove(cache->last_queue, message_packet);
				list_enqueue(cache->pending_request_buffer, message_packet);

			}
			else
			{
				fatal("cgm_mesi_gpu_l2_get(): invalid sharer/owning_core state\n");
			}

			break;

			case cgm_cache_block_shared:

				//block is in shared state in one or more L1 caches
				//set retry state, update directory, and puts to requesting L1.

				/*fatal("GPU l2 cache line hit shared block DIC %d %llu phy address 0x%08x state %d\n",
							dirty_in_core, message_packet->access_id,
							(message_packet->address & cache->block_address_mask),
							*cache_block_state_ptr);*/

				//check if the packet has coalesced accesses.
				if(message_packet->access_type == cgm_access_load_retry || message_packet->coalesced == 1)
				{
					//enter retry state.
					cache_coalesed_retry(cache, message_packet->tag, message_packet->set, message_packet->access_id);

					/*check for a pending get_fwd or getx_fwd join*/
					pending_join = cache_search_pending_request_buffer(cache, (message_packet->address & cache->block_address_mask));
					assert(!pending_join);
				}


				GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s load hit ID %llu type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, *cache_block_state_ptr, P_TIME);


				//update message status
				message_packet->access_type = cgm_access_puts;

				//get the cache block state
				message_packet->cache_block_state = *cache_block_state_ptr;

				//set the presence bit in the directory for the requesting core.
				cgm_cache_set_dir(cache, message_packet->set, message_packet->way, message_packet->l1_cache_id);

				//set message package size
				message_packet->size = packet_set_size(gpu_v_caches[cache->id].block_size);


				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;

				cache_put_io_up_queue(cache, message_packet);

			break;

	}

	return;
}



/*void cgm_mesi_gpu_l2_get_nack(struct cache_t *cache, struct cgm_packet_t *message_packet){

	int ort_row = 0;
	int conflict_bit = -1;

	int set = 0;
	int tag = 0;
	unsigned int offset = 0;
	int way = 0;
	int l3_map = 0;

	int *set_ptr = &set;
	int *tag_ptr = &tag;
	unsigned int *offset_ptr = &offset;
	int *way_ptr = &way;

	struct cache_t *l3_cache_ptr = NULL;


	//charge delay
	P_PAUSE(cache->latency);

	//probe the address for set, tag, and offset.
	cgm_cache_probe_address(cache, message_packet->address, set_ptr, tag_ptr, offset_ptr);

	GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s get_nack ID %llu type %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
			message_packet->access_type, P_TIME);

	//store the decoded address
	message_packet->tag = tag;
	message_packet->set = set;
	message_packet->offset = offset;

	//do not set retry because this contains the coalesce set and tag.
	//check that there is an ORT entry for this address
	ort_row = ort_search(cache, message_packet->tag, message_packet->set);
	assert(ort_row < cache->mshr_size);
	conflict_bit = cache->ort[ort_row][2];


	//if the conflict bit is set in the ort reset it because this is the nack



	if(conflict_bit == 0)
	{
		cache->ort[ort_row][2] = 1;

		GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s get_nack caught the conflict bit in the ORT table ID %llu type %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
			message_packet->access_type, P_TIME);
	}




	//add some routing/status data to the packet
	message_packet->access_type = cgm_access_get;

	l3_cache_ptr = cgm_l3_cache_map(message_packet->set);
	message_packet->l2_cache_id = cache->id;
	message_packet->l2_cache_name = str_map_value(&l2_strn_map, cache->id);

	SETROUTE(message_packet, cache, l3_cache_ptr)

	//transmit to L3
	cache_put_io_down_queue(cache, message_packet);

	stats
	mem_system_stats->l2_load_nack++;

	return;
}*/


void cgm_mesi_gpu_l2_getx_nack(struct cache_t *cache, struct cgm_packet_t *message_packet){

	int ort_row = 0;
	int conflict_bit = -1;

	int set = 0;
	int tag = 0;
	unsigned int offset = 0;
	/*int way = 0;*/
	//int l3_map = 0;

	int *set_ptr = &set;
	int *tag_ptr = &tag;
	unsigned int *offset_ptr = &offset;
	/*int *way_ptr = &way;*/

	/*should only occur if connected to l3*/
	assert(hub_iommu_connection_type == hub_to_l3);

	//charge delay
	GPU_PAUSE(cache->latency);

	//probe the address for set, tag, and offset.
	cgm_cache_probe_address(cache, message_packet->address, set_ptr, tag_ptr, offset_ptr);

	GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s getx_nack ID %llu type %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, P_TIME);

	//store the decoded address
	message_packet->tag = tag;
	message_packet->set = set;
	message_packet->offset = offset;

	//do not set retry because this contains the coalesce set and tag.
	//check that there is an ORT entry for this address
	ort_row = ort_search(cache, message_packet->tag, message_packet->set);
	assert(ort_row < cache->mshr_size);
	conflict_bit = cache->ort[ort_row][2];

	//pull the GETX_FWD from the pending request buffer
	//pending_get_getx_fwd_request = cache_search_pending_request_buffer(cache, message_packet->address);

	//if the conflict bit is set in the ort reset it because this is the nack
	if(conflict_bit == 0)
	{
		cache->ort[ort_row][2] = 1;

		GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s getx_nack caught the conflict bit in the ORT table ID %llu type %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
			message_packet->access_type, P_TIME);
	}


	if(ort_row >= cache->mshr_size)
	{
		ort_dump(cache);

		printf("problem set %d tag %d block 0x%08x cycle %llu\n",
			message_packet->set, message_packet->tag, (message_packet->address & cache->block_address_mask), P_TIME);

		assert(ort_row < cache->mshr_size);
	}

	//add some routing/status data to the packet
	//message is going down to L3 so its a getx
	message_packet->access_type = cgm_access_getx;
	message_packet->cpu_access_type = cgm_access_store;
	message_packet->cache_block_state = cgm_cache_block_exclusive;
	message_packet->size = HEADER_SIZE;

	/*reset mp flags*/
	message_packet->coalesced = 0;
	message_packet->assoc_conflict = 0;


	//L3 should see the entire GPU as a single core.
	message_packet->l2_cache_id = gpu_core_id;
	message_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

	//transmit to L3

	cache_put_io_down_queue(cache, message_packet);

	return;
}



int cgm_mesi_gpu_l2_getx(struct cache_t *cache, struct cgm_packet_t *message_packet){

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	struct cgm_packet_t *write_back_packet = NULL;
	struct cgm_packet_t *pending_join = NULL;

	int sharers = 0;
	int owning_core = 0;
	int pending_bit = 0;
	int dirty_in_core = 0;

	int num_cus = si_gpu_num_compute_units;

	struct cgm_packet_t *getx_inval_packet = NULL;

	unsigned long long bit_vector;
	int num_messages = 0;
	int i = 0;

	//charge delay
	GPU_PAUSE(cache->latency);

	//get the status of the cache block
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	//search the WB buffer for the data
	write_back_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

	//get number of sharers
	sharers = cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way);
	//check to see if access is from an already owning core
	owning_core = cgm_cache_is_owning_core(cache, message_packet->set, message_packet->way, message_packet->l1_cache_id);
	//check pending state
	pending_bit = cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way);

	dirty_in_core = cgm_cache_get_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);

	/*assumption block is never shared in GPU v caches*/
	/*if(sharers > 1)
		fatal("cgm_mesi_gpu_l2_getx(): sharers = %d\n", sharers);*/

	//update cache way list for cache replacement policies.
	if(*cache_block_hit_ptr == 1)
	{
		//make this block the MRU
		cgm_cache_update_waylist(&cache->sets[message_packet->set], cache->sets[message_packet->set].way_tail, cache_waylist_head);
	}

	if(pending_bit == 1 && *cache_block_hit_ptr == 1)
	{

		/*if(owning_core == 1)
		{
			assert(message_packet->coalesced == 0);

			GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s pending at L2 getx owning core PUTX back to L2 id %llu state %d cycle %llu\n",
				(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, *cache_block_state_ptr, P_TIME);

			there should be only be the owning core in the directory and the pending bit set
			if(sharers != 1 || owning_core != 1)
				printf("block 0x%08x %s pending at L2 getx owning core PUTX back to L2 id %llu state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, *cache_block_state_ptr, P_TIME);

			printf("sharers %d owning core %d\n", sharers, owning_core);

			assert(sharers == 1 && owning_core == 1);

			The block MUST be in the E or M state
			if(*cache_block_state_ptr != cgm_cache_block_exclusive || *cache_block_state_ptr != cgm_cache_block_modified)
				printf("block 0x%08x %s pending at L2 getx owning core (BUG HERE?) PUTX back to L2 id %llu state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, *cache_block_state_ptr, P_TIME);

			assert(*cache_block_state_ptr == cgm_cache_block_exclusive || *cache_block_state_ptr == cgm_cache_block_modified);

			//update message status
			message_packet->access_type = cgm_access_putx;

			//set cache block state modified
			message_packet->cache_block_state = cgm_cache_block_modified;

			// update message packet size
			message_packet->size = packet_set_size(gpu_v_caches[message_packet->l1_cache_id].block_size);

			//don't change the directory entries, the downgrade ack will come back and clean things up.

			reset mp flags
			message_packet->coalesced = 0;
			message_packet->assoc_conflict = 0;

			cache_put_io_up_queue(cache, message_packet);

			stats
			if(!message_packet->protocol_case)
				message_packet->protocol_case = L2_hit;

		}
		else
		{*/
			GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s pending at L2 getx nacked back to L1 %d id %llu state %d cycle %llu\n",
				(message_packet->address & cache->block_address_mask), cache->name,
				message_packet->l1_cache_id, message_packet->access_id, *cache_block_state_ptr, P_TIME);

			/*Third party is looking for access to the block, but it is busy nack and let node retry later*/

			//check if the packet has coalesced accesses.
			if(message_packet->access_type == cgm_access_store_retry || message_packet->coalesced == 1)
			{
				//enter retry state.
				cache_coalesed_retry(cache, message_packet->tag, message_packet->set, message_packet->access_id);
			}


			/*star todo find a better way to do this.
			this is for a special case where a coalesced access was pulled
			and is going to be nacked at this point we want the access to be
			treated as a new miss so set coalesced to 0*/
			if(message_packet->coalesced == 1)
			{
				message_packet->coalesced = 0;
			}

			//send the reply up as a NACK!
			message_packet->access_type = cgm_access_getx_nack;

			//set message package size
			message_packet->size = HEADER_SIZE;

			/*reset mp flags*/
			message_packet->coalesced = 0;
			message_packet->assoc_conflict = 0;

			//send the reply
			cache_put_io_up_queue(cache, message_packet);

			/*stats*//*stats*/
			mem_system_stats->l2_store_nack++;
		/*}*/

		return 1;
	}

	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_noncoherent:
		case cgm_cache_block_owned:
			fatal("l2_cache_ctrl(): Invalid block state on store hit assss %s\n", str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr));
			break;

		case cgm_cache_block_invalid:

			//stats
			//cache->misses++;

			if(write_back_packet)
			{
				/*found the packet in the write back buffer
				data should not be in the rest of the cache*/
				assert(*cache_block_hit_ptr == 0);
				assert(write_back_packet->cache_block_state == cgm_cache_block_modified || write_back_packet->cache_block_state == cgm_cache_block_exclusive);
				assert(message_packet->set == write_back_packet->set && message_packet->tag == write_back_packet->tag);
				assert(message_packet->access_type == cgm_access_getx || message_packet->access_type == cgm_access_store_retry);

				//see if we can write it back into the cache.
				write_back_packet->l2_victim_way = cgm_cache_get_victim_for_wb(cache, write_back_packet->set);

				/*special case nack the request back to L1 because L2 is still waiting on a a flush block ack*/
				if(write_back_packet->flush_pending == 1)
				{
					/*flush is pending, but we have a request for the block, nack it back to L1*/
					//send the reply up as a NACK!
					message_packet->access_type = cgm_access_getx_nack;

					//set message package size
					message_packet->size = HEADER_SIZE;

					/*stats*/
					/*star todo add a stat for this*/

					GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s store nack wb pending flush ID %llu type %d state %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type,
							*cache_block_state_ptr, P_TIME);

					/*reset mp flags*/
					message_packet->coalesced = 0;
					message_packet->assoc_conflict = 0;

					cache_put_io_up_queue(cache, message_packet);

					//warning("nacking store back to L1, flush still pending\n");

					return 1;
				}

				//if not then we must coalesce
				if(write_back_packet->l2_victim_way == -1)
				{
					//Set and ways are all transient must coalesce
					cache_check_ORT(cache, message_packet);

					/*stats*/
					cache->CoalescePut++;

					if(message_packet->coalesced == 1)
					{
						return 1;
					}
					else
					{
						/*FIXME*/

						warning("crazy train start getx! block 0x%08x %s ID %llu type %d state %d cycle %llu\n",
								(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type,
								*cache_block_state_ptr, P_TIME);

						/*ok we can't coalesce so we have to evict the write back and nack the request'*/

						//clear the ORT entry that we just set sigh...
						ort_clear(cache, message_packet);

						/*flush is pending, but we have a request for the block, nack it back to L1*/
						//send the reply up as a NACK!
						message_packet->access_type = cgm_access_getx_nack;

						//set message package size
						message_packet->size = HEADER_SIZE;

						/*stats*/
						/*star todo add a stat for this*/

						GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s load nack wb pending flush ID %llu type %d state %d cycle %llu\n",
								(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type,
								*cache_block_state_ptr, P_TIME);

						/*reset mp flags*/
						message_packet->coalesced = 0;
						message_packet->assoc_conflict = 0;

						cache_put_io_up_queue(cache, message_packet);

						//send the writeback on to main memory


						//verify that the block is out of L1
						int error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, write_back_packet->address);
						assert(error == 0);

						//verify that there is only one wb in L2 for this block.
						error = cache_search_wb_dup_packets(cache, write_back_packet->tag, write_back_packet->set);
						assert(error == 1); //error == 1 i.e only one wb packet and we are about to send it.

						if(write_back_packet->cache_block_state == cgm_cache_block_exclusive)
						{

							//GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s write back destroy ID %llu type %d cycle %llu\n",
							//		(message_packet->address & cache->block_address_mask), cache->name, message_packet->write_back_id, message_packet->access_type, P_TIME);


							/*stats*/
							cache->TotalWriteBackDropped++;

							write_back_packet = list_remove(cache->write_back_buffer, write_back_packet);
							packet_destroy(write_back_packet);
						}
						else
						{

							assert(write_back_packet->cache_block_state == cgm_cache_block_modified);

							//GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s write back sent (to L3) %llu type %d cycle %llu\n",
							//		(message_packet->address & cache->block_address_mask), cache->name, message_packet->write_back_id, message_packet->access_type, P_TIME);

							write_back_packet->size = packet_set_size(cache->block_size);

							if(hub_iommu_connection_type == hub_to_mc)
							{
								//message is going down to mc so its and mc_load
								write_back_packet->access_type = cgm_access_mc_store;

								write_back_packet->l2_cache_id = cache->id;
								write_back_packet->l2_cache_name = cache->name;

								SETROUTE(write_back_packet, cache, system_agent)

							}
							else
							{

								struct cache_t *l3_cache_ptr = NULL;

								l3_cache_ptr = cgm_l3_cache_map(write_back_packet->set);

								write_back_packet->l2_cache_id = cache->id;
								write_back_packet->l2_cache_name = cache->name;

								SETROUTE(write_back_packet, cache, l3_cache_ptr)
							}

							//send the write back on.
							write_back_packet = list_remove(cache->write_back_buffer, write_back_packet);
							assert(write_back_packet);

							list_enqueue(cache->Tx_queue_bottom, write_back_packet);
							advance(cache->cache_io_down_ec);
							//cache_put_io_down_queue(cache, write_back_packet);

						}


						return 1;
					}
				}

				assert(write_back_packet->L3_flush_join == 0);
				assert(write_back_packet->flush_pending == 0);

				//we are bringing a new block so evict the victim and flush the L1 copies
				assert(write_back_packet->l2_victim_way >= 0 && write_back_packet->l2_victim_way < cache->assoc);

				//first evict the old block if it isn't invalid already
				if(cgm_cache_get_block_state(cache, write_back_packet->set, write_back_packet->l2_victim_way) != cgm_cache_block_invalid)
					cgm_L2_cache_evict_block(cache, write_back_packet->set, write_back_packet->l2_victim_way,
							cgm_cache_get_num_shares(gpu, cache, write_back_packet->set, write_back_packet->l2_victim_way), 0, 0);

				//clear the old directory entry
				cgm_cache_clear_dir(cache,  write_back_packet->set, write_back_packet->l2_victim_way);
				//set the new directory entry
				cgm_cache_set_dir(cache, write_back_packet->set, write_back_packet->l2_victim_way, message_packet->l1_cache_id);

				//now set the new block
				assert(write_back_packet->cache_block_state == cgm_cache_block_exclusive || write_back_packet->cache_block_state == cgm_cache_block_modified);
				cgm_cache_set_block(cache, write_back_packet->set, write_back_packet->l2_victim_way, write_back_packet->tag, write_back_packet->cache_block_state);

				//check for retries on successful cache write...
				if(message_packet->access_type == cgm_access_store_retry || message_packet->coalesced == 1)
				{
					cache->MergeRetries++;
					//enter retry state.
					cache_coalesed_retry(cache, message_packet->tag, message_packet->set, message_packet->access_id);
				}

				//set message size
				message_packet->size = packet_set_size(gpu_v_caches[message_packet->l1_cache_id].block_size); //this can be either L1 I or L1 D cache block size.
				message_packet->cache_block_state = write_back_packet->cache_block_state;

				//update message status
				if(message_packet->cache_block_state == cgm_cache_block_modified)
				{
					message_packet->access_type = cgm_access_putx;
				}
				else if(message_packet->cache_block_state == cgm_cache_block_exclusive)
				{
					message_packet->access_type = cgm_access_put_clnx;
				}
				else
				{
					fatal("cgm_mesi_l2_getx(): invalid write back block state\n");
				}

				assert(write_back_packet->flush_pending == 0);

				//free the write back
				write_back_packet = list_remove(cache->write_back_buffer, write_back_packet);
				packet_destroy(write_back_packet);

				GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s store wb hit id %llu state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, *cache_block_state_ptr, P_TIME);

				/*stats*/
				cache->WbMerges++;
				if(!message_packet->protocol_case)
					message_packet->protocol_case = L2_hit;

				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;

				cache_put_io_up_queue(cache, message_packet);
			}
			else
			{

				//find victim
				message_packet->l2_victim_way = cgm_cache_search_way(cache, message_packet->set);

				//if not then we must coalesce
				if(message_packet->l2_victim_way == -1)
				{
					//nack here the everything after this is normal....

					//no room in the set for this request, usually because one or more blocks have some pending action.
					message_packet->access_type = cgm_access_getx_nack;

					//set message package size
					message_packet->size = HEADER_SIZE;

					//warning("Nacking getx access with no way back to v cache block 0x%08x %s load nack no way available flush ID %llu type %d state %d cycle %llu\n",
					//		(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type,
					//		*cache_block_state_ptr, P_TIME);

					GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s load nack no way available flush ID %llu type %d state %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type,
							*cache_block_state_ptr, P_TIME);

					/*reset mp flags*/
					message_packet->coalesced = 0;
					message_packet->assoc_conflict = 0;

					cache_put_io_up_queue(cache, message_packet);

					return 1;

				}
				else
				{
					//check ORT for coalesce
					cache_check_ORT(cache, message_packet);

					if(message_packet->coalesced == 1)
					{
						/*stats*/
						cache->CoalescePut++;

						GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s store miss coalesce ID %llu type %d state %d cycle %llu\n",
								(message_packet->address & cache->block_address_mask),
								cache->name,
								message_packet->access_id,
								message_packet->access_type,
								*cache_block_state_ptr,
								P_TIME);

						return 1;
					}
				}


				assert(cache->sets[message_packet->set].blocks[message_packet->l2_victim_way].directory_entry.entry_bits.pending != 1);
				assert(message_packet->l2_victim_way >= 0 && message_packet->l2_victim_way < cache->assoc);
				cgm_cache_set_victim(cache, message_packet->set, message_packet->l2_victim_way, message_packet->tag);

				//evict the victim
				if(cgm_cache_get_block_state(cache, message_packet->set, message_packet->l2_victim_way) != cgm_cache_block_invalid)
					cgm_L2_cache_evict_block(cache, message_packet->set, message_packet->l2_victim_way,
							cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->l2_victim_way), 0, 0);

				//clear the directory entry
				cgm_cache_clear_dir(cache, message_packet->set, message_packet->l2_victim_way);

				assert(cgm_cache_get_block_upgrade_pending_bit(cache, message_packet->set, message_packet->l2_victim_way) == 0);

				//set access type
				assert(message_packet->access_type == cgm_access_getx || message_packet->access_type == cgm_access_store_retry);

				message_packet->size = HEADER_SIZE;

				if(hub_iommu_connection_type == hub_to_mc)
				{
					//message is going down to mc so its and mc_load
					message_packet->access_type = cgm_access_mc_load;
				}
				else
				{
					//message is going down to L3 so its a getx
					message_packet->access_type = cgm_access_getx;
					message_packet->cpu_access_type = cgm_access_store;
				}

				message_packet->cache_block_state = cgm_cache_block_modified;

				message_packet->l2_cache_id = gpu_core_id;
				message_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

				GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s store miss ID %llu type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
						message_packet->access_type, *cache_block_state_ptr, P_TIME);

				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;

				cache_put_io_down_queue(cache, message_packet);
			}

			break;

		case cgm_cache_block_modified:
		case cgm_cache_block_exclusive:

			assert(sharers >= 0 && sharers <= 1);
			assert(owning_core >= 0 && owning_core <= 1);
			//assert(*cache_block_state_ptr != cgm_cache_block_shared);

			//set retry state
			if(message_packet->access_type == cgm_access_store_retry || message_packet->coalesced == 1)
			{
				//enter retry state.
				cache_coalesed_retry(cache, message_packet->tag, message_packet->set, message_packet->access_id);

				/*check for a pending get_fwd or getx_fwd join*/
				pending_join = cache_search_pending_request_buffer(cache, (message_packet->address & cache->block_address_mask));

				if(pending_join)
				{
					pending_join->downgrade_pending--;

					if (pending_join->downgrade_pending == 0)
					{
						GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s pulled pending get/getx_fwd id %llu state %d cycle %llu\n",
								(message_packet->address & cache->block_address_mask), cache->name, pending_join->access_id, *cache_block_state_ptr, P_TIME);

						pending_join = list_remove(cache->pending_request_buffer, pending_join);
						list_enqueue(cache->retry_queue, pending_join);
						advance(cache->ec_ptr);
					}
				}
			}


			//if it is a new access (l2 retry) or a repeat access from an already owning core.
			if(sharers == 0 || owning_core == 1)
			{
				//if the block is in the E state set M before sending up
				if(*cache_block_state_ptr == cgm_cache_block_exclusive)
				{
					cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_modified);
				}

				GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s store hit single shared ID %llu type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, *cache_block_state_ptr, P_TIME);

				//update message status
				message_packet->access_type = cgm_access_putx;

				//set cache block state modified
				message_packet->cache_block_state = cgm_cache_block_modified;

				//set message status and size
				message_packet->size = packet_set_size(gpu_v_caches[message_packet->l1_cache_id].block_size); //this should be L1 D cache block size.

				//update directory
				cgm_cache_clear_dir(cache, message_packet->set, message_packet->way);

				cgm_cache_set_dir(cache, message_packet->set, message_packet->way, message_packet->l1_cache_id);


				/*stats*/
				if(!message_packet->protocol_case)
					message_packet->protocol_case = L2_hit;

				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;

				//send up to L1 D cache
				cache_put_io_up_queue(cache, message_packet);

			}
			else if(sharers == 1)
			{

				/*The connection between GPU L1 and L2 caches is a cross bar
				hold onto this request, flush the block out of the compute unit,
				join on the ack, in the mean time nack all other L1 request for this block.*/

				//there better be only 1 sharer
				assert(sharers == 1 && pending_bit == 0);

				//flush the block out of the pending core...
				GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s store hit multi share (start getx_inval) sharers %d ID %llu type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask),
						cache->name, sharers,
						message_packet->access_id, message_packet->access_type,
						*cache_block_state_ptr, P_TIME);


				//set the directory pending bit.
				cgm_cache_set_dir_pending_bit(cache, message_packet->set, message_packet->way);

				//get the id of the owning core L1
				/*owning_core = cgm_cache_get_xown_core(gpu, cache, message_packet->set, message_packet->way);*/

				//flush the block out of the core...
				getx_inval_packet = packet_create();
				assert(getx_inval_packet);
				init_getx_inval_packet(getx_inval_packet, message_packet->address);

				getx_inval_packet->gpu_access_type = cgm_access_store;
				getx_inval_packet->l1_cache_id = cgm_cache_get_xown_core(gpu, cache, message_packet->set, message_packet->way);

				list_enqueue(cache->Tx_queue_top, getx_inval_packet);
				advance(cache->cache_io_up_ec);

				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;

				message_packet->getx_inval_n = sharers; //set the number of invals to wait for.

				/*shouldn't be another pending request for this block*/
				assert(cache_search_pending_request_buffer(cache, message_packet->address) == NULL);

				//drop the message packet into the pending request buffer
				message_packet = list_remove(cache->last_queue, message_packet);
				list_enqueue(cache->pending_request_buffer, message_packet);

			}
			else
			{
				fatal("cgm_mesi_gpu_l2_getx(): invalid sharer/owning_core state\n");
			}

			break;

		case cgm_cache_block_shared:


			//need a GPU equivalent PutX_N
			//flush all cores holding the block and then send to

			/*warning("cgm_mesi_gpu_l2_getx(): #2 store to multi shared or dirty in core DIC %d sharers %d owning core %d id %llu blk 0x%08x\n",
					dirty_in_core,
					sharers,
					owning_core,
					message_packet->access_id,
					(message_packet->address & cache->block_address_mask));*/

			//need to implment the other possible occurances
			/*if(dirty_in_core == 1 || sharers >= 2)
				fatal("cgm_mesi_gpu_l2_getx(): store to multi shared or dirty in core DIC %d sharers %d owning core %d id %llu blk 0x%08x\n",
						dirty_in_core,
						sharers,
						owning_core,
						message_packet->access_id,
						(message_packet->address & cache->block_address_mask));*/

			GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s store miss on shared line sharers %d DIC %d ID %llu type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask),
						cache->name,
						sharers,
						dirty_in_core,
						message_packet->access_id, message_packet->access_type,
						*cache_block_state_ptr, P_TIME);

			//block can be dirty in core and shared among one of more CUs. The GPU has the block E or M already at L3
			//block can be share and not dirty in core means the block is S at L3 and maybe in one or more CPU cores.

			/*if(sharers < 0 || sharers >= num_cus)
				printf("block 0x%08x %s store miss on shared line sharers %d DIC %d ID %llu type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask),
						cache->name,
						sharers,
						dirty_in_core,
						message_packet->access_id, message_packet->access_type,
						*cache_block_state_ptr, P_TIME);*/


			assert(sharers >= 0 && sharers <= num_cus);
			assert(dirty_in_core == 0 || dirty_in_core == 1);

			if(dirty_in_core == 0)
			{
				//the block is really in the shared state here in the GPU and maybe in the GPU
				//evict the line in all holders but the and set the line transient

				//add to ORT table
				cache_check_ORT(cache, message_packet);
				assert(message_packet->coalesced == 0);

				assert(cache->sets[message_packet->set].blocks[message_packet->way].directory_entry.entry_bits.pending != 1);
				assert(message_packet->way >= 0 && message_packet->way < cache->assoc);

				cgm_cache_set_victim(cache, message_packet->set, message_packet->way, message_packet->tag);

				cgm_L2_cache_evict_block(cache, message_packet->set, message_packet->way,
								cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way), 0, 0);

				//clear the directory entry
				cgm_cache_clear_dir(cache, message_packet->set, message_packet->way);

				assert(cache->sets[message_packet->set].blocks[message_packet->way].transient_state == cgm_cache_block_transient);
				assert(cache->sets[message_packet->set].blocks[message_packet->way].transient_tag == message_packet->tag);

				//set access type
				assert(message_packet->access_type == cgm_access_getx || message_packet->access_type == cgm_access_store_retry);

				message_packet->size = HEADER_SIZE;

				if(hub_iommu_connection_type == hub_to_mc)
				{
					//message is going down to mc so its and mc_load
					message_packet->access_type = cgm_access_mc_load;
				}
				else
				{
					//message is going down to L3 so its a getx
					message_packet->access_type = cgm_access_getx;
					message_packet->cpu_access_type = cgm_access_store;
				}

				message_packet->cache_block_state = cgm_cache_block_modified;

				message_packet->l2_cache_id = gpu_core_id;
				message_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;

				cache_put_io_down_queue(cache, message_packet);
			}
			else if(sharers > 0 && dirty_in_core == 1)
			{
				assert(pending_bit == 0);

				/*the block is actually E/M to the GPU, but dirty in core and shared
				among the compute units*/

				//there better be only 1 sharer
				/*warning("block 0x%08x %s store upgrade miss DIRTY IN CORE (start getx_inval) sharers %d ID %llu type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask),
						cache->name, sharers,
						message_packet->access_id, message_packet->access_type,
						*cache_block_state_ptr, P_TIME);*/

				//flush the block out of all CUs
				GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s store upgrade miss DIRTY IN CORE (start getx_inval) sharers %d ID %llu type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask),
						cache->name, sharers,
						message_packet->access_id, message_packet->access_type,
						*cache_block_state_ptr, P_TIME);


				//set the directory pending bit.
				cgm_cache_set_dir_pending_bit(cache, message_packet->set, message_packet->way);

				//get the id of the owning core L1
				/*owning_core = cgm_cache_get_xown_core(gpu, cache, message_packet->set, message_packet->way);*/

				//get the presence bits from the directory
				bit_vector = cache->sets[message_packet->set].blocks[message_packet->way].directory_entry.entry;
				bit_vector = bit_vector & cache->share_mask;

				assert(bit_vector > 0);

				num_messages = 0;

				for(i = 0; i < num_cus; i++)
				{
					//for each core that has a copy of the cache block send the eviction
					if((bit_vector & 1) == 1)
					{

						//flush the block out of the core...
						getx_inval_packet = packet_create();
						assert(getx_inval_packet);
						init_getx_inval_packet(getx_inval_packet, message_packet->address);

						getx_inval_packet->gpu_access_type = cgm_access_store;
						getx_inval_packet->l1_cache_id = i;

						GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s getx_inval packet created id %llu dest [%d] cycle %llu\n",
										(message_packet->address & cache->block_address_mask),
										cache->name, getx_inval_packet->downgrade_id, getx_inval_packet->l1_cache_id, P_TIME);


						list_enqueue(cache->Tx_queue_top, getx_inval_packet);
						advance(cache->cache_io_up_ec);

						num_messages++;
					}

					//shift the vector to the next position and continue
					bit_vector = bit_vector >> 1;
				}

				//make sure these two are aligned.
				assert(num_messages == sharers);


				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;

				message_packet->getx_inval_n = sharers; //set the number of invals to wait for.

				/*shouldn't be another pending request for this block*/
				assert(cache_search_pending_request_buffer(cache, message_packet->address) == NULL);

				//drop the message packet into the pending request buffer
				message_packet = list_remove(cache->last_queue, message_packet);
				list_enqueue(cache->pending_request_buffer, message_packet);
			}
			else
			{
				assert(sharers == 0 && dirty_in_core == 1);

				fatal("cgm_mesi_gpu_l2_getx(): GPU GetX to shared line, but sharers == 0 shound't happen id %llu blk 0x%08x\n",
						message_packet->access_id, (message_packet->address & cache->block_address_mask));

			}

			break;

	}

	return 1;
}




void cgm_mesi_gpu_l2_get_getx_fwd_inval_ack(struct cache_t *cache, struct cgm_packet_t *message_packet){

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	struct cgm_packet_t *get_getx_fwd_reply_packet = NULL;
	struct cgm_packet_t *pending_get_getx_fwd_request = NULL;
	struct cgm_packet_t *write_back_packet = NULL;
	struct cache_t *l3_cache_ptr = NULL;

	//int sharers, owning_core, pending_bit;

	//int num_cus = si_gpu_num_compute_units;

	//int l3_map;
	int error = 0;

	//charge delay
	GPU_PAUSE(cache->latency);

	//L1 D cache has been flushed

	//get the status of the cache block and try to find it in either the cache or wb buffer
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	//search the WB buffer for the data
	write_back_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

	//get number of sharers
	//sharers = cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way);
	//check to see if access is from an already owning core
	//owning_core = cgm_cache_is_owning_core(cache, message_packet->set, message_packet->way, message_packet->l1_cache_id);
	//check pending state
	//pending_bit = cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way);

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);

	GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s getx_fwd_inval_ack ID %llu type %d state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, *cache_block_state_ptr, P_TIME);

	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_noncoherent:
		case cgm_cache_block_owned:
		case cgm_cache_block_shared:
			fatal("cgm_mesi_l2_getx_fwd_inval_ack(): L2 id %d invalid block state on getx_fwd inval ask as %s address %u\n",
				cache->id, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr), message_packet->address);
			break;

		case cgm_cache_block_invalid:

			fatal("cgm_mesi_gpu_l2_get_getx_fwd_inval_ack(): assume not going to happen yet\n");

			//check WB for line...
			if(write_back_packet)
			{
				/*inval is complete for this write back so it should not be up in L1 D*/
				error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, message_packet->address);
				assert(error == 0);

				assert(write_back_packet->cache_block_state == cgm_cache_block_modified || write_back_packet->cache_block_state == cgm_cache_block_exclusive);

				//////////
				//GETX_FWD
				//////////

				//forward block to requesting core

				//pull the GETX_FWD from the pending request buffer
				pending_get_getx_fwd_request = cache_search_pending_request_buffer(cache, message_packet->address);
				/*if not found uh-oh...*/
				assert(pending_get_getx_fwd_request);
				/*the address better be the same too...*/
				assert(pending_get_getx_fwd_request->address == message_packet->address);
				assert(pending_get_getx_fwd_request->start_cycle != 0);

				//prepare to forward the block
				//set access type
				pending_get_getx_fwd_request->access_type = cgm_access_putx;

				//set the block state
				pending_get_getx_fwd_request->cache_block_state = cgm_cache_block_modified;

				//set message package size if modified in L2/L1.
				/*if(message_packet->cache_block_state == cgm_cache_block_modified || write_back_packet->cache_block_state == cgm_cache_block_modified)
				{*/
				pending_get_getx_fwd_request->size = packet_set_size(l2_caches[str_map_string(&l2_strn_map, pending_get_getx_fwd_request->l2_cache_name)].block_size);
				/*}
				else
				{
				pending_get_getx_fwd_request->size = 1;
				}*/

				//fwd block to requesting core
				//update routing headers swap dest and src
				//requesting node
				pending_get_getx_fwd_request->dest_name = str_map_value(node_strn_map, pending_get_getx_fwd_request->src_id);
				pending_get_getx_fwd_request->dest_id = str_map_string(node_strn_map, pending_get_getx_fwd_request->src_name);

				//owning node L2
				pending_get_getx_fwd_request->src_name = cache->name;
				pending_get_getx_fwd_request->src_id = str_map_string(node_strn_map, cache->name);

				//transmit block to requesting node
				pending_get_getx_fwd_request = list_remove(cache->pending_request_buffer, pending_get_getx_fwd_request);
				list_enqueue(cache->Tx_queue_bottom, pending_get_getx_fwd_request);
				advance(cache->cache_io_down_ec);

				///////////////
				//getx_fwd_ack
				///////////////

				//send the getx_fwd_ack to L3 cache.

				//create getx_fwd_ack packet
				get_getx_fwd_reply_packet = packet_create();
				assert(get_getx_fwd_reply_packet);

				init_getx_fwd_ack_packet(get_getx_fwd_reply_packet, message_packet->address);
				get_getx_fwd_reply_packet->access_id = message_packet->access_id;

				//set message package size if modified in L2/L1.
				if(message_packet->cache_block_state == cgm_cache_block_modified || write_back_packet->cache_block_state == cgm_cache_block_modified)
				{
					get_getx_fwd_reply_packet->size = packet_set_size(l2_caches[str_map_string(&l2_strn_map, pending_get_getx_fwd_request->l2_cache_name)].block_size);
					get_getx_fwd_reply_packet->cache_block_state = cgm_cache_block_modified;
				}
				else
				{
					get_getx_fwd_reply_packet->size = HEADER_SIZE;
					get_getx_fwd_reply_packet->cache_block_state = cgm_cache_block_invalid;
				}

				//fwd reply (getx_fwd_ack) to L3
				l3_cache_ptr = cgm_l3_cache_map(message_packet->set);

				//fakes src as the requester
				get_getx_fwd_reply_packet->l2_cache_id = pending_get_getx_fwd_request->l2_cache_id;
				get_getx_fwd_reply_packet->l2_cache_name = pending_get_getx_fwd_request->src_name;

				SETROUTE(get_getx_fwd_reply_packet, cache, l3_cache_ptr)

				//transmit getx_fwd_ack to L3 (home)
				list_enqueue(cache->Tx_queue_bottom, get_getx_fwd_reply_packet);
				advance(cache->cache_io_down_ec);

				write_back_packet = list_remove(cache->write_back_buffer, write_back_packet);
				packet_destroy(write_back_packet);

				//destroy the L1 D getx_fwd_inval_ack message because we don't need it anymore.
				message_packet = list_remove(cache->last_queue, message_packet);
				packet_destroy(message_packet);

			}
			else
			{

				fatal("cgm_mesi_l2_getx_fwd_inval_ack(): get_getx_fwd should no longer get this far\n");
				/*unsigned int temp = message_packet->address;
				temp = temp & cache->block_address_mask;
				fatal("cgm_mesi_l2_getx_fwd_inval_ack(): line missing in L2 after downgrade block addr 0x%08x\n", temp);

				//pull the GET_FWD from the pending request buffer
				pending_getx_fwd_request = cache_search_pending_request_buffer(cache, message_packet->address);
				if not found uh-oh...
				assert(pending_getx_fwd_request);
				the address better be the same too...
				assert(pending_getx_fwd_request->address == message_packet->address);
				assert(pending_getx_fwd_request->start_cycle != 0);

				//downgrade the local block
				assert(pending_getx_fwd_request->set == message_packet->set && pending_getx_fwd_request->way == message_packet->way);

				//set cgm_access_getx_fwd_nack
				pending_getx_fwd_request->access_type = cgm_access_getx_fwd_nack;

				//fwd reply (downgrade_nack) to L3
				l3_map = cgm_l3_cache_map(pending_getx_fwd_request->set);

				here send the nack down to the L3
				don't change any of the source information

				message_packet->l2_cache_id = l2_caches[my_pid].id;
				message_packet->l2_cache_name = str_map_value(&l2_strn_map, l2_caches[my_pid].id);
				reply_packet->src_name = l2_caches[my_pid].name;
				reply_packet->src_id = str_map_string(&node_strn_map, l2_caches[my_pid].name);

				pending_getx_fwd_request->dest_name = l3_caches[l3_map].name;
				pending_getx_fwd_request->dest_id = str_map_string(&node_strn_map, l3_caches[l3_map].name);

				//transmit back to L3
				pending_getx_fwd_request = list_remove(cache->pending_request_buffer, pending_getx_fwd_request);
				list_enqueue(cache->Tx_queue_bottom, pending_getx_fwd_request);
				advance(cache->cache_io_down_ec);

				message_packet = list_remove(cache->last_queue, message_packet);
				packet_destroy(message_packet);*/

			}

			break;

		case cgm_cache_block_exclusive:
		case cgm_cache_block_modified:

			//block still present in L2 cache

			//////////
			//GETX_FWD
			//////////

			//forward block to requesting core

			//pull the GETX_FWD from the pending request buffer
			pending_get_getx_fwd_request = cache_search_pending_request_buffer(cache, message_packet->address);
			/*if not found uh-oh...*/
			assert(pending_get_getx_fwd_request);
			/*the address better be the same too...*/
			assert(pending_get_getx_fwd_request->address == message_packet->address);
			assert(pending_get_getx_fwd_request->start_cycle != 0);
			assert(pending_get_getx_fwd_request->access_type == cgm_access_get_fwd || pending_get_getx_fwd_request->access_type == cgm_access_getx_fwd);

			//invalidate the local block
			assert(pending_get_getx_fwd_request->set == message_packet->set && pending_get_getx_fwd_request->way == message_packet->way);
			cgm_cache_set_block_state(cache, pending_get_getx_fwd_request->set, pending_get_getx_fwd_request->way, cgm_cache_block_invalid);

			//make sure the directory is clear...
			cgm_cache_clear_dir(cache, pending_get_getx_fwd_request->set, pending_get_getx_fwd_request->way);

			//prepare to forward the block
			pending_get_getx_fwd_request->size = packet_set_size(l2_caches[str_map_string(&l2_strn_map, pending_get_getx_fwd_request->l2_cache_name)].block_size);

			//set message package size if modified in L2/L1.
			if(*cache_block_state_ptr == cgm_cache_block_modified || message_packet->cache_block_state == cgm_cache_block_modified)
			{
				//set access type
				pending_get_getx_fwd_request->access_type = cgm_access_putx;

				//set the block state
				pending_get_getx_fwd_request->cache_block_state = cgm_cache_block_modified;
			}
			else
			{
				//set access type
				pending_get_getx_fwd_request->access_type = cgm_access_put_clnx;

				//set the block state
				pending_get_getx_fwd_request->cache_block_state = cgm_cache_block_exclusive;
			}

			//fwd block to requesting core
			//update routing headers swap dest and src
			//requesting node
			pending_get_getx_fwd_request->dest_name = str_map_value(node_strn_map, pending_get_getx_fwd_request->src_id);
			pending_get_getx_fwd_request->dest_id = str_map_string(node_strn_map, pending_get_getx_fwd_request->src_name);

			//owning node L2

			//pending_get_getx_fwd_request->src_name = cache->name;
			//pending_get_getx_fwd_request->src_id = str_map_string(node_strn_map, cache->name);

			//transmit block to requesting node
			pending_get_getx_fwd_request = list_remove(cache->pending_request_buffer, pending_get_getx_fwd_request);
			list_enqueue(cache->Tx_queue_bottom, pending_get_getx_fwd_request);
			advance(cache->cache_io_down_ec);

			///////////////
			//getx_fwd_ack
			///////////////

			//send the get_getx_fwd_ack to L3 cache.

			//create get_getx_fwd_ack packet
			get_getx_fwd_reply_packet = packet_create();
			assert(get_getx_fwd_reply_packet);

			init_getx_fwd_ack_packet(get_getx_fwd_reply_packet, message_packet->address);

			//set message package size if modified in L2/L1.
			if(*cache_block_state_ptr == cgm_cache_block_modified || message_packet->cache_block_state == cgm_cache_block_modified)
			{
				get_getx_fwd_reply_packet->size = packet_set_size(l2_caches[str_map_string(&l2_strn_map, pending_get_getx_fwd_request->l2_cache_name)].block_size);
				get_getx_fwd_reply_packet->cache_block_state = cgm_cache_block_modified;
			}
			else
			{
				get_getx_fwd_reply_packet->size = HEADER_SIZE;
				get_getx_fwd_reply_packet->cache_block_state = cgm_cache_block_exclusive;
			}

			//fwd reply (getx_fwd_ack) to L3
			//l3_cache_ptr = cgm_l3_cache_map(message_packet->set);

			//fakes src as the requester
			get_getx_fwd_reply_packet->l2_cache_id = pending_get_getx_fwd_request->l2_cache_id;
			get_getx_fwd_reply_packet->l2_cache_name = pending_get_getx_fwd_request->src_name;

			//SETROUTE(get_getx_fwd_reply_packet, cache, l3_cache_ptr)

			//transmit getx_fwd_ack to L3 (home)
			list_enqueue(cache->Tx_queue_bottom, get_getx_fwd_reply_packet);
			advance(cache->cache_io_down_ec);

			//destroy the L1 D getx_fwd_inval_ack message because we don't need it anymore.
			message_packet = list_remove(cache->last_queue, message_packet);
			packet_destroy(message_packet);

			break;
	}

	return;
}

void cgm_mesi_gpu_l2_get_getx_fwd_nack(struct cache_t *cache, struct cgm_packet_t *message_packet){


	//if we are here should just need to retry the access
	fatal("%s received get getx_fwd nack DOUBLE CHECK ME cycle %llu\n", cache->name, P_TIME);


	int set = 0;
	int tag = 0;
	unsigned int offset = 0;
	int way = 0;

	int *set_ptr = &set;
	int *tag_ptr = &tag;
	unsigned int *offset_ptr = &offset;
	// *way_ptr = &way;

	//int l3_map = 0;
	int ort_status = 0;
	//struct cgm_packet_t *pending_packet;
	enum cgm_cache_block_state_t victim_trainsient_state;

	//struct cgm_packet_t *pending_get_getx_fwd_request = NULL;

	//probe the address for set, tag, and offset.
	cgm_cache_probe_address(cache, message_packet->address, set_ptr, tag_ptr, offset_ptr);

	//store the decode in the packet for now.
	message_packet->tag = tag;
	message_packet->set = set;
	message_packet->offset = offset;
	message_packet->way = way;

	//charge delay
	GPU_PAUSE(cache->latency);

	/*our load (getx_fwd) request has been nacked by the owning L2*/

	/*verify status of cache blk*/
	victim_trainsient_state = cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->l2_victim_way);
	assert(victim_trainsient_state == cgm_cache_block_transient); //there is a transient block
	if(victim_trainsient_state != cgm_cache_block_transient)
	{
		ort_dump(cache);
		cgm_cache_dump_set(cache, message_packet->set);

		unsigned int temp = message_packet->address;
		temp = temp & cache->block_address_mask;

		fatal("cgm_mesi_l2_getx_fwd_nack(): %s block not in transient state access_id %llu address 0x%08x blk_addr 0x%08x set %d tag %d way %d cycle %llu\n",
			cache->name, message_packet->access_id, message_packet->address, temp,
			message_packet->set, message_packet->tag, message_packet->way, P_TIME);
	}


	ort_status = ort_search(cache, message_packet->tag, message_packet->set); // there is an outstanding request.
	assert(ort_status <= cache->mshr_size);

	GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s getx_fwd_nack nack ID %llu type %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, P_TIME);

	//change to a get
	message_packet->access_type = cgm_access_getx;
	message_packet->cpu_access_type = cgm_access_load;
	message_packet->cache_block_state = cgm_cache_block_exclusive;

	//L3 should see the entire GPU as a single core.
	message_packet->l2_cache_id = gpu_core_id;
	message_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);

	cache_put_io_down_queue(cache, message_packet);

	return;
}

void cgm_mesi_gpu_l2_get_fwd(struct cache_t *cache, struct cgm_packet_t *message_packet){

	/*fatal("cgm_mesi_gpu_l2_get_getx_fwd(): BOOM!\n");*/

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	int num_cus = si_gpu_num_compute_units;

	struct cgm_packet_t *downgrade_packet = NULL;
	struct cgm_packet_t *write_back_packet = NULL;
	struct cgm_packet_t *nack_packet = NULL;
	struct cgm_packet_t *reply_packet = NULL;

	int sharers = 0;
	int pending_bit = 0;
	int dirty_in_core = 0;//, owning_core;

	int error = 0;
	int ort_status = 0;
	//int l3_map;

	//charge delay
	GPU_PAUSE(cache->latency);

	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);

	//search the WB buffer for the data
	write_back_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

	//get number of sharers
	sharers = cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way);
	assert(sharers >= 0 && sharers <= num_cus);
	//check to see if access is from an already owning core
	//owning_core = cgm_cache_is_owning_core(cache, message_packet->set, message_packet->way, message_packet->l1_cache_id);
	//check pending state
	pending_bit = cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way);
	/*if(pending_bit)
		fatal("cgm_mesi_gpu_l2_get_fwd(): pending bit is 1 hit ptr %d block 0x%08x vtl addr 0x%08x\n",
				*cache_block_hit_ptr, message_packet->address & cache->block_address_mask, message_packet->vtladdress & cache->block_address_mask);*/
	assert(pending_bit == 0 || pending_bit == 1); //fix me if this happens, nack maybe?

	//see if the block is dirty in core.
	dirty_in_core = cgm_cache_get_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);
	assert(dirty_in_core == 0 || dirty_in_core == 1); //when this is 1 check that it is being written back to L3 as a sharing write-back

	GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s get_fwd ID %llu type %d state %d wb? %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
			message_packet->access_type, *cache_block_state_ptr, is_writeback_present(write_back_packet), P_TIME);


	/*look for an access conflict, this can happen if a get_fwd beats a putx/putx_n*/
	ort_status = ort_search(cache, message_packet->tag, message_packet->set);
	if(ort_status != cache->mshr_size)
	{

		warning("cgm_mesi_gpu_l2_get_fwd(): access conflict need to process a join. id %llu block 0x%08x vtl addr 0x%08x cycle %llu\n",
				message_packet->access_id,
				message_packet->address & cache->block_address_mask,
				message_packet->vtladdress & cache->block_address_mask,
				P_TIME);

		/*if there is a pending access int the ORT there better not be a block or a write back*/
		if(*cache_block_state_ptr == cgm_cache_block_invalid)
			assert(!write_back_packet);

		/*if(cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->way) == cgm_cache_block_transient)
			assert(*cache_block_state_ptr == cgm_cache_block_shared && *cache_block_hit_ptr == 1);*/
	}

	//directory is busy nack the request back
	if(pending_bit == 1 && *cache_block_hit_ptr == 1)
	{
		//directory is busy with a block movement, need to nack the request.

		/*fatal("block 0x%08x %s pending at gpu l2 get nacked back to requesting L2 id %llu state %d cycle %llu\n",
				(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, *cache_block_state_ptr, P_TIME);*/

		GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s pending at gpu l2 get nacked back to requesting L2 id %llu state %d cycle %llu\n",
				(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, *cache_block_state_ptr, P_TIME);

		/*two part reply (1) send nack to L3 and (2) send nack to requesting L2*/
		//set access type
		message_packet->access_type = cgm_access_downgrade_nack;

		//star fixme there has to be a better way to do this.
		//set this to two so the hub can sort out its destination
		message_packet->downgrade_ack = 2;

		//set the block state
		message_packet->cache_block_state = cgm_cache_block_invalid;

		//set message package size
		message_packet->size = HEADER_SIZE;

		//fwd nack to requesting core
		//update routing headers swap dest and src
		//requesting node
		message_packet->dest_name = str_map_value(node_strn_map, message_packet->src_id);
		message_packet->dest_id = str_map_string(node_strn_map, message_packet->src_name);

		//owning node L2
		message_packet->src_name = cache->name;
		message_packet->src_id = str_map_string(node_strn_map, cache->name);

		//transmit nack to L2
		cache_put_io_down_queue(cache, message_packet);

		////////
		//part 2
		////////

		//L3 should see the entire GPU as a single core.

		//create downgrade_nack
		nack_packet = packet_create();
		assert(nack_packet);

		init_downgrade_nack_packet(nack_packet, message_packet->address);
		nack_packet->cache_block_state = *cache_block_state_ptr; //let L3 know what the block state is
		assert(nack_packet->cache_block_state == cgm_cache_block_exclusive || nack_packet->cache_block_state == cgm_cache_block_modified);

		nack_packet->access_id = message_packet->access_id;

		nack_packet->l2_cache_id = gpu_core_id;
		nack_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

		//transmit block to L3
		list_enqueue(cache->Tx_queue_bottom, nack_packet);
		advance(cache->cache_io_down_ec);

		/*cgm_cache_dump_set(&l3_caches[0], 1156);
		printf("\n");*/

		return;
	}


	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_noncoherent:
		case cgm_cache_block_owned:

			cgm_cache_dump_set(cache, message_packet->set);

			fatal("cgm_mesi_l2_getx_fwd(): %s invalid block state on getx_fwd as %s access_id %llu address 0x%08x blk_addr 0x%08x set %d tag %d way %d state %d cycle %llu\n",
				cache->name, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr),
				message_packet->access_id, message_packet->address, message_packet->address & cache->block_address_mask,
				message_packet->set, message_packet->tag, message_packet->way, *cache_block_state_ptr, P_TIME);
			break;

		case cgm_cache_block_invalid:

			//check the WB buffer
			if(write_back_packet)
			{

				//Star checked April 6 2019

				/*found the packet in the write back buffer
				data should not be in the rest of the cache*/

				assert(*cache_block_hit_ptr == 0);
				assert(write_back_packet->cache_block_state == cgm_cache_block_modified
						|| write_back_packet->cache_block_state == cgm_cache_block_exclusive);

				/*check state of write back for flush*/
				if(write_back_packet->flush_pending == 0)
				{

					/*fatal("l2 get_fwd with WB make sure I am good to go phy addr 0x%08x vtl addr 0x%08x\n",
							message_packet->address & cache->block_address_mask, message_packet->vtladdress & cache->block_address_mask);*/


					/*the flush is complete finish the get_fwd now*/

					/*flush is complete for this write back so it should not be up in L1 D*/
					error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, message_packet->address);
					assert(error == 0);

					//////////
					//GET_FWD
					//////////

					//forward block to requesting core

					//set access type
					message_packet->access_type = cgm_access_puts;

					message_packet->cache_block_state = cgm_cache_block_shared;

					//prepare to forward the block
					message_packet->size = packet_set_size(cache->block_size);

					//fwd block to requesting core
					//update routing headers swap dest and src
					//requesting node
					message_packet->dest_name = str_map_value(node_strn_map, message_packet->src_id);
					message_packet->dest_id = str_map_string(node_strn_map, message_packet->src_name);

					//owning node L2
					message_packet->src_name = cache->name;
					message_packet->src_id = str_map_string(node_strn_map, cache->name);

					//transmit block to requesting node
					message_packet = list_remove(cache->last_queue, message_packet);
					list_enqueue(cache->Tx_queue_bottom, message_packet);
					advance(cache->cache_io_down_ec);

					/////////////
					//get_fwd_ack
					/////////////

					//send the getx_fwd_ack to L3 cache.

					//create get_fwd_ack packet
					reply_packet = packet_create();
					assert(reply_packet);

					init_downgrade_ack_packet(reply_packet, message_packet->address);

					//a flush has completed and the WB is still here so it better be modified
					assert(write_back_packet->cache_block_state == cgm_cache_block_modified);
					reply_packet->size = packet_set_size(l2_caches[str_map_string(&l2_strn_map, message_packet->l2_cache_name)].block_size);
					reply_packet->cache_block_state = cgm_cache_block_modified;

					//send reply to L3
					//fakes src as the requester L3 uses this to set holding core.
					reply_packet->l2_cache_id = message_packet->l2_cache_id;
					reply_packet->l2_cache_name = message_packet->src_name;

					//dest is set in hub-iommu

					//transmit getx_fwd_ack to L3 (home)
					list_enqueue(cache->Tx_queue_bottom, reply_packet);
					advance(cache->cache_io_down_ec);

					write_back_packet = list_remove(cache->write_back_buffer, write_back_packet);
					packet_destroy(write_back_packet);

				}
				else
				{
					/*write back is in the process of being flushed by L1*/
					assert(write_back_packet->flush_pending == 1);

					/*fatal("cgm_mesi_gpu_l2_get_getx_fwd(): shouldn't have pending flush yet vtl addr0x%08x\n",
							(message_packet->address & cache->block_address_mask));*/

					/*if the wb is flush pending we have to wait for the flush to complete and then join there*/

					//message_packet->downgrade_pending = 1;
					message_packet->L3_flush_join = 1;
					cgm_cache_insert_pending_request_buffer(cache, message_packet);
				}
			}
			else
			{

				if(ort_status < cache->mshr_size)
				{

					warning("block 0x%08x %s get_fwd conflict pending join ID %llu type %d state %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name,message_packet->access_id,
							message_packet->access_type, *cache_block_state_ptr, P_TIME);

					//if here the get_fwd beat a reply (put) from L3. Don't join the request in the GPU just nack it back to the requester
					//this can happen if the CPU is requesting the block need to handle this protocol case if it pops up
					// this could just be a simple nack back to requester.


					GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s get_fwd conflict pending join ID %llu type %d state %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
							message_packet->access_type, *cache_block_state_ptr, P_TIME);

					/*
					error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, message_packet->address);
					if(error == 1)
					{
						warning("block 0x%08x %s get_fwd block in l1 but not in l2 pending join ID %llu type %d state %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name,message_packet->access_id,
							message_packet->access_type, *cache_block_state_ptr, P_TIME);
					}

					two part reply (1) send nack to L3 and (2) send nack to requesting L2

					//set access type
					message_packet->access_type = cgm_access_downgrade_nack;

					//set the block state
					message_packet->cache_block_state = cgm_cache_block_invalid;

					//route to CPU L2s
					message_packet->downgrade_ack = 2;

					//set message package size
					message_packet->size = HEADER_SIZE;

					//fwd nack to requesting core
					//update routing headers swap dest and src
					//requesting node
					message_packet->dest_name = str_map_value(node_strn_map, message_packet->src_id);
					message_packet->dest_id = str_map_string(node_strn_map, message_packet->src_name);

					//owning node L2
					message_packet->src_name = cache->name;
					message_packet->src_id = str_map_string(node_strn_map, cache->name);

					//transmit nack to L2
					cache_put_io_down_queue(cache, message_packet);

					////////
					//part 2
					////////

					//create downgrade_nack
					nack_packet = packet_create();
					assert(nack_packet);

					init_downgrade_nack_packet(nack_packet, message_packet->address);

					//tell L3 what the block state is in the GPU
					//note the GPU is waiting on a put_clnx or putx
					nack_packet->cache_block_state = cgm_cache_block_exclusive; //set exclusive so the L3 waits for the retry

					//for debugging
					nack_packet->access_id = message_packet->access_id;

					//transmit block to L3
					list_enqueue(cache->Tx_queue_bottom, nack_packet);
					advance(cache->cache_io_down_ec);*/

					/*set the bit in the ort table to 0*/
					ort_set_pending_join_bit(cache, ort_status, message_packet->tag, message_packet->set);

					message_packet->downgrade_pending = 1;

					/*drop into the pending request buffer*/
					message_packet =  list_remove(cache->last_queue, message_packet);
					list_enqueue(cache->pending_request_buffer, message_packet);
				}
				else
				{

					/* The block was evicted silently and should not be in the L1 cache.
					 * However, the block may be in L1 D's write back or in the pipe between L1 D and L2.
					 * We have to send a flush to L1 D to make sure the block is really out of there before proceeding.*/
					/*assert(dirty_in_core == 0);*/

					/*this doesn't work for the GPU, its possible for some shared blocks to still be flushing
					this approach assumes that L1s will be flushed before the fwd makes it to the requesting CPU core*/
					error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, message_packet->address);
					if(error == 1)
					{
						warning("block 0x%08x %s get_fwd block in l1 but not in l2 pending join ID %llu type %d state %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name,message_packet->access_id,
							message_packet->access_type, *cache_block_state_ptr, P_TIME);
					}


					/*two part reply (1) send nack to L3 and (2) send nack to requesting L2*/

					//set access type
					message_packet->access_type = cgm_access_downgrade_nack;

					//set the block state
					message_packet->cache_block_state = cgm_cache_block_invalid;

					//route to CPU L2s
					message_packet->downgrade_ack = 2;

					//set message package size
					message_packet->size = HEADER_SIZE;

					//fwd nack to requesting core
					//update routing headers swap dest and src
					//requesting node
					message_packet->dest_name = str_map_value(node_strn_map, message_packet->src_id);
					message_packet->dest_id = str_map_string(node_strn_map, message_packet->src_name);


					//owning node L2
					message_packet->src_name = cache->name;
					message_packet->src_id = str_map_string(node_strn_map, cache->name);

					//transmit nack to L2
					cache_put_io_down_queue(cache, message_packet);

					////////
					//part 2
					////////

					//create downgrade_nack
					nack_packet = packet_create();
					assert(nack_packet);

					init_downgrade_nack_packet(nack_packet, message_packet->address);

					//tell L3 what the block state is in the GPU
					nack_packet->cache_block_state = cgm_cache_block_invalid;

					//for debugging
					nack_packet->access_id = message_packet->access_id;

					//transmit block to L3
					list_enqueue(cache->Tx_queue_bottom, nack_packet);
					advance(cache->cache_io_down_ec);
				}

			}

			break;

		case cgm_cache_block_exclusive:
		case cgm_cache_block_modified:

			//a GET/GETX_FWD means the block is E/M in this core. The block will be E/M in the L1(s)
			//We need to either downgrade or evict (getx_fwd_inval) the blocks in the CUs based on the fwd type

			//There may be more then one CU with the block if the block is dirty in core but shared

			//ok we have a get_fwd, down grade all holding cores.


			/*fatal("starting get_fwd 0x%08x\n", (message_packet->address & cache->block_address_mask));*/
			/*if(sharers != 1 || pending_bit != 0)
				fatal("gpu l2 get_fwd caught a problem here holders %d pending %d\n", sharers, pending_bit);*/

			assert((sharers == 1 || sharers == 0) && pending_bit == 0);

			if(sharers == 1)
			{
				///////////////////
				//Directory actions
				///////////////////

				//set the directory pending bit.
				cgm_cache_set_dir_pending_bit(cache, message_packet->set, message_packet->way);


				/*if((message_packet->address & cache->block_address_mask) == 0x0812b300)
					printf("get_fwd access start type is %s source name %s id %d access id %llu\n",
							str_map_value(&cgm_mem_access_strn_map, message_packet->access_type),
							message_packet->src_name,
							message_packet->src_id,
							message_packet->access_id);*/

				//store the getx_fwd in the pending request buffer
				message_packet->downgrade = 1;
				cgm_cache_insert_pending_request_buffer(cache, message_packet);

				//set the flush_pending bit to 1 in the block
				/*cgm_cache_set_block_flush_pending_bit(cache, message_packet->set, message_packet->way);*/

				//downgrade the holding L1 cache, note line maybe dirty
				downgrade_packet = packet_create();
				assert(downgrade_packet);
				init_downgrade_packet(downgrade_packet, message_packet->address);

				/*//find out who to send it to
				unsigned long long bit_vector;

				//get the presence bits from the directory
				bit_vector = cache->sets[message_packet->set].blocks[message_packet->way].directory_entry.entry;
				bit_vector = bit_vector & cache->share_mask;*/

				//get the id of the owning core L1
				downgrade_packet->l1_cache_id = cgm_cache_get_xown_core(gpu, cache, message_packet->set, message_packet->way);

				//send the L1 D cache the inval message
				downgrade_packet->gpu_access_type = cgm_access_store;
				downgrade_packet->access_id = message_packet->access_id;
				list_enqueue(cache->Tx_queue_top, downgrade_packet);
				advance(cache->cache_io_up_ec);
			}
			else
			{
				//directory shows no CU has the block process puts immediately.
				//this can occur if a holding CU sends a write-back which
				//results in the directory being empty. Also only possible in E/M state
				//Means the block is only stored in the L2 cache.

				GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s get_fwd zero shared processing now ID %llu type %d state %d wb? %d cycle %llu\n",
					(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
					message_packet->access_type, *cache_block_state_ptr, is_writeback_present(write_back_packet), P_TIME);

				/*fatal("made it here 0x%08x\n", (message_packet->address & cache->block_address_mask));*/
				assert(sharers == 0);
				assert(message_packet->access_type == cgm_access_get_fwd);

				//forwarding block to requesting CPU core in the modified state
				//the block better be out of all GPU L1 caches.
				error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, message_packet->address);
				assert(error == 0);

				///////////////////
				//Directory actions
				///////////////////

				//clear the directory (clears pending bit)
				cgm_cache_clear_dir(cache, message_packet->set, message_packet->way);

				//check/clear the dirty in core bit (this could be dirty in core if shared within the GPU)
				assert(cgm_cache_get_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way) == 0);

				//downgrade the local block
				cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_shared);


				//////////
				//GET_FWD
				//////////

				//prepare to forward the block
				message_packet->size = packet_set_size(cache->block_size);
				message_packet->cache_block_state = cgm_cache_block_shared;

				//set access type
				message_packet->access_type = cgm_access_puts;

				//fwd block to requesting core
				//update routing headers swap dest and src
				//requesting node
				message_packet->dest_name = str_map_value(node_strn_map, message_packet->src_id);
				message_packet->dest_id = str_map_string(node_strn_map, message_packet->src_name);

				//owning node L2
				//pending_get_getx_fwd_request->src_name = cache->name;
				//pending_get_getx_fwd_request->src_id = str_map_string(node_strn_map, cache->name);

				//transmit block to requesting node
				cache_put_io_down_queue(cache, message_packet);


				///////////////
				//get_fwd_ack
				///////////////

				//send the get_fwd_ack to L3 cache.
				//create and init get__fwd_ack packet
				reply_packet = packet_create();
				assert(reply_packet);

				//set access type
				init_downgrade_ack_packet(reply_packet, message_packet->address);

				//date is dirty in core so send down to L3
				if(dirty_in_core)
				{
					//set size
					reply_packet->size = packet_set_size(cache->block_size);

					//set block state
					reply_packet->cache_block_state = cgm_cache_block_modified;
				}
				else
				{
					reply_packet->size = HEADER_SIZE;
				}


				//send reply to L3
				//fakes src as the requester
				reply_packet->l2_cache_id = message_packet->l2_cache_id;
				reply_packet->l2_cache_name = message_packet->src_name;

				//assert(get_getx_fwd_reply_packet->access_type == cgm_access_downgrade_ack);
				//transmit getx_fwd_ack to L3 (home)
				list_enqueue(cache->Tx_queue_bottom, reply_packet);
				advance(cache->cache_io_down_ec);

				}

			break;

		case cgm_cache_block_shared:

			//the block is actually in the E/M state at L3, but has been shared within the GPU

			//If the block is dirty_in_core we need to process a sharing write-back.

			/*the block is shared throughout the GPU, but may be dirty in core
			process PutS immediately, but with a sharing write-back if dirty in core*/

			assert(sharers >= 1 && pending_bit == 0);

			//forward block to requesting core

			///////////////////
			//Directory actions
			///////////////////

			//all we need to do is clear the dirty in core bit
			cgm_cache_clear_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);


			//////////
			//GET_FWD
			//////////

			//prepare to forward the block
			message_packet->size = packet_set_size(l2_caches[str_map_string(&l2_strn_map, message_packet->l2_cache_name)].block_size);
			message_packet->cache_block_state = cgm_cache_block_shared;

			//set access type
			message_packet->access_type = cgm_access_puts;

			//fwd block to requesting core
			//update routing headers swap dest and src
			//requesting node
			message_packet->dest_name = str_map_value(node_strn_map, message_packet->src_id);
			message_packet->dest_id = str_map_string(node_strn_map, message_packet->src_name);

			//owning node L2
			//pending_get_getx_fwd_request->src_name = cache->name;
			//pending_get_getx_fwd_request->src_id = str_map_string(node_strn_map, cache->name);

			//transmit block to requesting node
			cache_put_io_down_queue(cache, message_packet);


			///////////////
			//get_fwd_ack
			///////////////

			//send the get_fwd_ack to L3 cache.
			//create and init get__fwd_ack packet
			reply_packet = packet_create();
			assert(reply_packet);

			//set access type
			init_downgrade_ack_packet(reply_packet, message_packet->address);

			//date is dirty in core so send down to L3
			if(dirty_in_core)
			{
				//set size
				reply_packet->size = packet_set_size(cache->block_size);

				//set block state
				reply_packet->cache_block_state = cgm_cache_block_modified;
			}
			else
			{
				reply_packet->size = HEADER_SIZE;
			}



			//send reply to L3
			//fakes src as the requester
			reply_packet->l2_cache_id = message_packet->l2_cache_id;
			reply_packet->l2_cache_name = message_packet->src_name;

			//assert(get_getx_fwd_reply_packet->access_type == cgm_access_downgrade_ack);
			//transmit getx_fwd_ack to L3 (home)
			list_enqueue(cache->Tx_queue_bottom, reply_packet);
			advance(cache->cache_io_down_ec);

			break;
	}

	return;
}

void cgm_mesi_gpu_l2_upgrade_inval(struct cache_t *cache, struct cgm_packet_t *message_packet){


	//fatal("GPU caught upgrade inval\n");

	//Invalidation/eviction request from L3 cache

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;
	//int l3_map = 0;
	int ort_status = -1;

	int num_cus = si_gpu_num_compute_units;

	int dirty_in_core = 0;
	int pending_bit = 0;
	int sharers = 0;

	struct cgm_packet_t *wb_packet = NULL;

	//charge delay
	GPU_PAUSE(cache->latency);

	//get the block status
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	//if not dropped by the GPU the block should be in S state only
	if(*cache_block_hit_ptr == 1)
		assert(*cache_block_state_ptr == cgm_cache_block_shared);

	//check writeback
	wb_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);
	assert(!wb_packet); //shouldn't have a write back packet.

	//get number of sharers
	sharers = cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way);
	assert(sharers >= 0 && sharers <= num_cus);

	pending_bit = cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way);

	if(*cache_block_hit_ptr == 1)
		assert(pending_bit == 0);

	dirty_in_core = cgm_cache_get_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);

	if(*cache_block_hit_ptr == 1)
		assert(dirty_in_core == 0); //on an upgrade inval this should not be dirty in core

	CPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s upgrade inval ID %llu type %d state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, *cache_block_state_ptr, P_TIME);

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);

	/*check the ORT table for an outstanding access*/
	ort_status = ort_search(cache, message_packet->tag, message_packet->set);
	if(ort_status != cache->mshr_size)
	{
		assert(*cache_block_hit_ptr == 0);
		//block silently dropped by GPU, the

		/*yep there is so set the bit in the ort table to 0.
		 * When the put/putx comes kill it and try again...*/
		ort_set_pending_join_bit(cache, ort_status, message_packet->tag, message_packet->set);
	}

	/*//search the WB buffer for the data
	write_back_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);*/
	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_noncoherent:
		case cgm_cache_block_owned:
			case cgm_cache_block_exclusive:
		case cgm_cache_block_modified:
			fatal("cgm_mesi_gpu_l2_upgrade_inval(): L2 id %d invalid block state on upgrade inval as %s address %u\n",
				cache->id, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr), message_packet->address);
			break;

		case cgm_cache_block_invalid:


			/*warning("block 0x%08x %s upgrade inval but block not in GPU L2 ID %llu type %d state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask),
			cache->name,
			message_packet->access_id,
			message_packet->access_type,
			*cache_block_state_ptr,
			P_TIME);*/

			//block isn't in the GPU just send the ack along

			//send ack to requesting core.
			assert(message_packet->access_type == cgm_access_upgrade_inval
					&& ( message_packet->upgrade == 1 || message_packet->upgrade_putx_n == 1));

			//transmit ack to requesting L2 cache
			if(message_packet->upgrade == 1)
			{
				message_packet->access_type = cgm_access_upgrade_ack;
			}
			else if(message_packet->upgrade_putx_n == 1)
			{
				message_packet->access_type = cgm_access_upgrade_putx_n;
			}
			else
			{
				fatal("cgm_mesi_gpu_l2_upgrade_inval(): invalid upgrade type set\n");
			}


			message_packet->upgrade_ack = -1;
			message_packet->upgrade_inval_ack_count = 0;

			message_packet->dest_name = message_packet->src_name;
			message_packet->dest_id = message_packet->src_id;

			/*printf("gpu l2 finish upgrade ack dest %s id %d\n",
					message_packet->dest_name,
					message_packet->dest_id);*/

			message_packet->src_name = cache->name;
			message_packet->src_id = str_map_string(node_strn_map, cache->name);

			//transmit block to requesting node
			cache_put_io_down_queue(cache, message_packet);

			break;

		case cgm_cache_block_shared:

			//note this assumes that the blocks will be evicted from the GPU prior to the CPU writing the data

			//evict the block right here and now
			cgm_L2_cache_evict_block(cache, message_packet->set, message_packet->way,
					cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way), 0, 0);

			//make sure the various flags are good.
			cgm_cache_clear_dir(cache, message_packet->set, message_packet->way);
			//cgm_cache_clear_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);

			//send ack to requesting core.
			assert(message_packet->access_type == cgm_access_upgrade_inval
					&& ( message_packet->upgrade == 1 || message_packet->upgrade_putx_n == 1));

			//transmit ack to requesting L2 cache
			if(message_packet->upgrade == 1)
			{
				message_packet->access_type = cgm_access_upgrade_ack;
			}
			else if(message_packet->upgrade_putx_n == 1)
			{
				message_packet->access_type = cgm_access_upgrade_putx_n;
			}
			else
			{
				fatal("cgm_mesi_gpu_l2_upgrade_inval(): invalid upgrade type set\n");
			}

			//block is evicted, send ack to requesting L2
			message_packet->upgrade_ack = -1;
			message_packet->upgrade_inval_ack_count = 0;

			message_packet->dest_name = message_packet->src_name;
			message_packet->dest_id = message_packet->src_id;

			/*printf("gpu l2 finish upgrade ack dest %s id %d\n",
					message_packet->dest_name,
					message_packet->dest_id);*/

			message_packet->src_name = cache->name;
			message_packet->src_id = str_map_string(node_strn_map, cache->name);

			//transmit block to requesting node
			cache_put_io_down_queue(cache, message_packet);

			break;
	}

	return;
}

void cgm_mesi_gpu_l2_downgrade_nack(struct cache_t *cache, struct cgm_packet_t *message_packet){


	/*warning("GPU L2 caught downgrade nack blk 0x%08x cycle %llu\n",
			(message_packet->address & cache->block_address_mask),
			P_TIME);*/

	//Received a downgrade nack, we need to retry the access as a get to L3

	//block should be transient
	//block should NOT be pending

	int set = 0;
	int tag = 0;
	unsigned int offset = 0;
	int way = 0;

	int *set_ptr = &set;
	int *tag_ptr = &tag;
	unsigned int *offset_ptr = &offset;
	// *way_ptr = &way;

	//int l3_map = 0;
	int ort_status = 0;
	//struct cgm_packet_t *pending_packet;
	enum cgm_cache_block_state_t victim_trainsient_state;

	//struct cgm_packet_t *pending_get_getx_fwd_request = NULL;

	//probe the address for set, tag, and offset.
	cgm_cache_probe_address(cache, message_packet->address, set_ptr, tag_ptr, offset_ptr);

	//store the decode in the packet for now.
	message_packet->tag = tag;
	message_packet->set = set;
	message_packet->offset = offset;
	message_packet->way = way;

	//charge delay
	GPU_PAUSE(cache->latency);

	/*our load (getx_fwd) request has been nacked by the owning L2*/

	/*verify status of cache blk*/
	victim_trainsient_state = cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->l2_victim_way);
	assert(victim_trainsient_state == cgm_cache_block_transient); //there is a transient block
	if(victim_trainsient_state != cgm_cache_block_transient)
	{
		ort_dump(cache);
		cgm_cache_dump_set(cache, message_packet->set);

		unsigned int temp = message_packet->address;
		temp = temp & cache->block_address_mask;

		fatal("cgm_mesi_l2_getx_fwd_nack(): %s block not in transient state access_id %llu address 0x%08x blk_addr 0x%08x set %d tag %d way %d cycle %llu\n",
			cache->name, message_packet->access_id, message_packet->address, temp,
			message_packet->set, message_packet->tag, message_packet->way, P_TIME);
	}


	ort_status = ort_search(cache, message_packet->tag, message_packet->set); // there is an outstanding request.
	assert(ort_status <= cache->mshr_size);

	GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s downgrade nack ID %llu type %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, P_TIME);

	//change to a get
	message_packet->access_type = cgm_access_get;
	message_packet->cpu_access_type = cgm_access_load;
	message_packet->cache_block_state = cgm_cache_block_exclusive;

	//L3 should see the entire GPU as a single core.
	message_packet->l2_cache_id = gpu_core_id;
	message_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);

	cache_put_io_down_queue(cache, message_packet);

	return;
}

void cgm_mesi_gpu_l2_getx_fwd_nack(struct cache_t *cache, struct cgm_packet_t *message_packet){

	//warning("GPU L2 caught getx_fwd nack blk 0x%08x\n", (message_packet->address & cache->block_address_mask));

	/*warning("GPU L2 caught downgrade nack blk 0x%08x cycle %llu\n",
			(message_packet->address & cache->block_address_mask),
			P_TIME);*/

	//Received a downgrade nack, we need to retry the access as a get to L3

	//block should be transient
	//block should NOT be pending

	int set = 0;
	int tag = 0;
	unsigned int offset = 0;
	int way = 0;

	int *set_ptr = &set;
	int *tag_ptr = &tag;
	unsigned int *offset_ptr = &offset;
	// *way_ptr = &way;

	//int l3_map = 0;
	int ort_status = 0;
	//struct cgm_packet_t *pending_packet;
	enum cgm_cache_block_state_t victim_trainsient_state;

	//struct cgm_packet_t *pending_get_getx_fwd_request = NULL;

	//probe the address for set, tag, and offset.
	cgm_cache_probe_address(cache, message_packet->address, set_ptr, tag_ptr, offset_ptr);

	//store the decode in the packet for now.
	message_packet->tag = tag;
	message_packet->set = set;
	message_packet->offset = offset;
	message_packet->way = way;

	//charge delay
	GPU_PAUSE(cache->latency);

	/*our load (getx_fwd) request has been nacked by the owning L2*/

	/*verify status of cache blk*/
	victim_trainsient_state = cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->l2_victim_way);
	assert(victim_trainsient_state == cgm_cache_block_transient); //there is a transient block
	if(victim_trainsient_state != cgm_cache_block_transient)
	{
		ort_dump(cache);
		cgm_cache_dump_set(cache, message_packet->set);

		unsigned int temp = message_packet->address;
		temp = temp & cache->block_address_mask;

		fatal("cgm_mesi_l2_getx_fwd_nack(): %s block not in transient state access_id %llu address 0x%08x blk_addr 0x%08x set %d tag %d way %d cycle %llu\n",
			cache->name, message_packet->access_id, message_packet->address, temp,
			message_packet->set, message_packet->tag, message_packet->way, P_TIME);
	}


	ort_status = ort_search(cache, message_packet->tag, message_packet->set); // there is an outstanding request.
	assert(ort_status <= cache->mshr_size);

	GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s downgrade nack ID %llu type %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, P_TIME);

	//change to a get
	message_packet->access_type = cgm_access_getx;
	message_packet->cpu_access_type = cgm_access_store;
	message_packet->cache_block_state = cgm_cache_block_exclusive;

	//L3 should see the entire GPU as a single core.
	message_packet->l2_cache_id = gpu_core_id;
	message_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);

	cache_put_io_down_queue(cache, message_packet);


	return;
}


int cgm_mesi_gpu_l2_upgrade_putx_n(struct cache_t *cache, struct cgm_packet_t *message_packet){

	/*warning("cgm_mesi_gpu_l2_upgrade_putx_n(): blk 0x%08x caught message num acks %d\n",
			message_packet->address & cache->block_address_mask,
			message_packet->upgrade_ack);*/

	//not an upgrade in the
	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	struct cgm_packet_t *pending_packet = NULL;
	struct cgm_packet_t *pending_packet_join = NULL;
	struct cgm_packet_t *putx_n_coutner = NULL;

	int i = 0;
	int pending_bit = 0;
	int dirty_in_core = 0;
	int ort_row = 0;
	int conflict_bit = 0;

	enum cgm_cache_block_state_t victim_trainsient_state;

	//charge delay
	GPU_PAUSE(cache->latency);

	//get the status of the cache block
	cache_get_transient_block(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	//check pending state
	pending_bit = cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way);
	assert(pending_bit == 0);

	dirty_in_core = cgm_cache_get_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);
	assert(dirty_in_core == 0);

	//block should be valid and in the transient state
	victim_trainsient_state = cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->way);
	assert(victim_trainsient_state == cgm_cache_block_transient);

	ort_row = ort_search(cache, message_packet->tag, message_packet->set);
	assert(ort_row < cache->mshr_size);
	conflict_bit = cache->ort[ort_row][2];

	/*if(conflict_bit != 1)
		printf("CRASHING!!!!! block 0x%08x %s upgrade_putx_n received ID %llu src %s type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
						message_packet->src_name, message_packet->access_type, *cache_block_state_ptr, P_TIME);
	assert(conflict_bit == 1); //this is ok, just check to make sure I am implemented ok.*/

	GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s upgrade_putx_n received ID %llu src %s type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
						message_packet->src_name, message_packet->access_type, *cache_block_state_ptr, P_TIME);

	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_noncoherent:
		case cgm_cache_block_owned:
		case cgm_cache_block_exclusive:
		case cgm_cache_block_modified:
		case cgm_cache_block_shared:

			cgm_cache_dump_set(cache, message_packet->set);

			fatal("cgm_mesi_gpu_l2_upgrade_putx_n(): %s invalid block state as %s access_id %llu address 0x%08x blk_addr 0x%08x set %d tag %d way %d state %d cycle %llu\n",
				cache->name, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr),
				message_packet->access_id, message_packet->address, message_packet->address & cache->block_address_mask,
				message_packet->set, message_packet->tag, message_packet->way, *cache_block_state_ptr, P_TIME);

			/*fatal("cgm_mesi_l2_upgrade_putx_n(): L2 id %d invalid block state on upgrade putx n as %s address %u\n",
				cache->id, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr), message_packet->address);*/
			break;

		case cgm_cache_block_invalid:

			/*putx_n occurs when a miss in L2 is a hit in L3 as shared
			the line comes from L3, and invalidation acks come from the other L2 caches
			we need to process a join. messages can be received in any order.*/

			//it is possible that an upgrade_inval_ack can be received from a responding L2 before the L3 cache.

			//check if we have already stored a pending request
			LIST_FOR_EACH(cache->pending_request_buffer, i)
			{
				//get pointer to access in queue and check it's status.
				pending_packet = list_get(cache->pending_request_buffer, i);

				if(((pending_packet->address & cache->block_address_mask) == (message_packet->address & cache->block_address_mask))
						&& pending_packet->access_type == cgm_access_upgrade_putx_n)
				{
					break;
				}
				else
				{
					pending_packet = NULL;
				}
			}


			/*if not found this is the first reply access*/
			if(!pending_packet)
			{
				//message from L3
				if(message_packet->upgrade_ack >= 0)
				{
					//insert pending packet into pending buffer free the L3's upgrade_ack message packet
					pending_packet = list_remove(cache->last_queue, message_packet);

					pending_packet->upgrade_inval_ack_count = message_packet->upgrade_ack;

					list_enqueue(cache->pending_request_buffer, pending_packet);

					//now wait for L2 replies
				}
				else if(message_packet->upgrade_ack < 0)
				{
					//L2 ack beat the L3 reply
					pending_packet = list_remove(cache->last_queue, message_packet);

					pending_packet->upgrade_inval_ack_count--;

					list_enqueue(cache->pending_request_buffer, pending_packet);
				}
				else
				{
					fatal("cgm_mesi_l2_upgrade_ack(): bad upgrade_ack counter value\n");
				}
			}
			/*if found we have received a reply*/
			else if (pending_packet)
			{
				//message from L3
				if(message_packet->upgrade_ack >= 0)
				{
					//L2 beat L3 swap the packet in the buffer
					pending_packet = list_remove(cache->pending_request_buffer, pending_packet);

					//adjust the counter
					message_packet->upgrade_inval_ack_count = pending_packet->upgrade_inval_ack_count + message_packet->upgrade_ack;

					//free the l2 ack message
					packet_destroy(pending_packet);

					//now wait for L2 replies
					message_packet = list_remove(cache->last_queue, message_packet);
					list_enqueue(cache->pending_request_buffer, message_packet);

					//assign the pointer so we can check the total below.
					putx_n_coutner = message_packet;

				}
				else if(message_packet->upgrade_ack < 0)
				{
					//L3 ack beat the L2 reply
					pending_packet->upgrade_inval_ack_count--;

					//free the L2 ack
					message_packet = list_remove(cache->last_queue, message_packet);
					packet_destroy(message_packet);

					//assign the pointer so we can check the total below.
					putx_n_coutner = pending_packet;
				}
				else
				{
					fatal("cgm_mesi_l2_upgrade_ack(): bad upgrade_ack counter value\n");
				}


				if(putx_n_coutner->upgrade_inval_ack_count == 0)
				{

					///////////
					//join
					///////////

					//we have received the L3 reply and the reply(s) from the other L2(s)

					//check the conflict bit
					//0 means there is a conflict!!!
					if(conflict_bit == 0)
					{

						warning("GPU l2 conflict on putx_n retry needs implementation\n");


						/*We have a conflict, either a pending get/getx_fwd or L3 has evicted the block*/

						/*warning("block 0x%08x %s upgrade_putx_n conflict caught ID %llu type %d state %d cycle %llu\n",
								(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, *cache_block_state_ptr, P_TIME);*/

						GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s upgrade_putx_n conflict caught ID %llu type %d state %d cycle %llu\n",
								(putx_n_coutner->address & cache->block_address_mask), cache->name, putx_n_coutner->access_id, putx_n_coutner->access_type, *cache_block_state_ptr, P_TIME);

						//check the pending request buffer for an outstanding get/getx_fwd sigh.....
						LIST_FOR_EACH(cache->pending_request_buffer, i)
						{
							//get pointer to access in queue and check it's status.
							pending_packet_join = list_get(cache->pending_request_buffer, i);

							if(((pending_packet_join->address & cache->block_address_mask) == (pending_packet->address & cache->block_address_mask))
									&& (pending_packet_join->access_type == cgm_access_get_fwd || pending_packet_join->access_type == cgm_access_getx_fwd))
							{
								break;
							}
							else
							{
								pending_packet_join = NULL;
							}

						}

						if(pending_packet_join)
						{
							//there is a pending get/getx_fwd

							warning("GPU l2 conflict joined get/getx_fwd\n");

							pending_packet_join = list_remove(cache->pending_request_buffer, pending_packet_join);
							list_enqueue(cache->retry_queue, pending_packet_join);
							advance(cache->ec_ptr);

							GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s get/getx_fwd (join) joined in upgrade_putx_n ID %llu src %s type %d state %d cycle %llu\n",
									(pending_packet_join->address & cache->block_address_mask), cache->name, pending_packet_join->access_id, pending_packet_join->src_name,
									pending_packet_join->access_type, *cache_block_state_ptr, P_TIME);
						}
						else
						{
							/*block conflict case, L3 has evicted the block during the putx_n epoch
							kill this and retry as a new getx.*/

							GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s putx_n killed by conflict retrying as getx (from L2) ID %llu type %d state %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, *cache_block_state_ptr, P_TIME);

							assert(putx_n_coutner->data);
							assert(putx_n_coutner->event_queue);
							assert(putx_n_coutner->address == message_packet->address);
							assert(putx_n_coutner->set == message_packet->set);
							assert(putx_n_coutner->tag == message_packet->tag);
							assert(ort_row < cache->mshr_size);
							assert(conflict_bit == 0);

							/*block should be transient*/
							assert(cgm_cache_get_block_transient_state(cache, putx_n_coutner->set, putx_n_coutner->way) == cgm_cache_block_transient);

							/*clear the conflict bit because we are retrying*/
							ort_clear_pending_join_bit(cache, ort_row, putx_n_coutner->tag, putx_n_coutner->set);

							putx_n_coutner->access_type = cgm_access_getx;
							putx_n_coutner->cpu_access_type = cgm_access_store;

							/*l3_cache_ptr = cgm_l3_cache_map(putx_n_coutner->set);*/
							putx_n_coutner->l2_cache_id = gpu_core_id;
							putx_n_coutner->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

							/*SETROUTE(putx_n_coutner, cache, l3_cache_ptr);*/

							//transmit to L3
							putx_n_coutner = list_remove(cache->pending_request_buffer, putx_n_coutner);
							list_enqueue(cache->Tx_queue_bottom, putx_n_coutner);
							advance(cache->cache_io_down_ec);

							/*message_packet = list_remove(cache->last_queue, message_packet);
							free(message_packet);*/

							/*old code*/
							//change to a getx
							/*message_packet->access_type = cgm_access_getx;
							message_packet->cpu_access_type = pending_packet->cpu_access_type;
							message_packet->l1_access_type = pending_packet->l1_access_type;
							message_packet->l1_victim_way = pending_packet->l1_victim_way;
							message_packet->event_queue = pending_packet->event_queue;
							message_packet->data = pending_packet->data;
							message_packet->access_id = pending_packet->access_id;
							message_packet->name = strdup(pending_packet->name);
							message_packet->start_cycle = pending_packet->start_cycle;
							assert(pending_packet->address == message_packet->address);*/

							return 1;
						}
					}

					//last thing is to clear this access in the ORT table.
					ort_clear(cache, putx_n_coutner);

					//clear the upgrade_pending bit in the block GPU should be clear
					assert(cgm_cache_get_block_upgrade_pending_bit(cache, putx_n_coutner->set, putx_n_coutner->way) == 0);

					//set local cache block to modified.
					cgm_cache_set_block(cache, putx_n_coutner->set, putx_n_coutner->way, putx_n_coutner->tag, cgm_cache_block_modified);

					GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s upgrade_putx_n blk upgraded! ID %llu src %s type %d state %d cycle %llu\n",
						(putx_n_coutner->address & cache->block_address_mask), cache->name, putx_n_coutner->access_id,
						putx_n_coutner->src_name, putx_n_coutner->access_type, *cache_block_state_ptr, P_TIME);

					//change to GetX
					assert(putx_n_coutner->l1_access_type == cgm_access_getx);

					//set the access type and what the block state should be.
					/*star todo; figure out if we need to eliminate the dependence on the l1_access_type.*/

					//set retry state
					putx_n_coutner->access_type = cgm_cache_get_retry_state(putx_n_coutner->gpu_access_type);

					//pull the pending request from the pending request buffer
					putx_n_coutner = list_remove(cache->pending_request_buffer, putx_n_coutner);
					list_enqueue(cache->retry_queue, putx_n_coutner);

					return 0;
				}
			}
			else
			{
				fatal("cgm_mesi_l2_upgrade_putx_n(): putx n packet trouble\n");
			}

			break;
	}

	return 1;
}

void cgm_mesi_gpu_l2_getx_fwd(struct cache_t *cache, struct cgm_packet_t *message_packet){

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	int num_cus = si_gpu_num_compute_units;

	struct cgm_packet_t *getx_inval_packet = NULL;
	struct cgm_packet_t *write_back_packet = NULL;
	struct cgm_packet_t *nack_packet = NULL;
	struct cgm_packet_t *reply_packet = NULL;

	int sharers, pending_bit, dirty_in_core;//, owning_core;

	int error = 0;
	int ort_status = 0;
	//int l3_map;

	//charge delay
	GPU_PAUSE(cache->latency);

	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);

	//search the WB buffer for the data
	write_back_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

	//get number of sharers
	sharers = cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way);
	assert(sharers >= 0 && sharers <= num_cus);
	//check to see if access is from an already owning core
	//owning_core = cgm_cache_is_owning_core(cache, message_packet->set, message_packet->way, message_packet->l1_cache_id);
	//check pending state
	pending_bit = cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way);
	/*if(pending_bit)
		fatal("cgm_mesi_gpu_l2_get_fwd(): pending bit is 1 hit ptr %d block 0x%08x vtl addr 0x%08x\n",
				*cache_block_hit_ptr, message_packet->address & cache->block_address_mask, message_packet->vtladdress & cache->block_address_mask);*/
	assert(pending_bit == 0 || pending_bit == 1); //fix me if this happens, nack maybe?

	//see if the block is dirty in core.
	dirty_in_core = cgm_cache_get_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);
	assert(dirty_in_core >= 0 && dirty_in_core <= 1); //when this is 1 check that it is being written back to L3 as a sharing write-back

	GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s getx_fwd ID %llu type %d state %d wb? %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
			message_packet->access_type, *cache_block_state_ptr, is_writeback_present(write_back_packet), P_TIME);


	/*look for an access conflict, this can happen if a get_fwd beats a putx/putx_n*/
	ort_status = ort_search(cache, message_packet->tag, message_packet->set);
	if(ort_status != cache->mshr_size)
	{

		warning("cgm_mesi_gpu_l2_getx_fwd(): access conflict need to process join block 0x%08x vtl addr 0x%08x cycle %llu\n",
				message_packet->address & cache->block_address_mask, message_packet->vtladdress & cache->block_address_mask, P_TIME);

		/*if there is a pending access int the ORT there better not be a block or a write back*/
		if(*cache_block_state_ptr == cgm_cache_block_invalid)
			assert(!write_back_packet);

		/*if(cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->way) == cgm_cache_block_transient)
			assert(*cache_block_state_ptr == cgm_cache_block_shared && *cache_block_hit_ptr == 1);*/
	}

	//directory is busy nack the request back
	if(pending_bit == 1 && *cache_block_hit_ptr == 1)
	{
		//directory is busy with a block movement, need to nack the request.

		/*fatal("block 0x%08x %s pending at gpu l2 getx_fwd nacked back to requesting L2 id %llu state %d cycle %llu\n",
				(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, *cache_block_state_ptr, P_TIME);*/

		GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s pending at gpu l2 get nacked back to requesting L2 id %llu state %d cycle %llu\n",
				(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, *cache_block_state_ptr, P_TIME);

		/*two part reply (1) send nack to L3 and (2) send nack to requesting L2*/
		//set access type
		message_packet->access_type = cgm_access_getx_fwd_nack;

		//star fixme there has to be a better way to do this.
		//set this to two so the hub can sort out its destination
		message_packet->inval_ack = 2;

		//set the block state
		message_packet->cache_block_state = cgm_cache_block_invalid;

		//set message package size
		message_packet->size = HEADER_SIZE;

		//fwd nack to requesting core
		//update routing headers swap dest and src
		//requesting node
		message_packet->dest_name = str_map_value(node_strn_map, message_packet->src_id);
		message_packet->dest_id = str_map_string(node_strn_map, message_packet->src_name);

		//owning node L2
		message_packet->src_name = cache->name;
		message_packet->src_id = str_map_string(node_strn_map, cache->name);

		//transmit nack to L2
		cache_put_io_down_queue(cache, message_packet);

		////////
		//part 2
		////////

		//L3 should see the entire GPU as a single core.

		//create downgrade_nack
		nack_packet = packet_create();
		assert(nack_packet);

		init_getx_fwd_nack_packet(nack_packet, message_packet->address);
		nack_packet->cache_block_state = *cache_block_state_ptr; //let L3 know what the block state is
		assert(nack_packet->cache_block_state == cgm_cache_block_exclusive || nack_packet->cache_block_state == cgm_cache_block_modified);
		nack_packet->access_id = message_packet->access_id;

		nack_packet->l2_cache_id = gpu_core_id;
		nack_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

		//transmit block to L3
		list_enqueue(cache->Tx_queue_bottom, nack_packet);
		advance(cache->cache_io_down_ec);

		/*cgm_cache_dump_set(&l3_caches[0], 1156);
		printf("\n");*/

		return;
	}


	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_noncoherent:
		case cgm_cache_block_owned:

			cgm_cache_dump_set(cache, message_packet->set);

			fatal("cgm_mesi_l2_getx_fwd(): %s invalid block state on getx_fwd as %s access_id %llu address 0x%08x blk_addr 0x%08x set %d tag %d way %d state %d cycle %llu\n",
				cache->name, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr),
				message_packet->access_id, message_packet->address, message_packet->address & cache->block_address_mask,
				message_packet->set, message_packet->tag, message_packet->way, *cache_block_state_ptr, P_TIME);
			break;

		case cgm_cache_block_invalid:

			//check the WB buffer
			if(write_back_packet)
			{

				//Star checked April 6 2019

				/*found the packet in the write back buffer
				data should not be in the rest of the cache*/

				assert(*cache_block_hit_ptr == 0);
				assert(write_back_packet->cache_block_state == cgm_cache_block_modified
						|| write_back_packet->cache_block_state == cgm_cache_block_exclusive);

				/*check state of write back for flush*/
				if(write_back_packet->flush_pending == 0)
				{

					warning("l2 getx_fwd with WB make sure I am good to go phy addr 0x%08x vtl addr 0x%08x\n",
							message_packet->address & cache->block_address_mask, message_packet->vtladdress & cache->block_address_mask);


					/*the flush is complete finish the get_fwd now*/

					/*flush is complete for this write back so it should not be up in L1 D*/
					error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, message_packet->address);
					assert(error == 0);

					//////////
					//GETX_FWD
					//////////

					//forward block to requesting core

					//set access type
					message_packet->access_type = cgm_access_putx;

					message_packet->cache_block_state = cgm_cache_block_modified;

					//prepare to forward the block
					message_packet->size = packet_set_size(cache->block_size);

					//fwd block to requesting core
					//update routing headers swap dest and src
					//requesting node
					message_packet->dest_name = str_map_value(node_strn_map, message_packet->src_id);
					message_packet->dest_id = str_map_string(node_strn_map, message_packet->src_name);

					//owning node L2
					message_packet->src_name = cache->name;
					message_packet->src_id = str_map_string(node_strn_map, cache->name);

					//transmit block to requesting node
					message_packet = list_remove(cache->last_queue, message_packet);
					list_enqueue(cache->Tx_queue_bottom, message_packet);
					advance(cache->cache_io_down_ec);

					/////////////
					//getx_fwd_ack
					/////////////

					//send the getx_fwd_ack to L3 cache.

					//create get_fwd_ack packet
					reply_packet = packet_create();
					assert(reply_packet);

					init_getx_fwd_ack_packet(reply_packet, message_packet->address);

					//a flush has completed and the WB is still here so it better be modified
					assert(write_back_packet->cache_block_state == cgm_cache_block_modified);
					reply_packet->size = packet_set_size(cache->block_size);
					reply_packet->cache_block_state = cgm_cache_block_modified;

					//send reply to L3
					//fakes src as the requester L3 uses this to set holding core.
					reply_packet->l2_cache_id = message_packet->l2_cache_id;
					reply_packet->l2_cache_name = message_packet->src_name;
					reply_packet->access_id = message_packet->access_id;

					//dest is set in hub-iommu

					//transmit getx_fwd_ack to L3 (home)
					list_enqueue(cache->Tx_queue_bottom, reply_packet);
					advance(cache->cache_io_down_ec);

					write_back_packet = list_remove(cache->write_back_buffer, write_back_packet);
					packet_destroy(write_back_packet);

				}
				else
				{
					/*write back is in the process of being flushed by L1*/
					assert(write_back_packet->flush_pending == 1);

					warning("cgm_mesi_gpu_l2_getx_fwd(): getx_fwd came to pending flush blk addr 0x%08x\n",
							(message_packet->address & cache->block_address_mask));

					/*if the wb is flush pending we have to wait for the flush to complete and then join there*/

					message_packet->L3_flush_join = 1;
					cgm_cache_insert_pending_request_buffer(cache, message_packet);
				}
			}
			else
			{

				/*fatal("cgm_mesi_gpu_l2_get_getx_fwd(): blk miss in l2 shouldn't happen yet id %llu access type %d blk_addr 0x%08x\n",
						message_packet->access_id, message_packet->access_type, message_packet->address & cache->block_address_mask);*/

				//printf("\tcgm_mesi_l2_getx_fwd(): no wb blk addr 0x%08x \n", message_packet->address & cache->block_address_mask);
				//fatal("Getx_fwd %s access id %llu blk_addr 0x%08x\n", cache->name, message_packet->access_id, message_packet->address & cache->block_address_mask);

				if(ort_status < cache->mshr_size)
				{
					warning("block 0x%08x %s getx_fwd conflict pending join ID %llu type %d state %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name,message_packet->access_id,
							message_packet->access_type, *cache_block_state_ptr, P_TIME);

					//this is a join issue.
					GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s getx_fwd conflict pending join ID %llu type %d state %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name,message_packet->access_id,
							message_packet->access_type, *cache_block_state_ptr, P_TIME);

					//this can happen if the CPU is requesting the block need to handle this protocol case if it pops up
					// this could just be a simple nack back to requestor.

					/*error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, message_packet->address);
					if(error == 1)
					{
						warning("block 0x%08x %s getx_fwd block in l1 but not in l2 pending join ID %llu type %d state %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name,message_packet->access_id,
							message_packet->access_type, *cache_block_state_ptr, P_TIME);
					}

					two part reply (1) send nack to L3 and (2) send nack to requesting L2
					//set access type

					message_packet->access_type = cgm_access_getx_fwd_nack;

					//set the block state
					message_packet->cache_block_state = cgm_cache_block_invalid;

					//route to CPU L2s
					message_packet->inval_ack = 2;

					//set message package size
					message_packet->size = HEADER_SIZE;

					//fwd nack to requesting core
					//update routing headers swap dest and src
					//requesting node
					message_packet->dest_name = str_map_value(node_strn_map, message_packet->src_id);
					message_packet->dest_id = str_map_string(node_strn_map, message_packet->src_name);

					//owning node L2
					message_packet->src_name = cache->name;
					message_packet->src_id = str_map_string(node_strn_map, cache->name);

					//transmit nack to L2
					cache_put_io_down_queue(cache, message_packet);

					////////
					//part 2
					////////

					//create downgrade_nack
					nack_packet = packet_create();
					assert(nack_packet);

					init_getx_fwd_nack_packet(nack_packet, message_packet->address);

					//tell L3 what the block state is in the GPU
					nack_packet->cache_block_state = cgm_cache_block_invalid;

					//for debugging
					nack_packet->access_id = message_packet->access_id;

					//transmit block to L3
					list_enqueue(cache->Tx_queue_bottom, nack_packet);
					advance(cache->cache_io_down_ec);*/

					/*set the bit in the ort table to 0*/
					ort_set_pending_join_bit(cache, ort_status, message_packet->tag, message_packet->set);

					message_packet->downgrade_pending = 1;

					/*drop into the pending request buffer*/
					message_packet =  list_remove(cache->last_queue, message_packet);
					list_enqueue(cache->pending_request_buffer, message_packet);
				}
				else
				{
					/* The block was evicted silently and should not be in the L1 cache.*/

					/*Its possible for some shared blocks to still be up in L1 that are about to be invalidated
					this approach assumes that L1s will be invalidated before the fwd makes it to the requesting CPU core*/
					error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, message_packet->address);
					if(error == 1)
					{
						warning("block 0x%08x %s getx_fwd block in l1 but not in l2 pending join ID %llu type %d state %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name,message_packet->access_id,
							message_packet->access_type, *cache_block_state_ptr, P_TIME);
					}

					/*two part reply (1) send nack to L3 and (2) send nack to requesting L2*/
					//set access type

					message_packet->access_type = cgm_access_getx_fwd_nack;

					//set the block state
					message_packet->cache_block_state = cgm_cache_block_invalid;

					//route to CPU L2s
					message_packet->inval_ack = 2;

					//set message package size
					message_packet->size = HEADER_SIZE;

					//fwd nack to requesting core
					//update routing headers swap dest and src
					//requesting node
					message_packet->dest_name = str_map_value(node_strn_map, message_packet->src_id);
					message_packet->dest_id = str_map_string(node_strn_map, message_packet->src_name);

					//owning node L2
					message_packet->src_name = cache->name;
					message_packet->src_id = str_map_string(node_strn_map, cache->name);

					//transmit nack to L2
					cache_put_io_down_queue(cache, message_packet);

					////////
					//part 2
					////////

					//create downgrade_nack
					nack_packet = packet_create();
					assert(nack_packet);

					init_getx_fwd_nack_packet(nack_packet, message_packet->address);

					//tell L3 what the block state is in the GPU
					nack_packet->cache_block_state = cgm_cache_block_invalid;

					//for debugging
					nack_packet->access_id = message_packet->access_id;

					//transmit block to L3
					list_enqueue(cache->Tx_queue_bottom, nack_packet);
					advance(cache->cache_io_down_ec);
				}

			}

			break;

		case cgm_cache_block_exclusive:
		case cgm_cache_block_modified:

			//a GET/GETX_FWD means the block is E/M in this core. The block will be E/M in the L1(s)
			//We need to either downgrade or evict (getx_fwd_inval) the blocks in the CUs based on the fwd type

			//There may be more then one CU with the block if the block is dirty in core but shared

			//ok we have a get_fwd, down grade all holding cores.


			/*fatal("starting getx_fwd to E/M block 0x%08x\n", (message_packet->address & cache->block_address_mask));*/
			/*if(sharers != 1 || pending_bit != 0)
				fatal("sharers != 1 || pending_bit != 0 phy addr 0x%08x vtl addr 0x%08x\n",
						(message_packet->address & cache->block_address_mask),
						(message_packet->vtladdress & cache->block_address_mask));*/

			assert((sharers == 1 || sharers == 0) && pending_bit == 0);

			//directory shows that a CU up in the GPU has the block
			if(sharers == 1)
			{

				///////////////////
				//Directory actions
				///////////////////

				//set the directory pending bit.
				cgm_cache_set_dir_pending_bit(cache, message_packet->set, message_packet->way);

				//get the id of the owning core l1


				//flush the block out of the core...
				getx_inval_packet = packet_create();
				assert(getx_inval_packet);
				init_getx_inval_packet(getx_inval_packet, message_packet->address);

				//set up routing data
				getx_inval_packet->gpu_access_type = cgm_access_store;
				getx_inval_packet->access_id = message_packet->access_id;
				getx_inval_packet->l1_cache_id = cgm_cache_get_xown_core(gpu, cache, message_packet->set, message_packet->way);

				//send to holding core.
				list_enqueue(cache->Tx_queue_top, getx_inval_packet);
				advance(cache->cache_io_up_ec);


				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;

				//store the getx_fwd in the pending request buffer
				message_packet->getx_inval_n = sharers;

				/*shouldn't be another pending request for this block*/
				assert(cache_search_pending_request_buffer(cache, message_packet->address) == NULL);

				//drop the message packet into the pending request buffer
				message_packet = list_remove(cache->last_queue, message_packet);
				list_enqueue(cache->pending_request_buffer, message_packet);
			}
			else
			{
				//directory shows no CU has the block process putx immediately.
				//this can occur if a holding CU sends a write-back which
				//results in the directory being empty.
				//Means the block is only stored in the L2 cache.

				GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s getx_fwd zero shared processing now ID %llu type %d state %d wb? %d cycle %llu\n",
					(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
					message_packet->access_type, *cache_block_state_ptr, is_writeback_present(write_back_packet), P_TIME);

				assert(sharers == 0);

				//forwarding block to requesting CPU core in the modified state
				//the block better be out of all GPU L1 caches.
				error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, message_packet->address);
				assert(error == 0);

				//clear the directory (clears pending bit)
				cgm_cache_clear_dir(cache, message_packet->set, message_packet->way);

				//clear the dirty in core bit (this could be dirty in core if shared within the GPU)
				cgm_cache_clear_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);

				//invalidate the local cache line
				cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_invalid);

				//send putx to requesting core
				message_packet->size = packet_set_size(cache->block_size);

				//set access type and block state
				message_packet->access_type = cgm_access_putx;
				message_packet->cache_block_state = cgm_cache_block_modified;

				/*reset mp flags*/
				message_packet->coalesced = 0;
				message_packet->assoc_conflict = 0;

				message_packet->dest_name = str_map_value(node_strn_map, message_packet->src_id);
				message_packet->dest_id = str_map_string(node_strn_map, message_packet->src_name);

				//transmit putx to L2
				cache_put_io_down_queue(cache, message_packet);

				//send the ack to L3

				///////////////
				//getx_fwd_ack
				///////////////

				//send the get_fwd_ack to L3 cache.
				//create and init get__fwd_ack packet
				reply_packet = packet_create();
				assert(reply_packet);

				//set access type
				init_getx_fwd_ack_packet(reply_packet, message_packet->address);

				//if dirty in core send the dirty block down to L3
				if(dirty_in_core == 1 || *cache_block_state_ptr == cgm_cache_block_modified)
				{
					reply_packet->cache_block_state = cgm_cache_block_modified;
					reply_packet->size = packet_set_size(cache->block_size);
				}
				else
				{
					reply_packet->cache_block_state = cgm_cache_block_exclusive;
					reply_packet->size = HEADER_SIZE;
				}

				//send reply to L3 - fakes src as the requester
				reply_packet->access_id = message_packet->access_id; //for error checking
				reply_packet->l2_cache_id = message_packet->l2_cache_id; //***changed here***
				reply_packet->l2_cache_name = message_packet->src_name;

				//assert(get_getx_fwd_reply_packet->access_type == cgm_access_downgrade_ack);
				//transmit getx_fwd_ack to L3 (home)
				list_enqueue(cache->Tx_queue_bottom, reply_packet);
				advance(cache->cache_io_down_ec);

			}

			break;

		case cgm_cache_block_shared:

			//the block is actually in the E/M state at L3, but has been shared within the GPU

			//If the block is dirty_in_core we need to process a sharing write-back.

			assert(sharers >= 1 && pending_bit == 0);

			/*the block is shared throughout the GPU, but may be dirty in core
			process putx immediately, but with a sharing write-back if dirty in core*/

			//forward block to requesting core

			//this can happen in the GPU, but, the block should be marked as dirty_in_core
			fatal("starting getx_fwd to S block 0x%08x DIC %d\n",
					(message_packet->address & cache->block_address_mask), dirty_in_core);

			///////////////////
			//Directory actions
			///////////////////

			//all we need to do is clear the dirty in core bit
			cgm_cache_clear_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);

			//need to evict the block here.


			//////////
			//GET_FWD
			//////////

			//prepare to forward the block
			message_packet->size = packet_set_size(l2_caches[str_map_string(&l2_strn_map, message_packet->l2_cache_name)].block_size);
			message_packet->cache_block_state = cgm_cache_block_modified;

			//set access type
			message_packet->access_type = cgm_access_putx;

			//fwd block to requesting core
			//update routing headers swap dest and src
			//requesting node
			message_packet->dest_name = str_map_value(node_strn_map, message_packet->src_id);
			message_packet->dest_id = str_map_string(node_strn_map, message_packet->src_name);

			//owning node L2
			//pending_get_getx_fwd_request->src_name = cache->name;
			//pending_get_getx_fwd_request->src_id = str_map_string(node_strn_map, cache->name);

			//transmit block to requesting node
			cache_put_io_down_queue(cache, message_packet);


			///////////////
			//get_fwd_ack
			///////////////

			//send the get_fwd_ack to L3 cache.
			//create and init get__fwd_ack packet
			reply_packet = packet_create();
			assert(reply_packet);

			//set access type
			init_downgrade_ack_packet(reply_packet, message_packet->address);

			//date is dirty in core so send down to L3
			if(dirty_in_core)
			{
				//set size
				reply_packet->size = packet_set_size(l2_caches[str_map_string(&l2_strn_map, message_packet->l2_cache_name)].block_size);

				//set block state
				reply_packet->cache_block_state = cgm_cache_block_modified;
			}
			else
			{
				reply_packet->size = HEADER_SIZE;
			}



			//send reply to L3
			//fakes src as the requester
			reply_packet->l2_cache_id = message_packet->l2_cache_id;
			reply_packet->l2_cache_name = message_packet->src_name;

			//assert(get_getx_fwd_reply_packet->access_type == cgm_access_downgrade_ack);
			//transmit getx_fwd_ack to L3 (home)
			list_enqueue(cache->Tx_queue_bottom, reply_packet);
			advance(cache->cache_io_down_ec);

			break;
	}

	return;

}


void cgm_mesi_gpu_l2_get_getx_fwd(struct cache_t *cache, struct cgm_packet_t *message_packet){

	fatal("cgm_mesi_gpu_l2_get_getx_fwd(): BOOM!\n");

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	int num_cus = si_gpu_num_compute_units;

	struct cgm_packet_t *get_getx_fwd_reply_packet = NULL;
	struct cgm_packet_t *inval_packet = NULL;
	struct cgm_packet_t *write_back_packet = NULL;
	struct cgm_packet_t *nack_packet = NULL;
	struct cgm_packet_t *reply_packet = NULL;
	/*struct cache_t *l3_cache_ptr = NULL;*/

	int sharers, pending_bit, dirty_in_core;//, owning_core;

	int error = 0;
	int ort_status = 0;
	//int l3_map;

	//charge delay
	GPU_PAUSE(cache->latency);

	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);

	//search the WB buffer for the data
	write_back_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

	//get number of sharers
	sharers = cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way);
	assert(sharers >= 0 && sharers <= num_cus);
	//check to see if access is from an already owning core
	//owning_core = cgm_cache_is_owning_core(cache, message_packet->set, message_packet->way, message_packet->l1_cache_id);
	//check pending state
	pending_bit = cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way);
	assert(pending_bit == 0); //fix me if this happens, nack maybe?

	//see if the block is dirty in core.
	dirty_in_core = cgm_cache_get_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);
	assert(dirty_in_core >= 0 && dirty_in_core <= 1); //when this is 1 check that it is being written back to L3 as a sharing write-back

	GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s get_getx_fwd ID %llu type %d state %d wb? %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
			message_packet->access_type, *cache_block_state_ptr, is_writeback_present(write_back_packet), P_TIME);


	/*look for an access conflict, this can happen if a get_fwd beats a putx/putx_n/upgrade(putx)*/
	ort_status = ort_search(cache, message_packet->tag, message_packet->set);
	if(ort_status != cache->mshr_size)
	{

		fatal("cgm_mesi_gpu_l2_getx_fwd(): access conflict, but shouldn't have one of these yet... block 0x%08x\n",
				message_packet->address & cache->block_address_mask);

		/*if there is a pending access int the ORT there better not be a block or a write back*/
		if(*cache_block_state_ptr == cgm_cache_block_invalid)
			assert(!write_back_packet);

		/*if(cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->way) == cgm_cache_block_transient)
			assert(*cache_block_state_ptr == cgm_cache_block_shared && *cache_block_hit_ptr == 1);*/
	}


	/*
	if(*cache_block_hit_ptr == 1 && dirty_in_core == 1)
	{	assert(!write_back_packet);
		*cache_block_state_ptr = cgm_cache_block_modified;
	}*/

	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_noncoherent:
		case cgm_cache_block_owned:

			cgm_cache_dump_set(cache, message_packet->set);

			fatal("cgm_mesi_l2_getx_fwd(): %s invalid block state on getx_fwd as %s access_id %llu address 0x%08x blk_addr 0x%08x set %d tag %d way %d state %d cycle %llu\n",
				cache->name, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr),
				message_packet->access_id, message_packet->address, message_packet->address & cache->block_address_mask,
				message_packet->set, message_packet->tag, message_packet->way, *cache_block_state_ptr, P_TIME);
			break;

		case cgm_cache_block_invalid:

			//if((message_packet->address & cache->block_address_mask) == 0x081c7e40)
				//fatal("%s blk 0x%08x in in gpu cycle %llu\n", cache->name, (message_packet->address & cache->block_address_mask), P_TIME);

			/*warning("************cgm_mesi_gpu_l2_get_getx_fwd(): block isn't in the cache id %llu blk addr 0x%08x\n",
					message_packet->access_id,  message_packet->address & cache->block_address_mask);*/

			//check the WB buffer
			if(write_back_packet)
			{
				/*found the packet in the write back buffer
				data should not be in the rest of the cache*/

				assert(*cache_block_hit_ptr == 0);
				assert(write_back_packet->cache_block_state == cgm_cache_block_modified
						|| write_back_packet->cache_block_state == cgm_cache_block_exclusive);

				/*check state of write back for flush*/
				if(write_back_packet->flush_pending == 0)
				{
					//a flush has completed and the WB is still here so it better be modified
					assert(write_back_packet->cache_block_state == cgm_cache_block_modified);

					/*the flush is complete finish the getx_fwd now*/

					/*flush is complete for this write back so it should not be up in L1 D*/
					error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, message_packet->address);
					assert(error == 0);

					//////////
					//GETX_FWD
					//////////

					/*for now we handle both get and getx fwd in the same function on the GPU*/

					//forward block to requesting core

					//set access type
					message_packet->access_type = cgm_access_putx;

					message_packet->cache_block_state = cgm_cache_block_modified;

					//prepare to forward the block
					message_packet->size = packet_set_size(l2_caches[str_map_string(&l2_strn_map, message_packet->l2_cache_name)].block_size);

					//set message package size if modified in L2/L1.
					/*if(write_back_packet->cache_block_state == cgm_cache_block_modified)
					{
						//prepare to forward the block
						message_packet->size = l2_caches[str_map_string(&node_strn_map, message_packet->l2_cache_name)].block_size;

						//set access type
						message_packet->access_type = cgm_access_putx;

						//set the block state
						message_packet->cache_block_state = cgm_cache_block_modified;
					}
					else
					{
						//prepare to forward the block
						message_packet->size = 1;

						//set access type
						message_packet->access_type = cgm_access_put_clnx;

						//set the block state
						message_packet->cache_block_state = cgm_cache_block_exclusive;
					}*/

					//--------------------------------

					//fwd block to requesting core
					//update routing headers swap dest and src
					//requesting node
					message_packet->dest_name = str_map_value(node_strn_map, message_packet->src_id);
					message_packet->dest_id = str_map_string(node_strn_map, message_packet->src_name);

					//owning node L2
					message_packet->src_name = cache->name;
					message_packet->src_id = str_map_string(node_strn_map, cache->name);

					//transmit block to requesting node
					message_packet = list_remove(cache->last_queue, message_packet);
					list_enqueue(cache->Tx_queue_bottom, message_packet);
					advance(cache->cache_io_down_ec);

					///////////////
					//getx_fwd_ack
					///////////////

					//send the getx_fwd_ack to L3 cache.

					//create getx_fwd_ack packet
					reply_packet = packet_create();
					assert(reply_packet);

					init_getx_fwd_ack_packet(reply_packet, message_packet->address);

					reply_packet->size = packet_set_size(l2_caches[str_map_string(&l2_strn_map, message_packet->l2_cache_name)].block_size);
					reply_packet->cache_block_state = cgm_cache_block_modified;

					//star maybe need to fix this
					//fakes src as the requester L3 uses this to set holding core.
					reply_packet->l2_cache_id = message_packet->l2_cache_id;
					reply_packet->l2_cache_name = message_packet->src_name;

					//set message package size if modified in L2/L1.
					/*if(write_back_packet->cache_block_state == cgm_cache_block_modified)
					{
						reply_packet->size = l2_caches[str_map_string(&node_strn_map, message_packet->l2_cache_name)].block_size;
						reply_packet->cache_block_state = cgm_cache_block_modified;
					}
					else
					{
						reply_packet->size = 1;
						reply_packet->cache_block_state = cgm_cache_block_invalid;
					}*/

					//fwd reply (getx_fwd_ack) to L3
					//l3_cache_ptr = cgm_l3_cache_map(message_packet->set);

					//fakes src as the requester
					//reply_packet->l2_cache_id = message_packet->l2_cache_id;
					//reply_packet->l2_cache_name = message_packet->src_name;

					//SETROUTE(reply_packet, cache, l3_cache_ptr)

					//transmit getx_fwd_ack to L3 (home)
					list_enqueue(cache->Tx_queue_bottom, reply_packet);
					advance(cache->cache_io_down_ec);

					write_back_packet = list_remove(cache->write_back_buffer, write_back_packet);
					packet_destroy(write_back_packet);

				}
				else
				{

					fatal("cgm_mesi_gpu_l2_get_getx_fwd(): shouldn't have pending flush yet\n");

					/*if the wb is flush pending we have to wait for the flush to complete and then join there*/

					/*write back is in the process of being flushed by L1*/
					assert(write_back_packet->flush_pending == 1);

					//message_packet->downgrade_pending = 1;
					message_packet->L3_flush_join = 1;
					cgm_cache_insert_pending_request_buffer(cache, message_packet);
				}
			}
			else
			{

				/*fatal("cgm_mesi_gpu_l2_get_getx_fwd(): blk miss in l2 shouldn't happen yet id %llu access type %d blk_addr 0x%08x\n",
						message_packet->access_id, message_packet->access_type, message_packet->address & cache->block_address_mask);*/

				//printf("\tcgm_mesi_l2_getx_fwd(): no wb blk addr 0x%08x \n", message_packet->address & cache->block_address_mask);
				//fatal("Getx_fwd %s access id %llu blk_addr 0x%08x\n", cache->name, message_packet->access_id, message_packet->address & cache->block_address_mask);

				if(ort_status < cache->mshr_size)
				{

					fatal("cgm_mesi_gpu_l2_get_getx_fwd(): shouldn't be an ORT issue in the GPU yet\n");

					GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s getx_fwd conflict pending join ID %llu type %d state %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name,message_packet->access_id,
							message_packet->access_type, *cache_block_state_ptr, P_TIME);

					/*set the bit in the ort table to 0*/
					ort_set_pending_join_bit(cache, ort_status, message_packet->tag, message_packet->set);

					message_packet->downgrade_pending = 1;

					/*drop into the pending request buffer*/
					message_packet =  list_remove(cache->last_queue, message_packet);
					list_enqueue(cache->pending_request_buffer, message_packet);
				}
				else
				{

					fatal("cgm_mesi_gpu_l2_get_getx_fwd(): blk miss in l2 shouldn't happen yet id %llu access type %d blk_addr 0x%08x\n",
						message_packet->access_id, message_packet->access_type, message_packet->address & cache->block_address_mask);

					/* The block was evicted silently and should not be in the L1 cache.
					 * However, the block may be in L1 D's write back or in the pipe between L1 D and L2.
					 * We have to send a flush to L1 D to make sure the block is really out of there before proceeding.*/
					error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, message_packet->address);
					assert(error == 0);

					//assert(cache->sets[message_packet->set].blocks[message_packet->way].directory_entry.entry == 0);

					assert(message_packet->access_type == cgm_access_getx_fwd || message_packet->access_type == cgm_access_get_fwd);

					/*two part reply (1) send nack to L3 and (2) send nack to requesting L2*/
					//set access type

					if(message_packet->access_type == cgm_access_getx_fwd)
						message_packet->access_type = cgm_access_getx_fwd_nack;
					else
						message_packet->access_type = cgm_access_downgrade_nack;

					//set the block state
					message_packet->cache_block_state = cgm_cache_block_invalid;

					//set message package size
					message_packet->size = HEADER_SIZE;

					//fwd nack to requesting core
					//update routing headers swap dest and src
					//requesting node
					message_packet->dest_name = str_map_value(node_strn_map, message_packet->src_id);
					message_packet->dest_id = str_map_string(node_strn_map, message_packet->src_name);

					//owning node L2
					message_packet->src_name = cache->name;
					message_packet->src_id = str_map_string(node_strn_map, cache->name);

					//transmit nack to L2
					cache_put_io_down_queue(cache, message_packet);

					////////
					//part 2
					////////

					//create downgrade_nack
					nack_packet = packet_create();
					assert(nack_packet);

					init_getx_fwd_nack_packet(nack_packet, message_packet->address);

					if(message_packet->access_type == cgm_access_getx_fwd)
						nack_packet->access_type = cgm_access_getx_fwd_nack;
					else
						nack_packet->access_type = cgm_access_downgrade_nack;

					nack_packet->access_id = message_packet->access_id;


					//transmit block to L3
					list_enqueue(cache->Tx_queue_bottom, nack_packet);
					advance(cache->cache_io_down_ec);
				}

			}

			break;

		case cgm_cache_block_exclusive:
		case cgm_cache_block_modified:

			//a GET/GETX_FWD means the block is E/M in this core. The block will be E/M in the L1(s)
			//We need to either downgrade or evict (getx_fwd_inval) the blocks in the CUs based on the fwd type

			//There may be more then one CU with the block if the block is dirty in core but shared

			fatal("gpu get/getx_fwd to e or m line finish me\n");

			if(message_packet->access_type == cgm_access_get_fwd)
			{
				//ok we have a get_fwd, down grade all holding cores.

				//store the getx_fwd in the pending request buffer
				message_packet->inval_pending = 1;
				cgm_cache_insert_pending_request_buffer(cache, message_packet);

				//set the flush_pending bit to 1 in the block
				cgm_cache_set_block_flush_pending_bit(cache, message_packet->set, message_packet->way);




				//flush the L1 cache because the line may be dirty in L1
				inval_packet = packet_create();
				assert(inval_packet);
				init_getx_fwd_inval_packet(inval_packet, message_packet->address);

				unsigned long long bit_vector;

				//get the presence bits from the directory
				bit_vector = cache->sets[message_packet->set].blocks[message_packet->way].directory_entry.entry;
				bit_vector = bit_vector & cache->share_mask;


				//send the L1 D cache the inval message

				inval_packet->l1_cache_id = LOG2(bit_vector);

				/*warning("bit vector value %llu owning core %d\n", bit_vector, inval_packet->l1_cache_id);*/


				inval_packet->gpu_access_type = cgm_access_store;
				inval_packet->access_id = message_packet->access_id;
				list_enqueue(cache->Tx_queue_top, inval_packet);
				advance(cache->cache_io_up_ec);
			}
			else
			{

				fatal("handle getx_fwd here\n");

				//store the getx_fwd in the pending request buffer
				message_packet->inval_pending = 1;
				cgm_cache_insert_pending_request_buffer(cache, message_packet);

				//set the flush_pending bit to 1 in the block
				cgm_cache_set_block_flush_pending_bit(cache, message_packet->set, message_packet->way);




				//flush the L1 cache because the line may be dirty in L1
				inval_packet = packet_create();
				assert(inval_packet);
				init_getx_fwd_inval_packet(inval_packet, message_packet->address);

				unsigned long long bit_vector;

				//get the presence bits from the directory
				bit_vector = cache->sets[message_packet->set].blocks[message_packet->way].directory_entry.entry;
				bit_vector = bit_vector & cache->share_mask;


				//send the L1 D cache the inval message

				inval_packet->l1_cache_id = LOG2(bit_vector);

				/*warning("bit vector value %llu owning core %d\n", bit_vector, inval_packet->l1_cache_id);*/


				inval_packet->gpu_access_type = cgm_access_store;
				inval_packet->access_id = message_packet->access_id;
				list_enqueue(cache->Tx_queue_top, inval_packet);
				advance(cache->cache_io_up_ec);

			}

			break;


		case cgm_cache_block_shared:

			//the block is actually in the E/M state at L3, but has been shared within the GPU

			if(dirty_in_core)
			{
				/*the block is shared throughout the GPU, but is dirty in core
				process puts immediately, but with a sharing write-back*/

				//////////
				//GETX_FWD
				//////////

				//forward block to requesting core
				assert(message_packet->access_type == cgm_access_get_fwd); //this doesn't work with getx_fwd

				//prepare to forward the block
				message_packet->size = packet_set_size(l2_caches[str_map_string(&l2_strn_map, message_packet->l2_cache_name)].block_size);
				message_packet->cache_block_state = cgm_cache_block_shared;

				//set access type
				if(message_packet->access_type == cgm_access_get_fwd)
					message_packet->access_type = cgm_access_puts;
				else
					message_packet->access_type = cgm_access_putx;

				//fwd block to requesting core
				//update routing headers swap dest and src
				//requesting node
				message_packet->dest_name = str_map_value(node_strn_map, message_packet->src_id);
				message_packet->dest_id = str_map_string(node_strn_map, message_packet->src_name);

				//owning node L2
				//pending_get_getx_fwd_request->src_name = cache->name;
				//pending_get_getx_fwd_request->src_id = str_map_string(node_strn_map, cache->name);

				//transmit block to requesting node
				cache_put_io_down_queue(cache, message_packet);


				///////////////
				//getx_fwd_ack
				///////////////

				//send the get_getx_fwd_ack to L3 cache.
				//create get_getx_fwd_ack packet
				get_getx_fwd_reply_packet = packet_create();
				assert(get_getx_fwd_reply_packet);

				//set access type
				if(message_packet->access_type == cgm_access_puts) //a downgrade
				{
					init_downgrade_ack_packet(get_getx_fwd_reply_packet, message_packet->address);
				}
				else
				{
					init_getx_fwd_ack_packet(get_getx_fwd_reply_packet, message_packet->address);
				}

				//date is dirty in core so send down to L3
				get_getx_fwd_reply_packet->size = packet_set_size(l2_caches[str_map_string(&l2_strn_map, message_packet->l2_cache_name)].block_size);

				//send reply to L3
				//fakes src as the requester
				get_getx_fwd_reply_packet->l2_cache_id = message_packet->l2_cache_id;
				get_getx_fwd_reply_packet->l2_cache_name = message_packet->src_name;

				//assert(get_getx_fwd_reply_packet->access_type == cgm_access_downgrade_ack);
				//transmit getx_fwd_ack to L3 (home)
				list_enqueue(cache->Tx_queue_bottom, get_getx_fwd_reply_packet);
				advance(cache->cache_io_down_ec);

			}
			else
			{
				assert(dirty_in_core == 0);

				fatal("GPU get_getx_fwd shared dic = 0\n");

			}


			//the CPU L3 caches have us down as being the only

			//if the block is not dirty_in_core we can share with the


			//GPU received a get/getx_fwd from the CPU. we need to forward the block to the CPU

			//If the block is dirty_in_core we need to process a sharing write-back.

			//invalidate all GPU copies and send over to CPU


			break;
	}



	return;
}

void cgm_mesi_gpu_l2_gpu_flush_ack(struct cache_t *cache, struct cgm_packet_t *message_packet){

	//this is the ack from L1, flush it down from here now...

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	//get number of sharers
	int sharers; //, owning_core, pending_bit;

	//int l3_map = 0;
	int ort_status = -1;

	enum cgm_cache_block_state_t victim_trainsient_state;

	//struct cgm_packet_t *wb_packet = NULL;
	//struct cache_t *l3_cache_ptr = NULL;

	//unsigned long long bit_vector;

	//charge delay
	GPU_PAUSE(cache->latency);

	//get the block status
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	victim_trainsient_state = cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->way);

	//get number of sharers
	sharers = cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way);
	//check to see if access is from an already owning core
	//owning_core = cgm_cache_is_owning_core(cache, message_packet->set, message_packet->way, message_packet->l1_cache_id);
	//check pending state
	//pending_bit = cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way);

	//wb_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

	GPUDEBUG(LEVEL == 2 || LEVEL == 3, "cgm_mesi_gpu_l2_gpu_flush_ack(): block 0x%08x %s GPU flush block ack ID %llu type %d state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->evict_id,
			message_packet->access_type, *cache_block_state_ptr, P_TIME);

	/*check the ORT table for an outstanding access
	check the ORT table is there an outstanding access for this block we are trying to evict?*/
	ort_status = ort_search(cache, message_packet->tag, message_packet->set);
	if(ort_status != cache->mshr_size)
	{
		fatal("cgm_mesi_gpu_l2_gpu_flush(): shouldn't have this\n");
		/*yep there is so set the bit in the ort table to 0.
		 * When the put/putx comes kill it and try again...*/
		//ort_set_pending_join_bit(cache, ort_status, message_packet->tag, message_packet->set);
	}

	//this is an ack so there better still be a sharer
	assert(sharers == 1);



	//search the WB buffer for the data
	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_noncoherent:
		case cgm_cache_block_owned:
			fatal("cgm_mesi_gpu_l2_gpu_flush(): L2 id %d invalid block state on inval as %s address %u\n",
				cache->id, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr), message_packet->address);
			break;

		case cgm_cache_block_invalid:

			fatal("cgm_mesi_gpu_l2_gpu_flush_ack(): block should NOT be invalid\n");

			/*wb_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

			//found the block in the WB buffer
			if(wb_packet)
			{
				if pending is 0 the L1 cache has been flushed process now
				if(wb_packet->flush_pending == 0)
				{
					assert(wb_packet->cache_block_state == cgm_cache_block_modified);

					message_packet->size = cache->block_size;
					message_packet->cache_block_state = cgm_cache_block_modified;

					//remove the block from the WB buffer
					wb_packet = list_remove(cache->write_back_buffer, wb_packet);
					packet_destroy(wb_packet);

					//set access type inval_ack
					message_packet->access_type = cgm_access_gpu_flush_ack;

					cache_put_io_down_queue(cache, message_packet);

				}
				else if(wb_packet->flush_pending == 1)
				{

					fatal("cgm_mesi_gpu_l2_gpu_flush(): hope this doesn't happen!\n");

					//waiting on flush to finish insert into pending request buffer
					assert(wb_packet->cache_block_state == cgm_cache_block_exclusive || wb_packet->cache_block_state == cgm_cache_block_modified);

					set flush_join bit
					wb_packet->L3_flush_join = 1;

					//put the message packet into the pending request buffer
					message_packet = list_remove(cache->last_queue, message_packet);
					list_enqueue(cache->pending_request_buffer, message_packet);
				}
				else
				{
					fatal("cgm_mesi_gpu_l2_gpu_flush(): wb_packet has invalid flush_pending value\n");
				}
			}
			else
			{

				//star todo somehow check and make sure these are modified
				if here the L2 cache has already written back, send down so the flush can complete
				message_packet->size = 1;
				message_packet->cache_block_state = cgm_cache_block_invalid;

				//set access type inval_ack
				message_packet->access_type = cgm_access_gpu_flush_ack;

				cache_put_io_down_queue(cache, message_packet);
			}*/

			break;


		case cgm_cache_block_exclusive:
		case cgm_cache_block_modified:

			/*The block is in L2 but not up in one of the compute units (sharers == 0)
			flush it from the L2 cache and move on.*/
			assert(victim_trainsient_state != cgm_cache_block_transient);

			//set the block state to invalid
			cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_invalid);

			//clear the directory entry
			cgm_cache_clear_dir(cache, message_packet->set, message_packet->way);

			if(*cache_block_state_ptr == cgm_cache_block_modified || message_packet->cache_block_state == cgm_cache_block_modified)
			{
				message_packet->size = packet_set_size(cache->block_size);
				message_packet->cache_block_state = cgm_cache_block_modified;
			}
			else
			{
				message_packet->size = HEADER_SIZE;
				message_packet->cache_block_state = cgm_cache_block_invalid;
			}

				//set access type inval_ack
				message_packet->access_type = cgm_access_gpu_flush_ack;

				cache_put_io_down_queue(cache, message_packet);

			break;

		case cgm_cache_block_shared:

			fatal("cgm_mesi_gpu_l2_gpu_flush(): shouldn't be a shared state\n");

			break;
	}

	return;
}


void cgm_mesi_gpu_l2_gpu_flush(struct cache_t *cache, struct cgm_packet_t *message_packet){

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	//get number of sharers
	int sharers; //, owning_core, pending_bit;

	//int l3_map = 0;
	int ort_status = -1;

	enum cgm_cache_block_state_t victim_trainsient_state;

	struct cgm_packet_t *wb_packet = NULL;
	//struct cache_t *l3_cache_ptr = NULL;

	unsigned long long bit_vector;

	//charge delay
	GPU_PAUSE(cache->latency);

	//get the block status
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	victim_trainsient_state = cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->way);

	//get number of sharers
	sharers = cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way);
	//check to see if access is from an already owning core
	//owning_core = cgm_cache_is_owning_core(cache, message_packet->set, message_packet->way, message_packet->l1_cache_id);
	//check pending state
	//pending_bit = cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way);

	wb_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

	GPUDEBUG(LEVEL == 2 || LEVEL == 3, "cgm_mesi_gpu_l2_gpu_flush(): (from system) block 0x%08x %s GPU flush block ID %llu type %d state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->evict_id,
			message_packet->access_type, *cache_block_state_ptr, P_TIME);

	/*check the ORT table for an outstanding access
	check the ORT table is there an outstanding access for this block we are trying to evict?*/
	ort_status = ort_search(cache, message_packet->tag, message_packet->set);
	if(ort_status != cache->mshr_size)
	{
		fatal("cgm_mesi_gpu_l2_gpu_flush(): shouldn't have this yet\n");
		/*yep there is so set the bit in the ort table to 0.
		 * When the put/putx comes kill it and try again...*/
		//ort_set_pending_join_bit(cache, ort_status, message_packet->tag, message_packet->set);
	}

	//first check and see if there are any sharers for the block

	if(sharers >= 1 && *cache_block_hit_ptr == 1)
	{
		/*should only be one compute unit with the block*/
		assert(sharers == 1);

		/*just forward the flush request on to l1 cache*/
		//get the presence bits from the directory
		bit_vector = cache->sets[message_packet->set].blocks[message_packet->way].directory_entry.entry;
		assert(bit_vector > 0);
		bit_vector = bit_vector & cache->share_mask;

		message_packet->l1_cache_id = LOG2(bit_vector);
		message_packet->gpu_access_type = cgm_access_store;

		//making sure something didn't happen to these fields.
		message_packet->size = HEADER_SIZE;
		message_packet->access_type = cgm_access_gpu_flush;

		//transmit to L1
		cache_put_io_up_queue(cache, message_packet);
	}
	else
	{
		//no GPU compute unit has the block, so finish the flush and send down

		//search the WB buffer for the data
		switch(*cache_block_state_ptr)
		{
			case cgm_cache_block_noncoherent:
			case cgm_cache_block_owned:
				fatal("cgm_mesi_gpu_l2_gpu_flush(): L2 id %d invalid block state on inval as %s address %u\n",
					cache->id, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr), message_packet->address);
				break;

			case cgm_cache_block_invalid:

				wb_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

				//found the block in the WB buffer
				if(wb_packet)
				{
					/*if pending is 0 the L1 cache has been flushed process now*/
					if(wb_packet->flush_pending == 0)
					{
						assert(wb_packet->cache_block_state == cgm_cache_block_modified);

						message_packet->size = packet_set_size(cache->block_size);
						message_packet->cache_block_state = cgm_cache_block_modified;

						//remove the block from the WB buffer
						wb_packet = list_remove(cache->write_back_buffer, wb_packet);
						packet_destroy(wb_packet);

						//set access type inval_ack
						message_packet->access_type = cgm_access_gpu_flush_ack;

						cache_put_io_down_queue(cache, message_packet);

					}
					else if(wb_packet->flush_pending == 1)
					{

						fatal("cgm_mesi_gpu_l2_gpu_flush(): hope this doesn't happen!\n");

						//waiting on flush to finish insert into pending request buffer
						assert(wb_packet->cache_block_state == cgm_cache_block_exclusive || wb_packet->cache_block_state == cgm_cache_block_modified);

						/*set flush_join bit*/
						wb_packet->L3_flush_join = 1;

						//put the message packet into the pending request buffer
						message_packet = list_remove(cache->last_queue, message_packet);
						list_enqueue(cache->pending_request_buffer, message_packet);
					}
					else
					{
						fatal("cgm_mesi_gpu_l2_gpu_flush(): wb_packet has invalid flush_pending value\n");
					}
				}
				else
				{

					//star todo somehow check and make sure these are modified
					/*if here the L2 cache may have already written back, send down so the flush can complete*/
					message_packet->size = HEADER_SIZE;
					message_packet->cache_block_state = cgm_cache_block_invalid;

					//set access type inval_ack
					message_packet->access_type = cgm_access_gpu_flush_ack;

					cache_put_io_down_queue(cache, message_packet);
				}

				break;


			case cgm_cache_block_exclusive:
			case cgm_cache_block_modified:

				assert(sharers == 0);

				/*The block is in L2 but not up in one of the compute units (sharers == 0)
				flush it from the L2 cache and move on.*/
				assert(victim_trainsient_state != cgm_cache_block_transient);

				assert(cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way) == 0);

				//set the block state to invalid
				cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_invalid);

				//clear the directory entry
				cgm_cache_clear_dir(cache, message_packet->set, message_packet->way);

				if(*cache_block_state_ptr == cgm_cache_block_modified)
				{
					message_packet->size = packet_set_size(cache->block_size);
					message_packet->cache_block_state = cgm_cache_block_modified;
				}
				else
				{
					message_packet->size = HEADER_SIZE;
					message_packet->cache_block_state = cgm_cache_block_exclusive;
				}

				//set access type inval_ack
				message_packet->access_type = cgm_access_gpu_flush_ack;

				cache_put_io_down_queue(cache, message_packet);

				break;

			case cgm_cache_block_shared:

				fatal("cgm_mesi_gpu_l2_gpu_flush(): shouldn't be a shared state\n");

				break;
		}

	}

	return;
}

void cgm_mesi_gpu_l2_flush_block(struct cache_t *cache, struct cgm_packet_t *message_packet){

	//Invalidation/eviction request from L3 cache

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;
	//int l3_map = 0;
	int ort_status = -1;

	/*int num_pending_requests = 0;*/
	int dirty_in_core = 0;
	int pending_bit = 0;

	/*struct cgm_packet_t *pending_request_packet = NULL;*/

	enum cgm_cache_block_state_t victim_trainsient_state;

	int sharers = -1;//, owning_core, pending_bit;

	struct cgm_packet_t *wb_packet = NULL;
	//struct cache_t *l3_cache_ptr = NULL;

	//charge delay
	GPU_PAUSE(cache->latency);

	//get the block status
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	//check block transient state
	victim_trainsient_state = cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->way);

	//check writeback
	wb_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

	//get number of sharers
	sharers = cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way);
	//check to see if access is from an already owning core
	//owning_core = cgm_cache_is_owning_core(cache, message_packet->set, message_packet->way, message_packet->l1_cache_id);
	//check pending state
	//pending_bit = cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way);

	dirty_in_core = cgm_cache_get_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);

	pending_bit = cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way);


	/*if(*cache_block_hit_ptr == 1 && dirty_in_core != 0) //handel dirty in core case
	{
		dirty in core is only set when downgrade occurs
		so if the block is in the cache it better shared
		a getx resulting in a putx_n would end up with a write back to L3
		when leaving the shared state.
		assert(*cache_block_state_ptr == cgm_cache_block_shared);

		fatal("block 0x%08x %s flush block from L3 block ID %llu type %d state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->evict_id,
			message_packet->access_type, *cache_block_state_ptr, P_TIME);

	}*/


	/*if(message_packet->evict_id == 203170)
			printf("GPU L2 has L3 flush block.\n");*/



	GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s flush block from L3 block ID %llu type %d state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->evict_id,
			message_packet->access_type, *cache_block_state_ptr, P_TIME);

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);
	assert(message_packet->access_type != cgm_access_gpu_flush);

	/*check the ORT table for an outstanding access*/
	//check the ORT table is there an outstanding access for this block we are trying to evict?
	ort_status = ort_search(cache, message_packet->tag, message_packet->set);
	if(ort_status != cache->mshr_size)
	{
		//flush block stuff here
		//warning("GPU flush block, ort conflict! Check me blk 0x%08x\n",
			//(message_packet->address & cache->block_address_mask));
		//fatal("block 0x%08x %s flush block conflict found ort set cycle %llu\n", (message_packet->address & cache->block_address_mask), cache->name, P_TIME);
		/*yep there is so set the bit in the ort table to 0.
		 * When the put/putx comes kill it and try again...*/
		ort_set_pending_join_bit(cache, ort_status, message_packet->tag, message_packet->set);
	}


	if(pending_bit == 1 && *cache_block_hit_ptr == 1)
	{
		/*its possible for the CPU L3 to try and evict a block during
		a GPU L2 block state change. The evict could be joined at the
		end of a state change, however in this case it gets really hairy
		its best to nack and let the L3 retry the eviction.
		However, watch this approach, it could lead to CPU starvation...*/

		/*warning("block 0x%08x %s L3 flush but block is pending evict ID %llu type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->evict_id,
						message_packet->access_type, *cache_block_state_ptr, P_TIME);*/


		//create downgrade_nack
		message_packet->access_type = cgm_access_flush_block_nack;

		message_packet->l2_cache_id = gpu_core_id;
		message_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

		//transmit block to L3
		cache_put_io_down_queue(cache, message_packet);

		return;
	}



	/*//search the WB buffer for the data
	write_back_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);*/
	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_noncoherent:
		case cgm_cache_block_owned:
			fatal("cgm_mesi_l2_inval(): L2 id %d invalid block state on inval as %s address %u\n",
				cache->id, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr), message_packet->address);
			break;

		case cgm_cache_block_invalid:

			//fatal("cgm_mesi_gpu_l2_flush_block(): %s blk not in cache 0x%08x\n", cache->name, (message_packet->address & cache->block_address_mask));
			/*wb_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);*/

			//found the block in the WB buffer
			if(wb_packet)
			{

				/*if pending is 0 the L1 cache has been flushed process now*/
				if(wb_packet->flush_pending == 0)
				{

					assert(wb_packet->cache_block_state == cgm_cache_block_modified);

					message_packet->size = packet_set_size(cache->block_size);
					message_packet->cache_block_state = cgm_cache_block_modified;

					//remove the block from the WB buffer
					wb_packet = list_remove(cache->write_back_buffer, wb_packet);
					packet_destroy(wb_packet);

					message_packet->l2_cache_id = gpu_core_id;
					message_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

					//set access type inval_ack
					message_packet->access_type = cgm_access_flush_block_ack;

					cache_put_io_down_queue(cache, message_packet);

				}
				else if(wb_packet->flush_pending == 1)
				{

					//fatal("cgm_mesi_l2_flush_block(): flush pending blk 0x%08x this is probably ok, just need to make sure its working right\n",
					//		message_packet->address & cache->block_address_mask);

					GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s Flush block from L3 WB is pending L1 flush block ID %llu type %d state %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->evict_id,
						message_packet->access_type, *cache_block_state_ptr, P_TIME);

					//waiting on flush to finish insert into pending request buffer
					assert(wb_packet->cache_block_state == cgm_cache_block_exclusive || wb_packet->cache_block_state == cgm_cache_block_modified);

					/*set flush_join bit*/
					wb_packet->L3_flush_join = 1;

					//put the message packet into the pending request buffer
					message_packet = list_remove(cache->last_queue, message_packet);
					list_enqueue(cache->pending_request_buffer, message_packet);
				}
				else
				{
					fatal("cgm_mesi_l2_flush_block(): wb_packet has invalid flush_pending value\n");
				}
			}
			else
			{

				//line is invalid and not in WB if shared at L3 do nothing.
				if(message_packet->cache_block_state == cgm_cache_block_shared)
				{
					message_packet = list_remove(cache->last_queue, message_packet);
					packet_destroy(message_packet);
				}
				else
				{
					assert(message_packet->cache_block_state == cgm_cache_block_modified
							|| message_packet->cache_block_state == cgm_cache_block_exclusive);

					/*if here the L2 cache has already written back, send down so the flush can complete*/
					message_packet->size = HEADER_SIZE;
					message_packet->cache_block_state = cgm_cache_block_invalid;

					message_packet->l2_cache_id = gpu_core_id;
					message_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

					//set access type inval_ack
					message_packet->access_type = cgm_access_flush_block_ack;

					//reply to the L3 cache
					cache_put_io_down_queue(cache, message_packet);
				}
			}
			break;


		case cgm_cache_block_exclusive:
		case cgm_cache_block_modified:

			/*if the block is found in the L2 it may or may not be in the L1 cache
			we must invalidate here and send an invalidation to the L1 D cache*/
			assert(victim_trainsient_state != cgm_cache_block_transient);

			if(sharers == 1)
			{
				/*assert(cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way) == 0);*/
				/*if(cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way) == 1)
				{

					//in the GPU the L2 can be trying to downgrade or remove a block


					do
					{	There are pending request for this block, we need to nack them now then evict.
						pending_request_packet = cache_search_pending_request_buffer(cache, message_packet->address);
						assert(pending_request_packet);
						assert(pending_request_packet->access_type == cgm_access_get
								|| pending_request_packet->access_type == cgm_access_getx
								|| pending_request_packet->access_type == cgm_access_load_retry
								|| pending_request_packet->access_type == cgm_access_store_retry);

						nack the packet
						if(pending_request_packet)
						{
						if(pending_request_packet->access_type != cgm_access_get && pending_request_packet->access_type != cgm_access_getx
								&& pending_request_packet->access_type != cgm_access_load_retry && pending_request_packet->access_type != cgm_access_store_retry)
						{
								//&& pending_request_packet->access_type != cgm_access_load_retry && pending_request_packet->access_type != cgm_access_store_retry)
							fatal("cgm_mesi_gpu_l2_flush_block(): bad pending packet block 0x%08x evict_is %llu pending_id %llu pending_type %d start_cycle %llu cycle %llu\n",
									(message_packet->address & cache->block_address_mask), message_packet->evict_id, pending_request_packet->access_id,
									pending_request_packet->access_type, pending_request_packet->start_cycle, P_TIME);
						}


						assert(pending_request_packet->access_type == cgm_access_get || pending_request_packet->access_type == cgm_access_getx
								|| pending_request_packet->access_type == cgm_access_load_retry || pending_request_packet->access_type == cgm_access_store_retry);

						num_pending_requests++;

						//send the reply up as a NACK!
						if(pending_request_packet->access_type == cgm_access_get
								|| pending_request_packet->access_type == cgm_access_load_retry)
						{
							pending_request_packet->access_type = cgm_access_get_nack;
						}
						else
						{
							assert(pending_request_packet->access_type == cgm_access_getx
									|| pending_request_packet->access_type == cgm_access_store_retry);
							pending_request_packet->access_type = cgm_access_getx_nack;
						}

						//set message package size
						pending_request_packet->size = HEADER_SIZE;

						GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s flush block from L3 found pending request issuing nack to l1 block ID %llu type %d state %d cycle %llu\n",
								(message_packet->address & cache->block_address_mask), cache->name, message_packet->evict_id,
								message_packet->access_type, *cache_block_state_ptr, P_TIME);

						reset mp flags
						assert(pending_request_packet->coalesced == 0);
						assert(pending_request_packet->assoc_conflict == 0);

						pending_request_packet = list_remove(cache->pending_request_buffer, pending_request_packet);

						list_enqueue(cache->Tx_queue_top, pending_request_packet);
						advance(cache->cache_io_up_ec);

						}

					} while(pending_request_packet);

					assert(num_pending_requests == 1);


				}*/

				/*evict block here*/

				/*protocol case; there is a flush packet already in the system don't send a 2nd*/
				//if pending request don't issue the eviction yet.
				/*cgm_L2_cache_evict_block(cache, message_packet->set, message_packet->way,
						cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way), num_pending_requests, 0);*/

				cgm_L2_cache_evict_block(cache, message_packet->set, message_packet->way,
						cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way), 0, 0);

				//clear the directory entry
				cgm_cache_clear_dir(cache,  message_packet->set, message_packet->way); /*NOTE CLEARS THE PENDING BIT*/

				//check the dirty in core bit
				if(cgm_cache_get_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way) == 1)
				{
					assert(*cache_block_state_ptr == cgm_cache_block_modified);
					cgm_cache_clear_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);
				}

				//search WB again, because the evict would drop this cache line into the WB buffer..
				//star todo find a better way to do this...
				wb_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

				assert(wb_packet);
				wb_packet->L3_flush_join = 1;

				message_packet = list_remove(cache->last_queue, message_packet);
				list_enqueue(cache->pending_request_buffer, message_packet);
			}
			else
			{
				//block is not up in one of the vector caches...
				assert(sharers == 0);

				assert(cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way) == 0);

				/*evict block here*/
				cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_invalid);

				//clear the directory entry
				cgm_cache_clear_dir(cache,  message_packet->set, message_packet->way);


				//check the dirty in core bit
				if(cgm_cache_get_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way) == 1)
				{
					assert(*cache_block_state_ptr == cgm_cache_block_modified);
					cgm_cache_clear_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);
				}


				if(*cache_block_state_ptr == cgm_cache_block_modified)
				{
					message_packet->size = packet_set_size(cache->block_size);
					message_packet->cache_block_state = cgm_cache_block_modified;
				}
				else
				{
					message_packet->size = HEADER_SIZE;
					message_packet->cache_block_state = cgm_cache_block_invalid;
				}

				message_packet->l2_cache_id = gpu_core_id;
				message_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

				message_packet->access_type = cgm_access_flush_block_ack;

				//reply to the L3 cache
				cache_put_io_down_queue(cache, message_packet);
			}

			break;

		case cgm_cache_block_shared:

			//remember to clear the dirty in core bit if needed.
			if(dirty_in_core == 0)
			{
				//the block is in the GPU as shared and is not dirty in core.
				//evict the block and send the flush block ack

				assert(message_packet->cache_block_state != cgm_cache_block_modified); //check me if I fail...
				assert(cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way) == 0);

				//evict the block
				cgm_L2_cache_evict_block(cache, message_packet->set, message_packet->way, sharers, 0, message_packet->way);

				//clear the directory entry
				cgm_cache_clear_dir(cache,  message_packet->set, message_packet->way);

				//the block at L3 is actually E/M send flush block ack
				if(message_packet->cache_block_state == cgm_cache_block_exclusive)
				{

					/*warning("cgm_mesi_gpu_l2_flush_block(): L3 evict came to shared not dirty in core blk 0x%08x\n",
					(message_packet->address & cache->block_address_mask));*/


					//block was previously written back/down graded
					message_packet->size = HEADER_SIZE;
					message_packet->cache_block_state = cgm_cache_block_invalid;

					message_packet->l2_cache_id = gpu_core_id;
					message_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

					message_packet->access_type = cgm_access_flush_block_ack;

					//reply to the L3 cache
					cache_put_io_down_queue(cache, message_packet);

				}
				else
				{
					//if shared at L3 we're done destroy the packet
					assert(message_packet->cache_block_state == cgm_cache_block_shared);

					message_packet = list_remove(cache->last_queue, message_packet);
					packet_destroy(message_packet);
				}
			}
			else
			{
				//block is shared, but dirty in core process as a write back.

				/*warning("cgm_mesi_gpu_l2_flush_block(): L3 evict came to shared and dirty in core blk 0x%08x cycle %llu\n",
					(message_packet->address & cache->block_address_mask),
					P_TIME);*/

				// the bit should be 1 no other values
				assert(dirty_in_core == 1);

				/*the CPU l3 cache should have the block as e or m
				the block is s in the gpu because it was locally downgraded*/
				assert(message_packet->cache_block_state == cgm_cache_block_exclusive
						|| message_packet->cache_block_state == cgm_cache_block_modified);

				//code up above should have taken care of the any pending state conflicts.
				assert(cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way) == 0);

				cgm_L2_cache_evict_block(cache, message_packet->set, message_packet->way,
						cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way), 0, 0);

				//clear the directory entry
				cgm_cache_clear_dir(cache,  message_packet->set, message_packet->way); /*NOTE CLEARS THE PENDING BIT*/

				cgm_cache_clear_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);

				//dirty in core so there better be a WB
				wb_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);
				assert(wb_packet);

				wb_packet = list_remove(cache->write_back_buffer, wb_packet);
				packet_destroy(wb_packet);

				//set size, state, etc
				message_packet->size = packet_set_size(cache->block_size);
				message_packet->cache_block_state = cgm_cache_block_modified;

				message_packet->l2_cache_id = gpu_core_id;
				message_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

				message_packet->access_type = cgm_access_flush_block_ack;

				//reply to the L3 cache
				cache_put_io_down_queue(cache, message_packet);
			}

			/*fatal("cgm_mesi_gpu_l2_flush_block(): L3 evict came to shared line in L2 blk 0x%08x\n",
					(message_packet->address & cache->block_address_mask));*/

			break;
	}

	return;
}

void cgm_mesi_gpu_l2_downgrade_ack(struct cache_t *cache, struct cgm_packet_t *message_packet){

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	struct cgm_packet_t *reply_packet = NULL;
	struct cgm_packet_t *pending_request = NULL;
	struct cgm_packet_t *write_back_packet = NULL;

	int pending_bit, sharers, dirty_in_core;

	//charge delay
	GPU_PAUSE(cache->latency);

	//get the status of the cache block and try to find it in either the cache or WB buffer
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);
	assert(*cache_block_hit_ptr == 1);
	/*
		fatal("block 0x%08x %s downgrade ack BUG block not in cache DG ID %llu type %d state %d pending %d shares %d cycle %llu\n",
				(message_packet->address & cache->block_address_mask), cache->name,
				message_packet->downgrade_id, message_packet->access_type, *cache_block_state_ptr,
				cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way),
				cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way), P_TIME);
	assert(*cache_block_hit_ptr == 1);*/


	if(*cache_block_hit_ptr != 1)
	{
			assert(write_back_packet);
			assert(write_back_packet->L3_flush_join == 1);


			//its possible for L3 to evict the block while L2 is working on a pending request.
			//look for a writeback on downgrade ack if the block is no longer in the cache
			//the wb should be pending l3 flush.

			fatal("cgm_mesi_gpu_l2_downgrade_ack() miss on ack needs to be implemented blk 0x%08x\n",
					(message_packet->address & cache->block_address_mask));
	}




	//check pending state
	pending_bit = cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way);

	if(pending_bit == 0)
		fatal("block 0x%08x %s downgrade ack BUG DG ID %llu type %d state %d cycle %llu\n",
				(message_packet->address & cache->block_address_mask), cache->name, message_packet->downgrade_id, message_packet->access_type, *cache_block_state_ptr, P_TIME);
	assert(pending_bit == 1);

	//get number of sharers
	sharers = cgm_cache_get_num_shares(gpu, cache, message_packet->set, message_packet->way);
	assert(sharers == 1 || sharers == 0);

	//search the WB buffer for the data
	write_back_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);


	dirty_in_core = cgm_cache_get_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);
	assert(dirty_in_core == 1 || dirty_in_core == 0);

	/*if(dirty_in_core)
		warning("GPU L2 downgrade ack with block dirtry in core (should be ok) 0x%08x\n",
				(message_packet->address & cache->block_address_mask));*/

	GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s downgrade ack ID %llu type %d state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, *cache_block_state_ptr, P_TIME);

	switch(*cache_block_state_ptr)
	{
		case cgm_cache_block_noncoherent:
		case cgm_cache_block_owned:
		case cgm_cache_block_shared:
		case cgm_cache_block_invalid:
			warning("block 0x%08x %s downgrade ack ID %llu type %d state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, message_packet->access_type, *cache_block_state_ptr, P_TIME);

			fatal("cgm_mesi_l2_downgrade_ack(): L2 id %d invalid block state on downgrade_ack as %s address %u\n",
				cache->id, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr), message_packet->address);
			break;


		case cgm_cache_block_exclusive:
		case cgm_cache_block_modified:

			//join the downgrade with pending_request

			//pull the get request from the pending request buffer
			pending_request = cache_search_pending_request_buffer(cache, message_packet->address);

			/*if not found uh-oh...*/
			assert(pending_request);

			//pending request better be waiting on the downgrade
			assert(pending_request->downgrade == 1);

			/*the address better be the same too...*/
			assert(pending_request->address == message_packet->address);

			//downgrade the local block
			if(pending_request->set != message_packet->set || pending_request->way != message_packet->way)
			{
				ort_dump(cache);

				cgm_cache_dump_set(cache, message_packet->set);

				printf("pr set %d tag %d pr way %d mp set %d mp tag %d mp way %d\n",
						pending_request->set, pending_request->tag, pending_request->way, message_packet->set, message_packet->tag, message_packet->way);

				fatal("cgm_mesi_gpu_l2_downgrade_ack(): %s access id %llu blk_addr 0x%08x type %d start_cycle %llu end_cycle %llu\n",
						cache->name, message_packet->access_id, message_packet->address & cache->block_address_mask, message_packet->access_type,
						message_packet->start_cycle, message_packet->end_cycle);
			}

			//set the block state shared
			assert(pending_request->set == message_packet->set && pending_request->way == message_packet->way);
			cgm_cache_set_block_state(cache, pending_request->set, pending_request->way, cgm_cache_block_shared);

			//clear the pending bit
			cgm_cache_clear_dir_pending_bit(cache, pending_request->set, pending_request->way);


			//yea yea i know this is probably a bad way to do this, maybe a switch would be better.
			//Differentiate between the intra GPU downgrade and the CPU-GPU get_fwd downgrade....
			if(pending_request->access_type == cgm_access_get || pending_request->access_type == cgm_access_getx
					|| pending_request->access_type == cgm_access_load_retry)
			{

				//intra GPU downgrade

				//set the new sharer bit in the directory
				cgm_cache_set_dir(cache, pending_request->set, pending_request->way, pending_request->l1_cache_id);

				//mark dirty in core if downgrade ack or local block is modified.
				if(message_packet->cache_block_state == cgm_cache_block_modified || *cache_block_state_ptr == cgm_cache_block_modified)
					cgm_cache_set_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);

				//send puts to requesting core
				//set access type
				pending_request->access_type = cgm_access_puts;

				//set the block state
				pending_request->cache_block_state = cgm_cache_block_shared;
				//end uncomment here

				//set message package size
				pending_request->size = packet_set_size(cache->block_size);

				//transmit block to requesting node
				pending_request = list_remove(cache->pending_request_buffer, pending_request);
				list_enqueue(cache->Tx_queue_top, pending_request);
				advance(cache->cache_io_up_ec);
			}
			else if (pending_request->access_type == cgm_access_get_fwd)
			{
				//CPU-GPU get_fwd downgrade

				//the block is actually in the E/M state at L3, but has been shared within the GPU
				GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s downgrade ack with get_fwd ID %llu DIC %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id, dirty_in_core, P_TIME);

				//If the block is dirty_in_core we need to process a sharing write-back.

				///////////////////
				//Directory actions
				///////////////////

				//all we need to do is clear the dirt in core bit
				//probably dont' need this but do it anyways just to make sure.
				cgm_cache_clear_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);

				//forward block to requesting core


				/////////
				//GET_FWD
				/////////

				//prepare to forward the block
				pending_request->size = packet_set_size(cache->block_size);
				pending_request->cache_block_state = cgm_cache_block_shared;

				//set access type
				pending_request->access_type = cgm_access_puts;

				//fwd block to requesting core
				//update routing headers swap dest and src
				//requesting node

				//set dest
				pending_request->dest_name = str_map_value(node_strn_map, pending_request->src_id);
				pending_request->dest_id = str_map_string(node_strn_map, pending_request->src_name);

				/*if((pending_request->address & cache->block_address_mask) == 0x0812b300)
					fatal("get_fwd access sfinish type is %s source name %s id %d dest name %s id %d access id %llu\n",
							str_map_value(&cgm_mem_access_strn_map, pending_request->access_type),
							pending_request->src_name,
							pending_request->src_id,
							pending_request->dest_name,
							pending_request->dest_id,
							pending_request->access_id);*/


				//owning node L2
				//pending_request->src_name = cache->name;
				//pending_request->src_id = str_map_string(node_strn_map, cache->name);


				//transmit block to requesting node
				pending_request = list_remove(cache->pending_request_buffer, pending_request);
				list_enqueue(cache->Tx_queue_bottom, pending_request);
				advance(cache->cache_io_down_ec);

				///////////////
				//get_fwd_ack
				///////////////

				//send the get_fwd_ack to L3 cache.
				//create and init get__fwd_ack packet
				reply_packet = packet_create();
				assert(reply_packet);

				//set access type
				init_downgrade_ack_packet(reply_packet, message_packet->address);

				//date is dirty in core so send down to L3
				if(dirty_in_core)
				{
					//set block state & size
					reply_packet->cache_block_state = cgm_cache_block_modified;
					reply_packet->size = packet_set_size(cache->block_size);
				}
				else
				{
					//set block state & size
					reply_packet->cache_block_state = cgm_cache_block_shared;
					reply_packet->size = HEADER_SIZE;
				}

				//send reply to L3
				//fakes src as the requester
				reply_packet->l2_cache_id = pending_request->l2_cache_id;
				reply_packet->l2_cache_name = pending_request->src_name;

				//dest is set in hub-iommu

				//assert(get_getx_fwd_reply_packet->access_type == cgm_access_downgrade_ack);
				//transmit getx_fwd_ack to L3 (home)
				list_enqueue(cache->Tx_queue_bottom, reply_packet);
				advance(cache->cache_io_down_ec);

			}
			else
			{
				fatal("cgm_mesi_gpu_l2_downgrade_ack(): what happened? type is %s\n",
						str_map_value(&cgm_mem_access_strn_map, pending_request->access_type));
			}


			//destroy the downgrade message because we don't need it anymore.
			message_packet = list_remove(cache->last_queue, message_packet);
			packet_destroy(message_packet);

			break;
	}

	return;
}

void cgm_mesi_gpu_l2_getx_inval_ack(struct cache_t *cache, struct cgm_packet_t *message_packet){

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	struct cgm_packet_t *wb_packet = NULL;
	struct cgm_packet_t *pending_request_packet = NULL;
	struct cgm_packet_t *reply_packet = NULL;
	int error = 0;
	int pending_bit = 0;
	int dirty_in_core = 0;

	//charge delay
	GPU_PAUSE(cache->latency);


	//getx_inval_ack from L1 D cache...
	assert(message_packet->cache_block_state == cgm_cache_block_modified || message_packet->cache_block_state == cgm_cache_block_invalid);

	//get the address set and tag
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);
	assert(*cache_block_hit_ptr == 1);

	pending_bit = cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way);
	assert(pending_bit == 1);

	wb_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);
	assert(!wb_packet);

	dirty_in_core = cgm_cache_get_block_dirty_in_core_bit(cache, message_packet->set, message_packet->way);
	assert(dirty_in_core == 0 || dirty_in_core == 1); //when this is 1 check that it is being written back to L3 as a sharing write-back

	//there must be a pending request packet
	pending_request_packet = cache_search_pending_request_buffer(cache, message_packet->address);
	assert(pending_request_packet);
	assert(pending_request_packet->getx_inval_n > 0);
	assert(pending_request_packet->set == message_packet->set && pending_request_packet->way == message_packet->way);


	//a getx_inval_ack has come from an L1 cache.
	//decrement the pending_request_packet counter
	//count down the number of acks and when 0 send the block to the requesting core.
	pending_request_packet->getx_inval_n--;

	//the l1 cache should no longer have this block
	if(pending_request_packet->getx_inval_n == 0)
	{
		error = cache_validate_block_flushed_from_l1(gpu_v_caches, message_packet->l1_cache_id, message_packet->address);
		assert(error == 0);

		if(error != 0)
		{
			struct cgm_packet_t *L1_wb_packet = cache_search_wb(&gpu_v_caches[message_packet->l1_cache_id], message_packet->tag, message_packet->set);

			if(L1_wb_packet)
				fatal("wbp found %llu\n", L1_wb_packet->evict_id);


			fatal("cgm_mesi_gpu_l2_flush_block_ack(): %s error %d as %s access_id %llu address 0x%08x blk_addr 0x%08x set %d tag %d way %d state %d cycle %llu\n",
				cache->name, error, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr),
				message_packet->access_id, message_packet->address, message_packet->address & cache->block_address_mask,
				message_packet->set, message_packet->tag, message_packet->way, *cache_block_state_ptr, P_TIME);
		}
	}

	//when zero all pending acks have returned. forward block to requesting core.
	if(pending_request_packet->getx_inval_n == 0
			&& (pending_request_packet->access_type == cgm_access_getx || pending_request_packet->access_type == cgm_access_store_retry))
	{

		//forwarding block to requesting L1 cache in the modified state
		//the block better be out of all L1 caches.
		error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, pending_request_packet->address);
		assert(error == 0);

		//if the block is in the S/E state set M before sending up
		cgm_cache_set_block_state(cache, pending_request_packet->set, pending_request_packet->way, cgm_cache_block_modified);


		GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s getx_inval_ack hit single shared ID %llu type %d state %d cycle %llu\n",
				(pending_request_packet->address & cache->block_address_mask),
				cache->name, pending_request_packet->access_id,
				pending_request_packet->access_type, *cache_block_state_ptr, P_TIME);

		//when ready
		//clear the directory
		//set the new exclusive holder (requester)
		//set cache block state
		//PutX to requester


		//update message status
		pending_request_packet->access_type = cgm_access_putx;

		//set cache block state modified
		pending_request_packet->cache_block_state = cgm_cache_block_modified;

		//set message status and size
		pending_request_packet->size = packet_set_size(gpu_v_caches[pending_request_packet->l1_cache_id].block_size); //this should be L1 D cache block size.

		//update directory
		cgm_cache_clear_dir(cache, pending_request_packet->set, pending_request_packet->way);

		cgm_cache_set_dir(cache, pending_request_packet->set, pending_request_packet->way, pending_request_packet->l1_cache_id);

		/*reset mp flags*/
		pending_request_packet->coalesced = 0;
		pending_request_packet->assoc_conflict = 0;

		//send up to L1 D cache
		pending_request_packet = list_remove(cache->pending_request_buffer, pending_request_packet);
		list_enqueue(cache->Tx_queue_top, pending_request_packet);
		advance(cache->cache_io_up_ec);

		//fatal("cgm_mesi_gpu_l2_getx_inval_ack() inval_n = 0\n");
	}
	else if(pending_request_packet->getx_inval_n == 0 && pending_request_packet->access_type == cgm_access_getx_fwd)
	{

		/*fatal("GPU L2 caught the getx_inval pending request id %llu\n", pending_request_packet->access_id);*/

		GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s getx_fwd_inval_ack ID %llu type %d state %d cycle %llu\n",
				(pending_request_packet->address & cache->block_address_mask),
				cache->name, pending_request_packet->access_id,
				pending_request_packet->access_type, *cache_block_state_ptr, P_TIME);

		///////////////////
		//Directory actions
		///////////////////


		//forwarding block to requesting CPU core in the modified state
		//the block better be out of all GPU L1 caches.
		error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, pending_request_packet->address);
		assert(error == 0);

		//clear the directory (clears pending bit)
		cgm_cache_clear_dir(cache, pending_request_packet->set, pending_request_packet->way);

		//clear the diry in core bit (this could be dirty in core if shared within the GPU)
		cgm_cache_clear_block_dirty_in_core_bit(cache, pending_request_packet->set, pending_request_packet->way);

		//invalidate the local cache line
		cgm_cache_set_block_state(cache, pending_request_packet->set, pending_request_packet->way, cgm_cache_block_invalid);


		//////////
		//GETX_FWD
		//////////


		//send putx to requesting core
		pending_request_packet->size = packet_set_size(cache->block_size);

		//set access type and block state
		pending_request_packet->access_type = cgm_access_putx;
		pending_request_packet->cache_block_state = cgm_cache_block_modified;

		/*reset mp flags*/
		pending_request_packet->coalesced = 0;
		pending_request_packet->assoc_conflict = 0;

		pending_request_packet->dest_name = str_map_value(node_strn_map, pending_request_packet->src_id);
		pending_request_packet->dest_id = str_map_string(node_strn_map, pending_request_packet->src_name);

		//transmit block to requesting node
		pending_request_packet = list_remove(cache->pending_request_buffer, pending_request_packet);
		list_enqueue(cache->Tx_queue_bottom, pending_request_packet);
		advance(cache->cache_io_down_ec);


		///////////////
		//getx_fwd_ack
		///////////////

		//send the get_fwd_ack to L3 cache.
		//create and init get__fwd_ack packet
		reply_packet = packet_create();
		assert(reply_packet);

		//set access type
		init_getx_fwd_ack_packet(reply_packet, message_packet->address);

		//if dirty in core send the dirty block down to L3
		if(dirty_in_core == 1 || message_packet->cache_block_state == cgm_cache_block_modified
				|| *cache_block_state_ptr == cgm_cache_block_modified)
		{
			reply_packet->cache_block_state = cgm_cache_block_modified;
			reply_packet->size = packet_set_size(cache->block_size);
		}
		else
		{
			reply_packet->cache_block_state = cgm_cache_block_exclusive;
			reply_packet->size = HEADER_SIZE;
		}

		//send reply to L3 - fakes src as the requester
		reply_packet->access_id = pending_request_packet->access_id; //for error checking
		reply_packet->l2_cache_id = pending_request_packet->l2_cache_id; //***change here***
		reply_packet->l2_cache_name = pending_request_packet->src_name;

		//assert(get_getx_fwd_reply_packet->access_type == cgm_access_downgrade_ack);
		//transmit getx_fwd_ack to L3 (home)
		list_enqueue(cache->Tx_queue_bottom, reply_packet);
		advance(cache->cache_io_down_ec);

		//done
	}
	//can't have an else here really


	//free the getx_inval_ack
	message_packet = list_remove(cache->last_queue, message_packet);
	packet_destroy(message_packet);

	//fatal("GPU L2 getx inval ack blk_addr 0x%08x\n", message_packet->address & cache->block_address_mask);
	//STOP;

	return;
}


void cgm_mesi_gpu_l2_flush_block_ack(struct cache_t *cache, struct cgm_packet_t *message_packet){

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	struct cgm_packet_t *wb_packet = NULL;
	struct cgm_packet_t *pending_request_packet = NULL;
	struct cgm_packet_t *reply_packet = NULL;
	/*struct cache_t *l3_cache_ptr = NULL;*/
	//int l3_map = 0;
	int error = 0;
	int pending_bit = 0;

	//charge delay
	GPU_PAUSE(cache->latency);

	//flush block ack from L1 D cache...

	//get the address set and tag
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	pending_bit = cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way);

	/*if(*cache_block_hit_ptr == 1)
	{
		if(pending_bit != 1)
		{
				fatal("cgm_mesi_gpu_l2_flush_block_ack(): %s block hit as %s access_id %llu address 0x%08x blk_addr 0x%08x set %d tag %d way %d state %d cycle %llu\n",
				cache->name, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr),
				message_packet->evict_id, message_packet->address, message_packet->address & cache->block_address_mask,
				message_packet->set, message_packet->tag, message_packet->way, *cache_block_state_ptr, P_TIME);
				assert(pending_bit == 1);
		}
	}*/

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);


	error = cache_validate_block_flushed_from_l1(gpu_v_caches, message_packet->l1_cache_id, message_packet->address);
	if(error != 0)
	{
		struct cgm_packet_t *L1_wb_packet = cache_search_wb(&gpu_v_caches[message_packet->l1_cache_id], message_packet->tag, message_packet->set);

		if(L1_wb_packet)
			fatal("wbp found %llu\n", L1_wb_packet->evict_id);


		fatal("cgm_mesi_gpu_l2_flush_block_ack(): %s error %d as %s access_id %llu address 0x%08x blk_addr 0x%08x set %d tag %d way %d state %d cycle %llu\n",
			cache->name, error, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr),
			message_packet->access_id, message_packet->address, message_packet->address & cache->block_address_mask,
			message_packet->set, message_packet->tag, message_packet->way, *cache_block_state_ptr, P_TIME);
	}

	assert(error == 0);

	//state should be either invalid of modified.
	assert(message_packet->cache_block_state == cgm_cache_block_modified || message_packet->cache_block_state == cgm_cache_block_invalid);

	//find the block in the local WB cache_get_block_status buffer
	wb_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);


	if(message_packet->cache_block_state == cgm_cache_block_modified)
		assert(message_packet->size == 72);

	if(*cache_block_state_ptr == 1 && wb_packet)
	{
		if(LEVEL == 2 || LEVEL == 3)
		{
			cgm_cache_dump_set(cache, message_packet->set);
			printf("\n");

			ort_dump(cache);
			printf("\n");

			cache_dump_queue(cache->pending_request_buffer);
			printf("\n");

			cache_dump_queue(cache->write_back_buffer);
			printf("\n");

			fatal("block 0x%08x %s *cache_block_state_ptr != 0 ID %llu type %d state %d set %d tag %d way %d cycle %llu\n",
				(message_packet->address & cache->block_address_mask), cache->name, message_packet->evict_id, message_packet->access_type, *cache_block_state_ptr,
				message_packet->set, message_packet->tag, message_packet->way, P_TIME);
		}
	}




	/*if a L1 flush check write back*/
	if(wb_packet)
	{

		GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s flush blk ack with wb in l2 id %llu flush_join %d pending bit %d ID %llu type %d state %d cycle %llu\n",
				(message_packet->address & cache->block_address_mask), cache->name, message_packet->evict_id, wb_packet->L3_flush_join, pending_bit, message_packet->evict_id,
				message_packet->access_type, *cache_block_state_ptr, P_TIME);


		wb_packet->inval_ack--;

		//wait for all holding CUs to return the ack
		if(wb_packet->inval_ack == 0)
		{

			//not caused by L3 evict
			if(wb_packet->L3_flush_join == 0)
			{
				/*if there is a pending get_fwd or getx_fwd request join here*/
				pending_request_packet = cache_search_pending_request_buffer(cache, message_packet->address);

				if(pending_request_packet)
				{

					CPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s flush blk ack pending packet present cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name, P_TIME);

					warning("block 0x%08x %s flush blk ack pending get/getx_fwd cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, P_TIME);

					//printf("%s processing get/getx_fwd join after evict access id %llu access type %d\n", cache->name, pending_request_packet->access_id, pending_request_packet->access_type);

					assert(pending_request_packet->access_type == cgm_access_get_fwd || pending_request_packet->access_type == cgm_access_getx_fwd);
					assert((pending_request_packet->address & cache->block_address_mask) == (message_packet->address & cache->block_address_mask));
					assert(pending_request_packet->start_cycle != 0);
					assert(pending_request_packet->set == message_packet->set);


					//prepare to forward the block
					if(pending_request_packet->access_type == cgm_access_get_fwd)
					{	//set access type
						pending_request_packet->access_type = cgm_access_puts;
						//set the block state
						pending_request_packet->cache_block_state = cgm_cache_block_shared;

					}
					else
					{
						/*OK to putx and modified because the getx means intent to store.*/
						//set access type
						pending_request_packet->access_type = cgm_access_putx;
						//set the block state
						pending_request_packet->cache_block_state = cgm_cache_block_modified;
					}

					//set message package size
					pending_request_packet->size = packet_set_size(cache->block_size);

					//fwd block to requesting core
					//update routing headers swap dest and src
					//requesting node

					pending_request_packet->dest_name = str_map_value(node_strn_map, pending_request_packet->src_id);
					pending_request_packet->dest_id = str_map_string(node_strn_map, pending_request_packet->src_name);

					//owning node L2
					pending_request_packet->src_name = cache->name;
					pending_request_packet->src_id = str_map_string(node_strn_map, cache->name);

					//transmit block to requesting node
					pending_request_packet = list_remove(cache->pending_request_buffer, pending_request_packet);
					list_enqueue(cache->Tx_queue_bottom, pending_request_packet);
					advance(cache->cache_io_down_ec);

					///////////////
					//downgrade/getx_inval_ack
					///////////////

					//send the downgrade/getx_inval ack to L3 cache.

					//create downgrade_ack
					reply_packet = packet_create();
					assert(reply_packet);

					/*NOTE access type changed above!!!*/
					if(pending_request_packet->access_type == cgm_access_puts)
					{
						init_downgrade_ack_packet(reply_packet, pending_request_packet->address);
					}
					else
					{
						init_getx_fwd_ack_packet(reply_packet, pending_request_packet->address);
					}

					reply_packet->access_id = pending_request_packet->access_id;

					//determine if we need to send dirty data to L3

					//assert(message_packet->cache_block_state == cgm_cache_block_modified || wb_packet->cache_block_state == cgm_cache_block_modified);
					if(message_packet->cache_block_state == cgm_cache_block_modified || wb_packet->cache_block_state == cgm_cache_block_modified)
					{
						reply_packet->cache_block_state = cgm_cache_block_modified;
						reply_packet->size = packet_set_size(cache->block_size);
					}
					else
					{
						reply_packet->cache_block_state = cgm_cache_block_shared;
						reply_packet->size = HEADER_SIZE;
					}

					//fakes src as the requester
					reply_packet->l2_cache_id = pending_request_packet->l2_cache_id;
					reply_packet->l2_cache_name = pending_request_packet->src_name;

					//transmit downgrad_ack to L3 (home)
					list_enqueue(cache->Tx_queue_bottom, reply_packet);
					advance(cache->cache_io_down_ec);

					//destroy write back packet
					wb_packet = list_remove(cache->write_back_buffer, wb_packet);
					free(wb_packet);

				}
				else
				{
					//incoming data from L1 is dirty
					if(wb_packet->cache_block_state == cgm_cache_block_modified || message_packet->cache_block_state == cgm_cache_block_modified)
					{
						//merge the block.
						wb_packet->cache_block_state = cgm_cache_block_modified;

						/*clear the pending bit and leave the wb in the buffer*/
						wb_packet->flush_pending = 0;
					}
					else
					{

						//Neither the l1 line or L2 line are dirty clear the wb from the buffer
						assert(wb_packet->cache_block_state == cgm_cache_block_exclusive);
						wb_packet = list_remove(cache->write_back_buffer, wb_packet);
						free(wb_packet);
					}

					//free the flush message packet
					//message_packet = list_remove(cache->last_queue, message_packet);
					//packet_destroy(message_packet);
				}
			}
			else
			{

				assert(wb_packet->L3_flush_join == 1); /*pull the join if there is one waiting*/

				CPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s flush blk completing with pending L3 flush join cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name, P_TIME);

				/*its's possible for L3 to evict a block during the epoch of the L2's block eviction movement.*/
				pending_request_packet = cache_search_pending_request_buffer(cache, message_packet->address);
				assert(pending_request_packet);

				assert(pending_request_packet->access_type == cgm_access_gpu_flush
						|| pending_request_packet->access_type == cgm_access_flush_block);


				//error check should not be a pending get/getx_fwd in the buffer
				error = cache_search_pending_request_get_getx_fwd(cache, (message_packet->address & cache->block_address_mask));
				assert(error == 0);

				wb_packet->flush_pending = 0;
				wb_packet->L3_flush_join = 0;

				if(wb_packet->cache_block_state == cgm_cache_block_modified || message_packet->cache_block_state == cgm_cache_block_modified)
				{
					pending_request_packet->size = packet_set_size(cache->block_size);
					pending_request_packet->cache_block_state = cgm_cache_block_modified;
				}
				else
				{
					pending_request_packet->size = HEADER_SIZE;
					pending_request_packet->cache_block_state = cgm_cache_block_invalid;
				}

				if(pending_request_packet->access_type == cgm_access_gpu_flush)
				{
					pending_request_packet->access_type = cgm_access_gpu_flush_ack;
				}
				else
				{
					pending_request_packet->access_type = cgm_access_flush_block_ack;
				}

				pending_request_packet->l2_cache_id = gpu_core_id;
				pending_request_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

				//reply to the L3 cache
				pending_request_packet = list_remove(cache->pending_request_buffer, pending_request_packet);
				list_enqueue(cache->Tx_queue_bottom, pending_request_packet);
				advance(cache->cache_io_down_ec);

				//free the write back
				wb_packet = list_remove(cache->write_back_buffer, wb_packet);
				packet_destroy(wb_packet);

				//free the message packet
				//message_packet = list_remove(cache->last_queue, message_packet);
				//packet_destroy(message_packet);

			}

		}
	}


	/*stats*/
	cache->EvictInv++;

	//free the message packet
	message_packet = list_remove(cache->last_queue, message_packet);
	packet_destroy(message_packet);


	return;
}



int cgm_mesi_gpu_l2_write_block(struct cache_t *cache, struct cgm_packet_t *message_packet){

	int row = 0;
	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;

	enum cgm_cache_block_state_t victim_trainsient_state;
	//struct cgm_packet_t *pending_get_fwd_getx_fwd_request = NULL;
	int pending_join_bit = -1;
	int pending_upgrade_bit = -1;

	int error = 0;


	//struct cgm_packet_t *pending_upgrade = NULL;
	struct cgm_packet_t *pending_get_getx_fwd = NULL;
	struct cgm_packet_t *nack_packet = NULL;
	//struct cgm_packet_t *pending_request = NULL;

	cache_get_transient_block(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	if(*cache_block_hit_ptr != 1)
	{
		cgm_cache_dump_set(cache, message_packet->set);

		warning("block 0x%08x %s write block ID %llu type %s state %d way %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
			str_map_value(&cgm_mem_access_strn_map, message_packet->access_type), message_packet->way, message_packet->cache_block_state, P_TIME);
		fflush(stderr);
	}

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);

	//victim should have been in transient state
	victim_trainsient_state = cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->way);
	assert(victim_trainsient_state == cgm_cache_block_transient);

	/*assert((message_packet->access_type == cgm_access_puts && message_packet->cache_block_state == cgm_cache_block_shared)
			|| (message_packet->access_type == cgm_access_put_clnx && message_packet->cache_block_state == cgm_cache_block_exclusive)
			|| (message_packet->access_type == cgm_access_putx && message_packet->cache_block_state == cgm_cache_block_modified));*/



	/*if(message_packet->access_id == 1201510)
		fatal("OK!\n");*/

	/*check the ort table for a pending join get/getx_fwd*/
	row = ort_search(cache, message_packet->tag, message_packet->set);
	pending_join_bit = cache->ort[row][2];
	assert(row != cache->mshr_size);


	pending_upgrade_bit = cgm_cache_get_block_upgrade_pending_bit(cache, message_packet->set, message_packet->way);
	assert(pending_upgrade_bit == 0);

	GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s write block ID %llu row %d pending bit %d type %d state %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
			row, pending_join_bit, message_packet->access_type, message_packet->cache_block_state, P_TIME);

	/*reply has returned for a previously sent gets/get/getx*/
	if(pending_join_bit == 1 && pending_upgrade_bit == 0)
	{

		if(message_packet->l3_pending == 1)
		{

			warning("gpu l2 write block l3 pending bit in gpu id %llu blk 0x%08x id %llu cycle %llu\n",
					message_packet->access_id,
					(message_packet->address & cache->block_address_mask),
					message_packet->access_id,
					P_TIME);

			pending_get_getx_fwd = cache_search_pending_request_buffer(cache, (message_packet->address & cache->block_address_mask));
			if(pending_get_getx_fwd)
				fatal("BAD! pending packet but pending join bit %d\n", pending_join_bit);

			CPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s write block failed on conflict retrying access ID %llu type %d state %d cycle %llu\n",
				(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
				message_packet->access_type, message_packet->cache_block_state, P_TIME);

			/*ORT entry is already set do just send down as a get/getx*/
			assert(row < cache->mshr_size);
			assert(pending_join_bit == 1);


			//clear the conflict bit and other flags in the packet
			message_packet->l3_pending = 0;

			message_packet->coalesced = 0;
			message_packet->assoc_conflict = 0;


			//add some routing/status data to the packet
			if(message_packet->l1_access_type == cgm_access_getx)
			{
				message_packet->access_type = cgm_access_getx;
			}
			else
			{
				message_packet->access_type = cgm_access_get;
			}


			//L3 should see the entire GPU as a single core.
			message_packet->l2_cache_id = gpu_core_id;
			message_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);


			//transmit to L3
			cache_put_io_down_queue(cache, message_packet);

			return 1;

		}


		ort_clear(cache, message_packet);

		cgm_cache_set_block(cache, message_packet->set, message_packet->way, message_packet->tag, message_packet->cache_block_state);

		//set retry state
		message_packet->access_type = cgm_cache_get_retry_state(message_packet->gpu_access_type);

		/*if((message_packet->address & cache->block_address_mask) == 0x08123380)
		warning("GPU L2 $ id %d write block %llu phy address 0x%08x state %d\n",
					cache->id, message_packet->access_id,
					(message_packet->address & cache->block_address_mask),
					message_packet->cache_block_state);
		fflush(stderr);*/

		message_packet = list_remove(cache->last_queue, message_packet);
		list_enqueue(cache->retry_queue, message_packet);
	}

	//We have a eviction conflict OR a pending get/getx_fwd packet
	else if(pending_join_bit == 0 && pending_upgrade_bit == 0)
	{
		/*ORT is set, this means either a pending request is here OR L3 evicted the line*/
		//first look for a pending request in the buffer
		pending_get_getx_fwd = cache_search_pending_request_buffer(cache, (message_packet->address & cache->block_address_mask));

		if(pending_get_getx_fwd)
		{
			/*pending get/getx_fwd*/

			warning("GPU PROCESS JOIN block 0x%08x %s pending get/getx_fwd ID %llu type %d state %d num pending %d cycle %llu\n",
					(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
					message_packet->access_type, message_packet->cache_block_state,
					(ort_get_num_coal(cache, message_packet->tag, message_packet->set) + 1), P_TIME);

			/*assert(pending_get_getx_fwd->access_type == cgm_access_get_fwd || pending_get_getx_fwd->access_type == cgm_access_getx_fwd);
			assert((pending_get_getx_fwd->address & cache->block_address_mask) == (message_packet->address & cache->block_address_mask));
			assert(pending_get_getx_fwd->downgrade_pending == 1);

			set the number of coalesced accesses
			pending_get_getx_fwd->downgrade_pending = (ort_get_num_coal(cache, message_packet->tag, message_packet->set) + 1);*/ // + 1 account for the packet that was not coalesced and went to L3

			//ok don't join nack the pending request back
			//this prevents a deadlock situation
			error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, message_packet->address);
			assert(error == 0);

			//assert(cache->sets[message_packet->set].blocks[message_packet->way].directory_entry.entry == 0);

			assert(pending_get_getx_fwd->access_type == cgm_access_getx_fwd
					|| pending_get_getx_fwd->access_type == cgm_access_get_fwd);

			/*two part reply (1) send nack to L3 and (2) send nack to requesting L2*/
			//set access type

			if(pending_get_getx_fwd->access_type == cgm_access_getx_fwd)
				pending_get_getx_fwd->access_type = cgm_access_getx_fwd_nack;
			else
				pending_get_getx_fwd->access_type = cgm_access_downgrade_nack;

			//for routing to L2 cache
			pending_get_getx_fwd->downgrade_ack = 2;

			//set the block state
			pending_get_getx_fwd->cache_block_state = cgm_cache_block_invalid;

			//set message package size
			pending_get_getx_fwd->size = HEADER_SIZE;

			//fwd nack to requesting core
			//update routing headers swap dest and src
			//requesting node
			pending_get_getx_fwd->dest_name = str_map_value(node_strn_map, pending_get_getx_fwd->src_id);
			pending_get_getx_fwd->dest_id = str_map_string(node_strn_map, pending_get_getx_fwd->src_name);

			//owning node L2
			pending_get_getx_fwd->src_name = cache->name;
			pending_get_getx_fwd->src_id = str_map_string(node_strn_map, cache->name);

			//transmit nack to L2
			pending_get_getx_fwd = list_remove(cache->pending_request_buffer, pending_get_getx_fwd);
			list_enqueue(cache->Tx_queue_bottom, pending_get_getx_fwd);
			advance(cache->cache_io_down_ec);

			////////
			//part 2
			////////

			//create downgrade_nack
			nack_packet = packet_create();
			assert(nack_packet);

			if(pending_get_getx_fwd->access_type == cgm_access_getx_fwd)
				init_getx_fwd_nack_packet(nack_packet, message_packet->address);
			else
				init_downgrade_nack_packet(nack_packet, message_packet->address);

			nack_packet->l2_cache_id = gpu_core_id;
			nack_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

			nack_packet->cache_block_state = message_packet->cache_block_state; //tell L3 what the block state is.

			//transmit block to L3
			list_enqueue(cache->Tx_queue_bottom, nack_packet);
			advance(cache->cache_io_down_ec);

		}
		else
		{
			/*eviction conflict*/

			assert(!pending_get_getx_fwd);
			assert(message_packet->l3_pending == 0);

			assert(message_packet->access_type == cgm_access_put_clnx || message_packet->access_type == cgm_access_putx
					|| message_packet->access_type == cgm_access_puts);

			GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s write block failed on conflict (L3 evict/upgrade_inval) retrying access ID %llu type %d state %d cycle %llu\n",
					(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
					message_packet->access_type, message_packet->cache_block_state, P_TIME);

			/*ORT entry is already set so just send down as a get/getx*/
			assert(row < cache->mshr_size);
			assert(pending_join_bit == 0);
			assert(cgm_cache_get_block_upgrade_pending_bit(cache, message_packet->set, message_packet->way) == 0);

			assert(cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way) == 0);

			//clear the ort pending join bit
			ort_clear_pending_join_bit(cache, row, message_packet->tag, message_packet->set);

			//add some routing/status data to the packet
			if(message_packet->l1_access_type == cgm_access_getx)
			{
				if(message_packet->cpu_access_type != cgm_access_store)
					printf("gpu l2 write block crashing id %llu blk addr 0x%08x\n", message_packet->access_id, (message_packet->address & cache->block_address_mask));

				assert(message_packet->cpu_access_type == cgm_access_store);

				message_packet->access_type = cgm_access_getx;
			}
			else if(message_packet->l1_access_type == cgm_access_get)
			{
				assert(message_packet->cpu_access_type == cgm_access_load);

				message_packet->access_type = cgm_access_get;
			}
			else
			{
				fatal("cgm_mesi_gpu_l2_write_block(): unexpected l1 access type as %d\n", message_packet->access_type);
			}

			/*should be headed to L3*/
			assert(hub_iommu_connection_type == hub_to_l3);


			/*reset mp flags*/
			message_packet->coalesced = 0;
			message_packet->assoc_conflict = 0;

			message_packet->size = HEADER_SIZE;

			message_packet->l2_cache_id = gpu_core_id;
			message_packet->l2_cache_name = str_map_value(&l2_strn_map, gpu_core_id);

			cache_put_io_down_queue(cache, message_packet);

			return 1;

		}

		//clear the conflict bit in the packet if set
		if(message_packet->l3_pending == 1)
			message_packet->l3_pending = 0;

		//write the block
		cgm_cache_set_block(cache, message_packet->set, message_packet->way, message_packet->tag, message_packet->cache_block_state);

		//set retry state
		message_packet->access_type = cgm_cache_get_retry_state(message_packet->gpu_access_type);

		//clear the ort
		ort_clear(cache, message_packet);

		//remove the pending request (upgrade)
		message_packet = list_remove(cache->last_queue, message_packet);
		list_enqueue(cache->retry_queue, message_packet);

	}
	else
	{
		warning("block 0x%08x %s write block bad case ID %llu type %d state %d set %d tag %d way %d pj_bit %d pu_bit %d cycle %llu\n",
			(message_packet->address & cache->block_address_mask), cache->name, message_packet->access_id,
			message_packet->access_type, message_packet->cache_block_state,
			message_packet->set, message_packet->tag, message_packet->way,
			pending_join_bit, pending_upgrade_bit, P_TIME);

		fatal("cgm_mesi_gpu_l2_write_block(): bad case\n");

	}

	/*stats*/
	cache->TotalWriteBlocks++;

	return 0;
}



int cgm_mesi_gpu_l2_write_back(struct cache_t *cache, struct cgm_packet_t *message_packet){

	int cache_block_hit;
	int cache_block_state;
	int *cache_block_hit_ptr = &cache_block_hit;
	int *cache_block_state_ptr = &cache_block_state;
	struct cgm_packet_t *wb_packet;
	//enum cgm_cache_block_state_t block_trainsient_state;
	//int l3_map;
	int error = 0;
	struct cache_t *l3_cache_ptr = NULL;

	//charge the delay
	GPU_PAUSE(cache->latency);

	//we should only receive modified lines from L1 cache
	assert(message_packet->cache_block_state == cgm_cache_block_modified);

	//get the state of the local cache block
	cache_get_block_status(cache, message_packet, cache_block_hit_ptr, cache_block_state_ptr);

	/*reset mp flags*/
	assert(message_packet->coalesced == 0);
	assert(message_packet->assoc_conflict == 0);

	//check for block transient state
	/*block_trainsient_state = cgm_cache_get_block_transient_state(cache, message_packet->set, message_packet->way);
	if(block_trainsient_state == cgm_cache_block_transient)
	{
		if potentially merging in cache the block better not be transient, check that the tags don't match
		if they don't match the block is missing from both the cache and wb buffer when it should not be

		//check that the tags don't match. This should not happen as the request should have been coalesced at L1 D.
		assert(message_packet->tag != cache->sets[message_packet->set].blocks[message_packet->way].tag);
	}*/

	/*on a write back with inclusive caches L2 Merges the line
	if the write back is a surprise the block will be exclusive and old in the L2 cache.*/

	//WB from L1 D cache
	if(cache->last_queue == cache->Rx_queue_top)
	{

		assert(message_packet->size == 72);

		switch(*cache_block_state_ptr)
		{
			case cgm_cache_block_noncoherent:
			case cgm_cache_block_owned:
			case cgm_cache_block_shared:
				cgm_cache_dump_set(cache, message_packet->set);

				fatal("cgm_mesi_l2_write_back(): %s invalid block state on write back as %s wb_id %llu address 0x%08x blk_addr 0x%08x set %d tag %d way %d state %d cycle %llu\n",
					cache->name, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr),
					message_packet->write_back_id, message_packet->address, message_packet->address & cache->block_address_mask,
					message_packet->set, message_packet->tag, message_packet->way, *cache_block_state_ptr, P_TIME);
				break;

			case cgm_cache_block_invalid:

				//check WB buffer
				wb_packet = cache_search_wb(cache, message_packet->tag, message_packet->set);

				if(wb_packet)
				{
					GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s write back - write back merge ID %llu type %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name, message_packet->write_back_id, message_packet->access_type, P_TIME);

					//cache block found in the WB buffer merge the change here
					//set modified if the line was exclusive
					wb_packet->cache_block_state = cgm_cache_block_modified;
					/*if(wb_packet->flush_pending != 1)
						fatal("l2 block address 0x%08x\n", message_packet->address & cache->block_address_mask);*/

					//assert(wb_packet->flush_pending == 1);

					//destroy the L1 D WB packet
					message_packet = list_remove(cache->last_queue, message_packet);
					packet_destroy(message_packet);
				}
				else
				{

					printf("block 0x%08x %s gpu miss in l2 write back ID %llu type %d cycle %llu\n",
							(message_packet->address & cache->block_address_mask), cache->name, message_packet->write_back_id, message_packet->access_type, P_TIME);

					fatal("cgm_mesi_gpu_l2_write_back(): GPU miss in L2 write back should no longer happen??\n");

					/*this case shouldn't happen any longer with the new changes.*/
					//cgm_cache_dump_set(cache, message_packet->set);
					/* cache_dump_queue(cache->write_back_buffer);


					fatal("cgm_mesi_l2_write_back(): %s write back missing in cache %s writeback_id %llu address 0x%08x blk_addr 0x%08x set %d tag %d way %d state %d cycle %llu\n",
						cache->name, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr),
						message_packet->write_back_id, message_packet->address, message_packet->address & cache->block_address_mask,
						message_packet->set, message_packet->tag, message_packet->way, *cache_block_state_ptr, P_TIME);*/

					//fatal("cgm_mesi_l2_write_back(): block not in cache or wb at L2 on L1 WB. this should not be happening anymore\n");

					/*it is possible for the WB from L1 D to miss at the L2. This means there was a recent L2 eviction of the block*/

				}
				break;

			case cgm_cache_block_exclusive:
			case cgm_cache_block_modified:

				//hit in cache merge write back here.

				GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s write back - cache merge ID %llu type %d cycle %llu\n",
						(message_packet->address & cache->block_address_mask), cache->name, message_packet->write_back_id, message_packet->access_type, P_TIME);


				cgm_cache_set_block_state(cache, message_packet->set, message_packet->way, cgm_cache_block_modified);

				//clear the directory for another core to pull, but only if not already pending...
				if(cgm_cache_get_dir_pending_bit(cache, message_packet->set, message_packet->way) != 1)
					cgm_cache_clear_dir(cache,  message_packet->set, message_packet->way);

				error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, message_packet->address);
				if(error != 0)
				{
					struct cgm_packet_t *L1_wb_packet = cache_search_wb(&gpu_v_caches[cache->id], message_packet->tag, message_packet->set);

					if(L1_wb_packet)
						warning("wbp found %llu\n", L1_wb_packet->evict_id);


					fatal("cgm_mesi_gpu_l2_write_back(): %s error %d as %s access_id %llu address 0x%08x blk_addr 0x%08x set %d tag %d way %d state %d cycle %llu\n",
						cache->name, error, str_map_value(&cgm_cache_block_state_map, *cache_block_state_ptr),
						message_packet->access_id, message_packet->address, message_packet->address & cache->block_address_mask,
						message_packet->set, message_packet->tag, message_packet->way, *cache_block_state_ptr, P_TIME);

				}
				assert(error == 0);

				//destroy the L1 D WB message. L2 will clear its WB at an opportune time.
				message_packet = list_remove(cache->last_queue, message_packet);
				packet_destroy(message_packet);
				break;
		}

		/*stats*/
		cache->TotalWriteBackRecieved++;

	}
	//if here the L2 generated it's own write back.
	else if(cache->last_queue == cache->write_back_buffer)
	{
		//the wb should not be waiting on a flush to finish.
		assert(message_packet->flush_pending == 0); //verify that the wb has completed it's flush.
		assert(*cache_block_hit_ptr == 0); //verify block is not in cache.

		//verify that the block is out of L1
		error = cache_validate_block_flushed_from_gpu_l1(gpu_v_caches, message_packet->address);
		//
		if(error)
		{
			warning("cgm_mesi_gpu_l2_write_back(): GPU L2 is sending WB, but found valid block in L1. OK if block in L1 is shared\n");

			assert(error == 0);
		}


		//verify that there is only one wb in L2 for this block.
		error = cache_search_wb_dup_packets(cache, message_packet->tag, message_packet->set);
		assert(error == 1); //error == 1 i.e only one wb packet and we are about to send it.

		//if the line is still in the exclusive state at this point drop it.
		if(message_packet->cache_block_state == cgm_cache_block_exclusive)
		{

			GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s write back destroy ID %llu type %d cycle %llu\n",
					(message_packet->address & cache->block_address_mask), cache->name, message_packet->write_back_id, message_packet->access_type, P_TIME);


			/*stats*/
			cache->TotalWriteBackDropped++;

			message_packet = list_remove(cache->last_queue, message_packet);
			packet_destroy(message_packet);
		}
		else if (message_packet->cache_block_state == cgm_cache_block_modified)
		{
			GPUDEBUG(LEVEL == 2 || LEVEL == 3, "block 0x%08x %s write back sent (to L3) %llu type %d cycle %llu\n",
					(message_packet->address & cache->block_address_mask), cache->name, message_packet->write_back_id, message_packet->access_type, P_TIME);

			message_packet->size = packet_set_size(cache->block_size);

			if(hub_iommu_connection_type == hub_to_mc)
			{
				//message is going down to mc so its and mc_load
				message_packet->access_type = cgm_access_mc_store;

				message_packet->l2_cache_id = cache->id;
				message_packet->l2_cache_name = cache->name;

				SETROUTE(message_packet, cache, system_agent)

			}
			else
			{

				l3_cache_ptr = cgm_l3_cache_map(message_packet->set);
				message_packet->l2_cache_id = cache->id;
				message_packet->l2_cache_name = cache->name;

				SETROUTE(message_packet, cache, l3_cache_ptr)
			}


			//send the write back on.
			cache_put_io_down_queue(cache, message_packet);

			/*stats*/
			cache->TotalWriteBackSent++;
		}
		else
		{
			fatal("cgm_mesi_l2_write_back(): Invalid block state in write back buffer cycle %llu\n", P_TIME);
		}

		return 0;
	}
	else
	{
		fatal("cgm_mesi_l2_write_back(): Invalid queue cycle %llu\n", P_TIME);
	}

	return 1;
}
