/*
 * Code of mono.dll that runs from memory that Mono allocates.
 *
 * Mono's x86 back end calls the cpuid instruction through cpuid_impl, a
 * byte array of x86 code in .rdata: it copies the array to executable
 * memory from its code manager (for Windows XP data execution prevention)
 * and calls the copy. SRW translates only the code section, so the runtime
 * has a procedure that does what the array does (rt_mono_cpuid), and
 * rt_code_target sends a call of an exact copy of the array to it.
 *
 * cpuid_impl, __cdecl void (int id, int *p_eax, int *p_ebx, int *p_ecx,
 * int *p_edx):
 *   push ebp; mov ebp, esp; push ebx; mov eax, [ebp+8]; cpuid; push eax;
 *   mov eax, [ebp+16]; mov [eax], ebx; mov eax, [ebp+20]; mov [eax], ecx;
 *   mov eax, [ebp+24]; mov [eax], edx; pop eax; mov edx, [ebp+12];
 *   mov [edx], eax; pop ebx; leave; ret
 *
 * Stack overflow helper: win32_seh_init (exceptions-x86.c of Mono) calls
 * mono_win32_get_handle_stackoverflow, which writes a helper of x86 code
 * to memory from mono_global_codeman_reserve and keeps the helper address
 * in a static variable. win32_handle_stack_overflow calls the helper only
 * for EXCEPTION_STACK_OVERFLOW. In AOT-only mode (--full-aot),
 * mono_global_codeman_reserve stops the process with g_error, so Mono
 * cannot start. The runtime cannot run code that Mono writes at run time,
 * and it does not raise EXCEPTION_STACK_OVERFLOW. Thus the runtime writes
 * the address of its procedure rt_mono_stack_overflow to the static
 * variable before Mono starts. Mono then uses that address and writes no
 * code. If the helper is called, rt_mono_stack_overflow stops the process
 * with an error message.
 *
 * InterfaceTypeAttribute of COM interfaces: when the Mono AOT compiler
 * makes the native wrapper of a method of a COM import interface,
 * mono_mb_emit_cominterop_call calls cominterop_get_com_slot_begin
 * (cominterop.c). That function gets the InterfaceTypeAttribute object of
 * the interface with mono_custom_attrs_get_attr, which runs the managed
 * constructor of each attribute of the interface through
 * mono_runtime_invoke. The JIT compiles the runtime-invoke wrapper to x86
 * code at run time, and the runtime cannot run that code. Thus an entry of
 * work/mono/instruction_replacements.sci changes the call of
 * mono_custom_attrs_get_attr at 0x1001F189 to a call of
 * rt_mono_interface_type_attr. rt_mono_interface_type_attr finds
 * the attribute as mono_custom_attrs_get_attr does, reads the constructor
 * argument from the attribute blob (ECMA-335 II.23.3) and makes the
 * object. The two constructors of InterfaceTypeAttribute in the game's
 * mscorlib.dll call Attribute::.ctor and store the argument in the field
 * intType, and they do nothing more. No other attribute constructor runs. *
 * AOT-only mode of the player: the runtime cannot run x86 code that the
 * Mono JIT writes, so Mono must use only the translated full-AOT images.
 * The Mono command-line driver sets the variable mono_aot_only for
 * --full-aot (mono_main, at 0x100B0F7C: mov dword [0x101F5DD4], 1). The
 * player starts Mono with mono_jit_init_version, and mono.dll exports no
 * function that sets the variable. Thus rt_mono_set_aot_only writes 1 to
 * the variable before the program runs. The code that reads it prints the
 * "aot-only mode" messages of Mono (for example at 0x10096C4A and
 * 0x100EFE76).
 *
 * TLS index of the current thread: Mono's x86 back end on Win32 makes
 * Thread.CurrentThread an inline TLS read (OP_TLS_GET): mov eax, fs:[0x18];
 * and dword [eax+0x34], 0; mov eax, [eax+0xE10+4*key]. The key is
 * current_object_key (0x101F3218), which mono_thread_init gets from TlsAlloc
 * (the call at 0x1007E2B3). The AOT compiler writes the key of its own
 * process into the AOT code. The translated images have the key 9
 * ([eax+0xE34]; for example CultureInfo.get_CurrentCulture at 0x20145DEE in
 * mscorlib.dll.dll, and the images of System, System.Core, System.Data,
 * Mono.Posix and Assembly-CSharp). In another process, TlsAlloc can give
 * Mono another index; then the AOT code reads an empty slot, and each use of
 * Thread.CurrentThread throws NullReferenceException. Thus
 * rt_mono_dll_loaded reserves index 9 for that TlsAlloc call in each host.
 *
 * AOT instance assembly: in AOT-only mode, Mono finds a generic instance in
 * the AOT image of each loaded assembly. PortAotInstances.dll
 * (tools/gen_aot_instances.cs) has the instances that the AOT compiler did
 * not find in the game's assemblies. The game does not load that assembly,
 * so the wrappers rt_mono_jit_init_version and rt_mono_jit_init call the
 * function of mono.dll, and then open the assembly in the new domain with
 * mono_domain_assembly_open. Mono then loads its AOT image. The player
 * gets the two functions with GetProcAddress; with
 * rt_mono_use_aot_instances, GetProcAddress gives the wrappers. If the
 * assembly cannot be opened, the process stops.
 *
 * P/Invoke wrappers with SetLastError: on Win32, the native wrapper of a
 * P/Invoke method with SetLastError=true calls GetLastError at once after
 * the native function, through a raw function pointer. In
 * mono_marshal_get_native_wrapper, 0x1004822F pushes the contents of the
 * import address table slot of GetLastError (0x101310FC), and 0x10048239
 * calls mono_mb_emit_native_call, which emits "mono_ldptr <pointer>;
 * calli". The JIT front end (CEE_MONO_LDPTR) puts a raw pointer into the
 * code as a constant and disables AOT for the method (0x100DE37B:
 * or dword [edi+0x180], 1, cfg->disable_aot). Thus the AOT compiler
 * compiled no such wrapper, and the player stopped with
 * ExecutionEngineException. The address of the GetLastError thunk is
 * different in each host, so it cannot be a constant of AOT code. Two
 * entries of work/mono/instruction_replacements.sci change this:
 *   - The call at 0x10048239 calls rt_mono_emit_get_last_error. It emits
 *     "mono_ldptr 0x101310FC; ldind.i; calli": the wrapper reads the slot
 *     when it runs, as "call [0x101310FC]" does. mono.dll is at its image
 *     base in each host, so the slot address is the same in the AOT
 *     compiler and in the player.
 *   - 0x100DE37B calls rt_mono_ldptr_disable_aot. It disables AOT as before,
 *     except for the pointer 0x101310FC.
 *
 * Method access of the AOT instance assembly: PortAotInstances.dll
 * (tools/gen_aot_instances.cs) calls generic method instances so that the
 * AOT compiler compiles them. Some of them are private methods of
 * mscorlib (MonoProperty.GetterAdapterFrame<T,R>, which
 * PropertyInfo.GetValue calls through reflection). mono_method_to_ir checks
 * the access of each called method with mono_method_can_access_method
 * (0x10016A5F) and fails the compilation of the calling method with
 * MONO_EXCEPTION_METHOD_ACCESS. An entry of
 * work/mono/instruction_replacements.sci replaces the first instruction of
 * mono_method_can_access_method with a call of rt_mono_can_access_method:
 * it returns TRUE when the calling method is in the image PortAotInstances,
 * and runs the check of mono.dll for all other methods. Nothing calls the
 * code of PortAotInstances.dll when the game runs.
 *
 * Native wrappers of generic internal calls: an internal call with type
 * parameters (Interlocked.Exchange<T>, Interlocked.CompareExchange<T>) needs
 * a managed-to-native wrapper for each instance. In AOT-only mode, Mono
 * finds the wrapper of the exact instance in an AOT image; the game's call
 * of Interlocked.Exchange<List`1<Console/ConsoleMessage>> stopped with
 * ExecutionEngineException. The AOT compiler adds these wrappers in the
 * MONO_PATCH_INFO_METHOD case of the patch loop of compile_method
 * (aot-compiler.c): for an inflated method that is an internal call, it
 * adds mono_marshal_get_native_wrapper (m, TRUE, TRUE) to the extra methods
 * (0x10093660-0x10093686). Before that, it skips each method that generic
 * sharing compiles once for all reference types
 * (mono_class_generic_sharing_enabled at 0x10016B0D, then
 * mono_method_is_generic_sharable_impl at 0x10109198). That is correct for
 * methods with IL, but not for internal calls: the player asks for the
 * wrapper of the exact instance, not a shared one. So for a type argument
 * of a reference type, the AOT compiler made no wrapper. An entry of
 * work/mono/instruction_replacements.sci replaces "push dword [edi+8]" at
 * 0x1009363A (the argument of mono_class_generic_sharing_enabled) with a
 * call of rt_mono_aot_icall_wrapper. For an internal call (edi is the
 * method), it continues at 0x10093655 (the check for type variables), so
 * the AOT compiler adds the wrapper. For all other methods, it pushes the
 * argument and continues at 0x1009363D, as before.
 */
