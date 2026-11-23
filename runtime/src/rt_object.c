/* Kernel objects, the handle table, and waits on objects. */
#include "rt_object.h"
#include "rt_text.h"
#include "rt_thread.h"

#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_HANDLES 65536u

static pthread_mutex_t obj_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t obj_cond = PTHREAD_COND_INITIALIZER;
static rt_object *handles[MAX_HANDLES];
static uint32_t next_free_hint;

void rt_obj_lock(void)
{
    int r = pthread_mutex_lock(&obj_lock);
    if (r != 0) rt_fatal("object lock failed (%d)", r);
}

void rt_obj_unlock(void)
{
    int r = pthread_mutex_unlock(&obj_lock);
    if (r != 0) rt_fatal("object unlock failed (%d)", r);
}

/* A thread in wait_objects. Each waiter has its own condition variable,
   and one node on the wait list of each object that it waits for. A
   signal wakes only the waiters of the signaled object. */
typedef struct waiter waiter;
struct waiter {
    pthread_cond_t cv;
    bool woken;
    rt_wait_hook *hook;  /* NULL: the waiter blocks on cv */
    waiter *prev, *next; /* list of all waiters (all_waiters) */
};
struct rt_wait_node {
    waiter *w;
    rt_wait_node *prev, *next;
};
static waiter *all_waiters;

static void wake(waiter *w)
{
    int r;
    w->woken = true;
    if (w->hook != NULL)
    {
        atomic_store(&w->hook->woken, true);
        w->hook->notify();
    }
    r = pthread_cond_signal(&w->cv);
    if (r != 0) rt_fatal("object signal failed (%d)", r);
}

void rt_obj_broadcast(void)
{
    waiter *w;
    int r = pthread_cond_broadcast(&obj_cond);
    if (r != 0) rt_fatal("object broadcast failed (%d)", r);
    for (w = all_waiters; w != NULL; w = w->next) wake(w);
}

void rt_obj_signal(rt_object *o, uint32_t n)
{
    rt_wait_node *x;
    uint32_t k = 0;
    for (x = o->wait_head; x != NULL && (n == 0 || k < n); x = x->next)
    {
        if (x->w->woken) continue;
        wake(x->w);
        k++;
    }
}

void rt_obj_wait(void)
{
    int r = pthread_cond_wait(&obj_cond, &obj_lock);
    if (r != 0) rt_fatal("condition wait failed (%d)", r);
}

rt_object *rt_object_new(rt_object_type type)
{
    rt_object *o = calloc(1, sizeof(*o));
    if (o == NULL) rt_fatal("out of host memory for a kernel object");
    o->type = type;
    o->refs = 1;
    return o;
}

void rt_object_ref(rt_object *o)
{
    rt_obj_lock();
    if (o->refs <= 0 || o->refs == INT32_MAX) rt_fatal("bad reference count %d of an object", o->refs);
    o->refs++;
    rt_obj_unlock();
}

/* Named events, mutexes and semaphores (protected by the object lock).
   The game is one process, so a name identifies an object in this process
   only. An entry is removed when the last reference of its object goes. */
#define MAX_NAMED 256u
static rt_object *named[MAX_NAMED];

void rt_object_unref(rt_object *o)
{
    int left;
    uint32_t i;
    rt_obj_lock();
    if (o->refs <= 0) rt_fatal("release of an object without references");
    left = --o->refs;
    if (left == 0 && o->name != NULL)
    {
        for (i = 0; i < MAX_NAMED; i++)
            if (named[i] == o) named[i] = NULL;
    }
    rt_obj_unlock();
    if (left == 0)
    {
        if (o->destroy != NULL) o->destroy(o);
        free(o->name);
        free(o);
    }
}

uint32_t rt_handle_new(rt_object *o)
{
    uint32_t i, n;
    rt_obj_lock();
    for (n = 0; n < MAX_HANDLES; n++)
    {
        i = (next_free_hint + n) % MAX_HANDLES;
        if (i == 0) continue; /* handle 0 is invalid */
        if (handles[i] == NULL)
        {
            if (o->refs == INT32_MAX) rt_fatal("too many references to an object");
            o->refs++;
            handles[i] = o;
            next_free_hint = i + 1;
            rt_obj_unlock();
            return i * 4u;
        }
    }
    rt_obj_unlock();
    return 0;
}

