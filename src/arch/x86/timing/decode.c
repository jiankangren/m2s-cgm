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

#include <m2s.h>
#include <lib/esim/trace.h>
#include <lib/util/list.h>
/*#include <mem-system/module.h>*/
//#include <instrumentation/stats.h>
#include <arch/x86/timing/core.h>
#include <arch/x86/timing/cpu.h>
#include <arch/x86/timing/decode.h>
#include <arch/x86/timing/fetch-queue.h>
#include <arch/x86/timing/thread.h>
#include <arch/x86/timing/uop.h>
#include <arch/x86/timing/uop-queue.h>

#include <cgm/cgm.h>



/*
 * Class 'X86Thread'
 */

static void X86ThreadDecode(X86Thread *self)
{
	X86Core *core = self->core;

	struct list_t *fetchq = self->fetch_queue;
	struct list_t *uopq = self->uop_queue;
	struct x86_uop_t *uop;
	int i;

	for (i = 0; i < x86_cpu_decode_width; i++)
	{
		/* Empty fetch queue, full uop_queue */
		if (!list_count(fetchq))
			break;
		if (list_count(uopq) >= x86_uop_queue_size)
			break;

		//star >> get a uop from the top of the queue
		uop = list_get(fetchq, 0);
		assert(x86_uop_exists(uop));

		/* If instructions come from the trace cache, i.e., are located in
		 * the trace cache queue, copy all of them
		 * into the uop queue in one single decode slot. */
		if (uop->trace_cache)
		{
			do
			{
				X86ThreadRemoveFromFetchQueue(self, 0);
				list_add(uopq, uop);
				uop->in_uop_queue = 1;
				uop = list_get(fetchq, 0);
			} while (uop && uop->trace_cache);
			break;
		}

		/* Decode one macro-instruction coming from a block in the instruction
		 * cache. If the cache access finished, extract it from the fetch queue. */
		assert(!uop->mop_index);

#if CGM
		//star todo pass the module here fix this
		//mod_in_flight_access(self->inst_mod, uop->fetch_access, uop->fetch_address)
		//needs the log_block_size and the access hash table.
		if (!cgm_in_flight_access(uop->fetch_access))
		{

#else
		if (!mod_in_flight_access(self->inst_mod, uop->fetch_access, uop->fetch_address))
		{
#endif

			do
			{
				/* Move from fetch queue to uop queue */

				X86ThreadRemoveFromFetchQueue(self, 0);
				list_add(uopq, uop);
				uop->in_uop_queue = 1;

				/* Trace */
				x86_trace("x86.inst id=%lld core=%d stg=\"dec\"\n", uop->id_in_core, core->id);

				/* Next */
				uop = list_get(fetchq, 0);

			} while (uop && uop->mop_index);
		}
	}
}




/*
 * Class 'X86Cpu'
 */

void X86CpuDecode(X86Cpu *self)
{
	int i;
	int j;

	self->stage = "decode";
	for (i = 0; i < x86_cpu_num_cores; i++)
		for (j = 0; j < x86_cpu_num_threads; j++)
			X86ThreadDecode(self->cores[i]->threads[j]);
}
