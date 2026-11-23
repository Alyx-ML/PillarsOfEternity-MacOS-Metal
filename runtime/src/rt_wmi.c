/*
 * WMI (Windows Management Instrumentation): the objects that the player
 * uses for the video memory size and for SystemInfo.deviceUniqueIdentifier.
 *
 * The player calls:
 *   CoCreateInstance(CLSID_WbemLocator, IID_IWbemLocator)
 *   IWbemLocator::ConnectServer("root\cimv2" or "\\.\root\cimv2")
 *   CoSetProxyBlanket(services)
 *   IWbemServices::CreateInstanceEnum(class, WBEM_FLAG_RETURN_IMMEDIATELY |
 *       WBEM_FLAG_FORWARD_ONLY or 0)
 *   IEnumWbemClassObject::Skip and Next
 *   IWbemClassObject::Get(property)
 * with these classes and properties:
 *   Win32_VideoController  AdapterRAM
 *   Win32_BaseBoard        SerialNumber
 *   Win32_BIOS             SerialNumber
 *   Win32_Processor        UniqueId
 *   Win32_DiskDrive        SerialNumber
 *   Win32_OperatingSystem  SerialNumber
 *
 * The data of the emulated system:
 *   Win32_VideoController  one instance: the Metal device (rt_gpu_info.m).
 *                          Also DeviceID, Name, Caption, Description and
 *                          VideoProcessor.
 *   Win32_BaseBoard,       one instance each. SerialNumber is the serial
 *   Win32_BIOS             number of the Mac (IOPlatformSerialNumber), or
 *                          NULL if the Mac has none.
 *   Win32_Processor        one instance (the one processor package of the
 *                          emulated system). UniqueId is NULL: Windows
 *                          does not fill it for x86 processors.
 *   Win32_DiskDrive        one instance for each block storage device of
 *                          the Mac. Windows XP has no SerialNumber property
 *                          in this class, so Get returns WBEM_E_NOT_FOUND.
 *   Win32_OperatingSystem  one instance. SerialNumber is the product ID of
 *                          the emulated Windows installation: digits from
 *                          the SHA-256 hash of the hardware UUID of the Mac
 *                          (IOPlatformUUID), in the form 12345-640-1234567-12345.
 *                          The value stays the same on one Mac.
 * Other namespaces, classes, properties and methods stop the process with
 * a message.
 */
#include "rt.h"
#include "rt_com.h"
#include "rt_com_thunks.h"
#include "rt_text.h"

#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define WBEM_S_FALSE 0x00000001u
#define WBEM_E_NOT_FOUND 0x80041002u
#define WBEM_E_INVALID_PARAMETER 0x80041008u
#define WBEM_E_INVALID_OPERATION 0x80041016u
#define WBEM_E_LOCAL_CREDENTIALS 0x80041064u
#define WBEM_FLAG_RETURN_IMMEDIATELY 0x10u
#define WBEM_FLAG_FORWARD_ONLY 0x20u

/* CIM types. */
#define CIM_STRING 8u
#define CIM_UINT32 19u
#define WBEM_FLAVOR_ORIGIN_PROPAGATED 0x20u

/* VARIANT types. */
#define VT_NULL 1u
#define VT_I4 3u
#define VT_BSTR 8u

