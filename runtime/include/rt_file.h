/*
 * The emulated file system.
 *
 * The guest sees one drive, C:. Guest paths are case-insensitive. Each
 * mount maps one guest directory to one host directory:
 *   C:\Prototype      the game directory (the directory of the game files)
 *   C:\Users\Player   the user directory of the port (saved games and
 *                     settings), on the host in
 *                     ~/Library/Application Support/<port name>/User
 *   C:\ProgramData    ~/Library/Application Support/<port name>/ProgramData
 *   C:\PortRuntime    optional: files that the port adds (rt_fs_mount_runtime)
 * Other guest paths do not exist (ERROR_PATH_NOT_FOUND).
 */
#ifndef RT_FILE_H
#define RT_FILE_H

#include "rt.h"

#include <stdbool.h>
#include <sys/stat.h>

#define RT_MAX_PATH 260
/* Size of a buffer for a guest path as UTF-8 (RT_MAX_PATH UTF-16 units
   need at most 3 bytes each). */
#define RT_PATH_BYTES 1024
#define RT_HOST_PATH 1024

/* Set the host directories. The game directory must exist; the other
   directories are created when necessary. */
void rt_fs_init(const char *game_dir, const char *user_dir, const char *programdata_dir);

/* Guest directory of files that the port adds, for example the AOT
   instance assembly of Mono (rt_mono.c). */
#define RT_GUEST_RUNTIME_DIR "C:\\PortRuntime"

/* Mount the existing host directory host_dir at RT_GUEST_RUNTIME_DIR.
   Without this call, RT_GUEST_RUNTIME_DIR does not exist. */
void rt_fs_mount_runtime(const char *host_dir);

/* Make an absolute, normalized guest path ("C:\dir\file") from a guest
   path. Returns 0 or a Win32 error code. */
uint32_t rt_path_full(const char *in, char *out, size_t n);

/* Map a guest path to a host path. Each existing component is found
   without regard to case. Returns 0 or a Win32 error code:
   ERROR_PATH_NOT_FOUND if a directory component does not exist (the last
   component can be missing; *exists tells whether it exists). */
uint32_t rt_path_host(const char *guest, char *host, size_t n, bool *exists);

/* Win32 error code for an errno value. not_found is the code for ENOENT. */
uint32_t rt_errno_to_win32(int e, uint32_t not_found);

/* FILETIME (100 ns intervals since 1601) for a host time. */
uint64_t rt_filetime_from_timespec(struct timespec ts);
/* Win32 file attributes of a host file. */
uint32_t rt_file_attributes(const struct stat *st, const char *name);

/* Host file descriptor of a file handle, or -1 if h is not a file handle. */
int rt_file_fd(uint32_t h);

/* Make the standard handles (GetStdHandle) from host descriptors 0, 1
   and 2. Call after the object system is ready. */
void rt_std_handles_init(void);

/* The host file descriptor of a file object (type OBJ_FILE), or -1 for a
   directory handle. The caller holds a reference to the object. */
struct rt_object;
int rt_file_object_fd(struct rt_object *o);

/* Wildcard match of a Windows file name pattern (case-insensitive). */
bool rt_wildcard_match(const char *pattern, const char *name);

#endif
