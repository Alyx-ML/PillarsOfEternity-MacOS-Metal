/*
 * ADVAPI32 event log functions (RegisterEventSourceA, ReportEventA,
 * DeregisterEventSource).
 *
 * The event log of the emulated system is the runtime log: each reported
 * event is one log line with the source name, the event type, the
 * category, the event identifier and the insertion strings. The raw data
 * of an event is written as hexadecimal bytes (at most 256 bytes). Only
 * the local computer has an event log.
 */
#include "rt.h"
#include "rt_object.h"
#include "rt_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RPC_S_SERVER_UNAVAILABLE 1722u

typedef struct event_source {
    char name[256];
} event_source;

static void source_destroy(rt_object *o)
{
    free(o->data);
}

/* HANDLE RegisterEventSourceA(LPCSTR server, LPCSTR source) */
void rt_RegisterEventSourceA(_cpu *cpu)
{
    event_source *e;
    rt_object *o;
    uint32_t h;
    if (ARG(0) != 0)
    {
        char server[256];
        if (rt_gstr_a(server, sizeof server, ARG(0)) != RT_STR_OK)
        {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
        if (server[0] != 0 && strcasecmp(server, rt_computer_name()) != 0 &&
            !(server[0] == '\\' && server[1] == '\\' && strcasecmp(server + 2, rt_computer_name()) == 0))
        {
            rt_set_last_error(RPC_S_SERVER_UNAVAILABLE);
            RET(0);
            return;
        }
    }
    e = calloc(1, sizeof *e);
    if (e == NULL) rt_fatal("out of host memory (event source)");
    if (ARG(1) == 0 || rt_gstr_a(e->name, sizeof e->name, ARG(1)) != RT_STR_OK)
    {
        free(e);
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    o = rt_object_new(OBJ_EVENTLOG);
    o->data = e;
    o->destroy = source_destroy;
    h = rt_handle_new(o);
    rt_object_unref(o);
    if (h == 0) rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    RET(h);
}

/* BOOL DeregisterEventSource(HANDLE) */
void rt_DeregisterEventSource(_cpu *cpu)
{
    rt_object *o = rt_handle_get(ARG(0), OBJ_EVENTLOG);
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    rt_object_unref(o);
    RET(rt_handle_close(ARG(0)) ? 1u : 0u);
}

/* BOOL ReportEventA(HANDLE, WORD type, WORD category, DWORD id, PSID,
   WORD nstrings, DWORD datasize, LPCSTR *strings, LPVOID data) */
void rt_ReportEventA(_cpu *cpu)
{
    uint32_t type = ARG(1) & 0xFFFFu, category = ARG(2) & 0xFFFFu, id = ARG(3), n = ARG(5) & 0xFFFFu;
    uint32_t dsize = ARG(6), strs = ARG(7), data = ARG(8), i;
    rt_object *o = rt_handle_get(ARG(0), OBJ_EVENTLOG);
    event_source *e;
    char *line;
    size_t len, cap;
    const char *tname;
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    e = o->data;
    switch (type)
    {
    case 0x0000: tname = "success"; break;
    case 0x0001: tname = "error"; break;
    case 0x0002: tname = "warning"; break;
    case 0x0004: tname = "information"; break;
    case 0x0008: tname = "audit success"; break;
    case 0x0010: tname = "audit failure"; break;
    default:
        rt_object_unref(o);
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if ((n != 0 && strs == 0) || (dsize != 0 && data == 0))
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    cap = 512;
    line = malloc(cap);
    if (line == NULL) rt_fatal("out of host memory (ReportEventA)");
    len = (size_t)snprintf(line, cap, "event log: source \"%s\", %s, category %u, event 0x%08x", e->name, tname, category, id);
    for (i = 0; i < n; i++)
    {
        uint32_t sp = rd32(strs + 4u * i), sl = 0;
        char *u;
        if (sp == 0) continue;
        while (rd8(sp + sl) != 0) sl++;
        u = malloc(3u * (size_t)sl + 1u);
        if (u == NULL) rt_fatal("out of host memory (ReportEventA)");
        if (rt_gstr_a(u, 3u * (size_t)sl + 1u, sp) != RT_STR_OK) rt_fatal("ReportEventA: string conversion failed");
        if (len + strlen(u) + 8 > cap)
        {
            while (len + strlen(u) + 8 > cap) cap *= 2;
            line = realloc(line, cap);
            if (line == NULL) rt_fatal("out of host memory (ReportEventA)");
        }
        len += (size_t)snprintf(line + len, cap - len, ", \"%s\"", u);
        free(u);
    }
    if (dsize != 0)
    {
        uint32_t shown = dsize > 256u ? 256u : dsize;
        if (len + 3u * shown + 32 > cap)
        {
            cap = len + 3u * shown + 32;
            line = realloc(line, cap);
            if (line == NULL) rt_fatal("out of host memory (ReportEventA)");
        }
        len += (size_t)snprintf(line + len, cap - len, ", data (%u bytes):", dsize);
        for (i = 0; i < shown; i++) len += (size_t)snprintf(line + len, cap - len, " %02x", rd8(data + i));
    }
    free(line);
    rt_object_unref(o);
    RET(1);
}
