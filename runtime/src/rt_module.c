/*
 * Modules and KERNEL32 module functions: LoadLibraryA/W, FreeLibrary,
 * GetModuleHandleW, GetModuleFileNameA/W, GetProcAddress, and process exit.
 *
 * Modules:
 *   the program        a minimal image header at a free guest address. The
 *                      host program sets its guest path (rt_modules_init).
 *   translated modules PE files of the game, translated by SRW with
 *                      --preserve_addresses. rt_module_load_pe maps the
 *                      player's file at its original image base, fills its
 *                      import address table with the runtime thunks and
 *                      registers its code map. The handle is the image
 *                      base; GetProcAddress gives the original export
 *                      addresses.
 *   system DLLs        the DLLs of imports.spec. They are always loaded.
 *                      GetProcAddress finds the functions that the runtime
 *                      implements. The handle is the guest address of a
 *                      minimal image header.
 * Every other DLL name gives ERROR_MOD_NOT_FOUND: the DLL is not in the
 * emulated system.
 */
#include "rt_module.h"
#include "rt_code.h"
#include "rt_file.h"
#include "rt_object.h"
#include "rt_text.h"
#include "rt_thread.h"

#include <CommonCrypto/CommonDigest.h>
#include <ctype.h>
#include <errno.h>
#include <mach/mach.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define DLL_PROCESS_DETACH 0u
#define DLL_PROCESS_ATTACH 1u
#define DLL_THREAD_ATTACH 2u
#define DLL_THREAD_DETACH 3u

#define MAX_NAME 64

typedef enum { MOD_EXE, MOD_PE, MOD_SYSTEM } mod_kind;

typedef struct rt_module {
    char name[MAX_NAME];        /* lower case file name, for example "kernel32.dll" */
    char key[MAX_NAME];         /* imports.spec DLL name, for example "kernel32" */
    char path[RT_PATH_BYTES];   /* guest path (UTF-8), for example "C:\\Windows\\system32\\kernel32.dll" */
    mod_kind kind;
    uint32_t base;              /* HMODULE */
    uint32_t size;              /* SizeOfImage */
    uint32_t entry;             /* original address of DllMain, 0 if none */
    /* Export directory of a translated module (guest addresses). */
    uint32_t exp_ordinal_base, exp_nfunc, exp_nname, exp_func, exp_name, exp_ord;
    int refs;                   /* LoadLibrary references (translated DLL) */
    bool attached;              /* DLL_PROCESS_ATTACH done */
    bool no_thread_calls;       /* DisableThreadLibraryCalls: no DLL_THREAD_* calls */
} rt_module;

#define MAX_MODULES 64

static pthread_mutex_t loader_lock;
static rt_module modules[MAX_MODULES];
static int nmodules;
static int load_order[MAX_MODULES];
static int nloaded;
static atomic_bool exiting;
static bool program_mapped; /* rt_module_load_program done */

static void loader_enter(void)
{
    if (pthread_mutex_lock(&loader_lock) != 0) rt_fatal("loader lock failed");
}

static void loader_leave(void)
{
    if (pthread_mutex_unlock(&loader_lock) != 0) rt_fatal("loader unlock failed");
}

/* A minimal PE header for the program or a system DLL: no sections, no
   directories. The header takes the highest free range below 0x80000000,
   where Windows also puts the system DLLs, so that it does not take the
   fixed base of a translated image (for example mono.dll at 0x10000000). */
static uint32_t make_minimal_image(bool dll)
{
    uint32_t base = rt_va_reserve_top(RT_GRANULE), pe;
    if (base == 0 || rt_va_commit(base, 0x1000) != 0) rt_fatal("no guest memory for a module header");
    pe = base + 0x40u;
    wr16(base + 0, 0x5A4D);       /* "MZ" */
    wr32(base + 0x3C, 0x40);      /* e_lfanew */
    wr32(pe + 0, 0x00004550);     /* "PE\0\0" */
    wr16(pe + 4, 0x014C);         /* i386 */
    wr16(pe + 6, 0);              /* NumberOfSections */
    wr16(pe + 20, 0xE0);          /* SizeOfOptionalHeader */
    wr16(pe + 22, dll ? 0x2102 : 0x0102); /* executable, 32-bit (, DLL) */
    wr16(pe + 24, 0x010B);        /* PE32 */
    wr32(pe + 24 + 28, base);     /* ImageBase */
    wr32(pe + 24 + 32, 0x1000);   /* SectionAlignment */
    wr32(pe + 24 + 36, 0x200);    /* FileAlignment */
    wr32(pe + 24 + 56, 0x1000);   /* SizeOfImage */
    wr32(pe + 24 + 60, 0x400);    /* SizeOfHeaders */
    wr32(pe + 24 + 92, 16);       /* NumberOfRvaAndSizes */
    return base;
}

/* Lower case file name part of a guest path. */
static void file_name_of(const char *path, char *out, size_t n)
{
    const char *p = path, *s;
    size_t i, len;
    for (s = path; *s; s++)
        if (*s == '\\' || *s == '/' || *s == ':') p = s + 1;
    len = strlen(p);
    if (len + 1 > n) rt_fatal("module file name %s is too long", p);
    for (i = 0; i <= len; i++) out[i] = (char)tolower((unsigned char)p[i]);
}

static rt_module *add_module(const char *path, const char *key, mod_kind kind)
{
    rt_module *m;
    if (nmodules >= MAX_MODULES) rt_fatal("too many modules");
    m = &modules[nmodules++];
    memset(m, 0, sizeof *m);
    if ((size_t)snprintf(m->path, sizeof(m->path), "%s", path) >= sizeof(m->path)) rt_fatal("module path %s is too long", path);
    file_name_of(path, m->name, sizeof m->name);
    if ((size_t)snprintf(m->key, sizeof(m->key), "%s", key) >= sizeof(m->key)) rt_fatal("module key too long");
    m->kind = kind;
    return m;
}

