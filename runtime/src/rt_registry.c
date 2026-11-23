/*
 * The registry (ADVAPI32 Reg* functions and SHLWAPI SHDeleteKeyA).
 *
 * The registry is a tree of keys in host memory. Each key has values
 * (name, type, data). Names are kept as UTF-8 and compared without regard
 * to case (ASCII and Latin-1 letters). String data (REG_SZ,
 * REG_EXPAND_SZ, REG_MULTI_SZ) is kept as UTF-16LE, as on Windows; the
 * "A" functions convert it from and to Windows-1252.
 *
 * Root keys:
 *   HKEY_LOCAL_MACHINE   SOFTWARE and SYSTEM are saved in
 *                        C:\ProgramData\Registry\HKLM.reg. HARDWARE is
 *                        volatile: the runtime makes it at each start from
 *                        the emulated hardware (rt_registry_init and
 *                        rt_reg_set_volatile).
 *   HKEY_CURRENT_USER    saved in C:\Users\Player\NTUSER.reg.
 *   HKEY_CLASSES_ROOT    HKEY_LOCAL_MACHINE\SOFTWARE\Classes.
 *   HKEY_USERS           the key .DEFAULT (volatile, empty).
 *   HKEY_CURRENT_CONFIG  HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\
 *                        Hardware Profiles\Current.
 * A change of a saved key or value writes its hive file again (a new file,
 * then rename). A write failure stops the process: the runtime does not
 * continue with registry data that it cannot save.
 *
 * The hive file is UTF-8 text:
 *   RTREG 1
 *   R                              the values of the hive root follow
 *   K <path>                       a key (path from the hive root)
 *   V <name> <type> <hex data>     a value of the key before it
 * Names and paths are percent-encoded (bytes below 0x21, above 0x7E, '%').
 *
 * Handles: the predefined keys are 0x80000000-0x80000005; other keys are
 * handles of the handle table (OBJ_REGKEY). A handle keeps its key alive
 * after the key is deleted; operations on it then return
 * ERROR_KEY_DELETED. The access mask of RegOpenKeyEx and RegCreateKeyEx
 * is checked for values (KEY_QUERY_VALUE, KEY_SET_VALUE) and subkeys
 * (KEY_CREATE_SUB_KEY). The emulated user is an administrator, so every
 * key gives all access rights.
 */
#include "rt_file.h"
#include "rt_object.h"
#include "rt_text.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ERROR_BAD_PATHNAME 161u
#define ERROR_BADKEY 1010u
#define ERROR_KEY_DELETED 1018u

#define HKEY_CLASSES_ROOT 0x80000000u
#define HKEY_CURRENT_USER 0x80000001u
#define HKEY_LOCAL_MACHINE 0x80000002u
#define HKEY_USERS 0x80000003u
#define HKEY_PERFORMANCE_DATA 0x80000004u
#define HKEY_CURRENT_CONFIG 0x80000005u

#define KEY_QUERY_VALUE 0x0001u
#define KEY_SET_VALUE 0x0002u
#define KEY_CREATE_SUB_KEY 0x0004u
#define KEY_ENUMERATE_SUB_KEYS 0x0008u
#define KEY_NOTIFY 0x0010u
#define KEY_CREATE_LINK 0x0020u
#define KEY_WOW64_64KEY 0x0100u
#define KEY_WOW64_32KEY 0x0200u
#define KEY_ALL_ACCESS 0xF003Fu
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_EXECUTE 0x20000000u
#define GENERIC_ALL 0x10000000u
#define MAXIMUM_ALLOWED 0x02000000u

#define REG_OPTION_VOLATILE 0x1u
#define REG_CREATED_NEW_KEY 1u
#define REG_OPENED_EXISTING_KEY 2u

#define REG_NONE 0u
#define REG_SZ 1u
#define REG_EXPAND_SZ 2u
#define REG_BINARY 3u
#define REG_DWORD 4u
#define REG_MULTI_SZ 7u

/* Limits of Windows: key names 255 characters, value names 16383. */
#define MAX_KEY_NAME 255u
#define MAX_VALUE_NAME 16383u
/* Upper limit for the data of one value (the runtime keeps it in memory). */
#define MAX_VALUE_DATA (16u << 20)
#define MAX_DEPTH 512u

typedef struct reg_value {
    char *name;
    uint32_t type, size;
    uint8_t *data;
} reg_value;

typedef enum { HIVE_NONE = 0, HIVE_MACHINE, HIVE_USER } hive_id;

typedef struct reg_key {
    char *name;
    struct reg_key *parent;
    struct reg_key **sub;
    uint32_t nsub, capsub;
    reg_value *val;
    uint32_t nval, capval;
    bool is_volatile;
    bool deleted;
    hive_id hive;
    int refs; /* the tree (while not deleted) and each handle */
} reg_key;

static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static bool reg_ready;
static reg_key *root_machine, *root_user, *root_users;

/* ---------------------------------------------------------------- names */

/* Upper case of one code point for name comparison: ASCII and Latin-1. */
static uint32_t fold_cp(uint32_t c)
{
    if (c >= 'a' && c <= 'z') return c - 32u;
    if (c >= 0xE0u && c <= 0xFEu && c != 0xF7u) return c - 32u;
    return c;
}

