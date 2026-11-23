/*
 * ADVAPI32 security functions: security identifiers (SIDs), access tokens,
 * access control lists (ACLs) and the security of files.
 *
 * Security model of the emulated system:
 *   - One user, "Player", on a workgroup computer. Its SID is
 *     S-1-5-21-A-B-C-R: A, B and C come from the host hardware UUID, and the
 *     relative identifier R is 1000 plus the host user id.
 *   - The access token of the process has the user, the primary group
 *     "None" (RID 513 of the computer domain) and the groups of a Windows XP
 *     administrator: None, Everyone, BUILTIN\Administrators, BUILTIN\Users,
 *     NT AUTHORITY\INTERACTIVE, NT AUTHORITY\Authenticated Users and LOCAL.
 *     The token has no logon SID and no privileges.
 *   - Each token handle is its own token object (OBJ_TOKEN). All of them
 *     show the same user. The object records the token type, the
 *     impersonation level and the access of the handle.
 *   - A thread impersonates with ImpersonateLoggedOnUser until RevertToSelf.
 *     Its token is in rt_thread.token.
 *
 * The security of a file comes from the host file mode:
 *   - owner: the user SID if the host user owns the file, BUILTIN\
 *     Administrators for root, else S-1-5-21-A-B-C-(1000 + uid);
 *   - group: "None";
 *   - DACL: one access-allowed ACE each for the owner, "None" and Everyone,
 *     from the user, group and other permission bits.
 * SetNamedSecurityInfoW writes a DACL as a new file mode. Each permission
 * class gets the rights that the DACL gives to its trustee: read
 * (FILE_READ_DATA), write (FILE_WRITE_DATA) and execute (FILE_EXECUTE).
 * A change of the owner or the group is not possible.
 */
#include "rt_file.h"
#include "rt_object.h"
#include "rt_text.h"
#include "rt_thread.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ERROR_NO_TOKEN 1008u
#define ERROR_INVALID_OWNER 1307u
#define ERROR_INVALID_PRIMARY_GROUP 1308u
#define ERROR_PRIVILEGE_NOT_HELD 1314u
#define ERROR_NONE_MAPPED 1332u
#define ERROR_INVALID_ACL 1336u
#define ERROR_INVALID_SID 1337u
#define ERROR_ALLOTTED_SPACE_EXCEEDED 1344u
#define ERROR_BAD_IMPERSONATION_LEVEL 1346u
#define ERROR_CANT_OPEN_ANONYMOUS 1347u

#define SID_REVISION 1u
#define SID_MAX_SUB_AUTHORITIES 15u

/* Token access rights. */
#define TOKEN_DUPLICATE 0x2u
#define TOKEN_IMPERSONATE 0x4u
#define TOKEN_QUERY 0x8u
#define TOKEN_ALL_ACCESS 0xF01FFu
#define TOKEN_READ 0x20008u
#define TOKEN_WRITE 0x200E0u
#define TOKEN_EXECUTE 0x20000u
#define MAXIMUM_ALLOWED 0x2000000u

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_EXECUTE 0x20000000u
#define GENERIC_ALL 0x10000000u

/* File access rights. */
#define FILE_READ_DATA 0x1u
#define FILE_WRITE_DATA 0x2u
#define FILE_EXECUTE 0x20u
#define FILE_GENERIC_READ 0x120089u
#define FILE_GENERIC_WRITE 0x120116u
#define FILE_GENERIC_EXECUTE 0x1200A0u
#define FILE_ALL_ACCESS 0x1F01FFu

/* TOKEN_INFORMATION_CLASS */
#define TokenUser 1u
#define TokenGroups 2u
#define TokenOwner 4u
#define TokenPrimaryGroup 5u
#define TokenType 8u
#define TokenImpersonationLevel 9u
#define TOKEN_CLASS_MAX_XP 17u /* TokenOrigin */

#define TOKEN_PRIMARY 1u
#define TOKEN_IMPERSONATION 2u
#define SECURITY_ANONYMOUS 0u
#define SECURITY_IMPERSONATION 2u
#define SECURITY_DELEGATION 3u

#define SE_GROUP_MANDATORY 0x1u
#define SE_GROUP_ENABLED_BY_DEFAULT 0x2u
#define SE_GROUP_ENABLED 0x4u
#define SE_GROUP_OWNER 0x8u

/* SID_NAME_USE */
#define SidTypeUser 1u
#define SidTypeGroup 2u
#define SidTypeDomain 3u
#define SidTypeAlias 4u
#define SidTypeWellKnownGroup 5u

/* ACLs */
#define ACL_REVISION 2u
#define ACL_REVISION_DS 4u
#define ACL_MAX_SIZE 0xFFFCu
#define ACCESS_ALLOWED_ACE_TYPE 0u
#define ACCESS_DENIED_ACE_TYPE 1u
#define SYSTEM_AUDIT_ACE_TYPE 2u
#define INHERIT_ONLY_ACE 0x8u
#define INHERITED_ACE 0x10u
#define VALID_INHERIT_FLAGS 0x1Fu

/* SECURITY_INFORMATION */
#define OWNER_SECURITY_INFORMATION 0x1u
#define GROUP_SECURITY_INFORMATION 0x2u
#define DACL_SECURITY_INFORMATION 0x4u
#define SACL_SECURITY_INFORMATION 0x8u
#define UNPROTECTED_SACL_SECURITY_INFORMATION 0x10000000u
#define UNPROTECTED_DACL_SECURITY_INFORMATION 0x20000000u
#define PROTECTED_SACL_SECURITY_INFORMATION 0x40000000u
#define PROTECTED_DACL_SECURITY_INFORMATION 0x80000000u

#define SE_FILE_OBJECT 1u
#define SE_SELF_RELATIVE 0x8000u
#define SE_DACL_PRESENT 0x4u
#define SE_DACL_PROTECTED 0x1000u

