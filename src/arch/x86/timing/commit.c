/*
 *  Multi2Sim
 *  Copyright (C) 2012  Rafael Ubal (ubal@ece.neu.edu)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <unistd.h>

#include <arch/x86/emu/context.h>
#include <lib/esim/esim.h>
#include <lib/esim/trace.h>
#include <lib/util/debug.h>

#include <arch/x86/timing/bpred.h>
#include <arch/x86/timing/commit.h>
#include <arch/x86/timing/core.h>
#include <arch/x86/timing/cpu.h>
#include <arch/x86/timing/fu.h>
#include <arch/x86/timing/recover.h>
#include <arch/x86/timing/reg-file.h>
#include <arch/x86/timing/rob.h>
#include <arch/x86/timing/sched.h>
#include <arch/x86/timing/thread.h>
#include <arch/x86/timing/trace-cache.h>
//#include <instrumentation/stats.h>

#include <cgm/cgm.h>

static char *err_commit_stall =
	"---The CPU commit stage has not received any instruction for 1M---\n"
	"---cycles. Most likely, this means that a deadlock condition---\n"
	"---occurred in the management of some modeled structure (network,---\n"
	"---cache system, core queues, etc.).---\n";

/*
 * Class 'X86Thread'
 */

int X86ThreadCanCommit(X86Thread *self)
{
	X86Cpu *cpu = self->cpu;
	X86Context *ctx = self->ctx;
	struct x86_uop_t *uop = NULL;
	//struct x86_uop_t *rob_head_uop = NULL;
	//struct cgm_packet_t *new_message = NULL;

	char buff[250];

	/* Sanity check - If the context is running, we assume that something is
	 * going wrong if more than 1M cycles go by without committing an inst. */
	if (!ctx || !X86ContextGetState(ctx, X86ContextRunning))
		self->last_commit_cycle = asTiming(cpu)->cycle;

	if (asTiming(cpu)->cycle - self->last_commit_cycle > 1000000)
	{
		/*Print the error to the screen first*/
		printf("\n---thread %s: simulation ended due to commit stall.---\n%s", self->name, err_commit_stall);

		printf("---Deadlock Detected Dumping System Status---\n"
			"---Current cycle %llu---\n"
			"---last real cycle (current minus 1M) %llu---\n"
			"---Last committed memory lsq access %llu last committed lsq blk address 0x%08x---\n"
			"---Last issued lsq memory access %llu last issued lsq blk address 0x%08x---\n"
			"---Last committed memory fetch access %llu last committed fetch blk address 0x%08x---\n"
			"---Last issued fetch memory access %llu last issued fetch blk address 0x%08x---\n",
			P_TIME,
			(P_TIME - 1000000),
			last_committed_lsq_access_id, last_committed_lsq_access_blk, last_issued_lsq_access_id, last_issued_lsq_access_blk,
			last_committed_fetch_access_id, last_committed_fetch_access_blk, last_issued_fetch_access_id, last_issued_fetch_access_blk);

		cgm_dump_system();
		cgm_dump_summary();
		printf("---Simulation End---\n\n");
		fflush(stdout);
		fflush(stderr);

		memset (buff,'\0' , 250);
		sprintf(buff, "%s", cgm_stats_output_path);
		sprintf(buff + strlen(buff), "m2s_cgm_stats_error_log_%s.txt", cgm_stat->date_time_file);
		FILE *cgm_stats_error_file;
		cgm_stats_error_file = fopen (buff, "w+");

		/*Now print the error to an error log file*/
		dup2(fileno(cgm_stats_error_file), fileno (stdout));
		printf("\n---thread %s: simulation ended due to commit stall.---\n%s", self->name, err_commit_stall);

		printf("---Deadlock Detected Dumping System Status---\n"
			"---Current cycle %llu---\n"
			"---last real cycle (current minus 1M) %llu---\n"
			"---Last committed memory lsq access %llu last committed lsq blk address 0x%08x---\n"
			"---Last issued lsq memory access %llu last issued lsq blk address 0x%08x---\n"
			"---Last committed memory fetch access %llu last committed fetch blk address 0x%08x---\n"
			"---Last issued fetch memory access %llu last issued fetch blk address 0x%08x---\n",
			P_TIME,
			(P_TIME - 1000000),
			last_committed_lsq_access_id, last_committed_lsq_access_blk, last_issued_lsq_access_id, last_issued_lsq_access_blk,
			last_committed_fetch_access_id, last_committed_fetch_access_blk, last_issued_fetch_access_id, last_issued_fetch_access_blk);

		cgm_dump_system();
		fflush(stdout);
		fflush(stderr);
		fclose(cgm_stats_error_file);

		esim_finish = esim_finish_stall;

		exit(0);
	}

	/* If there is no instruction in the ROB, or the instruction is not
	 * located at the ROB head in shared approaches, end. */
	if (!X86ThreadCanDequeueFromROB(self))
		return 0;

	/* Get instruction from ROB head */
	uop = X86ThreadGetROBHead(self);
	assert(x86_uop_exists(uop));
	assert(uop->thread == self);

	/* Stores must be ready. Update here 'uop->ready' flag for efficiency,
	 * if the call to 'X86ThreadIsUopReady' shows input registers to be ready. */
	/*if(uop->uinst->opcode == x86_uinst_store || uop->uinst->opcode == x86_uinst_store_ex)
	{

		assert(uop->ready == 1);
		//assert(uop->completed == 1);

		//old M2S code
		if (!uop->ready && X86ThreadIsUopReady(self, uop))
			uop->ready = 1;

		return uop->ready;
	}*/
	/*if(uop->uinst->opcode == x86_uinst_cpu_flush || uop->uinst->opcode == x86_uinst_gpu_flush)
	{
		clear these out of commit these will not be put in the event queue for write back to process
		assert(uop->ready == 1);

		fatal("commit: working flush opcode %d cycle %llu\n", uop->uinst->opcode, P_TIME);
		uop->completed = 1;
		uop->ready = 1;
		return uop->ready;
	}*/
	/*else if (uop->uinst->opcode == x86_uinst_cpu_fence || uop->uinst->opcode == x86_uinst_cpu_load_fence)
	{
		if(!uop->ready && uop->thread->d_cache_ptr[uop->thread->core->id].flush_counter == 0)
			uop->ready = 1;

		return uop->ready;
	}*/
	/*else
	{
		fatal("X86ThreadCanCommit(): bad access type %d\n", uop->uinst->opcode);
	}*/
	
	/* Instructions other than stores must be completed. */
	return uop->completed;
}


