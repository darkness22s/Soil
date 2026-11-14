/*
 * Soil VM engine - Windows Hypervisor Platform layer.
 *
 * This mirrors how Oracle VirtualBox's NEM backend drives WinHvPlatform.dll:
 * create partition, set properties, map GPA ranges, create the VP, set/get
 * registers, run, and inject interrupts.
 */
#define WIN32_LEAN_AND_MEAN
#include "vmdefs.h"

static HRESULT VhGetCaps(WHV_CAPABILITY_CODE enmCode, WHV_CAPABILITY *pOut, UINT32 *pcbWritten)
{
    UINT32 cb = 0;
    HRESULT hrc = WHvGetCapability(enmCode, pOut, sizeof(*pOut), &cb);
    if (pcbWritten)
        *pcbWritten = cb;
    return hrc;
}

int VhWhvInit(VH_VM *pVm, u32 cCpus)
{
    WHV_CAPABILITY cap;
    memset(&cap, 0, sizeof(cap));
    HRESULT hrc = VhGetCaps(WHvCapabilityCodeHypervisorPresent, &cap, NULL);
    if (FAILED(hrc) || !cap.HypervisorPresent)
    {
        VhLog("whv: hypervisor not present (hrc=%#lx) - enable the 'Windows Hypervisor Platform' Windows feature", (unsigned long)hrc);
        return 1;
    }

    WHV_PARTITION_HANDLE hPartition = NULL;
    hrc = WHvCreatePartition(&hPartition);
    if (FAILED(hrc))
    {
        VhLog("whv: WHvCreatePartition failed: %#lx", (unsigned long)hrc);
        return 1;
    }

    WHV_PARTITION_PROPERTY prop;
    memset(&prop, 0, sizeof(prop));
    prop.ProcessorCount = cCpus;
    hrc = WHvSetPartitionProperty(hPartition, WHvPartitionPropertyCodeProcessorCount, &prop, sizeof(prop));
    if (FAILED(hrc))
        goto fail;

    /*
     * No hypervisor local-APIC emulation: the guest runs in pure legacy
     * 8259 PIC mode (the engine's device layer supplies the PICs).
     */
    memset(&prop, 0, sizeof(prop));
    prop.LocalApicEmulationMode = WHvX64LocalApicEmulationModeNone;
    hrc = WHvSetPartitionProperty(hPartition, WHvPartitionPropertyCodeLocalApicEmulationMode, &prop, sizeof(prop));
    if (FAILED(hrc))
        VhLog("whv: LocalApicEmulationMode=None not accepted (%#lx) - continuing", (unsigned long)hrc);

    /*
     * Extended exits: CPUID and exception exits are emulated by the engine.
     * MSR exits stay disabled so WHPX handles MSRs natively - 64-bit guests
     * need real EFER (LME/LMA), STAR/LSTAR and FS/GS base state, which a
     * userspace shadow cannot provide for the hardware vCPU.
     */
    memset(&prop, 0, sizeof(prop));
    prop.ExtendedVmExits.X64CpuidExit  = 1;
    prop.ExtendedVmExits.X64MsrExit    = 0;
    prop.ExtendedVmExits.ExceptionExit = 1;
    hrc = WHvSetPartitionProperty(hPartition, WHvPartitionPropertyCodeExtendedVmExits, &prop, sizeof(prop));
    if (FAILED(hrc))
    {
        /* Older builds: try without extended exits (cpuid/msr still exit, with base context). */
        VhLog("whv: ExtendedVmExits not accepted (%#lx) - continuing with defaults", (unsigned long)hrc);
        memset(&prop, 0, sizeof(prop));
        hrc = WHvSetPartitionProperty(hPartition, WHvPartitionPropertyCodeExtendedVmExits, &prop, sizeof(prop));
        if (FAILED(hrc))
            goto fail;
    }

    /*
     * Exit only for CPUID leaves whose feature bits our legacy-PIC, PIT-
     * timed guest must mask. Without this list, asking WHPX for CPUID exits
     * makes Linux's thousands of unrelated CPUID calls round-trip through
     * userspace during every cold boot. WHPX still returns native virtual-
     * CPU values for the other leaves.
     */
    const UINT32 aCpuidExits[] = {
        0x00000001, 0x00000007,
        0x0000000D, 0x0000000E, 0x0000000F,
        0x80000001, 0x80000007
    };
    hrc = WHvSetPartitionProperty(hPartition,
                                  WHvPartitionPropertyCodeCpuidExitList,
                                  aCpuidExits, sizeof(aCpuidExits));
    if (FAILED(hrc))
        VhLog("whv: CpuidExitList not accepted (%#lx); using legacy all-leaves interception",
              (unsigned long)hrc);

    hrc = WHvSetupPartition(hPartition);
    if (FAILED(hrc))
    {
        VhLog("whv: WHvSetupPartition failed: %#lx", (unsigned long)hrc);
        WHvDeletePartition(hPartition);
        return 1;
    }

    pVm->hPartition = hPartition;
    VhLog("whv: partition created (%u vcpu)", cCpus);
    return 0;

fail:
    VhLog("whv: partition property setup failed: %#lx", (unsigned long)hrc);
    WHvDeletePartition(hPartition);
    return 1;
}