/* Compare two UTF-8 names without regard to case. */
static bool name_equal(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b), i = 0, j = 0;
    while (i < na && j < nb)
    {
        uint32_t ca, cb;
        int la = rt_utf8_decode((const uint8_t *)a + i, na - i, &ca);
        int lb = rt_utf8_decode((const uint8_t *)b + j, nb - j, &cb);
        if (la <= 0 || lb <= 0) rt_fatal("registry: a name is not valid UTF-8");
        if (fold_cp(ca) != fold_cp(cb)) return false;
        i += (size_t)la;
        j += (size_t)lb;
    }
    return i == na && j == nb;
}

static char *xstrdup(const char *s)
{
    char *d = strdup(s);
    if (d == NULL) rt_fatal("out of host memory (registry)");
    return d;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (q == NULL) rt_fatal("out of host memory (registry)");
    return q;
}

/* ---------------------------------------------------------------- tree */

static reg_key *key_new(const char *name, reg_key *parent, hive_id hive, bool is_volatile)
{
    reg_key *k = calloc(1, sizeof *k);
    if (k == NULL) rt_fatal("out of host memory (registry)");
    k->name = xstrdup(name);
    k->parent = parent;
    k->hive = hive;
    k->is_volatile = is_volatile;
    k->refs = 1;
    if (parent != NULL)
    {
        if (parent->nsub == parent->capsub)
        {
            parent->capsub = parent->capsub ? 2u * parent->capsub : 8u;
            parent->sub = xrealloc(parent->sub, parent->capsub * sizeof *parent->sub);
        }
        parent->sub[parent->nsub++] = k;
    }
    return k;
}

static void key_free(reg_key *k)
{
    uint32_t i;
    for (i = 0; i < k->nval; i++)
    {
        free(k->val[i].name);
        free(k->val[i].data);
    }
    free(k->val);
    free(k->sub);
    free(k->name);
    free(k);
}

static void key_unref(reg_key *k)
{
    if (--k->refs == 0) key_free(k);
}

static reg_key *find_sub(const reg_key *k, const char *name)
{
    uint32_t i;
    for (i = 0; i < k->nsub; i++)
        if (name_equal(k->sub[i]->name, name)) return k->sub[i];
    return NULL;
}

static reg_value *find_value(const reg_key *k, const char *name)
{
    uint32_t i;
    for (i = 0; i < k->nval; i++)
        if (name_equal(k->val[i].name, name)) return &k->val[i];
    return NULL;
}

/* Set a value (the data is copied). */
static void value_set(reg_key *k, const char *name, uint32_t type, const void *data, uint32_t size)
{
    reg_value *v = find_value(k, name);
    uint8_t *copy = malloc(size ? size : 1u);
    if (copy == NULL) rt_fatal("out of host memory (registry)");
    if (size) memcpy(copy, data, size);
    if (v == NULL)
    {
        if (k->nval == k->capval)
        {
            k->capval = k->capval ? 2u * k->capval : 8u;
            k->val = xrealloc(k->val, k->capval * sizeof *k->val);
        }
        v = &k->val[k->nval++];
        v->name = xstrdup(name);
    }
    else
        free(v->data);
    v->type = type;
    v->size = size;
    v->data = copy;
}

static bool value_delete(reg_key *k, const char *name)
{
    uint32_t i;
    for (i = 0; i < k->nval; i++)
        if (name_equal(k->val[i].name, name))
        {
            free(k->val[i].name);
            free(k->val[i].data);
            k->val[i] = k->val[--k->nval];
            return true;
        }
    return false;
}

/* Remove k and its subkeys from the tree. Handles keep the keys alive. */
static void key_delete_tree(reg_key *k)
{
    reg_key *p = k->parent;
    uint32_t i;
    while (k->nsub != 0) key_delete_tree(k->sub[k->nsub - 1u]);
    if (p != NULL)
        for (i = 0; i < p->nsub; i++)
            if (p->sub[i] == k)
            {
                memmove(&p->sub[i], &p->sub[i + 1u], (p->nsub - i - 1u) * sizeof *p->sub);
                p->nsub--;
                break;
            }
    k->parent = NULL;
    k->deleted = true;
    key_unref(k);
}

/* ---------------------------------------------------------------- hive files */

static const char *hive_guest_path(hive_id h)
{
    return h == HIVE_USER ? "C:\\Users\\Player\\NTUSER.reg" : "C:\\ProgramData\\Registry\\HKLM.reg";
}

static reg_key *hive_root(hive_id h)
{
    return h == HIVE_USER ? root_user : root_machine;
}