#include "rt.h"
#include "rt_code.h"
#include "rt_module.h"
#include "rt_thread.h"

#include <stdio.h>
#include <string.h>

/* The cpuid helper of the translated code (SR llasm support). */
extern void x86_cpuid(_cpu *cpu);
/* The runtime procedure of calls.llasm. */
extern char rt_mono_cpuid_asm2c[];

#define CPUID_IMPL_ADDR 0x101BCDDCu
/* mono_win32_get_handle_stackoverflow and its static variable (in the
   zero-filled part of .data). */
#define GET_HANDLE_STACKOVERFLOW_ADDR 0x100F84ABu
#define HANDLE_STACKOVERFLOW_VAR 0x101F5F10u

/* The start of mono_win32_get_handle_stackoverflow: mov eax, [var];
   test eax, eax; jnz return; push esi; push 128;
   call mono_global_codeman_reserve. */
static const uint8_t get_handle_stackoverflow[24] = {
    0xa1, 0x10, 0x5f, 0x1f, 0x10, 0x85, 0xc0, 0x0f, 0x85, 0xa4, 0x00, 0x00,
    0x00, 0x56, 0x68, 0x80, 0x00, 0x00, 0x00, 0xe8, 0x46, 0x23, 0xff, 0xff,
};

/* The runtime procedure of calls.llasm. */
extern char rt_mono_stack_overflow_asm2c[];

/* mono_aot_only (in .data, initial value 0) and the instruction of
   mono_main that sets it for --full-aot. */
