/*
 * Soil VM engine (vmhost) - main program.
 *
 * Runs one guest VM on the Windows Hypervisor Platform with host-side device
 * models (8259 PIC, 8254 PIT, MC146818 RTC, 16550 UART, i8042).
 *
 * Usage:
 *   vmhost --kernel <vmlinuz> --initrd <core.gz> [--cmdline "..."]
 *          [--ram <MiB>] [--log <file>]
 */
#define WIN32_LEAN_AND_MEAN
#include "vmdefs.h"
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdlib.h>
#include <mmsystem.h>

static VH_VM g_Vm;
enum { VMHOST_MAX_DYN_MMIO_PAGES = 256 };
static struct { u64 uGpa; void *pvPage; } g_aDynMmio[VMHOST_MAX_DYN_MMIO_PAGES];
static u32 g_cDynMmioPages;
static volatile bool g_fInputThreadDone = false;
static volatile bool g_fStdinEof = false;

int VhHostSnapSaveExtra(VH_VM *pVm, FILE *pFile)
{
    u32 count = g_cDynMmioPages;
    (void)pVm;
    if (fwrite(&count, sizeof(count), 1, pFile) != 1)
        return 1;
    for (u32 i = 0; i < count; i++)
    {
        if (fwrite(&g_aDynMmio[i].uGpa, sizeof(u64), 1, pFile) != 1 ||
            fwrite(g_aDynMmio[i].pvPage, VH_PAGE, 1, pFile) != 1)
            return 1;
    }
    return 0;
}

int VhHostSnapRestoreExtra(VH_VM *pVm, FILE *pFile, u32 cbPayload)
{
    u32 count = 0;
    if (cbPayload < sizeof(count) || fread(&count, sizeof(count), 1, pFile) != 1 ||
        count > VMHOST_MAX_DYN_MMIO_PAGES ||
        cbPayload != sizeof(count) + count * (sizeof(u64) + VH_PAGE))
        return 1;
    for (u32 i = 0; i < count; i++)
    {
        u64 gpa = 0;
        void *page = VirtualAlloc(NULL, VH_PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!page || fread(&gpa, sizeof(gpa), 1, pFile) != 1 ||
            fread(page, VH_PAGE, 1, pFile) != 1 ||
            VhWhvMapMmioPage(pVm, gpa, page, VH_PAGE))
        {
            if (page) VirtualFree(page, 0, MEM_RELEASE);
            return 1;
        }
        g_aDynMmio[i].uGpa = gpa;
        g_aDynMmio[i].pvPage = page;
    }
    g_cDynMmioPages = count;
    return 0;
}

/* ------------------------------------------------------------------ */
/* MSR state + emulation                                              */
/* ------------------------------------------------------------------ */
typedef struct VH_MSRS
{
    u64 uSysenterCs, uSysenterEsp, uSysenterEip;
    u64 uFsBase, uGsBase;
    u64 uEfer, uPat, uApicBase;
} VH_MSRS;
static VH_MSRS g_Msrs;

#define MSR_IA32_SYSENTER_CS    0x00000174
#define MSR_IA32_SYSENTER_ESP   0x00000175
#define MSR_IA32_SYSENTER_EIP   0x00000176
#define MSR_IA32_MCG_CAP        0x00000179
#define MSR_IA32_MCG_STATUS     0x0000017A
#define MSR_IA32_MISC_ENABLE    0x000001A0
#define MSR_IA32_DEBUGCTL       0x000001D9
#define MSR_IA32_PAT            0x00000277
#define MSR_IA32_APIC_BASE      0x0000001B
#define MSR_IA32_TSC            0x00000010
#define MSR_IA32_MTRR_CAP       0x000000FE
#define MSR_EFER                0xC0000080
#define MSR_FS_BASE             0xC0000100
#define MSR_GS_BASE             0xC0000101

static u64 VhMsrRead(VH_VM *pVm, u32 uMsr)
{
    (void)pVm;
    switch (uMsr)
    {
        case MSR_IA32_TSC:          return VhGuestNowNs() / 100;
        case MSR_IA32_APIC_BASE:    return g_Msrs.uApicBase ? g_Msrs.uApicBase : 0xFEE00900;
        case MSR_IA32_SYSENTER_CS:  return g_Msrs.uSysenterCs;
        case MSR_IA32_SYSENTER_ESP: return g_Msrs.uSysenterEsp;
        case MSR_IA32_SYSENTER_EIP: return g_Msrs.uSysenterEip;
        case MSR_IA32_MCG_CAP:      return 0;       /* no machine check */
        case MSR_IA32_MCG_STATUS:   return 0;
        case MSR_IA32_MISC_ENABLE:  return 0;
        case MSR_IA32_DEBUGCTL:     return 0;
        case MSR_IA32_PAT:          return g_Msrs.uPat;
        case MSR_IA32_MTRR_CAP:     return 0;       /* MTRRs not supported */
        case MSR_EFER:              return g_Msrs.uEfer & ~(0x400ULL | 0x100ULL);   /* LMA/LME off */
        case MSR_FS_BASE:           return g_Msrs.uFsBase;
        case MSR_GS_BASE:           return g_Msrs.uGsBase;
        default:
            if (uMsr == 0x8B || uMsr == 0x79)
                return 0;                            /* BIOS_SIGN_ID */
            if (uMsr >= 0x400 && uMsr < 0x500)
                return 0;                            /* MCi banks */
            return 0;
    }
}

