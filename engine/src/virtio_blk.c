/*
 * Soil VM engine - minimal virtio-blk (legacy PCI, PIO) disks.
 *
 * Two devices are supported: the read-only squashfs payload and a writable
 * per-machine data disk. The queue is processed synchronously on the vCPU
 * thread when the guest kicks VIRTIO_PCI_QUEUE_NOTIFY.
 */
#include "vmdefs.h"
#include <string.h>
#include <winioctl.h>

#define VBLK_Q_SIZE   256
#define VBLK_SECTOR   512u
#define VBLK_MAX_SEGS 256

/* legacy virtio PCI register offsets inside the I/O BAR */
#define VBLK_R_FEATURES   0x00
#define VBLK_R_GFEATURES  0x04
#define VBLK_R_PFN        0x08
#define VBLK_R_QSIZE      0x0C
#define VBLK_R_QSEL       0x0E
#define VBLK_R_NOTIFY     0x10
#define VBLK_R_STATUS     0x12
#define VBLK_R_ISR        0x13
#define VBLK_R_CONFIG     0x14

#define VIRTIO_BLK_F_RO    (1u << 5)
#define VIRTIO_BLK_F_FLUSH (1u << 9)

#define VBLK_T_IN         0u   /* read from disk into guest */
#define VBLK_T_OUT        1u   /* write from guest to disk */
#define VBLK_T_FLUSH      4u

#define VBLK_S_OK         0u
#define VBLK_S_IOERR      1u
#define VBLK_S_UNSUPP     2u

#define VRING_DESC_F_NEXT  1u
#define VRING_DESC_F_WRITE 2u
#define VRING_AVAIL_F_NO_INTERRUPT 1u
#define VIRTIO_CONFIG_S_DRIVER_OK  4u

typedef struct VBLK_DESC
{
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
} VBLK_DESC;

typedef struct VBLK_SEG
{
    u64 gpa;
    u32 len;
    u16 flags;
} VBLK_SEG;

typedef struct VBLK_STATE
{
    HANDLE hFile;
    HANDLE hMap;           /* section for the image */
    u8    *pbView;         /* mapped view base (NULL -> ReadFile fallback) */
    u64    cbDisk;
    u64    cSectors;
    bool   fPresent;
    bool   fWritable;
    u32    uIrq;

    u8     bStatus;
    u16    uQSel;
    u32    uPfn;
    u32    uGuestFeatures;
    u8     bIsr;
    u16    uLastAvail;

    u32    cLog;           /* diagnostics: bar writes */
    u32    cKicks;
    u32    cDone;
    u32    cRaised;
    u32    cIsrReads;
    u32    cReqs;
} VBLK_STATE;

static VBLK_STATE s_blk;
static VBLK_STATE s_blkData;

static int vblkSaveState(VBLK_STATE *state, const u8 **ppb)
{
    *ppb = (const u8 *)state;
    return (int)sizeof(*state);
}

static void vblkLoadState(VBLK_STATE *state, const u8 *pb)
{
    VBLK_STATE saved;
    memcpy(&saved, pb, sizeof(saved));
    state->bStatus = saved.bStatus;
    state->uQSel = saved.uQSel;
    state->uPfn = saved.uPfn;
    state->uGuestFeatures = saved.uGuestFeatures;
    state->bIsr = saved.bIsr;
    state->uLastAvail = saved.uLastAvail;
    state->cLog = saved.cLog;
    state->cKicks = saved.cKicks;
    state->cDone = saved.cDone;
    state->cRaised = saved.cRaised;
    state->cIsrReads = saved.cIsrReads;
    state->cReqs = saved.cReqs;
}

int VhVirtioBlkSaveState(const u8 **ppb)
{
    return vblkSaveState(&s_blk, ppb);
}

void VhVirtioBlkLoadState(const u8 *pb)
{
    vblkLoadState(&s_blk, pb);
}

int VhVirtioBlkDataSaveState(const u8 **ppb)
{
    return vblkSaveState(&s_blkData, ppb);
}

void VhVirtioBlkDataLoadState(const u8 *pb)
{
    vblkLoadState(&s_blkData, pb);
}