/* ACCESS_MODE */
#define NOT_USED_ACCESS 0u
#define GRANT_ACCESS 1u
#define SET_ACCESS 2u
#define DENY_ACCESS 3u
#define REVOKE_ACCESS 4u
#define SET_AUDIT_SUCCESS 5u
#define SET_AUDIT_FAILURE 6u

/* TRUSTEE_W: pMultipleTrustee, MultipleTrusteeOperation, TrusteeForm,
   TrusteeType, ptstrName. */
#define TRUSTEE_SIZE 20u
#define NO_MULTIPLE_TRUSTEE 0u
#define TRUSTEE_IS_SID 0u
#define TRUSTEE_IS_UNKNOWN 0u
/* EXPLICIT_ACCESS_W: grfAccessPermissions, grfAccessMode, grfInheritance,
   Trustee. */
#define EXPLICIT_ACCESS_SIZE 32u
#define MAX_EXPLICIT_ENTRIES 4096u

/* ---------------------------------------------------------------- SIDs */

typedef struct sid {
    uint8_t auth[6];
    uint8_t n;
    uint32_t sub[SID_MAX_SUB_AUTHORITIES];
} sid;

static uint32_t sid_len(const sid *s) { return 8u + 4u * s->n; }

static bool sid_eq(const sid *a, const sid *b)
{
    return a->n == b->n && memcmp(a->auth, b->auth, 6) == 0 && memcmp(a->sub, b->sub, 4u * a->n) == 0;
}

/* Read the guest SID at g. Returns false for an invalid SID. */
static bool sid_read(uint32_t g, sid *s)
{
    const uint8_t *p;
    uint8_t i;
    if (g == 0) return false;
    p = G2H(g);
    if (p[0] != SID_REVISION || p[1] > SID_MAX_SUB_AUTHORITIES) return false;
    s->n = p[1];
    memcpy(s->auth, p + 2, 6);
    for (i = 0; i < s->n; i++) s->sub[i] = rd32(g + 8u + 4u * i);
    return true;
}

static void sid_write(uint32_t g, const sid *s)
{
    uint8_t *p = G2H(g);
    uint8_t i;
    p[0] = SID_REVISION;
    p[1] = s->n;
    memcpy(p + 2, s->auth, 6);
    for (i = 0; i < s->n; i++) wr32(g + 8u + 4u * i, s->sub[i]);
}

static sid make_sid(uint8_t authority, uint8_t n, const uint32_t *sub)
{
    sid s;
    memset(&s, 0, sizeof(s));
    s.auth[5] = authority;
    s.n = n;
    memcpy(s.sub, sub, 4u * n);
    return s;
}

/* The computer domain S-1-5-21-A-B-C. */
static uint32_t domain_sub[3];
static pthread_once_t domain_once = PTHREAD_ONCE_INIT;

static void make_domain(void)
{
    uuid_t id;
    struct timespec wait = { 5, 0 };
    int i;
    if (gethostuuid(id, &wait) != 0) rt_fatal("gethostuuid failed: %s", strerror(errno));
    for (i = 0; i < 3; i++)
        domain_sub[i] = (uint32_t)id[4 * i] | (uint32_t)id[4 * i + 1] << 8 | (uint32_t)id[4 * i + 2] << 16 |
                        (uint32_t)id[4 * i + 3] << 24;
}

static sid domain_sid(uint8_t extra, uint32_t rid)
{
    uint32_t sub[5] = { 21, 0, 0, 0, rid };
    if (pthread_once(&domain_once, make_domain) != 0) rt_fatal("pthread_once failed");
    sub[1] = domain_sub[0];
    sub[2] = domain_sub[1];
    sub[3] = domain_sub[2];
    return make_sid(5, (uint8_t)(4u + extra), sub);
}

static uint32_t user_rid(void) { return 1000u + (uint32_t)getuid(); }
static sid user_sid(void) { return domain_sid(1, user_rid()); }
static sid none_sid(void) { return domain_sid(1, 513); }
static sid everyone_sid(void) { return make_sid(1, 1, (const uint32_t[]){ 0 }); }
static sid local_sid(void) { return make_sid(2, 1, (const uint32_t[]){ 0 }); }
static sid admins_sid(void) { return make_sid(5, 2, (const uint32_t[]){ 32, 544 }); }
static sid users_sid(void) { return make_sid(5, 2, (const uint32_t[]){ 32, 545 }); }
static sid interactive_sid(void) { return make_sid(5, 1, (const uint32_t[]){ 4 }); }
static sid auth_users_sid(void) { return make_sid(5, 1, (const uint32_t[]){ 11 }); }
static sid system_sid(void) { return make_sid(5, 1, (const uint32_t[]){ 18 }); }

#define TOKEN_GROUP_COUNT 7

/* The groups of the token, in the order of a Windows XP token. */
static void token_groups(sid g[TOKEN_GROUP_COUNT], uint32_t attr[TOKEN_GROUP_COUNT])
{
    const uint32_t a = SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED;
    int i;
    g[0] = none_sid();
    g[1] = everyone_sid();
    g[2] = admins_sid();
    g[3] = users_sid();
    g[4] = interactive_sid();
    g[5] = auth_users_sid();
    g[6] = local_sid();
    for (i = 0; i < TOKEN_GROUP_COUNT; i++) attr[i] = a;
    attr[2] |= SE_GROUP_OWNER;
}

/* The SIDs that the ACL rules apply to for trustee t: t, Everyone and, for
   the user, the token groups. Returns the count. */
static int trustee_members(const sid *t, sid out[TOKEN_GROUP_COUNT + 2])
{
    sid u = user_sid(), e = everyone_sid();
    int n = 0;
    out[n++] = *t;
    if (sid_eq(t, &u))
    {
        uint32_t attr[TOKEN_GROUP_COUNT];
        token_groups(out + n, attr);
        n += TOKEN_GROUP_COUNT;
    }
    else if (!sid_eq(t, &e))
        out[n++] = e;
    return n;
}

