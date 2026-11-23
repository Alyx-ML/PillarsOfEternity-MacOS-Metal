// The __GUEST segment of build/aot/mono-aot: zero-fill memory that the
// loader reserves with the program image. It covers guest addresses
// 0x0FF00000-0x30000000 (tools/build_aot_host.sh sets the segment
// address). The fixed-base guest images (mono.dll at 0x10000000, the AOT
// images from 0x20000000) are mapped there. rt_mem_init (runtime/src/rt_mem.c)
// replaces the segment with reserved memory and makes it free guest address
// space. Because the loader reserves the range, no host memory that is
// made before rt_mem_init can be in it.
//
// The segment starts 1 MiB below 0x10000000. The slide can move the
// segment start up to 48 KiB off a 64 KiB granule, and rt_mem_init uses only
// the granules that are completely in the segment.
//
// This object must be first on the link line: the linker puts segments in
// the order of their first appearance, and the addresses must increase.
// Alignment 2^14 (16 KiB): the linker does not align a segment more than
// the host page.
    .globl _rt_guest_space
    .zerofill __GUEST,__space,_rt_guest_space,0x20100000,14