static void vblkInit(VBLK_STATE *p, const char *pszPath, bool fWritable,
                     u64 cbRequested, u32 uIrq)
{
    if (!pszPath)
        return;

    DWORD dwAccess = GENERIC_READ | (fWritable ? GENERIC_WRITE : 0);
    DWORD dwDisposition = fWritable ? OPEN_ALWAYS : OPEN_EXISTING;
    HANDLE hFile = CreateFileA(pszPath, dwAccess, FILE_SHARE_READ, NULL,
                               dwDisposition, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        VhLog("vblk: cannot open disk image '%s' (error %lu)", pszPath,
              (unsigned long)GetLastError());
        return;
    }

    if (fWritable)
    {
        DWORD cbReturned = 0;
        DeviceIoControl(hFile, FSCTL_SET_SPARSE, NULL, 0, NULL, 0,
                        &cbReturned, NULL);

        LARGE_INTEGER liNow;
        if (GetFileSizeEx(hFile, &liNow) &&
            cbRequested > (u64)liNow.QuadPart)
        {
            LARGE_INTEGER liSize;
            liSize.QuadPart = (LONGLONG)cbRequested;
            if (SetFilePointerEx(hFile, liSize, NULL, FILE_BEGIN))
                SetEndOfFile(hFile);
        }
    }

    LARGE_INTEGER liSize;
    if (!GetFileSizeEx(hFile, &liSize) || liSize.QuadPart < VBLK_SECTOR)
    {
        VhLog("vblk: disk image '%s' is missing or too small", pszPath);
        CloseHandle(hFile);
        return;
    }

    p->hFile = hFile;
    p->cbDisk = (u64)liSize.QuadPart;
    p->cSectors = p->cbDisk / VBLK_SECTOR;
    p->fPresent = true;
    p->fWritable = fWritable;
    p->uIrq = uIrq;

    DWORD dwProtect = fWritable ? PAGE_READWRITE : PAGE_READONLY;
    DWORD dwMapAccess = fWritable ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ;
    HANDLE hMap = CreateFileMappingA(hFile, NULL, dwProtect, 0, 0, NULL);
    if (hMap)
    {
        void *pv = MapViewOfFile(hMap, dwMapAccess, 0, 0, 0);
        if (pv)
        {
            p->hMap = hMap;
            p->pbView = (u8 *)pv;
        }
        else
        {
            CloseHandle(hMap);
        }
    }

    VhLog("vblk: attached '%s' (%llu bytes, %llu sectors, %s, %s)",
          pszPath, (unsigned long long)p->cbDisk,
          (unsigned long long)p->cSectors,
          fWritable ? "writable" : "read-only",
          p->pbView ? "mapped" : "buffered");
}

void VhVirtioBlkInit(const char *pszPath)
{
    vblkInit(&s_blk, pszPath, false, 0, VH_VBLK_IRQ);
}

void VhVirtioBlkDataInit(const char *pszPath, u32 cbGb)
{
    vblkInit(&s_blkData, pszPath, true, (u64)cbGb << 30, VH_VBLK_DATA_IRQ);
}

bool VhVirtioBlkPresent(void)
{
    return s_blk.fPresent;
}

bool VhVirtioBlkDataPresent(void)
{
    return s_blkData.fPresent;
}

static bool vblkReadDisk(VBLK_STATE *p, u8 *pbDst, u64 uOff, u32 cb)
{
    if (p->pbView)
    {
        if (uOff + cb > p->cbDisk)
            return false;
        memcpy(pbDst, p->pbView + uOff, cb);
        return true;
    }

    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)uOff;
    if (!SetFilePointerEx(p->hFile, li, NULL, FILE_BEGIN))
        return false;

    while (cb)
    {
        DWORD cbChunk = cb > (1u << 20) ? (1u << 20) : cb;
        DWORD cbRead = 0;
        if (!ReadFile(p->hFile, pbDst, cbChunk, &cbRead, NULL) || cbRead != cbChunk)
            return false;
        pbDst += cbChunk;
        cb -= cbChunk;
    }
    return true;
}

