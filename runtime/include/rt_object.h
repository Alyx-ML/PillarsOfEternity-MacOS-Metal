/*
 * Kernel objects and handles.
 *
 * A guest handle is a nonzero multiple of 4 that identifies one entry of the
 * handle table. Each entry points to one object. An object has a reference
 * count: one reference for each handle and one for each runtime user.
 *
 * Waitable objects (events, mutexes, semaphores, threads) use one global
 * lock and one global condition variable. A change of a signal state
 * wakes all waiters, and each waiter checks its own objects again.
 */
#ifndef RT_OBJECT_H
#define RT_OBJECT_H

#include "rt.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

typedef enum {
    OBJ_EVENT = 1,
    OBJ_MUTEX,
    OBJ_SEMAPHORE,
    OBJ_THREAD,
    OBJ_FILE,
    OBJ_FIND,
    OBJ_REGKEY,
    OBJ_CRYPT,
    OBJ_DEVINFO,
    OBJ_SOCKET,
    OBJ_SECTION,
    OBJ_PROCESS,
    OBJ_TOKEN,
    OBJ_EVENTLOG,
    OBJ_HID, /* a HID device (rt_hid.c) */
} rt_object_type;

typedef struct rt_object rt_object;
typedef struct rt_wait_node rt_wait_node;
struct rt_object {
    rt_object_type type;
    int refs;                      /* protected by the object lock */
    void (*destroy)(rt_object *o); /* releases the type-specific data; NULL if none */
    /* Wait state (protected by the object lock). */
    bool signaled;                 /* events, threads */
    bool manual_reset;             /* events */
    bool abandoned;                /* mutexes */
    uint32_t owner_tid;            /* mutexes: owner thread id, 0 if free */
    uint32_t recursion;            /* mutexes */
    int32_t count, max_count;      /* semaphores */
    uint32_t exit_code;            /* threads */
    void *data;                    /* type-specific data */
    char *name;                    /* events, mutexes, semaphores: the object name, or NULL */
    rt_wait_node *wait_head, *wait_tail; /* threads that wait for this object (object lock) */
};

#define RT_PSEUDO_PROCESS 0xFFFFFFFFu
#define RT_PSEUDO_THREAD 0xFFFFFFFEu

/* Allocate an object with one reference. Stops the process when no host
   memory is available. */
rt_object *rt_object_new(rt_object_type type);

/* Add a handle for o. The handle takes one new reference. Returns 0 if the
   handle table is full. */
uint32_t rt_handle_new(rt_object *o);

/* Get the object of handle h with one new reference, or NULL if h is not a
   handle of the given type (type 0: any type). */
rt_object *rt_handle_get(uint32_t h, rt_object_type type);

/* Remove handle h. Returns false if h is not a handle. */
bool rt_handle_close(uint32_t h);

void rt_object_ref(rt_object *o);
void rt_object_unref(rt_object *o);

/* The global object lock and the condition variable for waits. */
void rt_obj_lock(void);
void rt_obj_unlock(void);
/* Wake all waiters. Call with the object lock held. */
void rt_obj_broadcast(void);
/* Wake up to n threads that wait for o (n = 0: all of them), in arrival
   order. Call with the object lock held after o became signaled. */
void rt_obj_signal(rt_object *o, uint32_t n);
/* Wait for the next broadcast. Call with the object lock held. */
void rt_obj_wait(void);

/* A wait that does not block on a condition variable (the main thread
   handles native events while it waits). */
typedef struct rt_wait_hook {
    /* Set by the object code (with the object lock held) when the waiter
       is woken. */
    atomic_bool woken;
    /* Called with the object lock held after woken was set. It must not
       block or take a lock. */
    void (*notify)(void);
    /* Called without the object lock: block until woken is true or the
       deadline (CLOCK_UPTIME_RAW nanoseconds) passes. Returns false at
       the deadline. */
    bool (*block)(struct rt_wait_hook *h, bool infinite, uint64_t deadline_ns);
} rt_wait_hook;

/* Wait for count objects (1 to 64; the caller keeps its references).
   Returns the Windows wait result (WAIT_OBJECT_0 + i, WAIT_ABANDONED_0 +
   i, WAIT_TIMEOUT, WAIT_IO_COMPLETION). hook: NULL for a normal wait. */
uint32_t rt_wait_object_list(_cpu *cpu, uint32_t count, rt_object **objs, bool wait_all, uint32_t ms, bool alertable,
                             rt_wait_hook *hook);
/* Get the objects of count handles with one reference each. Returns false
   if a handle is not a waitable object (no references are kept). */
bool rt_handles_get_waitable(uint32_t count, const uint32_t *hs, rt_object **objs);

/* The process object of the current process, with a new reference
   (OpenProcess, DuplicateHandle of the pseudo handle). */
rt_object *rt_process_object(void);

/* Signal a thread object (thread exit) and mark the mutexes owned by tid
   as abandoned. Takes the object lock. */
void rt_obj_thread_exited(rt_object *thread_obj, uint32_t tid, uint32_t exit_code);

#endif
