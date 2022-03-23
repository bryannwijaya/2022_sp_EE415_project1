/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
     decrement it.

   - up or "V": increment the value (and wake up one waiting
     thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) 
{
  ASSERT (sema != NULL);

  sema->value = value;
  list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. 
   
   This function now supports priority scheduling by inserting
   the thread to waiters list in decreasing order of priority. */
void
sema_down (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0) 
    {
      /* Original code */
      //list_push_back (&sema->waiters, &thread_current ()->elem);
      struct thread *t = thread_current ();
      /* Insert thread at waiters list in order of decreasing priority. */
      list_insert_ordered (&sema->waiters, &t->elem, thread_priority, NULL);
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) 
{
  enum intr_level old_level;
  bool success;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (sema->value > 0) 
    {
      sema->value--;
      success = true; 
    }
  else
    success = false;
  intr_set_level (old_level);

  return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. 
   
   This function now supports priority scheduling by sorting the
   list in the order of decreasing priority, checking for 
   preemption, and execute preemption when necesssary. */
void
sema_up (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (!list_empty (&sema->waiters)) 
  {
    /* Sort the waiters list in order of decreasing priority */
    list_sort (&sema->waiters, thread_priority, NULL);
    thread_unblock (list_entry (list_pop_front (&sema->waiters),
                                struct thread, elem));
  }
  sema->value++;
  
  /* To consider the case of changing priority of threads in the
     list, we check for premption and execute if necessary. */
  thread_preemption ();

  intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) 
{
  struct semaphore sema[2];
  int i;

  printf ("Testing semaphores...");
  sema_init (&sema[0], 0);
  sema_init (&sema[1], 0);
  thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) 
    {
      sema_up (&sema[0]);
      sema_down (&sema[1]);
    }
  printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) 
{
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) 
    {
      sema_down (&sema[0]);
      sema_up (&sema[1]);
    }
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);

  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);
}

/* Compares the priority of two donor threads t1 and t2, where 
   t1->d_elem is elem1 and t2->d_elem is elem2. Returns a bool 
   TRUE when the priority of t1 is bigger than that of t2.
   This function is used to insert the threads to the
   lock holder's donations list in an ordered manner
   (i.e., descending priority). 
   
   This is similar to "thread_priority()" but for the elements of 
   donations list instead of ready_list. Utilized for the handling
   of multiple priority donations .*/
static bool
donate_priority (const struct list_elem *elem1,
                 const struct list_elem *elem2, void *aux UNUSED)
{
  struct thread *t1 = list_entry (elem1, struct thread, d_elem);
  struct thread *t2 = list_entry (elem2, struct thread, d_elem);
  
  return t1->priority > t2->priority;
}

/* Handles nested priority donation, in which the current thread 
   donates its priority to the holder of the lock it is waiting for,
   and the process continues if the holder in turn is waiting for 
   another lock hold by another thread and so forth. */
static void
priority_donation (void)
{
  struct thread *cur = thread_current (), *hld;
  while (true)
  {
    if (!cur->wait_on_lock)
      break;
    hld = cur->wait_on_lock->holder;
    hld->priority = cur->priority;
    cur = hld;
  }
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. 
   
   This function now supports priority donation by storing the lock 
   address to current thread's wait_on_lock, inserting it to the 
   lock->holder->donations list in the order of decreasing priority, 
   and donating priority to the lock holder. This all happens when
   the lock is being hold by another thread (i.e., holder). 
   
   This function also supports advanced scheduling. */
void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  struct thread *cur = thread_current ();

  /* Support for priority donation. 
     However, when advanced scheduler is activated, 
     this is skipped. */
  if (!thread_mlfqs)
  {
    if (lock->holder)
    {
      cur->wait_on_lock = lock;
      list_insert_ordered (&lock->holder->donations, &cur->d_elem, 
                            donate_priority, NULL);
      priority_donation ();
    }
  }

  sema_down (&lock->semaphore);
  lock->holder = cur;
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock)
{
  bool success;

  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));

  success = sema_try_down (&lock->semaphore);
  if (success)
    lock->holder = thread_current ();
  return success;
}

/* When the current thread is releasing its LOCK lock, removes its 
   donations list elements d_elem that wait for LOCK lock and sets 
   the priority value of the current thread to the highest priority
   between its initial_priority or the highest priority among the
   threads in its donations list (if list is not empty). */