static void enc_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
    if (*len + n + 1 > *cap)
    {
        while (*len + n + 1 > *cap) *cap = *cap ? 2u * *cap : 4096u;
        *buf = xrealloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

static void enc_name(char **buf, size_t *len, size_t *cap, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    const uint8_t *p;
    for (p = (const uint8_t *)s; *p; p++)
    {
        if (*p < 0x21u || *p > 0x7Eu || *p == '%')
        {
            char e[3] = { '%', hex[*p >> 4], hex[*p & 15u] };
            enc_append(buf, len, cap, e, 3);
        }
        else
            enc_append(buf, len, cap, (const char *)p, 1);
    }
}

static void save_key(char **buf, size_t *len, size_t *cap, const reg_key *k, const char *path)
{
    uint32_t i, j;
    if (k->is_volatile) return;
    if (path[0] != 0 || k->nval != 0)
    {
        if (path[0] != 0)
        {
            enc_append(buf, len, cap, "K ", 2);
            enc_name(buf, len, cap, path);
            enc_append(buf, len, cap, "\n", 1);
        }
        else
            enc_append(buf, len, cap, "R\n", 2); /* the values of the hive root */
        for (i = 0; i < k->nval; i++)
        {
            char t[16];
            const reg_value *v = &k->val[i];
            enc_append(buf, len, cap, "V ", 2);
            if (v->name[0] == 0) enc_append(buf, len, cap, "%", 1); /* the default value */
            else enc_name(buf, len, cap, v->name);
            snprintf(t, sizeof t, " %u ", v->type);
            enc_append(buf, len, cap, t, strlen(t));
            for (j = 0; j < v->size; j++)
            {
                char hx[3];
                snprintf(hx, sizeof hx, "%02X", v->data[j]);
                enc_append(buf, len, cap, hx, 2);
            }
            enc_append(buf, len, cap, "\n", 1);
        }
    }
    for (i = 0; i < k->nsub; i++)
    {
        size_t pl = strlen(path), nl = strlen(k->sub[i]->name);
        char *sp = malloc(pl + nl + 2);
        if (sp == NULL) rt_fatal("out of host memory (registry)");
        if (pl) { memcpy(sp, path, pl); sp[pl] = '\\'; memcpy(sp + pl + 1, k->sub[i]->name, nl + 1); }
        else memcpy(sp, k->sub[i]->name, nl + 1);
        save_key(buf, len, cap, k->sub[i], sp);
        free(sp);
    }
}

/* Write the hive file of h. Stops the process on failure. */
static void hive_save(hive_id h)
{
    char host[RT_HOST_PATH], tmp[RT_HOST_PATH + 8], dir[RT_HOST_PATH];
    char *buf = NULL;
    size_t len = 0, cap = 0, done = 0;
    bool exists;
    uint32_t err = rt_path_host(hive_guest_path(h), host, sizeof host, &exists);
    int fd;
    if (err == ERROR_PATH_NOT_FOUND)
    {
        /* C:\ProgramData\Registry does not exist yet. */
        if (rt_path_host("C:\\ProgramData", dir, sizeof dir, &exists) != 0 || !exists)
            rt_fatal("registry: C:\\ProgramData is not available");
        if ((size_t)snprintf(tmp, sizeof tmp, "%s/Registry", dir) >= sizeof tmp) rt_fatal("registry: host path too long");
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) rt_fatal("registry: cannot create %s: %s", tmp, strerror(errno));
        err = rt_path_host(hive_guest_path(h), host, sizeof host, &exists);
    }
    if (err != 0) rt_fatal("registry: no host path for %s (error %u)", hive_guest_path(h), err);
    enc_append(&buf, &len, &cap, "RTREG 1\n", 8);
    save_key(&buf, &len, &cap, hive_root(h), "");
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.new", host) >= sizeof tmp) rt_fatal("registry: host path too long");
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) rt_fatal("registry: cannot write %s: %s", tmp, strerror(errno));
    while (done < len)
    {
        ssize_t w = write(fd, buf + done, len - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) rt_fatal("registry: write to %s failed: %s", tmp, strerror(errno));
        done += (size_t)w;
    }
    if (fsync(fd) != 0) rt_fatal("registry: fsync of %s failed: %s", tmp, strerror(errno));
    if (close(fd) != 0) rt_fatal("registry: close of %s failed: %s", tmp, strerror(errno));
    if (rename(tmp, host) != 0) rt_fatal("registry: rename to %s failed: %s", host, strerror(errno));
    free(buf);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Decode a percent-encoded name in place. Returns false for a bad escape. */
static bool dec_name(char *s)
{
    char *r = s, *w = s;
    while (*r)
    {
        if (*r == '%')
        {
            int a = hexval(r[1]), b = a >= 0 ? hexval(r[2]) : -1;
            if (a < 0 || b < 0) return false;
            *w++ = (char)(a * 16 + b);
            r += 3;
        }
        else
            *w++ = *r++;
    }
    *w = 0;
    return true;
}

/* Find or create the key at a backslash-separated path under root. */
static reg_key *create_path(reg_key *root, char *path, hive_id h)
{
    reg_key *k = root;
    char *save = NULL, *c;
    for (c = strtok_r(path, "\\", &save); c != NULL; c = strtok_r(NULL, "\\", &save))
    {
        reg_key *s = find_sub(k, c);
        k = s != NULL ? s : key_new(c, k, h, false);
    }
    return k;
}