#define MONO_AOT_ONLY_VAR 0x101F5DD4u
#define MONO_MAIN_FULL_AOT_ADDR 0x100B0F7Cu
static const uint8_t mono_main_full_aot[10] = { 0xc7, 0x05, 0xd4, 0x5d, 0x1f, 0x10, 0x01, 0x00, 0x00, 0x00 };

#define THREAD_INIT_TLSALLOC_ADDR 0x1007E2B3u
#define THREAD_INIT_TLSALLOC_RET 0x1007E2B9u
#define MONO_THREAD_TLS_INDEX 9u
/* call dword [TlsAlloc]; mov [current_object_key 0x101F3218], eax */
static const uint8_t thread_init_tlsalloc[11] = { 0xff, 0x15, 0x88, 0x11, 0x13, 0x10, 0xa3, 0x18, 0x32, 0x1f, 0x10 };

static const uint8_t cpuid_impl[34] = {
    0x55, 0x89, 0xe5, 0x53, 0x8b, 0x45, 0x08, 0x0f, 0xa2, 0x50, 0x8b, 0x45, 0x10, 0x89, 0x18, 0x8b, 0x45,
    0x14, 0x89, 0x08, 0x8b, 0x45, 0x18, 0x89, 0x10, 0x58, 0x8b, 0x55, 0x0c, 0x89, 0x02, 0x5b, 0xc9, 0xc3,
};

/* The effect of cpuid_impl. The return address is at [esp], the
   arguments follow it. Registers after the code: eax and ecx are the
   cpuid results, edx is p_eax, ebx and ebp are unchanged; no flags
   change. */
void rt_mono_cpuid(_cpu *cpu)
{
    uint32_t id = rd32(esp + 4), p_eax = rd32(esp + 8), p_ebx = rd32(esp + 12), p_ecx = rd32(esp + 16),
             p_edx = rd32(esp + 20), saved_ebx = cpu->_ebx;
    cpu->_eax = id;
    x86_cpuid(cpu);
    wr32(p_ebx, cpu->_ebx);
    wr32(p_ecx, cpu->_ecx);
    wr32(p_edx, cpu->_edx);
    wr32(p_eax, cpu->_eax);
    cpu->_edx = p_eax;
    cpu->_ebx = saved_ebx;
}

/* The stack overflow helper of Mono: __cdecl void (struct sigcontext *). */
_Noreturn void rt_mono_stack_overflow(_cpu *cpu);
_Noreturn void rt_mono_stack_overflow(_cpu *cpu)
{
    rt_fatal("mono.dll: the stack overflow helper was called (context 0x%08x); the runtime does not support "
             "guest stack overflow exceptions",
             rd32(esp + 4));
}

/* Exports of mono.dll (__cdecl). */
#define MONO_DOMAIN_GET_ADDR 0x10027BB8u
#define MONO_OBJECT_NEW_ADDR 0x10060E76u

/* Layouts that mono_custom_attrs_get_attr (0x10070D22) and
   mono_custom_attrs_construct (0x10070B70) of mono.dll use.
   MonoCustomAttrInfo: num_attrs, cached, image, then the entries.
   MonoCustomAttrEntry: ctor, data_size, data. */
#define CATTR_INFO_NUM_ATTRS 0u
#define CATTR_INFO_ATTRS 12u
#define CATTR_ENTRY_SIZE 12u
#define CATTR_ENTRY_CTOR 0u
#define CATTR_ENTRY_DATA_SIZE 4u
#define CATTR_ENTRY_DATA 8u
/* MonoMethod.klass, MonoClass.supertypes and MonoClass.idepth (16 bits). */
#define METHOD_KLASS 8u
#define CLASS_SUPERTYPES 8u
#define CLASS_IDEPTH 12u
/* cominterop_get_com_slot_begin reads intType at offset 8 of the
   attribute object (after the 8-byte MonoObject header). */
#define INTERFACE_TYPE_ATTR_INT_TYPE 8u
/* The number of attributes is a metadata row count (24 bits at most). */
#define CATTR_MAX_ATTRS 0x01000000u

/* mono_class_has_parent of mono.dll (the inline form in
   mono_custom_attrs_get_attr). */
static bool class_has_parent(uint32_t klass, uint32_t parent)
{
    uint32_t pdepth = rd16(parent + CLASS_IDEPTH);
    if (rd16(klass + CLASS_IDEPTH) < pdepth) return false;
    if (pdepth == 0) rt_fatal("mono.dll: class 0x%08x has supertype depth 0", parent);
    return rd32(rd32(klass + CLASS_SUPERTYPES) + 4u * (pdepth - 1u)) == parent;
}

/* Replacement of the call mono_custom_attrs_get_attr (cinfo, attr_klass)
   in cominterop_get_com_slot_begin. __cdecl; the return address is at
   [esp], cinfo at [esp+4] and attr_klass (InterfaceTypeAttribute) at
   [esp+8]. Returns the attribute object in eax, or 0 if the interface has
   no InterfaceTypeAttribute. */
