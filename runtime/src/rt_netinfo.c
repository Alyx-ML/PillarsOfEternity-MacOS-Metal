/*
 * Network information: DNSAPI DnsQuery_A and DnsFree, IPHLPAPI
 * GetIpAddrTable and WINHTTP WinHttpGetIEProxyConfigForCurrentUser.
 *
 * DnsQuery_A sends the query to the DNS servers of the host (libresolv,
 * res_nquery). The runtime supports the query type DNS_TYPE_A with the
 * options DNS_QUERY_STANDARD, DNS_QUERY_BYPASS_CACHE and
 * DNS_QUERY_NO_HOSTS_FILE. The result has the CNAME and A records of the
 * answer section, in the order of the reply. The runtime has no DNS cache
 * and does not read a hosts file, so the three options give the same
 * result. Other query types and options stop the process with a message.
 *
 * GetIpAddrTable gives the IPv4 addresses of the host interfaces
 * (getifaddrs), with the interface index of the host.
 *
 * WinHttpGetIEProxyConfigForCurrentUser gives the proxy settings of the
 * host (CFNetworkCopySystemProxySettings) in the Internet Explorer form.
 */
#include "rt.h"
#include "rt_text.h"

#include <CFNetwork/CFNetwork.h>
#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_NO_DATA 232u
#define ERROR_TIMEOUT 1460u
#define DNS_ERROR_RCODE_FORMAT_ERROR 9001u
#define DNS_ERROR_RCODE_SERVER_FAILURE 9002u
#define DNS_ERROR_RCODE_NAME_ERROR 9003u
#define DNS_ERROR_RCODE_NOT_IMPLEMENTED 9004u
#define DNS_ERROR_RCODE_REFUSED 9005u
#define DNS_INFO_NO_RECORDS 9501u
#define DNS_ERROR_INVALID_NAME 123u /* ERROR_INVALID_NAME */

#define DNS_TYPE_A 1u
#define DNS_TYPE_CNAME 5u
#define DNS_QUERY_BYPASS_CACHE 0x8u
#define DNS_QUERY_NO_HOSTS_FILE 0x40u

#define DNS_FREE_FLAT 0u
#define DNS_FREE_RECORD_LIST 1u

/* DNS_RECORD flags: the answer section (1) and DnsCharSetAnsi (3) in
   bits 3 and 4 (UNCONFIRMED: from the layout of DNS_RECORD_FLAGS in
   windns.h). */
#define DNS_RECORD_FLAGS_ANSWER_ANSI (1u | (3u << 3))

/* ---------------------------------------------------------------- DnsQuery_A */

/* One guest DNS_RECORDA (24 bytes of header and the data) with the owner
   name after the data. Returns 0 if no guest memory is left. */
static uint32_t dns_record(const char *owner, uint32_t type, uint32_t ttl, const void *data, uint32_t dlen,
                           const char *target)
{
    size_t on = strlen(owner) + 1u, tn = target != NULL ? strlen(target) + 1u : 0u;
    uint32_t blk, name_at, target_at;
    if (on > NS_MAXDNAME + 1u || tn > NS_MAXDNAME + 1u) rt_fatal("DnsQuery_A: a name is too long");
    blk = rt_heap_alloc(24u + 4u + (uint32_t)on + (uint32_t)tn);
    if (blk == 0) return 0;
    memset(G2H(blk), 0, 28);
    name_at = blk + 28u;
    target_at = name_at + (uint32_t)on;
    memcpy(G2H(name_at), owner, on);
    wr32(blk + 4, name_at);
    wr16(blk + 8, (uint16_t)type);
    wr16(blk + 10, (uint16_t)dlen);
    wr32(blk + 12, DNS_RECORD_FLAGS_ANSWER_ANSI);
    wr32(blk + 16, ttl);
    if (target != NULL)
    {
        memcpy(G2H(target_at), target, tn);
        wr32(blk + 24, target_at);
    }
    else
        memcpy(G2H(blk + 24), data, dlen);
    return blk;
}

