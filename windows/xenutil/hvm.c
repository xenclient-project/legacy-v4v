/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */

#include "ntddk.h"
#include "xsapi.h"
#include "base.h"
#include "verinfo.h"
#include "hypercall.h"
#include "xenutl.h"
#include "hvm.h"

#pragma warning(push)
#pragma warning(disable: 4200)
#include <xen/memory.h>
#include <xen/hvm/params.h>
#include <xen/hvm/hvm_op.h>
#include <xen/sched.h>
#pragma warning(pop)

//
// Dont care about unreferenced formal parameters here
//
#pragma warning( disable : 4100 )

hypercall_trap_gate *hypercall_page;
shared_info_t *HYPERVISOR_shared_info;
int HvmInterruptNumber;

static PHYSICAL_ADDRESS sharedInfoPhysAddr;

ULONG_PTR
HvmGetParameter(int param_nr)
{
    struct xen_hvm_param a;
    LONG_PTR rc;
    a.domid = DOMID_SELF;
    a.index = param_nr;
    a.value = 0xf001dead;
    rc = HYPERVISOR_hvm_op(HVMOP_get_param, &a);
    if (rc < 0) {
            TraceError (("Cannot get HVM parameter %d: %d.\n",
                    param_nr, rc));
        return rc;
    }
    /* Horrible hack to cope with the transition from
       return parameters through the hypercall return
       value to returning them through an in-memory
       structure. */
    if (a.value != 0xf001dead)
        rc = (int)a.value;    
    TraceDebug (("HVM param %d is %d.\n", param_nr, rc));
    return rc;
}

static int
HvmSetParameter(int param_nr, unsigned long value)
{
    struct xen_hvm_param a;
    a.domid = DOMID_SELF;
    a.index = param_nr;
    a.value = value;
    return (int)HYPERVISOR_hvm_op(HVMOP_set_param, &a);
}

int
__HvmSetCallbackIrq(const char *caller, int irq)
{
    int ret;

    TraceNotice(("%s setting callback irq to %d\n", caller, irq));

    HvmInterruptNumber = irq;
    ret = HvmSetParameter(HVM_PARAM_CALLBACK_IRQ, irq);
    return ret;
}

int
AddPageToPhysmap(PFN_NUMBER pfn,
                 unsigned space,
                 unsigned long offset)
{
    struct xen_add_to_physmap xatp;

#ifdef AMD64
    XM_ASSERT3U(pfn >> 32, ==, 0);
#endif

    xatp.domid = DOMID_SELF;
    xatp.size = 0; // not useing GMFN ranges
    xatp.space = space;
    xatp.idx = offset;
    xatp.gpfn = (unsigned long)pfn;

    return HYPERVISOR_memory_op(XENMEM_add_to_physmap, &xatp);
}

static ULONG
GetXenVersion(void)
{
    ULONG eax, ebx, ecx, edx;

    XenCpuid(1, &eax, &ebx, &ecx, &edx);
    return eax;
}

static PVOID
GetHypercallPage(VOID)
{
    ULONG eax, ebx ='fool', ecx = 'beef', edx = 'dead',
        nr_hypercall_pages;
    PVOID res;
    unsigned i;

    if (!CheckXenHypervisor()) {
        TraceError (("cpuid says this isn't really Xen.\n"));
        return NULL;
    }

    XenCpuid(1, &eax, &ebx, &ecx, &edx);
    TraceVerbose (("Xen version %d.%d.\n", eax >> 16, eax & 0xffff));

    //
    // Get the number of hypercall pages and the MSR to use to tell the
    // hypervisor which guest physical pages were assigned.
    //

    XenCpuid(2, &nr_hypercall_pages, &ebx, &ecx, &edx);

    res = XmAllocateMemory(PAGE_SIZE * nr_hypercall_pages);

    if (res == NULL) {
        TraceError (("Cannot allocate %d pages for hypercall trampolines.\n", nr_hypercall_pages));
        return NULL;
    }

    //
    // For each page, get the guest physical address and pass it to 
    // the hypervisor.
    //
    // Note: The low 12 bits of the address is used to pass the index
    // of the page within the hypercall area.
    //

    for (i = 0; i < nr_hypercall_pages; i++)
    {
        PHYSICAL_ADDRESS gpa;

        gpa = MmGetPhysicalAddress(((PCHAR)res) + (i << PAGE_SHIFT));
        _wrmsr(ebx, gpa.LowPart | i, gpa.HighPart);
    }

    return res;
}

