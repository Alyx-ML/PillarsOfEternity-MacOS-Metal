/* The emulated file system and the KERNEL32, SHELL32 and SHLWAPI file
   functions. See rt_file.h for the guest directory layout. */
#include "rt_file.h"
#include "rt_hid.h"
#include "rt_module.h"
#include "rt_object.h"
#include "rt_text.h"
#include "rt_thread.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <copyfile.h>
#include <sys/attr.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#define ERROR_INVALID_FUNCTION 1u
#define ERROR_WRITE_PROTECT 19u
#define ERROR_SHARING_VIOLATION 32u
#define ERROR_BAD_NETPATH 53u
#define ERROR_DISK_FULL 112u
#define ERROR_INVALID_NAME 123u
#define ERROR_DIR_NOT_EMPTY 145u
#define ERROR_NEGATIVE_SEEK 131u
#define ERROR_FILENAME_EXCED_RANGE 206u
#define ERROR_DIRECTORY 267u

#define INVALID_HANDLE_VALUE 0xFFFFFFFFu

/* The volume serial number of drive C (GetVolumeInformation and
   GetFileInformationByHandle agree). */
#define RT_VOLUME_SERIAL 0x50494C4Cu

typedef struct fs_mount {
    const char *guest; /* normalized, for example "C:\\Prototype" */
    char host[RT_HOST_PATH];
} fs_mount;

/* The runtime mount (index 3) is optional: its host directory is empty
   until rt_fs_mount_runtime sets it, and the lookups skip it. */
#define NUM_MOUNTS 4
#define RUNTIME_MOUNT 3
static fs_mount mounts[NUM_MOUNTS] = {
    { RT_GUEST_GAME_DIR, "" },
    { "C:\\Users\\Player", "" },
    { "C:\\ProgramData", "" },
    { RT_GUEST_RUNTIME_DIR, "" },
};

/* Directories above the mounts. They exist and contain only the mounts. */
static const char *const virtual_dirs[] = { "C:\\", "C:\\Users" };

static pthread_mutex_t cwd_lock = PTHREAD_MUTEX_INITIALIZER;
static char cwd[RT_PATH_BYTES] = RT_GUEST_GAME_DIR;

/* ---------------------------------------------------------------- setup */

static void make_dirs(const char *path)
{
    char buf[RT_HOST_PATH];
    size_t i, n = strlen(path);
    if (n >= sizeof(buf)) rt_fatal("host path too long: %s", path);
    memcpy(buf, path, n + 1);
    for (i = 1; i <= n; i++)
    {
        if (buf[i] == '/' || buf[i] == 0)
        {
            char c = buf[i];
            buf[i] = 0;
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) rt_fatal("cannot create directory %s (%s)", buf, strerror(errno));
            buf[i] = c;
        }
    }
}

static void set_mount(int i, const char *host, bool create)
{
    struct stat st;
    size_t n = strlen(host);
    if (n == 0 || n >= RT_HOST_PATH - 1) rt_fatal("bad host directory \"%s\"", host);
    memcpy(mounts[i].host, host, n + 1);
    while (n > 1 && mounts[i].host[n - 1] == '/') mounts[i].host[--n] = 0;
    if (create) make_dirs(mounts[i].host);
    if (stat(mounts[i].host, &st) != 0 || !S_ISDIR(st.st_mode)) rt_fatal("host directory %s does not exist", mounts[i].host);
}

void rt_fs_init(const char *game_dir, const char *user_dir, const char *programdata_dir)
{
    char sub[RT_HOST_PATH];
    static const char *const user_subdirs[] = { "Documents", "AppData/Roaming", "AppData/Local", "AppData/Local/Temp" };
    size_t i;
    set_mount(0, game_dir, false);
    set_mount(1, user_dir, true);
    set_mount(2, programdata_dir, true);
    for (i = 0; i < sizeof(user_subdirs) / sizeof(user_subdirs[0]); i++)
    {
        if ((size_t)snprintf(sub, sizeof(sub), "%s/%s", mounts[1].host, user_subdirs[i]) >= sizeof(sub))
            rt_fatal("host path too long");
        make_dirs(sub);
    }
}

void rt_fs_mount_runtime(const char *host_dir)
{
    if (mounts[RUNTIME_MOUNT].host[0] != 0) rt_fatal("the runtime directory is mounted a second time");
    set_mount(RUNTIME_MOUNT, host_dir, false);
}

/* ---------------------------------------------------------------- paths */

uint32_t rt_errno_to_win32(int e, uint32_t not_found)
{
    switch (e)
    {
    case ENOENT: return not_found;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EISDIR: return ERROR_ACCESS_DENIED;
    case EROFS: return ERROR_WRITE_PROTECT;
    case EEXIST: return ERROR_FILE_EXISTS;
    case EMFILE:
    case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
    case ENOSPC:
    case EDQUOT: return ERROR_DISK_FULL;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EBUSY:
    case ETXTBSY: return ERROR_SHARING_VIOLATION;
    case EINVAL: return ERROR_INVALID_PARAMETER;
    default: return ERROR_ACCESS_DENIED;
    }
}

static bool strieq_n(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
    {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
        if (a[i] == 0) return true;
    }
    return true;
}

static void get_cwd(char *out, size_t n)
{
    if (pthread_mutex_lock(&cwd_lock) != 0) rt_fatal("cwd lock failed");
    snprintf(out, n, "%s", cwd);
    if (pthread_mutex_unlock(&cwd_lock) != 0) rt_fatal("cwd unlock failed");
}

uint32_t rt_path_full(const char *in, char *out, size_t n)
{
    char tmp[RT_PATH_BYTES * 2 + 8], base[RT_PATH_BYTES];
    char *comp[RT_PATH_BYTES / 2];
    size_t i, len = strlen(in), ncomp = 0, pos;
    char drive;
    char *p, *s;

    if (len == 0) return ERROR_PATH_NOT_FOUND;
    if (len >= RT_PATH_BYTES) return ERROR_FILENAME_EXCED_RANGE;
    memcpy(tmp, in, len + 1);
    for (i = 0; i < len; i++)
        if (tmp[i] == '/') tmp[i] = '\\';
    p = tmp;
    if (p[0] == '\\' && p[1] == '\\')
    {
        if ((p[2] == '?' || p[2] == '.') && p[3] == '\\' && isalpha((unsigned char)p[4]) && p[5] == ':')
            p += 4; /* \\?\C:\... */
        else
            return ERROR_BAD_NETPATH;
    }

    get_cwd(base, sizeof(base));
    if (isalpha((unsigned char)p[0]) && p[1] == ':')
    {
        drive = (char)toupper((unsigned char)p[0]);
        p += 2;
        if (*p != '\\')
        {
            /* Drive-relative path: relative to the current directory of
               that drive. Only drive C has a current directory. */
            if (drive != 'C') return ERROR_PATH_NOT_FOUND;
            if (strlen(base) + 1 + strlen(p) >= sizeof(tmp) - 8) return ERROR_FILENAME_EXCED_RANGE;
            char joined[sizeof(tmp)];
            snprintf(joined, sizeof(joined), "%s\\%s", base + 2, p);
            memcpy(tmp, joined, strlen(joined) + 1);
            p = tmp;
        }
    }
    else if (p[0] == '\\')
    {
        drive = base[0];
    }
    else
    {
        char joined[sizeof(tmp)];
        drive = base[0];
        if (strlen(base) + 1 + strlen(p) >= sizeof(joined)) return ERROR_FILENAME_EXCED_RANGE;
        snprintf(joined, sizeof(joined), "%s\\%s", base + 2, p);
        memcpy(tmp, joined, strlen(joined) + 1);
        p = tmp;
    }

    /* p is now "\..." relative to the drive root. */
    for (s = strtok_r(p, "\\", &p); s != NULL; s = strtok_r(NULL, "\\", &p))
    {
        size_t cl = strlen(s);
        if (strcmp(s, ".") == 0) continue;
        if (strcmp(s, "..") == 0)
        {
            if (ncomp > 0) ncomp--;
            continue;
        }
        /* Windows removes trailing dots and spaces from a component. */
        while (cl > 0 && (s[cl - 1] == '.' || s[cl - 1] == ' ')) s[--cl] = 0;
        if (cl == 0) continue;
        for (i = 0; i < cl; i++)
        {
            unsigned char c = (unsigned char)s[i];
            if (c < 32 || strchr("<>:\"|?*", c) != NULL) return ERROR_INVALID_NAME;
        }
        comp[ncomp++] = s;
    }
    pos = 0;
    if (n < 4) return ERROR_FILENAME_EXCED_RANGE;
    out[pos++] = drive;
    out[pos++] = ':';
    out[pos++] = '\\';
    for (i = 0; i < ncomp; i++)
    {
        size_t cl = strlen(comp[i]);
        if (pos + cl + 2 > n || pos + cl + 1 > RT_PATH_BYTES) return ERROR_FILENAME_EXCED_RANGE;
        if (i > 0) out[pos++] = '\\';
        memcpy(out + pos, comp[i], cl);
        pos += cl;
    }
    out[pos] = 0;
    {
        /* MAX_PATH counts UTF-16 units with the NUL. */
        size_t units;
        if (!rt_utf16_length(out, &units)) return ERROR_INVALID_NAME;
        if (units + 1 > RT_MAX_PATH) return ERROR_FILENAME_EXCED_RANGE;
    }
    return 0;
}

static bool is_virtual_dir(const char *full)
{
    size_t i;
    for (i = 0; i < sizeof(virtual_dirs) / sizeof(virtual_dirs[0]); i++)
        if (strlen(full) == strlen(virtual_dirs[i]) && strieq_n(full, virtual_dirs[i], strlen(full))) return true;
    return false;
}

/* Find the host name of component c in host directory dir (case-insensitive).
   Returns true and writes the name if it exists. */