void X86ThreadCommit(X86Thread *self, int quant)
{
	X86Cpu *cpu = self->cpu;
	X86Core *core = self->core;
	X86Context *ctx = self->ctx;

	struct x86_uop_t *uop;
	int recover = 0;

	/* Commit stage for thread */
	assert(ctx);
	while (quant && X86ThreadCanCommit(self))
	{
		/* Get instruction at the head of the ROB */
		uop = X86ThreadGetROBHead(self);
		assert(x86_uop_exists(uop));
		assert(uop->thread == self);
		assert(!recover);
		
		/* Miss predicted branch */
		if (x86_cpu_recover_kind == x86_cpu_recover_kind_commit && (uop->flags & X86_UINST_CTRL) && uop->neip != uop->pred_neip)
			recover = 1;
	
		/* Free physical registers */
		assert(!uop->specmode);

		X86ThreadCommitUop(self, uop);

		/* Branches update branch predictor and btb */
		if (uop->flags & X86_UINST_CTRL)
		{
			X86ThreadUpdateBranchPred(self, uop);
			X86ThreadUpdateBTB(self, uop);
			self->btb_writes++;
		}



		//clear flags used for stats collection...
		if (uop->uinst->opcode == x86_uinst_cpu_fence || uop->uinst->opcode == x86_uinst_cpu_load_fence)
		{
			cpu_gpu_stats->core_num_fences[self->core->id]--;
		}



		/* Trace cache */
		if (x86_trace_cache_present)
			X86ThreadRecordUopInTraceCache(self, uop);
			
		/*star our stats*/
		cpu_gpu_stats->core_last_commit_cycle[self->core->id] = P_TIME;
		if (uop->flags == X86_UINST_MEM)
			cpu_gpu_stats->core_commited_memory_insts[self->core->id]++;


		if(uop->uinst->opcode == x86_uinst_syscall)
			warning("Commit: syscall id %llu cycle %llu\n", uop->id, P_TIME);

		if(uop->uinst->opcode == x86_uinst_cpu_fence)
			warning("commit: committing fence id %llu cycle %llu\n", uop->id, P_TIME);



		/* Statistics */
		self->last_commit_cycle = asTiming(cpu)->cycle;
		self->num_committed_uinst_array[uop->uinst->opcode]++;
		core->num_committed_uinst_array[uop->uinst->opcode]++;
		cpu->num_committed_uinst_array[uop->uinst->opcode]++;
		cpu->num_committed_uinst++;
		ctx->inst_count++;
		if (uop->trace_cache)
			self->trace_cache->num_committed_uinst++;
		if (!uop->mop_index)
			cpu->num_committed_inst++;
		if (uop->flags & X86_UINST_CTRL)
		{
			self->num_branch_uinst++;
			core->num_branch_uinst++;
			cpu->num_branch_uinst++;
			if (uop->neip != uop->pred_neip)
			{
				self->num_mispred_branch_uinst++;
				core->num_mispred_branch_uinst++;
				cpu->num_mispred_branch_uinst++;
			}
		}

		/* Trace */
		if (x86_tracing())
		{
			//testing if we enter here.
			fatal("x86_tracing() Warning getchar here\n");

			x86_trace("x86.inst id=%lld core=%d stg=\"co\"\n", uop->id_in_core, core->id);
			X86CpuAddToTraceList(cpu, uop);
		}

		/* Retire instruction */
		X86ThreadRemoveROBHead(self);
		core->rob_reads++;
		self->rob_reads++;
		quant--;

		/* Recover. Functional units are cleared when processor
		 * recovers at commit. */
		if (recover)
		{
			//fatal("X86ThreadCommit(): protection fault committing access_id %llu\n", uop->id);

			X86ThreadRecover(self);
			X86CoreReleaseAllFunctionalUnits(core);
		}
	}

	/* If context eviction signal is activated and pipeline is empty,
	 * deallocate context. */
	if (ctx->evict_signal && X86ThreadIsPipelineEmpty(self))
		X86ThreadEvictContext(self, ctx);
}




