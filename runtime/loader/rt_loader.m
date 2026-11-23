/*
 * The loader: the executable of "Pillars of Eternity.app".
 *
 * 1. Find the game directory.
 *    - With "--game-dir <path>", use that directory. The launcher window
 *      does not open.
 *    - Else the launcher window opens (rt_launcher.m). It shows the folder
 *      that the player selected before, or no folder on the first start.
 *      The player selects the folder with "Choose Folder…" and can change
 *      the graphics settings. Play starts the game; a closed window quits.
 * 2. Check the SHA-256 values of PillarsOfEternity.exe, mono.dll and
 *    ShroudUnityPlugin.dll. Only the GOG version is supported, because the
 *    converter output is tested for these files.
 * 3. Get the game program (the runtime and the translated game code):
 *    ~/Library/Application Support/Pillars of Eternity Mac Port/GameCode/
 *    <id>/game/PillarsOfEternity. If it does not exist, the converter makes
 *    it (one time) from the game files of the player, with a progress
 *    window. The converter needs Apple's linker: if the Command Line Tools
 *    (or Xcode) are not installed, an alert offers their installer and the
 *    loader quits. <id> comes from the SHA-256 value of
 *    Recompiler/manifest.txt, which has a value of all other files of the
 *    converter (the runtime, the tools and the translation inputs). A
 *    release that changes one of them makes a new program; a release that
 *    changes only this executable (for example the launcher window) keeps
 *    the program. Of the programs with a different <id>, the loader keeps
 *    the one that was used last and removes the others.
 * 4. Start the game program:
 *      PillarsOfEternity <game directory> <player directory> -force-d3d9 -nolog
 *    The player directory (Player in the folder of the port) holds the
 *    saved games and the settings of the game (user/), the data of the
 *    game for all users (programdata/) and the cache of the Metal pipelines.
 *    No log files are written: -nolog stops the output_log.txt of Unity,
 *    the runtime writes only the message of a fatal error, and the loader
 *    keeps the end of the error output of the game in memory. The loader
 *    removes the log files of earlier versions of the port
 *    (PillarsOfEternity.log, PillarsOfEternity.previous.log). It waits
 *    until the game ends. If the game stops with an error, an alert shows
 *    the end of the error output.
 *
 * An error shows an alert and ends the process.
 */
#import <Cocoa/Cocoa.h>

#include "rt_convert.h"
#include "rt_launcher.h"

#include <CommonCrypto/CommonDigest.h>
#include <copyfile.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <pthread.h>
#include <removefile.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

extern char **environ;

#define PORT_NAME "Pillars of Eternity Mac Port"
#define GAME_TITLE @"Pillars of Eternity"
#define DEFAULTS_GAME_DIR @"GameDirectory"
#define PATH_LEN 1024
#define GAME_PROGRAM "PillarsOfEternity"
/* The log files of earlier versions of the port. */
#define OLD_LOG_NAME "PillarsOfEternity.log"
#define OLD_PREVIOUS_LOG_NAME "PillarsOfEternity.previous.log"
#define LOG_TAIL 2000

typedef struct pinned_file {
    const char *name;
    const char *sha256;
} pinned_file;

/* The GOG version of Pillars of Eternity (the same values are in
   runtime/host/player_main.c and in the manifest of the converter). */
static const pinned_file pinned[] = {
    { "PillarsOfEternity.exe", "053c3fd4de19abced9be429d1d89f779f2da6bc13d9706c9768daf294c01931d" },
    { "PillarsOfEternity_data/Mono/mono.dll", "4e926907c947611b9fd6c88e9d8902d187a9a89c796e935937fd9b303419674d" },
    { "PillarsOfEternity_data/Plugins/ShroudUnityPlugin.dll",
      "43ec63429a4c708ed2e55abb23488704592d785c8230e9bf83adc67fece20a26" },
};
#define NPINNED (sizeof pinned / sizeof pinned[0])

/* ---------------------------------------------------------------- messages */

static void ensure_app(void)
{
    static bool done;
    if (done) return;
    done = true;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
}