static const rt_guid IID_IWbemLocator = { 0xDC12A687, 0x737F, 0x11CF, { 0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24 } };
static const rt_guid IID_IWbemServices = { 0x9556DC99, 0x828C, 0x11CF, { 0xA3, 0x7E, 0x00, 0xAA, 0x00, 0x32, 0x40, 0xC7 } };
static const rt_guid IID_IWbemClassObject = { 0xDC12A681, 0x737F, 0x11CF, { 0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24 } };
static const rt_guid IID_IEnumWbemClassObject = { 0x027947E1, 0xD731, 0x11CE, { 0xA3, 0x57, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

static const rt_com_iface locator_ifaces[] = { { &IID_IWbemLocator, 0 } };
static const rt_com_iface services_ifaces[] = { { &IID_IWbemServices, 0 } };
static const rt_com_iface enum_ifaces[] = { { &IID_IEnumWbemClassObject, 0 } };
static const rt_com_iface object_ifaces[] = { { &IID_IWbemClassObject, 0 } };

static void free_data(rt_com_object *o) { free(o->data); }

static const rt_com_class locator_class = {
    "WbemLocator", 1, { &rt_vt_WbemLocator, NULL, NULL, NULL }, locator_ifaces, 1, NULL
};
static const rt_com_class services_class = {
    "WbemServices", 1, { &rt_vt_WbemServices, NULL, NULL, NULL }, services_ifaces, 1, NULL
};
static const rt_com_class enum_class = {
    "EnumWbemClassObject", 1, { &rt_vt_EnumWbemClassObject, NULL, NULL, NULL }, enum_ifaces, 1, free_data
};
static const rt_com_class object_class = {
    "WbemClassObject", 1, { &rt_vt_WbemClassObject, NULL, NULL, NULL }, object_ifaces, 1, free_data
};

rt_com_object *rt_wmi_create_locator(void)
{
    return rt_com_create(&locator_class, NULL);
}

/* ---------------------------------------------------------------- host data */

/* A string property of the IOPlatformExpertDevice (UTF-8). Returns false
   if the device has no such property. */
static bool platform_string(const char *key, char *out, size_t n)
{
    io_service_t dev = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPlatformExpertDevice"));
    CFStringRef k, v;
    bool ok = false;
    if (dev == IO_OBJECT_NULL) rt_fatal("WMI: IOKit has no IOPlatformExpertDevice");
    k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (k == NULL) rt_fatal("out of host memory");
    v = IORegistryEntryCreateCFProperty(dev, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    IOObjectRelease(dev);
    if (v == NULL) return false;
    if (CFGetTypeID(v) != CFStringGetTypeID()) rt_fatal("WMI: IOKit property %s is not a string", key);
    ok = CFStringGetCString(v, out, (CFIndex)n, kCFStringEncodingUTF8);
    CFRelease(v);
    if (!ok) rt_fatal("WMI: IOKit property %s is too long", key);
    return true;
}

/* The number of block storage devices (disk drives) of the Mac. */
static uint32_t disk_count(void)
{
    io_iterator_t it;
    io_object_t s;
    uint32_t n = 0;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOBlockStorageDevice"), &it);
    if (kr != KERN_SUCCESS) rt_fatal("WMI: IOServiceGetMatchingServices(IOBlockStorageDevice) failed (0x%x)", kr);
    while ((s = IOIteratorNext(it)) != IO_OBJECT_NULL)
    {
        n++;
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    return n;
}

/* The product ID of the emulated Windows installation. */
static void product_id(char out[24])
{
    char uuid[128];
    uint8_t h[CC_SHA256_DIGEST_LENGTH];
    uint32_t a, b, c;
    if (!platform_string("IOPlatformUUID", uuid, sizeof uuid)) rt_fatal("WMI: the Mac has no IOPlatformUUID");
    CC_SHA256(uuid, (CC_LONG)strlen(uuid), h);
    a = ((uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3]) % 100000u;
    b = ((uint32_t)h[4] << 24 | (uint32_t)h[5] << 16 | (uint32_t)h[6] << 8 | h[7]) % 10000000u;
    c = ((uint32_t)h[8] << 24 | (uint32_t)h[9] << 16 | (uint32_t)h[10] << 8 | h[11]) % 100000u;
    snprintf(out, 24, "%05u-640-%07u-%05u", a, b, c);
}

/* ---------------------------------------------------------------- classes */

typedef enum { WC_VIDEO, WC_BASEBOARD, WC_BIOS, WC_PROCESSOR, WC_DISK, WC_OS, WC_COUNT } wmi_class;

static const char *const class_names[WC_COUNT] = {
    "Win32_VideoController", "Win32_BaseBoard", "Win32_BIOS", "Win32_Processor", "Win32_DiskDrive",
    "Win32_OperatingSystem",
};

static uint32_t instance_count(wmi_class c)
{
    return c == WC_DISK ? disk_count() : 1u;
}

typedef struct wmi_enum {
    wmi_class cls;
    uint32_t pos, count;
    bool forward_only;
} wmi_enum;

typedef struct wmi_instance {
    wmi_class cls;
    uint32_t index;
} wmi_instance;

/* One lock for the position of all enumerators. */
static pthread_mutex_t enum_lock = PTHREAD_MUTEX_INITIALIZER;

/* Copy a guest UTF-16 string (UTF-8, at most n-1 bytes) or stop the
   process with a message naming what. */
static void text_arg(uint32_t ws, char *out, size_t n, const char *what)
{
    if (rt_gstr_w(out, n, ws) != RT_STR_OK) rt_fatal("WMI: %s is not valid text of at most %zu bytes", what, n - 1);
}

/* HRESULT IWbemLocator::ConnectServer(BSTR resource, BSTR user,
   BSTR password, BSTR locale, LONG flags, BSTR authority, IWbemContext *,
   IWbemServices **) */
void rt_com_WbemLocator_ConnectServer(_cpu *cpu)
{
    uint32_t res = ARG(1), user = ARG(2), password = ARG(3), pp = ARG(8);
    char ns[256];
    rt_com_object *o;
    (void)rt_com_this(ARG(0), 0, &locator_class);
    if (pp == 0 || res == 0)
    {
        RET(WBEM_E_INVALID_PARAMETER);
        return;
    }
    wr32(pp, 0);
    if (user != 0 || password != 0)
    {
        RET(WBEM_E_LOCAL_CREDENTIALS); /* credentials are not allowed for a local connection */
        return;
    }
    if (ARG(7) != 0) rt_fatal("IWbemLocator::ConnectServer with a context object is not supported");
    text_arg(res, ns, sizeof ns, "the namespace");
    if (strcasecmp(ns, "root\\cimv2") != 0 && strcasecmp(ns, "\\\\.\\root\\cimv2") != 0)
        rt_fatal("WMI: namespace \"%s\" is not supported", ns);
    o = rt_com_create(&services_class, NULL);
    if (o == NULL)
    {
        RET(E_OUTOFMEMORY);
        return;
    }
    wr32(pp, rt_com_ptr(o, 0));
    RET(S_OK);
}

/* HRESULT IWbemServices::CreateInstanceEnum(BSTR class, LONG flags,
   IWbemContext *, IEnumWbemClassObject **) */
void rt_com_WbemServices_CreateInstanceEnum(_cpu *cpu)
{
    uint32_t cls = ARG(1), flags = ARG(2), ctx = ARG(3), pp = ARG(4);
    char name[128];
    wmi_enum *e;
    rt_com_object *o;
    int c;
    (void)rt_com_this(ARG(0), 0, &services_class);
    if (cls == 0 || pp == 0)
    {
        RET(WBEM_E_INVALID_PARAMETER);
        return;
    }
    wr32(pp, 0);
    if ((flags & ~(WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY)) != 0)
        rt_fatal("IWbemServices::CreateInstanceEnum with flags 0x%x is not supported", flags);
    if (ctx != 0) rt_fatal("IWbemServices::CreateInstanceEnum with a context object is not supported");
    text_arg(cls, name, sizeof name, "the class name");
    for (c = 0; c < WC_COUNT; c++)
        if (strcasecmp(name, class_names[c]) == 0) break;
    if (c == WC_COUNT) rt_fatal("WMI: the instance enumeration of class \"%s\" is not supported", name);
    e = calloc(1, sizeof(*e));
    if (e == NULL) rt_fatal("out of host memory");
    e->cls = (wmi_class)c;
    e->count = instance_count(e->cls);
    e->forward_only = (flags & WBEM_FLAG_FORWARD_ONLY) != 0;
    o = rt_com_create(&enum_class, e);
    if (o == NULL)
    {
        free(e);
        RET(E_OUTOFMEMORY);
        return;
    }
    wr32(pp, rt_com_ptr(o, 0));
    RET(S_OK);
}

static void enum_lock_take(void)
{
    if (pthread_mutex_lock(&enum_lock) != 0) rt_fatal("WMI lock failed");
}

static void enum_lock_drop(void)
{
    if (pthread_mutex_unlock(&enum_lock) != 0) rt_fatal("WMI unlock failed");
}

/* HRESULT IEnumWbemClassObject::Reset(void). A forward-only enumerator
   cannot go back. */
void rt_com_EnumWbemClassObject_Reset(_cpu *cpu)
{
    wmi_enum *e = rt_com_this(ARG(0), 0, &enum_class)->data;
    if (e->forward_only)
    {
        RET(WBEM_E_INVALID_OPERATION);
        return;
    }
    enum_lock_take();
    e->pos = 0;
    enum_lock_drop();
    RET(S_OK);
}

/* HRESULT IEnumWbemClassObject::Next(LONG timeout, ULONG count,
   IWbemClassObject **objects, ULONG *returned). The data is in memory, so
   the timeout has no effect. Returns WBEM_S_FALSE when fewer than count
   objects are left. */
void rt_com_EnumWbemClassObject_Next(_cpu *cpu)
{
    wmi_enum *e = rt_com_this(ARG(0), 0, &enum_class)->data;
    uint32_t count = ARG(2), objs = ARG(3), pret = ARG(4), n = 0;
    if (pret == 0 || (count != 0 && objs == 0))
    {
        RET(WBEM_E_INVALID_PARAMETER);
        return;
    }
    enum_lock_take();
    while (n < count && e->pos < e->count)
    {
        wmi_instance *in = malloc(sizeof(*in));
        rt_com_object *o;
        if (in == NULL) rt_fatal("out of host memory");
        in->cls = e->cls;
        in->index = e->pos;
        o = rt_com_create(&object_class, in);
        if (o == NULL)
        {
            free(in);
            enum_lock_drop();
            wr32(pret, n);
            RET(E_OUTOFMEMORY);
            return;
        }
        wr32(objs + 4u * n, rt_com_ptr(o, 0));
        n++;
        e->pos++;
    }
    enum_lock_drop();
    wr32(pret, n);
    RET(n == count ? S_OK : WBEM_S_FALSE);
}

/* HRESULT IEnumWbemClassObject::Skip(LONG timeout, ULONG count) */
void rt_com_EnumWbemClassObject_Skip(_cpu *cpu)
{
    wmi_enum *e = rt_com_this(ARG(0), 0, &enum_class)->data;
    uint32_t count = ARG(2), left;
    enum_lock_take();
    left = e->count - e->pos;
    if (count > left)
    {
        e->pos = e->count;
        enum_lock_drop();
        RET(WBEM_S_FALSE);
        return;
    }
    e->pos += count;
    enum_lock_drop();
    RET(S_OK);
}

/* HRESULT IWbemClassObject::Get(LPCWSTR name, LONG flags, VARIANT *value,
   CIMTYPE *type, LONG *flavor) */
void rt_com_WbemClassObject_Get(_cpu *cpu)
{
    wmi_instance *in = rt_com_this(ARG(0), 0, &object_class)->data;
    uint32_t name = ARG(1), flags = ARG(2), pv = ARG(3), ptype = ARG(4), pflavor = ARG(5);
    char prop[128], buf[256];
    const char *text = NULL;
    bool is_null = false;
    uint32_t cim = CIM_STRING, b;
    if (name == 0)
    {
        RET(WBEM_E_INVALID_PARAMETER);
        return;
    }
    if (flags != 0) rt_fatal("IWbemClassObject::Get with flags 0x%x is not supported", flags);
    text_arg(name, prop, sizeof prop, "the property name");
    switch (in->cls)
    {
    case WC_VIDEO:
        if (strcasecmp(prop, "AdapterRAM") == 0)
            cim = CIM_UINT32;
        else if (strcasecmp(prop, "DeviceID") == 0)
            text = "VideoController1";
        else if (strcasecmp(prop, "Name") == 0 || strcasecmp(prop, "Caption") == 0 ||
                 strcasecmp(prop, "Description") == 0 || strcasecmp(prop, "VideoProcessor") == 0)
            text = rt_gpu_name();
        else
            rt_fatal("WMI: property Win32_VideoController.%s is not supported", prop);
        break;
    case WC_BASEBOARD:
    case WC_BIOS:
        if (strcasecmp(prop, "SerialNumber") != 0) rt_fatal("WMI: property %s.%s is not supported", class_names[in->cls], prop);
        if (platform_string("IOPlatformSerialNumber", buf, sizeof buf))
            text = buf;
        else
            is_null = true;
        break;
    case WC_PROCESSOR:
        if (strcasecmp(prop, "UniqueId") != 0) rt_fatal("WMI: property Win32_Processor.%s is not supported", prop);
        is_null = true;
        break;
    case WC_DISK:
        if (strcasecmp(prop, "SerialNumber") == 0)
        {
            /* Not a property of Win32_DiskDrive on Windows XP. */
            RET(WBEM_E_NOT_FOUND);
            return;
        }
        rt_fatal("WMI: property Win32_DiskDrive.%s is not supported", prop);
    case WC_OS:
        if (strcasecmp(prop, "SerialNumber") != 0) rt_fatal("WMI: property Win32_OperatingSystem.%s is not supported", prop);
        product_id(buf);
        text = buf;
        break;
    default:
        rt_fatal("WMI: bad instance class %d", (int)in->cls);
    }
    if (pv != 0)
    {
        memset(G2H(pv), 0, 16);
        if (cim == CIM_UINT32)
        {
            /* WMI returns CIM_UINT32 values as VT_I4. */
            wr16(pv, VT_I4);
            wr32(pv + 8, rt_gpu_adapter_ram());
        }
        else if (is_null)
            wr16(pv, VT_NULL);
        else
        {
            b = rt_bstr_alloc_utf8(text);
            if (b == 0)
            {
                RET(E_OUTOFMEMORY);
                return;
            }
            wr16(pv, VT_BSTR);
            wr32(pv + 8, b);
        }
    }
    if (ptype != 0) wr32(ptype, cim);
    if (pflavor != 0) wr32(pflavor, WBEM_FLAVOR_ORIGIN_PROPAGATED);
    RET(S_OK);
}