static const CHAR *
PlatformIdName(
    IN  ULONG   PlatformId
    )
{
#define _VER_PLATFORM_NAME(_PlatformId)                                     \
        case VER_PLATFORM_ ## _PlatformId:                                  \
            return #_PlatformId;

    switch (PlatformId) {
    _VER_PLATFORM_NAME(WIN32s);
    _VER_PLATFORM_NAME(WIN32_WINDOWS);
    _VER_PLATFORM_NAME(WIN32_NT);
    default:
        break;
    }

    return "UNKNOWN";
#undef  _VER_PLATFORM_NAME
}

static const CHAR *
SuiteName(
    IN  ULONG  SuiteBit
    )
{
#define _VER_SUITE_NAME(_Suite)                                             \
        case VER_SUITE_ ## _Suite:                                          \
            return #_Suite;

    XM_ASSERT(SuiteBit < 16);
    switch (1 << SuiteBit) {
    _VER_SUITE_NAME(SMALLBUSINESS);
    _VER_SUITE_NAME(ENTERPRISE);
    _VER_SUITE_NAME(BACKOFFICE);
    _VER_SUITE_NAME(COMMUNICATIONS);
    _VER_SUITE_NAME(TERMINAL);
    _VER_SUITE_NAME(SMALLBUSINESS_RESTRICTED);
    _VER_SUITE_NAME(EMBEDDEDNT);
    _VER_SUITE_NAME(DATACENTER);
    _VER_SUITE_NAME(SINGLEUSERTS);
    _VER_SUITE_NAME(PERSONAL);
    _VER_SUITE_NAME(BLADE);
    _VER_SUITE_NAME(EMBEDDED_RESTRICTED);
    _VER_SUITE_NAME(SECURITY_APPLIANCE);
    _VER_SUITE_NAME(STORAGE_SERVER);
    _VER_SUITE_NAME(COMPUTE_SERVER);
    default:
        break;
    }

    return "UNKNOWN";
#undef  _VER_SUITE_NAME
}

static const CHAR *
ProductTypeName(
    IN  UCHAR   ProductType
    )
{
#define _VER_NT_NAME(_ProductType)                                          \
        case VER_NT_ ## _ProductType:                                       \
            return #_ProductType;

        switch (ProductType) {
        _VER_NT_NAME(WORKSTATION);
        _VER_NT_NAME(DOMAIN_CONTROLLER);
        _VER_NT_NAME(SERVER);
    default:
        break;
    }

    return "UNKNOWN";
#undef  _VER_NT_NAME
}

/* This is sometimes used for its side-effect of priming the
 * XenutilGetVersionInfo cache. */
static VOID
PrintVersionInformation(
    VOID
    )
{
    RTL_OSVERSIONINFOEXW    Info;
    ULONG                   Bit;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;

    XenutilGetVersionInfo(&Info);

    TraceNotice(("KERNEL: %d.%d (build %d) platform %s\n",
                    Info.dwMajorVersion, Info.dwMinorVersion, Info.dwBuildNumber,
                    PlatformIdName(Info.dwPlatformId)));

    if (Info.wServicePackMajor != 0 || Info.wServicePackMinor != 0) {
        TraceNotice(("SP: %d.%d (%s)\n",
                        Info.wServicePackMajor, Info.wServicePackMinor,
                        Info.szCSDVersion));
    } else {
        TraceNotice(("SP: NONE\n"));
    }

    TraceNotice(("SUITES:\n"));
    Bit = 0;
    while (Info.wSuiteMask != 0) {
        if (Info.wSuiteMask & 0x0001)
            TraceNotice(("- %s\n", SuiteName(Bit)));

        Info.wSuiteMask >>= 1;
        Bit++;
    }

    TraceNotice(("TYPE: %s\n", ProductTypeName(Info.wProductType)));
}