static void VhMsrWrite(VH_VM *pVm, u32 uMsr, u64 uValue)
{
    (void)pVm;
    switch (uMsr)
    {
        case MSR_IA32_APIC_BASE:    g_Msrs.uApicBase = uValue; break;
        case MSR_IA32_SYSENTER_CS:  g_Msrs.uSysenterCs = uValue & 0xFFFF; break;
        case MSR_IA32_SYSENTER_ESP: g_Msrs.uSysenterEsp = uValue; break;
        case MSR_IA32_SYSENTER_EIP: g_Msrs.uSysenterEip = uValue; break;
        case MSR_IA32_PAT:          g_Msrs.uPat = uValue; break;
        case MSR_EFER:              g_Msrs.uEfer = uValue & ~(0x400ULL | 0x100ULL); break;
        case MSR_FS_BASE:           g_Msrs.uFsBase = uValue; break;
        case MSR_GS_BASE:           g_Msrs.uGsBase = uValue; break;
        default:                    break;          /* swallow the rest */
    }
}

/* ------------------------------------------------------------------ */
/* CPUID emulation                                                    */
/* ------------------------------------------------------------------ */
/* Clear CPUID bits the guest must not see (gigantic parity with what a
   conservative VMM exposes; VirtualBox NEM likewise masks host bits). */
static void VhCpuidApply(VH_VM *pVm, const WHV_X64_CPUID_ACCESS_CONTEXT *pCtx,
                         u32 *pRax, u32 *pRbx, u32 *pRcx, u32 *pRdx)
{
    u32 uLeaf = (u32)pCtx->Rax;
    u32 uRax = (u32)pCtx->DefaultResultRax;
    u32 uRbx = (u32)pCtx->DefaultResultRbx;
    u32 uRcx = (u32)pCtx->DefaultResultRcx;
    u32 uRdx = (u32)pCtx->DefaultResultRdx;
    (void)pVm;

    if (uLeaf == 0x00000001)
    {
        uRcx &= ~((1u << 31)     /* hypervisor present */
                | (1u << 30)     /* RDRAND */
                | (1u << 29)     /* F16C */
                | (1u << 28)     /* AVX */
                | (1u << 27)     /* OSXSAVE */
                | (1u << 26)     /* XSAVE */
                | (1u << 12)     /* FMA */
                | (1u << 5)      /* VMX */
                | (1u << 1));    /* PCLMULQDQ */
        uRdx &= ~((1u << 9)      /* APIC */
                | (1u << 4)      /* TSC (keep PIT based) */
                | (1u << 28)     /* HTT */
                | (1u << 29)     /* TM */
                | (1u << 27)     /* SS */
                | (1u << 26));   /* SSE2 */
        uRdx |= (1u << 26);      /* SSE2 on (i686 kernel wants SSE2 available) */
    }
    else if (uLeaf == 0x00000007)
    {
        uRbx = 0;                /* no extended feature flags (AVX2/BMI/... off) */
        uRcx = 0;
        uRdx = 0;
    }
    else if (uLeaf >= 0x0000000D && uLeaf <= 0x0000000F)
    {
        uRax = uRbx = uRcx = uRdx = 0;   /* no XSAVE areas */
    }
    else if (uLeaf == 0x80000001)
    {
        uRdx &= ~((1u << 27) | (1u << 26));  /* RDTSCP | 1GB pages */
        uRcx &= ~((1u << 6) | (1u << 5) | (1u << 2)); /* SSE4A | LZCNT | SVM */
    }
    else if (uLeaf == 0x80000007)
    {
        uRax = uRbx = uRcx = uRdx = 0;   /* no invariant TSC */
    }

    *pRax = uRax;
    *pRbx = uRbx;
    *pRcx = uRcx;
    *pRdx = uRdx;
}

/* ------------------------------------------------------------------ */
/* Interrupt injection (QEMU WHPX PIC-mode model)                     */
/* ------------------------------------------------------------------ */
/*
 * With the Hyper-V APIC disabled, QEMU programs an external interrupt into
 * the pending-interruption register only when the guest is interruptible
 * per the last VM-exit context (IF=1, no interrupt shadow, no interruption
 * already pending); otherwise it arms the deliverability notification and
 * retries on the resulting interrupt-window exit.
 */
static void VhUpdateInterrupts(VH_VM *pVm)
{
    if (!VhDevHasPendingIrq(pVm))
        return;

    if (!pVm->fGuestIntrPending && pVm->fGuestInterruptable && pVm->fGuestHasIf)
    {
        u8 bVector = VhDevPicGetVector(pVm);
        if (bVector)
        {
            static u32 s_cInj = 0;
            if (s_cInj < 20) { VhLog("irq: inject vec=%#x", bVector); s_cInj++; }
            if (VhWhvInjectInterrupt(pVm, bVector) == 0)
                return;
            VhDevPicRequeueVector(pVm, bVector);
        }
    }

    if (!pVm->fIrqWindowArmed)
    {
        if (VhWhvSetDeliverabilityNotification(pVm, true) == 0)
            pVm->fIrqWindowArmed = true;
    }
}

/* ------------------------------------------------------------------ */
/* Run loop exit handling                                             */
/* ------------------------------------------------------------------ */
static void VhAdvanceRip(VH_VM *pVm, const WHV_VP_EXIT_CONTEXT *pVpCtx)
{
    WHV_REGISTER_NAME aenm[] = { WHvX64RegisterRip, WHvX64RegisterRflags };
    WHV_REGISTER_VALUE av[2];
    av[0].Reg64 = pVpCtx->Rip + pVpCtx->InstructionLength;
    av[1].Reg64 = pVpCtx->Rflags & ~(u64)0x10000;   /* clear RF */
    VhWhvSetRegs(pVm, aenm, av, 2);
}

static bool VhReadGuestU64(VH_VM *pVm, u64 uGpa, u64 *puValue)
{
    if (uGpa + 8 > pVm->mem.cbRam)
        return false;
    memcpy(puValue, pVm->mem.pbRam + uGpa, 8);
    return true;
}

