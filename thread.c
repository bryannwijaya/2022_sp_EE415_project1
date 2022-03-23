#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#include "threads/fixed_point.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of processes in THREAD_BLOCKED state, that is, processes
   that are waiting for an event to trigger. */
static struct list sleep_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;
bool thread_report_latency;

/* Value for advanced scheduler. */
fp_t load_average;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  list_init (&sleep_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Starts the load_average for advanced scheduling. */
  load_average = INITIAL_LOAD_AVERAGE;

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Compares the priority of two threads t1 and t2, where 
   t1->elem is elem1 and t2->elem is elem2. Returns a bool 
   TRUE when the priority of t1 is bigger than that of t2.
   This function is used to insert the threads to the
   ready_list in an ordered manner (i.e., descending priority). */
bool
thread_priority (const struct list_elem *elem1, 
                 const struct list_elem *elem2, void *aux UNUSED)
{  
  struct thread *t1 = list_entry (elem1, struct thread, elem);
  struct thread *t2 = list_entry (elem2, struct thread, elem);
  
  return t1->priority > t2->priority;
}

/* Checks if preemption needs to be done. If the priority of the 
   current thread is lower than the maximum priority of the 
   thread in the list LIST (i.e., the frontmost element), then 
   executes actual preemption. */
void
thread_preemption (void)
{
  if (!list_empty (&ready_list))
  {
    struct list_elem *e = list_front (&ready_list);
    struct thread *t = list_entry (e, struct thread, elem);
    struct thread *cur = thread_current ();

    if (t->priority > cur->priority)
      thread_yield ();
  }
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   This function now supports priority scheduling by inserting the
   thread to the ready_list in the decreasing order of priority 
   and doing preemption when necessary. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);

  /* Compare the priorities of the currently running thread and the 
     newly inserted one. Yield the CPU if the newly arriving thread
     has a higher priority (i.e., preemption). */
  thread_preemption ();
  
  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. 
   
   This function now supports priority scheduling by inserting 
   the thread to ready_list in the decreasing order of priority. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  
  /* Original code */
  //list_push_back (&ready_list, &t->elem);
  
  list_insert_ordered (&ready_list, &t->elem, thread_priority, NULL);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim.
   
   This function now supports priority scheduling by inserting
   the thread to ready_list in the order of decreasing priority. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    /* Original code */
    //list_push_back (&ready_list, &cur->elem);
    list_insert_ordered (&ready_list, &cur->elem, thread_priority, NULL);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Compares the wakeup_tick of two threads t1 and t2, where 
   t1->elem is elem1 and t2->elem is elem2. Returns a bool 
   TRUE when the wakeup_tick of t1 is smaller than that of t2.

   This function is used to insert the sleeping threads to the
   sleep_list in an ordered manner (i.e., ascending wakeup_tick). */
static bool
thread_compare (const struct list_elem *elem1, 
const struct list_elem *elem2, void *aux UNUSED)
{  
  struct thread *t1 = list_entry (elem1, struct thread, elem);
  struct thread *t2 = list_entry (elem2, struct thread, elem);
  
  return t1->wakeup_tick < t2->wakeup_tick;
}

/* If the caller thread is not idle_thread, stores the local wakeup_tick 
   to the caller thread's wakeup_tick attribute, moves the thread to 
   sleep_list in the order of increasing wakeup_tick, changes the state 
   of the caller thread to THREAD_BLOCKED, and call schedule(). 

   Interrupt is disabled while manipulating the lists.
   Global tick is not implmeneted, so not updated. */
void
thread_sleep (int64_t wakeup_tick) 
{  
  struct thread *cur = thread_current ();
  if (cur != idle_thread)
  {
    cur->wakeup_tick = wakeup_tick;

    enum intr_level old_level;
    ASSERT (!intr_context());
    old_level = intr_disable ();

    list_insert_ordered (&sleep_list, &cur->elem, thread_compare, NULL);
    thread_block ();

    intr_set_level (old_level);
  }
}

/* Called by timer_interrupt() to check the threads in the ordered 
   sleep_list and see if there is any threads to wake up. 
   When waking the thread(s) up, it is removed from the sleep_list, 
   added to the ready_list, and its status is modified to THREAD_READY. 

   Global tick is not implemented, so not updated. */
void
thread_wakeup (void)
{
  struct thread *t;
  while (!list_empty (&sleep_list)) 
  {  
    struct list_elem *e = list_front (&sleep_list);
    t = list_entry (e, struct thread, elem);
    
    /* Added to optimize the time needed for loop iteration.
       Valid since sleep_list is ordered based on wakeup_tick. */
    int64_t cur_tick = timer_ticks ();
    if (cur_tick < t->wakeup_tick) 
      break;
    
    list_pop_front (&sleep_list);
    thread_unblock (t);
  }
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Changes the priority and initial_priority of the current thread to
   new_priority. For supporting the priority donation, it then checks 
   if there is any d_elem dangling to its donation list that wait for 
   the lock(s) it holds. If that is the case, sets the priority value 
   to MAX(new_priority, highest_priority_of_d_elem) while 
   initial_priority stays to be new_priority. */
static void
priority_modif (int new_priority)
{
  struct thread *cur = thread_current ();

  cur->priority = cur->initial_priority = new_priority;
  if (!list_empty (&cur->donations)) 
  {
    //list_sort (&cur->donations, donate_priority, 0);
    struct thread *front = list_entry (list_front (&cur->donations), 
                                       struct thread, d_elem);
    if (front->priority > cur->priority)
      cur->priority = front->priority;
  }
}

/* Sets the current thread's priority to NEW_PRIORITY.
   
   This function now supports priority scheduling by checking 
   for preemption and reordering the ready_list via 
   thread_yield () int thread_preemption () if necessary.
   It also supports priority donation by priority_modif (). 
   
   This function also now supports advanced scheduling. */
void
thread_set_priority (int new_priority) 
{
  /* Disables manual priority setting when advanced scheduler 
     is activated. */
  if (thread_mlfqs)
    return;
  
  /* Original code */
  //thread_current ()->priority = new_priority;
  priority_modif (new_priority);
  thread_preemption ();
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE 
   and much more. */
void
thread_set_nice (int nice UNUSED) 
{
  struct thread *cur = thread_current ();

  /* Interrupt must be turned off when dealing with nice 
     values and other parameters related to advanced scheduling
     Since they are continuously updated by the 
     timer_interrupt (). 
     The same follows for the other functions below. */
  enum intr_level old_level;
  //ASSERT (!intr_context);
  old_level = intr_disable ();

  cur->nice = nice;
  /* In addition, priority which value is related to nice 
     in advanced scheduling is also updated, and we check
     based on priority if preemption is needed (and do so 
     if necessary). */ 
  priority_calculator (cur);
  thread_preemption ();

  intr_set_level (old_level);
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  enum intr_level old_level;
  //ASSERT (!intr_context);
  old_level = intr_disable ();

  struct thread *cur = thread_current ();
  int nice_val = cur->nice;

  intr_set_level (old_level);

  return nice_val;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  enum intr_level old_level;
  //ASSERT (!intr_context);
  old_level = intr_disable ();

  fp_t load_average_hundred = product_fp_and_int 
    (load_average, 100);
  int load_avg_int = cvt_fp_to_int_round (load_average_hundred);

  intr_set_level (old_level);

  return load_avg_int;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  enum intr_level old_level;
  //ASSERT (!intr_context);
  old_level = intr_disable ();

  struct thread *cur = thread_current ();
  fp_t recent_cpu_hundred = product_fp_and_int
    (cur->recent_cpu, 100);
  int recent_cpu_int = cvt_fp_to_int_round (recent_cpu_hundred);

  intr_set_level (old_level);

  return recent_cpu_int;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. 
   
   This function now initializes the DS for priority donation.*/
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->magic = THREAD_MAGIC;

  /* Set the data structure for priority donation. */
  t->wait_on_lock = NULL;
  list_init (&t->donations);
  t->initial_priority = priority;

  /* Initialize nice and recent_cpu for advanced scheduling. */
  t->nice = INITIAL_NICE;
  t->recent_cpu = INITIAL_RECENT_CPU;

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);

/* Calculates priority of a thread using its recent_cpu and nice. */
void 
priority_calculator (struct thread *t)
{
  if (t != idle_thread) /* idle_thread has a fixed priority */
    {
      fp_t recent_cpu_quarter = div_fp_by_int (t->recent_cpu, 4);
      int foo = PRI_MAX - (t->nice * 2);
      fp_t foo_fp = cvt_int_to_fp (foo);
      fp_t priority_fp = sub_fp_from_fp (foo_fp, recent_cpu_quarter);
      t->priority = cvt_fp_to_int_truncate (priority_fp);
    }
}

/* Calculates decay from load_average. */
fp_t
decay_calculator (void)
{
  fp_t load_average_double = product_fp_and_int (load_average, 2);
  fp_t lad_plus_one = add_fp_and_int (load_average_double, 1);

  return div_fp_by_fp (load_average_double, lad_plus_one);
}

/* Calculates the recent_cpu of a thread from decay and
   the thread's nice and recent_cpu. */
void
recent_cpu_calculator (struct thread *t)
{
  if (t != idle_thread)
  {
    fp_t decay = decay_calculator ();
    fp_t dec_cpu = product_two_fp (decay, t->recent_cpu);
    t->recent_cpu = add_fp_and_int (dec_cpu, t->nice);
  }
}

/* Calculates load_average from load_average and the number of 
   non-idle threads. 
   This happens in every second. */
void
load_average_calculator (void)
{
  int ready_threads = list_size (&ready_list);

  /* Include the current thread if it is not the idle thread. */
  struct thread *cur = thread_current ();
  if (cur != idle_thread)
    ready_threads++;

  fp_t load_mul = product_fp_and_int (load_average, 59);
  fp_t load_div = div_fp_by_int (load_mul, 60);

  fp_t ready_fp = cvt_int_to_fp (ready_threads);
  fp_t ready = div_fp_by_int (ready_fp, 60);

  load_average = add_two_fp (load_div, ready);
}

/* Increases recent_cpu of the running thread by 1. 
   This happens in every tick. */
void
recent_cpu_increment (void)
{
  struct thread *cur = thread_current ();
  if (cur != idle_thread)
    cur->recent_cpu = add_fp_and_int (cur->recent_cpu, 1);
}

/* Recalculates priority of all threads by calling 
   priority_calculator (). 
   This happens in every fourth tick. */
void
priority_recalculate (void)
{
  struct list_elem *e;
  struct thread *t;
  for (e = list_begin (&all_list); e != list_end (&all_list); 
    e = list_next (e))
  {
    t = list_entry (e, struct thread, allelem);
    priority_calculator (t); 
  }
}

/* Recalculates recent_cpu of all threads by calling 
   priority_calculator (). 
   This happens in every second. */
void
recent_cpu_recalculate (void)
{
  struct list_elem *e;
  struct thread *t;
  for (e = list_begin (&all_list); e != list_end (&all_list); 
    e = list_next (e))
  {
    t = list_entry (e, struct thread, allelem);
    recent_cpu_calculator (t); 
  }
}