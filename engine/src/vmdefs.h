/*
 * Soil VM engine (vmhost) - shared definitions.
 *
 * Re-implements, in miniature, the user-mode virtualization approach used by
 * Oracle VirtualBox on Windows: the VM runs on the Windows Hypervisor
 * Platform (WHPX) exactly like VirtualBox's NEM backend, with host-side
 * device models standing in for VirtualBox's DevPIC/DevPIT/DevUART/DevPS2/
 * DevRTC devices of the VMM.
 *
 * The guest boots directly from a bzImage (vmlinuz) + initrd (core.gz)
 * using the Linux x86 32-bit boot protocol - no PC BIOS/firmware emulation.
 */
#ifndef VMDEFS_H
#define VMDEFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <windows.h>
#include <winhvplatform.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

#define VH_PAGE 4096
#define VH_ALIGN_UP(v, a)   (((v) + ((a) - 1)) & ~((u64)((a) - 1)))
#define VH_NELEMS(a)        (sizeof(a) / sizeof((a)[0]))

/* Boot layout (guest physical addresses). */
#define VH_BOOTPARAMS_GPA   0x00010000ULL
#define VH_CMDLINE_GPA      0x00020000ULL
#define VH_CMDLINE_SIZE     0x1000
#define VH_INITRD_GPA       0x04000000ULL          /* 64 MiB */
#define VH_ENTRY_STACK      0x0009F000ULL
#define VH_KERNEL_GPA       0x00100000ULL          /* 1 MiB */

/* Framebuffer. */
#define VH_FB_W             1360
#define VH_FB_H             768
#define VH_FB_BPP           32
#define VH_FB_STRIDE        (VH_FB_W * (VH_FB_BPP / 8))
#define VH_FB_SIZE          (VH_FB_STRIDE * VH_FB_H)   /* 4 MiB */
#define VH_FB_TAIL          0x00800000ULL              /* reserve 8 MiB at top of RAM for fb */

/* Serial (COM1). */
#define VH_UART_IRQ         4

/* virtio-blk (PCI INT A line reported at 0x3C) */
#define VH_VBLK_IRQ         11
#define VH_VBLK_DATA_IRQ    10

/* ------------------------------------------------------------------ */
/* Guest memory                                                        */
/* ------------------------------------------------------------------ */
typedef struct VH_MEM
{
    u8 *pbRam;              /* host view of guest RAM (GPA 0..cbRam) */
    u64 cbRam;
    u8 *pbScratch;          /* backs legacy VGA/BIOS holes 0xA0000..0x100000 */
    u64 cbScratch;          /* 384 KiB */
    u64 gpaKernel;
    u64 cbKernel;
    u64 gpaInitrd;
    u64 cbInitrd;
    u64 gpaFb;
    u64 cbFb;
} VH_MEM;

/* ------------------------------------------------------------------ */
/* 8259 PIC pair                                                       */
/* ------------------------------------------------------------------ */
typedef struct VH_PIC_CHIP
{
    u8  imr;                /* interrupt mask register */
    u8  irr;                /* interrupt request register */
    u8  isr;                /* in-service register */
    u8  vectorBase;         /* ICW2 */
    u8  initState;          /* 0=none,1=ICW1,2=ICW2,3=ICW3,4=ICW4 */
    u8  icw1;
    u8  readIsr;            /* OCW3: 1 -> read ISR, 0 -> read IRR */
} VH_PIC_CHIP;

typedef struct VH_PIC
{
    VH_PIC_CHIP master;
    VH_PIC_CHIP slave;
    bool fSlaveInIrq2;      /* slave has 8..15 pending */
    u8  slavePendingVector; /* latched slave vector for cascade delivery */
} VH_PIC;

/* ------------------------------------------------------------------ */
/* 8254 PIT                                                            */
/* ------------------------------------------------------------------ */
typedef struct VH_PIT_CHAN
{
    u8  mode;               /* 0..5 */
    u8  rwMode;             /* 1=lsb,2=msb,3=lsb/msb */
    u8  readFlip;           /* for 2-byte reads */
    u8  writeFlip;
    u8  writeLatch;         /* first byte of 2-byte write */
    bool fNullCount;
    u64 startNs;            /* QPC ns when programmed */
    u64 periodNs;           /* one full period */
    u16 reload;             /* divisor */
    u16 latchValue;         /* latched count for readback */
    bool fLatched;
    u16 curValue;           /* last reported value */
} VH_PIT_CHAN;

