/* Code addresses of address-preserving translated modules (see rt_code.h). */
#include "rt_code.h"
#include "rt_thread.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_CODE_MODULES 64
/* Index of the modules by 64 KiB granule of the guest window. Each image
   starts on a granule boundary, so two images never share a granule. */
#define CODE_GRANULE_SHIFT 16

typedef struct code_module {
    char name[64];
    uint32_t image_base, image_size;
    uint32_t code_lo;      /* lowest entry address */
    uint32_t code_span;    /* highest entry address - code_lo + 1 */
    uint32_t *table;       /* code_span entries: translated procedure or 0 */
} code_module;

static code_module modules[MAX_CODE_MODULES];
/* Number of registered modules. A module is complete before the count that
   includes it is published (release); readers load the count (acquire). */
static atomic_uint nmodules;
/* For each granule: the index + 1 of the module whose image holds it, or 0.
   A module is complete before its granules are stored (release); module_of
   loads a granule entry with acquire. */
static _Atomic uint8_t granule_module[RT_WINDOW_SIZE >> CODE_GRANULE_SHIFT];
_Static_assert(MAX_CODE_MODULES < 256, "granule_module keeps module index + 1 in 8 bits");
static pthread_mutex_t register_lock = PTHREAD_MUTEX_INITIALIZER;

#define MAX_CODE_COPIES 8

typedef struct code_copy {
    char name[64];
    uint32_t template_addr, len, proc;
} code_copy;

/* Registered code copies; published as the modules (count with release,
   read with acquire). */
static code_copy copies[MAX_CODE_COPIES];
static atomic_uint ncopies;

static uint32_t map_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void rt_code_register(const char *name, uint32_t image_base, uint32_t image_size, const uint8_t *map)
{
    uint32_t count, i, prev = 0, lo, hi;
    unsigned n, j;
    code_module *m;

    if (image_size == 0 || (uint64_t)image_base + image_size > RT_WINDOW_SIZE)
        rt_fatal("code map of %s: bad image range 0x%08x + 0x%x", name, image_base, image_size);
    if ((image_base & ((1u << CODE_GRANULE_SHIFT) - 1u)) != 0)
        rt_fatal("code map of %s: image base 0x%08x is not 64 KiB aligned", name, image_base);
    count = map_u32(map);
    if (count == 0) rt_fatal("code map of %s has no entries", name);
    lo = map_u32(map + 4);
    hi = map_u32(map + 4 + 8u * (uint64_t)(count - 1));
    for (i = 0; i < count; i++)
    {
        uint32_t a = map_u32(map + 4 + 8u * (uint64_t)i);
        uint32_t p = map_u32(map + 8 + 8u * (uint64_t)i);
        if (i != 0 && a <= prev) rt_fatal("code map of %s: entry %u (0x%08x) is not above 0x%08x", name, i, a, prev);
        if (a - image_base >= image_size) rt_fatal("code map of %s: entry 0x%08x is outside the image", name, a);
        if (!rt_guest_in_program_image(p))
            rt_fatal("code map of %s: the procedure of 0x%08x (0x%08x) is outside the program image", name, a, p);
        prev = a;
    }

    if (pthread_mutex_lock(&register_lock) != 0) rt_fatal("code map lock failed");
    n = atomic_load_explicit(&nmodules, memory_order_relaxed);
    if (n >= MAX_CODE_MODULES) rt_fatal("more than %d translated modules", MAX_CODE_MODULES);
    for (j = 0; j < n; j++)
    {
        const code_module *o = &modules[j];
        if (image_base < o->image_base + o->image_size && o->image_base < image_base + image_size)
            rt_fatal("the image of %s overlaps the image of %s", name, o->name);
    }
    m = &modules[n];
    if ((size_t)snprintf(m->name, sizeof m->name, "%s", name) >= sizeof m->name) rt_fatal("module name %s is too long", name);
    m->image_base = image_base;
    m->image_size = image_size;
    m->code_lo = lo;
    m->code_span = hi - lo + 1;
    m->table = calloc(m->code_span, sizeof *m->table);
    if (m->table == NULL) rt_fatal("out of host memory for the code map of %s", name);
    for (i = 0; i < count; i++)
        m->table[map_u32(map + 4 + 8u * (uint64_t)i) - lo] = map_u32(map + 8 + 8u * (uint64_t)i);
    for (i = image_base >> CODE_GRANULE_SHIFT; i <= (image_base + image_size - 1u) >> CODE_GRANULE_SHIFT; i++)
        atomic_store_explicit(&granule_module[i], (uint8_t)(n + 1u), memory_order_release);
    atomic_store_explicit(&nmodules, n + 1, memory_order_release);
    if (pthread_mutex_unlock(&register_lock) != 0) rt_fatal("code map unlock failed");
}

/* The module whose image holds the address, or NULL. */
static const code_module *module_of(uint32_t address)
{
    const code_module *m;
    unsigned k;
    if (address >= RT_WINDOW_SIZE) return NULL;
    k = atomic_load_explicit(&granule_module[address >> CODE_GRANULE_SHIFT], memory_order_acquire);
    if (k == 0) return NULL;
    m = &modules[k - 1u];
    return address - m->image_base < m->image_size ? m : NULL;
}

void rt_code_register_copy(const char *name, uint32_t template_addr, uint32_t len, uint32_t proc)
{
    unsigned n;
    code_copy *c;
    if (len == 0 || !rt_va_is_committed(template_addr, len)) rt_fatal("code copy %s: the template is not mapped", name);
    if (pthread_mutex_lock(&register_lock) != 0) rt_fatal("code register lock failed");
    n = atomic_load_explicit(&ncopies, memory_order_relaxed);
    if (n == MAX_CODE_COPIES) rt_fatal("too many code copies");
    c = &copies[n];
    if ((size_t)snprintf(c->name, sizeof(c->name), "%s", name) >= sizeof(c->name)) rt_fatal("code copy name too long: %s", name);
    c->template_addr = template_addr;
    c->len = len;
    c->proc = proc;
    atomic_store_explicit(&ncopies, n + 1, memory_order_release);
    if (pthread_mutex_unlock(&register_lock) != 0) rt_fatal("code register unlock failed");
}

/* The runtime procedure for a copy of registered template code at address,
   or 0. */
static uint32_t copy_target(uint32_t address)
{
    unsigned n = atomic_load_explicit(&ncopies, memory_order_acquire), i;
    for (i = 0; i < n; i++)
    {
        const code_copy *c = &copies[i];
        if (rt_va_is_committed(address, c->len) && memcmp(G2H(address), G2H(c->template_addr), c->len) == 0) return c->proc;
    }
    return 0;
}

uint32_t rt_code_find(uint32_t address)
{
    const code_module *m = module_of(address);
    if (m == NULL || address - m->code_lo >= m->code_span) return 0;
    return m->table[address - m->code_lo];
}

uint32_t rt_code_target(uint32_t address)
{
    const code_module *m = module_of(address);
    if (m != NULL)
    {
        uint32_t p = (address - m->code_lo < m->code_span) ? m->table[address - m->code_lo] : 0;
        if (p == 0)
            rt_fatal("control transfer to 0x%08x in %s: the address is not a translated procedure entry", address, m->name);
        return p;
    }
    if (!rt_guest_in_program_image(address))
    {
        uint32_t p = copy_target(address);
        if (p != 0) return p;
        rt_fatal("control transfer to 0x%08x: the address is not in a translated module or in the program", address);
    }
    return address;
}