static bool VhStringTranslateGva(VH_VM *pVm, u64 uGva, u64 *puGpa)
{
    WHV_REGISTER_NAME aenm[2] = { WHvX64RegisterCr3, WHvX64RegisterCr4 };
    WHV_REGISTER_VALUE av[2];
    u64 uCr3, uCr4, uEntry, uTable;
    const u64 uMask = 0x000FFFFFFFFFF000ull;
    int aIdx[5];
    int nLevels, i;

    if (VhWhvGetRegs(pVm, aenm, av, 2) == 0)
    {
        uCr3 = av[0].Reg64 & ~0xFFFull;     /* strip PCID */
        uCr4 = av[1].Reg64;
        nLevels = (uCr4 & (1ull << 12)) ? 5 : 4;   /* CR4.LA57 */
        if (nLevels == 5)
            aIdx[0] = (int)((uGva >> 48) & 0x1FF);
        aIdx[nLevels - 4] = (int)((uGva >> 39) & 0x1FF);
        aIdx[nLevels - 3] = (int)((uGva >> 30) & 0x1FF);
        aIdx[nLevels - 2] = (int)((uGva >> 21) & 0x1FF);
        aIdx[nLevels - 1] = (int)((uGva >> 12) & 0x1FF);

        uTable = uCr3;
        for (i = 0; i < nLevels; i++)
        {
            if (!VhReadGuestU64(pVm, uTable + (u64)aIdx[i] * 8, &uEntry))
                break;
            if (!(uEntry & 1))
                break;
            if (i == nLevels - 1)
            {
                *puGpa = (uEntry & uMask) | (uGva & 0xFFFull);
                return true;
            }
            if (uEntry & 0x80)      /* PS: huge page */
            {
                if (i == nLevels - 2)   /* 2MB */
                {
                    *puGpa = (uEntry & 0x000FFFFFFFE00000ull) |
                             (uGva & 0x1FFFFFull);
                }
                else                    /* 1GB */
                {
                    *puGpa = (uEntry & 0x000FFFFFC0000000ull) |
                             (uGva & 0x3FFFFFFFull);
                }
                return true;
            }
            uTable = uEntry & uMask;
        }
    }

    {
        WHV_TRANSLATE_GVA_RESULT res;
        u64 uGpa = 0;
        HRESULT hr = WHvTranslateGva(pVm->hPartition, 0, uGva,
                                     WHvTranslateGvaFlagNone, &res, &uGpa);
        if (FAILED(hr) || res.ResultCode != WHvTranslateGvaResultSuccess)
        {
            static u32 s_cFail = 0;
            if (s_cFail < 5)
            {
                VhLog("xlate: failed gva=%#llx hr=%#lx rc=%u",
                      (unsigned long long)uGva, (unsigned long)hr,
                      (unsigned)res.ResultCode);
                s_cFail++;
            }
            return false;
        }
        *puGpa = uGpa;
        return true;
    }
}

/* Emulate INS/OUTS string port I/O (used by the guest ne2k-pci driver). */
static u32 s_cStrTrace = 0;
static bool VhHandleStringIo(VH_VM *pVm, const WHV_RUN_VP_EXIT_CONTEXT *pExit)
{
    const WHV_X64_IO_PORT_ACCESS_CONTEXT *pIo = &pExit->IoPortAccess;
    u16 uPort = pIo->PortNumber;
    u32 cbWidth = pIo->AccessInfo.AccessSize;
    bool fWrite = pIo->AccessInfo.IsWrite != 0;
    bool fRep = pIo->AccessInfo.RepPrefix != 0;
    bool fDf = (pExit->VpContext.Rflags & (1ull << 10)) != 0;
    bool fAddr32 = false;
    u64 cCount = fRep ? pIo->Rcx : 1;
    u64 uSegBase = fWrite ? pIo->Ds.Base : pIo->Es.Base;
    u64 uOffset = fWrite ? pIo->Rsi : pIo->Rdi;
    i64 lStep;
    u64 k;

    if (s_cStrTrace < 48)
    {
        VhLog("strio: port=%#x w=%u wr=%u rep=%u rcx=%llu ibc=%u b0=%02x b1=%02x",
              uPort, cbWidth, fWrite, fRep, (unsigned long long)pIo->Rcx,
              pIo->InstructionByteCount,
              pIo->InstructionBytes[0], pIo->InstructionBytes[1]);
        s_cStrTrace++;
    }

    if (cbWidth != 1 && cbWidth != 2 && cbWidth != 4)
        cbWidth = 1;
    lStep = fDf ? -(i64)cbWidth : (i64)cbWidth;

    for (k = 0; k < pIo->InstructionByteCount && k < 4; k++)
    {
        u8 b = pIo->InstructionBytes[k];
        if (b == 0x67)
            fAddr32 = true;
        if (b == 0x6C || b == 0x6D || b == 0x6E || b == 0x6F)
            break;      /* opcode reached */
    }

    for (k = 0; k < cCount; k++)
    {
        u64 uGva = uSegBase + (fAddr32 ? (uOffset & 0xFFFFFFFFu) : uOffset);
        u64 uGpa;
        u32 uVal = 0;

        if (!VhStringTranslateGva(pVm, uGva, &uGpa))
        {
            VhLog("strio: translate FAIL at iter %llu gva=%#llx",
                  (unsigned long long)k, (unsigned long long)uGva);
            break;
        }
        if (uGpa + cbWidth > pVm->mem.cbRam)
        {
            VhLog("strio: gpa OOB at iter %llu gpa=%#llx", (unsigned long long)k,
                  (unsigned long long)uGpa);
            break;
        }

        if (fWrite)
        {
            memcpy(&uVal, pVm->mem.pbRam + uGpa, cbWidth);
            VhDevIoPort(pVm, uPort, true, cbWidth, &uVal);
        }
        else
        {
            VhDevIoPort(pVm, uPort, false, cbWidth, &uVal);
            memcpy(pVm->mem.pbRam + uGpa, &uVal, cbWidth);
        }
        uOffset += (u64)lStep;
    }


    {
        WHV_REGISTER_NAME aenm[4] = { WHvX64RegisterRip, WHvX64RegisterRsi,
                                      WHvX64RegisterRdi, WHvX64RegisterRcx };
        WHV_REGISTER_VALUE av[4];
        av[0].Reg64 = pExit->VpContext.Rip + pExit->VpContext.InstructionLength;
        av[1].Reg64 = fWrite ? uOffset : pIo->Rsi;
        av[2].Reg64 = fWrite ? pIo->Rdi : uOffset;
        av[3].Reg64 = fRep ? 0 : pIo->Rcx;
        VhWhvSetRegs(pVm, aenm, av, 4);
    }
    return true;
}

