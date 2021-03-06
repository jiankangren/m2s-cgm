
#include <stdio.h>
#include <signal.h>
#include <sys/time.h>
#include <cgm/contexts.h>
#include <cgm/cgm.h>
#include <cgm/rdtsc.h>


struct process_record {
  context_t c;
};

/* current process */
process_t *current_process = NULL;
static process_t *terminated_process = NULL;

long long num_contexts = 0;
long long max_contexts = 0;

#define ENCODE_JMPBUF(j) j

#ifdef FAIR

static struct itimerval mt;	/* the timer for the main thread */
static int unfair = 0;
static int enable_mask;
static int disable_mask;

static int mt_in_cs = 0;	/* main thread in cs */
static int mt_pending = 0;	/* pending thread 0 */
static int mt_interrupted = 0;  /* main thread interrupted? */


#define SLICE_SEC  0		/* time slice in */
#define SLICE_USEC 100		/* secs and usecs */


#define IN_CS(proc) (proc ? (proc)->c.in_cs : mt_in_cs)

#define PENDING(proc) (proc ? (proc)->c.pending : mt_pending)

#define INTERRUPTED(proc) (proc ? (proc)->c.interrupted : mt_interrupted)

#define SET_INTRPT(proc) do { if (proc) (proc)->c.interrupted = 1; else mt_interrupted = 1; } while(0)

#define CLR_INTRPT(proc) do { if (proc) (proc)->c.interrupted = 0; else mt_interrupted = 0; } while(0)

#define ENTER_CS(proc) do {                  	\
		         if (proc)           	\
		           (proc)->c.in_cs = 1;	\
		         else                	\
		           mt_in_cs = 1;     	\
		       } while (0)

#define LEAVE_CS(proc) do {                                         	\
		         if (proc) {                                	\
		           (proc)->c.in_cs = 0;                       	\
		           if ((proc)->c.pending) {                   	\
		             (proc)->c.pending = 0;                   	\
		             setitimer (ITIMER_VIRTUAL, &mt, NULL); 	\
		             context_timeout ();                    	\
		           }                                        	\
		         }                                          	\
		         else {                                     	\
		           mt_in_cs = 0;                            	\
		           if (mt_pending) {                        	\
		             mt_pending = 0;                        	\
		             setitimer (ITIMER_VIRTUAL, &mt, NULL); 	\
		             context_timeout ();                    	\
		           }                                        	\
		         }                                          	\
		       } while(0)

#define MAKE_PENDING(proc)  do {                     	\
			      if (proc)              	\
			        (proc)->c.pending = 1; 	\
			      else                   	\
			        mt_pending = 1;      	\
			    } while (0)

/*------------------------------------------------------------------------
 *
 *  Handle timer interrupt
 *
 *------------------------------------------------------------------------
 */
static void interrupt_handler (int sig)
{
#if defined(__hppa) && defined (__hpux) || \
    defined(__sparc__) && defined(__svr4__)
  /* running a sucky os */
  signal (SIGVTALRM, interrupt_handler);
#endif
  if (IN_CS (current_process))
    MAKE_PENDING(current_process);
  else if (INTERRUPTED (current_process)) {
    CLR_INTRPT (current_process);
    setitimer (ITIMER_VIRTUAL, &mt, NULL);
    context_timeout ();
  }
  else 
    SET_INTRPT (current_process);
}

/*------------------------------------------------------------------------
 *
 * Initialize timer data structure
 *
 *------------------------------------------------------------------------
 */
static void context_internal_init (void)
{
  static int first = 1;
  if (first && !unfair) {
    first = 0;
    mt.it_interval.tv_sec = 0;
    mt.it_interval.tv_usec = 0;
    mt.it_value.tv_sec = SLICE_SEC;
    mt.it_value.tv_usec = SLICE_USEC;
    signal (SIGVTALRM, interrupt_handler);
    setitimer (ITIMER_VIRTUAL, &mt, NULL);
  }
}
#endif


/*------------------------------------------------------------------------
 *  
 *  context_unfair --
 *
 *      Return back to unfair scheduling
 *
 *------------------------------------------------------------------------
 */
void context_unfair (void)
{
#ifdef FAIR
  struct itimerval t;
  unfair = 1;
  t.it_interval.tv_sec = 0;  t.it_interval.tv_usec =0;
  t.it_value.tv_sec = 0; t.it_value.tv_usec = 0;
  setitimer (ITIMER_VIRTUAL, &t, NULL);	/* disable timer interrupts */
#endif
}

/*------------------------------------------------------------------------
 *  
 *  context_fair --
 *
 *      Return back to unfair scheduling
 *
 *------------------------------------------------------------------------
 */