static uint32_t handle_index(uint32_t h)
{
    if (h == 0 || (h & 3u) != 0) return 0;
    if (h / 4u >= MAX_HANDLES) return 0;
    return h / 4u;
}

rt_object *rt_handle_get(uint32_t h, rt_object_type type)
{
    uint32_t i = handle_index(h);
    rt_object *o;
    if (i == 0) return NULL;
    rt_obj_lock();
    o = handles[i];
    if (o != NULL && (type == 0 || o->type == type))
    {
        o->refs++;
    }
    else
    {
        o = NULL;
    }
    rt_obj_unlock();
    return o;
}

bool rt_handle_close(uint32_t h)
{
    uint32_t i = handle_index(h);
    rt_object *o;
    if (i == 0) return false;
    rt_obj_lock();
    o = handles[i];
    handles[i] = NULL;
    if (o != NULL && i < next_free_hint) next_free_hint = i;
    rt_obj_unlock();
    if (o == NULL) return false;
    rt_object_unref(o);
    return true;
}

void rt_obj_thread_exited(rt_object *thread_obj, uint32_t tid, uint32_t exit_code)
{
    uint32_t i;
    rt_obj_lock();
    for (i = 1; i < MAX_HANDLES; i++)
    {
        rt_object *o = handles[i];
        if (o != NULL && o->type == OBJ_MUTEX && o->owner_tid == tid)
        {
            o->owner_tid = 0;
            o->recursion = 0;
            o->abandoned = true;
        }
    }
    if (thread_obj != NULL)
    {
        thread_obj->signaled = true;
        thread_obj->exit_code = exit_code;
    }
    /* Thread exit is rare: wake every waiter (abandoned mutexes too). */
    rt_obj_broadcast();
    rt_obj_unlock();
}

/* ---------------------------------------------------------------- waits */

#define WAIT_OBJECT_0 0u
#define WAIT_ABANDONED_0 0x80u
#define WAIT_IO_COMPLETION 0xC0u
#define WAIT_TIMEOUT 0x102u
#define WAIT_FAILED 0xFFFFFFFFu
#define INFINITE 0xFFFFFFFFu
#define MAXIMUM_WAIT_OBJECTS 64

static bool is_waitable(const rt_object *o)
{
    return o->type == OBJ_EVENT || o->type == OBJ_MUTEX || o->type == OBJ_SEMAPHORE || o->type == OBJ_THREAD ||
           o->type == OBJ_PROCESS;
}

/* Object lock held. */
static bool can_acquire(const rt_object *o, uint32_t tid)
{
    switch (o->type)
    {
    case OBJ_EVENT:
    case OBJ_THREAD:
    case OBJ_PROCESS:
        return o->signaled;
    case OBJ_MUTEX:
        return o->owner_tid == 0 || o->owner_tid == tid;
    case OBJ_SEMAPHORE:
        return o->count > 0;
    default:
        return false;
    }
}

/* Object lock held. Returns true if the mutex was abandoned. */
static bool acquire(rt_object *o, uint32_t tid)
{
    bool was_abandoned = false;
    switch (o->type)
    {
    case OBJ_EVENT:
        if (!o->manual_reset) o->signaled = false;
        break;
    case OBJ_MUTEX:
        if (o->recursion == UINT32_MAX) rt_fatal("mutex recursion count overflow");
        o->owner_tid = tid;
        o->recursion++;
        was_abandoned = o->abandoned;
        o->abandoned = false;
        break;
    case OBJ_SEMAPHORE:
        o->count--;
        break;
    default:
        break;
    }
    return was_abandoned;
}

static uint64_t now_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* Wait on the condition variable of w until the deadline. Object lock
   held. Returns false when the deadline passed. */
static bool cond_wait_until(waiter *w, bool infinite, uint64_t deadline)
{
    int r;
    if (infinite)
    {
        r = pthread_cond_wait(&w->cv, &obj_lock);
        if (r != 0) rt_fatal("condition wait failed (%d)", r);
        return true;
    }
    uint64_t t = now_ns();
    if (t >= deadline) return false;
    uint64_t rem = deadline - t;
    struct timespec ts = { (time_t)(rem / 1000000000u), (long)(rem % 1000000000u) };
    r = pthread_cond_timedwait_relative_np(&w->cv, &obj_lock, &ts);
    if (r != 0 && r != ETIMEDOUT) rt_fatal("timed condition wait failed (%d)", r);
    return true;
}