static void dns_free_list(uint32_t r)
{
    while (r != 0)
    {
        uint32_t next = rd32(r);
        rt_heap_free(r);
        r = next;
    }
}

/* Convert UTF-8 text from the resolver to Windows-1252 for the ANSI
   record. DNS names are ASCII; a name with other bytes stops the process. */
static void ansi_name(const char *in, char *out, size_t n)
{
    size_t i;
    for (i = 0; in[i] != 0; i++)
    {
        if ((unsigned char)in[i] >= 0x80u) rt_fatal("DnsQuery_A: the reply has a name that is not ASCII");
        if (i + 1 >= n) rt_fatal("DnsQuery_A: a name is too long");
        out[i] = in[i];
    }
    out[i] = 0;
}

/* DNS_STATUS DnsQuery_A(PCSTR name, WORD type, DWORD options, PVOID extra,
   PDNS_RECORD *results, PVOID *reserved) */
void rt_DnsQuery_A(_cpu *cpu)
{
    uint32_t gname = ARG(0), type = ARG(1) & 0xFFFFu, options = ARG(2), extra = ARG(3), pres = ARG(4);
    char name[NS_MAXDNAME + 1];
    unsigned char *answer;
    struct __res_state rs;
    ns_msg msg;
    int len, i, count;
    uint32_t first = 0, last = 0;

    if (pres == 0 || gname == 0)
    {
        RET(ERROR_INVALID_PARAMETER);
        return;
    }
    wr32(pres, 0);
    if (extra != 0) rt_fatal("DnsQuery_A with DNS server addresses is not supported");
    if (type != DNS_TYPE_A) rt_fatal("DnsQuery_A of record type %u is not supported", type);
    if ((options & ~(DNS_QUERY_BYPASS_CACHE | DNS_QUERY_NO_HOSTS_FILE)) != 0)
        rt_fatal("DnsQuery_A with options 0x%x is not supported", options);
    if (rt_gstr_a(name, sizeof name, gname) != RT_STR_OK || name[0] == 0)
    {
        RET(DNS_ERROR_INVALID_NAME);
        return;
    }
    memset(&rs, 0, sizeof rs);
    if (res_ninit(&rs) != 0) rt_fatal("DnsQuery_A: res_ninit failed");
    answer = malloc(NS_MAXMSG);
    if (answer == NULL) rt_fatal("out of host memory");
    len = res_nquery(&rs, name, ns_c_in, ns_t_a, answer, NS_MAXMSG);
    if (len < 0)
    {
        uint32_t e;
        switch (rs.res_h_errno)
        {
        case HOST_NOT_FOUND: e = DNS_ERROR_RCODE_NAME_ERROR; break;
        case NO_DATA: e = DNS_INFO_NO_RECORDS; break;
        case TRY_AGAIN: e = ERROR_TIMEOUT; break;
        default: e = DNS_ERROR_RCODE_SERVER_FAILURE; break;
        }
        res_ndestroy(&rs);
        free(answer);
        RET(e);
        return;
    }
    if (ns_initparse(answer, len, &msg) != 0)
    {
        res_ndestroy(&rs);
        free(answer);
        RET(DNS_ERROR_RCODE_FORMAT_ERROR);
        return;
    }
    count = ns_msg_count(msg, ns_s_an);
    for (i = 0; i < count; i++)
    {
        ns_rr rr;
        char owner[NS_MAXDNAME + 1], target[NS_MAXDNAME + 1], expanded[NS_MAXDNAME + 1];
        uint32_t rec = 0, rtype;
        if (ns_parserr(&msg, ns_s_an, i, &rr) != 0)
        {
            dns_free_list(first);
            res_ndestroy(&rs);
            free(answer);
            RET(DNS_ERROR_RCODE_FORMAT_ERROR);
            return;
        }
        rtype = ns_rr_type(rr);
        if (ns_rr_class(rr) != ns_c_in || (rtype != DNS_TYPE_A && rtype != DNS_TYPE_CNAME)) continue;
        ansi_name(ns_rr_name(rr), owner, sizeof owner);
        if (rtype == DNS_TYPE_A)
        {
            if (ns_rr_rdlen(rr) != 4)
            {
                dns_free_list(first);
                res_ndestroy(&rs);
                free(answer);
                RET(DNS_ERROR_RCODE_FORMAT_ERROR);
                return;
            }
            rec = dns_record(owner, DNS_TYPE_A, ns_rr_ttl(rr), ns_rr_rdata(rr), 4, NULL);
        }
        else
        {
            if (dn_expand(ns_msg_base(msg), ns_msg_end(msg), ns_rr_rdata(rr), expanded, sizeof expanded) < 0)
            {
                dns_free_list(first);
                res_ndestroy(&rs);
                free(answer);
                RET(DNS_ERROR_RCODE_FORMAT_ERROR);
                return;
            }
            ansi_name(expanded, target, sizeof target);
            rec = dns_record(owner, DNS_TYPE_CNAME, ns_rr_ttl(rr), NULL, 4, target);
        }
        if (rec == 0)
        {
            dns_free_list(first);
            res_ndestroy(&rs);
            free(answer);
            RET(ERROR_NOT_ENOUGH_MEMORY);
            return;
        }
        if (last != 0)
            wr32(last, rec);
        else
            first = rec;
        last = rec;
    }
    res_ndestroy(&rs);
    free(answer);
    if (first == 0)
    {
        RET(DNS_INFO_NO_RECORDS);
        return;
    }
    wr32(pres, first);
    RET(0);
}