static bool vblkWriteDisk(VBLK_STATE *p, const u8 *pbSrc, u64 uOff, u32 cb)
{
    if (!p->fWritable)
        return false;

    if (p->pbView)
    {
        if (uOff + cb > p->cbDisk)
            return false;
        memcpy(p->pbView + uOff, pbSrc, cb);
        return true;
    }

    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)uOff;
    if (!SetFilePointerEx(p->hFile, li, NULL, FILE_BEGIN))
        return false;

    while (cb)
    {
        DWORD cbChunk = cb > (1u << 20) ? (1u << 20) : cb;
        DWORD cbWrite = 0;
        if (!WriteFile(p->hFile, pbSrc, cbChunk, &cbWrite, NULL) || cbWrite != cbChunk)
            return false;
        pbSrc += cbChunk;
        cb -= cbChunk;
    }
    return true;
}

static bool vblkFlush(VBLK_STATE *p)
{
    if (!p->fWritable)
        return false;
    if (p->pbView)
        FlushViewOfFile(p->pbView, 0);
    return FlushFileBuffers(p->hFile) != 0;
}

static u32 rd32(const u8 *pb) { u32 v; memcpy(&v, pb, 4); return v; }
static u64 rd64(const u8 *pb) { u64 v; memcpy(&v, pb, 8); return v; }

/* Execute one request (descriptor chain head). Always records the number of
 * bytes written into guest memory in *pcbWritten so the used ring stays sane. */
static void vblkExecReq(VH_VM *pVm, VBLK_STATE *p, const u8 *pbRing,
                        u16 uHead, u32 *pcbWritten)
{
    VBLK_SEG aSeg[VBLK_MAX_SEGS];
    u32 cSeg = 0;
    u32 uIdx = uHead;
    bool fBad = false;

    for (;;)
    {
        VBLK_DESC d;
        if (uIdx >= VBLK_Q_SIZE || cSeg >= VBLK_MAX_SEGS)
        {
            fBad = true;
            break;
        }
        memcpy(&d, pbRing + 16 * uIdx, sizeof(d));
        aSeg[cSeg].gpa = d.addr;
        aSeg[cSeg].len = d.len;
        aSeg[cSeg].flags = d.flags;
        cSeg++;
        if (!(d.flags & VRING_DESC_F_NEXT))
            break;
        uIdx = d.next;
    }

    u8 *pbRam = pVm->mem.pbRam;
    u64 cbRam = pVm->mem.cbRam;

    /* the status byte lives in the final writable single-byte segment */
    VBLK_SEG *pStatus = NULL;
    if (cSeg && (aSeg[cSeg - 1].flags & VRING_DESC_F_WRITE) && aSeg[cSeg - 1].len >= 1 &&
        aSeg[cSeg - 1].gpa < cbRam)
        pStatus = &aSeg[cSeg - 1];

    if (fBad || cSeg < 2 || (aSeg[0].flags & VRING_DESC_F_WRITE) || aSeg[0].len < 16 ||
        aSeg[0].gpa + 16 > cbRam || !pStatus)
    {
        if (pStatus)
        {
            *(pbRam + pStatus->gpa) = VBLK_S_IOERR;
            *pcbWritten = 1;
        }
        else
            *pcbWritten = 0;
        return;
    }

    u32 uType = rd32(pbRam + aSeg[0].gpa);
    u64 uSector = rd64(pbRam + aSeg[0].gpa + 8);
    u8 bStatus = VBLK_S_OK;
    u32 cbOut = 0;

    if (++p->cReqs <= 6)
        VhLog("vblk: req #%u type=%u sector=%llu segs=%u", p->cReqs, uType,
              (unsigned long long)uSector, cSeg);

    if (uType == VBLK_T_IN)
    {
        u64 uOff = uSector * VBLK_SECTOR;
        for (u32 i = 1; i + 1 < cSeg; i++)
        {
            VBLK_SEG *s = &aSeg[i];
            if (!(s->flags & VRING_DESC_F_WRITE) || s->gpa + s->len > cbRam ||
                uOff + s->len > p->cbDisk)
            {
                bStatus = VBLK_S_IOERR;
                break;
            }
            if (s->len && !vblkReadDisk(p, pbRam + s->gpa, uOff, s->len))
            {
                bStatus = VBLK_S_IOERR;
                break;
            }
            uOff += s->len;
            cbOut += s->len;
        }
    }
    else if (uType == VBLK_T_OUT && p->fWritable)
    {
        u64 uOff = uSector * VBLK_SECTOR;
        for (u32 i = 1; i + 1 < cSeg; i++)
        {
            VBLK_SEG *s = &aSeg[i];
            if ((s->flags & VRING_DESC_F_WRITE) || s->gpa + s->len > cbRam ||
                uOff + s->len > p->cbDisk)
            {
                bStatus = VBLK_S_IOERR;
                break;
            }
            if (s->len && !vblkWriteDisk(p, pbRam + s->gpa, uOff, s->len))
            {
                bStatus = VBLK_S_IOERR;
                break;
            }
            uOff += s->len;
        }
    }
    else if (uType == VBLK_T_FLUSH && p->fWritable)
    {
        if (!vblkFlush(p))
            bStatus = VBLK_S_IOERR;
    }
    else
    {
        bStatus = VBLK_S_UNSUPP;
    }

    *(pbRam + pStatus->gpa) = bStatus;
    *pcbWritten = (uType == VBLK_T_IN ? cbOut : 0) + 1;
}