typedef struct VH_PIT
{
    VH_PIT_CHAN ch[3];
    u8  controlRecent;      /* last control word per chan not tracked; use per-channel modes */
    u64 lastTickNs;
    u64 nsAccum;
    u8  speakerPort;        /* port 0x61 */
    /* Catch-up bookkeeping is part of the saved VM state. */
    u64 cPeriods;
    u64 startNsSeen;
} VH_PIT;

/* ------------------------------------------------------------------ */
/* MC146818 RTC                                                        */
/* ------------------------------------------------------------------ */
typedef struct VH_RTC
{
    u8 index;
    u8 regA;
    u8 regB;
} VH_RTC;

/* ------------------------------------------------------------------ */
/* 16550 UART                                                          */
/* ------------------------------------------------------------------ */
#define VH_UART_FIFO 256
typedef struct VH_UART
{
    u8  regs[8];            /* 0=THR/RBR 1=IER 2=IIR 3=LCR 4=MCR 5=LSR 6=MSR 7=SCR */
    u8  dll, dlm;
    u8  fcr;
    u8  rxBuf[VH_UART_FIFO];
    u32 iRxHead, iRxTail;
    u8  txFifo[VH_UART_FIFO];
    u32 iTxHead, iTxTail;
    void (*pfnOut)(void *pvUser, u8 b);
    void *pvOutUser;
} VH_UART;

/* ------------------------------------------------------------------ */
/* i8042 PS/2 controller                                               */
/* ------------------------------------------------------------------ */
#define VH_PS2_FIFO 256
typedef struct VH_PS2_PORT
{
    u8  outBuf[VH_PS2_FIFO];
    u32 iHead, iTail;
    u8  pendingCmd;         /* 0 none, else awaiting parameter */
    u8  pendingCmdByte;
    u8  pendingBytesNeeded;
    u8  reply[16];          /* queued reply bytes to output buffer */
    u32 cReply;
    u8  fEnabled;
    u8  fScaling;
    u8  sampleRate;
    u8  resolution;
} VH_PS2_PORT;

typedef struct VH_I8042
{
    u8  config;             /* controller config byte */
    u8  statusFlags;        /* sticky status bits (e.g. self-test ok) */
    u8  cmdByte;            /* last command */
    bool fWaitForData;      /* next 0x60 write is command (0x64=D4)? port60Target: 0=cmd,1=kbd,2=aux */
    u8  port60Target;
    u8  selfTestReply;      /* pending 0x55 to 0x60 */
    bool fA20;
    VH_PS2_PORT kbd;
    VH_PS2_PORT aux;
    bool fMouseStreaming;
} VH_I8042;

/* ------------------------------------------------------------------ */
/* IRQ controller glue                                                 */
/* ------------------------------------------------------------------ */
typedef struct VH_IRQS
{
    VH_PIC pic;
    bool fIrqPending;       /* raised lines not yet delivered */
} VH_IRQS;

/* ------------------------------------------------------------------ */
/* Whole VM                                                            */
/* ------------------------------------------------------------------ */
typedef struct VH_VM
{
    WHV_PARTITION_HANDLE hPartition;

    VH_MEM  mem;
    VH_PIT  pit;
    VH_RTC  rtc;
    VH_UART uart;
    VH_I8042 i8042;
    VH_IRQS irqs;

    char szCmdLine[VH_CMDLINE_SIZE];
    u8   uVgaCrtIndex;      /* dummy VGA CRTC index register */

    volatile bool fStop;
    volatile bool fHaltPending;   /* guest executed HLT */
    volatile bool fGuestRunning;  /* inside WHvRunVirtualProcessor */
    volatile u64  cTicks;         /* PIC timer ticks delivered */

    u64 nsBoot;
    FILE *pLog;             /* engine log (stderr or file) */

    /* interrupt injection state (QEMU WHPX PIC-mode model) */
    volatile bool fIrqWindowArmed;   /* deliverability notification armed */
    bool fGuestIntrPending;          /* InterruptionPending from last exit ctx */
    bool fGuestInterruptable;        /* !InterruptShadow from last exit ctx */
    bool fGuestHasIf;                /* RFLAGS.IF from last exit ctx */
    volatile u64 uLastExitNs;        /* timestamp of the last VM exit */

    /* atomic framebuffer capture handshake (viewer/timer <-> run loop) */
    volatile bool fFbCopyWanted;   /* capture pending: run loop must park */
    volatile bool fFbCopyPaused;   /* run loop parked outside WHvRunVirtualProcessor */

    /* Snapshot coordination between the IPC, timer, and VM run-loop threads. */
    char szSnapPath[1024];
    volatile bool fSnapWanted;
    volatile bool fSnapWorking;
    volatile bool fCleanExit;
    bool fSnapRamLazy;
    FILE *pFileLazy;
    u8 *pbRamLoaded;
    u64 cLazyChunks;
} VH_VM;

