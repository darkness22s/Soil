/*
 * Soil VM engine - minimal virtio-blk (legacy PCI, PIO) disk.
 *
 * Presents the legacy virtio PCI interface (vendor 1AF4, device 1001) with
 * a single virtqueue backed by a read-only host file. This is how the guest
 * sees the squashfs archive (us /usr/local + /opt payload) without paying
 * RAM for it: only the pages the guest actually reads cost memory, and those
 * are reclaimable page cache.
 *
 * The queue is processed synchronously on the vCPU thread when the guest
 * kicks VIRTIO_PCI_QUEUE_NOTIFY, so no worker thread is needed. The ring
 * layout matches Linux's vring_init():
 *     desc  @ base
 *     avail @ base + 16*Q            {u16 flags; u16 idx; u16 ring[Q];}
 *     used  @ ALIGN(avail + 6 + 2*Q, 4096)
 */
#include "vmdefs.h"
#include <string.h>

#define VBLK_Q_SIZE   256
#define VBLK_SECTOR   512u
#define VBLK_MAX_SEGS 80

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

#define VIRTIO_BLK_F_RO   (1u << 5)

#define VBLK_T_IN         0u   /* read from disk into guest */
#define VBLK_T_OUT        1u   /* write (unsupported, read-only device) */
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
    HANDLE hMap;           /* section for the read-only image */
    const u8 *pbView;      /* mapped view base (NULL -> ReadFile fallback) */
    u64    cbDisk;
    u64    cSectors;
    bool   fPresent;

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

/* snapshot glue ---------------------------------------------------- */
int VhVirtioBlkSaveState(const u8 **ppb)
{
    *ppb = (const u8 *)&s_blk;
    return (int)sizeof(s_blk);
}

void VhVirtioBlkLoadState(const u8 *pb)
{
    VBLK_STATE src;
    memcpy(&src, pb, sizeof(src));
    s_blk.bStatus = src.bStatus;
    s_blk.uQSel = src.uQSel;
    s_blk.uPfn = src.uPfn;
    s_blk.uGuestFeatures = src.uGuestFeatures;
    s_blk.bIsr = src.bIsr;
    s_blk.uLastAvail = src.uLastAvail;
    /* diagnostics only */
    s_blk.cLog = src.cLog;
    s_blk.cKicks = src.cKicks;
    s_blk.cDone = src.cDone;
    s_blk.cRaised = src.cRaised;
    s_blk.cIsrReads = src.cIsrReads;
    s_blk.cReqs = src.cReqs;
    /* fPresent/cbDisk/cSectors are re-established by VhVirtioBlkInit() on the
       single live socket; callers that changed them are a misuse. */
    if (src.cbDisk != s_blk.cbDisk || src.cSectors != s_blk.cSectors)
    {
        VhLog("vblk: disk geometry changed since snapshot (%llu/%llu vs %llu/%llu) - hot DVDetach",
              (unsigned long long)src.cbDisk, (unsigned long long)src.cSectors,
              (unsigned long long)s_blk.cbDisk, (unsigned long long)s_blk.cSectors);
    }
}

void VhVirtioBlkInit(const char *pszPath)
{
    if (!pszPath)
        return;

    HANDLE hFile = CreateFileA(pszPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        VhLog("vblk: cannot open disk image '%s' (error %lu)", pszPath, (unsigned long)GetLastError());
        return;
    }

    LARGE_INTEGER liSize;
    if (!GetFileSizeEx(hFile, &liSize) || liSize.QuadPart < VBLK_SECTOR)
    {
        VhLog("vblk: disk image '%s' is missing or too small", pszPath);
        CloseHandle(hFile);
        return;
    }

    s_blk.hFile = hFile;
    s_blk.cbDisk = (u64)liSize.QuadPart;
    s_blk.cSectors = s_blk.cbDisk / VBLK_SECTOR;
    s_blk.fPresent = true;

    /* Map the image so guest reads are plain memcpys: no syscall, no I/O
     * manager hop and no per-request file pointer serialization on the
     * vCPU thread. Pages fault in on demand, same as ReadFile buffering. */
    HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (hMap)
    {
        void *pv = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        if (pv)
        {
            s_blk.hMap = hMap;
            s_blk.pbView = (const u8 *)pv;
        }
        else
        {
            CloseHandle(hMap);
        }
    }

    VhLog("vblk: attached '%s' (%llu bytes, %llu sectors, read-only, %s)",
          pszPath, (unsigned long long)s_blk.cbDisk, (unsigned long long)s_blk.cSectors,
          s_blk.pbView ? "mapped" : "buffered");
}

bool VhVirtioBlkPresent(void)
{
    return s_blk.fPresent;
}

