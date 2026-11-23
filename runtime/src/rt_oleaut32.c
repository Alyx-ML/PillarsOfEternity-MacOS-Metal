/*
 * OLEAUT32 functions (imported by ordinal): BSTR strings (SysAllocString,
 * SysAllocStringLen, SysFreeString, SysStringLen), variants (VariantInit,
 * VariantClear, VariantChangeType) and safe arrays
 * (SafeArrayCreate, SafeArrayDestroy, SafeArrayGetDim, SafeArrayGetUBound,
 * SafeArrayGetLBound, SafeArrayPtrOfIndex, SafeArrayPutElement).
 *
 * A BSTR is a runtime heap block: a 4-byte byte count, the UTF-16 text and
 * a NUL character. The BSTR points to the text.
 *
 * A safe array descriptor has 16 hidden bytes in front of it, as on
 * Windows: the IID of the elements (VT_UNKNOWN, VT_DISPATCH), or the
 * VARTYPE in the last 4 bytes (FADF_HAVEVARTYPE). The bounds are stored
 * in reverse order of the SafeArrayCreate argument: rgsabound[0] is the
 * right-most (least significant) dimension, as the index vectors of
 * SafeArrayPtrOfIndex and SafeArrayPutElement.
 *
 * Arrays of VT_RECORD and arrays inside variants (VT_ARRAY) are not
 * supported; the runtime stops the process for them.
 */
#include "rt.h"
#include "rt_com.h"
#include "rt_text.h"

#include <inttypes.h>
#include <stdio.h>
#include "rt_thread.h"

#include <stdbool.h>
#include <string.h>

#define S_OK 0u
#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000Eu
#define DISP_E_BADVARTYPE 0x80020008u
#define DISP_E_BADINDEX 0x8002000Bu
#define DISP_E_ARRAYISLOCKED 0x8002000Du

#define VT_EMPTY 0u
#define VT_NULL 1u
#define VT_I2 2u
#define VT_I4 3u
#define VT_R4 4u
#define VT_R8 5u
#define VT_CY 6u
#define VT_DATE 7u
#define VT_BSTR 8u
#define VT_DISPATCH 9u
#define VT_ERROR 10u
#define VT_BOOL 11u
#define VT_VARIANT 12u
#define VT_UNKNOWN 13u
#define VT_DECIMAL 14u
#define VT_I1 16u
#define VT_UI1 17u
#define VT_UI2 18u
#define VT_UI4 19u
#define VT_I8 20u
#define VT_UI8 21u
#define VT_INT 22u
#define VT_UINT 23u
#define VT_RECORD 36u
#define VT_ARRAY 0x2000u
#define VT_BYREF 0x4000u

#define FADF_AUTO 0x1u
#define FADF_STATIC 0x2u
#define FADF_EMBEDDED 0x4u
#define FADF_FIXEDSIZE 0x10u
#define FADF_RECORD 0x20u
#define FADF_HAVEIID 0x40u
#define FADF_HAVEVARTYPE 0x80u
#define FADF_BSTR 0x100u
#define FADF_UNKNOWN 0x200u
#define FADF_DISPATCH 0x400u
#define FADF_VARIANT 0x800u

/* SAFEARRAY: cDims (WORD), fFeatures (WORD), cbElements, cLocks, pvData,
   then the bounds: cElements, lLbound for each dimension. */
#define SA_DIMS 0u
#define SA_FEATURES 2u
#define SA_ELEMSIZE 4u
#define SA_LOCKS 8u
#define SA_DATA 12u
#define SA_BOUNDS 16u
#define SA_HIDDEN 16u

#define BSTR_MAX_CHARS 0x3FFFFFF0u
#define MAX_DATA_BYTES 0x7FFFFFFFu

/* ---------------------------------------------------------------- BSTR */

/* A new BSTR of n characters copied from guest address s (0: zeros).
   Returns 0 if no memory is available. */
static uint32_t bstr_alloc(uint32_t s, uint32_t n)
{
    uint32_t blk, bytes;
    if (n > BSTR_MAX_CHARS) return 0;
    bytes = 2u * n;
    blk = rt_heap_alloc(bytes + 6u);
    if (blk == 0) return 0;
    wr32(blk, bytes);
    if (s != 0)
        memmove(G2H(blk + 4), G2H(s), bytes);
    else
        memset(G2H(blk + 4), 0, bytes);
    wr16(blk + 4 + bytes, 0);
    return blk + 4;
}