static void hive_load(hive_id h)
{
    char host[RT_HOST_PATH], *buf, *line, *save = NULL;
    bool exists;
    struct stat st;
    reg_key *cur = NULL;
    size_t done = 0;
    unsigned lineno = 0;
    int fd;
    uint32_t err = rt_path_host(hive_guest_path(h), host, sizeof host, &exists);
    if (err == ERROR_PATH_NOT_FOUND || (err == 0 && !exists)) return; /* a new registry */
    if (err != 0) rt_fatal("registry: no host path for %s (error %u)", hive_guest_path(h), err);
    fd = open(host, O_RDONLY);
    if (fd < 0) rt_fatal("registry: cannot read %s: %s", host, strerror(errno));
    if (fstat(fd, &st) != 0) rt_fatal("registry: fstat of %s failed", host);
    if (st.st_size > (off_t)(256u << 20)) rt_fatal("registry: %s is larger than 256 MiB", host);
    buf = malloc((size_t)st.st_size + 1);
    if (buf == NULL) rt_fatal("out of host memory (registry)");
    while (done < (size_t)st.st_size)
    {
        ssize_t r = read(fd, buf + done, (size_t)st.st_size - done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) rt_fatal("registry: read of %s failed", host);
        done += (size_t)r;
    }
    close(fd);
    buf[done] = 0;
    for (line = strtok_r(buf, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save))
    {
        lineno++;
        if (lineno == 1)
        {
            if (strcmp(line, "RTREG 1") != 0) rt_fatal("registry: %s is not a registry file of this runtime", host);
            continue;
        }
        if (strcmp(line, "R") == 0)
            cur = hive_root(h);
        else if (line[0] == 'K' && line[1] == ' ')
        {
            if (!dec_name(line + 2) || line[2] == 0) rt_fatal("registry: %s line %u: bad key path", host, lineno);
            cur = create_path(hive_root(h), line + 2, h);
        }
        else if (line[0] == 'V' && line[1] == ' ' && cur != NULL)
        {
            char *name = line + 2, *t = strchr(name, ' '), *hexs, *end;
            unsigned long type;
            size_t n, i;
            uint8_t *data;
            if (t == NULL) rt_fatal("registry: %s line %u: bad value", host, lineno);
            *t++ = 0;
            type = strtoul(t, &end, 10);
            if (end == t || *end != ' ' || type > 0xFFFFFFFFul) rt_fatal("registry: %s line %u: bad value type", host, lineno);
            hexs = end + 1;
            n = strlen(hexs);
            if (n % 2u || n / 2u > MAX_VALUE_DATA) rt_fatal("registry: %s line %u: bad value data", host, lineno);
            if (strcmp(name, "%") == 0) name[0] = 0;
            else if (!dec_name(name)) rt_fatal("registry: %s line %u: bad value name", host, lineno);
            data = malloc(n / 2u + 1u);
            if (data == NULL) rt_fatal("out of host memory (registry)");
            for (i = 0; i < n / 2u; i++)
            {
                int a = hexval(hexs[2 * i]), b = hexval(hexs[2 * i + 1]);
                if (a < 0 || b < 0) rt_fatal("registry: %s line %u: bad value data", host, lineno);
                data[i] = (uint8_t)(a * 16 + b);
            }
            value_set(cur, name, (uint32_t)type, data, (uint32_t)(n / 2u));
            free(data);
        }
        else
            rt_fatal("registry: %s line %u is not valid", host, lineno);
    }
    if (lineno == 0) rt_fatal("registry: %s is empty", host);
    free(buf);
}

/* Save the hive of k if k is saved. Call with the registry lock held. */
static void key_changed(const reg_key *k)
{
    const reg_key *p;
    for (p = k; p != NULL; p = p->parent)
        if (p->is_volatile) return;
    if (k->hive != HIVE_NONE) hive_save(k->hive);
}

/* ---------------------------------------------------------------- initialization */

static reg_key *ensure_path(reg_key *root, const char *path, bool is_volatile)
{
    char *copy = xstrdup(path), *save = NULL, *c;
    reg_key *k = root;
    for (c = strtok_r(copy, "\\", &save); c != NULL; c = strtok_r(NULL, "\\", &save))
    {
        reg_key *s = find_sub(k, c);
        k = s != NULL ? s : key_new(c, k, root->hive, is_volatile);
    }
    free(copy);
    return k;
}

static void set_volatile_locked(const char *path, const char *name, uint32_t type, const void *data, uint32_t size);

static void put_sz(const char *path, const char *name, const char *utf8)
{
    size_t units, i = 0, n = strlen(utf8);
    uint16_t *w;
    uint32_t u = 0;
    if (!rt_utf16_length(utf8, &units)) rt_fatal("registry: text is not valid UTF-8");
    w = malloc((units + 1) * 2);
    if (w == NULL) rt_fatal("out of host memory (registry)");
    while (i < n)
    {
        uint32_t cp;
        int l = rt_utf8_decode((const uint8_t *)utf8 + i, n - i, &cp);
        if (l <= 0) rt_fatal("registry: text is not valid UTF-8");
        if (cp >= 0x10000u)
        {
            w[u++] = (uint16_t)(0xD800u + ((cp - 0x10000u) >> 10));
            w[u++] = (uint16_t)(0xDC00u + ((cp - 0x10000u) & 0x3FFu));
        }
        else
            w[u++] = (uint16_t)cp;
        i += (size_t)l;
    }
    w[u++] = 0;
    set_volatile_locked(path, name, REG_SZ, w, u * 2u);
    free(w);
}

static void put_dword(const char *path, const char *name, uint32_t v)
{
    set_volatile_locked(path, name, REG_DWORD, &v, 4);
}

