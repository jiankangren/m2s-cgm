/*
 * mem-ctrl.c
 *
 *  Created on: Nov 25, 2014
 *      Author: stardica
 */


#include <cgm/mem-ctrl.h>

#include <mem-image/memory.h>

#include <mem-image/mmu.h>


/*
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lib/util/list.h>

#include <cgm/cgm.h>
#include <cgm/mem-ctrl.h>
#include <cgm/sys-agent.h>
#include <cgm/tasking.h>
#include <cgm/packet.h>
#include <cgm/cache.h>
*/

//#include <DRAMSim/DRAMSim.h>
//#include <dramsim/DRAMSim.h>


//structure declarations
struct mem_ctrl_t *mem_ctrl;
eventcount volatile *mem_ctrl_ec;
eventcount volatile *mem_ctrl_io_ec;
task *mem_ctrl_task;
task *mem_ctrl_io_task;
int mem_ctrl_pid = 0;
int mem_ctrl_io_pid = 0;

void memctrl_init(void){

	memctrl_create();
	memctrl_create_tasks();

	dram_init();

	return;
}

void memctrl_create(void){

	//one mem ctrl per CPU
	mem_ctrl = (void *) malloc(sizeof(struct mem_ctrl_t));

	return;
}

void dram_init(void){

	print_dramsim();
	dramsim_start();
	set_cpu_freq();

	fatal("exit\n");

	return;
}

void print_dramsim(void){

	call_print_me();

	return;

}

char dramsim_ddr_config_path[250] = "/home/stardica/Dropbox/CDA7919DoctoralResearch/Simulators/DRAMSim2/ini/DDR2_micron_16M_8b_x8_sg3E.ini";
char dramsim_system_config_path[250] = "/home/stardica/Dropbox/CDA7919DoctoralResearch/Simulators/DRAMSim2/system.ini";
char dramsim_vis_config_path[250] = "/home/stardica/Dropbox/CDA7919DoctoralResearch/Simulators/DRAMSim2/results";

void *dram_ptr;

void dramsim_start(void){

	void *temp = NULL;

	/*char *dev, char *sys, char *pwd,  char *trc, unsigned int megsOfMemory, char *visfilename*/
	temp = (void *) call_get_memory_system_instance(dramsim_ddr_config_path, dramsim_system_config_path, "/../", "exmaple_app", 4096, dramsim_vis_config_path);

	dram_ptr = temp;
	printf("C side temp_ptrint 0x%08x\n", temp);

	return;
}

void set_cpu_freq(void){

	call_set_CPU_clock_speed(dram_ptr, 40000);

	printf("C side freq set\n");

	return;
}

/*void set_CPU_clock_speed(void){

	set_the_clock(deam_ptr, 40000);

}*/

void memctrl_create_tasks(void){

	char buff[100];

	mem_ctrl_ec = (void *) calloc(1, sizeof(eventcount));

	memset(buff,'\0' , 100);
	snprintf(buff, 100, "mem_ctrl_ec");
	mem_ctrl_ec = new_eventcount(strdup(buff));

	mem_ctrl_io_ec = (void *) calloc(1, sizeof(eventcount));

	memset(buff,'\0' , 100);
	snprintf(buff, 100, "mem_ctrl_io_ec");
	mem_ctrl_io_ec = new_eventcount(strdup(buff));

	mem_ctrl_task = (void *) calloc(1, sizeof(task));

	memset(buff,'\0' , 100);
	snprintf(buff, 100, "mem_ctrl_task");
	mem_ctrl_task = create_task(memctrl_ctrl, DEFAULT_STACK_SIZE, strdup(buff));

	mem_ctrl_io_task = (void *) calloc(1, sizeof(task));

	memset(buff,'\0' , 100);
	snprintf(buff, 100, "mem_ctrl_io");
	mem_ctrl_io_task = create_task(memctrl_ctrl_io, DEFAULT_STACK_SIZE, strdup(buff));

	return;
}

int memctrl_can_access(void){

	//check if in queue is full
	if(QueueSize <= list_count(mem_ctrl->Rx_queue_top))
	{
		return 0;
	}

	//we can access the system agent
	return 1;
}