void rt_mono_interface_type_attr(_cpu *cpu);
void rt_mono_interface_type_attr(_cpu *cpu)
{
    uint32_t cinfo = rd32(esp + 4), attr_klass = rd32(esp + 8), n, i, entry = 0, ctor, klass, size, data, nnamed,
             domain, obj;
    int32_t value;
    const uint8_t *b;

    if (cinfo == 0 || attr_klass == 0) rt_fatal("mono.dll: InterfaceTypeAttribute lookup without attributes or class");
    n = rd32(cinfo + CATTR_INFO_NUM_ATTRS);
    if (n > CATTR_MAX_ATTRS) rt_fatal("mono.dll: custom attribute count %u is not valid", n);
    for (i = 0; i < n; i++)
    {
        uint32_t e = cinfo + CATTR_INFO_ATTRS + CATTR_ENTRY_SIZE * i;
        ctor = rd32(e + CATTR_ENTRY_CTOR);
        /* mono_custom_attrs_get_attr reads ctor->klass without a check. */
        if (ctor == 0) rt_fatal("mono.dll: custom attribute %u of 0x%08x has no constructor", i, cinfo);
        if (class_has_parent(rd32(ctor + METHOD_KLASS), attr_klass))
        {
            entry = e;
            break;
        }
    }
    if (entry == 0)
    {
        cpu->_eax = 0;
        return;
    }
    ctor = rd32(entry + CATTR_ENTRY_CTOR);
    klass = rd32(ctor + METHOD_KLASS);
    if (klass != attr_klass)
        rt_fatal("mono.dll: attribute class 0x%08x is derived from InterfaceTypeAttribute, which is sealed", klass);
    size = rd32(entry + CATTR_ENTRY_DATA_SIZE);
    data = rd32(entry + CATTR_ENTRY_DATA);
    /* Blob: prolog 0x0001, the fixed argument (int32 ComInterfaceType or
       int16), the number of named arguments (16 bits). */
    if ((size != 6u && size != 8u) || !rt_va_is_committed(data, size))
        rt_fatal("mono.dll: InterfaceTypeAttribute blob at 0x%08x has %u bytes", data, size);
    b = G2H(data);
    if (b[0] != 0x01 || b[1] != 0x00) rt_fatal("mono.dll: InterfaceTypeAttribute blob at 0x%08x has no prolog", data);
    if (size == 8u)
    {
        value = (int32_t)((uint32_t)b[2] | (uint32_t)b[3] << 8 | (uint32_t)b[4] << 16 | (uint32_t)b[5] << 24);
        nnamed = (uint32_t)b[6] | (uint32_t)b[7] << 8;
    }
    else
    {
        value = (int16_t)(uint16_t)((uint16_t)b[2] | (uint16_t)b[3] << 8);
        nnamed = (uint32_t)b[4] | (uint32_t)b[5] << 8;
    }
    /* InterfaceTypeAttribute has no fields or properties that a named
       argument can set. */
    if (nnamed != 0) rt_fatal("mono.dll: InterfaceTypeAttribute blob at 0x%08x has %u named arguments", data, nnamed);
    domain = rt_call_cdecl(cpu, MONO_DOMAIN_GET_ADDR, 0, 0);
    if (domain == 0) rt_fatal("mono.dll: mono_domain_get returned no domain");
    obj = rt_call_cdecl(cpu, MONO_OBJECT_NEW_ADDR, 2, domain, klass);
    if (obj == 0) rt_fatal("mono.dll: mono_object_new of InterfaceTypeAttribute failed");
    wr32(obj + INTERFACE_TYPE_ATTR_INT_TYPE, (uint32_t)value);
    cpu->_eax = obj;
}

/* The import address table slot of GetLastError, the code that pushes its
   contents and calls mono_mb_emit_native_call in
   mono_marshal_get_native_wrapper, and the store of
   cfg->disable_aot of CEE_MONO_LDPTR in mono_method_to_ir (see the comment
   at the top). The replaced instructions are checked in the mapped image. */
#define GET_LAST_ERROR_SLOT 0x101310FCu
#define NATIVE_WRAPPER_PUSH_ADDR 0x1004822Fu
/* mono_mb_emit_ptr, mono_mb_emit_calli and mono_mb_emit_byte (static
   functions of method-builder.c; mono_mb_emit_native_call at 0x1005727E
   calls the first two). */
#define MB_EMIT_PTR_ADDR 0x10057232u
#define MB_EMIT_CALLI_ADDR 0x10057254u
#define MB_EMIT_BYTE_ADDR 0x10056E71u
#define CEE_LDIND_I 0x4Du
#define LDPTR_DISABLE_AOT_ADDR 0x100DE37Bu
#define CFG_DISABLE_AOT_OFFSET 0x180u
#define LDPTR_PTR_EBP_OFFSET 0x10Cu
/* push dword [0x101310FC]; push eax; push dword [ebp+0xC];
   call mono_mb_emit_native_call */
static const uint8_t native_wrapper_push[15] = { 0xff, 0x35, 0xfc, 0x10, 0x13, 0x10, 0x50, 0xff, 0x75, 0x0c,
                                                 0xe8, 0x40, 0xf0, 0x00, 0x00 };
static const uint8_t ldptr_disable_aot[7] = { 0x83, 0x8f, 0x80, 0x01, 0x00, 0x00, 0x01 };

/* mono_method_can_access_method (method, called), a __cdecl function. Its
   first instruction is push ebp; the next instruction (mov ebp, esp) is at
   0x10016A60. The accessors mono_method_get_class, mono_class_get_image and
   mono_image_get_name give the field offsets: MonoMethod.klass at 8,
   MonoClass.image at 0x2C, MonoImage.assembly_name at 0x18. */