/* ------------------------------------------------------------------ */
/* log.c                                                               */
/* ------------------------------------------------------------------ */
void VhLog(const char *pszFmt, ...);
void VhLogSetFile(FILE *pFile);
void VhLogTeeConsole(FILE *pFile, const void *pv, size_t cb);

/* ------------------------------------------------------------------ */
/* winhv.c                                                             */
/* ------------------------------------------------------------------ */
int  VhWhvInit(VH_VM *pVm, u32 cCpus);
void VhWhvTerm(VH_VM *pVm);
int  VhWhvMapRam(VH_VM *pVm);
int  VhWhvMapMmioPage(VH_VM *pVm, u64 uGpa, void *pvPage, u64 cb);
int  VhWhvCreateCpu(VH_VM *pVm);
int  VhWhvSetReg(VH_VM *pVm, WHV_REGISTER_NAME enm, WHV_REGISTER_VALUE val);
int  VhWhvSetRegs(VH_VM *pVm, const WHV_REGISTER_NAME *paenm, const WHV_REGISTER_VALUE *pav, u32 c);
int  VhWhvGetReg(VH_VM *pVm, WHV_REGISTER_NAME enm, WHV_REGISTER_VALUE *pVal);
int  VhWhvGetRegs(VH_VM *pVm, const WHV_REGISTER_NAME *paenm, WHV_REGISTER_VALUE *pav, u32 c);
int  VhWhvSetSegment(VH_VM *pVm, WHV_REGISTER_NAME enm, u64 uBase, u32 uLimit, u16 uSelector,
                     u16 uType, bool fCode64, bool fDb, bool fGran, u8 uDpl);
/* Inject a hardware interrupt vector via the pending-interruption register. */
int  VhWhvInjectInterrupt(VH_VM *pVm, u8 bVector);
int  VhWhvInjectException(VH_VM *pVm, u8 bVector, u32 uErrorCode, bool fHasErrorCode);
int  VhWhvSetDeliverabilityNotification(VH_VM *pVm, bool fEnable);
int  VhWhvRequestInterrupt(VH_VM *pVm, u8 bVector);
int  VhWhvClearHaltSuspend(VH_VM *pVm);
int  VhWhvCancelRun(VH_VM *pVm);

/* ------------------------------------------------------------------ */
/* devices.c                                                           */
/* ------------------------------------------------------------------ */
void VhDevInit(VH_VM *pVm);
void VhDevResetState(VH_VM *pVm);
void VhDevPostRestoreFixup(VH_VM *pVm);
/* Returns true if the port was handled. */
bool VhDevIoPort(VH_VM *pVm, u16 uPort, bool fWrite, u32 cbWidth, u32 *puValue);
void VhDevRaiseIrq(VH_VM *pVm, u8 bIrq);
void VhDevLowerIrq(VH_VM *pVm, u8 bIrq);
/* Get the next interrupt vector to inject (0 = none). Acknowledges the PIC. */
u8   VhDevPicGetVector(VH_VM *pVm);
/* Undo an acknowledgement when delivery failed (re-raise the request). */
void VhDevPicRequeueVector(VH_VM *pVm, u8 bVector);
/* Acknowledge a specific vector (used when delivery bypassed VhDevPicGetVector). */
void VhDevPicAckVector(VH_VM *pVm, u8 bVector);
void VhDevRegisterInjection(VH_VM *pVm, u8 bVector);  /* called after successful injection */
bool VhDevHasPendingIrq(VH_VM *pVm);
void VhDevPitTick(VH_VM *pVm, u64 nsNow);
void VhDevUartWriteGuest(VH_VM *pVm, const void *pv, u32 cb);   /* host -> guest RX */
void VhDevUartGuestOutByte(VH_VM *pVm, u8 b);                   /* guest TX -> host */
void VhDevKbdQueueScancodes(VH_VM *pVm, const u8 *pb, u32 cb);
void VhDevMouseQueuePacket(VH_VM *pVm, const u8 *pb, u32 cb);

/* ------------------------------------------------------------------ */
/* viewer.c - interactive host window (live framebuffer + real input)  */
/* ------------------------------------------------------------------ */
void VhViewerStart(VH_VM *pVm);

