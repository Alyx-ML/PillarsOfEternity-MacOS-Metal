/*
 * OLE32: apartment functions (CoInitialize, CoInitializeEx,
 * CoUninitialize, CoCreateFreeThreadedMarshaler), object creation
 * (CoCreateInstance, CoSetProxyBlanket), GUIDs (CoCreateGuid,
 * StringFromGUID2) and the runtime COM object model (runtime/include/
 * rt_com.h). The task allocator is in rt_heap.c.
 *
 * The runtime has no marshaling, so an apartment is only the per-thread
 * state that CoInitializeEx records: the number of successful calls and
 * the concurrency model.
 *
 * Classes that CoCreateInstance can create:
 *   WbemLocator (rt_wmi.c).
 * Classes that the emulated Windows XP system has but the runtime does not
 * implement (the player uses them only for WebCamTexture, and the game
 * scripts do not use WebCamTexture):
 *   the DirectShow classes SystemDeviceEnum, FilterGraph,
 *   CaptureGraphBuilder2, SampleGrabber and NullRenderer.
 * CoCreateInstance of these classes and of every other class stops the
 * process with a message.
 */
#include "rt.h"
#include "rt_com.h"
#include "rt_com_thunks.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RPC_E_CHANGED_MODE 0x80010106u

#define COINIT_APARTMENTTHREADED 0x2u
#define COINIT_DISABLE_OLE1DDE 0x4u
#define COINIT_SPEED_OVER_MEMORY 0x8u

#define CLSCTX_INPROC_SERVER 0x1u

/* ---------------------------------------------------------------- apartments */

/* Guest threads are host threads, so thread-local host data is the
   per-thread COM state. Fibers of one thread share it, as on Windows. */
static _Thread_local uint32_t com_count;
static _Thread_local bool com_sta;

static uint32_t co_initialize(uint32_t reserved, uint32_t coinit)
{
    bool sta = (coinit & COINIT_APARTMENTTHREADED) != 0;
    if (reserved != 0 || (coinit & ~(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE | COINIT_SPEED_OVER_MEMORY)) != 0)
        return E_INVALIDARG;
    if (com_count != 0 && sta != com_sta) return RPC_E_CHANGED_MODE;
    if (com_count == UINT32_MAX) rt_fatal("CoInitializeEx: too many calls on one thread");
    com_sta = sta;
    return com_count++ == 0 ? S_OK : S_FALSE;
}

/* HRESULT CoInitializeEx(LPVOID reserved, DWORD coinit) */
void rt_CoInitializeEx(_cpu *cpu)
{
    RET(co_initialize(ARG(0), ARG(1)));
}

/* HRESULT CoInitialize(LPVOID reserved): CoInitializeEx with
   COINIT_APARTMENTTHREADED. */
void rt_CoInitialize(_cpu *cpu)
{
    RET(co_initialize(ARG(0), COINIT_APARTMENTTHREADED));
}

/* void CoUninitialize(void) */
void rt_CoUninitialize(_cpu *cpu)
{
    (void)cpu;
    if (com_count != 0) com_count--;
}

/* HRESULT CoCreateFreeThreadedMarshaler(LPUNKNOWN outer, LPUNKNOWN *marshal).
   mono.dll calls it only for the IMarshal interface of a COM callable
   wrapper, when a COM client asks a managed object for IMarshal. The
   runtime has no COM marshaling. */
void rt_CoCreateFreeThreadedMarshaler(_cpu *cpu)
{
    (void)cpu;
    rt_fatal("CoCreateFreeThreadedMarshaler: COM marshaling is not supported");
}

/* ---------------------------------------------------------------- GUIDs */

static const rt_guid IID_IUnknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };

rt_guid rt_guid_read(uint32_t g)
{
    rt_guid v;
    v.d1 = rd32(g);
    v.d2 = rd16(g + 4);
    v.d3 = rd16(g + 6);
    memcpy(v.d4, G2H(g + 8), 8);
    return v;
}