/* The account name of a SID. Returns false for an unknown SID. */
static bool sid_name(const sid *s, const char **name, const char **dom, uint32_t *use)
{
    struct {
        sid s;
        const char *name, *dom;
        uint32_t use;
    } t[] = {
        { user_sid(), "Player", NULL, SidTypeUser },
        { none_sid(), "None", NULL, SidTypeGroup },
        { domain_sid(0, 0), NULL, NULL, SidTypeDomain },
        { everyone_sid(), "Everyone", "", SidTypeWellKnownGroup },
        { local_sid(), "LOCAL", "", SidTypeWellKnownGroup },
        { admins_sid(), "Administrators", "BUILTIN", SidTypeAlias },
        { users_sid(), "Users", "BUILTIN", SidTypeAlias },
        { interactive_sid(), "INTERACTIVE", "NT AUTHORITY", SidTypeWellKnownGroup },
        { auth_users_sid(), "Authenticated Users", "NT AUTHORITY", SidTypeWellKnownGroup },
        { system_sid(), "SYSTEM", "NT AUTHORITY", SidTypeWellKnownGroup },
    };
    size_t i;
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
    {
        if (!sid_eq(s, &t[i].s)) continue;
        *name = t[i].name != NULL ? t[i].name : rt_computer_name();
        *dom = t[i].dom != NULL ? t[i].dom : rt_computer_name();
        *use = t[i].use;
        return true;
    }
    return false;
}

/* BOOL AllocateAndInitializeSid(PSID_IDENTIFIER_AUTHORITY, BYTE count,
   DWORD sub0, ..., DWORD sub7, PSID *) */
void rt_AllocateAndInitializeSid(_cpu *cpu)
{
    uint32_t auth = ARG(0), count = ARG(1) & 0xFFu, out = ARG(10), g;
    sid s;
    uint32_t i;
    if (auth == 0 || out == 0 || count > 8)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    memset(&s, 0, sizeof(s));
    memcpy(s.auth, G2H(auth), 6);
    s.n = (uint8_t)count;
    for (i = 0; i < count; i++) s.sub[i] = ARG(2 + i);
    g = rt_heap_alloc(sid_len(&s));
    if (g == 0)
    {
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    sid_write(g, &s);
    wr32(out, g);
    RET(1);
}

/* PVOID FreeSid(PSID): NULL on success. */
void rt_FreeSid(_cpu *cpu)
{
    rt_heap_free(ARG(0));
    RET(0);
}

/* DWORD GetLengthSid(PSID). The result for an invalid SID is undefined on
   Windows; here it is 0. */
void rt_GetLengthSid(_cpu *cpu)
{
    sid s;
    RET(sid_read(ARG(0), &s) ? sid_len(&s) : 0u);
}

/* BOOL CopySid(DWORD len, PSID dest, PSID src) */
void rt_CopySid(_cpu *cpu)
{
    uint32_t len = ARG(0), dest = ARG(1);
    sid s;
    if (!sid_read(ARG(2), &s))
    {
        rt_set_last_error(ERROR_INVALID_SID);
        RET(0);
        return;
    }
    if (len < sid_len(&s) || dest == 0)
    {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0);
        return;
    }
    sid_write(dest, &s);
    RET(1);
}

/* The size of s as a wide string with the NUL. */
static uint32_t wide_size(const char *s)
{
    size_t units;
    if (!rt_utf16_length(s, &units) || units > 0xFFFFu) rt_fatal("LookupAccountSidW: invalid account name");
    return (uint32_t)units + 1u;
}

/* BOOL LookupAccountSidW(LPCWSTR system, PSID, LPWSTR name, LPDWORD cch_name,
   LPWSTR domain, LPDWORD cch_domain, PSID_NAME_USE use) */
void rt_LookupAccountSidW(_cpu *cpu)
{
    uint32_t system = ARG(0), name_buf = ARG(2), cch_name = ARG(3), dom_buf = ARG(4), cch_dom = ARG(5), use_ptr = ARG(6);
    const char *name, *dom;
    uint32_t use, need_name, need_dom;
    sid s;
    if (system != 0)
    {
        char sys[64];
        const char *p = sys;
        if (rt_gstr_w(sys, sizeof(sys), system) != RT_STR_OK) rt_fatal("LookupAccountSidW: system name too long");
        if (p[0] == '\\' && p[1] == '\\') p += 2;
        if (p[0] != 0 && strcasecmp(p, rt_computer_name()) != 0)
            rt_fatal("LookupAccountSidW: lookup on the remote system \"%s\" is not supported", sys);
    }
    if (!sid_read(ARG(1), &s))
    {
        rt_set_last_error(ERROR_INVALID_SID);
        RET(0);
        return;
    }
    if (cch_name == 0 || cch_dom == 0 || use_ptr == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (!sid_name(&s, &name, &dom, &use))
    {
        rt_set_last_error(ERROR_NONE_MAPPED);
        RET(0);
        return;
    }
    /* If one buffer is too small, both sizes receive the required size
       with the NUL. */
    need_name = wide_size(name);
    need_dom = wide_size(dom);
    if (name_buf == 0 || dom_buf == 0 || rd32(cch_name) < need_name || rd32(cch_dom) < need_dom)
    {
        wr32(cch_name, need_name);
        wr32(cch_dom, need_dom);
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0);
        return;
    }
    wr32(cch_name, rt_put_wstr(name_buf, rd32(cch_name), name));
    wr32(cch_dom, rt_put_wstr(dom_buf, rd32(cch_dom), dom));
    wr32(use_ptr, use);
    RET(1);
}

/* ---------------------------------------------------------------- tokens */

typedef struct token {
    uint32_t type;   /* TOKEN_PRIMARY or TOKEN_IMPERSONATION */
    uint32_t level;  /* impersonation level (impersonation tokens) */
    uint32_t access; /* granted access of the handle */
} token;