void context_fair (void)
{
#ifdef FAIR
  unfair = 0;
  mt.it_interval.tv_sec = 0;
  mt.it_interval.tv_usec = 0;
  mt.it_value.tv_sec = SLICE_SEC;
  mt.it_value.tv_usec = SLICE_USEC;
  signal (SIGVTALRM, interrupt_handler);
  setitimer (ITIMER_VIRTUAL, &mt, NULL);
#endif
}


/*------------------------------------------------------------------------
 *
 * Enable timer interrupts: must be called with interrupts disabled.
 *
 *------------------------------------------------------------------------
 */
#ifdef FAIR
void context_enable (void)
{
  LEAVE_CS (current_process);
}
#endif


/*------------------------------------------------------------------------
 *
 * Disable timer interrupts: must be called with interrupts enabled.
 *
 *------------------------------------------------------------------------
 */
#ifdef FAIR
void context_disable (void)
{
  ENTER_CS (current_process);
}
#endif

 
/*------------------------------------------------------------------------
 *
 * Called with interrupts disabled. Enables interrupts on termination.
 *
 *------------------------------------------------------------------------
 */

long long current_cycle = 0;
long long max_num_contexts = 0;

unsigned long long set_start = 0;
unsigned long long set_time = 0;

void context_switch (process_t *p)
{
	if (!current_process || !_setjmp (current_process->c.buf))
	{
		current_process = p;

		/*if(current_cycle == P_TIME && P_TIME > 0)
		{
			if(max_num_contexts > max_contexts)
				max_contexts = max_num_contexts;
		}
		else
		{
			if(num_contexts)
				printf("time A %llu events %llu\n", (rdtsc() - set_start)/num_contexts, num_contexts);
			else
				printf("time B %llu events %llu\n", rdtsc() - set_start, num_contexts);

			if(num_contexts)
				printf("%llu\n", (rdtsc() - set_start)/num_contexts);


			if(P_TIME == 100)
				getchar();

			set_start = rdtsc();

			current_cycle = P_TIME;
			max_num_contexts = 0;
			num_contexts = 0;
		}

		num_contexts++;
		max_num_contexts++;*/

		_longjmp (p->c.buf,1);
	}

	if (terminated_process)
	{
		context_destroy (terminated_process);
		terminated_process = NULL;
	}

  context_enable ();
}

/*------------------------------------------------------------------------
 *
 *  Called with interrupts disabled
 *  cleaup routine
 *
 *------------------------------------------------------------------------
 */
void context_cleanup (void)
{
  if (terminated_process) {
    context_destroy (terminated_process);
    terminated_process = NULL;
  }
}

/*------------------------------------------------------------------------
 *
 * Called with interrupts disabled;
 * Enables interrupts on exit.
 *
 *------------------------------------------------------------------------
 */
static void context_stub (void)
{
#ifdef FAIR
  context_internal_init ();
#endif
  if (terminated_process) {
    context_destroy (terminated_process);
    terminated_process = NULL;
  }
  context_enable ();
  (*current_process->c.start)();
  context_disable ();
  terminated_process = current_process;
  /*
   * It would be nice to delete the process here, but we can't do that
   * because we're deleting the stack we're executing on! Some other
   * process must free our stack.
   */
  context_switch (context_select ());
}


/*------------------------------------------------------------------------
 *
 *  Called on exit; must be called with interrupts disabled
 *
 *------------------------------------------------------------------------
 */
void context_exit (void)
{
  terminated_process = current_process;
  context_switch (context_select ());
}


/*
 * Crazy Ubuntu jmpbuf encoder/decoder functions
 */
#if defined(__linux__) && defined(__i386__)
int DecodeJMPBUF32(int j){
   int retVal;

   asm ("mov %1,%%edx;"
		"ror $0x9,%%edx;"
		"xor %%gs:0x18,%%edx;"
		"mov %%edx,%0;"
      :"=r" (retVal)  /* output */           
      :"r" (j)        /* input */        
      :"%edx"         /* clobbered register */ 
   );

   return retVal;
}

int EncodeJMPBUF32(int j) {
   int retVal;

   asm ("mov %1,%%edx;"
		"xor %%gs:0x18,%%edx;"
		"rol $0x9,%%edx;"
		"mov %%edx,%0;"
      :"=r" (retVal)  /* output */           
      :"r" (j)        /* input */        
      :"%edx"         /* clobbered register */ 
   );

   return retVal;
}

#elif defined(__linux__) && defined(__x86_64)
long long DecodeJMPBUF64(long long j){

  long long retVal;

  asm volatile ("mov %1, %%rax;"
		"ror $0x11, %%rax;"
		"xor %%fs:0x30, %%rax;"
		"mov %%rax, %0;"
	 :"=r" (retVal)   /*output*/
	 :"r" (j)         /*input*/
	 :"%rax"          /*clobbered register*/
  );

  return retVal;
}