/* A copy of BSTR b with the same byte count (0 for a NULL BSTR). Sets
   *fail if no memory is available. */
static uint32_t bstr_copy(uint32_t b, bool *fail)
{
    uint32_t bytes, blk;
    if (b == 0) return 0;
    bytes = rd32(b - 4);
    if (bytes > 2u * BSTR_MAX_CHARS) rt_fatal("BSTR at 0x%08x has an invalid length", b);
    blk = rt_heap_alloc(bytes + 6u);
    if (blk == 0)
    {
        *fail = true;
        return 0;
    }
    wr32(blk, bytes);
    memcpy(G2H(blk + 4), G2H(b), bytes);
    wr16(blk + 4 + bytes, 0);
    return blk + 4;
}

static void bstr_free(uint32_t b)
{
    if (b == 0) return;
    if (b < 4) rt_fatal("SysFreeString of an invalid BSTR 0x%08x", b);
    rt_heap_free(b - 4);
}

/* BSTR SysAllocStringLen(const OLECHAR *, UINT len) */
void rt_SysAllocStringLen(_cpu *cpu) { RET(bstr_alloc(ARG(0), ARG(1))); }

/* void SysFreeString(BSTR) */
void rt_SysFreeString(_cpu *cpu) { bstr_free(ARG(0)); }

/* UINT SysStringLen(BSTR) */
void rt_SysStringLen(_cpu *cpu)
{
    uint32_t b = ARG(0);
    RET(b != 0 ? rd32(b - 4) / 2u : 0u);
}

/* BSTR SysAllocString(const OLECHAR *): NULL for a NULL string. */
void rt_SysAllocString(_cpu *cpu)
{
    uint32_t s = ARG(0);
    RET(s != 0 ? bstr_alloc(s, rt_wstrlen(s)) : 0u);
}

uint32_t rt_bstr_alloc_utf8(const char *s)
{
    size_t units;
    uint32_t b;
    if (!rt_utf16_length(s, &units)) rt_fatal("rt_bstr_alloc_utf8: the text is not valid UTF-8");
    if (units > BSTR_MAX_CHARS) return 0;
    b = bstr_alloc(0, (uint32_t)units);
    if (b == 0) return 0;
    if (rt_put_wstr(b, (uint32_t)units + 1u, s) != (uint32_t)units) rt_fatal("rt_bstr_alloc_utf8: write failed");
    return b;
}

/* ---------------------------------------------------------------- interfaces and variants */

/* Call IUnknown::AddRef (slot 1) or Release (slot 2) of p. */
static void unk_call(_cpu *cpu, uint32_t p, uint32_t slot)
{
    if (p == 0) return;
    rt_call_std(cpu, rd32(rd32(p) + 4u * slot), 1, p);
}

/* VariantClear of the VARIANT at v. */
static uint32_t variant_clear(_cpu *cpu, uint32_t v)
{
    uint32_t vt = rd16(v), base = vt & 0xFFFu;
    if (vt & VT_BYREF)
    {
        wr16(v, VT_EMPTY);
        return S_OK;
    }
    if (vt & VT_ARRAY) rt_fatal("VariantClear: VT_ARRAY variants are not supported");
    if (vt & ~0xFFFu) return DISP_E_BADVARTYPE;
    switch (base)
    {
    case VT_BSTR:
        bstr_free(rd32(v + 8));
        break;
    case VT_UNKNOWN:
    case VT_DISPATCH:
        unk_call(cpu, rd32(v + 8), 2);
        break;
    case VT_RECORD:
        rt_fatal("VariantClear: VT_RECORD variants are not supported");
    case VT_EMPTY: case VT_NULL: case VT_I2: case VT_I4: case VT_R4: case VT_R8: case VT_CY: case VT_DATE:
    case VT_ERROR: case VT_BOOL: case VT_DECIMAL: case VT_I1: case VT_UI1: case VT_UI2: case VT_UI4:
    case VT_I8: case VT_UI8: case VT_INT: case VT_UINT:
        break;
    default:
        return DISP_E_BADVARTYPE;
    }
    wr16(v, VT_EMPTY);
    return S_OK;
}