static void show_alert(NSString *title, NSString *text, NSAlertStyle style)
{
    NSAlert *a;
    ensure_app();
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    a = [NSAlert new];
    a.alertStyle = style;
    a.messageText = title;
    a.informativeText = text;
    [NSApp activate];
    [a runModal];
}

static _Noreturn void fail(NSString *title, NSString *text)
{
    show_alert(title, text, NSAlertStyleCritical);
    exit(1);
}

static NSString *str(const char *s)
{
    NSString *r = [NSString stringWithUTF8String:s];
    return r != nil ? r : @"(a name that is not UTF-8)";
}

/* ---------------------------------------------------------------- paths */

static bool join(char *out, size_t n, const char *dir, const char *name)
{
    int r = snprintf(out, n, "%s/%s", dir, name);
    return r > 0 && (size_t)r < n;
}

static bool is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void mkdir_p(const char *path)
{
    NSError *e = nil;
    if (![NSFileManager.defaultManager createDirectoryAtPath:str(path)
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:&e])
        fail(@"A folder cannot be made.", [NSString stringWithFormat:@"%s: %@", path, e.localizedDescription]);
}

/* The folder of the port in Application Support. */
static void port_dir(char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (home == NULL || !join(out, n, home, "Library/Application Support/" PORT_NAME))
        fail(@"The home folder is not known.", @"The HOME variable is not set or is too long.");
}

/* ---------------------------------------------------------------- game directory */

/* Does dir hold all game files? If not, *why tells the player what is
   wrong (a parent folder, the Mac version of the game, or no game files). */
static bool check_dir(const char *dir, NSString **why)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSArray<NSString *> *names;
    NSString *d = str(dir);
    size_t i;
    bool all = true;

    for (i = 0; i < NPINNED; i++)
    {
        char p[PATH_LEN];
        if (!join(p, sizeof p, dir, pinned[i].name) || !is_file(p)) all = false;
    }
    if (all) return true;
    if (why == NULL) return false;
    names = [fm contentsOfDirectoryAtPath:d error:nil];
    for (NSString *n in names)
    {
        NSString *sub = [d stringByAppendingPathComponent:n];
        BOOL isdir = NO;
        if ([fm fileExistsAtPath:sub isDirectory:&isdir] && isdir &&
            [fm fileExistsAtPath:[sub stringByAppendingPathComponent:@"PillarsOfEternity.exe"]])
        {
            *why = [NSString stringWithFormat:@"The game files are in the folder \"%@\". Select that folder.", sub];
            return false;
        }
    }
    if ([d.pathExtension.lowercaseString isEqualToString:@"app"] ||
        [fm fileExistsAtPath:[d stringByAppendingPathComponent:@"PillarsOfEternity.app"]])
    {
        *why = [NSString stringWithFormat:@"The folder \"%@\" holds the Mac version of the game. This port needs the "
                                          @"installed Windows version from GOG (the folder that holds "
                                          @"PillarsOfEternity.exe).",
                                          d];
        return false;
    }
    *why = [NSString stringWithFormat:@"The folder \"%@\" does not hold PillarsOfEternity.exe and the folder "
                                      @"PillarsOfEternity_data. Select the installed game folder of the GOG version "
                                      @"of Pillars of Eternity.",
                                      d];
    return false;
}

static bool copy_path(char *out, size_t n, const char *in)
{
    return (size_t)snprintf(out, n, "%s", in) < n;
}

/* The problem of the SHA-256 values of the game files in dir, or nil. */
static NSString *hash_problem(const char *dir, NSString **title)
{
    size_t i;
    for (i = 0; i < NPINNED; i++)
    {
        char path[PATH_LEN], sum[65], err[PATH_LEN + 64];
        if (!join(path, sizeof path, dir, pinned[i].name))
        {
            *title = @"The game folder path is too long.";
            return str(dir);
        }
        if (!rt_sha256_file(path, sum, err, sizeof err))
        {
            *title = @"A game file cannot be read.";
            return str(err);
        }
        if (strcmp(sum, pinned[i].sha256) != 0)
        {
            *title = [NSString stringWithFormat:@"%s is not the supported version.", pinned[i].name];
            return [NSString stringWithFormat:@"This port supports only the GOG version of Pillars of Eternity. The "
                                              @"file %s has the SHA-256 value %s; the supported file has %s.",
                                              path, sum, pinned[i].sha256];
        }
    }
    return nil;
}