#define CAN_ACCESS_METHOD_ADDR 0x10016A5Fu
#define CAN_ACCESS_METHOD_NEXT 0x10016A60u
#define MONO_METHOD_GET_CLASS_ADDR 0x100165C7u
#define MONO_CLASS_GET_IMAGE_ADDR 0x100164EAu
#define MONO_IMAGE_GET_NAME_ADDR 0x100165D2u
#define METHOD_KLASS_OFFSET 0x8u
#define CLASS_IMAGE_OFFSET 0x2Cu
#define IMAGE_ASSEMBLY_NAME_OFFSET 0x18u
#define AOT_INSTANCES_IMAGE "PortAotInstances"
/* push ebp; mov ebp, esp; push esi; mov esi, [ebp+0xC] */
static const uint8_t can_access_method[7] = { 0x55, 0x8b, 0xec, 0x56, 0x8b, 0x75, 0x0c };
/* push ebp; mov ebp, esp; mov eax, [ebp+8]; mov eax, [eax+<offset>]; pop ebp */
static const uint8_t accessor_get_class[10] = { 0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x8b, 0x40, 0x08, 0x5d };
static const uint8_t accessor_get_image[10] = { 0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x8b, 0x40, 0x2c, 0x5d };
static const uint8_t accessor_get_name[10] = { 0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x8b, 0x40, 0x18, 0x5d };

/* The MONO_PATCH_INFO_METHOD case of compile_method in the AOT compiler
   (see the comment at the top), from "test dword [edi+0x14], 0x800"
   (is_inflated) to "test word [edi+2], 0x1000" and its jump. */
#define PATCH_METHOD_CHECK_ADDR 0x10093631u
#define PATCH_METHOD_SHARING_NEXT 0x1009363Du
#define PATCH_METHOD_TYPE_VARS 0x10093655u
#define METHOD_IFLAGS_OFFSET 0x2u
#define METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL 0x1000u
static const uint8_t patch_method_check[58] = {
    0xf7, 0x47, 0x14, 0x00, 0x08, 0x00, 0x00, /* test dword [edi+0x14], 0x800 */
    0x74, 0x70,                               /* je 0x100936AA */
    0xff, 0x77, 0x08,                         /* push dword [edi+8] (replaced) */
    0xe8, 0xcb, 0x34, 0xf8, 0xff,             /* call mono_class_generic_sharing_enabled */
    0x59, 0x85, 0xc0, 0x74, 0x0e,             /* pop ecx; test eax, eax; je 0x10093655 */
    0x6a, 0x00, 0x57,                         /* push 0; push edi */
    0xe8, 0x49, 0x5b, 0x07, 0x00,             /* call mono_method_is_generic_sharable_impl */
    0x59, 0x59, 0x85, 0xc0, 0x75, 0x42,       /* pop ecx; pop ecx; test eax, eax; jne 0x10093697 */
    0x8b, 0xc7,                               /* mov eax, edi */
    0xe8, 0xa3, 0xb3, 0xff, 0xff,             /* call method_has_type_vars */
    0x85, 0xc0, 0x75, 0x37,                   /* test eax, eax; jne 0x10093697 */
    0xb8, 0x00, 0x10, 0x00, 0x00,             /* mov eax, 0x1000 */
    0x66, 0x85, 0x47, 0x02,                   /* test word [edi+2], ax */
    0x74, 0x1d,                               /* je 0x10093688 */
};

static void check_can_access_method(void)
{
    if (memcmp(G2H(CAN_ACCESS_METHOD_ADDR), can_access_method, sizeof(can_access_method)) != 0 ||
        memcmp(G2H(MONO_METHOD_GET_CLASS_ADDR), accessor_get_class, sizeof(accessor_get_class)) != 0 ||
        memcmp(G2H(MONO_CLASS_GET_IMAGE_ADDR), accessor_get_image, sizeof(accessor_get_image)) != 0 ||
        memcmp(G2H(MONO_IMAGE_GET_NAME_ADDR), accessor_get_name, sizeof(accessor_get_name)) != 0)
        rt_fatal("mono.dll: mono_method_can_access_method or its accessors are not at 0x%08x", CAN_ACCESS_METHOD_ADDR);
}