static bool find_component(const char *dir, const char *c, char *name, size_t n)
{
    char probe[RT_HOST_PATH];
    struct stat st;
    DIR *d;
    struct dirent *e;
    bool found = false;

    if ((size_t)snprintf(probe, sizeof(probe), "%s/%s", dir, c) < sizeof(probe) && lstat(probe, &st) == 0)
    {
        snprintf(name, n, "%s", c);
        return true;
    }
    d = opendir(dir);
    if (d == NULL) return false;
    while ((e = readdir(d)) != NULL)
    {
        if (strcasecmp(e->d_name, c) == 0)
        {
            snprintf(name, n, "%s", e->d_name);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

uint32_t rt_path_host(const char *guest, char *host, size_t n, bool *exists)
{
    char full[RT_PATH_BYTES];
    uint32_t err = rt_path_full(guest, full, sizeof(full));
    const fs_mount *m = NULL;
    size_t i, mlen = 0;
    char rest[RT_PATH_BYTES];
    char *s, *save;
    size_t pos;

    if (err != 0) return err;
    *exists = false;
    for (i = 0; i < NUM_MOUNTS; i++)
    {
        size_t l = strlen(mounts[i].guest);
        if (mounts[i].host[0] == 0) continue;
        if (strieq_n(full, mounts[i].guest, l) && (full[l] == 0 || full[l] == '\\') && l > mlen)
        {
            m = &mounts[i];
            mlen = l;
        }
    }
    if (m == NULL) return is_virtual_dir(full) ? ERROR_ACCESS_DENIED : ERROR_PATH_NOT_FOUND;

    pos = strlen(m->host);
    if (pos + 1 > n) return ERROR_FILENAME_EXCED_RANGE;
    memcpy(host, m->host, pos + 1);
    snprintf(rest, sizeof(rest), "%s", full + mlen);
    *exists = true;
    for (s = strtok_r(rest, "\\", &save); s != NULL; s = strtok_r(NULL, "\\", &save))
    {
        char name[256];
        bool last = (save == NULL || *save == 0);
        if (!*exists) return ERROR_PATH_NOT_FOUND; /* a missing directory before this component */
        if (find_component(host, s, name, sizeof(name)))
        {
            struct stat st;
            if (pos + 1 + strlen(name) + 1 > n) return ERROR_FILENAME_EXCED_RANGE;
            host[pos++] = '/';
            memcpy(host + pos, name, strlen(name) + 1);
            pos += strlen(name);
            if (!last && (stat(host, &st) != 0 || !S_ISDIR(st.st_mode))) return ERROR_PATH_NOT_FOUND;
        }
        else
        {
            if (pos + 1 + strlen(s) + 1 > n) return ERROR_FILENAME_EXCED_RANGE;
            host[pos++] = '/';
            memcpy(host + pos, s, strlen(s) + 1);
            pos += strlen(s);
            *exists = false;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- attributes and times */

#define FILE_ATTRIBUTE_READONLY 0x01u
#define FILE_ATTRIBUTE_HIDDEN 0x02u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_ARCHIVE 0x20u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu

#define FILETIME_UNIX_EPOCH UINT64_C(116444736000000000)

uint64_t rt_filetime_from_timespec(struct timespec ts)
{
    if (ts.tv_sec < -11644473600LL) return 0;
    return (uint64_t)((int64_t)ts.tv_sec * 10000000 + 116444736000000000LL) + (uint64_t)ts.tv_nsec / 100u;
}

uint32_t rt_file_attributes(const struct stat *st, const char *name)
{
    uint32_t a = 0;
    if (S_ISDIR(st->st_mode)) a |= FILE_ATTRIBUTE_DIRECTORY;
    else a |= FILE_ATTRIBUTE_ARCHIVE;
    if ((st->st_mode & S_IWUSR) == 0) a |= FILE_ATTRIBUTE_READONLY;
    if (name != NULL && name[0] == '.' && strcmp(name, ".") != 0 && strcmp(name, "..") != 0) a |= FILE_ATTRIBUTE_HIDDEN;
    return a;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* The string form of a guest function: ANSI ("A" functions) or wide
   ("W" functions). The runtime keeps paths as UTF-8 (rt_text.h). */
typedef struct str_form {
    int (*get)(char *dst, size_t max, uint32_t g);
    uint32_t (*put)(uint32_t buf, uint32_t n, const char *s);
    bool (*field)(uint32_t buf, uint32_t n, const char *s);
    uint32_t unit; /* bytes of one character: 1 or 2 */
} str_form;

static const str_form FORM_A = { rt_gstr_a, rt_put_astr, rt_put_afield, 1 };
static const str_form FORM_W = { rt_gstr_w, rt_put_wstr, rt_put_wfield, 2 };

/* Read a guest string argument with the error codes of the file
   functions. Returns false and sets the last error if the string is
   missing, too long or not valid. */
static bool path_arg(const str_form *sf, uint32_t g, char *buf, size_t n)
{
    int r;
    if (g == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        return false;
    }
    r = sf->get(buf, n, g);
    if (r != RT_STR_OK)
    {
        rt_set_last_error(r == RT_STR_INVALID ? ERROR_INVALID_NAME : ERROR_FILENAME_EXCED_RANGE);
        return false;
    }
    return true;
}

/* Number of characters (ANSI) or UTF-16 units (wide) in the first bytes
   bytes of the UTF-8 string s. */
static uint32_t form_units(const str_form *sf, const char *s, size_t bytes)
{
    char tmp[RT_PATH_BYTES];
    size_t units = 0, i = 0;
    if (bytes >= sizeof(tmp)) rt_fatal("form_units: string too long");
    memcpy(tmp, s, bytes);
    tmp[bytes] = 0;
    if (sf->unit == 2)
    {
        if (!rt_utf16_length(tmp, &units)) rt_fatal("runtime string is not valid UTF-8");
        return (uint32_t)units;
    }
    while (i < bytes)
    {
        uint32_t cp;
        int l = rt_utf8_decode((const uint8_t *)tmp + i, bytes - i, &cp);
        if (l < 0) rt_fatal("runtime string is not valid UTF-8");
        units++;
        i += (size_t)l;
    }
    return (uint32_t)units;
}

static void GetFileAttributes_form(_cpu *cpu, const str_form *sf)
{
    char gp[RT_PATH_BYTES], full[RT_PATH_BYTES], host[RT_HOST_PATH];
    struct stat st;
    bool exists;
    uint32_t err;

    if (!path_arg(sf, ARG(0), gp, sizeof(gp)))
    {
        RET(INVALID_FILE_ATTRIBUTES);
        return;
    }
    if (rt_path_full(gp, full, sizeof(full)) == 0 && is_virtual_dir(full))
    {
        RET(FILE_ATTRIBUTE_DIRECTORY);
        return;
    }
    err = rt_path_host(gp, host, sizeof(host), &exists);
    if (err == 0 && !exists) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && stat(host, &st) != 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(INVALID_FILE_ATTRIBUTES);
        return;
    }
    RET(rt_file_attributes(&st, base_name(host)));
}

void rt_GetFileAttributesA(_cpu *cpu)
{
    GetFileAttributes_form(cpu, &FORM_A);
}

void rt_GetFileAttributesW(_cpu *cpu)
{
    GetFileAttributes_form(cpu, &FORM_W);
}

static void SetFileAttributes_form(_cpu *cpu, const str_form *sf)
{
    char gp[RT_PATH_BYTES], host[RT_HOST_PATH];
    struct stat st;
    bool exists;
    uint32_t err, attr = ARG(1);
    mode_t mode;

    if (!path_arg(sf, ARG(0), gp, sizeof(gp)))
    {
        RET(0);
        return;
    }
    err = rt_path_host(gp, host, sizeof(host), &exists);
    if (err == 0 && !exists) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && stat(host, &st) != 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (err == 0)
    {
        /* Only the read-only attribute has a host equivalent. Hidden,
           system and archive attributes are not stored. */
        mode = st.st_mode & 07777;
        if (attr & FILE_ATTRIBUTE_READONLY) mode &= (mode_t)~(S_IWUSR | S_IWGRP | S_IWOTH);
        else mode |= S_IWUSR;
        if (chmod(host, mode) != 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    }
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

void rt_SetFileAttributesA(_cpu *cpu)
{
    SetFileAttributes_form(cpu, &FORM_A);
}

void rt_SetFileAttributesW(_cpu *cpu)
{
    SetFileAttributes_form(cpu, &FORM_W);
}

/* ---------------------------------------------------------------- file handles */

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define FILE_FLAG_OVERLAPPED 0x40000000u
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u

typedef struct file_data {
    int fd;
    bool is_dir;
    bool delete_on_close;
    bool is_pipe;          /* one end of an anonymous pipe (CreatePipe) */
    char host[RT_HOST_PATH];
} file_data;

static void locks_release(const file_data *f);

int rt_file_object_fd(rt_object *o)
{
    const file_data *f = o->data;
    if (o->type != OBJ_FILE) rt_fatal("rt_file_object_fd: not a file object");
    return f->is_dir ? -1 : f->fd;
}

static void file_destroy(rt_object *o)
{
    file_data *f = o->data;
    locks_release(f);
    if (f->fd >= 0 && close(f->fd) != 0) rt_log("close of %s failed (%s)", f->host, strerror(errno));
    if (f->delete_on_close && unlink(f->host) != 0) rt_log("delete on close of %s failed (%s)", f->host, strerror(errno));
    free(f);
}

int rt_file_fd(uint32_t h)
{
    rt_object *o = rt_handle_get(h, OBJ_FILE);
    int fd;
    if (o == NULL) return -1;
    fd = ((file_data *)o->data)->fd;
    rt_object_unref(o);
    return fd;
}

static void CreateFile_form(_cpu *cpu, const str_form *sf)
{
    char gp[RT_PATH_BYTES], host[RT_HOST_PATH];
    uint32_t access = ARG(1), disp = ARG(4), flags = ARG(5);
    bool exists;
    uint32_t err, last = ERROR_SUCCESS;
    int oflags = O_CLOEXEC, fd;
    struct stat st;
    rt_object *o;
    file_data *f;
    uint32_t h;

    if (!path_arg(sf, ARG(0), gp, sizeof(gp)))
    {
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    if (rt_hid_is_path(gp))
    {
        /* A HID device interface (raw input device names, rt_hid.c). */
        uint32_t hid_err = 0;
        if (disp != OPEN_EXISTING) rt_fatal("CreateFile(\"%s\"): a HID device needs OPEN_EXISTING", gp);
        if (ARG(6) != 0) rt_fatal("CreateFile with a template file is not supported");
        h = rt_hid_open(gp, access, flags, &hid_err);
        if (h == 0) rt_set_last_error(hid_err);
        RET(h != 0 ? h : INVALID_HANDLE_VALUE);
        return;
    }
    if (strncmp(gp, "\\\\.\\", 4) == 0 && !(isalpha((unsigned char)gp[4]) && gp[5] == ':' && gp[6] != 0))
    {
        /* A device (\\.\name). The emulated system has no devices. */
        rt_log("CreateFileA(\"%s\"): no such device in the emulated system", gp);
        rt_set_last_error(ERROR_FILE_NOT_FOUND);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    if (flags & FILE_FLAG_OVERLAPPED) rt_fatal("CreateFileA(\"%s\") with FILE_FLAG_OVERLAPPED is not supported", gp);
    if (ARG(6) != 0) rt_fatal("CreateFileA with a template file is not supported");

    err = rt_path_host(gp, host, sizeof(host), &exists);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    if (exists && stat(host, &st) == 0 && S_ISDIR(st.st_mode))
    {
        if (!(flags & FILE_FLAG_BACKUP_SEMANTICS) || (disp != OPEN_EXISTING && disp != OPEN_ALWAYS))
        {
            rt_set_last_error(ERROR_ACCESS_DENIED);
            RET(INVALID_HANDLE_VALUE);
            return;
        }
        oflags |= O_RDONLY | O_DIRECTORY;
    }
    else
    {
        /* GENERIC_ALL 0x10000000; FILE_READ_DATA 1; FILE_WRITE_DATA 2;
           FILE_APPEND_DATA 4. Other rights (attributes, DELETE,
           SYNCHRONIZE) need no host access mode. */
        bool rd = (access & (GENERIC_READ | 0x10000000u | 0x1u)) != 0;
        bool wrt = (access & (GENERIC_WRITE | 0x10000000u | 0x2u | 0x4u)) != 0;
        if (rd && wrt) oflags |= O_RDWR;
        else if (wrt) oflags |= O_WRONLY;
        else oflags |= O_RDONLY;
        switch (disp)
        {
        case CREATE_NEW: oflags |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS: oflags |= O_CREAT | O_TRUNC; if (exists) last = ERROR_ALREADY_EXISTS; break;
        case OPEN_EXISTING: break;
        case OPEN_ALWAYS: oflags |= O_CREAT; if (exists) last = ERROR_ALREADY_EXISTS; break;
        case TRUNCATE_EXISTING:
            if ((oflags & O_ACCMODE) == O_RDONLY)
            {
                rt_set_last_error(ERROR_INVALID_PARAMETER);
                RET(INVALID_HANDLE_VALUE);
                return;
            }
            oflags |= O_TRUNC;
            break;
        default:
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(INVALID_HANDLE_VALUE);
            return;
        }
        if (!exists && (disp == OPEN_EXISTING || disp == TRUNCATE_EXISTING))
        {
            rt_set_last_error(ERROR_FILE_NOT_FOUND);
            RET(INVALID_HANDLE_VALUE);
            return;
        }
    }
    fd = open(host, oflags, 0644);
    if (fd < 0)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND));
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    f = calloc(1, sizeof(*f));
    if (f == NULL) rt_fatal("out of host memory for a file");
    f->fd = fd;
    f->is_dir = (oflags & O_DIRECTORY) != 0;
    f->delete_on_close = (flags & FILE_FLAG_DELETE_ON_CLOSE) != 0;
    snprintf(f->host, sizeof(f->host), "%s", host);
    if ((flags & 0x1u) && (oflags & O_CREAT) && !exists)
    {
        /* FILE_ATTRIBUTE_READONLY on a new file. */
        if (fchmod(fd, 0444) != 0) rt_log("fchmod of %s failed (%s)", host, strerror(errno));
    }
    o = rt_object_new(OBJ_FILE);
    o->data = f;
    o->destroy = file_destroy;
    h = rt_handle_new(o);
    rt_object_unref(o);
    if (h == 0)
    {
        rt_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    rt_set_last_error(last);
    RET(h);
}

void rt_CreateFileA(_cpu *cpu)
{
    CreateFile_form(cpu, &FORM_A);
}

void rt_CreateFileW(_cpu *cpu)
{
    CreateFile_form(cpu, &FORM_W);
}

static file_data *file_arg(uint32_t h, rt_object **o)
{
    *o = rt_handle_get(h, OBJ_FILE);
    if (*o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return (*o)->data;
}

/* OVERLAPPED: Internal, InternalHigh, Offset, OffsetHigh, hEvent. With a
   handle without FILE_FLAG_OVERLAPPED the operation is synchronous at the
   given offset. */
static bool seek_overlapped(int fd, uint32_t ov)
{
    off_t off;
    if (ov == 0) return true;
    off = (off_t)(((uint64_t)rd32(ov + 12) << 32) | rd32(ov + 8));
    return lseek(fd, off, SEEK_SET) >= 0;
}

static void finish_overlapped(uint32_t ov, uint32_t n, uint32_t status)
{
    if (ov == 0) return;
    wr32(ov + 0, status);
    wr32(ov + 4, n);
    if (rd32(ov + 16) != 0)
    {
        rt_object *e = rt_handle_get(rd32(ov + 16), OBJ_EVENT);
        if (e != NULL)
        {
            rt_obj_lock();
            e->signaled = true;
            rt_obj_signal(e, e->manual_reset ? 0u : 1u);
            rt_obj_unlock();
            rt_object_unref(e);
        }
    }
}

/* BOOL CancelIo(HANDLE). The runtime does every file and socket operation
   at the time of the call (CreateFile refuses FILE_FLAG_OVERLAPPED), so no
   operation is pending. HID device handles have pending reads (rt_hid.c). */
void rt_CancelIo(_cpu *cpu)
{
    rt_object *o;
    if (rt_hid_is_handle(ARG(0)))
    {
        rt_hid_cancel_io(cpu);
        return;
    }
    o = rt_handle_get(ARG(0), 0);
    if (o == NULL || (o->type != OBJ_FILE && o->type != OBJ_SOCKET))
    {
        if (o != NULL) rt_object_unref(o);
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    rt_object_unref(o);
    RET(1);
}

/* BOOL GetOverlappedResult(HANDLE, LPOVERLAPPED, LPDWORD transferred,
   BOOL wait). The result is the status (Internal) and the byte count
   (InternalHigh) of the OVERLAPPED structure. Only reads of HID device
   handles can be pending (rt_hid.c); file and socket operations are
   complete at the time of the call. */
void rt_GetOverlappedResult(_cpu *cpu)
{
    uint32_t ov = ARG(1), out = ARG(2), status;
    rt_object *o = rt_handle_get(ARG(0), 0);
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    rt_object_unref(o);
    if (ov == 0 || !rt_va_is_committed(ov, 20))
    {
        rt_set_last_error(998u); /* ERROR_NOACCESS */
        RET(0);
        return;
    }
    status = __atomic_load_n((uint32_t *)G2H(ov), __ATOMIC_ACQUIRE);
    if (status == 0x103u) /* STATUS_PENDING */
    {
        if (!rt_hid_is_handle(ARG(0)))
            rt_fatal("GetOverlappedResult: the OVERLAPPED structure has STATUS_PENDING, but only HID reads can be pending");
        if (ARG(3) == 0)
        {
            rt_set_last_error(996u); /* ERROR_IO_INCOMPLETE */
            RET(0);
            return;
        }
        if (rd32(ov + 16) != 0)
        {
            rt_object *e = rt_handle_get(rd32(ov + 16), OBJ_EVENT);
            if (e == NULL)
            {
                rt_set_last_error(ERROR_INVALID_HANDLE);
                RET(0);
                return;
            }
            while (__atomic_load_n((uint32_t *)G2H(ov), __ATOMIC_ACQUIRE) == 0x103u)
                (void)rt_wait_object_list(cpu, 1, &e, false, 0xFFFFFFFFu, false, NULL);
            rt_object_unref(e);
        }
        else
            rt_hid_wait_overlapped(ARG(0), ov);
        status = __atomic_load_n((uint32_t *)G2H(ov), __ATOMIC_ACQUIRE);
    }
    if (out != 0) wr32(out, rd32(ov + 4));
    switch (status)
    {
    case 0: RET(1); return;
    case 0xC0000011u: rt_set_last_error(ERROR_HANDLE_EOF); break;     /* STATUS_END_OF_FILE */
    case 0xC0000120u: rt_set_last_error(995u); break;                 /* STATUS_CANCELLED: ERROR_OPERATION_ABORTED */
    case 0x80000005u: rt_set_last_error(ERROR_MORE_DATA); break;      /* STATUS_BUFFER_OVERFLOW */
    case 0xC000009Du: rt_set_last_error(1167u); break;                /* STATUS_DEVICE_NOT_CONNECTED: ERROR_DEVICE_NOT_CONNECTED */
    default: rt_fatal("GetOverlappedResult: status 0x%08x has no Win32 error in the runtime", status);
    }
    RET(0);
}

void rt_ReadFile(_cpu *cpu)
{
    rt_object *o;
    file_data *f;
    uint32_t buf = ARG(1), n = ARG(2), out = ARG(3), ov = ARG(4), done = 0;
    bool ok = true;

    if (rt_hid_is_handle(ARG(0)))
    {
        rt_hid_read_file(cpu);
        return;
    }
    f = file_arg(ARG(0), &o);

    if (f == NULL)
    {
        RET(0);
        return;
    }
    if (out != 0) wr32(out, 0);
    if (n > 0 && !rt_va_is_committed(buf, n))
    {
        rt_object_unref(o);
        rt_set_last_error(998u); /* ERROR_NOACCESS */
        RET(0);
        return;
    }
    if (!seek_overlapped(f->fd, ov))
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_PARAMETER));
        ok = false;
    }
    while (ok && done < n)
    {
        ssize_t r = read(f->fd, (uint8_t *)G2H(buf) + done, n - done);
        if (r < 0)
        {
            if (errno == EINTR) continue;
            rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
            ok = false;
            break;
        }
        if (r == 0) break;
        done += (uint32_t)r;
        /* A pipe read returns the data that is available, as on Windows. */
        if (f->is_pipe) break;
    }
    if (ok && f->is_pipe && done == 0 && n > 0)
    {
        /* All write ends are closed. */
        rt_set_last_error(109u); /* ERROR_BROKEN_PIPE */
        ok = false;
    }
    rt_object_unref(o);
    if (out != 0) wr32(out, done);
    if (ok && ov != 0 && done == 0 && n > 0)
    {
        /* With an OVERLAPPED structure, a read at the end of the file fails
           with ERROR_HANDLE_EOF. */
        finish_overlapped(ov, 0, 0xC0000011u);
        rt_set_last_error(ERROR_HANDLE_EOF);
        RET(0);
        return;
    }
    if (ok) finish_overlapped(ov, done, 0);
    RET(ok ? 1 : 0);
}

void rt_WriteFile(_cpu *cpu)
{
    rt_object *o;
    file_data *f;
    uint32_t buf = ARG(1), n = ARG(2), out = ARG(3), ov = ARG(4), done = 0;
    bool ok = true;

    if (rt_hid_is_handle(ARG(0)))
    {
        rt_hid_write_file(cpu);
        return;
    }
    f = file_arg(ARG(0), &o);

    if (f == NULL)
    {
        RET(0);
        return;
    }
    if (out != 0) wr32(out, 0);
    if (n > 0 && !rt_va_is_committed(buf, n))
    {
        rt_object_unref(o);
        rt_set_last_error(998u);
        RET(0);
        return;
    }
    if (!seek_overlapped(f->fd, ov))
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_PARAMETER));
        ok = false;
    }
    while (ok && done < n)
    {
        ssize_t r = write(f->fd, (const uint8_t *)G2H(buf) + done, n - done);
        if (r < 0)
        {
            if (errno == EINTR) continue;
            /* EPIPE: the read end of a pipe is closed (ERROR_NO_DATA). */
            rt_set_last_error(errno == EPIPE ? 232u : rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
            ok = false;
            break;
        }
        done += (uint32_t)r;
    }
    rt_object_unref(o);
    if (out != 0) wr32(out, done);
    if (ok) finish_overlapped(ov, done, 0);
    RET(ok ? 1 : 0);
}

void rt_SetFilePointer(_cpu *cpu)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    int32_t lo = (int32_t)ARG(1);
    uint32_t hi_ptr = ARG(2), method = ARG(3);
    int64_t dist;
    off_t r;
    int whence;

    if (f == NULL)
    {
        RET(0xFFFFFFFFu);
        return;
    }
    dist = hi_ptr ? (int64_t)(((uint64_t)rd32(hi_ptr) << 32) | (uint32_t)lo) : (int64_t)lo;
    switch (method)
    {
    case 0: whence = SEEK_SET; break;
    case 1: whence = SEEK_CUR; break;
    case 2: whence = SEEK_END; break;
    default:
        rt_object_unref(o);
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0xFFFFFFFFu);
        return;
    }
    {
        off_t cur = lseek(f->fd, 0, whence);
        if (cur < 0 || (int64_t)cur + dist < 0)
        {
            rt_object_unref(o);
            rt_set_last_error(cur < 0 ? rt_errno_to_win32(errno, ERROR_INVALID_HANDLE) : ERROR_NEGATIVE_SEEK);
            RET(0xFFFFFFFFu);
            return;
        }
        r = lseek(f->fd, (off_t)((int64_t)cur + dist), SEEK_SET);
    }
    rt_object_unref(o);
    if (r < 0)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
        RET(0xFFFFFFFFu);
        return;
    }
    if (hi_ptr) wr32(hi_ptr, (uint32_t)((uint64_t)r >> 32));
    else if ((uint64_t)r > 0xFFFFFFFFu)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0xFFFFFFFFu);
        return;
    }
    rt_set_last_error(ERROR_SUCCESS);
    RET((uint32_t)r);
}

void rt_GetFileSize(_cpu *cpu)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    struct stat st;
    int r;
    if (f == NULL)
    {
        RET(0xFFFFFFFFu);
        return;
    }
    r = fstat(f->fd, &st);
    rt_object_unref(o);
    if (r != 0)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
        RET(0xFFFFFFFFu);
        return;
    }
    if (ARG(1) != 0) wr32(ARG(1), (uint32_t)((uint64_t)st.st_size >> 32));
    rt_set_last_error(ERROR_SUCCESS);
    RET((uint32_t)st.st_size);
}