static u64 s_acIoPorts[0x10000];

static void VhLogHotIoPorts(void)
{
    u32 aTop[3] = { 0, 0, 0 };
    u64 aCnt[3] = { 0, 0, 0 };
    for (u32 i = 0; i < 0x10000; i++)
    {
        u64 c = s_acIoPorts[i];
        if (!c)
            continue;
        for (int j = 0; j < 3; j++)
        {
            if (c > aCnt[j])
            {
                for (int k = 2; k > j; k--) { aCnt[k] = aCnt[k - 1]; aTop[k] = aTop[k - 1]; }
                aCnt[j] = c;
                aTop[j] = i;
                break;
            }
        }
    }
    VhLog("io: hot ports %#x=%llu %#x=%llu %#x=%llu",
          aTop[0], (unsigned long long)aCnt[0],
          aTop[1], (unsigned long long)aCnt[1],
          aTop[2], (unsigned long long)aCnt[2]);
}

static bool VhHandleIoPort(VH_VM *pVm, const WHV_RUN_VP_EXIT_CONTEXT *pExit)
{
    const WHV_X64_IO_PORT_ACCESS_CONTEXT *pIo = &pExit->IoPortAccess;
    u32 uPort = pIo->PortNumber;
    u32 cb = pIo->AccessInfo.AccessSize;
    bool fWrite = pIo->AccessInfo.IsWrite != 0;
    u32 uValue = (u32)pIo->Rax;
    u32 uMask = (cb == 1) ? 0xFFu : (cb == 2) ? 0xFFFFu : 0xFFFFFFFFu;

    s_acIoPorts[uPort]++;

    if (pIo->AccessInfo.StringOp)
        return VhHandleStringIo(pVm, pExit);

    uValue &= uMask;
    if (!VhDevIoPort(pVm, (u16)uPort, fWrite, cb, &uValue))
    {
        static u32 s_uLastWarn = 0xFFFFFFFF;
        if (s_uLastWarn != uPort)
        {
            VhLog("io: unhandled %s port %#x LB %u (guest RAX=%#llx RIP=%#llx)",
                  fWrite ? "OUT" : "IN", uPort, cb,
                  (unsigned long long)pIo->Rax, (unsigned long long)pExit->VpContext.Rip);
            s_uLastWarn = uPort;
        }
        uValue = (cb == 4) ? 0xFFFFFFFFu : uMask;
    }

    if (!fWrite)
    {
        u64 uNewRax;
        if (cb == 4)
            uNewRax = uValue;   /* zero-extended to 64 bits */
        else
            uNewRax = (pIo->Rax & ~(u64)uMask) | (uValue & uMask);
        VhWhvSetReg(pVm, WHvX64RegisterRax, (WHV_REGISTER_VALUE){ .Reg64 = uNewRax });
    }

    VhAdvanceRip(pVm, &pExit->VpContext);
    return true;
}

static bool VhHandleMsr(VH_VM *pVm, const WHV_RUN_VP_EXIT_CONTEXT *pExit)
{
    const WHV_X64_MSR_ACCESS_CONTEXT *pMsr = &pExit->MsrAccess;
    u64 uValue = (u64)(u32)pMsr->Rax | ((u64)(u32)pMsr->Rdx << 32);

    if (pMsr->AccessInfo.IsWrite)
        VhMsrWrite(pVm, pMsr->MsrNumber, uValue);
    else
    {
        uValue = VhMsrRead(pVm, pMsr->MsrNumber);
        WHV_REGISTER_NAME aenm[] = { WHvX64RegisterRax, WHvX64RegisterRdx };
        WHV_REGISTER_VALUE av[2];
        av[0].Reg64 = (u32)uValue;
        av[1].Reg64 = uValue >> 32;
        VhWhvSetRegs(pVm, aenm, av, 2);
    }

    VhAdvanceRip(pVm, &pExit->VpContext);
    return true;
}

static bool VhHandleCpuid(VH_VM *pVm, const WHV_RUN_VP_EXIT_CONTEXT *pExit)
{
    u32 uRax, uRbx, uRcx, uRdx;
    VhCpuidApply(pVm, &pExit->CpuidAccess, &uRax, &uRbx, &uRcx, &uRdx);

    WHV_REGISTER_NAME aenm[] = { WHvX64RegisterRax, WHvX64RegisterRbx, WHvX64RegisterRcx, WHvX64RegisterRdx };
    WHV_REGISTER_VALUE av[4];
    av[0].Reg64 = uRax;
    av[1].Reg64 = uRbx;
    av[2].Reg64 = uRcx;
    av[3].Reg64 = uRdx;
    VhWhvSetRegs(pVm, aenm, av, 4);

    VhAdvanceRip(pVm, &pExit->VpContext);
    return true;
}