void rt_modules_init(const char *exe_guest_path)
{
    char path[RT_PATH_BYTES], key[MAX_NAME];
    pthread_mutexattr_t a;
    unsigned i;
    rt_module *m;
    char *dot;

    if (pthread_mutexattr_init(&a) != 0) rt_fatal("pthread_mutexattr_init failed");
    if (pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE) != 0) rt_fatal("pthread_mutexattr_settype failed");
    if (pthread_mutex_init(&loader_lock, &a) != 0) rt_fatal("pthread_mutex_init failed");
    pthread_mutexattr_destroy(&a);

    file_name_of(exe_guest_path, key, sizeof key);
    dot = strrchr(key, '.');
    if (dot != NULL) *dot = 0;
    m = add_module(exe_guest_path, key, MOD_EXE);
    m->base = make_minimal_image(false);
    m->size = 0x1000;
    m->attached = true;

    /* System DLLs: every DLL of the import table, and ntdll.dll (always
       loaded on Windows). */
    for (i = 0; i < rt_import_table_count; i++)
    {
        const char *dll = rt_import_table[i].dll;
        int j;
        bool found = false;
        for (j = 0; j < nmodules && !found; j++) found = modules[j].kind == MOD_SYSTEM && strcmp(modules[j].key, dll) == 0;
        if (found) continue;
        if ((size_t)snprintf(path, sizeof(path), "%s\\%s.dll", RT_GUEST_SYSTEM_DIR, dll) >= sizeof(path)) rt_fatal("DLL name too long");
        m = add_module(path, dll, MOD_SYSTEM);
        m->base = make_minimal_image(true);
        m->size = 0x1000;
        m->attached = true;
    }
    m = add_module(RT_GUEST_SYSTEM_DIR "\\ntdll.dll", "ntdll", MOD_SYSTEM);
    m->base = make_minimal_image(true);
    m->size = 0x1000;
    m->attached = true;
}

uint32_t rt_exe_module(void)
{
    return modules[0].base;
}

const char *rt_exe_guest_path(void)
{
    return modules[0].path;
}

/* ---------------------------------------------------------------- PE image mapping */

/* Upper limits for the PE files of the game. */
#define MAX_PE_FILE (256u << 20)
#define MAX_SECTIONS 96u
#define MAX_EXPORTS 65536u
#define MAX_IMPORT_NAME 512u

typedef struct pe_view {
    const uint8_t *d;
    size_t n;
} pe_view;

static bool pe_u16(const pe_view *p, size_t off, uint16_t *v)
{
    if (off > p->n || p->n - off < 2) return false;
    memcpy(v, p->d + off, 2);
    return true;
}

static bool pe_u32(const pe_view *p, size_t off, uint32_t *v)
{
    if (off > p->n || p->n - off < 4) return false;
    memcpy(v, p->d + off, 4);
    return true;
}