/* ------------------------------------------------------------------ */
/* ipc.c - app transport (shared framebuffer + named-pipe input)       */
/* ------------------------------------------------------------------ */
typedef struct VH_IPC_SHARED
{
    volatile LONG uSeq;         /* incremented after each complete frame */
    volatile LONG uStop;        /* host asks the VM to power off */
    u32 uWidth;
    u32 uHeight;
    u32 uStride;
    u32 uReserved;
    u8  abFrame[VH_FB_SIZE];
    /* Snapshot command tail is beyond the fixed app framebuffer view. */
    volatile LONG uCmd;
    volatile LONG cCmdHave;
    char szCmd[1024];
} VH_IPC_SHARED;

void VhIpcStart(VH_VM *pVm, const char *pszToken);

#define VH_LAZY_CHUNK 0x10000ULL
int VhSnapLazyLoad(VH_VM *pVm, u64 uGpa);

/* ------------------------------------------------------------------ */
/* vmhost.c                                                            */
/* ------------------------------------------------------------------ */
/* Copy one coherent frame of the guest framebuffer to pbDst. Parks the
   vCPU between runs so the capture cannot tear mid-update. */
void VhFbSnapshot(VH_VM *pVm, u8 *pbDst);

/* ------------------------------------------------------------------ */
/* guest.c                                                             */
/* ------------------------------------------------------------------ */
int  VhGuestLoad(VH_VM *pVm, const char *pszKernel, const char *pszInitrd, const char *pszCmdLine);
int  VhGuestSetupCpu(VH_VM *pVm);
u64  VhGuestNowNs(void);

/* ------------------------------------------------------------------ */
/* pci.c                                                               */
/* ------------------------------------------------------------------ */
void VhPciInit(void);
bool VhPciIo(VH_VM *pVm, u16 uPort, bool fWrite, u32 cbWidth, u32 *puValue);
u16  VhPciNicIoBase(void);
u16  VhPciBlkIoBase(void);
u16  VhPciBlkDataIoBase(void);
u32  VhPciSaveState(const u32 **ppbData);
void VhPciLoadState(const u32 *pbData, u32 cWords);

/* ------------------------------------------------------------------ */
/* virtio_blk.c                                                        */
/* ------------------------------------------------------------------ */
void VhVirtioBlkInit(const char *pszPath);
void VhVirtioBlkDataInit(const char *pszPath, u32 cbGb);
bool VhVirtioBlkPresent(void);
bool VhVirtioBlkDataPresent(void);
bool VhVirtioBlkIo(VH_VM *pVm, u32 uOffset, bool fWrite, u32 cbWidth, u32 *puValue);
bool VhVirtioBlkDataIo(VH_VM *pVm, u32 uOffset, bool fWrite, u32 cbWidth, u32 *puValue);
int  VhVirtioBlkSaveState(const u8 **ppb);
void VhVirtioBlkLoadState(const u8 *pb);
int  VhVirtioBlkDataSaveState(const u8 **ppb);
void VhVirtioBlkDataLoadState(const u8 *pb);

/* ------------------------------------------------------------------ */
/* ne2000.c                                                            */
/* ------------------------------------------------------------------ */
void Ne2000Init(VH_VM *pVm);
bool Ne2000IoAccess(VH_VM *pVm, u32 uOffset, bool fWrite, u32 cbWidth, u32 *puValue);
void Ne2000Receive(const u8 *pb, u32 cb);
void Ne2000BindVm(VH_VM *pVm);
int  Ne2000SaveState(const u8 **ppb);
void Ne2000LoadState(const u8 *pb);
int  Ne2000CanReceive(void);

/* snapshot.c - VM state save/restore */
typedef struct VhSnapImgRefs
{
    const char *pszKernel;
    const char *pszInitrd;
    const char *pszDisk;
    const char *pszDataDisk;
} VhSnapImgRefs;
int  VhSnapSave(VH_VM *pVm, const char *pszPath, const VhSnapImgRefs *pRefs);
int  VhSnapRestore(VH_VM *pVm, const char *pszPath, const VhSnapImgRefs *pRefs);
int  VhHostSnapSaveExtra(VH_VM *pVm, FILE *pFile);
int  VhHostSnapRestoreExtra(VH_VM *pVm, FILE *pFile, u32 cbPayload);

/* ------------------------------------------------------------------ */
/* netglue.c                                                           */
/* ------------------------------------------------------------------ */
void VhNetInit(VH_VM *pVm, bool fHostForward);
void VhNetSetHostFwdPorts(int sshPort, int mcpPort);
void VhNetPoll(void);
int  VhNetInput(const void *pv, int cb);

/* ------------------------------------------------------------------ */
/* devices.c - exported device lock (serializes with net poll thread)  */
/* ------------------------------------------------------------------ */
void VhDevLock(void);
void VhDevUnlock(void);

#endif /* VMDEFS_H */