void rt_mono_dll_loaded(uint32_t base)
{
    if (base != 0x10000000u) rt_fatal("mono.dll is not at its image base");
    if (memcmp(G2H(CPUID_IMPL_ADDR), cpuid_impl, sizeof(cpuid_impl)) != 0)
        rt_fatal("mono.dll: cpuid_impl is not at 0x%08x", CPUID_IMPL_ADDR);
    rt_code_register_copy("mono.dll cpuid_impl", CPUID_IMPL_ADDR, sizeof(cpuid_impl), H2G(rt_mono_cpuid_asm2c));
    if (memcmp(G2H(GET_HANDLE_STACKOVERFLOW_ADDR), get_handle_stackoverflow, sizeof(get_handle_stackoverflow)) != 0)
        rt_fatal("mono.dll: mono_win32_get_handle_stackoverflow is not at 0x%08x", GET_HANDLE_STACKOVERFLOW_ADDR);
    if (rd32(HANDLE_STACKOVERFLOW_VAR) != 0)
        rt_fatal("mono.dll: the stack overflow helper variable at 0x%08x is not zero", HANDLE_STACKOVERFLOW_VAR);
    wr32(HANDLE_STACKOVERFLOW_VAR, H2G(rt_mono_stack_overflow_asm2c));
    rt_log("mono.dll stack overflow helper: rt_mono_stack_overflow");
    if (memcmp(G2H(THREAD_INIT_TLSALLOC_ADDR), thread_init_tlsalloc, sizeof(thread_init_tlsalloc)) != 0)
        rt_fatal("mono.dll: the TlsAlloc call of mono_thread_init is not at 0x%08x", THREAD_INIT_TLSALLOC_ADDR);
    rt_tls_reserve_for_call(THREAD_INIT_TLSALLOC_RET, MONO_THREAD_TLS_INDEX);
    rt_log("mono.dll: TLS index %u is reserved for the current thread key of mono_thread_init", MONO_THREAD_TLS_INDEX);
    if (memcmp(G2H(NATIVE_WRAPPER_PUSH_ADDR), native_wrapper_push, sizeof(native_wrapper_push)) != 0 ||
        memcmp(G2H(LDPTR_DISABLE_AOT_ADDR), ldptr_disable_aot, sizeof(ldptr_disable_aot)) != 0)
        rt_fatal("mono.dll: the GetLastError code of the native wrappers is not at 0x%08x and 0x%08x",
                 NATIVE_WRAPPER_PUSH_ADDR, LDPTR_DISABLE_AOT_ADDR);
    rt_log("mono.dll: the native wrappers read GetLastError from its import slot 0x%08x", GET_LAST_ERROR_SLOT);
    check_can_access_method();
    if (memcmp(G2H(PATCH_METHOD_CHECK_ADDR), patch_method_check, sizeof(patch_method_check)) != 0)
        rt_fatal("mono.dll: the MONO_PATCH_INFO_METHOD case of the AOT compiler is not at 0x%08x", PATCH_METHOD_CHECK_ADDR);
}

/* Replacement of "or dword [edi+0x180], 1" at 0x100DE37B: edi is the
   MonoCompile, and the pointer of the CEE_MONO_LDPTR instruction is at
   [ebp-0x10C]. The next instruction is a jump to code that sets the flags
   again, so the flags of the or instruction are not used. */
/* Replacement of "call mono_mb_emit_native_call" at 0x10048239, a __cdecl
   call (mb, sig, func). func is the contents of the GetLastError slot. */
void rt_mono_emit_get_last_error(_cpu *cpu);
void rt_mono_emit_get_last_error(_cpu *cpu)
{
    uint32_t mb = rd32(esp + 4), sig = rd32(esp + 8), func = rd32(esp + 12);
    if (func != rd32(GET_LAST_ERROR_SLOT))
        rt_fatal("mono.dll: the native wrapper calls 0x%08x, not GetLastError", func);
    rt_call_cdecl(cpu, MB_EMIT_PTR_ADDR, 2, mb, GET_LAST_ERROR_SLOT);
    rt_call_cdecl(cpu, MB_EMIT_BYTE_ADDR, 2, mb, CEE_LDIND_I);
    rt_call_cdecl(cpu, MB_EMIT_CALLI_ADDR, 2, mb, sig);
}

void rt_mono_ldptr_disable_aot(_cpu *cpu);
void rt_mono_ldptr_disable_aot(_cpu *cpu)
{
    uint32_t cfg = cpu->_edi, ptr = rd32(cpu->_ebp - LDPTR_PTR_EBP_OFFSET);
    if (ptr != GET_LAST_ERROR_SLOT) wr32(cfg + CFG_DISABLE_AOT_OFFSET, rd32(cfg + CFG_DISABLE_AOT_OFFSET) | 1u);
}

/* Replacement of "push ebp" at the start of mono_method_can_access_method.
   At the call: [esp] is the address that the replacement pushed, [esp+4]
   the return address, [esp+8] the calling method. */
void rt_mono_can_access_method(_cpu *cpu);
void rt_mono_can_access_method(_cpu *cpu)
{
    uint32_t e = cpu->_esp, method = rd32(e + 8), klass, image, name = 0, i;
    if (method != 0 && (klass = rd32(method + METHOD_KLASS_OFFSET)) != 0 &&
        (image = rd32(klass + CLASS_IMAGE_OFFSET)) != 0)
        name = rd32(image + IMAGE_ASSEMBLY_NAME_OFFSET);
    if (name != 0)
    {
        /* Compare with the terminating zero; stop at the first different
           byte, so no byte after the end of a shorter name is read. */
        for (i = 0; i < sizeof AOT_INSTANCES_IMAGE; i++)
            if (rd8(name + i) != (uint8_t)AOT_INSTANCES_IMAGE[i]) break;
        if (i == sizeof AOT_INSTANCES_IMAGE)
        {
            /* Return TRUE to the caller. */
            cpu->_eax = 1;
            cpu->_esp = e + 4;
            return;
        }
    }
    /* push ebp, then continue at the next instruction. */
    wr32(e - 4, CAN_ACCESS_METHOD_NEXT);
    wr32(e, cpu->_ebp);
    cpu->_esp = e - 4;
}

/* Replacement of "push dword [edi+8]" at 0x1009363A. At the call, [esp]
   is the address that the replacement pushed (0x1009363D). */