static void token_destroy(rt_object *o) { free(o->data); }

static rt_object *token_new(uint32_t type, uint32_t level, uint32_t access)
{
    rt_object *o = rt_object_new(OBJ_TOKEN);
    token *t = calloc(1, sizeof(*t));
    if (t == NULL) rt_fatal("out of host memory (token)");
    t->type = type;
    t->level = level;
    t->access = access;
    o->data = t;
    o->destroy = token_destroy;
    return o;
}

/* Map generic token rights. The user has all rights on its tokens. */
static uint32_t token_access(uint32_t a)
{
    if (a & MAXIMUM_ALLOWED) return TOKEN_ALL_ACCESS;
    if (a & GENERIC_ALL) a |= TOKEN_ALL_ACCESS;
    if (a & GENERIC_READ) a |= TOKEN_READ;
    if (a & GENERIC_WRITE) a |= TOKEN_WRITE;
    if (a & GENERIC_EXECUTE) a |= TOKEN_EXECUTE;
    return a & TOKEN_ALL_ACCESS;
}

/* Make a handle for o, store it at out and return TRUE; or fail. Takes
   the reference of o. */
static void return_token(_cpu *cpu, rt_object *o, uint32_t out)
{
    uint32_t h = rt_handle_new(o);
    rt_object_unref(o);
    if (h == 0)
    {
        rt_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
        RET(0);
        return;
    }
    wr32(out, h);
    RET(1);
}

/* BOOL OpenProcessToken(HANDLE process, DWORD access, PHANDLE) */
void rt_OpenProcessToken(_cpu *cpu)
{
    uint32_t process = ARG(0), out = ARG(2);
    if (out == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (process != RT_PSEUDO_PROCESS)
    {
        rt_object *p = rt_handle_get(process, OBJ_PROCESS);
        rt_object *self = rt_process_object();
        bool same = p == self;
        if (p != NULL) rt_object_unref(p);
        rt_object_unref(self);
        if (!same)
        {
            rt_set_last_error(ERROR_INVALID_HANDLE);
            RET(0);
            return;
        }
    }
    return_token(cpu, token_new(TOKEN_PRIMARY, 0, token_access(ARG(1))), out);
}

/* The guest state of thread handle h with the object lock held, or NULL
   (the lock is then not held). *o receives the reference of the thread
   object to release after the lock. */
static rt_thread *lock_thread(uint32_t h, rt_object **o)
{
    rt_thread *t;
    *o = NULL;
    if (h == RT_PSEUDO_THREAD)
    {
        rt_obj_lock();
        return rt_self();
    }
    *o = rt_handle_get(h, OBJ_THREAD);
    if (*o == NULL) return NULL;
    rt_obj_lock();
    t = rt_thread_state_locked(*o);
    if (t == NULL)
    {
        rt_obj_unlock();
        rt_object_unref(*o);
        *o = NULL;
    }
    return t;
}

/* BOOL OpenThreadToken(HANDLE thread, DWORD access, BOOL open_as_self,
   PHANDLE) */
void rt_OpenThreadToken(_cpu *cpu)
{
    uint32_t out = ARG(3), level;
    rt_object *to, *tok;
    rt_thread *t;
    if (out == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    t = lock_thread(ARG(0), &to);
    if (t == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    tok = t->token;
    level = tok != NULL ? ((token *)tok->data)->level : 0;
    rt_obj_unlock();
    if (to != NULL) rt_object_unref(to);
    if (tok == NULL)
    {
        rt_set_last_error(ERROR_NO_TOKEN);
        RET(0);
        return;
    }
    if (level == SECURITY_ANONYMOUS)
    {
        rt_set_last_error(ERROR_CANT_OPEN_ANONYMOUS);
        RET(0);
        return;
    }
    return_token(cpu, token_new(TOKEN_IMPERSONATION, level, token_access(ARG(1))), out);
}

/* The token of handle h, with a reference, if the handle has all rights
   of need. Sets the last error and returns NULL otherwise. */
static rt_object *get_token(uint32_t h, uint32_t need)
{
    rt_object *o = rt_handle_get(h, OBJ_TOKEN);
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        return NULL;
    }
    if ((((token *)o->data)->access & need) != need)
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_ACCESS_DENIED);
        return NULL;
    }
    return o;
}

/* BOOL DuplicateToken(HANDLE existing, SECURITY_IMPERSONATION_LEVEL,
   PHANDLE). The new token is an impersonation token with TOKEN_IMPERSONATE
   and TOKEN_QUERY access. */
void rt_DuplicateToken(_cpu *cpu)
{
    uint32_t level = ARG(1), out = ARG(2);
    rt_object *o;
    if (out == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (level > SECURITY_DELEGATION)
    {
        rt_set_last_error(ERROR_BAD_IMPERSONATION_LEVEL);
        RET(0);
        return;
    }
    o = get_token(ARG(0), TOKEN_DUPLICATE);
    if (o == NULL)
    {
        RET(0);
        return;
    }
    {
        const token *t = o->data;
        bool raise = t->type == TOKEN_IMPERSONATION && level > t->level;
        rt_object_unref(o);
        if (raise)
        {
            rt_set_last_error(ERROR_BAD_IMPERSONATION_LEVEL);
            RET(0);
            return;
        }
    }
    return_token(cpu, token_new(TOKEN_IMPERSONATION, level, TOKEN_IMPERSONATE | TOKEN_QUERY), out);
}

/* Set the impersonation token of the calling thread (NULL: none). Takes
   the reference of tok. */
static void set_thread_token(rt_object *tok)
{
    rt_thread *t = rt_self();
    rt_object *old;
    rt_obj_lock();
    old = t->token;
    t->token = tok;
    rt_obj_unlock();
    if (old != NULL) rt_object_unref(old);
}

/* BOOL ImpersonateLoggedOnUser(HANDLE token) */
void rt_ImpersonateLoggedOnUser(_cpu *cpu)
{
    rt_object *o = rt_handle_get(ARG(0), OBJ_TOKEN);
    const token *t;
    uint32_t need, level;
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    t = o->data;
    need = TOKEN_QUERY | (t->type == TOKEN_PRIMARY ? TOKEN_DUPLICATE : TOKEN_IMPERSONATE);
    level = t->type == TOKEN_PRIMARY ? SECURITY_IMPERSONATION : t->level;
    if ((t->access & need) != need)
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_ACCESS_DENIED);
        RET(0);
        return;
    }
    rt_object_unref(o);
    set_thread_token(token_new(TOKEN_IMPERSONATION, level, TOKEN_ALL_ACCESS));
    RET(1);
}