/*
 * Class 'X86Core'
 */

void X86CoreCommit(X86Core *self)
{
	int pass;
	int quantum;

	/* Commit stage for core */
	switch (x86_cpu_commit_kind)
	{

	//star >> commented out for clarity
	case x86_cpu_commit_kind_shared:

		pass = x86_cpu_num_threads;
		quantum = x86_cpu_commit_width;
		while (quantum && pass)
		{
			self->commit_current = (self->commit_current + 1) % x86_cpu_num_threads;
			if (X86ThreadCanCommit(self->threads[self->commit_current]))
			{
				X86ThreadCommit(self->threads[self->commit_current], 1);
				quantum--;
				pass = x86_cpu_num_threads;
			}
			else
				pass--;
		}
		break;
	
	//star >> commented out for clarity
	//not sure why the default is shared vs. timeslice when the rest of the CPU is defaults to timeslice.
	case x86_cpu_commit_kind_timeslice:
	
		/*//look for a not empty VB
		int new;
		new = (self->commit_current + 1) % x86_cpu_num_threads;
		while (new != self->commit_current && !X86ThreadCanCommit(self->threads[new]))
			new = (new + 1) % x86_cpu_num_threads;
		
		 //Commit new thread
		self->commit_current = new;
		X86ThreadCommit(self->threads[new], x86_cpu_commit_width);*/
		fatal("X86CoreCommit(): \"x86_cpu_commit_kind_timeslice\" add this back in\n");
		break;
	
	}
}




/*
 * Class 'X86Cpu'
 */

void X86CpuCommit(X86Cpu *self)
{
	int i;

	self->stage = "commit";
	for (i = 0; i < x86_cpu_num_cores; i++)
		X86CoreCommit(self->cores[i]);
}