void rt_mono_aot_icall_wrapper(_cpu *cpu);
void rt_mono_aot_icall_wrapper(_cpu *cpu)
{
    uint32_t e = cpu->_esp, method = cpu->_edi;
    if (rd16(method + METHOD_IFLAGS_OFFSET) & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)
    {
        /* An internal call: no sharing check; continue at the check for
           type variables. */
        wr32(e, PATCH_METHOD_TYPE_VARS);
        return;
    }
    /* push dword [edi+8], then continue at the call. */
    wr32(e - 4, PATCH_METHOD_SHARING_NEXT);
    wr32(e, rd32(method + METHOD_KLASS_OFFSET));
    cpu->_esp = e - 4;
}

extern char rt_mono_jit_init_version_asm2c[];
extern char rt_mono_jit_init_asm2c[];

static uint32_t mono_jit_init_version_addr, mono_jit_init_addr, mono_domain_assembly_open_addr;
static char aot_instances_path[260];
static bool aot_instances_loaded;

/* Open the AOT instance assembly in domain (0: the Mono function failed,
   and the program reports that). */
static void load_aot_instances(_cpu *cpu, uint32_t domain)
{
    uint32_t path, assembly;
    size_t n = strlen(aot_instances_path) + 1;
    if (domain == 0) return;
    if (aot_instances_loaded) rt_fatal("mono.dll: the program starts Mono a second time");
    path = rt_heap_alloc((uint32_t)n);
    if (path == 0) rt_fatal("out of guest memory (AOT instance assembly path)");
    memcpy(G2H(path), aot_instances_path, n);
    assembly = rt_call_cdecl(cpu, mono_domain_assembly_open_addr, 2, domain, path);
    rt_heap_free(path);
    if (assembly == 0) rt_fatal("mono.dll: the AOT instance assembly %s cannot be opened", aot_instances_path);
    aot_instances_loaded = true;
    rt_log("mono.dll: AOT instance assembly %s is loaded", aot_instances_path);
}

/* MonoDomain *mono_jit_init_version (const char *name, const char *version) */
void rt_mono_jit_init_version(_cpu *cpu);
void rt_mono_jit_init_version(_cpu *cpu)
{
    uint32_t name = rd32(esp + 4), version = rd32(esp + 8);
    uint32_t domain = rt_call_cdecl(cpu, mono_jit_init_version_addr, 2, name, version);
    load_aot_instances(cpu, domain);
    cpu->_eax = domain;
}

/* MonoDomain *mono_jit_init (const char *file) */
void rt_mono_jit_init(_cpu *cpu);
void rt_mono_jit_init(_cpu *cpu)
{
    uint32_t file = rd32(esp + 4);
    uint32_t domain = rt_call_cdecl(cpu, mono_jit_init_addr, 1, file);
    load_aot_instances(cpu, domain);
    cpu->_eax = domain;
}

void rt_mono_use_aot_instances(uint32_t base, const char *guest_path)
{
    if (base != 0x10000000u) rt_fatal("mono.dll is not at its image base");
    if ((size_t)snprintf(aot_instances_path, sizeof(aot_instances_path), "%s", guest_path) >= sizeof(aot_instances_path))
        rt_fatal("the AOT instance assembly path %s is too long", guest_path);
    mono_jit_init_version_addr = rt_module_export(base, "mono_jit_init_version");
    mono_jit_init_addr = rt_module_export(base, "mono_jit_init");
    mono_domain_assembly_open_addr = rt_module_export(base, "mono_domain_assembly_open");
    rt_module_override_export(base, "mono_jit_init_version", H2G(rt_mono_jit_init_version_asm2c));
    rt_module_override_export(base, "mono_jit_init", H2G(rt_mono_jit_init_asm2c));
}

void rt_mono_set_aot_only(uint32_t base)
{
    if (base != 0x10000000u) rt_fatal("mono.dll is not at its image base");
    if (memcmp(G2H(MONO_MAIN_FULL_AOT_ADDR), mono_main_full_aot, sizeof(mono_main_full_aot)) != 0)
        rt_fatal("mono.dll: the --full-aot store of mono_main is not at 0x%08x", MONO_MAIN_FULL_AOT_ADDR);
    if (rd32(MONO_AOT_ONLY_VAR) != 0) rt_fatal("mono.dll: mono_aot_only at 0x%08x is not zero", MONO_AOT_ONLY_VAR);
    wr32(MONO_AOT_ONLY_VAR, 1);
    rt_log("mono.dll: AOT-only mode");
    /* BinaryFormatter (the save games of Persistent.BinarySerialize) makes
       a TypeMetadata class for each serialized type with Reflection.Emit
       (CodeGenerator.GenerateMetadataType). The JIT must compile that
       class, so the call stops in AOT-only mode. BinaryCommon..cctor reads
       MONO_REFLECTION_SERIALIZER with GetEnvironmentVariableW; with "yes",
       ObjectWriter.CreateMemberTypeMetadata uses MemberTypeMetadata, which
       reads the fields with reflection and needs no JIT. */
    rt_env_set("MONO_REFLECTION_SERIALIZER", "yes");
}

/* ---------------------------------------------------------------- port graphics rows */

/* The managed half of the port's rows on the Graphics tab
   (tools/port_graphics/PortGraphics.cs): rt_mono_port_tick calls the static
   method PortGraphics.GraphicsOptions.Tick once after each presented frame
   (rt_portgfx_pump). The player runs the game's scripts in the domain of
   mono_jit_init_version ("Unity Root Domain"; the text "Unity Child Domain"
   of PillarsOfEternity.exe is only part of a log message, and the player
   calls no mono_domain_create_appdomain). The rows refer to types of
   Assembly-CSharp, so the first tick after Mono has loaded Assembly-CSharp
   (mono_image_loaded) opens the assembly in the current domain of the main
   thread. Before that, the tick does nothing. A later change of the current
   domain stops the process: the rows would refer to the scripts of the old
   domain. Tick catches the exceptions of the managed code and reports them
   with PortGfxFail (rt_portgfx.c); an exception that still reaches
   mono_runtime_invoke stops the process with its class name. */