void rt_GetFileTime(_cpu *cpu)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    struct stat st;
    int r;
    if (f == NULL)
    {
        RET(0);
        return;
    }
    r = fstat(f->fd, &st);
    rt_object_unref(o);
    if (r != 0)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
        RET(0);
        return;
    }
    if (ARG(1) != 0) wr64(ARG(1), rt_filetime_from_timespec(st.st_birthtimespec));
    if (ARG(2) != 0) wr64(ARG(2), rt_filetime_from_timespec(st.st_atimespec));
    if (ARG(3) != 0) wr64(ARG(3), rt_filetime_from_timespec(st.st_mtimespec));
    RET(1);
}

void rt_FlushFileBuffers(_cpu *cpu)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    int r;
    if (f == NULL)
    {
        RET(0);
        return;
    }
    r = fsync(f->fd);
    rt_object_unref(o);
    if (r != 0)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
        RET(0);
        return;
    }
    RET(1);
}

void rt_SetEndOfFile(_cpu *cpu)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    off_t pos;
    int r = -1;
    if (f == NULL)
    {
        RET(0);
        return;
    }
    pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos >= 0) r = ftruncate(f->fd, pos);
    rt_object_unref(o);
    if (r != 0)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
        RET(0);
        return;
    }
    RET(1);
}

/* Devices are not part of the emulated system, so no handle accepts a
   device control code. */
void rt_DeviceIoControl(_cpu *cpu)
{
    rt_object *o = rt_handle_get(ARG(0), 0);
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    rt_object_unref(o);
    if (ARG(6) != 0) wr32(ARG(6), 0);
    rt_set_last_error(ERROR_INVALID_FUNCTION);
    RET(0);
}

/* ---------------------------------------------------------------- directories and names */

static uint32_t host_of_arg(const str_form *sf, uint32_t g, char *host, size_t n, bool *exists)
{
    char gp[RT_PATH_BYTES];
    if (!path_arg(sf, g, gp, sizeof(gp))) return rt_get_last_error();
    return rt_path_host(gp, host, n, exists);
}