/* The check of the launcher window: the problem of dir for the player, or
   nil if the game can start from dir. */
static NSString *dir_problem(const char *dir)
{
    NSString *why = nil, *title = nil, *problem;
    if (!check_dir(dir, &why)) return why;
    problem = hash_problem(dir, &title);
    if (problem == nil) return nil;
    return [NSString stringWithFormat:@"%@ %@", title, problem];
}

static void find_game_dir(int argc, char **argv, const char *settings, char *out, size_t n)
{
    NSString *remembered = [NSUserDefaults.standardUserDefaults stringForKey:DEFAULTS_GAME_DIR];
    char found[PATH_LEN];
    const char *initial = NULL;
    int i;

    for (i = 1; i < argc; i++)
    {
        NSString *why = nil;
        if (strcmp(argv[i], "--game-dir") != 0) continue;
        if (i + 1 >= argc) fail(@"The option --game-dir needs a folder.", @"Give the game folder after --game-dir.");
        if (strlen(argv[i + 1]) >= PATH_MAX || n < PATH_MAX || realpath(argv[i + 1], out) == NULL)
            fail(@"The game folder does not exist.", str(argv[i + 1]));
        if (!check_dir(out, &why)) fail(@"The game folder cannot be used.", why);
        return;
    }
    /* The folder that the player selected before. The window checks it and
       shows its problem if its files were moved or removed. */
    if (remembered != nil && remembered.fileSystemRepresentation != NULL &&
        copy_path(found, sizeof found, remembered.fileSystemRepresentation))
        initial = found;
    ensure_app();
    if (!rt_launcher_run(settings, initial, dir_problem, out, n)) exit(0);
    [NSUserDefaults.standardUserDefaults setObject:str(out) forKey:DEFAULTS_GAME_DIR];
}

static void check_game_files(const char *dir)
{
    NSString *title = nil, *problem = hash_problem(dir, &title);
    if (problem != nil) fail(title, problem);
}

/* ---------------------------------------------------------------- Command Line Tools */

/* The converter links with Apple's linker. If no developer directory
   exists, offer the installer of the Command Line Tools and quit: the
   installer runs on its own, and the player opens the application again
   when it is done. */
static void require_developer_tools(rt_convert_tools *tools)
{
    char err[1024];
    if (!rt_convert_have_developer_dir())
    {
        NSAlert *a;
        NSModalResponse r;
        ensure_app();
        a = [NSAlert new];
        a.alertStyle = NSAlertStyleInformational;
        a.messageText = @"The Command Line Tools are necessary.";
        a.informativeText = @"On the first start, Pillars of Eternity is prepared for this Mac with the linker of "
                            @"Apple's Command Line Tools for Xcode. Click Install to open the installer of macOS. "
                            @"When the installation is complete, open Pillars of Eternity again.";
        [a addButtonWithTitle:@"Install"];
        [a addButtonWithTitle:@"Quit"];
        [NSApp activate];
        r = [a runModal];
        if (r == NSAlertFirstButtonReturn)
        {
            char *argv[] = { (char *)"/usr/bin/xcode-select", (char *)"--install", NULL };
            pid_t pid;
            int status = 0, e = posix_spawn(&pid, argv[0], NULL, NULL, argv, environ);
            if (e != 0) fail(@"The installer cannot be opened.", [NSString stringWithFormat:@"xcode-select: %s", strerror(e)]);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                fail(@"The installer cannot be opened.",
                     @"xcode-select --install failed. Install the Command Line Tools with the command "
                     @"\"xcode-select --install\" in Terminal, then open Pillars of Eternity again.");
        }
        exit(0);
    }
    if (!rt_convert_find_tools(tools, err, sizeof err))
        fail(@"The linker of the Command Line Tools cannot be used.",
             [NSString stringWithFormat:@"%s. If Xcode is installed, open it one time to complete its installation, "
                                        @"or install the Command Line Tools with \"xcode-select --install\" in "
                                        @"Terminal.",
                                        err]);
}