static uint8_t *read_host_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long end;
    if (f == NULL) rt_fatal("cannot open %s: %s", path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0 || (end = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
        rt_fatal("cannot get the size of %s", path);
    if ((unsigned long)end > MAX_PE_FILE) rt_fatal("%s is larger than %u bytes", path, MAX_PE_FILE);
    buf = malloc(end > 0 ? (size_t)end : 1);
    if (buf == NULL) rt_fatal("out of host memory");
    if (fread(buf, 1, (size_t)end, f) != (size_t)end) rt_fatal("read of %s failed", path);
    fclose(f);
    *size = (size_t)end;
    return buf;
}

static void check_sha256(const char *path, const uint8_t *d, size_t n, const char *expected)
{
    uint8_t md[CC_SHA256_DIGEST_LENGTH];
    char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
    unsigned i;
    if (n > UINT32_MAX) rt_fatal("%s is too large", path);
    CC_SHA256(d, (CC_LONG)n, md);
    for (i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", md[i]);
    if (strcmp(hex, expected) != 0)
        rt_fatal("%s has SHA-256 %s; this build supports only the file with SHA-256 %s", path, hex, expected);
}

/* Is the guest range [rva, rva + size) inside the image of m? */
static bool in_image(const rt_module *m, uint32_t rva, uint32_t size)
{
    return rva <= m->size && size <= m->size - rva;
}

/* The NUL-terminated string at an RVA of the image, with at most max-1
   characters, or NULL. */
static const char *image_str(const rt_module *m, uint32_t rva, size_t max)
{
    const char *s;
    size_t len;
    if (rva >= m->size) return NULL;
    s = G2H(m->base + rva);
    len = strnlen(s, m->size - rva < max ? m->size - rva : max);
    if (len == m->size - rva || len >= max) return NULL;
    return s;
}

static const rt_import_entry *find_import(const char *dll, const char *name, uint32_t ordinal)
{
    unsigned i;
    for (i = 0; i < rt_import_table_count; i++)
    {
        const rt_import_entry *e = &rt_import_table[i];
        if (strcmp(e->dll, dll) != 0) continue;
        if (name != NULL ? (e->name != NULL && strcmp(e->name, name) == 0) : (e->name == NULL && e->ordinal == ordinal)) return e;
    }
    return NULL;
}

/* Write the runtime thunk address of every import into the import address
   table. An import that the runtime does not implement stops the process. */
static void bind_imports(rt_module *m, uint32_t imp_rva, uint32_t imp_size)
{
    uint32_t d, missing = 0;
    if (imp_rva == 0) return;
    if (!in_image(m, imp_rva, imp_size)) rt_fatal("%s: bad import directory", m->name);
    for (d = imp_rva;; d += 20)
    {
        uint32_t lookup, name_rva, iat, slot, entry;
        const char *dll_name;
        char dll[MAX_NAME];
        size_t len, i;

        if (!in_image(m, d, 20)) rt_fatal("%s: the import directory has no end", m->name);
        lookup = rd32(m->base + d);
        name_rva = rd32(m->base + d + 12);
        iat = rd32(m->base + d + 16);
        if (lookup == 0 && name_rva == 0 && iat == 0) break;
        dll_name = image_str(m, name_rva, MAX_NAME);
        if (dll_name == NULL) rt_fatal("%s: bad import DLL name", m->name);
        len = strlen(dll_name);
        if (len > 4 && strcasecmp(dll_name + len - 4, ".dll") == 0) len -= 4;
        for (i = 0; i < len; i++) dll[i] = (char)tolower((unsigned char)dll_name[i]);
        dll[len] = 0;
        if (lookup == 0) lookup = iat;
        for (slot = 0;; slot++)
        {
            const rt_import_entry *e;
            const char *name = NULL;
            uint32_t ordinal = 0;
            if (!in_image(m, lookup + 4u * slot, 4) || !in_image(m, iat + 4u * slot, 4))
                rt_fatal("%s: bad import table of %s", m->name, dll_name);
            entry = rd32(m->base + lookup + 4u * slot);
            if (entry == 0) break;
            if (entry & 0x80000000u)
                ordinal = entry & 0xFFFFu;
            else if ((name = image_str(m, entry + 2u, MAX_IMPORT_NAME)) == NULL)
                rt_fatal("%s: bad import name in the table of %s", m->name, dll_name);
            e = find_import(dll, name, ordinal);
            if (e == NULL)
            {
                missing++;
                continue;
            }
            wr32(m->base + iat + 4u * slot, H2G(e->addr));
        }
    }
    if (missing != 0) rt_fatal("%s: %u imports are not in runtime/imports.spec", m->name, missing);
}

/* Read and check the export directory. Forwarded exports are not
   supported. */
static void read_exports(rt_module *m, uint32_t exp_rva, uint32_t exp_size)
{
    uint32_t e, i;
    if (exp_rva == 0) return;
    if (!in_image(m, exp_rva, exp_size) || exp_size < 40) rt_fatal("%s: bad export directory", m->name);
    e = m->base + exp_rva;
    m->exp_ordinal_base = rd32(e + 16);
    m->exp_nfunc = rd32(e + 20);
    m->exp_nname = rd32(e + 24);
    m->exp_func = rd32(e + 28);
    m->exp_name = rd32(e + 32);
    m->exp_ord = rd32(e + 36);
    if (m->exp_nfunc > MAX_EXPORTS || m->exp_nname > m->exp_nfunc || !in_image(m, m->exp_func, 4u * m->exp_nfunc) ||
        !in_image(m, m->exp_name, 4u * m->exp_nname) || !in_image(m, m->exp_ord, 2u * m->exp_nname))
        rt_fatal("%s: bad export directory tables", m->name);
    for (i = 0; i < m->exp_nfunc; i++)
    {
        uint32_t rva = rd32(m->base + m->exp_func + 4u * i);
        if (rva == 0) continue;
        if (rva >= exp_rva && rva - exp_rva < exp_size)
            rt_fatal("%s: the forwarded export at ordinal %u is not supported", m->name, m->exp_ordinal_base + i);
        if (rva >= m->size) rt_fatal("%s: export ordinal %u is outside the image", m->name, m->exp_ordinal_base + i);
    }
    for (i = 0; i < m->exp_nname; i++)
    {
        if (rd16(m->base + m->exp_ord + 2u * i) >= m->exp_nfunc) rt_fatal("%s: export name %u has a bad index", m->name, i);
        if (image_str(m, rd32(m->base + m->exp_name + 4u * i), 1024) == NULL) rt_fatal("%s: bad export name %u", m->name, i);
    }
}

/* Original address of an export, or 0. */
static uint32_t export_address(const rt_module *m, const char *name, uint32_t ordinal)
{
    uint32_t i, idx;
    if (name == NULL)
    {
        if (ordinal < m->exp_ordinal_base || ordinal - m->exp_ordinal_base >= m->exp_nfunc) return 0;
        idx = ordinal - m->exp_ordinal_base;
    }
    else
    {
        for (i = 0; i < m->exp_nname; i++)
            if (strcmp(G2H(m->base + rd32(m->base + m->exp_name + 4u * i)), name) == 0) break;
        if (i == m->exp_nname) return 0;
        idx = rd16(m->base + m->exp_ord + 2u * i);
    }
    i = rd32(m->base + m->exp_func + 4u * idx);
    return i == 0 ? 0 : m->base + i;
}

/* Apply the base relocations of an image that is at load_base instead of
   its preferred base. Only IMAGE_REL_BASED_HIGHLOW (and the padding entry
   IMAGE_REL_BASED_ABSOLUTE) is supported. The image is writable. */
static void relocate(const char *host_path, uint32_t load_base, uint32_t image_size, uint32_t rel_rva, uint32_t rel_size,
                     uint32_t delta)
{
    uint32_t off = 0;
    if (rel_rva > image_size || rel_size > image_size - rel_rva) rt_fatal("%s: bad base relocation directory", host_path);
    while (off < rel_size)
    {
        uint32_t page, block, i;
        if (rel_size - off < 8) rt_fatal("%s: truncated base relocation block", host_path);
        page = rd32(load_base + rel_rva + off);
        block = rd32(load_base + rel_rva + off + 4);
        if (block < 8 || (block & 1u) != 0 || block > rel_size - off) rt_fatal("%s: bad base relocation block size", host_path);
        for (i = 8; i < block; i += 2)
        {
            uint32_t e = rd16(load_base + rel_rva + off + i), type = e >> 12, rva = page + (e & 0xFFFu);
            if (type == 0) continue; /* IMAGE_REL_BASED_ABSOLUTE */
            if (type != 3) rt_fatal("%s: base relocation type %u is not supported", host_path, type);
            if (rva < page || rva > image_size - 4u) rt_fatal("%s: base relocation outside the image", host_path);
            wr32(load_base + rva, rd32(load_base + rva) + delta);
        }
        off += block;
    }
}

/* Map a translated PE file (see rt_module_load_pe). program: the file is
   the program (not a DLL), and it replaces the minimal program module.
   load_base: the guest address of the translation, 0 for the preferred
   base. */
static uint32_t map_image(const char *guest_path, const char *host_path, const char *sha256, const uint8_t *code_map,
                          uint32_t load_base, bool program)
{
    pe_view p;
    uint8_t *file;
    uint32_t pe, sig, image_base, image_size, headers_size, sec_align, entry_rva, ndirs, dir_rva[16], dir_size[16];
    uint16_t machine, nsec, opt_size, magic, characteristics;
    size_t opt;
    unsigned i;
    rt_module *m;
    char key[MAX_NAME];
    char *dot;

    memset(&p, 0, sizeof p);
    file = read_host_file(host_path, &p.n);
    p.d = file;
    check_sha256(host_path, file, p.n, sha256);
    if (p.n < 0x40 || file[0] != 'M' || file[1] != 'Z' || !pe_u32(&p, 0x3C, &pe) || !pe_u32(&p, pe, &sig) || sig != 0x00004550u)
        rt_fatal("%s is not a PE file", host_path);
    if (!pe_u16(&p, (size_t)pe + 4, &machine) || !pe_u16(&p, (size_t)pe + 6, &nsec) ||
        !pe_u16(&p, (size_t)pe + 20, &opt_size) || !pe_u16(&p, (size_t)pe + 22, &characteristics))
        rt_fatal("%s: bad PE header", host_path);
    opt = (size_t)pe + 24;
    if (machine != 0x014C || !pe_u16(&p, opt, &magic) || magic != 0x10B) rt_fatal("%s is not a 32-bit x86 PE file", host_path);
    if (!pe_u32(&p, opt + 16, &entry_rva) || !pe_u32(&p, opt + 28, &image_base) || !pe_u32(&p, opt + 32, &sec_align) ||
        !pe_u32(&p, opt + 56, &image_size) || !pe_u32(&p, opt + 60, &headers_size) || !pe_u32(&p, opt + 92, &ndirs))
        rt_fatal("%s: bad optional header", host_path);
    if (nsec > MAX_SECTIONS || sec_align != 0x1000 || (image_base & (RT_GRANULE - 1)) != 0 || image_size == 0 ||
        headers_size > image_size || headers_size > p.n)
        rt_fatal("%s: unsupported image layout", host_path);
    memset(dir_rva, 0, sizeof dir_rva);
    memset(dir_size, 0, sizeof dir_size);
    for (i = 0; i < ndirs && i < 16; i++)
        if (!pe_u32(&p, opt + 96 + 8u * i, &dir_rva[i]) || !pe_u32(&p, opt + 100 + 8u * i, &dir_size[i]))
            rt_fatal("%s: bad data directory", host_path);
    /* Directories that the loader would have to process. The load
       configuration (security cookie, safe exception handler table) needs
       no loader work. */
    if (dir_rva[9] != 0) rt_fatal("%s: thread local storage directories are not supported", host_path);
    if (dir_rva[13] != 0) rt_fatal("%s: delay-load imports are not supported", host_path);
    if (dir_rva[14] != 0) rt_fatal("%s: .NET images are not supported", host_path);
    if (((characteristics & 0x2000u) == 0) != program)
        rt_fatal(program ? "%s is a DLL, not a program file" : "%s is not a DLL", host_path);
    if (load_base == 0) load_base = image_base;
    if ((load_base & (RT_GRANULE - 1)) != 0) rt_fatal("%s: load address 0x%08x is not granule aligned", host_path, load_base);
    if (load_base != image_base && ((characteristics & 1u) != 0 || dir_rva[5] == 0))
        rt_fatal("%s: the file has no base relocations, so it cannot be at 0x%08x", host_path, load_base);

    /* The translated code uses the addresses of the translation, so the
       image must be at load_base. */
    if (rt_va_reserve(load_base, image_size) != load_base)
        rt_fatal("%s: the guest range 0x%08x-0x%08x of the image is not free", host_path, load_base, load_base + image_size - 1);
    if (rt_va_commit(load_base, image_size) != 0) rt_fatal("%s: commit of the image failed", host_path);
    memcpy(G2H(load_base), file, headers_size);
    for (i = 0; i < nsec; i++)
    {
        size_t o = opt + opt_size + 40u * i;
        uint32_t vsize, va, rsize, rptr, n;
        if (!pe_u32(&p, o + 8, &vsize) || !pe_u32(&p, o + 12, &va) || !pe_u32(&p, o + 16, &rsize) || !pe_u32(&p, o + 20, &rptr))
            rt_fatal("%s: bad section table", host_path);
        n = rsize < vsize ? rsize : vsize;
        if (vsize == 0) n = rsize;
        if (va > image_size || n > image_size - va || (n != 0 && (rptr > p.n || n > p.n - rptr)))
            rt_fatal("%s: section %u is outside the image or the file", host_path, i);
        if (n != 0) memcpy(G2H(load_base + va), file + rptr, n);
    }
    if (load_base != image_base)
    {
        /* As the Windows loader does: apply the relocations and write the
           actual base to OptionalHeader.ImageBase. */
        relocate(host_path, load_base, image_size, dir_rva[5], dir_size[5], load_base - image_base);
        wr32(load_base + (uint32_t)opt + 28u, load_base);
        image_base = load_base;
    }
    /* VirtualQuery shows the image as on Windows: an image allocation,
       read-only headers, and the protection of each section from its
       characteristics (written data sections as PAGE_READWRITE). */
    rt_va_set_alloc_info(image_base, RT_MEM_IMAGE, RT_PAGE_EXECUTE_WRITECOPY);
    {
        uint32_t old;
        if (rt_va_protect(image_base, headers_size, RT_PAGE_READONLY, &old) != 0) rt_fatal("%s: protection of the headers failed", host_path);
    }
    for (i = 0; i < nsec; i++)
    {
        size_t o = opt + opt_size + 40u * i;
        uint32_t vsize, va, ch, prot, old, len;
        bool x, r, w;
        if (!pe_u32(&p, o + 8, &vsize) || !pe_u32(&p, o + 12, &va) || !pe_u32(&p, o + 36, &ch))
            rt_fatal("%s: bad section table", host_path);
        x = (ch & 0x20000000u) != 0;
        r = (ch & 0x40000000u) != 0;
        w = (ch & 0x80000000u) != 0;
        prot = w ? (x ? RT_PAGE_EXECUTE_READWRITE : RT_PAGE_READWRITE)
                 : x ? (r ? RT_PAGE_EXECUTE_READ : RT_PAGE_EXECUTE)
                     : r ? RT_PAGE_READONLY : RT_PAGE_NOACCESS;
        len = vsize != 0 ? vsize : 1u;
        if (va >= image_size || len > image_size - va) rt_fatal("%s: section %u is outside the image", host_path, i);
        if (rt_va_protect(image_base + va, len, prot, &old) != 0) rt_fatal("%s: protection of section %u failed", host_path, i);
    }
    free(file);

    file_name_of(guest_path, key, sizeof key);
    dot = strrchr(key, '.');
    if (dot != NULL) *dot = 0;
    loader_enter();
    if (program)
    {
        /* The program file replaces the minimal program module of
           rt_modules_init: the same guest path, module 0. */
        m = &modules[0];
        if (m->kind != MOD_EXE || program_mapped || strcasecmp(m->path, guest_path) != 0)
            rt_fatal("%s: the program module is %s, not %s", host_path, m->path, guest_path);
        if (rt_va_release(m->base) != 0) rt_fatal("release of the minimal program header failed");
        program_mapped = true;
    }
    else
        m = add_module(guest_path, key, MOD_PE);
    m->base = image_base;
    m->size = image_size;
    m->entry = entry_rva != 0 ? image_base + entry_rva : 0;
    read_exports(m, dir_rva[0], dir_size[0]);
    bind_imports(m, dir_rva[1], dir_size[1]);
    loader_leave();
    rt_code_register(m->name, image_base, image_size, code_map);
    return image_base;
}

uint32_t rt_module_load_pe_at(const char *guest_path, const char *host_path, const char *sha256, const uint8_t *code_map,
                              uint32_t load_base)
{
    return map_image(guest_path, host_path, sha256, code_map, load_base, false);
}

uint32_t rt_module_load_pe(const char *guest_path, const char *host_path, const char *sha256, const uint8_t *code_map)
{
    return map_image(guest_path, host_path, sha256, code_map, 0, false);
}

uint32_t rt_module_load_program(const char *guest_path, const char *host_path, const char *sha256, const uint8_t *code_map,
                                uint32_t load_base)
{
    uint32_t base = map_image(guest_path, host_path, sha256, code_map, load_base, true);
    if (base != load_base) rt_fatal("%s: the program is at 0x%08x, not at 0x%08x", host_path, base, load_base);
    return base;
}

/* ---------------------------------------------------------------- lookup */

/* Normalize a DLL name: the file name part, lower case, ".dll" added when
   the name has no extension. Returns false if the name is too long. */
static bool normalize(const char *in, char *out, size_t n)
{
    const char *p = in, *s;
    size_t len, i;
    bool has_dot = false;
    for (s = in; *s; s++)
    {
        if (*s == '\\' || *s == '/' || *s == ':') p = s + 1;
    }
    len = strlen(p);
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '.'))
    {
        /* A trailing dot means "no extension"; Windows removes it. */
        if (p[len - 1] == '.') has_dot = true;
        len--;
    }
    if (len + 5 > n) return false;
    for (i = 0; i < len; i++)
    {
        out[i] = (char)tolower((unsigned char)p[i]);
        if (p[i] == '.') has_dot = true;
    }
    out[len] = 0;
    if (!has_dot) memcpy(out + len, ".dll", 5);
    return true;
}