static bool vblkReadDisk(u8 *pbDst, u64 uOff, u32 cb)
{
    if (s_blk.pbView)
    {
        if (uOff + cb > s_blk.cbDisk)
            return false;
        memcpy(pbDst, s_blk.pbView + uOff, cb);
        return true;
    }

    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)uOff;
    if (!SetFilePointerEx(s_blk.hFile, li, NULL, FILE_BEGIN))
        return false;

    while (cb)
    {
        DWORD cbChunk = cb > (1u << 20) ? (1u << 20) : cb;
        DWORD cbRead = 0;
        if (!ReadFile(s_blk.hFile, pbDst, cbChunk, &cbRead, NULL) || cbRead != cbChunk)
            return false;
        pbDst += cbChunk;
        cb -= cbChunk;
    }
    return true;
}

static u32 rd32(const u8 *pb) { u32 v; memcpy(&v, pb, 4); return v; }
static u64 rd64(const u8 *pb) { u64 v; memcpy(&v, pb, 8); return v; }

/* Execute one request (descriptor chain head). Always records the number of
 * bytes written into guest memory in *pcbWritten so the used ring stays sane. */
static void vblkExecReq(VH_VM *pVm, const u8 *pbRing, u16 uHead, u32 *pcbWritten)
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
        aSeg[0].gpa + 16 > cbRam)
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

    if (++s_blk.cReqs <= 6)
        VhLog("vblk: req #%u type=%u sector=%llu segs=%u", s_blk.cReqs, uType,
              (unsigned long long)uSector, cSeg);

    if (uType != VBLK_T_IN)
    {
        /* read-only device: writes, flushes and everything else are refused */
        bStatus = VBLK_S_UNSUPP;
    }
    else
    {
        u64 uOff = uSector * VBLK_SECTOR;
        for (u32 i = 1; i + 1 < cSeg; i++)
        {
            VBLK_SEG *p = &aSeg[i];
            if (!(p->flags & VRING_DESC_F_WRITE) || p->gpa + p->len > cbRam ||
                uOff + p->len > s_blk.cbDisk)
            {
                bStatus = VBLK_S_IOERR;
                break;
            }
            if (p->len && !vblkReadDisk(pbRam + p->gpa, uOff, p->len))
            {
                bStatus = VBLK_S_IOERR;
                break;
            }
            uOff += p->len;
            cbOut += p->len;
        }
    }

    *(pbRam + pStatus->gpa) = bStatus;
    *pcbWritten = cbOut + 1;
}

static void vblkKick(VH_VM *pVm)
{
    if (!s_blk.fPresent || !s_blk.uPfn || !(s_blk.bStatus & VIRTIO_CONFIG_S_DRIVER_OK))
        return;

    const u32 Q = VBLK_Q_SIZE;
    u64 gpaRing = (u64)s_blk.uPfn << 12;
    u32 offAvail = 16 * Q;
    u32 offUsed = (u32)VH_ALIGN_UP((u64)offAvail + 6 + 2 * Q, VH_PAGE);
    u64 cbRing = VH_ALIGN_UP((u64)offUsed + 6 + 8 * Q, VH_PAGE);
    if (gpaRing + cbRing > pVm->mem.cbRam)
    {
        VhLog("vblk: virtqueue page %#x is outside guest RAM", s_blk.uPfn);
        return;
    }

    u8 *pbRing = pVm->mem.pbRam + gpaRing;
    u8 *pbAvail = pbRing + offAvail;
    u8 *pbUsed = pbRing + offUsed;

    u16 uAvail = *(volatile u16 *)(pbAvail + 2);
    if (++s_blk.cKicks <= 8)
        VhLog("vblk: kick #%u pfn=%#x lastAvail=%u avail=%u flags=%#x",
              s_blk.cKicks, s_blk.uPfn, s_blk.uLastAvail, uAvail, *(volatile u16 *)pbAvail);
    if (uAvail == s_blk.uLastAvail)
        return;

    bool fCompleted = false;
    while (s_blk.uLastAvail != uAvail)
    {
        u16 uHead = *(u16 *)(pbAvail + 4 + 2 * (s_blk.uLastAvail % Q));
        u32 cbWritten = 0;
        vblkExecReq(pVm, pbRing, uHead, &cbWritten);

        u16 uUsedIdx = *(volatile u16 *)(pbUsed + 2);
        u8 *pbEnt = pbUsed + 4 + 8 * (uUsedIdx % Q);
        *(u32 *)(pbEnt) = uHead;
        *(u32 *)(pbEnt + 4) = cbWritten;
        __sync_synchronize();
        *(volatile u16 *)(pbUsed + 2) = (u16)(uUsedIdx + 1);

        s_blk.uLastAvail++;
        fCompleted = true;
        if (++s_blk.cDone <= 8)
            VhLog("vblk: done #%u head=%u len=%u", s_blk.cDone, uHead, cbWritten);
    }

    if (fCompleted)
    {
        u16 uFlags = *(volatile u16 *)pbAvail;
        if (!(uFlags & VRING_AVAIL_F_NO_INTERRUPT))
        {
            s_blk.bIsr |= 1;
            VhDevRaiseIrq(pVm, VH_VBLK_IRQ);
            if (++s_blk.cRaised <= 10)
                VhLog("vblk: irq raise #%u (used=%u) pic m.imr=%#x s.imr=%#x m.irr=%#x s.irr=%#x",
                      s_blk.cRaised, *(volatile u16 *)(pbUsed + 2),
                      pVm->irqs.pic.master.imr, pVm->irqs.pic.slave.imr,
                      pVm->irqs.pic.master.irr, pVm->irqs.pic.slave.irr);
        }
        else if (s_blk.cRaised < 10)
        {
            s_blk.cRaised = 10;
            VhLog("vblk: interrupts suppressed by avail flags");
        }
    }
}