/* BOOL RevertToSelf(void) */
void rt_RevertToSelf(_cpu *cpu)
{
    set_thread_token(NULL);
    RET(1);
}

/* BOOL GetTokenInformation(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID buf,
   DWORD len, PDWORD ret_len). Pointers in the result point into buf. */
void rt_GetTokenInformation(_cpu *cpu)
{
    uint32_t cls = ARG(1), buf = ARG(2), len = ARG(3), ret_len = ARG(4), need, i;
    rt_object *o;
    token t;
    sid one, groups[TOKEN_GROUP_COUNT];
    uint32_t attr[TOKEN_GROUP_COUNT];

    if (ret_len == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    o = get_token(ARG(0), TOKEN_QUERY);
    if (o == NULL)
    {
        RET(0);
        return;
    }
    t = *(const token *)o->data;
    rt_object_unref(o);

    switch (cls)
    {
    case TokenUser:
    case TokenOwner:
    case TokenPrimaryGroup:
        one = cls == TokenPrimaryGroup ? none_sid() : user_sid();
        need = (cls == TokenUser ? 8u : 4u) + sid_len(&one);
        break;
    case TokenGroups:
        token_groups(groups, attr);
        need = 4u + 8u * TOKEN_GROUP_COUNT;
        for (i = 0; i < TOKEN_GROUP_COUNT; i++) need += sid_len(&groups[i]);
        break;
    case TokenType:
        need = 4;
        break;
    case TokenImpersonationLevel:
        if (t.type != TOKEN_IMPERSONATION)
        {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
        need = 4;
        break;
    default:
        if (cls == 0 || cls > TOKEN_CLASS_MAX_XP)
        {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
        rt_fatal("GetTokenInformation: information class %u is not supported", cls);
    }
    wr32(ret_len, need);
    if (buf == 0 || len < need)
    {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0);
        return;
    }
    switch (cls)
    {
    case TokenUser:
        /* TOKEN_USER: SID_AND_ATTRIBUTES { PSID, DWORD attributes }. */
        wr32(buf, buf + 8);
        wr32(buf + 4, 0);
        sid_write(buf + 8, &one);
        break;
    case TokenOwner:
    case TokenPrimaryGroup:
        wr32(buf, buf + 4);
        sid_write(buf + 4, &one);
        break;
    case TokenGroups:
    {
        uint32_t at = buf + 4u + 8u * TOKEN_GROUP_COUNT;
        wr32(buf, TOKEN_GROUP_COUNT);
        for (i = 0; i < TOKEN_GROUP_COUNT; i++)
        {
            wr32(buf + 4 + 8 * i, at);
            wr32(buf + 8 + 8 * i, attr[i]);
            sid_write(at, &groups[i]);
            at += sid_len(&groups[i]);
        }
        break;
    }
    case TokenType:
        wr32(buf, t.type);
        break;
    default: /* TokenImpersonationLevel */
        wr32(buf, t.level);
        break;
    }
    RET(1);
}

/* ---------------------------------------------------------------- ACLs */

typedef struct ace {
    uint8_t type, flags;
    uint32_t mask;
    sid s;
} ace;

typedef struct ace_list {
    ace *a;
    uint32_t n, cap;
} ace_list;

static void list_add(ace_list *l, const ace *a)
{
    if (l->n == l->cap)
    {
        uint32_t cap = l->cap == 0 ? 16u : l->cap * 2u;
        ace *na;
        if (cap > 0x10000u) rt_fatal("ACL with more than 65536 entries");
        na = realloc(l->a, cap * sizeof(*na));
        if (na == NULL) rt_fatal("out of host memory (ACL)");
        l->a = na;
        l->cap = cap;
    }
    l->a[l->n++] = *a;
}

/* Read the guest ACL at g into l. Returns 0 or a Win32 error code. */
static uint32_t acl_read(uint32_t g, ace_list *l)
{
    const uint8_t *p = G2H(g);
    uint32_t size = rd16(g + 2), count = rd16(g + 4), off = 8, i;
    if ((p[0] != ACL_REVISION && p[0] != ACL_REVISION_DS) || size < 8) return ERROR_INVALID_ACL;
    for (i = 0; i < count; i++)
    {
        ace a;
        uint32_t asize;
        if (off + 8 > size) return ERROR_INVALID_ACL;
        a.type = p[off];
        a.flags = p[off + 1];
        asize = rd16(g + off + 2);
        if (asize < 8 || off + asize > size) return ERROR_INVALID_ACL;
        if (a.type == ACCESS_ALLOWED_ACE_TYPE || a.type == ACCESS_DENIED_ACE_TYPE)
        {
            a.mask = rd32(g + off + 4);
            if (!sid_read(g + off + 8, &a.s) || 8 + sid_len(&a.s) > asize) return ERROR_INVALID_ACL;
            list_add(l, &a);
        }
        else if (a.type != SYSTEM_AUDIT_ACE_TYPE)
            rt_fatal("ACL: ACE type %u is not supported", a.type);
        off += asize;
    }
    return 0;
}

/* Write l as a new guest ACL from the runtime heap (LocalFree releases
   it). Returns 0 or a Win32 error code. */
static uint32_t acl_write(const ace_list *l, uint32_t *out)
{
    uint32_t size = 8, i, g, off = 8;
    uint8_t *p;
    for (i = 0; i < l->n; i++)
    {
        size += 8 + sid_len(&l->a[i].s);
        if (size > ACL_MAX_SIZE) return ERROR_ALLOTTED_SPACE_EXCEEDED;
    }
    g = rt_heap_alloc(size);
    if (g == 0) return ERROR_NOT_ENOUGH_MEMORY;
    p = G2H(g);
    p[0] = ACL_REVISION;
    p[1] = 0;
    wr16(g + 2, (uint16_t)size);
    wr16(g + 4, (uint16_t)l->n);
    wr16(g + 6, 0);
    for (i = 0; i < l->n; i++)
    {
        const ace *a = &l->a[i];
        uint32_t asize = 8 + sid_len(&a->s);
        p[off] = a->type;
        p[off + 1] = a->flags;
        wr16(g + off + 2, (uint16_t)asize);
        wr32(g + off + 4, a->mask);
        sid_write(g + off + 8, &a->s);
        off += asize;
    }
    *out = g;
    return 0;
}

static uint32_t map_file_generic(uint32_t m)
{
    if (m & GENERIC_READ) m |= FILE_GENERIC_READ;
    if (m & GENERIC_WRITE) m |= FILE_GENERIC_WRITE;
    if (m & GENERIC_EXECUTE) m |= FILE_GENERIC_EXECUTE;
    if (m & GENERIC_ALL) m |= FILE_ALL_ACCESS;
    return m & ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL);
}

/* The rights that l gives to trustee t, in ACE order: a deny ACE removes
   the rights that no earlier ACE allowed. Inherit-only ACEs do not apply.
   file_mapping maps the generic rights to file rights. */
static uint32_t effective_rights(const ace_list *l, const sid *t, bool file_mapping)
{
    sid m[TOKEN_GROUP_COUNT + 2];
    int nm = trustee_members(t, m), j;
    uint32_t allowed = 0, denied = 0, i;
    for (i = 0; i < l->n; i++)
    {
        const ace *a = &l->a[i];
        uint32_t mask;
        bool match = false;
        if (a->flags & INHERIT_ONLY_ACE) continue;
        for (j = 0; j < nm && !match; j++) match = sid_eq(&a->s, &m[j]);
        if (!match) continue;
        mask = file_mapping ? map_file_generic(a->mask) : a->mask;
        if (a->type == ACCESS_ALLOWED_ACE_TYPE)
            allowed |= mask & ~denied;
        else
            denied |= mask & ~allowed;
    }
    return allowed;
}

/* Read a TRUSTEE_W with a SID. Returns false for an invalid SID. */
static bool trustee_sid(uint32_t tr, sid *s)
{
    uint32_t multi = rd32(tr), op = rd32(tr + 4), form = rd32(tr + 8);
    if (multi != 0 || op != NO_MULTIPLE_TRUSTEE) rt_fatal("ACL: multiple trustees are not supported");
    if (form != TRUSTEE_IS_SID) rt_fatal("ACL: trustee form %u is not supported (only TRUSTEE_IS_SID)", form);
    return sid_read(rd32(tr + 16), s);
}

/* VOID BuildTrusteeWithSidW(PTRUSTEE_W, PSID) */
void rt_BuildTrusteeWithSidW(_cpu *cpu)
{
    uint32_t tr = ARG(0);
    if (tr == 0) return;
    wr32(tr, 0);
    wr32(tr + 4, NO_MULTIPLE_TRUSTEE);
    wr32(tr + 8, TRUSTEE_IS_SID);
    wr32(tr + 12, TRUSTEE_IS_UNKNOWN);
    wr32(tr + 16, ARG(1));
}

/* DWORD GetEffectiveRightsFromAclW(PACL, PTRUSTEE_W, PACCESS_MASK). The
   result is not mapped: generic rights stay generic. */
void rt_GetEffectiveRightsFromAclW(_cpu *cpu)
{
    uint32_t acl = ARG(0), tr = ARG(1), out = ARG(2), e;
    ace_list l = { NULL, 0, 0 };
    sid s;
    if (acl == 0 || tr == 0 || out == 0)
    {
        RET(ERROR_INVALID_PARAMETER);
        return;
    }
    if (!trustee_sid(tr, &s))
    {
        RET(ERROR_INVALID_SID);
        return;
    }
    e = acl_read(acl, &l);
    if (e == 0) wr32(out, effective_rights(&l, &s, false));
    free(l.a);
    RET(e);
}

/* Remove the explicit (not inherited) allow and deny ACEs of s from l. */
static void remove_explicit(ace_list *l, const sid *s)
{
    uint32_t i, k = 0;
    for (i = 0; i < l->n; i++)
        if ((l->a[i].flags & INHERITED_ACE) || !sid_eq(&l->a[i].s, s)) l->a[k++] = l->a[i];
    l->n = k;
}

/* Add an explicit ACE, or merge the rights into an explicit ACE of the
   same type, SID and flags. */
static void merge_explicit(ace_list *l, const ace *a)
{
    uint32_t i;
    for (i = 0; i < l->n; i++)
    {
        ace *b = &l->a[i];
        if (b->type == a->type && b->flags == a->flags && sid_eq(&b->s, &a->s))
        {
            b->mask |= a->mask;
            return;
        }
    }
    list_add(l, a);
}

/* DWORD SetEntriesInAclW(ULONG count, PEXPLICIT_ACCESS_W, PACL old,
   PACL *new). The new ACL is in canonical order: explicit deny ACEs,
   explicit allow ACEs, then the inherited ACEs of the old ACL. */
void rt_SetEntriesInAclW(_cpu *cpu)
{
    uint32_t count = ARG(0), ea = ARG(1), old = ARG(2), out = ARG(3), i, e = 0, result = 0;
    ace_list cur = { NULL, 0, 0 }, sorted = { NULL, 0, 0 };
    if (out == 0 || (count != 0 && ea == 0) || count > MAX_EXPLICIT_ENTRIES)
    {
        RET(ERROR_INVALID_PARAMETER);
        return;
    }
    if (count == 0 && old == 0)
    {
        wr32(out, 0);
        RET(ERROR_SUCCESS);
        return;
    }
    if (old != 0) e = acl_read(old, &cur);
    for (i = 0; i < count && e == 0; i++)
    {
        uint32_t x = ea + i * EXPLICIT_ACCESS_SIZE, mode = rd32(x + 4), inherit = rd32(x + 8);
        ace a;
        if ((inherit & ~VALID_INHERIT_FLAGS) != 0)
        {
            e = ERROR_INVALID_PARAMETER;
            break;
        }
        if (mode == NOT_USED_ACCESS) continue;
        if (mode == SET_AUDIT_SUCCESS || mode == SET_AUDIT_FAILURE)
            rt_fatal("SetEntriesInAclW: audit entries are not supported");
        if (mode > REVOKE_ACCESS)
        {
            e = ERROR_INVALID_PARAMETER;
            break;
        }
        if (!trustee_sid(x + 12, &a.s))
        {
            e = ERROR_INVALID_SID;
            break;
        }
        a.mask = rd32(x);
        a.flags = (uint8_t)(inherit & ~INHERITED_ACE);
        a.type = mode == DENY_ACCESS ? ACCESS_DENIED_ACE_TYPE : ACCESS_ALLOWED_ACE_TYPE;
        if (mode == SET_ACCESS || mode == REVOKE_ACCESS) remove_explicit(&cur, &a.s);
        if (mode != REVOKE_ACCESS && a.mask != 0) merge_explicit(&cur, &a);
    }
    if (e == 0)
    {
        for (i = 0; i < cur.n; i++)
            if (!(cur.a[i].flags & INHERITED_ACE) && cur.a[i].type == ACCESS_DENIED_ACE_TYPE) list_add(&sorted, &cur.a[i]);
        for (i = 0; i < cur.n; i++)
            if (!(cur.a[i].flags & INHERITED_ACE) && cur.a[i].type == ACCESS_ALLOWED_ACE_TYPE) list_add(&sorted, &cur.a[i]);
        for (i = 0; i < cur.n; i++)
            if (cur.a[i].flags & INHERITED_ACE) list_add(&sorted, &cur.a[i]);
        e = acl_write(&sorted, &result);
    }
    free(cur.a);
    free(sorted.a);
    if (e == 0) wr32(out, result);
    RET(e);
}

/* ---------------------------------------------------------------- file security */

/* Resolve the wide guest path at g and stat it. Returns 0 or a Win32
   error code. */
static uint32_t stat_guest(uint32_t g, char host[RT_HOST_PATH], struct stat *st)
{
    char in[RT_HOST_PATH], full[RT_HOST_PATH];
    bool exists;
    uint32_t e;
    if (g == 0) return ERROR_INVALID_PARAMETER;
    if (rt_gstr_w(in, sizeof(in), g) != RT_STR_OK) return 206u; /* ERROR_FILENAME_EXCED_RANGE */
    e = rt_path_full(in, full, sizeof(full));
    if (e == 0) e = rt_path_host(full, host, RT_HOST_PATH, &exists);
    if (e != 0) return e;
    if (!exists) return ERROR_FILE_NOT_FOUND;
    if (stat(host, st) != 0) return rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    return 0;
}

static sid owner_of(const struct stat *st)
{
    if (st->st_uid == getuid()) return user_sid();
    if (st->st_uid == 0) return admins_sid();
    return domain_sid(1, 1000u + (uint32_t)st->st_uid);
}

static uint32_t rights_of_bits(unsigned rwx)
{
    uint32_t m = 0;
    if (rwx == 7) return FILE_ALL_ACCESS;
    if (rwx & 4) m |= FILE_GENERIC_READ;
    if (rwx & 2) m |= FILE_GENERIC_WRITE;
    if (rwx & 1) m |= FILE_GENERIC_EXECUTE;
    return m;
}

static unsigned bits_of_rights(uint32_t m)
{
    return ((m & FILE_READ_DATA) ? 4u : 0u) | ((m & FILE_WRITE_DATA) ? 2u : 0u) | ((m & FILE_EXECUTE) ? 1u : 0u);
}

/* DWORD GetNamedSecurityInfoW(LPWSTR name, SE_OBJECT_TYPE, SECURITY_INFORMATION,
   PSID *owner, PSID *group, PACL *dacl, PACL *sacl, PSECURITY_DESCRIPTOR *sd).
   The security descriptor is self-relative and comes from the runtime heap
   (LocalFree releases it). The other results point into it. */
void rt_GetNamedSecurityInfoW(_cpu *cpu)
{
    uint32_t type = ARG(1), si = ARG(2), p_owner = ARG(3), p_group = ARG(4), p_dacl = ARG(5), p_sacl = ARG(6),
             p_sd = ARG(7), e, size, g, at, dacl_g = 0;
    char host[RT_HOST_PATH];
    struct stat st;
    sid owner, group;
    ace_list l = { NULL, 0, 0 };

    if (type != SE_FILE_OBJECT) rt_fatal("GetNamedSecurityInfoW: object type %u is not supported", type);
    if (si & SACL_SECURITY_INFORMATION)
    {
        RET(ERROR_PRIVILEGE_NOT_HELD);
        return;
    }
    if (p_sd == 0 && (p_owner | p_group | p_dacl | p_sacl) != 0)
    {
        RET(ERROR_INVALID_PARAMETER);
        return;
    }
    e = stat_guest(ARG(0), host, &st);
    if (e != 0)
    {
        RET(e);
        return;
    }
    owner = owner_of(&st);
    group = none_sid();
    if (si & DACL_SECURITY_INFORMATION)
    {
        ace a;
        unsigned bits[3] = { (st.st_mode >> 6) & 7u, (st.st_mode >> 3) & 7u, st.st_mode & 7u };
        sid who[3] = { owner, group, everyone_sid() };
        int i;
        for (i = 0; i < 3; i++)
        {
            if (bits[i] == 0) continue;
            a.type = ACCESS_ALLOWED_ACE_TYPE;
            a.flags = 0;
            a.mask = rights_of_bits(bits[i]);
            a.s = who[i];
            list_add(&l, &a);
        }
        e = acl_write(&l, &dacl_g);
        free(l.a);
        if (e != 0)
        {
            RET(e);
            return;
        }
    }
    if (p_sd == 0)
    {
        rt_heap_free(dacl_g);
        RET(ERROR_SUCCESS);
        return;
    }
    size = 20u;
    if (si & OWNER_SECURITY_INFORMATION) size += sid_len(&owner);
    if (si & GROUP_SECURITY_INFORMATION) size += sid_len(&group);
    if (dacl_g != 0) size += rd16(dacl_g + 2);
    g = rt_heap_calloc(1, size);
    if (g == 0)
    {
        rt_heap_free(dacl_g);
        RET(ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    /* SECURITY_DESCRIPTOR_RELATIVE: Revision, Sbz1, Control, Owner, Group,
       Sacl, Dacl (offsets). */
    *(uint8_t *)G2H(g) = 1;
    wr16(g + 2, (uint16_t)(SE_SELF_RELATIVE | (dacl_g != 0 ? SE_DACL_PRESENT : 0u)));
    at = 20u;
    if (p_owner != 0) wr32(p_owner, 0);
    if (p_group != 0) wr32(p_group, 0);
    if (p_dacl != 0) wr32(p_dacl, 0);
    if (p_sacl != 0) wr32(p_sacl, 0);
    if (si & OWNER_SECURITY_INFORMATION)
    {
        wr32(g + 4, at);
        sid_write(g + at, &owner);
        if (p_owner != 0) wr32(p_owner, g + at);
        at += sid_len(&owner);
    }
    if (si & GROUP_SECURITY_INFORMATION)
    {
        wr32(g + 8, at);
        sid_write(g + at, &group);
        if (p_group != 0) wr32(p_group, g + at);
        at += sid_len(&group);
    }
    if (dacl_g != 0)
    {
        uint32_t dsize = rd16(dacl_g + 2);
        wr32(g + 16, at);
        memcpy(G2H(g + at), G2H(dacl_g), dsize);
        if (p_dacl != 0) wr32(p_dacl, g + at);
        rt_heap_free(dacl_g);
    }
    wr32(p_sd, g);
    RET(ERROR_SUCCESS);
}

/* DWORD SetNamedSecurityInfoW(LPWSTR name, SE_OBJECT_TYPE, SECURITY_INFORMATION,
   PSID owner, PSID group, PACL dacl, PACL sacl) */
void rt_SetNamedSecurityInfoW(_cpu *cpu)
{
    uint32_t type = ARG(1), si = ARG(2), owner_g = ARG(3), group_g = ARG(4), dacl = ARG(5), e;
    const uint32_t flags = UNPROTECTED_SACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION |
                           PROTECTED_SACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;
    char host[RT_HOST_PATH];
    struct stat st;
    sid s, cur;

    if (type != SE_FILE_OBJECT) rt_fatal("SetNamedSecurityInfoW: object type %u is not supported", type);
    if (si & ~(OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
               SACL_SECURITY_INFORMATION | flags))
    {
        RET(ERROR_INVALID_PARAMETER);
        return;
    }
    if (si & SACL_SECURITY_INFORMATION)
    {
        RET(ERROR_PRIVILEGE_NOT_HELD);
        return;
    }
    e = stat_guest(ARG(0), host, &st);
    if (e != 0)
    {
        RET(e);
        return;
    }
    cur = owner_of(&st);
    if (si & OWNER_SECURITY_INFORMATION)
    {
        if (!sid_read(owner_g, &s))
        {
            RET(ERROR_INVALID_SID);
            return;
        }
        if (!sid_eq(&s, &cur))
        {
            RET(ERROR_INVALID_OWNER);
            return;
        }
    }
    if (si & GROUP_SECURITY_INFORMATION)
    {
        sid none = none_sid();
        if (!sid_read(group_g, &s))
        {
            RET(ERROR_INVALID_SID);
            return;
        }
        if (!sid_eq(&s, &none))
        {
            RET(ERROR_INVALID_PRIMARY_GROUP);
            return;
        }
    }
    if (si & DACL_SECURITY_INFORMATION)
    {
        mode_t mode;
        if (dacl == 0)
            mode = 0777; /* a NULL DACL gives all access to everyone */
        else
        {
            ace_list l = { NULL, 0, 0 };
            sid none = none_sid(), every = everyone_sid();
            e = acl_read(dacl, &l);
            if (e != 0)
            {
                free(l.a);
                RET(e);
                return;
            }
            mode = (mode_t)(bits_of_rights(effective_rights(&l, &cur, true)) << 6 |
                            bits_of_rights(effective_rights(&l, &none, true)) << 3 |
                            bits_of_rights(effective_rights(&l, &every, true)));
            free(l.a);
        }
        mode |= st.st_mode & 07000;
        if (chmod(host, mode) != 0)
        {
            RET(rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND));
            return;
        }
    }
    RET(ERROR_SUCCESS);
}

/* BOOL GetUserNameA(LPSTR buf, LPDWORD size): the user "Player". *size is
   in characters with the NUL. */
void rt_GetUserNameA(_cpu *cpu)
{
    static const char name[] = "Player";
    uint32_t buf = ARG(0), psize = ARG(1);
    if (psize == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (buf == 0 || rd32(psize) < sizeof name)
    {
        wr32(psize, sizeof name);
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0);
        return;
    }
    memcpy(G2H(buf), name, sizeof name);
    wr32(psize, sizeof name);
    RET(1);
}