static rt_module *find_by_name(const char *norm)
{
    int i;
    for (i = 0; i < nmodules; i++)
        if (strcmp(modules[i].name, norm) == 0) return &modules[i];
    return NULL;
}

static rt_module *find_by_handle(uint32_t h)
{
    int i;
    if (h == 0) return &modules[0];
    for (i = 0; i < nmodules; i++)
        if (modules[i].base == h) return &modules[i];
    return NULL;
}

/* Is the translated DLL loaded? */
static bool is_loaded(const rt_module *m)
{
    return m->kind != MOD_PE || m->refs > 0;
}

uint32_t rt_module_image_size(uint32_t h)
{
    rt_module *m;
    uint32_t size = 0;
    loader_enter();
    m = find_by_handle(h);
    if (m != NULL && m->kind != MOD_SYSTEM && is_loaded(m)) size = m->size;
    loader_leave();
    return size;
}

uint32_t rt_module_entry(uint32_t base)
{
    rt_module *m;
    uint32_t entry;
    loader_enter();
    m = find_by_handle(base);
    if (base == 0 || m == NULL || m->kind == MOD_SYSTEM) rt_fatal("rt_module_entry: 0x%08x is not a translated module", base);
    entry = m->entry;
    loader_leave();
    if (entry == 0) rt_fatal("%s has no entry point", m->name);
    return entry;
}