long long EncodeJMPBUF64(long long j) {

long long retVal;

asm (	"mov %1, %%rax;"
		"xor %%fs:0x30, %%rax;"
		"rol $0x11, %%rax;"
		"mov %%rax, %0;"
		:"=r" (retVal)   /*output*/
		:"r" (j)         /*input*/
		:"%rax"          /*clobbered register*/
	);

return retVal;
}
#endif

/*
 * Non-portable code is in this function
 */
void context_init (process_t *p, void (*f)(void))
{
  void *stack;
  int n;
  /*int temp;*/

  p->c.start = f;
  stack = p->c.stack;
  n = p->c.sz;

  _setjmp (p->c.buf);

#ifdef FAIR
  p->c.in_cs = 0;
  p->c.pending = 0;
  p->c.interrupted = 0;
#endif

#if defined(__sparc__) && !defined(__svr4__)

#define INIT_SP(p) (int)((double*)(p)->c.stack + (p)->c.sz/sizeof(double)-11)
#define CURR_SP(p) (p)->c.buf[2]

  /* Need 12 more doubles to save register windows. */
  p->c.buf[3] = p->c.buf[4] = (int)context_stub;
  p->c.buf[2] = (int)((double*)stack + n/sizeof(double)-11);

#elif defined(__sparc__)

#define INIT_SP(p) (int)((double*)(p)->c.stack + (p)->c.sz/sizeof(double)-11)
#define CURR_SP(p) (p)->c.buf[1]

  /* Need 12 more doubles to save register windows. */
  p->c.buf[2] = (int)context_stub;
  p->c.buf[1] = (int)((double*)stack + n/sizeof(double)-11);

#elif defined(__NetBSD__) && defined(__i386__)

#define INIT_SP(p) (int)((char*)(p)->c.stack + (p)->c.sz-4)
#define CURR_SP(p) (p)->c.buf[2]

  p->c.buf[0] = (int)context_stub;
  p->c.buf[2] = (int)((char*)stack+n-4);

#elif defined(__NetBSD__) && defined(__m68k__)

#define INIT_SP(p) (int)((char*)(p)->c.stack + (p)->c.sz)
#define CURR_SP(p) (p)->c.buf[2]

  p->c.buf[5] = (int)context_stub;
  p->c.buf[2] = (int)((char*)stack+n-4);

#elif defined(__linux__) && defined(__i386__)

/* This works with Ubuntu 12.04 */
#define INIT_SP(p) (int)((char*)(p)->c.stack + (p)->c.sz)
#define CURR_SP(p) DecodeJMPBUF32((p)->c.buf[0].__jmpbuf[4])

/*newer versions of GCC enable stack protectors by default
which breaks the way context.c works via _FORTIFY_SOURCE =1 or = 2
To fix this undef and redefine _FORTIFY_SOURCE to get things working again.*/


  //debug
  //int  i = 0;
  //for (i=4; i < sizeof(p->c.buf[0].__jmpbuf)/4; i++) {
  //  printf("%d: Before Decode: %x\tAfter Decode: %x\n", i, p->c.buf[0].__jmpbuf[i], DecodeJMPBUF(p->c.buf[0].__jmpbuf[i]));
  //}

  p->c.buf[0].__jmpbuf[5] = EncodeJMPBUF32((int)context_stub);
  p->c.buf[0].__jmpbuf[4] = EncodeJMPBUF32((int)((char*)stack+n-4));

  /*AFAIK there is no way to distinguish between the lines
  below and above using ifdefs... argh!*/

	/* This works with Red Hat 7.1

#define INIT_SP(p) (int)((char*)(p)->c.stack + (p)->c.sz)
#define CURR_SP(p) (p)->c.buf[0].__jmpbuf[4]

  p->c.buf[0].__jmpbuf[5] = (int)context_stub;
  p->c.buf[0].__jmpbuf[4] = (int)((char*)stack+n-4);


   This used to work with Linux, version unknown... 

#define INIT_SP(p) (int)((char*)(p)->c.stack + (p)->c.sz)
#define CURR_SP(p) (p)->c.buf[0].__sp

  p->c.buf[0].__pc = (__ptr_t)context_stub;
  p->c.buf[0].__sp = (__ptr_t)((char*)stack+n-4);*/
#elif defined(__linux__) && defined(__x86_64)

//#define INIT_SP(p) EncodeJMPBUF64((long long)(((char *)stack) + n - 4));
//#define CURR_SP(p) DecodeJMPBUF64((p)->c.buf[0].__jmpbuf[4])

#define INIT_SP(p) 0
#define CURR_SP(p) 0

  p->c.buf[0].__jmpbuf[7] = EncodeJMPBUF64((long long)context_stub);
  p->c.buf[0].__jmpbuf[6] = EncodeJMPBUF64((long long)(((char *)stack) + n - 4));

#elif defined(__FreeBSD__) && defined(__i386__)

#define INIT_SP(p) (int)((char*)(p)->c.stack + (p)->c.sz)
#define CURR_SP(p) (p)->c.buf[0]._jb[2]

  p->c.buf[0]._jb[0] = (long)context_stub;
  p->c.buf[0]._jb[2] = (long)((char*)(stack+n-4));

#elif defined (__alpha)

#define INIT_SP(p) (long)((char*)(p)->c.stack + (p)->c.sz)
#define CURR_SP(p) (p)->c.buf[34]

  /* entry point needs adjustment */
  p->c.buf[30] = (long)context_stub+8;
  p->c.buf[34] = (long)stack+n-8;

#elif defined (mips) && defined(__sgi)

#define INIT_SP(p) (long)((char*)(p)->c.stack + (p)->c.sz)
#define CURR_SP(p) (p)->c.buf[JB_SP]

  p->c.buf[JB_PC] = (long)context_stub;
  p->c.buf[JB_SP] = (long)stack+n-8;

#elif defined(__hppa) && defined (__hpux)

#define INIT_SP(p) (long)((char*)(p)->c.stack)
#define CURR_SP(p) ((unsigned long*)&(p)->c.buf)[1]

  /* two stack frames, stack grows up */
  ((unsigned long*)&p->c.buf)[1] = (unsigned long)stack + 128; 

  /* function pointers point to a deref table */
  ((unsigned long*)&p->c.buf)[44] = 
    *((unsigned long*)((unsigned long)context_stub & ~3));

#elif defined(PC_OFFSET) && defined(SP_OFFSET)

#define Max(a,b) ((a) > (b) ? (a) : (b))

#define INIT_SP(p) (unsigned long)((unsigned long)(p)->c.stack + (p)->c.sz)
#define CURR_SP(p) ((unsigned long*)&p->c.buf)[SP_OFFSET]

  ((unsigned long*)&p->c.buf)[PC_OFFSET] = (unsigned long)context_stub;
#if !defined (STACK_DIR_UP)
  ((unsigned long*)&p->c.buf)[SP_OFFSET] = (unsigned long)stack+n-Max(sizeof(long),sizeof(double));
#else
  ((unsigned long*)&p->c.buf)[SP_OFFSET] = (unsigned long)stack+Max(sizeof(long),sizeof(double));
#endif

#else
#error Unknown machine/OS combination
#endif
}