static void VhHandleException(VH_VM *pVm, const WHV_RUN_VP_EXIT_CONTEXT *pExit)
{
    const WHV_VP_EXCEPTION_CONTEXT *pExc = &pExit->VpException;

    WHV_REGISTER_VALUE val;
    memset(&val, 0, sizeof(val));
    val.ExceptionEvent.EventPending = 1;
    val.ExceptionEvent.EventType = WHvX64PendingEventException;
    val.ExceptionEvent.DeliverErrorCode = pExc->ExceptionInfo.ErrorCodeValid ? 1 : 0;
    val.ExceptionEvent.Vector = pExc->ExceptionType;
    val.ExceptionEvent.ErrorCode = pExc->ErrorCode;
    val.ExceptionEvent.ExceptionParameter = pExc->ExceptionParameter;
    VhWhvSetReg(pVm, WHvRegisterPendingEvent, val);
}

/* ------------------------------------------------------------------ */
/* Threads                                                            */
/* ------------------------------------------------------------------ */
/* Copy one coherent frame: park the vCPU between runs so the guest cannot
   modify the framebuffer while it is being read (prevents tearing). */
static CRITICAL_SECTION g_csFbSnap;
static volatile LONG g_fFbSnapInit;

void VhFbSnapshot(VH_VM *pVm, u8 *pbDst)
{
    if (!pVm->mem.pbRam || !pVm->mem.gpaFb || !pbDst || pVm->fSnapWorking || pVm->fSnapWanted)
        return;
    if (InterlockedCompareExchange(&g_fFbSnapInit, 1, 0) == 0)
        InitializeCriticalSection(&g_csFbSnap);
    EnterCriticalSection(&g_csFbSnap);
    pVm->fFbCopyWanted = true;
    VhWhvCancelRun(pVm);
    for (u32 i = 0; pVm->fFbCopyWanted && !pVm->fFbCopyPaused && !pVm->fStop && i < 2000; i++)
        Sleep(0);
    if (!pVm->fStop)
        memcpy(pbDst, pVm->mem.pbRam + pVm->mem.gpaFb, VH_FB_SIZE);
    pVm->fFbCopyWanted = false;
    pVm->fFbCopyPaused = false;
    LeaveCriticalSection(&g_csFbSnap);
}

static unsigned __stdcall TimerThreadProc(void *pvArg)
{
    VH_VM *pVm = (VH_VM *)pvArg;
    u64 uNextBeatNs = VhGuestNowNs() + 60000000000ull;
    while (!pVm->fStop)
    {
        if (pVm->fSnapWorking)
        {
            Sleep(1);
            continue;
        }
        VhDevPitTick(pVm, VhGuestNowNs());
        VhNetPoll();
        if (VhDevHasPendingIrq(pVm) && !pVm->fIrqWindowArmed)
            VhWhvCancelRun(pVm);
        /* stall watchdog: if the vCPU has been running without exits for a
           while, kick it out so the run loop can inspect the guest state */
        if (pVm->uLastExitNs && VhGuestNowNs() - pVm->uLastExitNs > 3000000000ull)
            VhWhvCancelRun(pVm);
        if (VhGuestNowNs() >= uNextBeatNs)
        {
            uNextBeatNs = VhGuestNowNs() + 60000000000ull;
            VhLog("timer: alive t=%llus ticks=%llu lastExit=%llus",
                  VhGuestNowNs() / 1000000000ull, pVm->cTicks,
                  pVm->uLastExitNs / 1000000000ull);
        }
        Sleep(1);
    }
    return 0;
}