/* ---------------------------------------------------------------- DLL entry points */

/* Call DllMain with DLL_PROCESS_ATTACH. Returns false (and undoes the
   load) if DllMain fails. The loader lock is held. */
static bool attach(_cpu *cpu, rt_module *m, uint32_t reserved)
{
    uint32_t ok;
    m->attached = true;
    load_order[nloaded++] = (int)(m - modules);
    if (m->entry == 0) return true;
    ok = rt_call_std(cpu, m->entry, 3, m->base, DLL_PROCESS_ATTACH, reserved);
    if (ok != 0) return true;
    /* DllMain failed: Windows calls it with DLL_PROCESS_DETACH and unloads
       the DLL. */
    rt_call_std(cpu, m->entry, 3, m->base, DLL_PROCESS_DETACH, reserved);
    m->attached = false;
    nloaded--;
    return false;
}

void rt_module_attach_static(_cpu *cpu, uint32_t base)
{
    rt_module *m = find_by_handle(base);
    if (base == 0 || m == NULL || m->kind != MOD_PE) rt_fatal("rt_module_attach_static: 0x%08x is not a translated module", base);
    loader_enter();
    if (m->refs != 0) rt_fatal("%s is already loaded", m->name);
    m->refs = 1;
    /* lpReserved is not NULL for a DLL that the program imports. */
    if (!attach(cpu, m, 1)) rt_fatal("DllMain of %s failed", m->name);
    loader_leave();
}