/*------------------------------------------------------------------------
 *
 *  Save context to file
 *
 *------------------------------------------------------------------------
 */
void context_write (FILE *fp, process_t *p)
{
  int i;
  //int n;
  unsigned long *l;
  unsigned char *curr_sp, *init_sp;

  l = (unsigned long *)&p->c.buf;
  for (i=0; i < sizeof(p->c.buf)/sizeof(unsigned long); i++) {
    fprintf (fp, "%lu\n", *l);
    l++;
  }
  /*save stack*/
  init_sp = (unsigned char *)INIT_SP(p);
  curr_sp = (unsigned char *)CURR_SP(p);

  if (init_sp > curr_sp) {
    curr_sp = init_sp;
    init_sp = (unsigned char *)CURR_SP(p);
  }

  /* * This is broken. if init_sp == curr_sp, do we save or not???
   * (machine-dependent)
   **/

  while (init_sp < curr_sp) {
    i = *init_sp;
    fprintf (fp, "%d\n", i);
    init_sp++;
  }
#ifdef FAIR
  fprintf (fp, "%d\n%d\n%d\n", p->c.in_cs, p->c.pending, p->c.interrupted);
#endif
}


/*------------------------------------------------------------------------
 *
 *  Read Context
 *
 *------------------------------------------------------------------------
 */
void context_read (FILE *fp, process_t *p)
{
  int i;
  //int n;
  unsigned long *l;
  unsigned char *init_sp, *curr_sp;

  l = (unsigned long *)&p->c.buf;
  for (i=0; i < sizeof(p->c.buf)/sizeof(unsigned long); i++) {
    fscanf (fp, "%lu", l++);
  }
  init_sp = (unsigned char *)INIT_SP(p);
  curr_sp = (unsigned char *)CURR_SP(p);

  if (init_sp > curr_sp) {
    curr_sp = init_sp;
    init_sp = (unsigned char *)CURR_SP(p);
  }
  /* this is broken too...*/
  while (init_sp < curr_sp) {
    fscanf (fp, "%d", &i);
    *init_sp = i;
    init_sp++;
  }
#ifdef FAIR
  fscanf (fp, "%d%d%d\n", &p->c.in_cs, &p->c.pending, &p->c.interrupted);
#endif
}