void VhWhvTerm(VH_VM *pVm)
{
    if (pVm->hPartition)
    {
        WHvDeletePartition(pVm->hPartition);
        pVm->hPartition = NULL;
    }
}

int VhWhvMapRam(VH_VM *pVm)
{
    HRESULT hrc = WHvMapGpaRange(pVm->hPartition, pVm->mem.pbRam, 0, pVm->mem.cbRam,
                                 WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute);
    if (FAILED(hrc))
    {
        VhLog("whv: map RAM (%llu MiB) failed: %#lx", (unsigned long long)(pVm->mem.cbRam / (1024 * 1024)), (unsigned long)hrc);
        return 1;
    }

    if (VhWhvMapScratch(pVm))
        return 1;
    VhLog("whv: memory mapped (ram=%lluMiB + 384KiB hole)", (unsigned long long)(pVm->mem.cbRam >> 20));
    return 0;
}

/* Restore maps only the 384 KiB legacy hole up front. The guest RAM
   remains unmapped until a snapshot chunk is actually touched. */
int VhWhvMapScratch(VH_VM *pVm)
{
    HRESULT hrc = WHvMapGpaRange(pVm->hPartition, pVm->mem.pbScratch, 0xA0000,
                                 pVm->mem.cbScratch,
                                 WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite |
                                 WHvMapGpaRangeFlagExecute);
    if (FAILED(hrc))
    {
        VhLog("whv: map scratch hole failed: %#lx", (unsigned long)hrc);
        return 1;
    }
    return 0;
}

int VhWhvMapRamChunk(VH_VM *pVm, u64 uGpa, void *pvPage, u64 cb)
{
    HRESULT hrc = WHvMapGpaRange(pVm->hPartition, pvPage, uGpa, cb,
                                 WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite |
                                 WHvMapGpaRangeFlagExecute);
    if (FAILED(hrc))
    {
        VhLog("whv: lazy RAM chunk at %#llx failed: %#lx",
              (unsigned long long)uGpa, (unsigned long)hrc);
        return 1;
    }
    return 0;
}

int VhWhvMapMmioPage(VH_VM *pVm, u64 uGpa, void *pvPage, u64 cb)
{
    HRESULT hrc = WHvMapGpaRange(pVm->hPartition, pvPage, uGpa, cb,
                                 WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite);
    if (FAILED(hrc))
    {
        VhLog("whv: map MMIO page @ %#llx failed: %#lx", (unsigned long long)uGpa, (unsigned long)hrc);
        return 1;
    }
    return 0;
}

int VhWhvCreateCpu(VH_VM *pVm)
{
    HRESULT hrc = WHvCreateVirtualProcessor(pVm->hPartition, 0, 0);
    if (FAILED(hrc))
    {
        VhLog("whv: WHvCreateVirtualProcessor failed: %#lx", (unsigned long)hrc);
        return 1;
    }
    return 0;
}

int VhWhvSetReg(VH_VM *pVm, WHV_REGISTER_NAME enm, WHV_REGISTER_VALUE val)
{
    return VhWhvSetRegs(pVm, &enm, &val, 1);
}

int VhWhvSetRegs(VH_VM *pVm, const WHV_REGISTER_NAME *paenm, const WHV_REGISTER_VALUE *pav, u32 c)
{
    HRESULT hrc = WHvSetVirtualProcessorRegisters(pVm->hPartition, 0, paenm, c, pav);
    if (FAILED(hrc))
    {
        VhLog("whv: WHvSetVirtualProcessorRegisters failed: %#lx", (unsigned long)hrc);
        return 1;
    }
    return 0;
}

int VhWhvGetReg(VH_VM *pVm, WHV_REGISTER_NAME enm, WHV_REGISTER_VALUE *pVal)
{
    return VhWhvGetRegs(pVm, &enm, pVal, 1);
}

int VhWhvGetRegs(VH_VM *pVm, const WHV_REGISTER_NAME *paenm, WHV_REGISTER_VALUE *pav, u32 c)
{
    HRESULT hrc = WHvGetVirtualProcessorRegisters(pVm->hPartition, 0, paenm, c, pav);
    if (FAILED(hrc))
    {
        VhLog("whv: WHvGetVirtualProcessorRegisters failed: %#lx", (unsigned long)hrc);
        return 1;
    }
    return 0;
}