/* LoadLibrary for a name in UTF-8. */
static void load_library(_cpu *cpu, const char *buf)
{
    char norm[MAX_NAME];
    rt_module *m;

    if (!normalize(buf, norm, sizeof(norm)) || (m = find_by_name(norm)) == NULL || m->kind == MOD_EXE)
    {
        rt_set_last_error(ERROR_MOD_NOT_FOUND);
        RET(0);
        return;
    }
    loader_enter();
    if (m->kind == MOD_PE)
    {
        if (m->refs == INT32_MAX) rt_fatal("too many references to %s", m->name);
        m->refs++;
        if (!m->attached && !attach(cpu, m, 0))
        {
            m->refs = 0;
            loader_leave();
            rt_set_last_error(1114u); /* ERROR_DLL_INIT_FAILED */
            RET(0);
            return;
        }
    }
    loader_leave();
    rt_set_last_error(ERROR_SUCCESS);
    RET(m->base);
}

void rt_LoadLibraryA(_cpu *cpu)
{
    char buf[RT_PATH_BYTES];
    if (ARG(0) == 0 || rt_gstr_a(buf, sizeof(buf), ARG(0)) != RT_STR_OK)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    load_library(cpu, buf);
}

void rt_LoadLibraryW(_cpu *cpu)
{
    char buf[RT_PATH_BYTES];
    if (ARG(0) == 0 || rt_gstr_w(buf, sizeof(buf), ARG(0)) != RT_STR_OK)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    load_library(cpu, buf);
}

/* BOOL DisableThreadLibraryCalls(HMODULE): the DLL gets no
   DLL_THREAD_ATTACH and DLL_THREAD_DETACH calls. */