NTSTATUS
HvmResume(VOID *ignore, SUSPEND_TOKEN token)
{
    int ret;

    UNREFERENCED_PARAMETER(ignore);
    UNREFERENCED_PARAMETER(token);

    ret = AddPageToPhysmap((PFN_NUMBER)(sharedInfoPhysAddr.QuadPart >> PAGE_SHIFT),
                           XENMAPSPACE_shared_info,
                           0);
    if (ret != 0) {
        TraceError (("Failed to add shared info to physmap: %d.\n", ret));
        /* XXX error code */
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    TraceInfo (("Mapped shared info.\n"));

    if (HvmInterruptNumber)
        HvmSetParameter(HVM_PARAM_CALLBACK_IRQ, HvmInterruptNumber);

    return STATUS_SUCCESS;
}

#define BUILD_TIMESTAMP(_date, _time)       \
        _date ## "." ## _time

static const CHAR *DriverTimestamp = BUILD_TIMESTAMP(__DATE__, __TIME__);

static void
SetDriverVersion(void)
{
    TraceNotice(("PV DRIVERS: VERSION: %d.%d.%d BUILD: %d (%s)\n",
                 VER_MAJOR_VERSION, 
                 VER_MINOR_VERSION, 
                 VER_MICRO_VERSION, 
                 VER_BUILD_NR,
                 DriverTimestamp));
}

//
// InitHvm - Perform actions required to hook into Xen
//                   HVM interface.
//
NTSTATUS
InitHvm(void)
{
    static struct SuspendHandler *sh;

    TraceVerbose (("InitHvm.\n"));

    /* This call has the side-effect of priming the version
     * XenutilGetVersionInfo cache.  Do not move it around
     * unnecessarily. */

    PrintVersionInformation();

    hypercall_page = GetHypercallPage();
    if (hypercall_page == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    SetDriverVersion();

    //
    // Allocate and map the shared info page.
    //
    HYPERVISOR_shared_info = XenevtchnAllocIoMemory(PAGE_SIZE, &sharedInfoPhysAddr);
    if (!HYPERVISOR_shared_info) {
        TraceError (("Cannot allocate shared info page.\n"));
        hypercall_page = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    TraceInfo(("HYPERVISOR_shared_info at %p (%x:%x)\n",
               HYPERVISOR_shared_info, sharedInfoPhysAddr));

    /* We're too early to need to worry about races with suspend */
    if (!sh)
        sh = EvtchnRegisterSuspendHandler(HvmResume, NULL, "HvmResume",
                                          SUSPEND_CB_EARLY);
    HvmResume(NULL, null_SUSPEND_TOKEN());

    return STATUS_SUCCESS;
}

//
// CleanupHvm - Return resources consumed by driver back to the system.
//
VOID
CleanupHvm(VOID)
{
    if (hypercall_page) {
        //
        // Clear registered hvm callback.
        //
        HvmSetCallbackIrq(0);
    }
}

static int
GetXenTime(xen_hvm_get_time_t *gxt)
{
    return (int)HYPERVISOR_hvm_op(HVMOP_get_time, gxt);
}

/* Get Xen's idea of the current time, in hundreds of nanoseconds
 * since 1601. */
ULONG64
HvmGetXenTime(void)
{
    xen_hvm_get_time_t gxt;
    uint32_t version, wc_nsec;
    uint64_t wc_sec;
    uint64_t system_time;
    uint64_t result;

    /* Read wc_sec, wc_nsec.  These give the UTC time at which Xen was
     * booted. */
    do {
        version = HYPERVISOR_shared_info->wc_version;
        XsMemoryBarrier();
        wc_sec = HYPERVISOR_shared_info->wc_sec;
        wc_nsec = HYPERVISOR_shared_info->wc_nsec;
        XsMemoryBarrier();
    } while (version != HYPERVISOR_shared_info->wc_version);

    TraceInfo(("GetXenTime: wc_sec %I64x, nsec %x.\n",
               wc_sec, wc_nsec));
    /* Convert from unix epoch (1970) to Windows epoch (1601) */
    wc_sec += 11644473600ull;

    /* wc_sec + wc_nsec*1e-9 now gives the time at which the system
       was booted, relative to 1601.  Find out the ``system time''
       (i.e. the number of nanoseconds since we booted). */
    if (!XenPVFeatureEnabled(DEBUG_MTC_PROTECTED_VM) && (GetXenTime(&gxt) == 0)) {
        system_time = gxt.now;
    } else {
        /* Couldn't use the new protocol for getting the system time,
           so fall back to the old PV one.  This is based on pulling
           an epoch out of shared info and then applying an offset
           based on rdtsc.  Unfortunately, Xen lies to us about the
           actual value of the TSC (because we're an HVM guest).  Just
           ignore it and use the epoch value without any correction.
           It's unlikely to be out by more than a few hundred
           milliseconds. */
        system_time = HYPERVISOR_shared_info->vcpu_info[0].time.system_time;
    }

    result = (system_time + wc_nsec)/100;
    result += wc_sec * 10000000ull;

    return result;
}

/* Give Xen a hint that this vcpu isn't going to be doing anything
   productive for the next @ms milliseconds.  This only really makes
   sense in Austere mode; anywhere else, you'd use
   KeDelayExecutionThread(). */
void
DescheduleVcpu(unsigned ms)
{
    xen_hvm_get_time_t gxt;
    sched_poll_t sp;
    int code;

    memset(&sp, 0, sizeof(sp));

    if (GetXenTime(&gxt) != 0)
        return;
    sp.timeout = gxt.now + ms * 1000000ull;
    while (gxt.now < sp.timeout) {
        code = HYPERVISOR_sched_op(SCHEDOP_poll, &sp);
        if (code != 0)
            TraceNotice(("Failed to sleep %d\n", code));
        GetXenTime(&gxt);
    }
}