static void CreateDirectory_form(_cpu *cpu, const str_form *sf)
{
    char host[RT_HOST_PATH], gp[RT_PATH_BYTES], full[RT_PATH_BYTES];
    bool exists;
    uint32_t err;

    if (ARG(0) != 0 && sf->get(gp, sizeof(gp), ARG(0)) == RT_STR_OK && rt_path_full(gp, full, sizeof(full)) == 0 && is_virtual_dir(full))
    {
        rt_set_last_error(ERROR_ALREADY_EXISTS);
        RET(0);
        return;
    }
    err = host_of_arg(sf, ARG(0), host, sizeof(host), &exists);
    if (err == 0 && exists) err = ERROR_ALREADY_EXISTS;
    if (err == 0 && mkdir(host, 0755) != 0) err = rt_errno_to_win32(errno, ERROR_PATH_NOT_FOUND);
    if (err == ERROR_FILE_EXISTS) err = ERROR_ALREADY_EXISTS;
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

void rt_CreateDirectoryA(_cpu *cpu)
{
    CreateDirectory_form(cpu, &FORM_A);
}

void rt_CreateDirectoryW(_cpu *cpu)
{
    CreateDirectory_form(cpu, &FORM_W);
}

static void RemoveDirectory_form(_cpu *cpu, const str_form *sf)
{
    char host[RT_HOST_PATH];
    bool exists;
    uint32_t err = host_of_arg(sf, ARG(0), host, sizeof(host), &exists);
    if (err == 0 && !exists) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && rmdir(host) != 0) err = (errno == ENOTDIR) ? ERROR_DIRECTORY : rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

void rt_RemoveDirectoryA(_cpu *cpu)
{
    RemoveDirectory_form(cpu, &FORM_A);
}

void rt_RemoveDirectoryW(_cpu *cpu)
{
    RemoveDirectory_form(cpu, &FORM_W);
}

static void DeleteFile_form(_cpu *cpu, const str_form *sf)
{
    char host[RT_HOST_PATH];
    bool exists;
    struct stat st;
    uint32_t err = host_of_arg(sf, ARG(0), host, sizeof(host), &exists);
    if (err == 0 && !exists) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && stat(host, &st) == 0)
    {
        if (S_ISDIR(st.st_mode)) err = ERROR_ACCESS_DENIED;
        else if ((st.st_mode & S_IWUSR) == 0) err = ERROR_ACCESS_DENIED; /* read-only attribute */
    }
    if (err == 0 && unlink(host) != 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

void rt_DeleteFileA(_cpu *cpu)
{
    DeleteFile_form(cpu, &FORM_A);
}

void rt_DeleteFileW(_cpu *cpu)
{
    DeleteFile_form(cpu, &FORM_W);
}

static void MoveFile_form(_cpu *cpu, const str_form *sf)
{
    char from[RT_HOST_PATH], to[RT_HOST_PATH];
    bool e1, e2;
    uint32_t err = host_of_arg(sf, ARG(0), from, sizeof(from), &e1);
    if (err == 0) err = host_of_arg(sf, ARG(1), to, sizeof(to), &e2);
    if (err == 0 && !e1) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && e2 && strcasecmp(from, to) != 0) err = ERROR_ALREADY_EXISTS;
    if (err == 0 && rename(from, to) != 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

void rt_MoveFileA(_cpu *cpu)
{
    MoveFile_form(cpu, &FORM_A);
}

void rt_MoveFileW(_cpu *cpu)
{
    MoveFile_form(cpu, &FORM_W);
}

#define MOVEFILE_REPLACE_EXISTING 0x1u
#define MOVEFILE_COPY_ALLOWED 0x2u
#define MOVEFILE_DELAY_UNTIL_REBOOT 0x4u
#define MOVEFILE_WRITE_THROUGH 0x8u
#define ERROR_NOT_SAME_DEVICE 17u

/* BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags).
   MOVEFILE_REPLACE_EXISTING replaces a file (not a directory).
   MOVEFILE_COPY_ALLOWED copies a file to another volume and then deletes
   the source. MOVEFILE_WRITE_THROUGH returns after the data is on the
   disk. The emulated system does not restart, so
   MOVEFILE_DELAY_UNTIL_REBOOT stops the process. */
void rt_MoveFileExW(_cpu *cpu)
{
    char from[RT_HOST_PATH], to[RT_HOST_PATH];
    bool e1, e2 = false;
    uint32_t flags = ARG(2), err;
    struct stat sf, st;
    if (flags & ~(MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_DELAY_UNTIL_REBOOT | MOVEFILE_WRITE_THROUGH))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (flags & MOVEFILE_DELAY_UNTIL_REBOOT) rt_fatal("MoveFileExW: MOVEFILE_DELAY_UNTIL_REBOOT is not supported (the emulated system does not restart)");
    err = host_of_arg(&FORM_W, ARG(0), from, sizeof(from), &e1);
    if (err == 0) err = host_of_arg(&FORM_W, ARG(1), to, sizeof(to), &e2);
    if (err == 0 && !e1) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && stat(from, &sf) != 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (err == 0 && e2 && strcasecmp(from, to) != 0)
    {
        if (!(flags & MOVEFILE_REPLACE_EXISTING)) err = ERROR_ALREADY_EXISTS;
        else if (stat(to, &st) == 0 && (S_ISDIR(st.st_mode) || S_ISDIR(sf.st_mode))) err = ERROR_ACCESS_DENIED;
        else if ((st.st_mode & S_IWUSR) == 0) err = ERROR_ACCESS_DENIED; /* read-only attribute */
    }
    if (err == 0 && rename(from, to) != 0)
    {
        if (errno == EXDEV)
        {
            if (!(flags & MOVEFILE_COPY_ALLOWED) || S_ISDIR(sf.st_mode)) err = ERROR_NOT_SAME_DEVICE;
            else if (copyfile(from, to, NULL, COPYFILE_ALL) != 0) err = rt_errno_to_win32(errno, ERROR_PATH_NOT_FOUND);
            else if (unlink(from) != 0) err = rt_errno_to_win32(errno, ERROR_ACCESS_DENIED);
        }
        else
            err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    }
    if (err == 0 && (flags & MOVEFILE_WRITE_THROUGH))
    {
        int fd = open(to, O_RDONLY | O_CLOEXEC);
        if (fd < 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
        else
        {
            if (fcntl(fd, F_FULLFSYNC) != 0 && fsync(fd) != 0) err = rt_errno_to_win32(errno, ERROR_ACCESS_DENIED);
            close(fd);
        }
    }
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

static void CopyFile_form(_cpu *cpu, const str_form *sf)
{
    char from[RT_HOST_PATH], to[RT_HOST_PATH];
    bool e1, e2;
    uint32_t err = host_of_arg(sf, ARG(0), from, sizeof(from), &e1);
    int in = -1, out = -1;
    struct stat st;

    if (err == 0) err = host_of_arg(sf, ARG(1), to, sizeof(to), &e2);
    if (err == 0 && !e1) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && e2 && ARG(2) != 0) err = ERROR_FILE_EXISTS;
    if (err == 0 && (in = open(from, O_RDONLY | O_CLOEXEC)) < 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (err == 0 && fstat(in, &st) != 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (err == 0 && S_ISDIR(st.st_mode)) err = ERROR_ACCESS_DENIED;
    if (err == 0 && (out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 0777)) < 0)
        err = rt_errno_to_win32(errno, ERROR_PATH_NOT_FOUND);
    if (err == 0)
    {
        static char buf[65536];
        static pthread_mutex_t copy_lock = PTHREAD_MUTEX_INITIALIZER;
        if (pthread_mutex_lock(&copy_lock) != 0) rt_fatal("copy lock failed");
        for (;;)
        {
            ssize_t r = read(in, buf, sizeof(buf)), w = 0;
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) { err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND); break; }
            if (r == 0) break;
            while (w < r)
            {
                ssize_t k = write(out, buf + w, (size_t)(r - w));
                if (k < 0 && errno == EINTR) continue;
                if (k < 0) { err = rt_errno_to_win32(errno, ERROR_DISK_FULL); break; }
                w += k;
            }
            if (err != 0) break;
        }
        if (pthread_mutex_unlock(&copy_lock) != 0) rt_fatal("copy unlock failed");
    }
    if (in >= 0) close(in);
    if (out >= 0 && close(out) != 0 && err == 0) err = rt_errno_to_win32(errno, ERROR_DISK_FULL);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

void rt_CopyFileA(_cpu *cpu)
{
    CopyFile_form(cpu, &FORM_A);
}

void rt_CopyFileW(_cpu *cpu)
{
    CopyFile_form(cpu, &FORM_W);
}

static void GetCurrentDirectory_form(_cpu *cpu, const str_form *sf)
{
    char c[RT_PATH_BYTES];
    get_cwd(c, sizeof(c));
    RET(sf->put(ARG(1), ARG(0), c));
}

void rt_GetCurrentDirectoryA(_cpu *cpu)
{
    GetCurrentDirectory_form(cpu, &FORM_A);
}

void rt_GetCurrentDirectoryW(_cpu *cpu)
{
    GetCurrentDirectory_form(cpu, &FORM_W);
}

static void SetCurrentDirectory_form(_cpu *cpu, const str_form *sf)
{
    char gp[RT_PATH_BYTES], full[RT_PATH_BYTES], host[RT_HOST_PATH];
    struct stat st;
    bool exists;
    uint32_t err;
    if (!path_arg(sf, ARG(0), gp, sizeof(gp)))
    {
        RET(0);
        return;
    }
    err = rt_path_full(gp, full, sizeof(full));
    if (err == 0 && !is_virtual_dir(full))
    {
        err = rt_path_host(full, host, sizeof(host), &exists);
        if (err == 0 && !exists) err = ERROR_FILE_NOT_FOUND;
        if (err == 0 && (stat(host, &st) != 0 || !S_ISDIR(st.st_mode))) err = ERROR_DIRECTORY;
    }
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    if (pthread_mutex_lock(&cwd_lock) != 0) rt_fatal("cwd lock failed");
    snprintf(cwd, sizeof(cwd), "%s", full);
    if (pthread_mutex_unlock(&cwd_lock) != 0) rt_fatal("cwd unlock failed");
    RET(1);
}

void rt_SetCurrentDirectoryA(_cpu *cpu)
{
    SetCurrentDirectory_form(cpu, &FORM_A);
}

void rt_SetCurrentDirectoryW(_cpu *cpu)
{
    SetCurrentDirectory_form(cpu, &FORM_W);
}

static void GetFullPathName_form(_cpu *cpu, const str_form *sf)
{
    char gp[RT_PATH_BYTES], full[RT_PATH_BYTES];
    uint32_t buf = ARG(2), n = ARG(1), part = ARG(3), err, r;
    size_t len;
    if (!path_arg(sf, ARG(0), gp, sizeof(gp)))
    {
        RET(0);
        return;
    }
    len = strlen(gp);
    err = rt_path_full(gp, full, sizeof(full));
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    /* A trailing separator in the input stays in the result. */
    if (len > 0 && (gp[len - 1] == '\\' || gp[len - 1] == '/') && strlen(full) > 3 && strlen(full) + 1 < sizeof(full))
        strcat(full, "\\");
    r = sf->put(buf, n, full);
    if (r < n && part != 0)
    {
        const char *s = strrchr(full, '\\');
        wr32(part, (s != NULL && s[1] != 0) ? buf + sf->unit * form_units(sf, full, (size_t)(s + 1 - full)) : 0);
    }
    RET(r);
}

void rt_GetFullPathNameA(_cpu *cpu)
{
    GetFullPathName_form(cpu, &FORM_A);
}

void rt_GetFullPathNameW(_cpu *cpu)
{
    GetFullPathName_form(cpu, &FORM_W);
}

/* ---------------------------------------------------------------- find files */

typedef struct find_entry {
    char name[256];
    struct stat st;
    bool virtual_dir;
} find_entry;

typedef struct find_data {
    find_entry *e;
    size_t n, next;
} find_data;

static void find_destroy(rt_object *o)
{
    find_data *f = o->data;
    free(f->e);
    free(f);
}

bool rt_wildcard_match(const char *pat, const char *name)
{
    /* "*.*" and "*" match every name; "x.*" matches "x" (no extension). */
    if (strcmp(pat, "*.*") == 0 || strcmp(pat, "*") == 0) return true;
    while (*pat)
    {
        if (*pat == '*')
        {
            pat++;
            if (*pat == 0) return true;
            for (; *name; name++)
                if (rt_wildcard_match(pat, name)) return true;
            return strcmp(pat, ".*") == 0 || strcmp(pat, ".") == 0 || rt_wildcard_match(pat, name);
        }
        if (*name == 0) return strcmp(pat, ".*") == 0 || strcmp(pat, ".") == 0;
        if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*name)) return false;
        pat++;
        name++;
    }
    return *name == 0;
}

static bool find_add(find_data *f, size_t *cap, const char *name, const struct stat *st, bool vdir)
{
    if (f->n == *cap)
    {
        size_t c = *cap ? *cap * 2u : 32u;
        find_entry *ne = realloc(f->e, c * sizeof(find_entry));
        if (ne == NULL) return false;
        f->e = ne;
        *cap = c;
    }
    snprintf(f->e[f->n].name, sizeof(f->e[f->n].name), "%s", name);
    if (st != NULL) f->e[f->n].st = *st;
    else memset(&f->e[f->n].st, 0, sizeof(struct stat));
    f->e[f->n].virtual_dir = vdir;
    f->n++;
    return true;
}

/* WIN32_FIND_DATAA (320 bytes) or WIN32_FIND_DATAW (592 bytes):
   attributes, three FILETIMEs, the size, two reserved dwords, then
   cFileName[260] and cAlternateFileName[14] in the characters of the form.
   The emulated file system has no short (8.3) names, so
   cAlternateFileName is empty, as for a long name without a short name. */
static void write_find_data(const str_form *sf, uint32_t g, const find_entry *e)
{
    memset(G2H(g), 0, sf->unit == 2 ? 592u : 320u);
    if (e->virtual_dir)
    {
        wr32(g + 0, FILE_ATTRIBUTE_DIRECTORY);
    }
    else
    {
        wr32(g + 0, rt_file_attributes(&e->st, e->name));
        wr64(g + 4, rt_filetime_from_timespec(e->st.st_birthtimespec));
        wr64(g + 12, rt_filetime_from_timespec(e->st.st_atimespec));
        wr64(g + 20, rt_filetime_from_timespec(e->st.st_mtimespec));
        if (!S_ISDIR(e->st.st_mode))
        {
            wr32(g + 28, (uint32_t)((uint64_t)e->st.st_size >> 32));
            wr32(g + 32, (uint32_t)e->st.st_size);
        }
    }
    if (!sf->field(g + 44, RT_MAX_PATH, e->name)) rt_fatal("file name %s is longer than MAX_PATH", e->name);
}

static void FindFirstFile_form(_cpu *cpu, const str_form *sf, uint32_t name, uint32_t data)
{
    char gp[RT_PATH_BYTES], full[RT_PATH_BYTES], dir_guest[RT_PATH_BYTES], host[RT_HOST_PATH];
    const char *pattern;
    char *slash;
    find_data *f;
    size_t cap = 0, i;
    bool exists;
    uint32_t err, h;
    rt_object *o;

    if (!path_arg(sf, name, gp, sizeof(gp)))
    {
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    /* Split into the directory and the last component (the pattern). The
       pattern can have wildcards; rt_path_full does not accept them. */
    for (i = 0; gp[i]; i++)
        if (gp[i] == '/') gp[i] = '\\';
    slash = strrchr(gp, '\\');
    if (slash == NULL && gp[0] != 0 && gp[1] == ':')
    {
        rt_set_last_error(ERROR_FILE_NOT_FOUND);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    if (slash != NULL)
    {
        *slash = 0;
        pattern = slash + 1;
        if (gp[0] == 0) snprintf(dir_guest, sizeof(dir_guest), "\\");
        else if (gp[1] == ':' && gp[2] == 0) snprintf(dir_guest, sizeof(dir_guest), "%s\\", gp);
        else snprintf(dir_guest, sizeof(dir_guest), "%s", gp);
    }
    else
    {
        pattern = gp;
        snprintf(dir_guest, sizeof(dir_guest), ".");
    }
    if (*pattern == 0)
    {
        rt_set_last_error(ERROR_FILE_NOT_FOUND);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    err = rt_path_full(dir_guest, full, sizeof(full));
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    f = calloc(1, sizeof(*f));
    if (f == NULL) rt_fatal("out of host memory for a find handle");

    if (is_virtual_dir(full))
    {
        /* The children of a directory above the mounts are the next
           components of the mount paths. */
        size_t fl = strlen(full);
        for (i = 0; i < NUM_MOUNTS; i++)
        {
            const char *mg = mounts[i].guest;
            char child[64];
            size_t k = 0;
            if (mounts[i].host[0] == 0 || !strieq_n(mg, full, fl)) continue;
            const char *rest = mg + fl;
            if (*rest == '\\') rest++;
            while (rest[k] && rest[k] != '\\' && k + 1 < sizeof(child)) { child[k] = rest[k]; k++; }
            child[k] = 0;
            bool dup = false;
            for (size_t j = 0; j < f->n; j++) dup |= strcasecmp(f->e[j].name, child) == 0;
            if (k > 0 && !dup && rt_wildcard_match(pattern, child) && !find_add(f, &cap, child, NULL, true))
                rt_fatal("out of host memory");
        }
    }
    else
    {
        err = rt_path_host(full, host, sizeof(host), &exists);
        if (err == 0 && !exists) err = ERROR_PATH_NOT_FOUND;
        if (err == 0)
        {
            DIR *d = opendir(host);
            struct dirent *de;
            if (d == NULL) err = rt_errno_to_win32(errno, ERROR_PATH_NOT_FOUND);
            else
            {
                while ((de = readdir(d)) != NULL)
                {
                    char p[RT_HOST_PATH];
                    struct stat st;
                    if (!rt_wildcard_match(pattern, de->d_name)) continue;
                    if ((size_t)snprintf(p, sizeof(p), "%s/%s", host, de->d_name) >= sizeof(p)) continue;
                    if (stat(p, &st) != 0) continue;
                    if (!find_add(f, &cap, de->d_name, &st, false)) rt_fatal("out of host memory");
                }
                closedir(d);
            }
        }
    }
    if (err == 0 && f->n == 0) err = ERROR_FILE_NOT_FOUND;
    if (err != 0)
    {
        free(f->e);
        free(f);
        rt_set_last_error(err);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    write_find_data(sf, data, &f->e[0]);
    f->next = 1;
    o = rt_object_new(OBJ_FIND);
    o->data = f;
    o->destroy = find_destroy;
    h = rt_handle_new(o);
    rt_object_unref(o);
    if (h == 0)
    {
        rt_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    rt_set_last_error(ERROR_SUCCESS);
    RET(h);
}

void rt_FindFirstFileA(_cpu *cpu)
{
    FindFirstFile_form(cpu, &FORM_A, ARG(0), ARG(1));
}

void rt_FindFirstFileW(_cpu *cpu)
{
    FindFirstFile_form(cpu, &FORM_W, ARG(0), ARG(1));
}

/* FindFirstFileEx(name, info_level, data, search_op, filter, flags).
   FindExInfoStandard (0) and FindExInfoBasic (1) write the same structure;
   the emulated file system has no short names, so they give the same
   result. FindExSearchLimitToDirectories (1) is a hint that Windows file
   systems also ignore; the result is the same as FindExSearchNameMatch.
   FIND_FIRST_EX_LARGE_FETCH (2) changes only the buffer size. Other values
   are errors, as on Windows. */
static void FindFirstFileEx_form(_cpu *cpu, const str_form *sf)
{
    uint32_t level = ARG(1), op = ARG(3), filter = ARG(4), flags = ARG(5);
    if (level > 1 || op > 1 || filter != 0 || (flags & ~0x3u) != 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    if (flags & 0x1u)
    {
        /* FIND_FIRST_EX_CASE_SENSITIVE: the emulated file system matches
           names without regard to case, as NTFS does by default. */
        rt_log("FindFirstFileEx: FIND_FIRST_EX_CASE_SENSITIVE is ignored (names match without regard to case)");
    }
    FindFirstFile_form(cpu, sf, ARG(0), ARG(2));
}

void rt_FindFirstFileExA(_cpu *cpu)
{
    FindFirstFileEx_form(cpu, &FORM_A);
}

void rt_FindFirstFileExW(_cpu *cpu)
{
    FindFirstFileEx_form(cpu, &FORM_W);
}

static void FindNextFile_form(_cpu *cpu, const str_form *sf)
{
    rt_object *o = rt_handle_get(ARG(0), OBJ_FIND);
    find_data *f;
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    f = o->data;
    if (f->next >= f->n)
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_NO_MORE_FILES);
        RET(0);
        return;
    }
    write_find_data(sf, ARG(1), &f->e[f->next++]);
    rt_object_unref(o);
    RET(1);
}

void rt_FindNextFileA(_cpu *cpu)
{
    FindNextFile_form(cpu, &FORM_A);
}

void rt_FindNextFileW(_cpu *cpu)
{
    FindNextFile_form(cpu, &FORM_W);
}

void rt_FindClose(_cpu *cpu)
{
    rt_object *o = rt_handle_get(ARG(0), OBJ_FIND);
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    rt_object_unref(o);
    rt_handle_close(ARG(0));
    RET(1);
}

/* ---------------------------------------------------------------- OpenFile */

#define OF_READ 0x0000u
#define OF_WRITE 0x0001u
#define OF_READWRITE 0x0002u
#define OF_PARSE 0x0100u
#define OF_DELETE 0x0200u
#define OF_CREATE 0x1000u
#define OF_EXIST 0x4000u
#define HFILE_ERROR 0xFFFFFFFFu

/* OpenFile(name, OFSTRUCT*, style). OFSTRUCT: cBytes, fFixedDisk,
   nErrCode (16-bit), 2 reserved words, szPathName[128]. */
void rt_OpenFile(_cpu *cpu)
{
    const str_form *sf = &FORM_A;
    char gp[RT_PATH_BYTES], full[RT_PATH_BYTES], host[RT_HOST_PATH];
    uint32_t of = ARG(1), style = ARG(2), err;
    bool exists;
    int fd, oflags = O_CLOEXEC;
    rt_object *o;
    file_data *f;
    uint32_t h;

    if (!path_arg(sf, ARG(0), gp, sizeof(gp)) || of == 0)
    {
        RET(HFILE_ERROR);
        return;
    }
    memset(G2H(of), 0, 136);
    wr8(of + 0, 136);
    wr8(of + 1, 1);
    err = rt_path_full(gp, full, sizeof(full));
    if (err == 0) rt_put_afield(of + 8, 128, full); /* an empty szPathName if the path is longer */
    if (err == 0 && (style & OF_PARSE))
    {
        RET(0);
        return;
    }
    if (err == 0) err = rt_path_host(full, host, sizeof(host), &exists);
    if (err == 0 && !exists && !(style & OF_CREATE)) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && (style & OF_DELETE))
    {
        if (unlink(host) != 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
        if (err == 0)
        {
            RET(1);
            return;
        }
    }
    if (err != 0)
    {
        wr16(of + 2, (uint16_t)err);
        rt_set_last_error(err);
        RET(HFILE_ERROR);
        return;
    }
    if (style & OF_EXIST)
    {
        RET(1);
        return;
    }
    if (style & OF_READWRITE) oflags |= O_RDWR;
    else if (style & OF_WRITE) oflags |= O_WRONLY;
    else oflags |= O_RDONLY;
    if (style & OF_CREATE) oflags |= O_CREAT | O_TRUNC | ((oflags & O_ACCMODE) == O_RDONLY ? O_RDWR : 0);
    fd = open(host, oflags, 0644);
    if (fd < 0)
    {
        err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
        wr16(of + 2, (uint16_t)err);
        rt_set_last_error(err);
        RET(HFILE_ERROR);
        return;
    }
    f = calloc(1, sizeof(*f));
    if (f == NULL) rt_fatal("out of host memory for a file");
    f->fd = fd;
    snprintf(f->host, sizeof(f->host), "%s", host);
    o = rt_object_new(OBJ_FILE);
    o->data = f;
    o->destroy = file_destroy;
    h = rt_handle_new(o);
    rt_object_unref(o);
    if (h == 0)
    {
        rt_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
        RET(HFILE_ERROR);
        return;
    }
    RET(h);
}

/* ---------------------------------------------------------------- volumes */

static bool is_c_root(const str_form *sf, uint32_t g)
{
    char buf[8];
    if (g == 0) return true; /* the root of the current drive */
    if (sf->get(buf, sizeof(buf), g) != RT_STR_OK) return false;
    return (buf[0] == 'C' || buf[0] == 'c') && buf[1] == ':' && (buf[2] == '\\' || buf[2] == '/') && buf[3] == 0;
}

static void GetDriveType_form(_cpu *cpu, const str_form *sf)
{
    RET(is_c_root(sf, ARG(0)) ? 3u : 1u); /* DRIVE_FIXED, DRIVE_NO_ROOT_DIR */
}

void rt_GetDriveTypeA(_cpu *cpu)
{
    GetDriveType_form(cpu, &FORM_A);
}

void rt_GetDriveTypeW(_cpu *cpu)
{
    GetDriveType_form(cpu, &FORM_W);
}

void rt_GetLogicalDrives(_cpu *cpu)
{
    RET(1u << 2); /* C: */
}

/* GetVolumeInformationA(root, name, name_size, serial, max_component,
   flags, fs_name, fs_name_size). */
static void GetVolumeInformation_form(_cpu *cpu, const str_form *sf)
{
    if (!is_c_root(sf, ARG(0)))
    {
        rt_set_last_error(ERROR_PATH_NOT_FOUND);
        RET(0);
        return;
    }
    if (ARG(1) != 0 && sf->put(ARG(1), ARG(2), "") >= ARG(2))
    {
        rt_set_last_error(ERROR_MORE_DATA);
        RET(0);
        return;
    }
    if (ARG(3) != 0) wr32(ARG(3), RT_VOLUME_SERIAL);
    if (ARG(4) != 0) wr32(ARG(4), 255);
    if (ARG(5) != 0) wr32(ARG(5), 0x00000006u); /* FILE_CASE_PRESERVED_NAMES, FILE_UNICODE_ON_DISK */
    if (ARG(6) != 0 && sf->put(ARG(6), ARG(7), "NTFS") >= ARG(7))
    {
        rt_set_last_error(ERROR_MORE_DATA);
        RET(0);
        return;
    }
    RET(1);
}

void rt_GetVolumeInformationA(_cpu *cpu)
{
    GetVolumeInformation_form(cpu, &FORM_A);
}

void rt_GetVolumeInformationW(_cpu *cpu)
{
    GetVolumeInformation_form(cpu, &FORM_W);
}

static void GetDiskFreeSpaceEx_form(_cpu *cpu, const str_form *sf)
{
    char host[RT_HOST_PATH], gp[RT_PATH_BYTES];
    bool exists;
    struct statfs fs;
    uint32_t err;
    uint64_t bs;

    if (ARG(0) == 0) get_cwd(gp, sizeof(gp));
    else if (sf->get(gp, sizeof(gp), ARG(0)) != RT_STR_OK)
    {
        rt_set_last_error(ERROR_FILENAME_EXCED_RANGE);
        RET(0);
        return;
    }
    err = rt_path_host(gp, host, sizeof(host), &exists);
    if (err == ERROR_ACCESS_DENIED) /* a directory above the mounts: use the user directory */
    {
        snprintf(host, sizeof(host), "%s", mounts[1].host);
        err = 0;
        exists = true;
    }
    if (err == 0 && !exists) err = ERROR_PATH_NOT_FOUND;
    if (err == 0 && statfs(host, &fs) != 0) err = rt_errno_to_win32(errno, ERROR_PATH_NOT_FOUND);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    bs = fs.f_bsize;
    if (ARG(1) != 0) wr64(ARG(1), (uint64_t)fs.f_bavail * bs);
    if (ARG(2) != 0) wr64(ARG(2), (uint64_t)fs.f_blocks * bs);
    if (ARG(3) != 0) wr64(ARG(3), (uint64_t)fs.f_bfree * bs);
    RET(1);
}

void rt_GetDiskFreeSpaceExA(_cpu *cpu)
{
    GetDiskFreeSpaceEx_form(cpu, &FORM_A);
}

void rt_GetDiskFreeSpaceExW(_cpu *cpu)
{
    GetDiskFreeSpaceEx_form(cpu, &FORM_W);
}

/* ---------------------------------------------------------------- shell folders */

#define CSIDL_PERSONAL 0x05u
#define CSIDL_APPDATA 0x1Au
#define CSIDL_LOCAL_APPDATA 0x1Cu
#define CSIDL_COMMON_APPDATA 0x23u
#define CSIDL_PROFILE 0x28u
#define CSIDL_COMMON_DOCUMENTS 0x2Eu
#define CSIDL_FLAG_CREATE 0x8000u
#define S_OK 0u
#define E_INVALIDARG 0x80070057u
#define E_FAIL 0x80004005u

/* The guest path of a shell folder (CSIDL value without flags), or NULL if
   the emulated system does not have the folder. */
static const char *shell_folder(uint32_t csidl)
{
    switch (csidl)
    {
    case CSIDL_PERSONAL: return "C:\\Users\\Player\\Documents";
    case CSIDL_APPDATA: return "C:\\Users\\Player\\AppData\\Roaming";
    case CSIDL_LOCAL_APPDATA: return "C:\\Users\\Player\\AppData\\Local";
    case CSIDL_PROFILE: return "C:\\Users\\Player";
    case CSIDL_COMMON_APPDATA: return "C:\\ProgramData";
    case CSIDL_COMMON_DOCUMENTS: return "C:\\ProgramData\\Documents";
    default: return NULL;
    }
}

/* Create the directory of a shell folder. Returns false on failure. */
static bool create_shell_folder(const char *p)
{
    char host[RT_HOST_PATH];
    bool exists;
    if (rt_path_host(p, host, sizeof(host), &exists) != 0) return false;
    if (!exists) make_dirs(host);
    return true;
}

/* SHGetFolderPathA(hwnd, csidl, token, flags, path[MAX_PATH]). */
static void SHGetFolderPath_form(_cpu *cpu, const str_form *sf)
{
    uint32_t out = ARG(4);
    const char *p = shell_folder(ARG(1) & 0xFFu);
    if (p == NULL)
    {
        rt_log("SHGetFolderPathA: folder 0x%x is not in the emulated system", ARG(1));
        if (out != 0) sf->field(out, 1, "");
        RET(E_INVALIDARG);
        return;
    }
    if (out == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    if ((ARG(1) & CSIDL_FLAG_CREATE) && !create_shell_folder(p))
    {
        RET(E_FAIL);
        return;
    }
    if (!sf->field(out, RT_MAX_PATH, p)) rt_fatal("SHGetFolderPath: the folder path is longer than MAX_PATH");
    RET(S_OK);
}

/* BOOL SHGetSpecialFolderPathW(HWND hwnd, LPWSTR path[MAX_PATH], int csidl,
   BOOL create). */
void rt_SHGetSpecialFolderPathW(_cpu *cpu)
{
    uint32_t out = ARG(1);
    const char *p = shell_folder(ARG(2) & 0xFFu);
    if (p == NULL)
    {
        rt_log("SHGetSpecialFolderPathW: folder 0x%x is not in the emulated system", ARG(2));
        RET(0);
        return;
    }
    if (out == 0 || (ARG(3) != 0 && !create_shell_folder(p)))
    {
        RET(0);
        return;
    }
    if (!FORM_W.field(out, RT_MAX_PATH, p)) rt_fatal("SHGetSpecialFolderPathW: the folder path is longer than MAX_PATH");
    RET(1);
}

void rt_SHGetFolderPathA(_cpu *cpu)
{
    SHGetFolderPath_form(cpu, &FORM_A);
}

void rt_SHGetFolderPathW(_cpu *cpu)
{
    SHGetFolderPath_form(cpu, &FORM_W);
}

/* PathRelativePathToA(out, from, from_attr, to, to_attr): the relative
   path from "from" to "to". Both must be on the same drive. */
static size_t split_components(char *path, char **comp, size_t max)
{
    size_t n = 0;
    char *save, *c;
    for (c = strtok_r(path + 3, "\\", &save); c != NULL && n < max; c = strtok_r(NULL, "\\", &save)) comp[n++] = c;
    return n;
}

void rt_PathRelativePathToA(_cpu *cpu)
{
    char from_in[RT_PATH_BYTES], to_in[RT_PATH_BYTES], from[RT_PATH_BYTES], to[RT_PATH_BYTES], res[RT_PATH_BYTES];
    char *fc[RT_PATH_BYTES / 2], *tc[RT_PATH_BYTES / 2];
    uint32_t out = ARG(0);
    size_t nf, nt, k = 0, i, pos = 0;

    if (out == 0 || ARG(1) == 0 || ARG(3) == 0 || rt_gstr_a(from_in, sizeof(from_in), ARG(1)) != RT_STR_OK ||
        rt_gstr_a(to_in, sizeof(to_in), ARG(3)) != RT_STR_OK || rt_path_full(from_in, from, sizeof(from)) != 0 ||
        rt_path_full(to_in, to, sizeof(to)) != 0)
    {
        if (out != 0) wr8(out, 0);
        RET(0);
        return;
    }
    if (toupper((unsigned char)from[0]) != toupper((unsigned char)to[0]))
    {
        wr8(out, 0);
        RET(0);
        return;
    }
    nf = split_components(from, fc, sizeof(fc) / sizeof(fc[0]));
    nt = split_components(to, tc, sizeof(tc) / sizeof(tc[0]));
    /* A file as "from": its directory is the start. */
    if (!(ARG(2) & FILE_ATTRIBUTE_DIRECTORY) && nf > 0) nf--;
    while (k < nf && k < nt && strcasecmp(fc[k], tc[k]) == 0) k++;
    if (nf == k)
    {
        res[pos++] = '.';
    }
    else
    {
        for (i = k; i < nf; i++)
        {
            if (pos + 3 >= sizeof(res)) { RET(0); return; }
            if (i > k) res[pos++] = '\\';
            res[pos++] = '.';
            res[pos++] = '.';
        }
    }
    for (i = k; i < nt; i++)
    {
        size_t l = strlen(tc[i]);
        if (pos + 1 + l + 1 > sizeof(res)) { RET(0); return; }
        res[pos++] = '\\';
        memcpy(res + pos, tc[i], l);
        pos += l;
    }
    res[pos] = 0;
    if (!rt_put_afield(out, RT_MAX_PATH, res))
    {
        RET(0);
        return;
    }
    RET(1);
}

/* ---------------------------------------------------------------- attribute data and temporary path */

/* GetFileAttributesExW(name, GetFileExInfoStandard, WIN32_FILE_ATTRIBUTE_DATA*).
   The structure: attributes, creation, access and write FILETIMEs, size
   (high dword, low dword); 36 bytes. */
void rt_GetFileAttributesExW(_cpu *cpu)
{
    const str_form *sf = &FORM_W;
    char gp[RT_PATH_BYTES], full[RT_PATH_BYTES], host[RT_HOST_PATH];
    uint32_t out = ARG(2), err;
    struct stat st;
    bool exists;

    if (ARG(1) != 0 || out == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (!path_arg(sf, ARG(0), gp, sizeof(gp)))
    {
        RET(0);
        return;
    }
    memset(G2H(out), 0, 36);
    if (rt_path_full(gp, full, sizeof(full)) == 0 && is_virtual_dir(full))
    {
        wr32(out, FILE_ATTRIBUTE_DIRECTORY);
        RET(1);
        return;
    }
    err = rt_path_host(gp, host, sizeof(host), &exists);
    if (err == 0 && !exists) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && stat(host, &st) != 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    wr32(out + 0, rt_file_attributes(&st, base_name(host)));
    wr64(out + 4, rt_filetime_from_timespec(st.st_birthtimespec));
    wr64(out + 12, rt_filetime_from_timespec(st.st_atimespec));
    wr64(out + 20, rt_filetime_from_timespec(st.st_mtimespec));
    if (!S_ISDIR(st.st_mode))
    {
        wr32(out + 28, (uint32_t)((uint64_t)st.st_size >> 32));
        wr32(out + 32, (uint32_t)st.st_size);
    }
    RET(1);
}

/* GetLogicalDriveStringsW(size, buffer): "C:\" and a second NUL. The
   result is the length without the last NUL, or the size that is
   necessary when the buffer is too small. */
void rt_GetLogicalDriveStringsW(_cpu *cpu)
{
    uint32_t n = ARG(0), buf = ARG(1);
    if (n < 5 || buf == 0)
    {
        RET(5);
        return;
    }
    wr16(buf + 0, 'C');
    wr16(buf + 2, ':');
    wr16(buf + 4, '\\');
    wr16(buf + 6, 0);
    wr16(buf + 8, 0);
    RET(4);
}

#define RT_GUEST_TEMP_DIR "C:\\Users\\Player\\AppData\\Local\\Temp\\"

/* GetTempPathW(size, buffer): the temporary directory of the user, with
   a trailing backslash. rt_fs_init creates the directory. */
void rt_GetTempPathW(_cpu *cpu)
{
    RET(rt_put_wstr(ARG(1), ARG(0), RT_GUEST_TEMP_DIR));
}

/* ReplaceFileW(replaced, replacement, backup, flags, exclude, reserved).
   The replacement file gets the name of the replaced file; with a backup
   name, the replaced file first gets the backup name. */
void rt_ReplaceFileW(_cpu *cpu)
{
    const str_form *sf = &FORM_W;
    char replaced[RT_HOST_PATH], replacement[RT_HOST_PATH], backup[RT_HOST_PATH];
    bool e1, e2, e3;
    uint32_t err;

    if (ARG(4) != 0 || ARG(5) != 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    err = host_of_arg(sf, ARG(0), replaced, sizeof(replaced), &e1);
    if (err == 0) err = host_of_arg(sf, ARG(1), replacement, sizeof(replacement), &e2);
    if (err == 0 && ARG(2) != 0) err = host_of_arg(sf, ARG(2), backup, sizeof(backup), &e3);
    if (err == 0 && !e1) err = 1175u; /* ERROR_UNABLE_TO_REMOVE_REPLACED */
    if (err == 0 && !e2) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && ARG(2) != 0 && rename(replaced, backup) != 0) err = 1175u;
    if (err == 0 && rename(replacement, replaced) != 0)
    {
        err = 1176u; /* ERROR_UNABLE_TO_MOVE_REPLACEMENT */
        /* Windows gives the replaced file its name back when the move
           fails after the backup step. */
        if (ARG(2) != 0 && rename(backup, replaced) != 0) err = 1177u; /* ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 */
    }
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

/* ---------------------------------------------------------------- handle information */

/* GetFileInformationByHandle(h, BY_HANDLE_FILE_INFORMATION*): attributes,
   three FILETIMEs, the volume serial, the size, the number of links and
   the file index; 52 bytes. */
void rt_GetFileInformationByHandle(_cpu *cpu)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    uint32_t out = ARG(1);
    struct stat st;
    int r;

    if (f == NULL)
    {
        RET(0);
        return;
    }
    r = fstat(f->fd, &st);
    rt_object_unref(o);
    if (r != 0)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
        RET(0);
        return;
    }
    if (out == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    wr32(out + 0, rt_file_attributes(&st, base_name(f->host)));
    wr64(out + 4, rt_filetime_from_timespec(st.st_birthtimespec));
    wr64(out + 12, rt_filetime_from_timespec(st.st_atimespec));
    wr64(out + 20, rt_filetime_from_timespec(st.st_mtimespec));
    wr32(out + 28, RT_VOLUME_SERIAL);
    wr32(out + 32, S_ISDIR(st.st_mode) ? 0u : (uint32_t)((uint64_t)st.st_size >> 32));
    wr32(out + 36, S_ISDIR(st.st_mode) ? 0u : (uint32_t)st.st_size);
    wr32(out + 40, (uint32_t)st.st_nlink);
    wr32(out + 44, (uint32_t)((uint64_t)st.st_ino >> 32));
    wr32(out + 48, (uint32_t)st.st_ino);
    RET(1);
}

#define FILE_TYPE_UNKNOWN 0u
#define FILE_TYPE_DISK 1u
#define FILE_TYPE_CHAR 2u
#define FILE_TYPE_PIPE 3u

void rt_GetFileType(_cpu *cpu)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    struct stat st;
    int r;

    if (f == NULL)
    {
        RET(FILE_TYPE_UNKNOWN);
        return;
    }
    r = fstat(f->fd, &st);
    rt_object_unref(o);
    if (r != 0)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
        RET(FILE_TYPE_UNKNOWN);
        return;
    }
    rt_set_last_error(ERROR_SUCCESS);
    if (S_ISCHR(st.st_mode)) RET(FILE_TYPE_CHAR);
    else if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) RET(FILE_TYPE_PIPE);
    else RET(FILE_TYPE_DISK);
}

/* A FILETIME argument of SetFileTime: false if the pointer is NULL or the
   value is 0 or 0xFFFFFFFF'FFFFFFFF (Windows: do not change this time). */
static bool filetime_arg(uint32_t g, struct timespec *ts)
{
    uint64_t ft;
    if (g == 0) return false;
    ft = rd64(g);
    if (ft == 0 || ft == UINT64_MAX) return false;
    if (ft >= FILETIME_UNIX_EPOCH)
    {
        ts->tv_sec = (time_t)((ft - FILETIME_UNIX_EPOCH) / 10000000u);
        ts->tv_nsec = (long)(((ft - FILETIME_UNIX_EPOCH) % 10000000u) * 100u);
    }
    else
    {
        uint64_t d = FILETIME_UNIX_EPOCH - ft;
        ts->tv_sec = -(time_t)((d + 9999999u) / 10000000u);
        ts->tv_nsec = (long)((10000000u - d % 10000000u) % 10000000u * 100u);
    }
    return true;
}

/* SetFileTime(h, creation, access, write). */
void rt_SetFileTime(_cpu *cpu)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    struct timespec ts[2], ct;
    int r = 0;

    if (f == NULL)
    {
        RET(0);
        return;
    }
    ts[0].tv_sec = 0;
    ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec = 0;
    ts[1].tv_nsec = UTIME_OMIT;
    filetime_arg(ARG(2), &ts[0]);
    filetime_arg(ARG(3), &ts[1]);
    if (ts[0].tv_nsec != UTIME_OMIT || ts[1].tv_nsec != UTIME_OMIT) r = futimens(f->fd, ts);
    if (r == 0 && filetime_arg(ARG(1), &ct))
    {
        struct attrlist al;
        memset(&al, 0, sizeof al);
        al.bitmapcount = ATTR_BIT_MAP_COUNT;
        al.commonattr = ATTR_CMN_CRTIME;
        r = fsetattrlist(f->fd, &al, &ct, sizeof ct, 0);
    }
    rt_object_unref(o);
    if (r != 0)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
        RET(0);
        return;
    }
    RET(1);
}

/* ---------------------------------------------------------------- byte range locks */

/* LockFile and UnlockFile. Windows byte range locks are mandatory and
   belong to a handle: a second lock of an overlapping range fails, also
   for the same process and the same handle. POSIX record locks belong to
   the process, so the runtime keeps the locks of the process in this
   table. Other host processes do not see these locks. */
typedef struct file_lock {
    dev_t dev;
    ino_t ino;
    uint64_t off, len;
    const file_data *owner;
} file_lock;

static pthread_mutex_t lock_table_lock = PTHREAD_MUTEX_INITIALIZER;
static file_lock *lock_table;
static size_t lock_count, lock_cap;

#define ERROR_LOCK_VIOLATION 33u
#define ERROR_NOT_LOCKED 158u

static void locks_release(const file_data *f)
{
    size_t i = 0;
    if (pthread_mutex_lock(&lock_table_lock) != 0) rt_fatal("lock table lock failed");
    while (i < lock_count)
    {
        if (lock_table[i].owner == f) lock_table[i] = lock_table[--lock_count];
        else i++;
    }
    if (pthread_mutex_unlock(&lock_table_lock) != 0) rt_fatal("lock table unlock failed");
}

static void lock_or_unlock(_cpu *cpu, bool lock)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    uint64_t off = ((uint64_t)ARG(2) << 32) | ARG(1), len = ((uint64_t)ARG(4) << 32) | ARG(3);
    struct stat st;
    uint32_t err = 0;
    size_t i;

    if (f == NULL)
    {
        RET(0);
        return;
    }
    if (fstat(f->fd, &st) != 0)
    {
        err = rt_errno_to_win32(errno, ERROR_INVALID_HANDLE);
        rt_object_unref(o);
        rt_set_last_error(err);
        RET(0);
        return;
    }
    if (pthread_mutex_lock(&lock_table_lock) != 0) rt_fatal("lock table lock failed");
    if (lock)
    {
        if (len == 0 || off + len < off) err = ERROR_INVALID_PARAMETER;
        for (i = 0; err == 0 && i < lock_count; i++)
        {
            const file_lock *l = &lock_table[i];
            if (l->dev == st.st_dev && l->ino == st.st_ino && off < l->off + l->len && l->off < off + len)
                err = ERROR_LOCK_VIOLATION;
        }
        if (err == 0)
        {
            if (lock_count == lock_cap)
            {
                size_t c = lock_cap ? lock_cap * 2u : 16u;
                file_lock *nt = realloc(lock_table, c * sizeof *nt);
                if (nt == NULL) rt_fatal("out of host memory for file locks");
                lock_table = nt;
                lock_cap = c;
            }
            lock_table[lock_count].dev = st.st_dev;
            lock_table[lock_count].ino = st.st_ino;
            lock_table[lock_count].off = off;
            lock_table[lock_count].len = len;
            lock_table[lock_count].owner = f;
            lock_count++;
        }
    }
    else
    {
        err = ERROR_NOT_LOCKED;
        for (i = 0; i < lock_count; i++)
        {
            if (lock_table[i].owner == f && lock_table[i].off == off && lock_table[i].len == len)
            {
                lock_table[i] = lock_table[--lock_count];
                err = 0;
                break;
            }
        }
    }
    if (pthread_mutex_unlock(&lock_table_lock) != 0) rt_fatal("lock table unlock failed");
    rt_object_unref(o);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

void rt_LockFile(_cpu *cpu)
{
    lock_or_unlock(cpu, true);
}

void rt_UnlockFile(_cpu *cpu)
{
    lock_or_unlock(cpu, false);
}

/* ---------------------------------------------------------------- pipes */

/* A file handle for host descriptor fd. Returns 0 if the handle table is
   full (fd is then closed). */
static uint32_t new_fd_handle(int fd, const char *name, bool is_pipe)
{
    rt_object *o;
    file_data *f = calloc(1, sizeof(*f));
    uint32_t h;
    if (f == NULL) rt_fatal("out of host memory for a file");
    f->fd = fd;
    f->is_pipe = is_pipe;
    snprintf(f->host, sizeof(f->host), "%s", name);
    o = rt_object_new(OBJ_FILE);
    o->data = f;
    o->destroy = file_destroy;
    h = rt_handle_new(o);
    rt_object_unref(o);
    return h;
}

/* CreatePipe(&read, &write, security, size). The pipe is a pair of
   connected host stream sockets: PeekNamedPipe needs to look at data
   without reading it (MSG_PEEK). SO_NOSIGPIPE turns a write to a closed
   pipe into an error, as on Windows. */
void rt_CreatePipe(_cpu *cpu)
{
    int sv[2], one = 1;
    uint32_t hr, hw;

    if (ARG(0) == 0 || ARG(1) == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_TOO_MANY_OPEN_FILES));
        RET(0);
        return;
    }
    if (setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0 ||
        setsockopt(sv[1], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0 ||
        shutdown(sv[0], SHUT_WR) != 0 || shutdown(sv[1], SHUT_RD) != 0 ||
        fcntl(sv[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(sv[1], F_SETFD, FD_CLOEXEC) != 0)
        rt_fatal("setup of a pipe failed: %s", strerror(errno));
    hr = new_fd_handle(sv[0], "<pipe>", true);
    if (hr == 0)
    {
        close(sv[1]);
        rt_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
        RET(0);
        return;
    }
    hw = new_fd_handle(sv[1], "<pipe>", true);
    if (hw == 0)
    {
        rt_handle_close(hr);
        rt_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
        RET(0);
        return;
    }
    wr32(ARG(0), hr);
    wr32(ARG(1), hw);
    RET(1);
}

/* PeekNamedPipe(h, buffer, size, &read, &available, &left_in_message). */
void rt_PeekNamedPipe(_cpu *cpu)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    uint32_t buf = ARG(1), size = ARG(2);
    int avail = 0;
    ssize_t got = 0;

    if (f == NULL)
    {
        RET(0);
        return;
    }
    if (!f->is_pipe)
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    if (ioctl(f->fd, FIONREAD, &avail) != 0)
    {
        rt_object_unref(o);
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
        RET(0);
        return;
    }
    if (avail == 0)
    {
        /* No data: a pipe without a write end is broken. */
        char c;
        ssize_t r = recv(f->fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r == 0)
        {
            rt_object_unref(o);
            rt_set_last_error(109u); /* ERROR_BROKEN_PIPE */
            RET(0);
            return;
        }
    }
    if (buf != 0 && size != 0 && avail > 0)
    {
        if (!rt_va_is_committed(buf, size))
        {
            rt_object_unref(o);
            rt_set_last_error(998u); /* ERROR_NOACCESS */
            RET(0);
            return;
        }
        do got = recv(f->fd, G2H(buf), size, MSG_PEEK | MSG_DONTWAIT);
        while (got < 0 && errno == EINTR);
        if (got < 0) got = 0;
    }
    rt_object_unref(o);
    if (ARG(3) != 0) wr32(ARG(3), (uint32_t)got);
    if (ARG(4) != 0) wr32(ARG(4), (uint32_t)avail);
    if (ARG(5) != 0) wr32(ARG(5), 0); /* byte mode pipe */
    RET(1);
}

/* ---------------------------------------------------------------- profile (INI) files */

/* Remove leading and trailing spaces and tabs (in place). */
static char *ini_trim(char *t)
{
    size_t n;
    while (*t == ' ' || *t == '\t') t++;
    n = strlen(t);
    while (n > 0 && (t[n - 1] == ' ' || t[n - 1] == '\t' || t[n - 1] == '\r')) t[--n] = 0;
    return t;
}

/* Case-insensitive comparison of UTF-8 names for the ASCII letters and the
   Latin-1 letters, as the profile functions of Windows (CompareString with
   NORM_IGNORECASE for the en-US locale gives the same result for names). */
static bool ini_name_eq(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b), i = 0, j = 0;
    while (i < na && j < nb)
    {
        uint32_t ca, cb;
        int la = rt_utf8_decode((const uint8_t *)a + i, na - i, &ca), lb = rt_utf8_decode((const uint8_t *)b + j, nb - j, &cb);
        if (la <= 0 || lb <= 0) return false;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca >= 0xE0 && ca <= 0xFE && ca != 0xF7) ca -= 32;
        if (cb >= 0xE0 && cb <= 0xFE && cb != 0xF7) cb -= 32;
        if (ca != cb) return false;
        i += (size_t)la;
        j += (size_t)lb;
    }
    return i == na && j == nb;
}

/* BOOL WritePrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR
   value, LPCWSTR file). A file name without a directory is in the Windows
   directory. key NULL deletes the section; value NULL deletes the key. A
   file that starts with a UTF-16LE byte order mark stays UTF-16LE; other
   files are ANSI (Windows-1252, '?' for other characters), as on Windows.
   The new file replaces the old file in one rename. All arguments NULL
   flush the cache: the runtime has no cache, so the call succeeds. */
void rt_WritePrivateProfileStringW(_cpu *cpu)
{
    char sec[1024], key[1024], *val = NULL, file[RT_PATH_BYTES], guest[RT_PATH_BYTES], host[RT_HOST_PATH], tmp[RT_HOST_PATH + 8];
    char *text = NULL, *out = NULL, *line, *save = NULL;
    size_t olen = 0, ocap = 0, tlen = 0;
    bool exists, wide = false, in_sec = false, sec_found = false, done = false;
    uint32_t err;
    int fd;
    if (ARG(0) == 0 && ARG(1) == 0 && ARG(2) == 0)
    {
        RET(1);
        return;
    }
    if (ARG(0) == 0 || ARG(3) == 0 || rt_gstr_w(sec, sizeof sec, ARG(0)) != RT_STR_OK ||
        (ARG(1) != 0 && rt_gstr_w(key, sizeof key, ARG(1)) != RT_STR_OK) || rt_gstr_w(file, sizeof file, ARG(3)) != RT_STR_OK)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (ARG(2) != 0)
    {
        size_t cap = 3u * (size_t)rt_wstrlen(ARG(2)) + 1u;
        val = malloc(cap);
        if (val == NULL) rt_fatal("out of host memory (WritePrivateProfileStringW)");
        if (rt_gstr_w(val, cap, ARG(2)) != RT_STR_OK)
        {
            free(val);
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
    }
    if (strchr(file, '\\') == NULL && strchr(file, '/') == NULL && strchr(file, ':') == NULL)
    {
        if ((size_t)snprintf(guest, sizeof guest, "%s\\%s", RT_GUEST_WINDOWS_DIR, file) >= sizeof guest)
        {
            free(val);
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
    }
    else
        memcpy(guest, file, strlen(file) + 1);
    err = rt_path_host(guest, host, sizeof host, &exists);
    if (err != 0)
    {
        free(val);
        rt_set_last_error(err);
        RET(0);
        return;
    }
    if (exists)
    {
        FILE *f = fopen(host, "rb");
        long n;
        if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
        {
            err = rt_errno_to_win32(errno, ERROR_ACCESS_DENIED);
            if (f != NULL) fclose(f);
            free(val);
            rt_set_last_error(err);
            RET(0);
            return;
        }
        if (n > (16l << 20)) rt_fatal("WritePrivateProfileStringW: %s is larger than 16 MiB", host);
        {
            uint8_t *raw = malloc((size_t)n + 2);
            if (raw == NULL || fread(raw, 1, (size_t)n, f) != (size_t)n) rt_fatal("WritePrivateProfileStringW: read of %s failed", host);
            fclose(f);
            if (n >= 2 && raw[0] == 0xFF && raw[1] == 0xFE)
            {
                size_t units = ((size_t)n - 2) / 2, k, o = 0;
                wide = true;
                text = malloc(3 * units + 1);
                if (text == NULL) rt_fatal("out of host memory (WritePrivateProfileStringW)");
                for (k = 0; k < units; k++)
                {
                    uint32_t c = (uint32_t)raw[2 + 2 * k] | ((uint32_t)raw[3 + 2 * k] << 8);
                    char e[4];
                    size_t l;
                    if (c >= 0xD800 && c <= 0xDBFF && k + 1 < units)
                    {
                        uint32_t c2 = (uint32_t)raw[4 + 2 * k] | ((uint32_t)raw[5 + 2 * k] << 8);
                        if (c2 >= 0xDC00 && c2 <= 0xDFFF)
                        {
                            c = 0x10000u + ((c - 0xD800u) << 10) + (c2 - 0xDC00u);
                            k++;
                        }
                    }
                    if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
                    l = rt_utf8_encode(c, e);
                    memcpy(text + o, e, l);
                    o += l;
                }
                text[o] = 0;
                tlen = o;
            }
            else
            {
                long k;
                size_t o = 0;
                text = malloc(3 * (size_t)n + 1);
                if (text == NULL) rt_fatal("out of host memory (WritePrivateProfileStringW)");
                for (k = 0; k < n; k++)
                {
                    char e[4];
                    size_t l = rt_utf8_encode(rt_cp1252_to_unicode(raw[k]), e);
                    memcpy(text + o, e, l);
                    o += l;
                }
                text[o] = 0;
                tlen = o;
            }
            free(raw);
        }
    }
#define OUT(str, len) do { \
        size_t l_ = (len); \
        if (olen + l_ + 1 > ocap) { while (olen + l_ + 1 > ocap) ocap = ocap ? 2 * ocap : 1024; out = realloc(out, ocap); \
            if (out == NULL) rt_fatal("out of host memory (WritePrivateProfileStringW)"); } \
        memcpy(out + olen, (str), l_); olen += l_; out[olen] = 0; \
    } while (0)
    /* Lines end with LF or CR LF; empty lines stay. A last line without an
       end gets CR LF. */
    if (text != NULL && tlen > 0)
        for (line = text; line != NULL && *line != 0; line = save)
        {
            char copy[4096], *t, *nl = strchr(line, '\n');
            size_t ll;
            if (nl != NULL)
            {
                *nl = 0;
                save = nl + 1;
            }
            else
                save = NULL;
            if (strlen(line) > 0 && line[strlen(line) - 1] == '\r') line[strlen(line) - 1] = 0;
            ll = strlen(line);
            if (ll >= sizeof copy) ll = sizeof copy - 1;
            memcpy(copy, line, ll);
            copy[ll] = 0;
            t = ini_trim(copy);
            if (t[0] == '[')
            {
                char *e = strchr(t, ']');
                bool was = in_sec;
                if (e != NULL) *e = 0;
                in_sec = e != NULL && ini_name_eq(ini_trim(t + 1), sec);
                if (was && !in_sec && !done && ARG(1) != 0 && val != NULL)
                {
                    OUT(key, strlen(key));
                    OUT("=", 1);
                    OUT(val, strlen(val));
                    OUT("\r\n", 2);
                    done = true;
                }
                if (in_sec)
                {
                    sec_found = true;
                    if (ARG(1) == 0) continue; /* delete the section */
                }
            }
            else if (in_sec)
            {
                char *eq;
                if (ARG(1) == 0) continue;
                eq = strchr(t, '=');
                if (eq != NULL)
                {
                    *eq = 0;
                    if (ini_name_eq(ini_trim(t), key))
                    {
                        if (val != NULL && !done)
                        {
                            OUT(key, strlen(key));
                            OUT("=", 1);
                            OUT(val, strlen(val));
                            OUT("\r\n", 2);
                        }
                        done = true;
                        continue;
                    }
                }
            }
            OUT(line, strlen(line));
            OUT("\r\n", 2);
        }
    if (!done && ARG(1) != 0 && val != NULL)
    {
        if (!sec_found)
        {
            OUT("[", 1);
            OUT(sec, strlen(sec));
            OUT("]\r\n", 3);
        }
        else if (!in_sec)
            rt_fatal("WritePrivateProfileStringW: internal state error");
        OUT(key, strlen(key));
        OUT("=", 1);
        OUT(val, strlen(val));
        OUT("\r\n", 2);
    }
#undef OUT
    free(text);
    free(val);
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.new", host) >= sizeof tmp) rt_fatal("WritePrivateProfileStringW: host path too long");
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        err = rt_errno_to_win32(errno, ERROR_PATH_NOT_FOUND);
        free(out);
        rt_set_last_error(err);
        RET(0);
        return;
    }
    {
        uint8_t *bytes;
        size_t nb = 0, i = 0, n = olen, done_w = 0;
        bytes = malloc(2 * n + 4);
        if (bytes == NULL) rt_fatal("out of host memory (WritePrivateProfileStringW)");
        if (wide)
        {
            bytes[nb++] = 0xFF;
            bytes[nb++] = 0xFE;
        }
        while (i < n)
        {
            uint32_t c;
            int l = rt_utf8_decode((const uint8_t *)out + i, n - i, &c);
            if (l <= 0) rt_fatal("WritePrivateProfileStringW: the text is not valid UTF-8");
            i += (size_t)l;
            if (wide)
            {
                if (c >= 0x10000u)
                {
                    uint32_t hi = 0xD800u + ((c - 0x10000u) >> 10), lo = 0xDC00u + ((c - 0x10000u) & 0x3FFu);
                    bytes = realloc(bytes, nb + 4 + 2 * (n - i) + 4);
                    if (bytes == NULL) rt_fatal("out of host memory (WritePrivateProfileStringW)");
                    bytes[nb++] = (uint8_t)hi; bytes[nb++] = (uint8_t)(hi >> 8);
                    bytes[nb++] = (uint8_t)lo; bytes[nb++] = (uint8_t)(lo >> 8);
                }
                else
                {
                    bytes[nb++] = (uint8_t)c;
                    bytes[nb++] = (uint8_t)(c >> 8);
                }
            }
            else
            {
                int b = rt_unicode_to_cp1252(c);
                bytes[nb++] = b < 0 ? (uint8_t)'?' : (uint8_t)b;
            }
        }
        while (done_w < nb)
        {
            ssize_t w = write(fd, bytes + done_w, nb - done_w);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0)
            {
                err = rt_errno_to_win32(errno, ERROR_DISK_FULL);
                break;
            }
            done_w += (size_t)w;
        }
        free(bytes);
    }
    free(out);
    if (close(fd) != 0 && err == 0) err = rt_errno_to_win32(errno, ERROR_DISK_FULL);
    if (err == 0 && rename(tmp, host) != 0) err = rt_errno_to_win32(errno, ERROR_ACCESS_DENIED);
    if (err != 0)
    {
        unlink(tmp);
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

/* ---------------------------------------------------------------- standard handles and the console */

/* The standard handles of the process: file handles for duplicates of
   host descriptors 0, 1 and 2. The host terminal is the console. */
static uint32_t std_handles[3];
static bool console_attached = true;

void rt_std_handles_init(void)
{
    static const char *const names[3] = { "<stdin>", "<stdout>", "<stderr>" };
    int i;
    for (i = 0; i < 3; i++)
    {
        int fd = fcntl(i, F_DUPFD_CLOEXEC, 3);
        if (fd < 0)
        {
            if (errno == EBADF) continue; /* the host process has no such descriptor */
            rt_fatal("duplicate of descriptor %d failed: %s", i, strerror(errno));
        }
        std_handles[i] = new_fd_handle(fd, names[i], false);
        if (std_handles[i] == 0) rt_fatal("no handle for a standard handle");
    }
}

static int std_index(uint32_t which)
{
    switch (which)
    {
    case 0xFFFFFFF6u: return 0; /* STD_INPUT_HANDLE */
    case 0xFFFFFFF5u: return 1; /* STD_OUTPUT_HANDLE */
    case 0xFFFFFFF4u: return 2; /* STD_ERROR_HANDLE */
    default: return -1;
    }
}

void rt_GetStdHandle(_cpu *cpu)
{
    int i = std_index(ARG(0));
    if (i < 0)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    RET(std_handles[i]);
}

void rt_SetStdHandle(_cpu *cpu)
{
    int i = std_index(ARG(0));
    if (i < 0)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    std_handles[i] = ARG(1);
    RET(1);
}

/* The host descriptor of a console handle: a file handle of a terminal,
   while the process has a console. Returns -1 and sets the last error. */
static int console_fd(uint32_t h, rt_object **o)
{
    file_data *f = file_arg(h, o);
    if (f == NULL) return -1;
    if (!console_attached || !isatty(f->fd))
    {
        rt_object_unref(*o);
        rt_set_last_error(ERROR_INVALID_HANDLE);
        return -1;
    }
    return f->fd;
}

/* GetConsoleMode(h, &mode). Input: ENABLE_PROCESSED_INPUT,
   ENABLE_LINE_INPUT, ENABLE_ECHO_INPUT (the default 0x7). Output:
   ENABLE_PROCESSED_OUTPUT, ENABLE_WRAP_AT_EOL_OUTPUT (the default 0x3). */
void rt_GetConsoleMode(_cpu *cpu)
{
    rt_object *o;
    int fd = console_fd(ARG(0), &o);
    int flags;
    if (fd < 0)
    {
        RET(0);
        return;
    }
    flags = fcntl(fd, F_GETFL);
    rt_object_unref(o);
    if (flags < 0)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    if (ARG(1) != 0) wr32(ARG(1), (flags & O_ACCMODE) == O_RDONLY ? 0x7u : 0x3u);
    RET(1);
}

/* BOOL SetConsoleMode(HANDLE, DWORD mode). The host terminal keeps its
   own line discipline; the runtime keeps the modes that GetConsoleMode
   reports, so the valid values are those modes. Input: the bits
   ENABLE_PROCESSED_INPUT, ENABLE_LINE_INPUT, ENABLE_ECHO_INPUT,
   ENABLE_WINDOW_INPUT, ENABLE_MOUSE_INPUT, ENABLE_INSERT_MODE,
   ENABLE_QUICK_EDIT_MODE, ENABLE_EXTENDED_FLAGS (0xFF). Output: the bits
   ENABLE_PROCESSED_OUTPUT and ENABLE_WRAP_AT_EOL_OUTPUT. A mode that the
   terminal cannot give stops the process. */
void rt_SetConsoleMode(_cpu *cpu)
{
    rt_object *o;
    int fd = console_fd(ARG(0), &o);
    uint32_t mode = ARG(1);
    int flags;
    if (fd < 0)
    {
        RET(0);
        return;
    }
    flags = fcntl(fd, F_GETFL);
    rt_object_unref(o);
    if (flags < 0)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    if ((flags & O_ACCMODE) == O_RDONLY)
    {
        if (mode & ~0xFFu)
        {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
        if ((mode & 0x7u) != 0x7u)
            rt_fatal("SetConsoleMode: input mode 0x%x (without line input, echo or processed input) is not supported by the terminal of the runtime", mode);
    }
    else
    {
        if (mode & ~0x3u)
        {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
        if (mode != 0x3u)
            rt_fatal("SetConsoleMode: output mode 0x%x is not supported by the terminal of the runtime", mode);
    }
    RET(1);
}

/* BOOL FlushConsoleInputBuffer(HANDLE): discard the input that the
   terminal has not given to the process. */
void rt_FlushConsoleInputBuffer(_cpu *cpu)
{
    rt_object *o;
    int fd = console_fd(ARG(0), &o);
    if (fd < 0)
    {
        RET(0);
        return;
    }
    if (tcflush(fd, TCIFLUSH) != 0)
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    rt_object_unref(o);
    RET(1);
}

/* BOOL ReadConsoleInputA(HANDLE, PINPUT_RECORD, DWORD n, LPDWORD read).
   The terminal gives characters, not key events: each character of the
   input becomes a KEY_EVENT record with bKeyDown TRUE and the character,
   without key codes (wVirtualKeyCode and wVirtualScanCode 0). The call
   waits for at least one character. INPUT_RECORD (20 bytes): EventType,
   then KEY_EVENT_RECORD: bKeyDown, wRepeatCount, wVirtualKeyCode,
   wVirtualScanCode, uChar, dwControlKeyState. */
void rt_ReadConsoleInputA(_cpu *cpu)
{
    rt_object *o;
    int fd = console_fd(ARG(0), &o);
    uint32_t rec = ARG(1), n = ARG(2), out = ARG(3), i;
    uint8_t buf[256];
    ssize_t r;
    if (fd < 0)
    {
        RET(0);
        return;
    }
    if (n == 0 || rec == 0 || !rt_va_is_committed(rec, n * 20u) || n > 0x0CCCCCCCu)
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (n > sizeof buf) n = sizeof buf;
    do
        r = read(fd, buf, n);
    while (r < 0 && errno == EINTR);
    rt_object_unref(o);
    if (r <= 0)
    {
        rt_set_last_error(r == 0 ? ERROR_HANDLE_EOF : rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
        RET(0);
        return;
    }
    for (i = 0; i < (uint32_t)r; i++)
    {
        uint32_t e = rec + 20u * i;
        memset(G2H(e), 0, 20);
        wr16(e, 1);       /* KEY_EVENT */
        wr32(e + 4, 1);   /* bKeyDown */
        wr16(e + 8, 1);   /* wRepeatCount */
        wr8(e + 14, buf[i] == '\n' ? (uint8_t)'\r' : buf[i]);
    }
    if (out != 0) wr32(out, (uint32_t)r);
    RET(1);
}

/* BOOL SetConsoleCtrlHandler(PHANDLER_ROUTINE, BOOL add). The handler list
   of the process (at most 64 handlers). After the first call, the host
   signals SIGINT (Ctrl+C), SIGQUIT (CTRL_BREAK_EVENT) and SIGHUP (the
   terminal closes: CTRL_CLOSE_EVENT) become console control events. As on
   Windows, each event runs on a new thread, which calls the handlers from
   the last one added to the first one until a handler returns TRUE. If no
   handler returns TRUE, the process ends with STATUS_CONTROL_C_EXIT. A
   NULL routine sets or clears the flag that ignores Ctrl+C. */
#define MAX_CTRL_HANDLERS 64u
#define CTRL_C_EVENT 0u
#define CTRL_BREAK_EVENT 1u
#define CTRL_CLOSE_EVENT 2u
#define STATUS_CONTROL_C_EXIT 0xC000013Au
static pthread_mutex_t ctrl_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t ctrl_handlers[MAX_CTRL_HANDLERS];
static uint32_t ctrl_count;
static bool ctrl_c_ignored;
static bool ctrl_started;
static int ctrl_pipe[2] = { -1, -1 };

static void ctrl_signal(int sig)
{
    uint8_t e = sig == SIGINT ? (uint8_t)CTRL_C_EVENT : sig == SIGQUIT ? (uint8_t)CTRL_BREAK_EVENT : (uint8_t)CTRL_CLOSE_EVENT;
    int saved = errno;
    /* A full pipe drops the event: the events before it are still to run. */
    (void)!write(ctrl_pipe[1], &e, 1);
    errno = saved;
}

static void ctrl_event_body(void *ctx, _cpu *cpu)
{
    uint32_t ev = (uint32_t)(uintptr_t)ctx, list[MAX_CTRL_HANDLERS], n, i;
    bool ignored;
    if (pthread_mutex_lock(&ctrl_lock) != 0) rt_fatal("console handler lock failed");
    n = ctrl_count;
    memcpy(list, ctrl_handlers, n * sizeof list[0]);
    ignored = ctrl_c_ignored;
    if (pthread_mutex_unlock(&ctrl_lock) != 0) rt_fatal("console handler unlock failed");
    for (i = n; i > 0; i--)
        if (rt_call_std(cpu, list[i - 1], 1, ev) != 0) return;
    if (ev == CTRL_C_EVENT && ignored) return;
    rt_log("console control event %u: no handler handled it; the process ends", ev);
    rt_process_exit(STATUS_CONTROL_C_EXIT);
}

static void *ctrl_dispatch(void *arg)
{
    (void)arg;
    for (;;)
    {
        uint8_t e;
        ssize_t r = read(ctrl_pipe[0], &e, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r != 1) rt_fatal("console control event pipe read failed");
        rt_thread_start_runtime(ctrl_event_body, (void *)(uintptr_t)e, 0x10000u);
    }
    return NULL;
}

/* Start the event dispatch (ctrl_lock held). */
static void ctrl_start_locked(void)
{
    struct sigaction sa;
    pthread_t t;
    pthread_attr_t a;
    static const int sigs[3] = { SIGINT, SIGQUIT, SIGHUP };
    int i;
    if (ctrl_started) return;
    if (pipe(ctrl_pipe) != 0) rt_fatal("console control event pipe: %s", strerror(errno));
    if (fcntl(ctrl_pipe[1], F_SETFL, O_NONBLOCK) != 0 || fcntl(ctrl_pipe[0], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(ctrl_pipe[1], F_SETFD, FD_CLOEXEC) != 0)
        rt_fatal("console control event pipe: %s", strerror(errno));
    if (pthread_attr_init(&a) != 0 || pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED) != 0 ||
        pthread_create(&t, &a, ctrl_dispatch, NULL) != 0)
        rt_fatal("the console control event thread could not start");
    pthread_attr_destroy(&a);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = ctrl_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    for (i = 0; i < 3; i++)
        if (sigaction(sigs[i], &sa, NULL) != 0) rt_fatal("sigaction for signal %d failed: %s", sigs[i], strerror(errno));
    ctrl_started = true;
}

void rt_SetConsoleCtrlHandler(_cpu *cpu)
{
    uint32_t fn = ARG(0), add = ARG(1), i;
    if (pthread_mutex_lock(&ctrl_lock) != 0) rt_fatal("console handler lock failed");
    ctrl_start_locked();
    if (fn == 0)
    {
        ctrl_c_ignored = add != 0;
        if (pthread_mutex_unlock(&ctrl_lock) != 0) rt_fatal("console handler unlock failed");
        RET(1);
        return;
    }
    if (add != 0)
    {
        if (ctrl_count == MAX_CTRL_HANDLERS)
        {
            if (pthread_mutex_unlock(&ctrl_lock) != 0) rt_fatal("console handler unlock failed");
            rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            RET(0);
            return;
        }
        ctrl_handlers[ctrl_count++] = fn;
    }
    else
    {
        for (i = ctrl_count; i > 0; i--)
            if (ctrl_handlers[i - 1] == fn) break;
        if (i == 0)
        {
            if (pthread_mutex_unlock(&ctrl_lock) != 0) rt_fatal("console handler unlock failed");
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
        memmove(&ctrl_handlers[i - 1], &ctrl_handlers[i], (ctrl_count - i) * sizeof ctrl_handlers[0]);
        ctrl_count--;
    }
    if (pthread_mutex_unlock(&ctrl_lock) != 0) rt_fatal("console handler unlock failed");
    RET(1);
}

/* The input code page of the console: the OEM code page of the emulated
   system (437, as GetOEMCP). */
void rt_GetConsoleCP(_cpu *cpu)
{
    RET(console_attached ? 437u : 0u);
}

/* WriteConsoleW(h, chars, count, &written, reserved): the host terminal
   gets the text as UTF-8. */
void rt_WriteConsoleW(_cpu *cpu)
{
    rt_object *o;
    int fd = console_fd(ARG(0), &o);
    uint32_t count = ARG(2);
    size_t max, len, done = 0;
    char *text;
    int r;

    if (ARG(3) != 0) wr32(ARG(3), 0);
    if (fd < 0)
    {
        RET(0);
        return;
    }
    if (count > 0x1000000u)
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    max = (size_t)count * 3u + 1u;
    text = malloc(max);
    if (text == NULL) rt_fatal("out of host memory for console text");
    r = rt_gstrn_w(text, max, ARG(1), count);
    if (r != RT_STR_OK)
    {
        free(text);
        rt_object_unref(o);
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    len = strlen(text);
    while (done < len)
    {
        ssize_t w = write(fd, text + done, len - done);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) break;
        done += (size_t)w;
    }
    free(text);
    rt_object_unref(o);
    if (done < len)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
        RET(0);
        return;
    }
    if (ARG(3) != 0) wr32(ARG(3), count);
    RET(1);
}

/* FreeConsole: the process has no console after the call. The standard
   handles stay valid as file handles, as on Windows. */
void rt_FreeConsole(_cpu *cpu)
{
    console_attached = false;
    RET(1);
}

/* SetHandleCount: no effect on Win32; the result is the argument. */
void rt_SetHandleCount(_cpu *cpu)
{
    RET(ARG(0));
}
