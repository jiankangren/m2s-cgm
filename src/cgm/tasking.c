
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <cgm/contexts.h>
#include <cgm/tasking.h>

typedef char boolean;

#define STK_OVFL_MAGIC 0x12349876 /* stack overflow check */

static char *end_of_concurrency = "--this-should-never-happen--";

static void (*cleanup_stuff) (void) = NULL;

static task inittask = { NULL, NULL, 0, NULL, NULL, NULL, -1, STK_OVFL_MAGIC, };

task *curtask = &inittask;

static task *hint = (task *) NULL;

task *wakeup_task;

//extern FILE *logF;

eventcount etime = {
		NULL,	/* tasklist */
		NULL,	/* name */
		0,		/* count */
		NULL	/* eclist */
};

static eventcount *ectail = NULL;

/* epause(N) -- wait N cycles.  Equivalent to await(etime, etime.c+N) */
void epause (count_t count){

	/* if no tasks or still first task, keep running */
	if ((etime.tasklist == NULL) || ((etime.count + count) <= etime.tasklist->count))
	{
		etime.count += count;
	}
	else /* switch to next task */
	{
		await(&etime, etime.count + count);
	}
	return;
}

/* save current task on ec, return next task to run */
static inline void find_next_task (eventcount *ec, count_t value){

	task *pptr = ec->tasklist;

	/* save current task on ec's tasklist */
	if ((pptr == NULL) || (value < pptr->count))
	{
		/* insert at head of list */
		if (curtask)
		{
			ec->tasklist = curtask;
			curtask->tasklist = pptr;
		}
	}
	else
	{
		/* insert in middle of list */
		task *ptr;
		boolean time_list = (ec == &etime);
		if (time_list && hint && (value >= hint->count))
		{
			ptr = hint->tasklist;
			pptr = hint;
		}
		else
		{
			ptr = pptr->tasklist;
		}

		while (ptr && (value >= ptr->count))
		{
			pptr = ptr;
			ptr = ptr->tasklist;
		}

		if (curtask)
		{
			pptr->tasklist = curtask;
			curtask->tasklist = ptr;
		}

		if (time_list)
		{
			hint = curtask;
		}
	}

	curtask->count = value;
 
	/* get next task to run */
	curtask = etime.tasklist;
	if (hint == curtask)
	{
		hint = NULL;
	}

	if (curtask == NULL)
	{
		context_cleanup ();

		if (cleanup_stuff)
			(*cleanup_stuff) ();

		exit (0);
	}

	assert (curtask->count >= etime.count);
  	etime.tasklist = curtask->tasklist;
  	etime.count = curtask->count;

  	return;
}

eventcount *last_ec = NULL; /* to work with context library */

count_t last_value = 0;	   /* more stuff like that */

process_t *context_select (void){

	#if 0
	assert ( !(!!(curtask->name == end_of_concurrency))^(!!(current_process == NULL)) );
	#endif

	if (last_ec)
	{
		find_next_task (last_ec, last_value);
		last_ec = NULL;
	}
	else
	{
		curtask = etime.tasklist;

		if (hint == curtask)
		{
			hint = NULL;
		}

		if (curtask == NULL)
		{
			//context_cleanup ();
			if (cleanup_stuff)
				(*cleanup_stuff) ();
			exit (0);
		}

		etime.tasklist = curtask->tasklist;
		etime.count = curtask->count;
	}

	return (process_t*)&(curtask->c);
}

void simulate (void (*f)(void)){

	cleanup_stuff = f;
	inittask.name = end_of_concurrency;
	process_t * process;

	process = context_select();
	/*assert(!process == NULL);*/
	assert(process);


	context_switch (process);
	context_cleanup ();

	if (cleanup_stuff)
	{
		(*cleanup_stuff) ();
		exit (0);
	}

	return;
}

void context_timeout (void){

	/* should never be called */
	printf ("CONTEXT_TIMEOUT! This should not happen\n");
	context_switch (context_select ());

	return;
}

void switch_context (eventcount *ec, count_t value){

	process_t *p;
	last_ec = ec;
	last_value = value;


	//debug
	//printf("in switch_context\n");
	//fflush(stdout);

	//fprintf (logF, ">>>>>>>> out: %s\n", get_tname());

	p = context_select ();

	//debug
	//printf("after context_select\n");
	//fflush(stdout);

	if (p) context_switch (p);
	//fprintf (logF, ">>>>>>>> in: %s\n", get_tname());


	return;
}

/* await(ec, value) -- suspend until ec.c >= value. */
void await (eventcount *ec, count_t value){


	//int bogus; /* just for grabbing something close to current sp... */

	// if stack overflowed before call to await, it will most likely have
	// thrashed magic.
	/*assert(curtask->magic == STK_OVFL_MAGIC);*/

	//fprintf(logF, "%s in await\n", curtask->name);

	if (ec->count >= value)
		return;


	//fprintf(logF, "%s awaiting until count is %llu\n", curtask->name, value);

	//Check for stack overflow

	/*assert(((unsigned)curtask + sizeof(task) < (unsigned) &bogus));*/

	switch_context(ec, value);


	return;
}

/* ticket(sequencer) -- return next ticket. */
count_t ticket (ticket_t *tkt){

	return (*tkt)++;
}

/* advance(ec) -- increment an event count. */
/* Don't use on time event count! */
void advance (eventcount *ec){

	task *ptr, *pptr;

	/* advance count */
	ec->count++;

	/* check for no tasks being enabled */
	ptr = ec->tasklist;

	if ((ptr == NULL) || (ptr->count > ec->count))
		return;

	/* advance down task list */
	do
	{
		/* set tasks time equal to current time */
		ptr->count = etime.count;
		pptr = ptr;
		ptr = ptr->tasklist;
	} while (ptr && (ptr->count == ec->count));

	/* add list of events to etime */
	pptr->tasklist = etime.tasklist;
	etime.tasklist = ec->tasklist;
	ec->tasklist = ptr;
  
	return;
}