void rt_guid_write(uint32_t g, const rt_guid *v)
{
    wr32(g, v->d1);
    wr16(g + 4, v->d2);
    wr16(g + 6, v->d3);
    memcpy(G2H(g + 8), v->d4, 8);
}

bool rt_guid_eq(const rt_guid *a, const rt_guid *b)
{
    return a->d1 == b->d1 && a->d2 == b->d2 && a->d3 == b->d3 && memcmp(a->d4, b->d4, 8) == 0;
}

void rt_guid_format(const rt_guid *v, char out[39])
{
    snprintf(out, 39, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", v->d1, v->d2, v->d3, v->d4[0],
             v->d4[1], v->d4[2], v->d4[3], v->d4[4], v->d4[5], v->d4[6], v->d4[7]);
}

/* HRESULT CoCreateGuid(GUID *): a random (version 4) GUID, as UuidCreate. */
void rt_CoCreateGuid(_cpu *cpu)
{
    uint8_t b[16];
    rt_guid g;
    if (ARG(0) == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    arc4random_buf(b, sizeof b);
    memcpy(&g.d1, b, 4);
    memcpy(&g.d2, b + 4, 2);
    memcpy(&g.d3, b + 6, 2);
    memcpy(g.d4, b + 8, 8);
    g.d3 = (uint16_t)((g.d3 & 0x0FFFu) | 0x4000u);
    g.d4[0] = (uint8_t)((g.d4[0] & 0x3Fu) | 0x80u);
    rt_guid_write(ARG(0), &g);
    RET(S_OK);
}

/* int StringFromGUID2(REFGUID, LPOLESTR buf, int cch): the braced upper
   case text. Returns the characters with the NUL (39), or 0 if the buffer
   is too small. */
void rt_StringFromGUID2(_cpu *cpu)
{
    uint32_t pg = ARG(0), buf = ARG(1), cch = ARG(2), i;
    char text[39];
    rt_guid g;
    if (pg == 0) rt_fatal("StringFromGUID2 with a NULL GUID");
    if ((int32_t)cch < 39 || buf == 0)
    {
        RET(0);
        return;
    }
    g = rt_guid_read(pg);
    rt_guid_format(&g, text);
    for (i = 0; i < 39; i++) wr16(buf + 2u * i, (uint16_t)(unsigned char)text[i]);
    RET(39);
}

/* ---------------------------------------------------------------- objects */

#define MAX_OBJECTS 0x100000u

/*
 * The object table.
 *
 * rt_com_this, rt_com_lookup, rt_com_addref and rt_com_release do not use
 * the COM lock, so that guest threads can call methods of runtime objects
 * at the same time.
 *
 * - objects has a fixed size, so that it does not move while a thread
 *   reads it. An entry is set once and then always holds the same
 *   rt_com_object. When an object is destroyed, its id goes to free_ids
 *   and the next object with that id uses the same rt_com_object again.
 *   An rt_com_object is never freed, so a thread that reads a stale entry
 *   reads valid host memory.
 * - refs is changed with atomic operations. 0 means that the entry has no
 *   live object. rt_com_create stores refs = 1 with release order after it
 *   sets the other fields, and find_slot loads refs with acquire order
 *   before it reads them.
 * - cls and guest are read and written with atomic operations, because a
 *   lookup with a stale guest pointer can read them while rt_com_create
 *   sets them for a new object.
 * The COM lock protects nobjects (written), free_ids, nfree and the guest
 * vtable copies.
 */
static pthread_mutex_t com_lock = PTHREAD_MUTEX_INITIALIZER;
static rt_com_object *objects[MAX_OBJECTS]; /* index = id; entry 0 is not used */
static uint32_t nobjects;
static uint32_t *free_ids;
static uint32_t nfree, cfree;

static void lock(void)
{
    if (pthread_mutex_lock(&com_lock) != 0) rt_fatal("COM lock failed");
}

static void unlock(void)
{
    if (pthread_mutex_unlock(&com_lock) != 0) rt_fatal("COM unlock failed");
}

/* The guest copy of a vtable. The COM lock is held. */
static uint32_t vtable_guest(rt_com_vtable *vt)
{
    uint32_t i, g;
    if (vt->guest != 0) return vt->guest;
    if (vt->count == 0 || vt->count > 1024) rt_fatal("COM vtable %s has %u entries", vt->interface, vt->count);
    g = rt_heap_alloc(4u * vt->count);
    if (g == 0) rt_fatal("no guest memory for the COM vtable %s", vt->interface);
    for (i = 0; i < vt->count; i++) wr32(g + 4u * i, H2G(vt->thunks[i]));
    __atomic_store_n(&vt->guest, g, __ATOMIC_RELEASE);
    return g;
}

/* A new object id. The COM lock is held. Returns 0 if none is left. */
static uint32_t new_id(void)
{
    uint32_t n;
    if (nfree > 0) return free_ids[--nfree];
    n = nobjects == 0 ? 1u : nobjects;
    if (n >= MAX_OBJECTS) return 0;
    /* free_ids must hold every id that is in use, so that a release never
       needs to allocate. */
    if (n >= cfree)
    {
        uint32_t nc = cfree ? cfree * 2u : 256u;
        uint32_t *nf;
        if (nc > MAX_OBJECTS) nc = MAX_OBJECTS;
        nf = realloc(free_ids, (size_t)nc * sizeof(*nf));
        if (nf == NULL) rt_fatal("out of host memory");
        free_ids = nf;
        cfree = nc;
    }
    __atomic_store_n(&nobjects, n + 1u, __ATOMIC_RELEASE);
    return n;
}

rt_com_object *rt_com_create(const rt_com_class *cls, void *data)
{
    rt_com_object *o;
    uint32_t g, id;
    int k;
    if (cls->nslots < 1 || cls->nslots > 4) rt_fatal("COM class %s has %d slots", cls->name, cls->nslots);
    g = rt_heap_alloc(4u + 4u * (uint32_t)cls->nslots);
    if (g == 0) return NULL;
    lock();
    id = new_id();
    if (id == 0)
    {
        unlock();
        rt_fatal("more than %u runtime COM objects", MAX_OBJECTS - 1u);
    }
    o = objects[id];
    if (o == NULL)
    {
        o = calloc(1, sizeof(*o));
        if (o == NULL)
        {
            unlock();
            rt_fatal("out of host memory");
        }
        o->id = id;
    }
    __atomic_store_n(&o->cls, cls, __ATOMIC_RELAXED);
    __atomic_store_n(&o->guest, g, __ATOMIC_RELAXED);
    o->data = data;
    wr32(g, id);
    for (k = 0; k < cls->nslots; k++) wr32(g + 4u + 4u * (uint32_t)k, vtable_guest(cls->vtables[k]));
    /* Publish the object: the release store makes the fields above visible
       to a thread that loads refs or the entry with acquire order. */
    __atomic_store_n(&o->refs, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&objects[id], o, __ATOMIC_RELEASE);
    unlock();
    return o;
}

uint32_t rt_com_ptr(const rt_com_object *o, int slot)
{
    if (slot < 0 || slot >= o->cls->nslots) rt_fatal("COM class %s has no slot %d", o->cls->name, slot);
    return o->guest + 4u + 4u * (uint32_t)slot;
}

/* The live object whose slot k has interface pointer p, or NULL. The COM
   lock is not needed (see the object table). */
static rt_com_object *find_slot(uint32_t p, int k)
{
    uint32_t block, id;
    rt_com_object *o;
    const rt_com_class *cls;
    if (p < 4u + 4u * (uint32_t)k) return NULL;
    block = p - 4u - 4u * (uint32_t)k;
    if (!rt_va_is_committed(block, 4u + 4u * (uint32_t)(k + 1))) return NULL;
    id = rd32(block);
    if (id == 0 || id >= __atomic_load_n(&nobjects, __ATOMIC_ACQUIRE)) return NULL;
    o = __atomic_load_n(&objects[id], __ATOMIC_ACQUIRE);
    if (o == NULL || __atomic_load_n(&o->refs, __ATOMIC_ACQUIRE) == 0) return NULL;
    cls = __atomic_load_n(&o->cls, __ATOMIC_RELAXED);
    if (__atomic_load_n(&o->guest, __ATOMIC_RELAXED) != block || k >= cls->nslots) return NULL;
    if (rd32(p) != __atomic_load_n(&cls->vtables[k]->guest, __ATOMIC_ACQUIRE)) return NULL;
    return o;
}

rt_com_object *rt_com_this(uint32_t this_ptr, int slot, const rt_com_class *cls)
{
    rt_com_object *o = find_slot(this_ptr, slot);
    if (o == NULL || (cls != NULL && o->cls != cls))
    {
        rt_fatal("0x%08x is not interface slot %d of a runtime %s object", this_ptr, slot,
                 cls != NULL ? cls->name : "COM");
    }
    return o;
}

rt_com_object *rt_com_lookup(uint32_t this_ptr)
{
    rt_com_object *o = NULL;
    int k;
    for (k = 0; k < 4 && o == NULL; k++) o = find_slot(this_ptr, k);
    return o;
}

void rt_com_addref(rt_com_object *o)
{
    uint32_t r = __atomic_load_n(&o->refs, __ATOMIC_RELAXED);
    do
    {
        if (r == 0) rt_fatal("COM AddRef of a destroyed %s object", o->cls->name);
        if (r == UINT32_MAX) rt_fatal("COM reference count overflow (%s)", o->cls->name);
    } while (!__atomic_compare_exchange_n(&o->refs, &r, r + 1u, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

uint32_t rt_com_release(rt_com_object *o)
{
    uint32_t r = __atomic_load_n(&o->refs, __ATOMIC_RELAXED);
    do
    {
        if (r == 0) rt_fatal("COM release of a destroyed %s object", o->cls->name);
    } while (!__atomic_compare_exchange_n(&o->refs, &r, r - 1u, true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
    r--;
    if (r == 0)
    {
        uint32_t g = o->guest;
        if (o->cls->destroy != NULL) o->cls->destroy(o);
        o->data = NULL;
        /* The id word no longer names the object. */
        wr32(g, 0);
        rt_heap_free(g);
        /* The id can be used again only after the object is fully
           destroyed. The rt_com_object stays in the table for that id. */
        lock();
        free_ids[nfree++] = o->id;
        unlock();
    }
    return r;
}

uint32_t rt_com_query(rt_com_object *o, const rt_guid *iid, uint32_t ppv)
{
    int i, slot = -1;
    char text[39];
    if (ppv == 0) return E_POINTER;
    if (rt_guid_eq(iid, &IID_IUnknown)) slot = 0;
    for (i = 0; i < o->cls->nifaces && slot < 0; i++)
        if (rt_guid_eq(iid, o->cls->ifaces[i].iid)) slot = o->cls->ifaces[i].slot;
    if (slot < 0)
    {
        rt_guid_format(iid, text);
        rt_fatal("QueryInterface of the runtime %s object for interface %s, which the runtime does not implement",
                 o->cls->name, text);
    }
    rt_com_addref(o);
    wr32(ppv, rt_com_ptr(o, slot));
    return S_OK;
}

/* IUnknown of vtable slot n. */
static rt_com_object *unk_this(uint32_t p, int slot)
{
    rt_com_object *o = find_slot(p, slot);
    if (o == NULL) rt_fatal("IUnknown call on 0x%08x, which is not slot %d of a runtime COM object", p, slot);
    return o;
}

void rt_com_unk0_QueryInterface(_cpu *cpu)
{
    rt_com_object *o = unk_this(ARG(0), 0);
    rt_guid iid;
    if (ARG(1) == 0) rt_fatal("QueryInterface with a NULL IID");
    iid = rt_guid_read(ARG(1));
    RET(rt_com_query(o, &iid, ARG(2)));
}

void rt_com_unk0_AddRef(_cpu *cpu)
{
    rt_com_object *o = unk_this(ARG(0), 0);
    rt_com_addref(o);
    RET(__atomic_load_n(&o->refs, __ATOMIC_RELAXED));
}

void rt_com_unk0_Release(_cpu *cpu)
{
    RET(rt_com_release(unk_this(ARG(0), 0)));
}

/* ---------------------------------------------------------------- class creation */

rt_com_object *rt_wmi_create_locator(void);

typedef struct com_class_entry {
    rt_guid clsid;
    const char *name;
    rt_com_object *(*create)(void); /* NULL: not implemented by the runtime */
} com_class_entry;

static const com_class_entry classes[] = {
    { { 0x4590F811, 0x1D3A, 0x11D0, { 0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24 } }, "WbemLocator", rt_wmi_create_locator },
    { { 0x62BE5D10, 0x60EB, 0x11D0, { 0xBD, 0x3B, 0x00, 0xA0, 0xC9, 0x11, 0xCE, 0x86 } }, "SystemDeviceEnum (DirectShow)", NULL },
    { { 0xE436EBB3, 0x524F, 0x11CE, { 0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70 } }, "FilterGraph (DirectShow)", NULL },
    { { 0xBF87B6E1, 0x8C27, 0x11D0, { 0xB3, 0xF0, 0x00, 0xAA, 0x00, 0x37, 0x61, 0xC5 } }, "CaptureGraphBuilder2 (DirectShow)", NULL },
    { { 0xC1F400A0, 0x3F08, 0x11D3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } }, "SampleGrabber (DirectShow)", NULL },
    { { 0xC1F400A4, 0x3F08, 0x11D3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } }, "NullRenderer (DirectShow)", NULL },
};

/* HRESULT CoCreateInstance(REFCLSID, IUnknown *outer, DWORD context,
   REFIID, void **ppv) */
void rt_CoCreateInstance(_cpu *cpu)
{
    uint32_t pclsid = ARG(0), outer = ARG(1), ctx = ARG(2), piid = ARG(3), ppv = ARG(4);
    rt_guid clsid, iid;
    char text[39];
    size_t i;
    if (ppv == 0)
    {
        RET(E_POINTER);
        return;
    }
    wr32(ppv, 0);
    if (pclsid == 0 || piid == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    if (com_count == 0)
    {
        RET(CO_E_NOTINITIALIZED);
        return;
    }
    clsid = rt_guid_read(pclsid);
    iid = rt_guid_read(piid);
    rt_guid_format(&clsid, text);
    for (i = 0; i < sizeof classes / sizeof classes[0]; i++)
    {
        rt_com_object *o;
        uint32_t r;
        if (!rt_guid_eq(&clsid, &classes[i].clsid)) continue;
        if (classes[i].create == NULL)
            rt_fatal("CoCreateInstance of class %s %s, which the runtime does not implement", classes[i].name, text);
        if (outer != 0)
        {
            RET(CLASS_E_NOAGGREGATION);
            return;
        }
        if ((ctx & CLSCTX_INPROC_SERVER) == 0)
            rt_fatal("CoCreateInstance of %s with context 0x%x (no in-process server) is not supported", classes[i].name, ctx);
        o = classes[i].create();
        if (o == NULL)
        {
            RET(E_OUTOFMEMORY);
            return;
        }
        r = rt_com_query(o, &iid, ppv);
        rt_com_release(o);
        RET(r);
        return;
    }
    rt_fatal("CoCreateInstance of class %s, which the runtime does not implement", text);
}

/* HRESULT CoSetProxyBlanket(IUnknown *proxy, DWORD authn, DWORD authz,
   OLECHAR *principal, DWORD authn_level, DWORD imp_level, void *auth_info,
   DWORD capabilities). On Windows the WMI services object is a proxy of
   the WMI service process, and the call sets its security. The runtime
   WMI services object is in-process; it accepts the call. Every other
   runtime object is an in-process object without IClientSecurity. */
void rt_CoSetProxyBlanket(_cpu *cpu)
{
    rt_com_object *o = rt_com_lookup(ARG(0));
    if (o == NULL) rt_fatal("CoSetProxyBlanket on 0x%08x, which is not a runtime COM object", ARG(0));
    if (o->cls->vtables[0] == &rt_vt_WbemServices)
    {
        RET(S_OK);
        return;
    }
    RET(E_NOINTERFACE);
}