/* Object lock held. True if a waiter could take o now (a mutex: if it has
   no owner). */
static bool available(const rt_object *o)
{
    switch (o->type)
    {
    case OBJ_EVENT:
    case OBJ_THREAD:
    case OBJ_PROCESS:
        return o->signaled;
    case OBJ_MUTEX:
        return o->owner_tid == 0;
    case OBJ_SEMAPHORE:
        return o->count > 0;
    default:
        return false;
    }
}

/* Try to finish the wait. Object lock held. Returns true and sets *result
   when the wait is satisfied. */
static bool try_acquire(rt_object **objs, uint32_t count, bool wait_all, uint32_t tid, uint32_t *result)
{
    uint32_t i;
    if (wait_all)
    {
        bool all = true, ab = false;
        for (i = 0; i < count && all; i++) all = can_acquire(objs[i], tid);
        if (!all) return false;
        for (i = 0; i < count; i++) ab |= acquire(objs[i], tid);
        *result = ab ? WAIT_ABANDONED_0 : WAIT_OBJECT_0;
        return true;
    }
    for (i = 0; i < count; i++)
    {
        if (can_acquire(objs[i], tid))
        {
            *result = (acquire(objs[i], tid) ? WAIT_ABANDONED_0 : WAIT_OBJECT_0) + i;
            return true;
        }
    }
    return false;
}

/* Object lock held. */
static bool apc_pending(const rt_thread *t)
{
    return t->apc_head != NULL;
}

/* Wait for count objects. Returns the Windows wait result. An alertable
   wait ends with WAIT_IO_COMPLETION when user APCs are queued for the
   thread; they run after the wait, without the object lock. */
uint32_t rt_wait_object_list(_cpu *cpu, uint32_t count, rt_object **objs, bool wait_all, uint32_t ms, bool alertable,
                             rt_wait_hook *hook)
{
    uint32_t i, result = WAIT_FAILED;
    rt_thread *self = rt_self();
    uint32_t tid = self->tid;
    bool infinite = (ms == INFINITE);
    uint64_t deadline = infinite ? 0 : now_ns() + (uint64_t)ms * 1000000u;

    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS) rt_fatal("rt_wait_object_list: %u objects", count);
    for (i = 0; i < count; i++)
        if (!is_waitable(objs[i])) rt_fatal("rt_wait_object_list: object type %d cannot be waited for", (int)objs[i]->type);

    rt_obj_lock();
    if (alertable && apc_pending(self))
    {
        result = WAIT_IO_COMPLETION;
    }
    else if (!try_acquire(objs, count, wait_all, tid, &result))
    {
        if (!infinite && ms == 0)
        {
            result = WAIT_TIMEOUT;
        }
        else
        {
            waiter me;
            rt_wait_node nodes[MAXIMUM_WAIT_OBJECTS];
            bool was_woken = false;
            int r = pthread_cond_init(&me.cv, NULL);
            if (r != 0) rt_fatal("condition variable creation failed (%d)", r);
            me.woken = false;
            me.hook = hook;
            me.prev = NULL;
            me.next = all_waiters;
            if (all_waiters != NULL) all_waiters->prev = &me;
            all_waiters = &me;
            if (alertable) self->alert_waiter = &me;
            for (i = 0; i < count; i++)
            {
                nodes[i].w = &me;
                nodes[i].next = NULL;
                nodes[i].prev = objs[i]->wait_tail;
                if (objs[i]->wait_tail != NULL)
                    objs[i]->wait_tail->next = &nodes[i];
                else
                    objs[i]->wait_head = &nodes[i];
                objs[i]->wait_tail = &nodes[i];
            }
            for (;;)
            {
                bool in_time;
                me.woken = false;
                if (hook != NULL)
                {
                    /* The hook blocks without the object lock (the main
                       thread handles native events while it waits). */
                    atomic_store(&hook->woken, false);
                    rt_obj_unlock();
                    in_time = hook->block(hook, infinite, deadline);
                    rt_obj_lock();
                }
                else
                    in_time = cond_wait_until(&me, infinite, deadline);
                was_woken |= me.woken;
                if (alertable && apc_pending(self))
                {
                    result = WAIT_IO_COMPLETION;
                    break;
                }
                if (try_acquire(objs, count, wait_all, tid, &result)) break;
                if (!in_time)
                {
                    result = WAIT_TIMEOUT;
                    break;
                }
            }
            for (i = 0; i < count; i++)
            {
                if (nodes[i].prev != NULL) nodes[i].prev->next = nodes[i].next; else objs[i]->wait_head = nodes[i].next;
                if (nodes[i].next != NULL) nodes[i].next->prev = nodes[i].prev; else objs[i]->wait_tail = nodes[i].prev;
            }
            if (me.prev != NULL) me.prev->next = me.next; else all_waiters = me.next;
            if (me.next != NULL) me.next->prev = me.prev;
            if (alertable) self->alert_waiter = NULL;
            /* A wakeup that this thread did not use (it took another
               object, its time ran out, or another thread was first) goes
               to the next waiter of each object that is still available. */
            if (was_woken)
                for (i = 0; i < count; i++)
                    if (available(objs[i])) rt_obj_signal(objs[i], 1);
            r = pthread_cond_destroy(&me.cv);
            if (r != 0) rt_fatal("condition variable destruction failed (%d)", r);
        }
    }
    rt_obj_unlock();

    if (result == WAIT_IO_COMPLETION) rt_run_apcs(cpu);
    return result;
}