static char port_graphics_path[260];
static uint32_t pg_domain_get, pg_image_loaded, pg_assembly_open, pg_assembly_image, pg_class_from_name,
    pg_method_from_name, pg_runtime_invoke, pg_object_class, pg_class_name, pg_class_namespace;
static uint32_t pg_scripts_name, pg_domain, pg_method, pg_exc;

void rt_mono_use_port_graphics(uint32_t base, const char *guest_path)
{
    if (base != 0x10000000u) rt_fatal("mono.dll is not at its image base");
    if ((size_t)snprintf(port_graphics_path, sizeof(port_graphics_path), "%s", guest_path) >= sizeof(port_graphics_path))
        rt_fatal("the port graphics assembly path %s is too long", guest_path);
    pg_domain_get = rt_module_export(base, "mono_domain_get");
    pg_image_loaded = rt_module_export(base, "mono_image_loaded");
    pg_assembly_open = rt_module_export(base, "mono_domain_assembly_open");
    pg_assembly_image = rt_module_export(base, "mono_assembly_get_image");
    pg_class_from_name = rt_module_export(base, "mono_class_from_name");
    pg_method_from_name = rt_module_export(base, "mono_class_get_method_from_name");
    pg_runtime_invoke = rt_module_export(base, "mono_runtime_invoke");
    pg_object_class = rt_module_export(base, "mono_object_get_class");
    pg_class_name = rt_module_export(base, "mono_class_get_name");
    pg_class_namespace = rt_module_export(base, "mono_class_get_namespace");
}

/* A guest copy of the host string s (rt_heap_free it). */
static uint32_t guest_string(const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t g = rt_heap_alloc((uint32_t)n);
    if (g == 0) rt_fatal("out of guest memory (Mono name)");
    memcpy(G2H(g), s, n);
    return g;
}

static void port_graphics_open(_cpu *cpu, uint32_t domain)
{
    uint32_t path = guest_string(port_graphics_path), ns = guest_string("PortGraphics"),
             name = guest_string("GraphicsOptions"), tick = guest_string("Tick");
    uint32_t assembly, image, klass;
    assembly = rt_call_cdecl(cpu, pg_assembly_open, 2, domain, path);
    if (assembly == 0) rt_fatal("mono.dll: the port graphics assembly %s cannot be opened", port_graphics_path);
    image = rt_call_cdecl(cpu, pg_assembly_image, 1, assembly);
    klass = image != 0 ? rt_call_cdecl(cpu, pg_class_from_name, 3, image, ns, name) : 0;
    if (klass == 0) rt_fatal("mono.dll: %s has no class PortGraphics.GraphicsOptions", port_graphics_path);
    pg_method = rt_call_cdecl(cpu, pg_method_from_name, 3, klass, tick, 0);
    if (pg_method == 0) rt_fatal("mono.dll: PortGraphics.GraphicsOptions has no method Tick()");
    pg_exc = rt_heap_alloc(4);
    if (pg_exc == 0) rt_fatal("out of guest memory (Mono exception slot)");
    rt_heap_free(path);
    rt_heap_free(ns);
    rt_heap_free(name);
    rt_heap_free(tick);
    pg_domain = domain;
    rt_log("mono.dll: port graphics assembly %s is loaded in domain 0x%08x", port_graphics_path, domain);
}

void rt_mono_port_tick(_cpu *cpu)
{
    uint32_t domain, exc;
    if (pg_domain_get == 0) return;
    domain = rt_call_cdecl0(cpu, pg_domain_get);
    if (pg_domain == 0)
    {
        if (domain == 0) return;
        if (pg_scripts_name == 0) pg_scripts_name = guest_string("Assembly-CSharp");
        if (rt_call_cdecl(cpu, pg_image_loaded, 1, pg_scripts_name) == 0) return;
        rt_heap_free(pg_scripts_name);
        pg_scripts_name = 0;
        port_graphics_open(cpu, domain);
    }
    else if (domain != pg_domain)
        rt_fatal("mono.dll: the current domain changed from 0x%08x to 0x%08x after the port graphics assembly was loaded",
                 pg_domain, domain);
    wr32(pg_exc, 0);
    (void)rt_call_cdecl(cpu, pg_runtime_invoke, 4, pg_method, 0, 0, pg_exc);
    exc = rd32(pg_exc);
    if (exc != 0)
    {
        char ns[128], name[128];
        uint32_t klass = rt_call_cdecl(cpu, pg_object_class, 1, exc);
        if (klass == 0 || rt_gstr(ns, sizeof ns, rt_call_cdecl(cpu, pg_class_namespace, 1, klass)) != 0 ||
            rt_gstr(name, sizeof name, rt_call_cdecl(cpu, pg_class_name, 1, klass)) != 0)
            rt_fatal("PortGraphics.GraphicsOptions.Tick threw an exception (its class name cannot be read)");
        rt_fatal("PortGraphics.GraphicsOptions.Tick threw %s%s%s", ns, ns[0] != 0 ? "." : "", name);
    }
}