static void vblkKick(VH_VM *pVm, VBLK_STATE *p)
{
    if (!p->fPresent || !p->uPfn || !(p->bStatus & VIRTIO_CONFIG_S_DRIVER_OK))
        return;

    const u32 Q = VBLK_Q_SIZE;
    u64 gpaRing = (u64)p->uPfn << 12;
    u32 offAvail = 16 * Q;
    u32 offUsed = (u32)VH_ALIGN_UP((u64)offAvail + 6 + 2 * Q, VH_PAGE);
    u64 cbRing = VH_ALIGN_UP((u64)offUsed + 6 + 8 * Q, VH_PAGE);
    if (gpaRing + cbRing > pVm->mem.cbRam)
    {
        VhLog("vblk: virtqueue page %#x is outside guest RAM", p->uPfn);
        return;
    }

    u8 *pbRing = pVm->mem.pbRam + gpaRing;
    u8 *pbAvail = pbRing + offAvail;
    u8 *pbUsed = pbRing + offUsed;

    u16 uAvail = *(volatile u16 *)(pbAvail + 2);
    if (++p->cKicks <= 8)
        VhLog("vblk: kick #%u pfn=%#x lastAvail=%u avail=%u flags=%#x",
              p->cKicks, p->uPfn, p->uLastAvail, uAvail, *(volatile u16 *)pbAvail);
    if (uAvail == p->uLastAvail)
        return;

    bool fCompleted = false;
    while (p->uLastAvail != uAvail)
    {
        u16 uHead = *(u16 *)(pbAvail + 4 + 2 * (p->uLastAvail % Q));
        u32 cbWritten = 0;
        vblkExecReq(pVm, p, pbRing, uHead, &cbWritten);

        u16 uUsedIdx = *(volatile u16 *)(pbUsed + 2);
        u8 *pbEnt = pbUsed + 4 + 8 * (uUsedIdx % Q);
        *(u32 *)(pbEnt) = uHead;
        *(u32 *)(pbEnt + 4) = cbWritten;
        __sync_synchronize();
        *(volatile u16 *)(pbUsed + 2) = (u16)(uUsedIdx + 1);

        p->uLastAvail++;
        fCompleted = true;
        if (++p->cDone <= 8)
            VhLog("vblk: done #%u head=%u len=%u", p->cDone, uHead, cbWritten);
    }

    if (fCompleted)
    {
        u16 uFlags = *(volatile u16 *)pbAvail;
        if (!(uFlags & VRING_AVAIL_F_NO_INTERRUPT))
        {
            p->bIsr |= 1;
            VhDevRaiseIrq(pVm, p->uIrq);
            if (++p->cRaised <= 10)
                VhLog("vblk: irq raise #%u (used=%u) pic m.imr=%#x s.imr=%#x m.irr=%#x s.irr=%#x",
                      p->cRaised, *(volatile u16 *)(pbUsed + 2),
                      pVm->irqs.pic.master.imr, pVm->irqs.pic.slave.imr,
                      pVm->irqs.pic.master.irr, pVm->irqs.pic.slave.irr);
        }
        else if (p->cRaised < 10)
        {
            p->cRaised = 10;
            VhLog("vblk: interrupts suppressed by avail flags");
        }
    }
}