int VhWhvSetSegment(VH_VM *pVm, WHV_REGISTER_NAME enm, u64 uBase, u32 uLimit, u16 uSelector,
                    u16 uType, bool fCode64, bool fDb, bool fGran, u8 uDpl)
{
    WHV_REGISTER_VALUE val;
    memset(&val, 0, sizeof(val));
    val.Segment.Base = uBase;
    val.Segment.Limit = uLimit;
    val.Segment.Selector = uSelector;
    val.Segment.SegmentType = uType & 0xf;
    val.Segment.NonSystemSegment = 1;
    val.Segment.DescriptorPrivilegeLevel = uDpl;
    val.Segment.Present = 1;
    val.Segment.Long = fCode64 ? 1 : 0;
    val.Segment.Default = fDb ? 1 : 0;
    val.Segment.Granularity = fGran ? 1 : 0;
    return VhWhvSetReg(pVm, enm, val);
}

/*
 * Interrupt delivery via the pending-interruption register, mirroring what
 * QEMU's WHPX backend does when the Hyper-V APIC is disabled: an external
 * interrupt is only programmed when the guest is actually interruptible
 * (caller checks IF / interrupt shadow / no pending interruption).
 */
int VhWhvInjectInterrupt(VH_VM *pVm, u8 bVector)
{
    WHV_REGISTER_VALUE val;
    memset(&val, 0, sizeof(val));
    val.PendingInterruption.InterruptionPending = 1;
    val.PendingInterruption.InterruptionType = WHvX64PendingInterrupt;
    val.PendingInterruption.InterruptionVector = bVector;
    int rc = VhWhvSetReg(pVm, WHvRegisterPendingInterruption, val);
    if (rc)
        VhLog("whv: failed to set pending interruption %#x", bVector);
    return rc;
}

/* Clear a hypervisor-side HLT suspend so a pending interrupt can wake the vCPU. */
int VhWhvClearHaltSuspend(VH_VM *pVm)
{
    WHV_REGISTER_VALUE val;
    memset(&val, 0, sizeof(val));
    if (VhWhvGetReg(pVm, WHvRegisterInternalActivityState, &val))
        return -1;
    if (!val.InternalActivity.HaltSuspend)
        return 0;
    val.InternalActivity.HaltSuspend = 0;
    return VhWhvSetReg(pVm, WHvRegisterInternalActivityState, val);
}

int VhWhvInjectException(VH_VM *pVm, u8 bVector, u32 uErrorCode, bool fHasErrorCode)
{
    WHV_REGISTER_VALUE val;
    memset(&val, 0, sizeof(val));
    val.ExceptionEvent.EventPending = 1;
    val.ExceptionEvent.EventType = WHvX64PendingEventException;
    val.ExceptionEvent.DeliverErrorCode = fHasErrorCode ? 1 : 0;
    val.ExceptionEvent.Vector = bVector;
    val.ExceptionEvent.ErrorCode = uErrorCode;
    return VhWhvSetReg(pVm, WHvRegisterPendingEvent, val);
}

int VhWhvSetDeliverabilityNotification(VH_VM *pVm, bool fEnable)
{
    WHV_REGISTER_VALUE val;
    memset(&val, 0, sizeof(val));
    val.DeliverabilityNotifications.InterruptNotification = fEnable ? 1 : 0;
    return VhWhvSetReg(pVm, WHvX64RegisterDeliverabilityNotifications, val);
}

/*
 * Alternative delivery via the hypervisor's local APIC emulation
 * (WHvRequestInterrupt), used as a fallback path.
 */
int VhWhvRequestInterrupt(VH_VM *pVm, u8 bVector)
{
    WHV_INTERRUPT_CONTROL ctl;
    memset(&ctl, 0, sizeof(ctl));
    ctl.Type = WHvX64InterruptTypeFixed;
    ctl.DestinationMode = 0 /* physical */;
    ctl.TriggerMode = WHvX64InterruptTriggerModeEdge;
    ctl.Destination = 0;    /* BSP */
    ctl.Vector = bVector;
    HRESULT hrc = WHvRequestInterrupt(pVm->hPartition, &ctl, sizeof(ctl));
    if (FAILED(hrc))
    {
        VhLog("whv: WHvRequestInterrupt(%#x) failed: %#lx", bVector, (unsigned long)hrc);
        return 1;
    }
    return 0;
}

int VhWhvCancelRun(VH_VM *pVm)
{
    HRESULT hrc = WHvCancelRunVirtualProcessor(pVm->hPartition, 0, 0);
    return SUCCEEDED(hrc) ? 0 : 1;
}