/* VariantCopy(dst, src) for an empty destination. */
static uint32_t variant_copy(_cpu *cpu, uint32_t dst, uint32_t src)
{
    uint32_t vt = rd16(src), base = vt & 0xFFFu;
    bool fail = false;
    if (vt & VT_BYREF)
    {
        memmove(G2H(dst), G2H(src), 16);
        return S_OK;
    }
    if (vt & VT_ARRAY) rt_fatal("VariantCopy: VT_ARRAY variants are not supported");
    if (base == VT_RECORD) rt_fatal("VariantCopy: VT_RECORD variants are not supported");
    if (vt & ~0xFFFu) return DISP_E_BADVARTYPE;
    if (base == VT_BSTR)
    {
        uint32_t b = bstr_copy(rd32(src + 8), &fail);
        if (fail) return E_OUTOFMEMORY;
        memmove(G2H(dst), G2H(src), 16);
        wr32(dst + 8, b);
        return S_OK;
    }
    memmove(G2H(dst), G2H(src), 16);
    if (base == VT_UNKNOWN || base == VT_DISPATCH) unk_call(cpu, rd32(dst + 8), 1);
    return S_OK;
}

/* void VariantInit(VARIANT *): VT_EMPTY. The other fields keep their
   values, as on Windows. */
void rt_VariantInit(_cpu *cpu)
{
    wr16(ARG(0), VT_EMPTY);
}