/* ---------------------------------------------------------------- conversion */

@interface RTProgress : NSObject <NSWindowDelegate>
@property(nonatomic) NSWindow *window;
@property(nonatomic) NSProgressIndicator *bar;
@property(nonatomic) NSTextField *detail;
@property(atomic) BOOL closed;
@end

@implementation RTProgress
- (instancetype)init
{
    NSTextField *title;
    NSView *v;
    if ((self = [super init]) == nil) return nil;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 150)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = GAME_TITLE;
    _window.releasedWhenClosed = NO;
    _window.delegate = self;
    v = _window.contentView;
    title = [NSTextField labelWithString:@"Preparing Pillars of Eternity for this Mac"];
    title.font = [NSFont boldSystemFontOfSize:15];
    title.frame = NSMakeRect(24, 100, 432, 24);
    [v addSubview:title];
    _detail = [NSTextField labelWithString:@"This is done one time. It can take a long time."];
    _detail.frame = NSMakeRect(24, 72, 432, 20);
    [v addSubview:_detail];
    _bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(24, 36, 432, 20)];
    _bar.indeterminate = NO;
    _bar.minValue = 0;
    _bar.maxValue = 1;
    [v addSubview:_bar];
    [_window center];
    return self;
}

- (BOOL)windowShouldClose:(NSWindow *)sender
{
    (void)sender;
    self.closed = YES;
    rt_convert_cancel();
    return YES;
}

/* The quit Apple event (Quit in the Dock, log out, shut down) does what
   the close button does. */
- (void)handleQuitEvent:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply
{
    (void)event;
    (void)reply;
    self.closed = YES;
    rt_convert_cancel();
}
@end

typedef struct convert_state {
    rt_convert_job job;
    void *ui; /* RTProgress, retained by the caller */
    atomic_bool finished;
    bool ok;
} convert_state;

static void post_wake_event(void)
{
    NSEvent *e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                    location:NSZeroPoint
                               modifierFlags:0
                                   timestamp:0
                                windowNumber:0
                                     context:nil
                                     subtype:0
                                       data1:0
                                       data2:0];
    [NSApp postEvent:e atStart:NO];
}

static void on_progress(void *ctx, double fraction, const char *text)
{
    convert_state *s = ctx;
    NSString *t = str(text);
    RTProgress *ui = (__bridge RTProgress *)s->ui;
    dispatch_async(dispatch_get_main_queue(), ^{
        ui.bar.doubleValue = fraction;
        ui.detail.stringValue = t;
    });
}

static void *convert_thread(void *arg)
{
    convert_state *s = arg;
    @autoreleasepool
    {
        s->ok = rt_convert_run(&s->job);
        atomic_store(&s->finished, true);
        post_wake_event();
    }
    return NULL;
}