/* void DnsFree(PVOID data, DNS_FREE_TYPE type) */
void rt_DnsFree(_cpu *cpu)
{
    uint32_t p = ARG(0), type = ARG(1);
    if (p == 0) return;
    if (type == DNS_FREE_FLAT)
        rt_heap_free(p);
    else if (type == DNS_FREE_RECORD_LIST)
        dns_free_list(p);
    else
        rt_fatal("DnsFree with free type %u is not supported", type);
}

/* ---------------------------------------------------------------- GetIpAddrTable */

#define MAX_IP_ROWS 256u
#define MIB_IPADDR_PRIMARY 0x1u

typedef struct ip_row {
    uint32_t addr, index, mask;
} ip_row;

static int row_cmp(const void *a, const void *b)
{
    uint32_t x = ntohl(((const ip_row *)a)->addr), y = ntohl(((const ip_row *)b)->addr);
    return x < y ? -1 : x > y;
}

/* DWORD GetIpAddrTable(PMIB_IPADDRTABLE table, PULONG size, BOOL order).
   MIB_IPADDRTABLE: dwNumEntries and rows of 24 bytes (dwAddr, dwIndex,
   dwMask, dwBCastAddr, dwReasmSize, unused1, wType). dwBCastAddr is 1
   and dwReasmSize is 65535, the values that Windows gives (UNCONFIRMED:
   Windows documents that it does not give the real broadcast address). */