static u8 vblkRegRead(u32 uOff)
{
    switch (uOff)
    {
    case 0x00: case 0x01: case 0x02: case 0x03:
        return (u8)(VIRTIO_BLK_F_RO >> (8 * (uOff - 0x00)));
    case 0x04: case 0x05: case 0x06: case 0x07:
        return (u8)(s_blk.uGuestFeatures >> (8 * (uOff - 0x04)));
    case 0x08: case 0x09: case 0x0A: case 0x0B:
        return (u8)(s_blk.uPfn >> (8 * (uOff - 0x08)));
    case 0x0C: return (u8)(VBLK_Q_SIZE & 0xFF);
    case 0x0D: return (u8)(VBLK_Q_SIZE >> 8);
    case 0x0E: case 0x0F:
        return (u8)(s_blk.uQSel >> (8 * (uOff - 0x0E)));
    case VBLK_R_STATUS: return s_blk.bStatus;
    case VBLK_R_ISR:    return s_blk.bIsr;
    default:
        if (uOff >= VBLK_R_CONFIG && uOff < VBLK_R_CONFIG + 8)
            return (u8)(s_blk.cSectors >> (8 * (uOff - VBLK_R_CONFIG)));
        return 0;
    }
}

static void vblkRegWrite(u32 uOff, u8 b)
{
    switch (uOff)
    {
    case 0x04: case 0x05: case 0x06: case 0x07:
        s_blk.uGuestFeatures = (s_blk.uGuestFeatures & ~(0xFFu << (8 * (uOff - 0x04)))) |
                               ((u32)b << (8 * (uOff - 0x04)));
        break;
    case 0x08: case 0x09: case 0x0A: case 0x0B:
        s_blk.uPfn = (s_blk.uPfn & ~(0xFFu << (8 * (uOff - 0x08)))) |
                     ((u32)b << (8 * (uOff - 0x08)));
        if (uOff == 0x0B && s_blk.cLog < 12)
            VhLog("vblk: pfn write %#x", s_blk.uPfn);
        break;
    case 0x0E: s_blk.uQSel = (u16)((s_blk.uQSel & 0xFF00) | b); break;
    case 0x0F: s_blk.uQSel = (u16)((s_blk.uQSel & 0x00FF) | (b << 8)); break;
    case VBLK_R_STATUS:
        if (s_blk.cLog < 12)
            VhLog("vblk: status write %#x", b);
        s_blk.bStatus = b;
        if (b == 0)
        {
            s_blk.uLastAvail = 0;
            s_blk.bIsr = 0;
        }
        break;
    default:
        break;
    }
}

bool VhVirtioBlkIo(VH_VM *pVm, u32 uOff, bool fWrite, u32 cbWidth, u32 *puValue)
{
    if (!s_blk.fPresent)
        return false;

    if (!fWrite)
    {
        u32 uVal = 0;
        for (u32 i = 0; i < cbWidth; i++)
        {
            uVal |= (u32)vblkRegRead(uOff + i) << (8 * i);
            if (uOff + i == VBLK_R_ISR)
            {
                if (++s_blk.cIsrReads <= 10)
                    VhLog("vblk: isr read #%u -> %#x", s_blk.cIsrReads, s_blk.bIsr);
                s_blk.bIsr = 0;
                VhDevLowerIrq(pVm, VH_VBLK_IRQ);
            }
        }
        *puValue = uVal;
        return true;
    }

    for (u32 i = 0; i < cbWidth; i++)
        vblkRegWrite(uOff + i, (u8)((*puValue >> (8 * i)) & 0xFF));

    if (uOff == VBLK_R_NOTIFY)
        vblkKick(pVm);
    return true;
}