static u8 vblkRegRead(VBLK_STATE *p, u32 uOff)
{
    u32 uFeatures = p->fWritable ? VIRTIO_BLK_F_FLUSH : VIRTIO_BLK_F_RO;
    switch (uOff)
    {
    case 0x00: case 0x01: case 0x02: case 0x03:
        return (u8)(uFeatures >> (8 * (uOff - 0x00)));
    case 0x04: case 0x05: case 0x06: case 0x07:
        return (u8)(p->uGuestFeatures >> (8 * (uOff - 0x04)));
    case 0x08: case 0x09: case 0x0A: case 0x0B:
        return (u8)(p->uPfn >> (8 * (uOff - 0x08)));
    case 0x0C: return (u8)(VBLK_Q_SIZE & 0xFF);
    case 0x0D: return (u8)(VBLK_Q_SIZE >> 8);
    case 0x0E: case 0x0F:
        return (u8)(p->uQSel >> (8 * (uOff - 0x0E)));
    case VBLK_R_STATUS: return p->bStatus;
    case VBLK_R_ISR:    return p->bIsr;
    default:
        if (uOff >= VBLK_R_CONFIG && uOff < VBLK_R_CONFIG + 8)
            return (u8)(p->cSectors >> (8 * (uOff - VBLK_R_CONFIG)));
        return 0;
    }
}

static void vblkRegWrite(VBLK_STATE *p, u32 uOff, u8 b)
{
    switch (uOff)
    {
    case 0x04: case 0x05: case 0x06: case 0x07:
        p->uGuestFeatures = (p->uGuestFeatures & ~(0xFFu << (8 * (uOff - 0x04)))) |
                            ((u32)b << (8 * (uOff - 0x04)));
        break;
    case 0x08: case 0x09: case 0x0A: case 0x0B:
        p->uPfn = (p->uPfn & ~(0xFFu << (8 * (uOff - 0x08)))) |
                  ((u32)b << (8 * (uOff - 0x08)));
        if (uOff == 0x0B && p->cLog < 12)
            VhLog("vblk: pfn write %#x", p->uPfn);
        break;
    case 0x0E: p->uQSel = (u16)((p->uQSel & 0xFF00) | b); break;
    case 0x0F: p->uQSel = (u16)((p->uQSel & 0x00FF) | (b << 8)); break;
    case VBLK_R_STATUS:
        if (p->cLog < 12)
            VhLog("vblk: status write %#x", b);
        p->bStatus = b;
        if (b == 0)
        {
            p->uLastAvail = 0;
            p->bIsr = 0;
        }
        break;
    default:
        break;
    }
}

static bool vblkIo(VBLK_STATE *p, VH_VM *pVm, u32 uOff, bool fWrite,
                   u32 cbWidth, u32 *puValue)
{
    if (!p->fPresent)
        return false;

    if (!fWrite)
    {
        u32 uVal = 0;
        for (u32 i = 0; i < cbWidth; i++)
        {
            uVal |= (u32)vblkRegRead(p, uOff + i) << (8 * i);
            if (uOff + i == VBLK_R_ISR)
            {
                if (++p->cIsrReads <= 10)
                    VhLog("vblk: isr read #%u -> %#x", p->cIsrReads, p->bIsr);
                p->bIsr = 0;
                VhDevLowerIrq(pVm, p->uIrq);
            }
        }
        *puValue = uVal;
        return true;
    }

    for (u32 i = 0; i < cbWidth; i++)
        vblkRegWrite(p, uOff + i, (u8)((*puValue >> (8 * i)) & 0xFF));

    if (uOff == VBLK_R_NOTIFY)
        vblkKick(pVm, p);
    return true;
}

bool VhVirtioBlkIo(VH_VM *pVm, u32 uOff, bool fWrite, u32 cbWidth, u32 *puValue)
{
    return vblkIo(&s_blk, pVm, uOff, fWrite, cbWidth, puValue);
}

bool VhVirtioBlkDataIo(VH_VM *pVm, u32 uOff, bool fWrite, u32 cbWidth, u32 *puValue)
{
    return vblkIo(&s_blkData, pVm, uOff, fWrite, cbWidth, puValue);
}