void rt_GetIpAddrTable(_cpu *cpu)
{
    uint32_t table = ARG(0), psize = ARG(1), order = ARG(2), n = 0, need, i;
    struct ifaddrs *ifs = NULL, *p;
    ip_row rows[MAX_IP_ROWS];

    if (psize == 0)
    {
        RET(ERROR_INVALID_PARAMETER);
        return;
    }
    if (getifaddrs(&ifs) != 0) rt_fatal("GetIpAddrTable: getifaddrs failed");
    for (p = ifs; p != NULL; p = p->ifa_next)
    {
        if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_INET) continue;
        if (n == MAX_IP_ROWS) rt_fatal("GetIpAddrTable: the host has more than %u IPv4 addresses", MAX_IP_ROWS);
        rows[n].addr = ((const struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr;
        rows[n].mask = p->ifa_netmask != NULL ? ((const struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr : 0u;
        rows[n].index = if_nametoindex(p->ifa_name);
        n++;
    }
    freeifaddrs(ifs);
    need = 4u + 24u * n;
    if (table == 0 || rd32(psize) < need)
    {
        wr32(psize, need);
        RET(ERROR_INSUFFICIENT_BUFFER);
        return;
    }
    if (n == 0)
    {
        wr32(table, 0);
        RET(ERROR_NO_DATA);
        return;
    }
    if (order != 0) qsort(rows, n, sizeof rows[0], row_cmp);
    wr32(table, n);
    for (i = 0; i < n; i++)
    {
        uint32_t r = table + 4u + 24u * i;
        memcpy(G2H(r + 0), &rows[i].addr, 4);
        wr32(r + 4, rows[i].index);
        memcpy(G2H(r + 8), &rows[i].mask, 4);
        wr32(r + 12, 1);
        wr32(r + 16, 65535);
        wr16(r + 20, 0);
        wr16(r + 22, MIB_IPADDR_PRIMARY);
    }
    RET(0);
}

/* ---------------------------------------------------------------- WinHttpGetIEProxyConfigForCurrentUser */

/* Text buffer for the proxy strings. */
typedef struct tbuf {
    char s[4096];
    size_t n;
} tbuf;

static void tb_add(tbuf *b, const char *s)
{
    size_t l = strlen(s);
    if (b->n + l >= sizeof b->s) rt_fatal("WinHttpGetIEProxyConfigForCurrentUser: the proxy settings are too long");
    memcpy(b->s + b->n, s, l + 1);
    b->n += l;
}

static bool dict_flag(CFDictionaryRef d, CFStringRef key)
{
    CFNumberRef v = CFDictionaryGetValue(d, key);
    int x = 0;
    if (v == NULL || CFGetTypeID(v) != CFNumberGetTypeID()) return false;
    if (!CFNumberGetValue(v, kCFNumberIntType, &x)) return false;
    return x != 0;
}

/* The UTF-8 text of a string value; false if the key has no string. */
static bool dict_text(CFDictionaryRef d, CFStringRef key, char *out, size_t n)
{
    CFStringRef v = CFDictionaryGetValue(d, key);
    if (v == NULL || CFGetTypeID(v) != CFStringGetTypeID()) return false;
    if (!CFStringGetCString(v, out, (CFIndex)n, kCFStringEncodingUTF8))
        rt_fatal("WinHttpGetIEProxyConfigForCurrentUser: a proxy setting is too long");
    return true;
}

/* "host:port" of a proxy kind, or false if it is not enabled. */
static bool proxy_of(CFDictionaryRef d, CFStringRef enable, CFStringRef host, CFStringRef port, char *out, size_t n)
{
    char h[1024];
    CFNumberRef pv;
    int portnum = 0;
    if (!dict_flag(d, enable) || !dict_text(d, host, h, sizeof h) || h[0] == 0) return false;
    pv = CFDictionaryGetValue(d, port);
    if (pv != NULL && CFGetTypeID(pv) == CFNumberGetTypeID() && CFNumberGetValue(pv, kCFNumberIntType, &portnum) &&
        portnum > 0)
        snprintf(out, n, "%s:%d", h, portnum);
    else
        snprintf(out, n, "%s", h);
    return true;
}

/* A guest wide string in a fixed GlobalAlloc block (a guest heap block;
   the caller frees it with GlobalFree). Returns false if no guest memory
   is left. */
static bool global_wstr(const char *s, uint32_t *out)
{
    size_t units;
    uint32_t b;
    if (!rt_utf16_length(s, &units) || units > 0xFFFFu) rt_fatal("WinHttpGetIEProxyConfigForCurrentUser: bad text");
    b = rt_heap_alloc(2u * ((uint32_t)units + 1u));
    if (b == 0) return false;
    rt_put_wstr(b, (uint32_t)units + 1u, s);
    *out = b;
    return true;
}

/* BOOL WinHttpGetIEProxyConfigForCurrentUser(
   WINHTTP_CURRENT_USER_IE_PROXY_CONFIG *cfg): fAutoDetect,
   lpszAutoConfigUrl, lpszProxy, lpszProxyBypass. */
void rt_WinHttpGetIEProxyConfigForCurrentUser(_cpu *cpu)
{
    uint32_t cfg = ARG(0), url = 0, proxy = 0, bypass = 0;
    CFDictionaryRef d;
    tbuf p = { .n = 0 }, b = { .n = 0 };
    char one[1100], pac[2048];
    bool have_pac;
    CFArrayRef ex;

    if (cfg == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    p.s[0] = b.s[0] = 0;
    d = CFNetworkCopySystemProxySettings();
    if (d == NULL) rt_fatal("WinHttpGetIEProxyConfigForCurrentUser: CFNetworkCopySystemProxySettings failed");
    if (proxy_of(d, kCFNetworkProxiesHTTPEnable, kCFNetworkProxiesHTTPProxy, kCFNetworkProxiesHTTPPort, one, sizeof one))
    {
        tb_add(&p, "http=");
        tb_add(&p, one);
    }
    if (proxy_of(d, kCFNetworkProxiesHTTPSEnable, kCFNetworkProxiesHTTPSProxy, kCFNetworkProxiesHTTPSPort, one, sizeof one))
    {
        if (p.n) tb_add(&p, ";");
        tb_add(&p, "https=");
        tb_add(&p, one);
    }
    if (proxy_of(d, kCFNetworkProxiesFTPEnable, kCFNetworkProxiesFTPProxy, kCFNetworkProxiesFTPPort, one, sizeof one))
    {
        if (p.n) tb_add(&p, ";");
        tb_add(&p, "ftp=");
        tb_add(&p, one);
    }
    if (proxy_of(d, kCFNetworkProxiesSOCKSEnable, kCFNetworkProxiesSOCKSProxy, kCFNetworkProxiesSOCKSPort, one, sizeof one))
    {
        if (p.n) tb_add(&p, ";");
        tb_add(&p, "socks=");
        tb_add(&p, one);
    }
    ex = CFDictionaryGetValue(d, kCFNetworkProxiesExceptionsList);
    if (ex != NULL && CFGetTypeID(ex) == CFArrayGetTypeID())
    {
        CFIndex i, n = CFArrayGetCount(ex);
        for (i = 0; i < n; i++)
        {
            CFStringRef e = CFArrayGetValueAtIndex(ex, i);
            if (e == NULL || CFGetTypeID(e) != CFStringGetTypeID()) continue;
            if (!CFStringGetCString(e, one, (CFIndex)sizeof one, kCFStringEncodingUTF8))
                rt_fatal("WinHttpGetIEProxyConfigForCurrentUser: a proxy exception is too long");
            if (b.n) tb_add(&b, ";");
            tb_add(&b, one);
        }
    }
    if (dict_flag(d, kCFNetworkProxiesExcludeSimpleHostnames))
    {
        if (b.n) tb_add(&b, ";");
        tb_add(&b, "<local>");
    }
    have_pac = dict_flag(d, kCFNetworkProxiesProxyAutoConfigEnable) &&
               dict_text(d, kCFNetworkProxiesProxyAutoConfigURLString, pac, sizeof pac) && pac[0] != 0;
    wr32(cfg + 0, dict_flag(d, kCFNetworkProxiesProxyAutoDiscoveryEnable) ? 1u : 0u);
    CFRelease(d);
    if ((have_pac && !global_wstr(pac, &url)) || (p.n && !global_wstr(p.s, &proxy)) || (b.n && !global_wstr(b.s, &bypass)))
    {
        if (url) rt_heap_free(url);
        if (proxy) rt_heap_free(proxy);
        if (bypass) rt_heap_free(bypass);
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    wr32(cfg + 4, url);
    wr32(cfg + 8, proxy);
    wr32(cfg + 12, bypass);
    RET(1);
}