static unsigned __stdcall InputThreadProc(void *pvArg)
{
    VH_VM *pVm = (VH_VM *)pvArg;
    u8 abBuf[512];
    /* input framing:
       0x1B 'k' <n> <n scancode bytes>  -> keyboard make/break codes (set 1)
       0x1B 'm' <b0> <b1> <b2>          -> PS/2 mouse packet
       0x1B 0x1B                        -> literal ESC to guest UART
       anything else                    -> raw bytes to guest UART */
    int st = 0;
    u32 need = 0, got = 0;
    u8 frame[256];
    while (!pVm->fStop)
    {
        int cb = _read(_fileno(stdin), abBuf, sizeof(abBuf));
        if (cb <= 0)
        {
            Sleep(50);
            continue;
        }
        if (cb > 0)
        {
            for (int i = 0; i < cb; i++)
            {
                u8 b = abBuf[i];
                if (st == 0)
                {
                    if (b == 0x1B) st = 1;
                    else VhDevUartWriteGuest(pVm, &b, 1);
                }
                else if (st == 1)
                {
                    if (b == 'k') { st = 2; }
                    else if (b == 'm') { st = 3; need = 3; got = 0; }
                    else if (b == 0x1B) { VhDevUartWriteGuest(pVm, &b, 1); st = 0; }
                    else
                    {
                        u8 abTwo[2] = { 0x1B, b };
                        VhDevUartWriteGuest(pVm, abTwo, 2);
                        st = 0;
                    }
                }
                else if (st == 2)                       /* waiting for count byte */
                {
                    need = b;
                    got = 0;
                    st = need ? 4 : 0;
                }
                else if (st == 4)                       /* collecting scancodes */
                {
                    frame[got++] = b;
                    if (got >= need)
                    {
                        VhDevKbdQueueScancodes(pVm, frame, got);
                        st = 0;
                    }
                }
                else if (st == 3)                       /* collecting mouse packet */
                {
                    frame[got++] = b;
                    if (got >= need)
                    {
                        VhDevMouseQueuePacket(pVm, frame, got);
                        st = 0;
                    }
                }
            }
        }
        else
        {
            g_fStdinEof = true;
            VhLog("stdin closed - input thread exiting");
            break;
        }
    }
    g_fInputThreadDone = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */
static void usage(void)
{
    fprintf(stderr,
            "usage: vmhost --kernel <vmlinuz> --initrd <core.gz> [--cmdline \"...\"] [--ram <MiB>] [--disk <img>] [--data-disk <img>] [--data-gb <n>] [--log <file>] [--no-ui] [--ipc <token>] [--no-hostfwd] [--ssh-port <port>] [--mcp-port <port>] [--snapshot-out <file>] [--restore <file>]\n");
}

int main(int argc, char *argv[])
{
    const char *pszKernel = NULL;
    const char *pszInitrd = NULL;
    const char *pszDisk = NULL;
    const char *pszDataDisk = NULL;
    u32 cbDataGb = 0;
    const char *pszCmdLine = "console=ttyS0 loglevel=7 acpi=off noapic nolapic idle=poll";
    const char *pszLogFile = NULL;
    const char *pszIpc = NULL;
    const char *pszSnapshotOut = NULL;
    const char *pszRestore = NULL;
    u32 cbRamMiB = 512;
    int sshPort = 2222;
    int mcpPort = 2223;
    bool fNoUi = false;
    bool fHostFwd = true;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--kernel") && i + 1 < argc)
            pszKernel = argv[++i];
        else if (!strcmp(argv[i], "--initrd") && i + 1 < argc)
            pszInitrd = argv[++i];
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc)
            pszDisk = argv[++i];
        else if (!strcmp(argv[i], "--data-disk") && i + 1 < argc)
            pszDataDisk = argv[++i];
        else if (!strcmp(argv[i], "--data-gb") && i + 1 < argc)
            cbDataGb = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cmdline") && i + 1 < argc)
            pszCmdLine = argv[++i];
        else if (!strcmp(argv[i], "--ram") && i + 1 < argc)
            cbRamMiB = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)
            pszLogFile = argv[++i];
        else if (!strcmp(argv[i], "--ipc") && i + 1 < argc)
            pszIpc = argv[++i];
        else if (!strcmp(argv[i], "--snapshot-out") && i + 1 < argc)
            pszSnapshotOut = argv[++i];
        else if (!strcmp(argv[i], "--restore") && i + 1 < argc)
            pszRestore = argv[++i];
        else if (!strcmp(argv[i], "--ssh-port") && i + 1 < argc)
            sshPort = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mcp-port") && i + 1 < argc)
            mcpPort = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-hostfwd"))
            fHostFwd = false;
        else if (!strcmp(argv[i], "--no-ui"))
            fNoUi = true;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        {
            usage();
            return 0;
        }
        else
        {
            fprintf(stderr, "vmhost: unknown argument '%s'\n", argv[i]);
            usage();
            return 1;
        }
    }

    if (!pszKernel || !pszInitrd)
    {
        usage();
        return 1;
    }

    /* binary stdio: serial data is raw */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);

    /* 1ms scheduler granularity: the guest clock is driven by PIT IRQs raised
       from the timer thread; the default 15.6ms Sleep quantum would cap
       delivery at ~64 irqs/s and make guest time run ~4.6x slow */
    timeBeginPeriod(1);

    if (pszLogFile)
    {
        FILE *pLog = fopen(pszLogFile, "w");
        if (pLog)
            VhLogSetFile(pLog);
    }

    VH_VM *pVm = &g_Vm;
    VhSnapImgRefs snapRefs = { pszKernel, pszInitrd, pszDisk, pszDataDisk };
    memset(pVm, 0, sizeof(*pVm));
    pVm->nsBoot = VhGuestNowNs();

    /* RAM + legacy hole scratch */
    pVm->mem.cbRam = (u64)cbRamMiB << 20;
    pVm->mem.pbRam = (u8 *)VirtualAlloc(NULL, (SIZE_T)pVm->mem.cbRam, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pVm->mem.pbRam)
    {
        VhLog("vmhost: failed to allocate %u MiB of RAM", cbRamMiB);
        return 1;
    }
    pVm->mem.cbScratch = 0x60000;
    pVm->mem.pbScratch = (u8 *)VirtualAlloc(NULL, (SIZE_T)pVm->mem.cbScratch, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pVm->mem.pbScratch)
    {
        VhLog("vmhost: failed to allocate the legacy hole scratch page");
        return 1;
    }

    if (VhWhvInit(pVm, 1))
        return 1;
    /* A snapshot restores RAM on fault, rather than allocating/mapping every
       guest page before the first instruction. Cold boot maps all RAM. */
    if (pszRestore ? VhWhvMapScratch(pVm) : VhWhvMapRam(pVm))
        return 1;
    if (VhWhvCreateCpu(pVm))
        return 1;

    VhDevInit(pVm);
    VhNetSetHostFwdPorts(sshPort, mcpPort);
    VhNetInit(pVm, fHostFwd);
    VhVirtioBlkInit(pszDisk);
    VhVirtioBlkDataInit(pszDataDisk, cbDataGb);

    bool fRestored = false;
    if (pszRestore)
    {
        if (!VhSnapRestore(pVm, pszRestore, &snapRefs))
            fRestored = true; /* restore already fixed up device callbacks */
        else
            VhLog("vmhost: snapshot restore failed; performing clean boot");
    }
    if (!fRestored)
    {
        /* A bad/incompatible checkpoint may have loaded partial registers,
           device state and lazy pages. Recreate the partition (rather than
           unmapping a sparse GPA range) before starting a clean guest. */
        if (pszRestore)
        {
            for (u32 i = 0; i < g_cDynMmioPages; i++)
                VirtualFree(g_aDynMmio[i].pvPage, 0, MEM_RELEASE);
            g_cDynMmioPages = 0;
            VhWhvTerm(pVm);
            memset(pVm->mem.pbRam, 0, (size_t)pVm->mem.cbRam);
            memset(pVm->mem.pbScratch, 0, (size_t)pVm->mem.cbScratch);
            VhDevResetState(pVm);
            if (VhWhvInit(pVm, 1) || VhWhvMapRam(pVm) || VhWhvCreateCpu(pVm))
            {
                VhLog("vmhost: could not reset partition for cold boot");
                return 1;
            }
        }
        if (VhGuestLoad(pVm, pszKernel, pszInitrd, pszCmdLine))
            return 1;
        if (VhGuestSetupCpu(pVm))
            return 1;
    }

    VhLog("vmhost: starting VM (engine pid %lu)", (unsigned long)GetCurrentProcessId());

    HANDLE hTimerThread = (HANDLE)_beginthreadex(NULL, 0, TimerThreadProc, pVm, 0, NULL);
    HANDLE hInputThread = (HANDLE)_beginthreadex(NULL, 0, InputThreadProc, pVm, 0, NULL);
    if (pszIpc)
        VhIpcStart(pVm, pszIpc);
    if (!fNoUi)
        VhViewerStart(pVm);

    u64 cExits = 0;
    u64 cHalts = 0;
    u64 cIo = 0;
    u64 cCpuid = 0;
    u64 cMsr = 0;
    u64 cCanceled = 0;
    u64 cIntWin = 0;
    u64 cExc = 0;
    u32 cHaltsWithoutProgress = 0;
    u64 uLastProgressNs = VhGuestNowNs();
    u32 cStallReports = 0;

    /* on-demand MMIO pages are kept at file scope for snapshot hooks */

    while (!pVm->fStop)
    {
        if (pVm->fSnapWanted && !pVm->fSnapWorking)
        {
            pVm->fSnapWorking = true;
            VhSnapSave(pVm, pVm->szSnapPath, &snapRefs);
            pVm->fSnapWanted = false;
            pVm->fSnapWorking = false;
            continue;
        }
        if (pVm->fFbCopyWanted)
        {
            /* park the vCPU for one coherent frame capture */
            pVm->fFbCopyPaused = true;
            while (pVm->fFbCopyWanted && !pVm->fStop)
                Sleep(0);
            pVm->fFbCopyPaused = false;
            continue;
        }
        WHV_RUN_VP_EXIT_CONTEXT exitCtx;
        memset(&exitCtx, 0, sizeof(exitCtx));
        HRESULT hrc = WHvRunVirtualProcessor(pVm->hPartition, 0, &exitCtx, sizeof(exitCtx));
        if (FAILED(hrc))
        {
            VhLog("vmhost: WHvRunVirtualProcessor failed: %#lx - stopping", (unsigned long)hrc);
            pVm->fStop = true;
            break;
        }

        cExits++;
        pVm->fGuestRunning = false;
        pVm->uLastExitNs = VhGuestNowNs();
        u32 uReason = exitCtx.ExitReason;
        if ((cExits % 20000) == 0)
        {
            VhLog("vmhost: progress: exits=%llu reason=%#x rip=%#llx rflags=%#llx io=%llu",
                  (unsigned long long)cExits, uReason,
                  (unsigned long long)exitCtx.VpContext.Rip,
                  (unsigned long long)exitCtx.VpContext.Rflags,
                  (unsigned long long)cIo);
            VhLogHotIoPorts();
        }

        switch (uReason)
        {
            case WHvRunVpExitReasonX64IoPortAccess:
                cIo++;
                if (!VhHandleIoPort(pVm, &exitCtx))
                    pVm->fStop = true;
                break;

            case WHvRunVpExitReasonX64Halt:
                cHalts++;
                if (cHalts <= 5)
                    VhLog("vmhost: guest halted (RIP=%#llx, rflags=%#llx)",
                          (unsigned long long)exitCtx.VpContext.Rip,
                          (unsigned long long)exitCtx.VpContext.Rflags);
                if (++cHaltsWithoutProgress > 1000000)
                {
                    VhLog("vmhost: guest stuck in HLT with no interrupts - stopping");
                    pVm->fStop = true;
                    break;
                }
                /* wait for an interrupt, then let the pre-run path inject it */
                while (!pVm->fStop && !VhDevHasPendingIrq(pVm) && !pVm->fFbCopyWanted)
                    Sleep(1);
                VhWhvClearHaltSuspend(pVm);
                break;

            case WHvRunVpExitReasonX64InterruptWindow:
                cIntWin++;
                pVm->fIrqWindowArmed = false;
                break;

            case WHvRunVpExitReasonX64Cpuid:
                cCpuid++;
                VhHandleCpuid(pVm, &exitCtx);
                break;

            case WHvRunVpExitReasonX64MsrAccess:
                cMsr++;
                VhHandleMsr(pVm, &exitCtx);
                break;

            case WHvRunVpExitReasonException:
                cExc++;
                cHaltsWithoutProgress = 0;
                VhHandleException(pVm, &exitCtx);
                break;

            case WHvRunVpExitReasonCanceled:
                cCanceled++;
                if (VhGuestNowNs() - uLastProgressNs > 5000000000ull
                    && cStallReports++ < 8)
                {
                    WHV_REGISTER_VALUE aVal[3];
                    WHV_REGISTER_NAME aName[3] = { WHvX64RegisterRip, WHvX64RegisterRflags, WHvX64RegisterCs };
                    memset(aVal, 0, sizeof(aVal));
                    if (VhWhvGetRegs(pVm, aName, aVal, 3) == 0)
                        VhLog("vmhost: stall: guest has made no progress for >5s (rip=%#llx rflags=%#llx cs=%#x)",
                              (unsigned long long)aVal[0].Reg64, (unsigned long long)aVal[1].Reg64,
                              (unsigned)aVal[2].Segment.Selector);
                    else
                        VhLog("vmhost: stall: guest has made no progress for >5s (state query failed)");
                }
                break;

            case WHvRunVpExitReasonMemoryAccess:
            {
                u64 uGpa = exitCtx.MemoryAccess.Gpa;
                u32 uAccessType = exitCtx.MemoryAccess.AccessInfo.AccessType;   /* 0=read 1=write 2=exec */
                if (pVm->fSnapRamLazy && uGpa < pVm->mem.cbRam &&
                    !(uGpa >= 0xA0000 && uGpa < 0x100000))
                {
                    if (VhSnapLazyLoad(pVm, uGpa))
                    {
                        VhLog("vmhost: snapshot page-in failed at %#llx",
                              (unsigned long long)uGpa);
                        pVm->fStop = true;
                    }
                    break; /* retry the same guest instruction after page-in */
                }
                if (uAccessType == 2 || g_cDynMmioPages >= VMHOST_MAX_DYN_MMIO_PAGES)
                {
                    VhLog("vmhost: unexpected memory access exit at GPA %#llx (type=%u rip=%#llx %u instr bytes: %02x %02x %02x %02x) - stopping",
                          (unsigned long long)uGpa, uAccessType,
                          (unsigned long long)exitCtx.VpContext.Rip,
                          exitCtx.MemoryAccess.InstructionByteCount,
                          exitCtx.MemoryAccess.InstructionBytes[0], exitCtx.MemoryAccess.InstructionBytes[1],
                          exitCtx.MemoryAccess.InstructionBytes[2], exitCtx.MemoryAccess.InstructionBytes[3]);
                    pVm->fStop = true;
                    break;
                }

                u64 uPage = uGpa & ~(u64)0xFFF;
                bool fKnown = false;
                for (u32 i = 0; i < g_cDynMmioPages; i++)
                    if (g_aDynMmio[i].uGpa == uPage)
                    {
                        fKnown = true;
                        break;
                    }

                if (!fKnown)
                {
                    void *pvPage = VirtualAlloc(NULL, 0x1000, MEM_COMMIT, PAGE_READWRITE);
                    if (!pvPage)
                    {
                        VhLog("vmhost: out of memory for MMIO page @ %#llx - stopping", (unsigned long long)uGpa);
                        pVm->fStop = true;
                        break;
                    }
                    memset(pvPage, 0xFF, 0x1000);
                    if (VhWhvMapMmioPage(pVm, uPage, pvPage, 0x1000))
                    {
                        VirtualFree(pvPage, 0, MEM_RELEASE);
                        VhLog("vmhost: failed to map MMIO page for GPA %#llx - stopping", (unsigned long long)uGpa);
                        pVm->fStop = true;
                        break;
                    }
                    g_aDynMmio[g_cDynMmioPages].uGpa = uPage;
                    g_aDynMmio[g_cDynMmioPages].pvPage = pvPage;
                    g_cDynMmioPages++;
                    VhLog("vmhost: mmio: mapped %s @ %#llx as absent-device RAM (rip=%#llx)",
                          uAccessType == 1 ? "write" : "read",
                          (unsigned long long)uGpa, (unsigned long long)exitCtx.VpContext.Rip);
                }
                /* resume; the instruction re-executes against the new mapping */
                break;
            }

            case WHvRunVpExitReasonUnrecoverableException:
                VhLog("vmhost: unrecoverable exception in guest - stopping");
                pVm->fStop = true;
                break;

            case WHvRunVpExitReasonInvalidVpRegisterValue:
            case WHvRunVpExitReasonUnsupportedFeature:
                VhLog("vmhost: fatal vCPU condition (reason %#x) - stopping", uReason);
                pVm->fStop = true;
                break;

            default:
                VhLog("vmhost: unhandled exit reason %#x - stopping", uReason);
                pVm->fStop = true;
                break;
        }

        if (uReason != WHvRunVpExitReasonX64Halt)
            cHaltsWithoutProgress = 0;
        if (uReason != WHvRunVpExitReasonCanceled)
        {
            uLastProgressNs = VhGuestNowNs();
            cStallReports = 0;
        }

        /* update guest interruptibility from this exit's vCPU context */
        pVm->fGuestIntrPending = !!exitCtx.VpContext.ExecutionState.InterruptionPending;
        pVm->fGuestInterruptable = !exitCtx.VpContext.ExecutionState.InterruptShadow;
        pVm->fGuestHasIf = !!(exitCtx.VpContext.Rflags & 0x200);

        if (!pVm->fStop)
            VhUpdateInterrupts(pVm);
    }

    pVm->fStop = true;
    VhWhvCancelRun(pVm);
    if (hInputThread)
        WaitForSingleObject(hInputThread, 500);
    if (hTimerThread)
    {
        WaitForSingleObject(hTimerThread, 100);
        CloseHandle(hTimerThread);
    }
    if (hInputThread)
        CloseHandle(hInputThread);

    /* MMIO state is part of the checkpoint; keep its pages until saved. */
    VhLog("vmhost: stopped after %llu exits (io=%llu cpuid=%llu msr=%llu halt=%llu intwin=%llu exc=%llu canceled=%llu), %llu timer ticks",
          (unsigned long long)cExits, (unsigned long long)cIo, (unsigned long long)cCpuid,
          (unsigned long long)cMsr, (unsigned long long)cHalts, (unsigned long long)cIntWin,
          (unsigned long long)cExc, (unsigned long long)cCanceled, (unsigned long long)pVm->cTicks);

    /* Never replace a good snapshot with CPU/device state after an engine crash.
       Only a host-initiated, clean stop is safe to checkpoint. */
    int fSnapError = 0;
    if (pszSnapshotOut && pVm->fCleanExit)
    {
        VhLog("vmhost: saving final snapshot to %s", pszSnapshotOut);
        if (VhSnapSave(pVm, pszSnapshotOut, &snapRefs))
        {
            VhLog("vmhost: snapshot save failed; next start will cold boot");
            fSnapError = 1;
        }
    }
    else if (pszSnapshotOut)
        VhLog("vmhost: unclean exit; not saving snapshot");

    for (u32 i = 0; i < g_cDynMmioPages; i++)
        VirtualFree(g_aDynMmio[i].pvPage, 0, MEM_RELEASE);
    VhWhvTerm(pVm);
    return fSnapError;
}