static void registry_init_locked(void)
{
    uint32_t n, i;
    if (reg_ready) return;
    root_machine = key_new("HKEY_LOCAL_MACHINE", NULL, HIVE_MACHINE, false);
    root_user = key_new("HKEY_CURRENT_USER", NULL, HIVE_USER, false);
    root_users = key_new("HKEY_USERS", NULL, HIVE_NONE, true);
    reg_ready = true;
    hive_load(HIVE_MACHINE);
    hive_load(HIVE_USER);
    ensure_path(root_machine, "SOFTWARE\\Classes", false);
    ensure_path(root_machine, "SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current", false);
    key_new(".DEFAULT", root_users, HIVE_NONE, true);
    /* HKLM\HARDWARE: the emulated processors (see x86_cpuid and
       GetSystemInfo). The cpuid helper has no brand string and no clock
       frequency, so ProcessorNameString and ~MHz do not exist. */
    n = rt_host_processor_count();
    for (i = 0; i < n; i++)
    {
        char path[96];
        snprintf(path, sizeof path, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\%u", i);
        put_sz(path, "VendorIdentifier", "GenuineIntel");
        put_sz(path, "Identifier", "x86 Family 15 Model 2 Stepping 9");
        put_dword(path, "FeatureSet", 0);
    }
    put_sz("HARDWARE\\DESCRIPTION\\System", "Identifier", "AT/AT COMPATIBLE");
}

static void ensure_ready(void)
{
    if (!reg_ready) registry_init_locked();
}

/* Set a value in a volatile key of HKEY_LOCAL_MACHINE (path below HKLM,
   for example "HARDWARE\\DEVICEMAP\\VIDEO"). Call with the lock held. */
static void set_volatile_locked(const char *path, const char *name, uint32_t type, const void *data, uint32_t size)
{
    reg_key *k = ensure_path(root_machine, path, true);
    value_set(k, name, type, data, size);
}

void rt_reg_set_volatile(const char *path, const char *name, uint32_t type, const void *data, uint32_t size)
{
    if (pthread_mutex_lock(&reg_lock) != 0) rt_fatal("registry lock failed");
    ensure_ready();
    if (strncmp(path, "HARDWARE\\", 9) != 0) rt_fatal("rt_reg_set_volatile: %s is not below HARDWARE", path);
    set_volatile_locked(path, name, type, data, size);
    pthread_mutex_unlock(&reg_lock);
}

void rt_reg_set_volatile_sz(const char *path, const char *name, const char *utf8)
{
    if (pthread_mutex_lock(&reg_lock) != 0) rt_fatal("registry lock failed");
    ensure_ready();
    if (strncmp(path, "HARDWARE\\", 9) != 0) rt_fatal("rt_reg_set_volatile_sz: %s is not below HARDWARE", path);
    put_sz(path, name, utf8);
    pthread_mutex_unlock(&reg_lock);
}

/* ---------------------------------------------------------------- handles */

typedef struct key_handle {
    reg_key *key;
    uint32_t access;
} key_handle;

static void handle_destroy(rt_object *o)
{
    key_handle *kh = o->data;
    if (pthread_mutex_lock(&reg_lock) != 0) rt_fatal("registry lock failed");
    key_unref(kh->key);
    pthread_mutex_unlock(&reg_lock);
    free(kh);
}

/* Map generic rights and MAXIMUM_ALLOWED to key rights. */
static uint32_t map_access(uint32_t sam)
{
    uint32_t a = sam & ~(KEY_WOW64_64KEY | KEY_WOW64_32KEY);
    if (a & (GENERIC_ALL | MAXIMUM_ALLOWED)) a |= KEY_ALL_ACCESS;
    if (a & GENERIC_READ) a |= KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_NOTIFY | 0x20000u;
    if (a & GENERIC_WRITE) a |= KEY_SET_VALUE | KEY_CREATE_SUB_KEY | 0x20000u;
    if (a & GENERIC_EXECUTE) a |= KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_NOTIFY | 0x20000u;
    return a;
}

/* A new handle for key k (with the lock held). Returns 0 if the table is
   full. */
static uint32_t handle_for(reg_key *k, uint32_t access)
{
    key_handle *kh = malloc(sizeof *kh);
    rt_object *o;
    uint32_t h;
    if (kh == NULL) rt_fatal("out of host memory (registry)");
    k->refs++;
    kh->key = k;
    kh->access = access;
    o = rt_object_new(OBJ_REGKEY);
    o->data = kh;
    o->destroy = handle_destroy;
    h = rt_handle_new(o);
    rt_object_unref(o);
    return h;
}

/* The key of a guest HKEY (lock held). *access gets the rights. Returns a
   Win32 error. The key keeps one reference until reg_release. */
static uint32_t resolve(uint32_t hkey, reg_key **out, uint32_t *access)
{
    rt_object *o;
    *out = NULL;
    switch (hkey)
    {
    case HKEY_CLASSES_ROOT: *out = ensure_path(root_machine, "SOFTWARE\\Classes", false); break;
    case HKEY_CURRENT_USER: *out = root_user; break;
    case HKEY_LOCAL_MACHINE: *out = root_machine; break;
    case HKEY_USERS: *out = root_users; break;
    case HKEY_CURRENT_CONFIG: *out = ensure_path(root_machine, "SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current", false); break;
    case HKEY_PERFORMANCE_DATA: return ERROR_INVALID_HANDLE; /* no performance counters */
    default: break;
    }
    if (*out != NULL)
    {
        (*out)->refs++;
        *access = KEY_ALL_ACCESS;
        return ERROR_SUCCESS;
    }
    o = rt_handle_get(hkey, OBJ_REGKEY);
    if (o == NULL) return ERROR_INVALID_HANDLE;
    {
        key_handle *kh = o->data;
        *out = kh->key;
        *access = kh->access;
        (*out)->refs++;
    }
    rt_object_unref(o);
    if ((*out)->deleted)
    {
        key_unref(*out);
        *out = NULL;
        return ERROR_KEY_DELETED;
    }
    return ERROR_SUCCESS;
}

/* ---------------------------------------------------------------- paths */

/* Split a UTF-8 subkey path into components and walk from k. create:
   make missing keys (is_volatile for new keys). Returns a Win32 error;
   *out gets the key with one reference, *created whether the last key is
   new. */
static uint32_t walk(reg_key *k, const char *path, bool create, bool is_volatile, reg_key **out, bool *created)
{
    char *copy, *save = NULL, *c;
    unsigned depth = 0;
    reg_key *cur = k;
    *out = NULL;
    if (created) *created = false;
    if (path[0] == '\\') return create ? ERROR_BAD_PATHNAME : ERROR_FILE_NOT_FOUND;
    copy = xstrdup(path);
    for (c = strtok_r(copy, "\\", &save); c != NULL; c = strtok_r(NULL, "\\", &save))
    {
        size_t units;
        reg_key *s;
        if (!rt_utf16_length(c, &units) || units > MAX_KEY_NAME)
        {
            free(copy);
            return ERROR_INVALID_PARAMETER;
        }
        if (++depth > MAX_DEPTH)
        {
            free(copy);
            return ERROR_INVALID_PARAMETER;
        }
        s = find_sub(cur, c);
        if (s == NULL)
        {
            if (!create)
            {
                free(copy);
                return ERROR_FILE_NOT_FOUND;
            }
            /* A saved key cannot be below a volatile key. */
            if (cur->is_volatile && !is_volatile)
            {
                free(copy);
                return 1021u; /* ERROR_CHILD_MUST_BE_VOLATILE */
            }
            s = key_new(c, cur, cur->hive, is_volatile);
            if (created) *created = true;
        }
        else if (created)
            *created = false;
        cur = s;
    }
    free(copy);
    cur->refs++;
    *out = cur;
    return ERROR_SUCCESS;
}

/* Read a guest key or value name (ANSI or wide) as UTF-8. NULL gives "".
   Returns NULL if the name is too long or not valid. */
static char *guest_name(uint32_t g, bool wide, size_t max_units)
{
    size_t cap = 3u * max_units + 1u;
    char *s = malloc(cap);
    int r;
    if (s == NULL) rt_fatal("out of host memory (registry)");
    if (g == 0)
    {
        s[0] = 0;
        return s;
    }
    r = wide ? rt_gstr_w(s, cap, g) : rt_gstr_a(s, cap, g);
    if (r != RT_STR_OK)
    {
        free(s);
        return NULL;
    }
    return s;
}

/* ---------------------------------------------------------------- string data conversion */

static bool is_string_type(uint32_t t)
{
    return t == REG_SZ || t == REG_EXPAND_SZ || t == REG_MULTI_SZ;
}

/* ANSI bytes to UTF-16LE (one unit per byte). */
static uint8_t *ansi_to_utf16(const uint8_t *a, uint32_t n, uint32_t *out_size)
{
    uint8_t *w = malloc(n ? 2u * (size_t)n : 1u);
    uint32_t i;
    if (w == NULL) rt_fatal("out of host memory (registry)");
    for (i = 0; i < n; i++)
    {
        uint16_t u = rt_cp1252_to_unicode(a[i]);
        memcpy(w + 2u * i, &u, 2);
    }
    *out_size = 2u * n;
    return w;
}

/* UTF-16LE bytes to ANSI: each unit (a surrogate pair as one unit) to one
   byte, '?' for characters without a Windows-1252 byte. An odd last byte
   is not converted. */
static uint8_t *utf16_to_ansi(const uint8_t *w, uint32_t size, uint32_t *out_size)
{
    uint32_t units = size / 2u, i, n = 0;
    uint8_t *a = malloc(units ? units : 1u);
    if (a == NULL) rt_fatal("out of host memory (registry)");
    for (i = 0; i < units; i++)
    {
        uint16_t u;
        uint32_t cp;
        int b;
        memcpy(&u, w + 2u * i, 2);
        cp = u;
        if (u >= 0xD800u && u <= 0xDBFFu && i + 1 < units)
        {
            uint16_t u2;
            memcpy(&u2, w + 2u * (i + 1), 2);
            if (u2 >= 0xDC00u && u2 <= 0xDFFFu)
            {
                cp = 0x10000u + (((uint32_t)u - 0xD800u) << 10) + (u2 - 0xDC00u);
                i++;
            }
        }
        b = rt_unicode_to_cp1252(cp);
        a[n++] = b < 0 ? (uint8_t)'?' : (uint8_t)b;
    }
    *out_size = n;
    return a;
}

/* ---------------------------------------------------------------- API */

static void lock(void)
{
    if (pthread_mutex_lock(&reg_lock) != 0) rt_fatal("registry lock failed");
    ensure_ready();
}

static void unlock(void)
{
    pthread_mutex_unlock(&reg_lock);
}

/* LSTATUS RegCloseKey(HKEY) */
void rt_RegCloseKey(_cpu *cpu)
{
    uint32_t h = ARG(0);
    if (h >= HKEY_CLASSES_ROOT && h <= HKEY_CURRENT_CONFIG)
    {
        RET(ERROR_SUCCESS);
        return;
    }
    {
        rt_object *o = rt_handle_get(h, OBJ_REGKEY);
        if (o == NULL)
        {
            RET(ERROR_INVALID_HANDLE);
            return;
        }
        rt_object_unref(o);
    }
    RET(rt_handle_close(h) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE);
}

static uint32_t open_key(uint32_t hkey, uint32_t subkey, bool wide, uint32_t sam, uint32_t result)
{
    reg_key *base, *k;
    uint32_t access, err, h;
    char *name;
    if (result == 0) return ERROR_INVALID_PARAMETER;
    name = guest_name(subkey, wide, 32767u);
    if (name == NULL) return ERROR_INVALID_PARAMETER;
    lock();
    err = resolve(hkey, &base, &access);
    if (err == ERROR_SUCCESS)
    {
        err = walk(base, name, false, false, &k, NULL);
        key_unref(base);
        if (err == ERROR_SUCCESS)
        {
            h = handle_for(k, map_access(sam));
            key_unref(k);
            if (h == 0) err = ERROR_NOT_ENOUGH_MEMORY;
            else wr32(result, h);
        }
    }
    unlock();
    free(name);
    if (err != ERROR_SUCCESS) wr32(result, 0);
    return err;
}

/* LSTATUS RegOpenKeyExA/W(HKEY, LPCSTR subkey, DWORD options, REGSAM, PHKEY) */
void rt_RegOpenKeyExA(_cpu *cpu) { RET(open_key(ARG(0), ARG(1), false, ARG(3), ARG(4))); }
void rt_RegOpenKeyExW(_cpu *cpu) { RET(open_key(ARG(0), ARG(1), true, ARG(3), ARG(4))); }

static uint32_t create_key(uint32_t hkey, uint32_t subkey, bool wide, uint32_t options, uint32_t sam, uint32_t result, uint32_t disposition)
{
    reg_key *base, *k;
    uint32_t access, err, h;
    bool created = false;
    char *name;
    if (result == 0) return ERROR_INVALID_PARAMETER;
    if (options & ~(REG_OPTION_VOLATILE | 0x2u | 0x4u | 0x8u)) return ERROR_INVALID_PARAMETER;
    if (options & 0x2u) return ERROR_INVALID_PARAMETER; /* REG_OPTION_CREATE_LINK: no symbolic links */
    name = guest_name(subkey, wide, 32767u);
    if (name == NULL) return ERROR_INVALID_PARAMETER;
    lock();
    err = resolve(hkey, &base, &access);
    if (err == ERROR_SUCCESS)
    {
        if (name[0] != 0 && !(access & KEY_CREATE_SUB_KEY)) err = ERROR_ACCESS_DENIED;
        else err = walk(base, name, true, (options & REG_OPTION_VOLATILE) != 0, &k, &created);
        key_unref(base);
        if (err == ERROR_SUCCESS)
        {
            if (created) key_changed(k);
            h = handle_for(k, map_access(sam));
            key_unref(k);
            if (h == 0) err = ERROR_NOT_ENOUGH_MEMORY;
            else
            {
                wr32(result, h);
                if (disposition != 0) wr32(disposition, created ? REG_CREATED_NEW_KEY : REG_OPENED_EXISTING_KEY);
            }
        }
    }
    unlock();
    free(name);
    return err;
}

/* LSTATUS RegCreateKeyExW(HKEY, LPCWSTR subkey, DWORD reserved, LPWSTR
   class, DWORD options, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD
   disposition). The class string and the security descriptor are not
   kept. */
void rt_RegCreateKeyExW(_cpu *cpu)
{
    if (ARG(1) == 0)
    {
        RET(ERROR_BADKEY);
        return;
    }
    RET(create_key(ARG(0), ARG(1), true, ARG(4), ARG(5), ARG(7), ARG(8)));
}

/* LSTATUS RegCreateKeyA(HKEY, LPCSTR subkey, PHKEY). A NULL or empty
   subkey gives the same handle. */
void rt_RegCreateKeyA(_cpu *cpu)
{
    uint32_t hkey = ARG(0), subkey = ARG(1), result = ARG(2);
    if (result == 0)
    {
        RET(ERROR_INVALID_PARAMETER);
        return;
    }
    if (subkey == 0 || rd8(subkey) == 0)
    {
        reg_key *k;
        uint32_t access, err;
        lock();
        err = resolve(hkey, &k, &access);
        if (err == ERROR_SUCCESS) key_unref(k);
        unlock();
        if (err == ERROR_SUCCESS) wr32(result, hkey);
        RET(err);
        return;
    }
    RET(create_key(hkey, subkey, false, 0, KEY_ALL_ACCESS, result, 0));
}

static uint32_t query_value(uint32_t hkey, uint32_t vname, bool wide, uint32_t reserved, uint32_t ptype, uint32_t pdata, uint32_t pcb)
{
    reg_key *k;
    uint32_t access, err, need, cb;
    reg_value *v;
    char *name;
    if (reserved != 0) return ERROR_INVALID_PARAMETER;
    if (pdata != 0 && pcb == 0) return ERROR_INVALID_PARAMETER;
    name = guest_name(vname, wide, MAX_VALUE_NAME);
    if (name == NULL) return ERROR_INVALID_PARAMETER;
    lock();
    err = resolve(hkey, &k, &access);
    if (err == ERROR_SUCCESS)
    {
        if (!(access & KEY_QUERY_VALUE)) err = ERROR_ACCESS_DENIED;
        else if ((v = find_value(k, name)) == NULL) err = ERROR_FILE_NOT_FOUND;
        else
        {
            uint8_t *conv = NULL;
            const uint8_t *src = v->data;
            need = v->size;
            if (!wide && is_string_type(v->type))
            {
                conv = utf16_to_ansi(v->data, v->size, &need);
                src = conv;
            }
            if (ptype != 0) wr32(ptype, v->type);
            if (pcb != 0)
            {
                cb = rd32(pcb);
                if (pdata != 0)
                {
                    if (cb < need) err = ERROR_MORE_DATA;
                    else if (need != 0) memcpy(G2H(pdata), src, need);
                }
                wr32(pcb, need);
            }
            free(conv);
        }
        key_unref(k);
    }
    unlock();
    free(name);
    return err;
}

/* LSTATUS RegQueryValueExA/W(HKEY, LPCSTR name, LPDWORD reserved, LPDWORD
   type, LPBYTE data, LPDWORD cbData) */
void rt_RegQueryValueExA(_cpu *cpu) { RET(query_value(ARG(0), ARG(1), false, ARG(2), ARG(3), ARG(4), ARG(5))); }
void rt_RegQueryValueExW(_cpu *cpu) { RET(query_value(ARG(0), ARG(1), true, ARG(2), ARG(3), ARG(4), ARG(5))); }

static uint32_t set_value(uint32_t hkey, uint32_t vname, bool wide, uint32_t type, uint32_t pdata, uint32_t cb)
{
    reg_key *k;
    uint32_t access, err;
    char *name;
    if (cb > MAX_VALUE_DATA) return ERROR_INVALID_PARAMETER;
    if (cb != 0 && pdata == 0) return 998u; /* ERROR_NOACCESS */
    if (cb != 0 && !rt_va_is_committed(pdata, cb)) return 998u;
    name = guest_name(vname, wide, MAX_VALUE_NAME);
    if (name == NULL) return ERROR_INVALID_PARAMETER;
    lock();
    err = resolve(hkey, &k, &access);
    if (err == ERROR_SUCCESS)
    {
        if (!(access & KEY_SET_VALUE)) err = ERROR_ACCESS_DENIED;
        else
        {
            if (!wide && is_string_type(type))
            {
                uint32_t n;
                uint8_t *w = ansi_to_utf16(G2H(pdata), cb, &n);
                value_set(k, name, type, w, n);
                free(w);
            }
            else
                value_set(k, name, type, G2H(pdata), cb);
            key_changed(k);
        }
        key_unref(k);
    }
    unlock();
    free(name);
    return err;
}

/* LSTATUS RegSetValueExA/W(HKEY, LPCSTR name, DWORD reserved, DWORD type,
   const BYTE *data, DWORD cbData) */
void rt_RegSetValueExA(_cpu *cpu) { RET(set_value(ARG(0), ARG(1), false, ARG(3), ARG(4), ARG(5))); }
void rt_RegSetValueExW(_cpu *cpu) { RET(set_value(ARG(0), ARG(1), true, ARG(3), ARG(4), ARG(5))); }

/* LSTATUS RegDeleteValueA(HKEY, LPCSTR name) */
void rt_RegDeleteValueA(_cpu *cpu)
{
    reg_key *k;
    uint32_t access, err;
    char *name = guest_name(ARG(1), false, MAX_VALUE_NAME);
    if (name == NULL)
    {
        RET(ERROR_INVALID_PARAMETER);
        return;
    }
    lock();
    err = resolve(ARG(0), &k, &access);
    if (err == ERROR_SUCCESS)
    {
        if (!(access & KEY_SET_VALUE)) err = ERROR_ACCESS_DENIED;
        else if (!value_delete(k, name)) err = ERROR_FILE_NOT_FOUND;
        else key_changed(k);
        key_unref(k);
    }
    unlock();
    free(name);
    RET(err);
}

/* LSTATUS SHDeleteKeyA(HKEY, LPCSTR subkey): delete the key and all of its
   subkeys. */
void rt_SHDeleteKeyA(_cpu *cpu)
{
    reg_key *base, *k;
    uint32_t access, err;
    char *name = guest_name(ARG(1), false, 32767u);
    if (name == NULL)
    {
        RET(ERROR_INVALID_PARAMETER);
        return;
    }
    lock();
    err = resolve(ARG(0), &base, &access);
    if (err == ERROR_SUCCESS)
    {
        err = walk(base, name, false, false, &k, NULL);
        key_unref(base);
        if (err == ERROR_SUCCESS)
        {
            if (k->parent == NULL) err = ERROR_ACCESS_DENIED; /* a root key */
            else
            {
                reg_key *p = k->parent;
                bool saved = !k->is_volatile;
                key_delete_tree(k);
                if (saved) key_changed(p);
            }
            key_unref(k);
        }
    }
    unlock();
    free(name);
    RET(err);
}