static void
set_priorities_right (struct lock *lock)
{
  struct thread *cur = thread_current ();

  /* Removes all the donations list elements d_elem that were waiting
     for the lock LOCK (which is being released). */
  for (struct list_elem *e = list_begin (&cur->donations); 
    e != list_end (&cur->donations); e = list_next (e))
  {
    struct thread *t = list_entry (e, struct thread, d_elem);
    if (t->wait_on_lock == lock)
      list_remove (&t->d_elem);
  }

  /* Changes the priority of the lock-releasing thread.
     First, it sets its priority value to its initial_priority.
     It then checks if there is any d_elem dangling to donations list
     that wait for the other lock(s) it holds such that the lock is
     not LOCK. If that is the case, sets the priority value to 
     MAX(initial_priority, highest_thread_priority_in_donations) */
  cur->priority = cur->initial_priority;
  if (!list_empty (&cur->donations)) 
  {
    //list_sort (&cur->donations, donate_priority, 0);
    struct thread *front = list_entry (list_front (&cur->donations), 
                                       struct thread, d_elem);
    if (front->priority > cur->priority)
      cur->priority = front->priority;
  }
}

/* Releases LOCK, which must be owned by the current thread.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. 
   
   This function now supports priority donation by restoring
   the suitable priority values when the lock is released. 
   
   This function also now supports advanced scheduling. */
void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  /* Support for priority donation. 
     However, when advanced scheduler is activated, 
     this is skipped. */
  if (!thread_mlfqs)
    set_priorities_right (lock);

  lock->holder = NULL;
  sema_up (&lock->semaphore);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);

  return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem 
  {
    struct list_elem elem;              /* List element. */
    struct semaphore semaphore;         /* This semaphore. */
  };

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);

  list_init (&cond->waiters);
}

/* Compares the priority of two threads that are each located 
   in the front of priority-ordered semaphore->waiters lists 
   where each semaphore belongs to semaphore_elems se1 and se2 
   and se1->elem is elem1 and se2->elem is elem2. Returns 
   a bool TRUE when the priority of se1 front thread is bigger 
   than that of se2. This function is used to insert the 
   semaphore_elem to the list in an ordered manner
   (i.e., descending priority). 

   This is similar to "thread_priority()" but for struct 
   semaphore_elem instead of thread due to the construction 
   of semaphore_elem and semaphore. Utilized for the handling
   of condition variables. */
static bool
semaphore_elem_priority (const struct list_elem *elem1, 
const struct list_elem *elem2, void *aux UNUSED)
{  
  /* Trace semaphore_elem from semaphore_elem->elem. */
  struct semaphore_elem *se1 = list_entry (elem1, 
                                struct semaphore_elem, elem);
  struct semaphore_elem *se2 = list_entry (elem2, 
                                struct semaphore_elem, elem);
  
  /* Trace semaphore from semaphore_elem. */
  struct semaphore *sse1 = &se1->semaphore;
  struct semaphore *sse2 = &se2->semaphore;

  /* Trace semaphore->waiters list from semaphore. */
  struct list *waiter_se1 = &sse1->waiters;
  struct list *waiter_se2 = &sse2->waiters;

  /* Trace list_elem of the frontmost thread of semaphore->
     waiters list.
     list_begin () is used instead of list_front () because kernel
     panic occurs with the latter due to the (!list_empty (list))
     assertion in it :( */
  struct list_elem *fnt_waiter_se1 = list_begin (waiter_se1);
  struct list_elem *fnt_waiter_se2 = list_begin (waiter_se2);

  /* Trace the frontmost thread of list from its list_elem. */
  struct thread *tfnt_waiter_se1 = list_entry (fnt_waiter_se1, 
                                        struct thread, elem);
  struct thread *tfnt_waiter_se2 = list_entry (fnt_waiter_se2, 
                                        struct thread, elem);

  return tfnt_waiter_se1->priority > tfnt_waiter_se2->priority;
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. 
   
   This function now supports priority scheduling by inserting 
   waiter element to the condition variable waiters in the order 
   of decreasing priority. */
void
cond_wait (struct condition *cond, struct lock *lock) 
{
  struct semaphore_elem waiter;

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));
  
  sema_init (&waiter.semaphore, 0);
  /* Original code */
  //list_push_back (&cond->waiters, &waiter.elem);
  list_insert_ordered (&cond->waiters, &waiter.elem, 
                          semaphore_elem_priority, NULL);
  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. 
   
   This function now supports priority scheduling by sorting the
   condition variable waiters list in the decreasing order of
   priority. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  if (!list_empty (&cond->waiters)) 
  {
    list_sort (&cond->waiters, semaphore_elem_priority, NULL);
    sema_up (&list_entry (list_pop_front (&cond->waiters),
                          struct semaphore_elem, elem)->semaphore);
  }
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);

  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}
