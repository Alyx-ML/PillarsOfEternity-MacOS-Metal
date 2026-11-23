/*
 * HID game controllers (rt_hid.c): IOKit devices shown as Windows HID
 * devices (raw input device list, CreateFileW device paths, HID.DLL).
 */
#ifndef RT_HID_H
#define RT_HID_H

#include "rt.h"

#include <stdbool.h>
#include <stdint.h>

#define RT_HID_NAME_CHARS 160
#define RT_HID_MAX_DEVICES 16

/* A HID device as the raw input functions show it. */
typedef struct rt_hid_info {
    uint32_t raw_handle; /* the raw input device handle */
    uint16_t vendor, product, version;
    uint16_t page, usage; /* the top-level collection */
    uint16_t in_len;      /* InputReportByteLength */
    char name[RT_HID_NAME_CHARS]; /* the device path (ASCII) */
} rt_hid_info;

/* The present devices (at most max). Starts the HID thread on the first
   call. */
int rt_hid_devices(rt_hid_info *out, int max);
/* The device of a raw input device handle. */
bool rt_hid_device(uint32_t raw_handle, rt_hid_info *out);
/* The preparsed data of a device (RIDI_PREPARSEDDATA): the size in bytes,
   or 0 if raw_handle is not a present device. Copies min(size, cap)
   bytes to dst when dst is not NULL. */
uint32_t rt_hid_preparsed(uint32_t raw_handle, uint8_t *dst, uint32_t cap);

/* CreateFile of a HID device path ("\\?\hid#..."). */
bool rt_hid_is_path(const char *path);
/* Returns the handle, or 0 with *err set (ERROR_*). */
uint32_t rt_hid_open(const char *path, uint32_t access, uint32_t flags, uint32_t *err);
/* True if h is a HID device handle. */
bool rt_hid_is_handle(uint32_t h);
/* ReadFile, WriteFile and CancelIo for a HID device handle (the same
   arguments as the KERNEL32 functions). */
void rt_hid_read_file(_cpu *cpu);
void rt_hid_write_file(_cpu *cpu);
void rt_hid_cancel_io(_cpu *cpu);
/* GetOverlappedResult with wait = TRUE and no event: wait until the read
   of ov on HID handle h is complete. */
void rt_hid_wait_overlapped(uint32_t h, uint32_t ov);

/* The raw input part (rt_rawinput.c) gets each input report of a device
   and each device arrival and removal. Any thread. */
void rt_rawinput_hid_report(uint32_t raw_handle, uint16_t page, uint16_t usage, const uint8_t *report, uint32_t len);
void rt_rawinput_device_change(uint32_t raw_handle, uint16_t page, uint16_t usage, bool arrival);

#endif