/* The object of a handle to wait for, with a reference, or NULL. As on
   Windows, the pseudo handles of the current thread and process are the
   thread and process objects: they are not signaled while the thread or
   process runs, so the wait ends at its time limit or with an APC. */
static rt_object *wait_handle_object(uint32_t h)
{
    if (h == RT_PSEUDO_THREAD) return rt_thread_self_object();
    if (h == RT_PSEUDO_PROCESS) return rt_process_object();
    return rt_handle_get(h, 0);
}

/* Wait for count handles. Returns the Windows wait result. */
static uint32_t wait_objects(_cpu *cpu, uint32_t count, const uint32_t *hs, bool wait_all, uint32_t ms, bool alertable)
{
    rt_object *objs[MAXIMUM_WAIT_OBJECTS];
    uint32_t i, result;

    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    for (i = 0; i < count; i++)
    {
        objs[i] = wait_handle_object(hs[i]);
        if (objs[i] == NULL || !is_waitable(objs[i]))
        {
            uint32_t j;
            if (objs[i] != NULL) rt_object_unref(objs[i]);
            for (j = 0; j < i; j++) rt_object_unref(objs[j]);
            rt_set_last_error(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
    }
    result = rt_wait_object_list(cpu, count, objs, wait_all, ms, alertable, NULL);
    for (i = 0; i < count; i++) rt_object_unref(objs[i]);
    return result;
}

/* Get the waitable objects of count handles (MsgWaitForMultipleObjects).
   Returns false (and releases what it got) if a handle is not waitable. */
bool rt_handles_get_waitable(uint32_t count, const uint32_t *hs, rt_object **objs)
{
    uint32_t i;
    for (i = 0; i < count; i++)
    {
        objs[i] = wait_handle_object(hs[i]);
        if (objs[i] == NULL || !is_waitable(objs[i]))
        {
            uint32_t j;
            if (objs[i] != NULL) rt_object_unref(objs[i]);
            for (j = 0; j < i; j++) rt_object_unref(objs[j]);
            return false;
        }
    }
    return true;
}

void rt_WaitForSingleObject(_cpu *cpu)
{
    uint32_t h = ARG(0);
    RET(wait_objects(cpu, 1, &h, false, ARG(1), false));
}

void rt_WaitForSingleObjectEx(_cpu *cpu)
{
    uint32_t h = ARG(0);
    RET(wait_objects(cpu, 1, &h, false, ARG(1), ARG(2) != 0));
}

static void wait_multiple(_cpu *cpu, bool alertable)
{
    uint32_t count = ARG(0), arr = ARG(1), wait_all = ARG(2), ms = ARG(3);
    uint32_t hs[MAXIMUM_WAIT_OBJECTS];
    uint32_t i;
    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS || arr == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(WAIT_FAILED);
        return;
    }
    for (i = 0; i < count; i++) hs[i] = rd32(arr + 4u * i);
    RET(wait_objects(cpu, count, hs, wait_all != 0, ms, alertable));
}

void rt_WaitForMultipleObjects(_cpu *cpu) { wait_multiple(cpu, false); }
void rt_WaitForMultipleObjectsEx(_cpu *cpu) { wait_multiple(cpu, ARG(4) != 0); }

/* ---------------------------------------------------------------- APCs and alertable sleep */

bool rt_run_apcs(_cpu *cpu)
{
    rt_thread *t = rt_self();
    bool ran = false;
    for (;;)
    {
        rt_apc *a;
        uint32_t func, data;
        rt_obj_lock();
        a = t->apc_head;
        if (a != NULL)
        {
            t->apc_head = a->next;
            if (t->apc_head == NULL) t->apc_tail = NULL;
        }
        rt_obj_unlock();
        if (a == NULL) break;
        func = a->func;
        data = a->data;
        free(a);
        /* VOID CALLBACK APCProc(ULONG_PTR) */
        (void)rt_call_std(cpu, func, 1, data);
        ran = true;
    }
    return ran;
}

/* DWORD QueueUserAPC(PAPCFUNC func, HANDLE thread, ULONG_PTR data) */
void rt_QueueUserAPC(_cpu *cpu)
{
    uint32_t func = ARG(0), h = ARG(1), data = ARG(2);
    rt_object *o;
    rt_thread *t;
    rt_apc *a;
    if (func == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    o = h == RT_PSEUDO_THREAD ? rt_thread_self_object() : rt_handle_get(h, OBJ_THREAD);
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    a = malloc(sizeof(*a));
    if (a == NULL) rt_fatal("out of host memory (APC)");
    a->func = func;
    a->data = data;
    a->next = NULL;
    rt_obj_lock();
    t = rt_thread_state_locked(o);
    if (t == NULL)
    {
        bool ended = o->signaled;
        rt_obj_unlock();
        rt_object_unref(o);
        free(a);
        if (!ended) rt_fatal("QueueUserAPC to a thread that has not started is not supported");
        /* STATUS_THREAD_IS_TERMINATING */
        rt_set_last_error(ERROR_ACCESS_DENIED);
        RET(0);
        return;
    }
    if (t->apc_tail != NULL) t->apc_tail->next = a; else t->apc_head = a;
    t->apc_tail = a;
    if (t->alert_waiter != NULL) wake((waiter *)t->alert_waiter);
    rt_obj_unlock();
    rt_object_unref(o);
    RET(1);
}

/* DWORD SleepEx(DWORD ms, BOOL alertable): 0, or WAIT_IO_COMPLETION when
   an alertable sleep ended for user APCs. */
void rt_SleepEx(_cpu *cpu)
{
    uint32_t ms = ARG(0);
    rt_thread *self = rt_self();
    bool infinite = ms == INFINITE, apc = false;
    uint64_t deadline = infinite ? 0 : now_ns() + (uint64_t)ms * 1000000u;
    waiter me;
    int r;
    if (ARG(1) == 0)
    {
        if (infinite) rt_fatal("SleepEx(INFINITE) without an alertable wait");
        if (ms == 0)
        {
            sched_yield();
        }
        else
        {
            struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
            while (nanosleep(&ts, &ts) != 0)
                if (errno != EINTR) rt_fatal("nanosleep failed (%d)", errno);
        }
        RET(0);
        return;
    }
    r = pthread_cond_init(&me.cv, NULL);
    if (r != 0) rt_fatal("condition variable creation failed (%d)", r);
    /* QueueUserAPC calls wake() on this waiter: every field that wake()
       reads must be set before the waiter is published. */
    me.woken = false;
    me.hook = NULL;
    me.prev = me.next = NULL;
    rt_obj_lock();
    self->alert_waiter = &me;
    for (;;)
    {
        bool in_time;
        if (apc_pending(self))
        {
            apc = true;
            break;
        }
        if (!infinite && ms == 0) break;
        me.woken = false;
        in_time = cond_wait_until(&me, infinite, deadline);
        if (!in_time && !apc_pending(self)) break;
    }
    self->alert_waiter = NULL;
    rt_obj_unlock();
    r = pthread_cond_destroy(&me.cv);
    if (r != 0) rt_fatal("condition variable destruction failed (%d)", r);
    if (apc)
    {
        rt_run_apcs(cpu);
        RET(WAIT_IO_COMPLETION);
        return;
    }
    if (ms == 0) sched_yield();
    RET(0);
}

/* ---------------------------------------------------------------- creation */

static uint32_t publish(rt_object *o)
{
    uint32_t h = rt_handle_new(o);
    rt_object_unref(o);
    if (h == 0)
    {
        rt_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
        return 0;
    }
    rt_set_last_error(ERROR_SUCCESS);
    return h;
}

/* Read the object name at guest address g (ANSI or wide) as UTF-8.
   Returns 1 for a name, 0 for no name (g is 0) and -1 (with the last
   error set) for a name that is not valid. */
static int read_name(uint32_t g, bool wide, char *buf, size_t n)
{
    int r;
    if (g == 0) return 0;
    r = wide ? rt_gstr_w(buf, n, g) : rt_gstr_a(buf, n, g);
    if (r != RT_STR_OK)
    {
        rt_set_last_error(r == RT_STR_TOO_LONG ? 206u /* ERROR_FILENAME_EXCED_RANGE */ : ERROR_INVALID_PARAMETER);
        return -1;
    }
    return 1;
}

/* CreateXxx with a name. If an object with the name exists, *existing
   gets it with one new reference (NULL and ERROR_INVALID_HANDLE for an
   object of another type). Otherwise o (a new object) gets the name.
   Returns false if the call must fail. */
static bool name_object(const char *name, rt_object *o, rt_object **existing, const char *what)
{
    uint32_t i, free_slot = MAX_NAMED;
    *existing = NULL;
    rt_obj_lock();
    for (i = 0; i < MAX_NAMED; i++)
    {
        if (named[i] == NULL)
        {
            if (free_slot == MAX_NAMED) free_slot = i;
        }
        else if (strcmp(named[i]->name, name) == 0)
        {
            if (named[i]->type != o->type)
            {
                rt_obj_unlock();
                rt_set_last_error(ERROR_INVALID_HANDLE);
                return false;
            }
            if (named[i]->refs == INT32_MAX) rt_fatal("too many references to an object");
            named[i]->refs++;
            *existing = named[i];
            rt_obj_unlock();
            return true;
        }
    }
    if (free_slot == MAX_NAMED) rt_fatal("%s: more than %u named objects", what, MAX_NAMED);
    o->name = strdup(name);
    if (o->name == NULL) rt_fatal("out of host memory (object name)");
    named[free_slot] = o;
    rt_obj_unlock();
    return true;
}

/* Finish CreateXxx: the handle of the new object o, or of the existing
   object with the same name (ERROR_ALREADY_EXISTS). gname is the guest
   name (0: no name). */
static uint32_t create_named(uint32_t gname, bool wide, rt_object *o, const char *what)
{
    char name[520];
    rt_object *existing;
    uint32_t h;
    int r = read_name(gname, wide, name, sizeof(name));
    if (r < 0)
    {
        rt_object_unref(o);
        return 0;
    }
    if (r == 0) return publish(o);
    if (!name_object(name, o, &existing, what))
    {
        rt_object_unref(o);
        return 0;
    }
    if (existing == NULL) return publish(o);
    rt_object_unref(o);
    h = publish(existing);
    if (h != 0) rt_set_last_error(ERROR_ALREADY_EXISTS);
    return h;
}

static void create_event(_cpu *cpu, bool wide)
{
    rt_object *o = rt_object_new(OBJ_EVENT);
    o->manual_reset = ARG(1) != 0;
    o->signaled = ARG(2) != 0;
    RET(create_named(ARG(3), wide, o, wide ? "CreateEventW" : "CreateEventA"));
}

void rt_CreateEventA(_cpu *cpu) { create_event(cpu, false); }
void rt_CreateEventW(_cpu *cpu) { create_event(cpu, true); }

static void create_mutex(_cpu *cpu, bool wide)
{
    rt_object *o = rt_object_new(OBJ_MUTEX);
    /* For an existing named mutex, Windows ignores the initial owner flag;
       the new object with the owner is released again. */
    if (ARG(1) != 0)
    {
        o->owner_tid = rt_self()->tid;
        o->recursion = 1;
    }
    RET(create_named(ARG(2), wide, o, wide ? "CreateMutexW" : "CreateMutexA"));
}

void rt_CreateMutexA(_cpu *cpu) { create_mutex(cpu, false); }
void rt_CreateMutexW(_cpu *cpu) { create_mutex(cpu, true); }

static void create_semaphore(_cpu *cpu, bool wide)
{
    int32_t initial = (int32_t)ARG(1), maximum = (int32_t)ARG(2);
    rt_object *o;
    if (maximum <= 0 || initial < 0 || initial > maximum)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    o = rt_object_new(OBJ_SEMAPHORE);
    o->count = initial;
    o->max_count = maximum;
    RET(create_named(ARG(3), wide, o, wide ? "CreateSemaphoreW" : "CreateSemaphoreA"));
}

void rt_CreateSemaphoreA(_cpu *cpu) { create_semaphore(cpu, false); }
void rt_CreateSemaphoreW(_cpu *cpu) { create_semaphore(cpu, true); }

/* OpenEventW, OpenMutexW, OpenSemaphoreW(DWORD access, BOOL inherit,
   LPCWSTR name): a new handle of the named object. */
static void open_named(_cpu *cpu, rt_object_type type, bool wide)
{
    char name[520];
    uint32_t i;
    rt_object *o = NULL;
    int r = read_name(ARG(2), wide, name, sizeof(name));
    if (r <= 0)
    {
        if (r == 0) rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    rt_obj_lock();
    for (i = 0; i < MAX_NAMED; i++)
    {
        if (named[i] != NULL && strcmp(named[i]->name, name) == 0)
        {
            o = named[i];
            break;
        }
    }
    if (o != NULL && o->type != type)
    {
        rt_obj_unlock();
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    if (o != NULL)
    {
        if (o->refs == INT32_MAX) rt_fatal("too many references to an object");
        o->refs++;
    }
    rt_obj_unlock();
    if (o == NULL)
    {
        rt_set_last_error(ERROR_FILE_NOT_FOUND);
        RET(0);
        return;
    }
    RET(publish(o));
}

void rt_OpenEventA(_cpu *cpu) { open_named(cpu, OBJ_EVENT, false); }
void rt_OpenEventW(_cpu *cpu) { open_named(cpu, OBJ_EVENT, true); }
void rt_OpenMutexW(_cpu *cpu) { open_named(cpu, OBJ_MUTEX, true); }
void rt_OpenSemaphoreW(_cpu *cpu) { open_named(cpu, OBJ_SEMAPHORE, true); }

static void set_event_state(_cpu *cpu, bool state)
{
    rt_object *o = rt_handle_get(ARG(0), OBJ_EVENT);
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    rt_obj_lock();
    o->signaled = state;
    if (state) rt_obj_signal(o, o->manual_reset ? 0u : 1u);
    rt_obj_unlock();
    rt_object_unref(o);
    RET(1);
}

void rt_SetEvent(_cpu *cpu) { set_event_state(cpu, true); }
void rt_ResetEvent(_cpu *cpu) { set_event_state(cpu, false); }

void rt_ReleaseSemaphore(_cpu *cpu)
{
    rt_object *o = rt_handle_get(ARG(0), OBJ_SEMAPHORE);
    int32_t n = (int32_t)ARG(1);
    uint32_t prev_ptr = ARG(2);
    bool ok;
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    rt_obj_lock();
    ok = n > 0 && o->count <= o->max_count - n;
    if (ok)
    {
        if (prev_ptr != 0) wr32(prev_ptr, (uint32_t)o->count);
        o->count += n;
        rt_obj_signal(o, (uint32_t)n);
    }
    rt_obj_unlock();
    rt_object_unref(o);
    if (!ok) rt_set_last_error(298u); /* ERROR_TOO_MANY_POSTS */
    RET(ok ? 1 : 0);
}

/* Release mutex o once. Returns false (ERROR_NOT_OWNER) if the calling
   thread does not own it. */
static bool release_mutex(rt_object *o)
{
    bool ok;
    rt_obj_lock();
    ok = o->owner_tid == rt_self()->tid && o->recursion > 0;
    if (ok && --o->recursion == 0)
    {
        o->owner_tid = 0;
        rt_obj_signal(o, 1);
    }
    rt_obj_unlock();
    if (!ok) rt_set_last_error(288u); /* ERROR_NOT_OWNER */
    return ok;
}

/* BOOL ReleaseMutex(HANDLE) */
void rt_ReleaseMutex(_cpu *cpu)
{
    rt_object *o = rt_handle_get(ARG(0), OBJ_MUTEX);
    bool ok;
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    ok = release_mutex(o);
    rt_object_unref(o);
    RET(ok ? 1 : 0);
}

/* DWORD SignalObjectAndWait(HANDLE signal, HANDLE wait, DWORD ms, BOOL
   alertable). As on Windows, the signal and the wait are two steps. */
void rt_SignalObjectAndWait(_cpu *cpu)
{
    uint32_t hw = ARG(1);
    rt_object *o = rt_handle_get(ARG(0), 0);
    bool ok = true;
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(WAIT_FAILED);
        return;
    }
    switch (o->type)
    {
    case OBJ_EVENT:
        rt_obj_lock();
        o->signaled = true;
        rt_obj_signal(o, o->manual_reset ? 0u : 1u);
        rt_obj_unlock();
        break;
    case OBJ_MUTEX:
        ok = release_mutex(o);
        break;
    case OBJ_SEMAPHORE:
        rt_obj_lock();
        ok = o->count < o->max_count;
        if (ok)
        {
            o->count++;
            rt_obj_signal(o, 1);
        }
        rt_obj_unlock();
        if (!ok) rt_set_last_error(298u); /* ERROR_TOO_MANY_POSTS */
        break;
    default:
        rt_set_last_error(ERROR_INVALID_HANDLE);
        ok = false;
        break;
    }
    rt_object_unref(o);
    if (!ok)
    {
        RET(WAIT_FAILED);
        return;
    }
    RET(wait_objects(cpu, 1, &hw, false, ARG(2), ARG(3) != 0));
}

/* ---------------------------------------------------------------- the process object and handle duplication */

static pthread_once_t process_once = PTHREAD_ONCE_INIT;
static rt_object *process_obj;

static void make_process_object(void)
{
    process_obj = rt_object_new(OBJ_PROCESS); /* the reference of the process itself */
}

rt_object *rt_process_object(void)
{
    if (pthread_once(&process_once, make_process_object) != 0) rt_fatal("process object setup failed");
    rt_object_ref(process_obj);
    return process_obj;
}

/* Is h the current process (the pseudo handle or a process handle)? */
static bool is_current_process(uint32_t h)
{
    rt_object *o;
    if (h == RT_PSEUDO_PROCESS) return true;
    o = rt_handle_get(h, OBJ_PROCESS);
    if (o == NULL) return false;
    rt_object_unref(o);
    return true;
}

/* BOOL DuplicateHandle(HANDLE src_process, HANDLE src, HANDLE dst_process,
   LPHANDLE dst, DWORD access, BOOL inherit, DWORD options). One process:
   both process handles must be the current process. The pseudo handles
   of the current thread and process give real handles. Access rights are
   not checked by the runtime, so a duplicate has the access of its
   source. */
void rt_DuplicateHandle(_cpu *cpu)
{
    uint32_t sp = ARG(0), src = ARG(1), dp = ARG(2), dst = ARG(3), options = ARG(6), h = 0;
    rt_object *o;
    if (!is_current_process(sp) || !is_current_process(dp))
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    if (options & ~3u)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (src == RT_PSEUDO_THREAD) o = rt_thread_self_object();
    else if (src == RT_PSEUDO_PROCESS) o = rt_process_object();
    else o = rt_handle_get(src, 0);
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    if (dst != 0)
    {
        h = rt_handle_new(o);
        if (h == 0)
        {
            rt_object_unref(o);
            rt_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
            RET(0);
            return;
        }
        wr32(dst, h);
    }
    rt_object_unref(o);
    if ((options & 1u) && src != RT_PSEUDO_THREAD && src != RT_PSEUDO_PROCESS) /* DUPLICATE_CLOSE_SOURCE */
        (void)rt_handle_close(src);
    RET(1);
}

/* CloseHandle works for every object type. */
void rt_CloseHandle(_cpu *cpu)
{
    uint32_t h = ARG(0);
    if (h == RT_PSEUDO_PROCESS || h == RT_PSEUDO_THREAD)
    {
        RET(1);
        return;
    }
    if (!rt_handle_close(h))
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    RET(1);
}