void memctrl_ctrl_io(void){

	int my_pid = mem_ctrl_io_pid;
	long long step = 1;

	struct cgm_packet_t *message_packet;
	/*long long access_id = 0;*/
	int transfer_time = 0;

	set_id((unsigned int)my_pid);

	while(1)
	{
		await(mem_ctrl_io_ec, step);
		step++;

		message_packet = list_dequeue(mem_ctrl->Tx_queue);
		assert(message_packet);

		/*access_id = message_packet->access_id;*/
		transfer_time = (message_packet->size/mem_ctrl->bus_width);

		if(transfer_time == 0)
		{
			transfer_time = 1;
		}

		P_PAUSE(transfer_time);

		/*while(transfer_time > 0)
		{
			P_PAUSE(1);
			transfer_time--;
			//printf("Access_is %llu cycle %llu transfer %d\n", access_id, P_TIME, transfer_time);
		}*/

		list_enqueue(mem_ctrl->system_agent_queue, message_packet);
		advance(system_agent_ec);
	}
	return;
}




//do some work.
void memctrl_ctrl(void){

	int my_pid = mem_ctrl_pid;
	struct cgm_packet_t *message_packet;
	long long step = 1;

	long long access_id = 0;
	enum cgm_access_kind_t access_type;
	unsigned int addr;

	//for accessing the memory image.
	unsigned char buffer[20];
	unsigned char *buffer_ptr;

	int i = 0;

	set_id((unsigned int)my_pid);

	while(1)
	{
		//printf("mem_ctrl\n");
		await(mem_ctrl_ec, step);

		if(!sys_agent_can_access_bottom())
		{
			printf("MC stalling up\n");
			P_PAUSE(1);
		}
		else
		{
			step++;

			//star todo connect up DRAMsim here.
			message_packet = list_dequeue(mem_ctrl->Rx_queue_top);
			assert(message_packet);

			//access_type = message_packet->access_type;
			access_id = message_packet->access_id;
			addr = message_packet->address;
			access_type = message_packet->access_type;

			CGM_DEBUG(memctrl_debug_file,"%s access_id %llu cycle %llu as %s addr 0x%08u\n",
					mem_ctrl->name, access_id, P_TIME, (char *)str_map_value(&cgm_mem_access_strn_map, access_type), addr);

			P_PAUSE(mem_ctrl->DRAM_latency);

			/*****NOTE!!*****/
			/*the memory image is entirely based on the ELF's provided virtual addresses
			to access the memory image from the memory controller you first have to do a quick
			swap back to the virtual address. This is just a simulator-ism. In the real world
			the real physical address would be used at this point to gather data.*/

			if(message_packet->access_type == cgm_access_mc_store)
			{
				/*the message is a store message (Write Back) from a L3 cache
				for now charge the latency for the store, then, just destroy the packet*/

				message_packet = list_remove(mem_ctrl->Rx_queue_top, message_packet);
				free(message_packet);
			}
			else if(message_packet->access_type == cgm_access_mc_load)
			{
				/*This is a L3 load request (cached memory system miss)
				charge the latency for the load, then, reply with data*/

				/*buffer_ptr = mem_get_buffer(mem_ctrl->mem, mmu_get_vtladdr(0, message_packet->address), 20, mem_access_read);

				if (!buffer_ptr)
				{
					 Disable safe mode. If a part of the 20 read bytes does not belong to the
					 actual instruction, and they lie on a page with no permissions, this would
					 generate an undesired protection fault.
					mem_ctrl->mem->safe = 0;
					buffer_ptr = buffer;
					mem_access(mem_ctrl->mem, mmu_get_vtladdr(0, message_packet->address), 20, buffer_ptr, mem_access_read);
				}

				mem_ctrl->mem->safe = mem_safe_mode;

				for(i = 0; i < 20; i++)
				{
					printf("buffer 0x%02x\n", *buffer_ptr);
					buffer_ptr++;
				}
				printf(" blah blah blah!!! address 0x%08x\n", mmu_get_vtladdr(0, message_packet->address)),
				getchar();*/

				if(message_packet->access_id == 1627680)
				{
					printf("message %llu MC service\n", message_packet->access_id);
				}


				//set the access type
				message_packet->access_type = cgm_access_mc_put;
				message_packet->size = l3_caches[0].block_size;

				//reply to L3
				list_enqueue(mem_ctrl->Tx_queue, message_packet);
				advance(mem_ctrl_io_ec);
			}
		}
	}
	return;
}