void rt_DisableThreadLibraryCalls(_cpu *cpu)
{
    rt_module *m;
    loader_enter();
    m = ARG(0) == 0 ? NULL : find_by_handle(ARG(0));
    if (m == NULL || m->kind != MOD_PE || !is_loaded(m))
    {
        loader_leave();
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    m->no_thread_calls = true;
    loader_leave();
    RET(1);
}

void rt_FreeLibrary(_cpu *cpu)
{
    rt_module *m = find_by_handle(ARG(0));
    if (ARG(0) == 0 || m == NULL || !is_loaded(m))
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    loader_enter();
    if (m->kind == MOD_PE)
    {
        m->refs--;
        if (m->refs == 0 && m->attached)
        {
            int i;
            if (m->entry != 0) rt_call_std(cpu, m->entry, 3, m->base, DLL_PROCESS_DETACH, 0);
            m->attached = false;
            for (i = 0; i < nloaded; i++)
            {
                if (load_order[i] == (int)(m - modules))
                {
                    memmove(&load_order[i], &load_order[i + 1], (size_t)(nloaded - i - 1) * sizeof(int));
                    nloaded--;
                    break;
                }
            }
        }
    }
    loader_leave();
    RET(1);
}

static void get_module_handle(_cpu *cpu, bool wide)
{
    char buf[RT_PATH_BYTES], norm[MAX_NAME];
    rt_module *m;
    if (ARG(0) == 0)
    {
        RET(modules[0].base);
        return;
    }
    if ((wide ? rt_gstr_w(buf, sizeof(buf), ARG(0)) : rt_gstr_a(buf, sizeof(buf), ARG(0))) != RT_STR_OK || !normalize(buf, norm, sizeof(norm)) ||
        (m = find_by_name(norm)) == NULL || !is_loaded(m))
    {
        rt_set_last_error(ERROR_MOD_NOT_FOUND);
        RET(0);
        return;
    }
    RET(m->base);
}

/* HMODULE GetModuleHandleA/W(LPCSTR name) */
void rt_GetModuleHandleA(_cpu *cpu) { get_module_handle(cpu, false); }
void rt_GetModuleHandleW(_cpu *cpu) { get_module_handle(cpu, true); }

/* GetModuleFileNameA/W with the Windows XP truncation rules: when the
   buffer (n characters) is too small, the result is truncated to n
   characters without a terminator, the return value is n and the error is
   ERROR_INSUFFICIENT_BUFFER. */
static void module_file_name(_cpu *cpu, bool wide, uint32_t h, uint32_t buf, uint32_t n)
{
    rt_module *m = find_by_handle(h);
    uint32_t len = 0, i;
    uint16_t units[RT_PATH_BYTES];
    const uint8_t *s;
    size_t pos, total;

    if (m == NULL || !is_loaded(m))
    {
        rt_set_last_error(ERROR_MOD_NOT_FOUND);
        RET(0);
        return;
    }
    /* The path as UTF-16 units (wide) or Windows-1252 bytes (ANSI). */
    s = (const uint8_t *)m->path;
    total = strlen(m->path);
    for (pos = 0; pos < total;)
    {
        uint32_t cp;
        int l = rt_utf8_decode(s + pos, total - pos, &cp);
        if (l < 0 || len + 2 > RT_PATH_BYTES) rt_fatal("module path %s cannot be converted", m->path);
        if (wide && cp >= 0x10000)
        {
            units[len++] = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
            units[len++] = (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        }
        else if (wide)
            units[len++] = (uint16_t)cp;
        else
        {
            int b = rt_unicode_to_cp1252(cp);
            units[len++] = (uint16_t)(b < 0 ? '?' : b);
        }
        pos += (size_t)l;
    }
    if (n == 0)
    {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0);
        return;
    }
    for (i = 0; i < len && i < n; i++)
    {
        if (wide) wr16(buf + 2u * i, units[i]);
        else wr8(buf + i, (uint8_t)units[i]);
    }
    if (len + 1 > n)
    {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(n);
        return;
    }
    if (wide) wr16(buf + 2u * len, 0);
    else wr8(buf + len, 0);
    rt_set_last_error(ERROR_SUCCESS);
    RET(len);
}

void rt_GetModuleFileNameA(_cpu *cpu)
{
    module_file_name(cpu, false, ARG(0), ARG(1), ARG(2));
}

void rt_GetModuleFileNameW(_cpu *cpu)
{
    module_file_name(cpu, true, ARG(0), ARG(1), ARG(2));
}

/* ---------------------------------------------------------------- PSAPI */

/* Is h the current process (the pseudo handle or a process handle)? */
static bool current_process(uint32_t h)
{
    rt_object *o;
    if (h == RT_PSEUDO_PROCESS) return true;
    o = rt_handle_get(h, OBJ_PROCESS);
    if (o == NULL) return false;
    rt_object_unref(o);
    return true;
}

/* BOOL EnumProcessModules(HANDLE process, HMODULE *mods, DWORD cb,
   LPDWORD needed): the loaded modules, the program first. */
void rt_EnumProcessModules(_cpu *cpu)
{
    uint32_t mods = ARG(1), cb = ARG(2), needed = ARG(3), n = 0;
    int i;
    if (!current_process(ARG(0)))
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    if (needed == 0 || (cb != 0 && mods == 0))
    {
        rt_set_last_error(998u); /* ERROR_NOACCESS */
        RET(0);
        return;
    }
    loader_enter();
    for (i = 0; i < nmodules; i++)
    {
        if (!is_loaded(&modules[i])) continue;
        if (4u * (n + 1u) <= cb) wr32(mods + 4u * n, modules[i].base);
        n++;
    }
    loader_leave();
    wr32(needed, 4u * n);
    RET(1);
}

/* DWORD GetModuleBaseNameW(HANDLE process, HMODULE, LPWSTR buf, DWORD n):
   the file name of the module; a name longer than the buffer is cut. */
void rt_GetModuleBaseNameW(_cpu *cpu)
{
    uint32_t buf = ARG(2), n = ARG(3), len, k, i;
    rt_module *m;
    const char *name;
    uint16_t units[RT_PATH_BYTES];
    size_t pos, total;
    if (!current_process(ARG(0)))
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    m = find_by_handle(ARG(1));
    if (m == NULL || !is_loaded(m))
    {
        rt_set_last_error(ERROR_MOD_NOT_FOUND);
        RET(0);
        return;
    }
    name = strrchr(m->path, '\\');
    name = name != NULL ? name + 1 : m->path;
    total = strlen(name);
    len = 0;
    for (pos = 0; pos < total;)
    {
        uint32_t c;
        int l = rt_utf8_decode((const uint8_t *)name + pos, total - pos, &c);
        if (l < 0 || len + 2 > RT_PATH_BYTES) rt_fatal("module path %s cannot be converted", m->path);
        if (c >= 0x10000)
        {
            units[len++] = (uint16_t)(0xD800 + ((c - 0x10000) >> 10));
            units[len++] = (uint16_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
        }
        else
            units[len++] = (uint16_t)c;
        pos += (size_t)l;
    }
    if (n == 0 || buf == 0)
    {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0);
        return;
    }
    k = len < n - 1u ? len : n - 1u;
    for (i = 0; i < k; i++) wr16(buf + 2u * i, units[i]);
    wr16(buf + 2u * k, 0);
    RET(k);
}

/* DWORD GetModuleFileNameExW(HANDLE process, HMODULE, LPWSTR buf, DWORD n) */
void rt_GetModuleFileNameExW(_cpu *cpu)
{
    if (!current_process(ARG(0)))
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    module_file_name(cpu, true, ARG(1), ARG(2), ARG(3));
}

/* BOOL GetModuleInformation(HANDLE process, HMODULE, LPMODULEINFO, DWORD cb):
   MODULEINFO is the base, the image size and the entry point. */
void rt_GetModuleInformation(_cpu *cpu)
{
    uint32_t info = ARG(2), cb = ARG(3);
    rt_module *m;
    if (!current_process(ARG(0)))
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    if (cb < 12u || info == 0)
    {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0);
        return;
    }
    m = find_by_handle(ARG(1));
    if (m == NULL || !is_loaded(m))
    {
        rt_set_last_error(ERROR_MOD_NOT_FOUND);
        RET(0);
        return;
    }
    wr32(info + 0, m->base);
    wr32(info + 4, m->size);
    wr32(info + 8, m->entry);
    RET(1);
}