static bool remove_tree(const char *path, char *err, size_t errn)
{
    struct stat st;
    if (lstat(path, &st) != 0 && errno == ENOENT) return true;
    if (removefile(path, NULL, REMOVEFILE_RECURSIVE) != 0)
    {
        snprintf(err, errn, "cannot remove %s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

/* Remove the quarantine attribute from each file of the copy of the
   Recompiler directory, so that the tools can run. macOS removes an
   attribute only if the owner can write the file. Some files of the
   release are read-only (libclang_rt.osx.a has mode 0444), so a read-only
   file or folder of the copy (the copy belongs to the loader) gets the
   write permission of the owner before the removal. */
static void strip_quarantine(const char *root)
{
    static const char attr[] = "com.apple.quarantine";
    char *paths[2] = { (char *)root, NULL };
    FTS *fts = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    FTSENT *e;
    if (fts == NULL) fail(@"The converter cannot be prepared.", str(strerror(errno)));
    while ((e = fts_read(fts)) != NULL)
    {
        if (e->fts_info == FTS_DP) continue;
        if (e->fts_info == FTS_ERR || e->fts_info == FTS_DNR || e->fts_info == FTS_NS)
            fail(@"The converter cannot be prepared.", str(strerror(e->fts_errno)));
        if (getxattr(e->fts_path, attr, NULL, 0, 0, XATTR_NOFOLLOW) < 0)
        {
            if (errno == ENOATTR) continue;
            fail(@"The converter cannot be prepared.",
                 [NSString stringWithFormat:@"%s: %s", e->fts_path, strerror(errno)]);
        }
        if (removexattr(e->fts_path, attr, XATTR_NOFOLLOW) == 0) continue;
        if (errno == EACCES && !S_ISLNK(e->fts_statp->st_mode) && (e->fts_statp->st_mode & S_IWUSR) == 0 &&
            chmod(e->fts_path, (e->fts_statp->st_mode & 07777) | S_IWUSR) == 0 &&
            removexattr(e->fts_path, attr, XATTR_NOFOLLOW) == 0)
            continue;
        fail(@"The converter cannot be prepared.", [NSString stringWithFormat:@"%s: %s", e->fts_path, strerror(errno)]);
    }
    fts_close(fts);
}

/* Clean the GameCode directory. Keep the entry keep and the complete game
   program that was used last of the other entries (the marker "complete"
   gets the time of each use), so that a player who uses two versions of
   the application does not convert the game at each change. Remove all
   other entries. */
static void remove_other_caches(const char *base, const char *keep)
{
    NSError *e = nil;
    NSArray<NSString *> *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:str(base) error:&e];
    NSString *newest = nil;
    struct timespec newest_time = { 0, 0 };
    if (names == nil)
        fail(@"The folder of the game code cannot be read.", [NSString stringWithFormat:@"%s: %@", base, e.localizedDescription]);
    for (NSString *n in names)
    {
        char p[PATH_LEN], m[PATH_LEN];
        struct stat st;
        if ([n isEqualToString:str(keep)]) continue;
        if (!join(p, sizeof p, base, n.fileSystemRepresentation) || !join(m, sizeof m, p, "complete"))
            fail(@"The home folder path is too long.", str(base));
        if (!is_dir(p) || stat(m, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (newest == nil || st.st_mtimespec.tv_sec > newest_time.tv_sec ||
            (st.st_mtimespec.tv_sec == newest_time.tv_sec && st.st_mtimespec.tv_nsec > newest_time.tv_nsec))
        {
            newest = n;
            newest_time = st.st_mtimespec;
        }
    }
    for (NSString *n in names)
    {
        char p[PATH_LEN], err[PATH_LEN + 64];
        if ([n isEqualToString:str(keep)] || (newest != nil && [n isEqualToString:newest])) continue;
        if (!join(p, sizeof p, base, n.fileSystemRepresentation)) fail(@"The home folder path is too long.", str(base));
        if (!remove_tree(p, err, sizeof err)) fail(@"An old copy of the game code cannot be removed.", str(err));
    }
}

/* The game program: from the cache, or made now by the converter. */
static void get_program(const char *game_dir, const char *base_dir, char *program, size_t n)
{
    NSString *res = NSBundle.mainBundle.resourcePath;
    char recompiler[PATH_LEN], manifest[PATH_LEN], base[PATH_LEN], dir[PATH_LEN], marker[PATH_LEN], copy[PATH_LEN],
        work[PATH_LEN], out[PATH_LEN], err[PATH_LEN + 64], key[65], id[17];
    rt_convert_tools tools;
    convert_state *s;
    RTProgress *ui;
    pthread_t th;
    pthread_attr_t attr;
    FILE *f;

    if (res == nil) fail(@"The application is damaged.", @"The resources were not found.");
    if (!join(recompiler, sizeof recompiler, res.fileSystemRepresentation, "Recompiler") ||
        !join(manifest, sizeof manifest, recompiler, "manifest.txt") || !is_file(manifest))
        fail(@"The application is damaged.", @"Contents/Resources/Recompiler/manifest.txt was not found.");
    if (!rt_sha256_file(manifest, key, err, sizeof err)) fail(@"The application is damaged.", str(err));
    memcpy(id, key, 16);
    id[16] = 0;

    if (!join(base, sizeof base, base_dir, "GameCode") || !join(dir, sizeof dir, base, id) ||
        !join(marker, sizeof marker, dir, "complete") || !join(copy, sizeof copy, dir, "Recompiler") ||
        !join(work, sizeof work, dir, "work") || !join(out, sizeof out, dir, "game") ||
        !join(program, n, out, GAME_PROGRAM))
        fail(@"The home folder path is too long.", str(base_dir));
    mkdir_p(base);
    remove_other_caches(base, id);
    if (is_file(marker) && is_file(program))
    {
        /* The time of this use (see remove_other_caches). */
        if (utimes(marker, NULL) != 0)
            fail(@"The converted game code cannot be recorded.", [NSString stringWithFormat:@"%s: %s", marker, strerror(errno)]);
        return;
    }

    require_developer_tools(&tools);
    /* A conversion that did not complete leaves files; start again. */
    if (!remove_tree(dir, err, sizeof err)) fail(@"An old copy of the game code cannot be removed.", str(err));
    mkdir_p(dir);
    if (copyfile(recompiler, copy, NULL, COPYFILE_CLONE | COPYFILE_RECURSIVE) != 0)
        fail(@"The converter cannot be prepared.",
             [NSString stringWithFormat:@"Copy of %s to %s: %s", recompiler, copy, strerror(errno)]);
    strip_quarantine(copy);
    mkdir_p(work);
    mkdir_p(out);

    ensure_app();
    ui = [RTProgress new];
    [ui.window makeKeyAndOrderFront:nil];
    [NSApp activate];
    s = calloc(1, sizeof *s);
    if (s == NULL) fail(@"Out of memory.", @"The conversion cannot start.");
    s->job.recompiler = copy;
    s->job.game_dir = game_dir;
    s->job.work_dir = work;
    s->job.out_dir = out;
    s->job.tools = &tools;
    s->job.progress = on_progress;
    s->job.ctx = s;
    s->ui = (__bridge void *)ui;
    atomic_init(&s->finished, false);
    if (pthread_attr_init(&attr) != 0 || pthread_attr_setstacksize(&attr, 8u << 20) != 0 ||
        pthread_create(&th, &attr, convert_thread, s) != 0)
        fail(@"The conversion cannot start.", @"A thread cannot be made.");
    pthread_attr_destroy(&attr);
    [NSAppleEventManager.sharedAppleEventManager setEventHandler:ui
                                                     andSelector:@selector(handleQuitEvent:withReplyEvent:)
                                                   forEventClass:kCoreEventClass
                                                      andEventID:kAEQuitApplication];
    while (!atomic_load(&s->finished))
    {
        @autoreleasepool
        {
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:NSDate.distantFuture
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
            if (e != nil && e.type != NSEventTypeApplicationDefined) [NSApp sendEvent:e];
        }
    }
    [NSAppleEventManager.sharedAppleEventManager removeEventHandlerForEventClass:kCoreEventClass
                                                                  andEventID:kAEQuitApplication];
    if (pthread_join(th, NULL) != 0) fail(@"The conversion failed.", @"pthread_join failed.");
    [ui.window orderOut:nil];
    if (!s->ok)
    {
        char rm_err[PATH_LEN + 64];
        bool closed = ui.closed;
        NSString *msg = str(s->job.error);
        bool removed = remove_tree(dir, rm_err, sizeof rm_err);
        free(s);
        if (!removed)
            fail(@"Pillars of Eternity cannot be prepared for this Mac.",
                 [NSString stringWithFormat:@"%@\n\nThe work folder cannot be removed: %s", closed ? @"The conversion was cancelled." : msg, rm_err]);
        if (closed) exit(0);
        fail(@"Pillars of Eternity cannot be prepared for this Mac.", msg);
    }
    free(s);
    if (!remove_tree(work, err, sizeof err) || !remove_tree(copy, err, sizeof err))
        fail(@"The temporary files of the conversion cannot be removed.", str(err));
    f = fopen(marker, "w");
    if (f == NULL || fprintf(f, "%s\n", key) < 0 || fclose(f) != 0)
        fail(@"The converted game code cannot be recorded.", [NSString stringWithFormat:@"%s: %s", marker, strerror(errno)]);
    /* A quit or a close that came after the conversion completed: keep the
       result for the next start, but do not start the game. */
    if (ui.closed) exit(0);
}

/* ---------------------------------------------------------------- the game */

/* The quit Apple event while the game runs: the loader has no window, so it
   ends; the game continues and ends on its own. */
@interface RTWaiter : NSObject
@property(atomic) BOOL quit;
@end

@implementation RTWaiter
- (void)handleQuitEvent:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply
{
    (void)event;
    (void)reply;
    self.quit = YES;
}
@end

/* The end of the error output of the game (main queue only). */
static char g_tail[LOG_TAIL];
static size_t g_tail_len;

/* Read what the game wrote to the pipe (non-blocking) and keep the last
   LOG_TAIL bytes. Returns false at the end of the output. */
static bool read_output(int fd)
{
    char chunk[4096];
    for (;;)
    {
        ssize_t n = read(fd, chunk, sizeof chunk);
        size_t add, drop;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return true;
        if (n <= 0) return false;
        add = (size_t)n;
        if (add >= LOG_TAIL)
        {
            memcpy(g_tail, chunk + add - LOG_TAIL, LOG_TAIL);
            g_tail_len = LOG_TAIL;
            continue;
        }
        drop = g_tail_len + add > LOG_TAIL ? g_tail_len + add - LOG_TAIL : 0;
        memmove(g_tail, g_tail + drop, g_tail_len - drop);
        g_tail_len -= drop;
        memcpy(g_tail + g_tail_len, chunk, add);
        g_tail_len += add;
    }
}

static NSString *output_tail(void)
{
    NSString *s;
    if (g_tail_len == 0) return @"(the game wrote no message)";
    s = [[NSString alloc] initWithBytes:g_tail length:g_tail_len encoding:NSUTF8StringEncoding];
    return s ?: [[NSString alloc] initWithBytes:g_tail length:g_tail_len encoding:NSISOLatin1StringEncoding];
}

static void remove_old_log(const char *base_dir, const char *name)
{
    char path[PATH_LEN];
    if (!join(path, sizeof path, base_dir, name)) fail(@"The home folder path is too long.", str(base_dir));
    if (unlink(path) != 0 && errno != ENOENT)
        fail(@"An old log file cannot be removed.", [NSString stringWithFormat:@"%s: %s", path, strerror(errno)]);
}

static _Noreturn void run_game(const char *program, const char *game_dir, const char *player_dir, const char *base_dir)
{
    char user[PATH_LEN], data[PATH_LEN];
    char *argv[] = { (char *)program, (char *)game_dir, (char *)player_dir, (char *)"-force-d3d9", (char *)"-nolog", NULL };
    posix_spawn_file_actions_t fa;
    __block bool ended = false;
    __block int status = 0;
    RTWaiter *waiter = [RTWaiter new];
    dispatch_source_t src, out_src;
    pid_t pid;
    int r, out[2];

    if (!join(user, sizeof user, player_dir, "user") || !join(data, sizeof data, player_dir, "programdata"))
        fail(@"The home folder path is too long.", str(base_dir));
    mkdir_p(user);
    mkdir_p(data);
    remove_old_log(base_dir, OLD_LOG_NAME);
    remove_old_log(base_dir, OLD_PREVIOUS_LOG_NAME);
    /* The standard output and the standard error of the game go to a pipe;
       the loader keeps the end in memory. */
    if (pipe(out) != 0) fail(@"The game cannot start.", [NSString stringWithFormat:@"pipe failed: %s", strerror(errno)]);
    if (fcntl(out[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(out[0], F_SETFL, O_NONBLOCK) != 0)
        fail(@"The game cannot start.", [NSString stringWithFormat:@"fcntl failed: %s", strerror(errno)]);
    if (posix_spawn_file_actions_init(&fa) != 0) fail(@"The game cannot start.", @"posix_spawn_file_actions_init failed.");
    r = posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (r == 0) r = posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    if (r == 0) r = posix_spawn_file_actions_adddup2(&fa, out[1], 2);
    if (r == 0) r = posix_spawn_file_actions_addclose(&fa, out[1]);
    if (r == 0) r = posix_spawn(&pid, program, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (close(out[1]) != 0) fail(@"The game cannot start.", [NSString stringWithFormat:@"close failed: %s", strerror(errno)]);
    if (r != 0) fail(@"The game cannot start.", [NSString stringWithFormat:@"%s: %s", program, strerror(r)]);
    out_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)out[0], 0, dispatch_get_main_queue());
    if (out_src == nil) fail(@"The game cannot be watched.", @"dispatch_source_create failed.");
    int out_fd = out[0];
    dispatch_source_set_event_handler(out_src, ^{
      if (!read_output(out_fd)) dispatch_source_cancel(out_src);
    });
    dispatch_resume(out_src);

    /* The game has its own windows and its own entry in the Dock. */
    ensure_app();
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)pid, DISPATCH_PROC_EXIT, dispatch_get_main_queue());
    if (src == nil) fail(@"The game cannot be watched.", @"dispatch_source_create failed.");
    dispatch_source_set_event_handler(src, ^{
      pid_t got;
      if (ended) return;
      do got = waitpid(pid, &status, 0);
      while (got < 0 && errno == EINTR);
      if (got != pid) status = -1;
      ended = true;
      dispatch_source_cancel(src);
      post_wake_event();
    });
    dispatch_resume(src);
    /* A game that ended before the source was made. */
    dispatch_async(dispatch_get_main_queue(), ^{
      pid_t got;
      if (ended) return;
      do got = waitpid(pid, &status, WNOHANG);
      while (got < 0 && errno == EINTR);
      if (got == 0) return;
      if (got != pid) status = -1;
      ended = true;
      dispatch_source_cancel(src);
      post_wake_event();
    });
    [NSAppleEventManager.sharedAppleEventManager setEventHandler:waiter
                                                     andSelector:@selector(handleQuitEvent:withReplyEvent:)
                                                   forEventClass:kCoreEventClass
                                                      andEventID:kAEQuitApplication];
    while (!ended && !waiter.quit)
    {
        @autoreleasepool
        {
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:NSDate.distantFuture
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
            if (e != nil && e.type != NSEventTypeApplicationDefined) [NSApp sendEvent:e];
        }
    }
    if (!ended) exit(0);
    if (status == -1) fail(@"The game cannot be watched.", @"waitpid failed.");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) exit(0);
    /* What the game wrote last, before the source delivered it. */
    (void)read_output(out[0]);
    fail(WIFSIGNALED(status) ? [NSString stringWithFormat:@"The game stopped with signal %d.", WTERMSIG(status)]
                             : [NSString stringWithFormat:@"The game stopped with exit status %d.", WEXITSTATUS(status)],
         [NSString stringWithFormat:@"The end of its error output:\n\n%@", output_tail()]);
}

int main(int argc, char **argv)
{
    char base[PATH_LEN], player[PATH_LEN], settings[PATH_LEN], game[PATH_MAX], program[PATH_LEN];
    @autoreleasepool
    {
        port_dir(base, sizeof base);
        if (!join(player, sizeof player, base, "Player") ||
            !join(settings, sizeof settings, player, "user/PortGraphics.cfg"))
            fail(@"The home folder path is too long.", str(base));
        mkdir_p(base);
        find_game_dir(argc, argv, settings, game, sizeof game);
        check_game_files(game);
        get_program(game, base, program, sizeof program);
        mkdir_p(player);
        run_game(program, game, player, base);
    }
}