void initialize_event_count (eventcount * ec, count_t count, char *ecname){

	ec->tasklist = NULL;
	ec->count    = count;
	ec->name     = ecname;
	ec->eclist   = NULL;


    if (ectail == NULL)
    {
       etime.eclist = ec;
    } 
    else
    {
       ectail->eclist = ec;
    }

    ectail = ec;

    //debug
   	//printf("event count created, name = %s, count = %d\n", ec->name, ec->count);


    return;
}

eventcount *new_eventcount (char *name){

	eventcount *ec;

	ec = (eventcount *)malloc (sizeof(eventcount));
	if (!ec)
	{
		fprintf (stderr, "Malloc failed, in new_eventcount()\n");
		exit (1);
	}

	initialize_event_count (ec, 0, name);

	return ec;
}

void delete_event_count (eventcount *ec){

	eventcount	*front;
	eventcount	*back;
	unsigned	found = 0;

	assert(ec != NULL);

	// --- This function should unlink the eventcount from the etime eclist ---
	assert(etime.eclist != NULL);

	front = etime.eclist;
	if (ec == front)
	{
		etime.eclist = ec->eclist;
		free ((void*)ec);
		if (ectail == ec)
		{
			ectail = NULL;
		}
	}
	else
	{
	   back = etime.eclist;
	   front = front->eclist;
	   while ((front != NULL) && (!found))
	   {
		   if (ec == front)
		   {
			   back->eclist = ec->eclist;
			   free ((void*)ec);
			   found = 1;
		   }
		   else
		   {
			   back = back->eclist;
			   front = front->eclist;
		   }
	   }

	   assert(found == 1);
	   if (ectail == ec)
	   {
		   ectail = back;
	   }
	}
}

/* frees up space with current task, and then selects next
   task to run and sets curtask to point to it */
void finish_task (void){

	free ((void*)curtask);

	/* get next task to run */
	last_ec = NULL;

	return;
}

void context_destroy (process_t *p){

	free (((context_t*)p)->stack);
  /* free stack space */

}

/* create_task(task, stacksize) -- create a task with specified stack size. */
/* task is code pointer and closure pointer */
task * create_task(void (*func)(void), unsigned stacksize, char *name){

	char*     ptr;
	task*     tptr;
	unsigned  size = sizeof(task);

	/* stacksize should be multiple of unsigned size */
	assert ((stacksize % sizeof(unsigned)) == 0);

	ptr = (char *) malloc(size);
	assert(ptr != NULL);

	tptr = (task *) ptr;
	tptr->count = etime.count;
	tptr->name = name;

	tptr->c.stack = (char *)malloc(stacksize);
	assert(tptr->c.stack);
	tptr->c.sz = stacksize;

	context_init ((process_t*)&tptr->c, func);

	/* link into tasklist */
	tptr->tasklist = etime.tasklist;
	etime.tasklist = tptr;

	// for stack overflow check
	tptr->magic = STK_OVFL_MAGIC;

	//debug
	//printf("In create_task1, name = %s, func address %#010x\n", tptr->name, &tptr->f);

	return tptr;
}


static void stub_function (void)
{
  assert (&curtask->c == (context_t *)current_process);

  (*curtask->f)(curtask->arg2);
}


task * create_task2(void (*func)(void *), void *arg, unsigned stacksize, char *name){

	char*     ptr;
	task*     tptr;
	unsigned  size = sizeof(task);

	/* stacksize should be multiple of unsigned size */
	assert ((stacksize % sizeof(unsigned)) == 0);

	ptr = (char *) malloc(size);
	assert(ptr != NULL);

	tptr = (task *) ptr;
	tptr->count = etime.count;
	tptr->name = name;
	tptr->arg2 = arg;
	tptr->f = func;
	tptr->c.stack = (char *)malloc(stacksize);
   	tptr->c.sz = stacksize;

	context_init ((process_t*)&tptr->c, stub_function); /* this is wrong */

	/* link into tasklist */
	tptr->tasklist = etime.tasklist;
	etime.tasklist = tptr;

	// for stack overflow check
	tptr->magic = STK_OVFL_MAGIC;
  
	return tptr;
}

// delete last inserted task
void remove_last_task (task *t){

	assert (etime.tasklist == t);
	etime.tasklist = t->tasklist;
	t->tasklist = NULL;

	return;
}


void initialize_this_task(void){

	curtask = &inittask;

	return;
}

void set_id(unsigned id) {

	curtask->id = (int)id;

	return;
}


unsigned get_id(void){

	// FlashLite-specific thingy here
	if (curtask->id == -1)
	{
		return 0;
	}

	return (unsigned)(curtask->id);
}


char *get_task_name (void){

	return curtask->name ? curtask->name : (char*)"-unknown-";

}


count_t get_time (void){

	return etime.count;

}

/*void task_write (FILE *fp, task *t, int ctxt){

	if (ctxt)
		context_write(fp, (process_t*)&t->c);

	count_write (fp, t->count);

	return;
}

void task_read (FILE *fp, task *t, int ctxt){

	if (ctxt)
		context_read(fp, (process_t*)&t->c);

	count_read (fp, &t->count);

	return;
}*/

void count_write (FILE *fp, count_t c){

	unsigned long x;

	x = (unsigned long)c;
  
	fprintf (fp, "%lu\n", x);

	return;
}

void count_read (FILE *fp, count_t *c){

	unsigned long x;

	fscanf(fp, "%lu", &x);

	*c = x;

	return;
}