static struct
{
    uint32_t base, addr;
    char name[64];
} export_overrides[RT_MAX_EXPORT_OVERRIDES];
static int n_export_overrides;

void rt_module_override_export(uint32_t base, const char *name, uint32_t addr)
{
    rt_module *m = find_by_handle(base);
    if (base == 0 || m == NULL || m->kind != MOD_PE) rt_fatal("rt_module_override_export: 0x%08x is not a translated module", base);
    if (n_export_overrides == RT_MAX_EXPORT_OVERRIDES) rt_fatal("more than %d export overrides", RT_MAX_EXPORT_OVERRIDES);
    if (strlen(name) >= sizeof(export_overrides[0].name)) rt_fatal("export override name %s is too long", name);
    (void)rt_module_export(base, name); /* the export must exist */
    export_overrides[n_export_overrides].base = base;
    export_overrides[n_export_overrides].addr = addr;
    snprintf(export_overrides[n_export_overrides].name, sizeof(export_overrides[0].name), "%s", name);
    n_export_overrides++;
}

void rt_GetProcAddress(_cpu *cpu)
{
    /* A NULL module is the program image, as in Windows (kernel32 maps NULL
       to the image base of the process). */
    rt_module *m = find_by_handle(ARG(0) != 0 ? ARG(0) : modules[0].base);
    uint32_t name = ARG(1), addr = 0;
    char buf[256];
    bool by_ordinal = (name >> 16) == 0;

    if (m == NULL || !is_loaded(m))
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    if (!by_ordinal && rt_gstr(buf, sizeof(buf), name) != 0)
    {
        rt_set_last_error(ERROR_PROC_NOT_FOUND);
        RET(0);
        return;
    }
    if (!by_ordinal)
    {
        int i;
        for (i = 0; i < n_export_overrides; i++)
            if (export_overrides[i].base == m->base && strcmp(export_overrides[i].name, buf) == 0)
            {
                RET(export_overrides[i].addr);
                return;
            }
    }
    if (m->kind == MOD_SYSTEM)
    {
        const rt_import_entry *e = find_import(m->key, by_ordinal ? NULL : buf, name);
        if (e != NULL) addr = H2G(e->addr);
    }
    else if (m->kind == MOD_PE)
        addr = export_address(m, by_ordinal ? NULL : buf, name);
    if (addr != 0)
    {
        RET(addr);
        return;
    }
    rt_set_last_error(ERROR_PROC_NOT_FOUND);
    RET(0);
}

uint32_t rt_module_export(uint32_t base, const char *name)
{
    rt_module *m = find_by_handle(base);
    uint32_t addr;
    if (base == 0 || m == NULL || m->kind != MOD_PE) rt_fatal("rt_module_export: 0x%08x is not a translated module", base);
    addr = export_address(m, name, 0);
    if (addr == 0) rt_fatal("%s has no export %s", m->name, name);
    return addr;
}

/* ---------------------------------------------------------------- thread notifications */

static void notify_threads(_cpu *cpu, uint32_t reason)
{
    int i;
    loader_enter();
    for (i = 0; i < nloaded; i++)
    {
        rt_module *m = &modules[load_order[i]];
        if (m->attached && m->entry != 0 && !m->no_thread_calls) rt_call_std(cpu, m->entry, 3, m->base, reason, 0);
    }
    loader_leave();
}

void rt_module_thread_attach(_cpu *cpu)
{
    notify_threads(cpu, DLL_THREAD_ATTACH);
}

void rt_module_thread_detach(_cpu *cpu)
{
    if (atomic_load(&exiting)) return;
    notify_threads(cpu, DLL_THREAD_DETACH);
}

/* ---------------------------------------------------------------- process exit */

static void suspend_other(rt_thread *t, void *ctx)
{
    rt_thread *self = ctx;
    if (t == self) return;
    /* A thread that ended already cannot be suspended; it runs no more
       guest code. */
    (void)thread_suspend(pthread_mach_thread_np(t->pt));
}

_Noreturn void rt_process_exit(uint32_t code)
{
    rt_thread *self = rt_self_or_null();
    int i;

    if (atomic_exchange(&exiting, true))
    {
        /* A second exit during the exit notifications ends the process. */
        fflush(stderr);
        _exit((int)code);
    }
    if (self != NULL)
    {
        /* ExitProcess ends all other threads before the DLL notifications. */
        rt_thread_foreach(suspend_other, self);
        loader_enter();
        for (i = nloaded - 1; i >= 0; i--)
        {
            rt_module *m = &modules[load_order[i]];
            if (m->attached && m->entry != 0)
            {
                m->attached = false;
                rt_call_std(self->cpu, m->entry, 3, m->base, DLL_PROCESS_DETACH, 1);
            }
        }
        loader_leave();
    }
    fflush(stdout);
    fflush(stderr);
    _exit((int)code);
}

void rt_ExitProcess(_cpu *cpu)
{
    rt_process_exit(ARG(0));
}

void rt_TerminateProcess(_cpu *cpu)
{
    if (ARG(0) != 0xFFFFFFFFu) rt_fatal("TerminateProcess of another process");
    fflush(stderr);
    _exit((int)ARG(1));
}