/* HRESULT VariantClear(VARIANT *) */
void rt_VariantClear(_cpu *cpu)
{
    uint32_t v = ARG(0);
    if (v == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    RET(variant_clear(cpu, v));
}

#define VARIANT_NOVALUEPROP 0x1u
#define VARIANT_ALPHABOOL 0x2u
#define VARIANT_NOUSEROVERRIDE 0x4u
#define VARIANT_LOCALBOOL 0x10u
#define DISP_E_TYPEMISMATCH 0x80020005u

/* The text of an integer or boolean VARIANT for the conversion to
   VT_BSTR. Returns false if vt has no such conversion in the runtime. */
static bool number_text(uint32_t v, uint32_t vt, uint32_t flags, char out[32])
{
    switch (vt)
    {
    case VT_I1: snprintf(out, 32, "%d", (int)(int8_t)rd8(v + 8)); return true;
    case VT_UI1: snprintf(out, 32, "%u", (unsigned)rd8(v + 8)); return true;
    case VT_I2: snprintf(out, 32, "%d", (int)(int16_t)rd16(v + 8)); return true;
    case VT_UI2: snprintf(out, 32, "%u", (unsigned)rd16(v + 8)); return true;
    case VT_I4: case VT_INT: snprintf(out, 32, "%" PRId32, (int32_t)rd32(v + 8)); return true;
    case VT_UI4: case VT_UINT: snprintf(out, 32, "%" PRIu32, rd32(v + 8)); return true;
    case VT_I8: snprintf(out, 32, "%" PRId64, (int64_t)rd64(v + 8)); return true;
    case VT_UI8: snprintf(out, 32, "%" PRIu64, rd64(v + 8)); return true;
    case VT_BOOL:
        /* VARIANT_LOCALBOOL gives the names of the user locale (English
           in the emulated system), as VARIANT_ALPHABOOL. */
        if (flags & (VARIANT_ALPHABOOL | VARIANT_LOCALBOOL))
            snprintf(out, 32, "%s", rd16(v + 8) != 0 ? "True" : "False");
        else
            snprintf(out, 32, "%s", rd16(v + 8) != 0 ? "-1" : "0");
        return true;
    default:
        return false;
    }
}

/* HRESULT VariantChangeType(VARIANT *dst, VARIANT *src, USHORT flags,
   VARTYPE vt). The runtime converts to the same type (a copy) and to
   VT_BSTR from VT_EMPTY, the integer types and VT_BOOL. Every other
   conversion stops the process with a message. */
void rt_VariantChangeType(_cpu *cpu)
{
    uint32_t dst = ARG(0), src = ARG(1), flags = ARG(2) & 0xFFFFu, vt = ARG(3) & 0xFFFFu, svt, hr, b;
    uint8_t tmp[16];
    char text[32];
    if (dst == 0 || src == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    if (flags & ~(VARIANT_NOVALUEPROP | VARIANT_ALPHABOOL | VARIANT_NOUSEROVERRIDE | VARIANT_LOCALBOOL))
    {
        RET(E_INVALIDARG);
        return;
    }
    svt = rd16(src);
    if (svt & (VT_BYREF | VT_ARRAY)) rt_fatal("VariantChangeType: source type 0x%x is not supported", svt);
    if (svt == vt)
    {
        if (dst == src)
        {
            RET(S_OK);
            return;
        }
        hr = variant_clear(cpu, dst);
        if (hr == S_OK) hr = variant_copy(cpu, dst, src);
        RET(hr);
        return;
    }
    if (vt != VT_BSTR) rt_fatal("VariantChangeType: conversion from type %u to type %u is not supported", svt, vt);
    if (svt == VT_NULL)
    {
        RET(DISP_E_TYPEMISMATCH);
        return;
    }
    if (svt == VT_EMPTY)
        text[0] = 0;
    else if (!number_text(src, svt, flags, text))
        rt_fatal("VariantChangeType: conversion from type %u to VT_BSTR is not supported", svt);
    b = rt_bstr_alloc_utf8(text);
    if (b == 0)
    {
        RET(E_OUTOFMEMORY);
        return;
    }
    /* The source has no resources (an integer, a boolean or nothing), so
       clearing it in place loses nothing. */
    hr = variant_clear(cpu, dst);
    if (hr != S_OK)
    {
        bstr_free(b);
        RET(hr);
        return;
    }
    memset(tmp, 0, sizeof tmp);
    memcpy(G2H(dst), tmp, sizeof tmp);
    wr16(dst, VT_BSTR);
    wr32(dst + 8, b);
    RET(S_OK);
}

/* ---------------------------------------------------------------- safe arrays */

/* Element size of vt in a safe array, or 0 for a type that is not valid. */
static uint32_t elem_size(uint32_t vt)
{
    switch (vt)
    {
    case VT_I1: case VT_UI1:
        return 1;
    case VT_I2: case VT_UI2: case VT_BOOL:
        return 2;
    case VT_I4: case VT_UI4: case VT_R4: case VT_INT: case VT_UINT: case VT_ERROR:
    case VT_BSTR: case VT_DISPATCH: case VT_UNKNOWN:
        return 4;
    case VT_R8: case VT_CY: case VT_DATE: case VT_I8: case VT_UI8:
        return 8;
    case VT_VARIANT: case VT_DECIMAL:
        return 16;
    default:
        return 0;
    }
}

static uint32_t dims(uint32_t psa) { return rd16(psa + SA_DIMS); }

/* Number of elements, or false on overflow. */
static bool cell_count(uint32_t psa, uint64_t *cells)
{
    uint32_t i, n = dims(psa);
    uint64_t c = 1;
    for (i = 0; i < n; i++)
    {
        c *= rd32(psa + SA_BOUNDS + 8u * i);
        if (c > MAX_DATA_BYTES) return false;
    }
    *cells = c;
    return true;
}

static void write_iid(uint32_t g, uint32_t d1)
{
    /* {0000000x-0000-0000-C000-000000000046} */
    static const uint8_t tail[8] = { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
    wr32(g, d1);
    wr16(g + 4, 0);
    wr16(g + 6, 0);
    memcpy(G2H(g + 8), tail, 8);
}

/* SAFEARRAY *SafeArrayCreate(VARTYPE, UINT dims, SAFEARRAYBOUND *) */
void rt_SafeArrayCreate(_cpu *cpu)
{
    uint32_t vt = ARG(0) & 0xFFFFu, n = ARG(1), bounds = ARG(2), size, blk, psa, data, i, features;
    uint64_t cells, bytes;
    if (vt == VT_RECORD) rt_fatal("SafeArrayCreate: VT_RECORD arrays are not supported");
    size = elem_size(vt);
    if (size == 0 || n == 0 || n > 0xFFFFu || bounds == 0)
    {
        RET(0);
        return;
    }
    blk = rt_heap_calloc(1, SA_HIDDEN + SA_BOUNDS + 8u * n);
    if (blk == 0)
    {
        RET(0);
        return;
    }
    psa = blk + SA_HIDDEN;
    wr16(psa + SA_DIMS, (uint16_t)n);
    wr32(psa + SA_ELEMSIZE, size);
    for (i = 0; i < n; i++)
    {
        /* Reverse order: the last argument bound is the right-most
           dimension, rgsabound[0]. */
        uint32_t from = bounds + 8u * (n - 1u - i);
        wr32(psa + SA_BOUNDS + 8u * i, rd32(from));
        wr32(psa + SA_BOUNDS + 8u * i + 4u, rd32(from + 4));
    }
    if (vt == VT_UNKNOWN || vt == VT_DISPATCH)
    {
        features = FADF_HAVEIID | (vt == VT_UNKNOWN ? FADF_UNKNOWN : FADF_DISPATCH);
        write_iid(blk, vt == VT_UNKNOWN ? 0u : 0x00020400u);
    }
    else
    {
        features = FADF_HAVEVARTYPE | (vt == VT_BSTR ? FADF_BSTR : vt == VT_VARIANT ? FADF_VARIANT : 0u);
        wr32(psa - 4, vt);
    }
    wr16(psa + SA_FEATURES, (uint16_t)features);
    if (!cell_count(psa, &cells) || (bytes = cells * size) > MAX_DATA_BYTES)
    {
        rt_heap_free(blk);
        RET(0);
        return;
    }
    data = rt_heap_calloc(1, bytes != 0 ? (uint32_t)bytes : 1u);
    if (data == 0)
    {
        rt_heap_free(blk);
        RET(0);
        return;
    }
    wr32(psa + SA_DATA, data);
    RET(psa);
}

/* HRESULT SafeArrayDestroy(SAFEARRAY *) */
void rt_SafeArrayDestroy(_cpu *cpu)
{
    uint32_t psa = ARG(0), features, data, size;
    uint64_t cells, i;
    if (psa == 0)
    {
        RET(S_OK);
        return;
    }
    if (__atomic_load_n((uint32_t *)G2H(psa + SA_LOCKS), __ATOMIC_ACQUIRE) != 0)
    {
        RET(DISP_E_ARRAYISLOCKED);
        return;
    }
    features = rd16(psa + SA_FEATURES);
    if (features & (FADF_AUTO | FADF_STATIC | FADF_EMBEDDED | FADF_RECORD))
        rt_fatal("SafeArrayDestroy: array features 0x%x are not supported (only SafeArrayCreate arrays)", features);
    if (!cell_count(psa, &cells)) rt_fatal("SafeArrayDestroy: invalid array 0x%08x", psa);
    data = rd32(psa + SA_DATA);
    size = rd32(psa + SA_ELEMSIZE);
    if (data != 0)
    {
        for (i = 0; i < cells; i++)
        {
            uint32_t e = data + (uint32_t)i * size;
            if (features & FADF_BSTR)
                bstr_free(rd32(e));
            else if (features & (FADF_UNKNOWN | FADF_DISPATCH))
                unk_call(cpu, rd32(e), 2);
            else if (features & FADF_VARIANT)
            {
                uint32_t hr = variant_clear(cpu, e);
                if (hr != S_OK)
                {
                    RET(hr);
                    return;
                }
            }
        }
        rt_heap_free(data);
    }
    rt_heap_free(psa - SA_HIDDEN);
    RET(S_OK);
}

/* UINT SafeArrayGetDim(SAFEARRAY *) */
void rt_SafeArrayGetDim(_cpu *cpu)
{
    uint32_t psa = ARG(0);
    RET(psa != 0 ? dims(psa) : 0u);
}

/* The bound of dimension d (1 = left-most) of psa: rgsabound[dims - d]. */
static void get_bound(_cpu *cpu, bool upper)
{
    uint32_t psa = ARG(0), d = ARG(1), out = ARG(2), b;
    int64_t lb;
    if (psa == 0 || out == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    if (d < 1 || d > dims(psa))
    {
        RET(DISP_E_BADINDEX);
        return;
    }
    b = psa + SA_BOUNDS + 8u * (dims(psa) - d);
    lb = (int32_t)rd32(b + 4);
    wr32(out, (uint32_t)(int32_t)(upper ? lb + (int64_t)rd32(b) - 1 : lb));
    RET(S_OK);
}

/* HRESULT SafeArrayGetUBound(SAFEARRAY *, UINT dim, LONG *) */
void rt_SafeArrayGetUBound(_cpu *cpu) { get_bound(cpu, true); }

/* HRESULT SafeArrayGetLBound(SAFEARRAY *, UINT dim, LONG *) */
void rt_SafeArrayGetLBound(_cpu *cpu) { get_bound(cpu, false); }

/* The address of the element at the index vector idx (idx[0] for
   rgsabound[0]). Returns an HRESULT. */
static uint32_t element_at(uint32_t psa, uint32_t idx, uint32_t *addr)
{
    uint32_t i, n;
    uint64_t cell = 0, scale = 1;
    if (psa == 0 || idx == 0) return E_INVALIDARG;
    n = dims(psa);
    if (n == 0) return E_INVALIDARG;
    for (i = 0; i < n; i++)
    {
        uint32_t b = psa + SA_BOUNDS + 8u * i, count = rd32(b);
        int64_t lb = (int32_t)rd32(b + 4), x = (int32_t)rd32(idx + 4u * i);
        if (x < lb || x >= lb + (int64_t)count) return DISP_E_BADINDEX;
        cell += (uint64_t)(x - lb) * scale;
        scale *= count;
        if (scale > MAX_DATA_BYTES) return DISP_E_BADINDEX;
    }
    if (cell * rd32(psa + SA_ELEMSIZE) > MAX_DATA_BYTES) return DISP_E_BADINDEX;
    *addr = rd32(psa + SA_DATA) + (uint32_t)(cell * rd32(psa + SA_ELEMSIZE));
    return S_OK;
}

/* HRESULT SafeArrayPtrOfIndex(SAFEARRAY *, LONG *indices, void **) */
void rt_SafeArrayPtrOfIndex(_cpu *cpu)
{
    uint32_t out = ARG(2), addr = 0, hr;
    if (out == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    hr = element_at(ARG(0), ARG(1), &addr);
    if (hr == S_OK) wr32(out, addr);
    RET(hr);
}

/* HRESULT SafeArrayPutElement(SAFEARRAY *, LONG *indices, void *value).
   For BSTR, VT_UNKNOWN and VT_DISPATCH arrays, value is the string or the
   interface; for the other types it points to the value. */
void rt_SafeArrayPutElement(_cpu *cpu)
{
    uint32_t psa = ARG(0), v = ARG(2), addr = 0, hr, features;
    uint32_t *locks;
    hr = element_at(psa, ARG(1), &addr);
    if (hr != S_OK)
    {
        RET(hr);
        return;
    }
    features = rd16(psa + SA_FEATURES);
    if (features & FADF_RECORD) rt_fatal("SafeArrayPutElement: VT_RECORD arrays are not supported");
    locks = G2H(psa + SA_LOCKS);
    __atomic_fetch_add(locks, 1u, __ATOMIC_ACQ_REL);
    if (features & FADF_BSTR)
    {
        bool fail = false;
        uint32_t b = bstr_copy(v, &fail);
        if (fail)
            hr = E_OUTOFMEMORY;
        else
        {
            bstr_free(rd32(addr));
            wr32(addr, b);
        }
    }
    else if (features & (FADF_UNKNOWN | FADF_DISPATCH))
    {
        uint32_t old = rd32(addr);
        unk_call(cpu, v, 1);
        wr32(addr, v);
        unk_call(cpu, old, 2);
    }
    else if (features & FADF_VARIANT)
    {
        if (v == 0)
            hr = E_INVALIDARG;
        else
        {
            hr = variant_clear(cpu, addr);
            if (hr == S_OK) hr = variant_copy(cpu, addr, v);
        }
    }
    else if (v == 0)
        hr = E_INVALIDARG;
    else
        memmove(G2H(addr), G2H(v), rd32(psa + SA_ELEMSIZE));
    __atomic_fetch_sub(locks, 1u, __ATOMIC_ACQ_REL);
    RET(hr);
}
