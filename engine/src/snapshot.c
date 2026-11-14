/*
 * Soil VM engine - instant-boot snapshots.
 *
 * Captures the entire VM state (guest RAM, CPU context, device models) into
 * a single file, and resumes a freshly initialized VM from it. This is what
 * makes a Soil VM "start" in well under a second: the expensive boot already
 * happened once and every later start replays the exactly frozen state.
 *
 * Everything the guest can observe must round-trip:
 *   - guest RAM (zero-page bitmap and packed pages)
 *   - the vCPU architectural state (GPRs, CRs, segments, table bases, MSRs,
 *     FPU/XMM registers)
 *   - PIC/PIT/RTC/UART/i8042 device registers
 *   - the NE2000 NIC SRAM, the virtio-blk ring counters, the PCI config
 *   - the on-the-fly mapped "absent device" MMIO pages
 *   - the 384 KiB legacy-hole scratch
 *
 * Host-side differences are handled deliberately:
 *   - slirp networking is re-initialized fresh on restore (identical setup:
 *     same MAC, same addressing, same hostfwd), so no protocol state needs
 *     to travel inside the file
 *   - the RTC reads host wallclock, so it stays live
 *   - the TSC is re-based to the host clock at restore so guest timers keep
 *     running; the PIT bookkeeping is re-anchored so no tick backlog can
 *     burst into the resumed guest
 */
#define WIN32_LEAN_AND_MEAN
#include "vmdefs.h"
#include <fcntl.h>
#include <io.h>
#include <string.h>
#include <process.h>

#define SNAP_MAGIC      0x50414E534C493053ULL   /* "S0ILSNAP" little-endian */
#define SNAP_VERSION    3
#define SNAP_HDR_SIZE   80
#define SNAP_PAGE       4096
/* v2: full-RAM payload at a fixed file offset. The layout is
     header (80) + PAD tag hdr (8) + PAD payload (4016) + RAM tag hdr (8)
   so the raw guest RAM starts at 80 + 8 + 4016 + 8 = 4112. The two 8-byte
   section headers matter: swBegin writes tag+size before every payload. */
#define SNAP_RAM_OFF    (SNAP_HDR_SIZE + 16 + 4016)

/* section tags */
#define SNAP_TAG_VCPU     1
#define SNAP_TAG_PIT      2
#define SNAP_TAG_RTC      3
#define SNAP_TAG_UART     4
#define SNAP_TAG_I8042    5
#define SNAP_TAG_PIC      6
#define SNAP_TAG_NE2K     7
#define SNAP_TAG_VBLK     8
#define SNAP_TAG_PCI      9
#define SNAP_TAG_META     10
#define SNAP_TAG_MMIO     11
#define SNAP_TAG_RAM      12
#define SNAP_TAG_SCRATCH  13
#define SNAP_TAG_RAM_FULL 14
#define SNAP_TAG_PAD      15
#define SNAP_TAG_VBLK_DATA 16

typedef struct SnapHeader
{
    u64 magic;
    u32 cbHeader;       /* struct size (compat guard) */
    u32 uVersion;
    u64 cbRam;
    u64 cbFileKernel;   /* host file sizes for staleness detection */
    u64 cbFileInitrd;
    u64 cbFileDisk;
    u64 cbSnap;         /* total file size incl. header (patched at the end) */
    u32 uCrcHeader;     /* CRC32 of the header with this field = 0 */
    u32 cPages;         /* cbRam / 4096 */
    u32 cSections;      /* number of sections (informational) */
    u32 uFlags;
    u64 nsSaved;
} SnapHeader;

_Static_assert(sizeof(SnapHeader) == 80, "SnapHeader layout is pinned at 80 bytes");

typedef struct SnapMeta
{
    u64 gpaKernel;
    u64 cbKernel;
    u64 gpaInitrd;
    u64 cbInitrd;
    u64 gpaFb;
    u64 cbFb;
    u32 cbCmdLine;
    u32 uReserved;
} SnapMeta;

typedef struct SnapRegPair
{
    WHV_REGISTER_NAME enm;
    WHV_REGISTER_VALUE val;
} SnapRegPair;

/* ------------------------------------------------------------------ */
/* small utils                                                          */
/* ------------------------------------------------------------------ */
static u32 Crc32Buf(const u8 *pb, u32 cb)
{
    u32 crc = 0xFFFFFFFFu;
    while (cb--)
    {
        crc ^= *pb++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & (u32)(-(i32)(crc & 1)));
    }
    return ~crc;
}

static u64 FileSizeOf(const char *pszPath)
{
    if (!pszPath || !*pszPath)
        return 0;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(pszPath, GetFileExInfoStandard, &fad))
        return 0;
    return ((u64)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

/* Open sequentially so Windows can optimize read-ahead and avoid retaining
   snapshot pages that the restore pass will not revisit. */
static FILE *SnapOpenSequentialRead(const char *pszPath)
{
    HANDLE hFile = CreateFileA(pszPath, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return NULL;

    int fd = _open_osfhandle((intptr_t)hFile, _O_RDONLY | _O_BINARY);
    if (fd < 0)
    {
        CloseHandle(hFile);
        return NULL;
    }

    FILE *pFile = _fdopen(fd, "rb");
    if (!pFile)
        _close(fd);
    return pFile;
}

/* ------------------------------------------------------------------ */
/* parallel page movers                                                 */
/*                                                                      */
/* The RAM section stores occupied pages as consecutive 4 KiB packets   */
/* in GPA order behind the zero bitmap, so file offset and RAM          */
/* destination are pure arithmetic from the bitmap. That makes both     */
/* request halves (intelligent classify on save, straight ingestion on  */
/* restore) monotonic per page and independent per worker.              */
/* ------------------------------------------------------------------ */

static int SnapThreadCount(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    u32 c = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
    if (c > 8)
        c = 8;
    return (int)c;
}

/* ---- restore side: contiguous chunk jobs ---- */

typedef struct SnapRun
{
    u32 iStart;     /* first occupied page */
    u32 iPages;     /* pages in the run */
} SnapRun;

typedef struct SnapRamJob
{
    HANDLE hFile;
    u64 offFile;    /* file offset of the first packed byte */
    u8 *pbDst;      /* destination inside pbRam */
    u64 cb;         /* bytes in this chunk */
    u32 rc;
} SnapRamJob;

static unsigned __stdcall SnapRamProc(void *pv)
{
    SnapRamJob *pJob = (SnapRamJob *)pv;
    pJob->rc = 0;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)pJob->offFile;
    if (!SetFilePointerEx(pJob->hFile, li, NULL, FILE_BEGIN))
    {
        pJob->rc = 1;
        return 0;
    }
    u64 cbLeft = pJob->cb;
    u8 *pb = pJob->pbDst;
    while (cbLeft)
    {
        u32 cbNow = cbLeft > (64u << 20) ? (64u << 20) : (u32)cbLeft;
        DWORD cbGot = 0;
        if (!ReadFile(pJob->hFile, pb, cbNow, &cbGot, NULL) || cbGot != cbNow)
        {
            pJob->rc = 1;
            return 0;
        }
        pb += cbNow;
        cbLeft -= cbNow;
    }
    return 0;
}

static HANDLE SnapOpenRawRead(const char *pszPath)
{
    return CreateFileA(pszPath, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
}

static int SnapLoadRamSequential(FILE *pFile, const u8 *pbMask, u32 cPages,
                                 u8 *pbRam)
{
    /* Read contiguous occupied-page runs to reduce restore reads. */
    for (u32 i = 0; i < cPages; )
    {
        if (!(pbMask[i >> 3] & (u8)(1u << (i & 7))))
        {
            i++;
            continue;
        }

        u32 iStart = i;
        do
        {
            i++;
        } while (i < cPages &&
                 (pbMask[i >> 3] & (u8)(1u << (i & 7))));

        size_t cbRun = (size_t)(i - iStart) * SNAP_PAGE;
        if (fread(pbRam + (u64)iStart * SNAP_PAGE, 1, cbRun, pFile) != cbRun)
            return 1;
    }
    return 0;
}

/* Load the packed occupied pages into pbRam with worker threads, each
   reading its own contiguous span of the snapshot file. Returns 0 when
   the whole payload has been loaded, 1 when the caller should fall back
   to the sequential path (partial or no data), 2 on unexpected abort.
   pFile is never touched: the payload starts exactly at the current
   position, given by offMaskEnd. */
static int SnapLoadRamParallel(const char *pszPath, const u8 *pbMask,
                               u32 cPages, u8 *pbRam, u64 cbPacked,
                               u64 offMaskEnd)
{
    /* pass 1: count the occupied runs and verify the packed size */
    u32 cRuns = 0;
    u64 cbSum = 0;
    for (u32 i = 0; i < cPages; )
    {
        if (!(pbMask[i >> 3] & (u8)(1u << (i & 7))))
        {
            i++;
            continue;
        }
        u32 iStart = i;
        do
        {
            i++;
        } while (i < cPages && (pbMask[i >> 3] & (u8)(1u << (i & 7))));
        cRuns++;
        cbSum += (u64)(i - iStart) * SNAP_PAGE;
    }
    if (cbSum != cbPacked)
        return 1;
    if (cRuns == 0)
        return cbPacked == 0 ? 0 : 1;

    int cThr = SnapThreadCount();
    if ((u32)cThr > cRuns)
        cThr = (int)cRuns;

    SnapRun *aRuns = (SnapRun *)malloc(sizeof(SnapRun) * cRuns);
    SnapRamJob *aJobs = (SnapRamJob *)calloc((size_t)cThr, sizeof(SnapRamJob));
    HANDLE *aWait = (HANDLE *)calloc((size_t)cThr, sizeof(HANDLE));
    int rc = 1;
    if (aRuns && aJobs && aWait)
    {
        /* pass 2: fill the run array */
        u32 r = 0;
        for (u32 i = 0; i < cPages; )
        {
            if (!(pbMask[i >> 3] & (u8)(1u << (i & 7))))
            {
                i++;
                continue;
            }
            u32 iStart = i;
            do
            {
                i++;
            } while (i < cPages && (pbMask[i >> 3] & (u8)(1u << (i & 7))));
            aRuns[r].iStart = iStart;
            aRuns[r].iPages = i - iStart;
            r++;
        }

        /* share the runs out by bytes */
        u64 quota = (cbPacked + (u64)cThr - 1) / (u64)cThr;
        int cJob = 1;
        SnapRamJob *pJob = &aJobs[0];
        u64 cbAcc = 0;
        memset(pJob, 0, sizeof(*pJob));
        for (u32 n = 0; n < cRuns; n++)
        {
            u64 cbRun = (u64)aRuns[n].iPages * SNAP_PAGE;
            if (cbAcc >= quota && cJob < cThr)
            {
                pJob++;
                cJob++;
                memset(pJob, 0, sizeof(*pJob));
                cbAcc = 0;
            }
            if (pJob->cb == 0)
            {
                pJob->offFile = offMaskEnd + (u64)aRuns[n].iStart * SNAP_PAGE;
                pJob->pbDst = pbRam + (u64)aRuns[n].iStart * SNAP_PAGE;
            }
            pJob->cb += cbRun;
            cbAcc += cbRun;
        }

        for (int g = 0; g < cJob; g++)
            aJobs[g].hFile = SnapOpenRawRead(pszPath);
        int fOpened = 1;
        for (int g = 0; g < cJob; g++)
            if (aJobs[g].hFile == INVALID_HANDLE_VALUE)
                fOpened = 0;
        if (fOpened)
        {
            u64 ns0Load = VhGuestNowNs();
            int cWait = 0;
            for (int g = 1; g < cJob; g++)
            {
                HANDLE h = (HANDLE)_beginthreadex(NULL, 0, SnapRamProc,
                                                  &aJobs[g], 0, NULL);
                if (h)
                    aWait[cWait++] = h;
                else
                    SnapRamProc(&aJobs[g]);   /* spawn failed: run here */
            }
            SnapRamProc(&aJobs[0]);
            u64 ns0Join = VhGuestNowNs();
            if (cWait)
                WaitForMultipleObjects((DWORD)cWait, aWait, TRUE, INFINITE);
            u64 ns1Join = VhGuestNowNs();
            VhLog("snap: ram load: %d jobs, spawn+job0=%llu ms, join=%llu ms",
                  cJob, (ns0Join - ns0Load) / 1000000ULL,
                  (ns1Join - ns0Join) / 1000000ULL);
            for (int n = 0; n < cWait; n++)
                CloseHandle(aWait[n]);

            u32 fFailed = 0;
            for (int g = 0; g < cJob; g++)
            {
                fFailed |= aJobs[g].rc;
                CloseHandle(aJobs[g].hFile);
            }
            rc = fFailed ? 1 : 0;
        }
        else
        {
            for (int g = 0; g < cJob; g++)
                if (aJobs[g].hFile != INVALID_HANDLE_VALUE)
                    CloseHandle(aJobs[g].hFile);
            rc = 1;
        }
    }
    free(aRuns);
    free(aJobs);
    free(aWait);
    return rc;
}

/* ------------------------------------------------------------------ */
/* vCPU register table                                                  */
/* ------------------------------------------------------------------ */
static const WHV_REGISTER_NAME s_aRegIds[] =
{
    /* general purpose + program counter */
    WHvX64RegisterRax, WHvX64RegisterRcx, WHvX64RegisterRdx, WHvX64RegisterRbx,
    WHvX64RegisterRsp, WHvX64RegisterRbp, WHvX64RegisterRsi, WHvX64RegisterRdi,
    WHvX64RegisterR8, WHvX64RegisterR9, WHvX64RegisterR10, WHvX64RegisterR11,
    WHvX64RegisterR12, WHvX64RegisterR13, WHvX64RegisterR14, WHvX64RegisterR15,
    WHvX64RegisterRip, WHvX64RegisterRflags,
    /* segmentation: one name carries the full descriptor context */
    WHvX64RegisterEs, WHvX64RegisterCs, WHvX64RegisterSs,
    WHvX64RegisterDs, WHvX64RegisterFs, WHvX64RegisterGs,
    WHvX64RegisterLdtr, WHvX64RegisterTr,
    WHvX64RegisterIdtr, WHvX64RegisterGdtr,
    /* control + debug */
    WHvX64RegisterCr0, WHvX64RegisterCr2, WHvX64RegisterCr3, WHvX64RegisterCr4,
    WHvX64RegisterCr8, WHvX64RegisterEfer,
    WHvX64RegisterDr0, WHvX64RegisterDr1, WHvX64RegisterDr2, WHvX64RegisterDr3,
    WHvX64RegisterDr6, WHvX64RegisterDr7,
    /* fast system MSRs */
    WHvX64RegisterSysenterCs, WHvX64RegisterSysenterEsp, WHvX64RegisterSysenterEip,
    WHvX64RegisterStar, WHvX64RegisterCstar, WHvX64RegisterLstar,
    WHvX64RegisterKernelGsBase, WHvX64RegisterPat,
    /* FPU/XMM (AVX/XSAVE are masked out of the guest's CPUID) */
    WHvX64RegisterFpMmx0, WHvX64RegisterFpMmx1, WHvX64RegisterFpMmx2, WHvX64RegisterFpMmx3,
    WHvX64RegisterFpMmx4, WHvX64RegisterFpMmx5, WHvX64RegisterFpMmx6, WHvX64RegisterFpMmx7,
    WHvX64RegisterFpControlStatus, WHvX64RegisterXmmControlStatus,
    WHvX64RegisterXmm0,  WHvX64RegisterXmm1,  WHvX64RegisterXmm2,  WHvX64RegisterXmm3,
    WHvX64RegisterXmm4,  WHvX64RegisterXmm5,  WHvX64RegisterXmm6,  WHvX64RegisterXmm7,
    WHvX64RegisterXmm8,  WHvX64RegisterXmm9,  WHvX64RegisterXmm10, WHvX64RegisterXmm11,
    WHvX64RegisterXmm12, WHvX64RegisterXmm13, WHvX64RegisterXmm14, WHvX64RegisterXmm15,
};
#define SNAP_REG_MAX VH_NELEMS(s_aRegIds)

/* ------------------------------------------------------------------ */
/* writer                                                               */
/* ------------------------------------------------------------------ */
typedef struct SnapWriter
{
    FILE *pFile;
    u8 *pbBuf;
    u32 cbBuf;
    u32 cbUsed;
    u64 cbWritten;
} SnapWriter;

static void swOpen(SnapWriter *pW, FILE *pFile)
{
    memset(pW, 0, sizeof(*pW));
    pW->pFile = pFile;
    pW->cbBuf = 8u << 20;
    pW->pbBuf = (u8 *)malloc(pW->cbBuf);
}

static void swClose(SnapWriter *pW)
{
    free(pW->pbBuf);
    memset(pW, 0, sizeof(*pW));
}

static int swFlush(SnapWriter *pW)
{
    if (!pW->cbUsed)
        return 0;
    u32 cb = pW->cbUsed;
    pW->cbUsed = 0;
    if (fwrite(pW->pbBuf, 1, cb, pW->pFile) != cb)
        return 1;
    return 0;
}

static int swWrite(SnapWriter *pW, const void *pv, u64 cb)
{
    const u8 *pb = (const u8 *)pv;
    while (cb)
    {
        u32 cbCan = pW->cbBuf - pW->cbUsed;
        if (cbCan > cb)
            cbCan = (u32)cb;
        memcpy(pW->pbBuf + pW->cbUsed, pb, cbCan);
        pW->cbUsed += cbCan;
        pb += cbCan;
        pW->cbWritten += cbCan;
        cb -= cbCan;
        if (pW->cbUsed == pW->cbBuf && swFlush(pW))
            return 1;
    }
    return 0;
}

/* begin a section; nTag receives the file position of the size record */
static int swBegin(SnapWriter *pW, u32 uTag, u64 *pnTag)
{
    u32 ab[2] = { uTag, 0 };
    *pnTag = pW->cbWritten;
    return swWrite(pW, ab, 8);
}

/* patch the section's size record (header is tag bytes + u32 size; payload
   follows; the position after a flush = header + payload) */
static int swEnd(SnapWriter *pW, u64 nTag)
{
    if (swFlush(pW))
        return 1;
    u64 nPos = pW->cbWritten;
    u32 cbSection = (u32)(nPos - nTag - 8);
    if (_fseeki64(pW->pFile, (long long)nTag + 4, SEEK_SET))
        return 1;
    if (fwrite(&cbSection, sizeof(cbSection), 1, pW->pFile) != 1)
        return 1;
    if (fflush(pW->pFile))
        return 1;
    if (_fseeki64(pW->pFile, (long long)nPos, SEEK_SET))
        return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* SAVE                                                                 */
/* ------------------------------------------------------------------ */
int VhSnapSave(VH_VM *pVm, const char *pszPath, const VhSnapImgRefs *pRefs)
{
    SnapHeader hdr;
    u32 cSections = 0;
    u64 ns0 = VhGuestNowNs();
    int rc = 1;
    SnapWriter w;
    char szTmp[1088];
    SnapRegPair aRegs[SNAP_REG_MAX];
    u32 cRegs = 0;
    FILE *pFile = NULL;
    int fTempOpened = 0;

    memset(&w, 0, sizeof(w));
    snprintf(szTmp, sizeof(szTmp), "%s.tmp", pszPath);
    VhLog("snap: save '%s' (ram=%llu MiB)", pszPath,
          (unsigned long long)(pVm->mem.cbRam >> 20));

    /* device state must be frozen for the whole capture; everything that
       mutates device/FIFO state goes through VhDevLock */
    VhDevLock();

    /* ---- read the vCPU state first: a failing access aborts early ---- */
    /* read register-by-register: unknown name -> skip that one, everything
       else is captured individually and no batch failure can hide state */
    for (u32 r = 0; r < SNAP_REG_MAX; r++)
    {
        WHV_REGISTER_VALUE val;
        memset(&val, 0, sizeof(val));
        if (VhWhvGetReg(pVm, s_aRegIds[r], &val) == 0)
        {
            aRegs[cRegs].enm = s_aRegIds[r];
            aRegs[cRegs].val = val;
            cRegs++;
        }
    }
    if (cRegs == 0)
    {
        VhLog("snap: cannot read vCPU registers - not saving");
        goto save_out;
    }

    pFile = fopen(szTmp, "wb");
    fTempOpened = pFile != NULL;
    if (!pFile)
    {
        VhLog("snap: cannot create '%s'", szTmp);
        goto save_out;
    }
    swOpen(&w, pFile);
    if (!w.pbBuf)
    {
        VhLog("snap: no snapshot buffer memory");
        goto save_out;
    }

    /* ---- header (patched at the end with total size + CRC) ---- */
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = SNAP_MAGIC;
    hdr.cbHeader = SNAP_HDR_SIZE;
    hdr.uVersion = SNAP_VERSION;
    hdr.cbRam = pVm->mem.cbRam;
    hdr.cPages = (u32)(pVm->mem.cbRam / SNAP_PAGE);
    hdr.nsSaved = ns0;
    hdr.cbFileKernel = FileSizeOf(pRefs ? pRefs->pszKernel : NULL);
    hdr.cbFileInitrd = FileSizeOf(pRefs ? pRefs->pszInitrd : NULL);
    hdr.cbFileDisk = FileSizeOf(pRefs ? pRefs->pszDisk : NULL);
    hdr.uCrcHeader = Crc32Buf((u8 *)&hdr, SNAP_HDR_SIZE);
    if (swWrite(&w, &hdr, sizeof(hdr)))
    {
        VhLog("snap: header write failed");
        goto save_out;
    }

    /* ---- RAM: the whole memory, first in the file at a fixed offset so a
            restore can page it in lazily straight from this file. The PAD
            section pads the gap up to SNAP_RAM_OFF; both 8-byte section
            headers are accounted for in that constant. ---- */
    {
        u64 nTag = 0;
        static const u8 abPad[SNAP_RAM_OFF - SNAP_HDR_SIZE - 16] = { 0 };
        if (swBegin(&w, SNAP_TAG_PAD, &nTag) ||
            swWrite(&w, abPad, sizeof(abPad)) || swEnd(&w, nTag))
        {
            VhLog("snap: pad section failed");
            goto save_out;
        }
        cSections++;
        int fRamOk = 1;
        if (swBegin(&w, SNAP_TAG_RAM_FULL, &nTag))
            fRamOk = 0;
        if (fRamOk && pVm->fSnapRamLazy && pVm->pFileLazy)
        {
            /* chunks that were never paged in still hold the previous
               snapshot's bytes: copy them straight from the old file
               instead of materializing them in guest RAM */
            u8 *pbBounce = (u8 *)malloc(0x100000);
            if (!pbBounce)
                fRamOk = 0;
            u64 cb = pVm->mem.cbRam;
            for (u64 off = 0; fRamOk && off < cb; )
            {
                u64 cbChunk = cb - off;
                if (cbChunk > VH_LAZY_CHUNK)
                    cbChunk = VH_LAZY_CHUNK;
                u32 iChunk = (u32)(off / VH_LAZY_CHUNK);
                if (pVm->pbRamLoaded[iChunk >> 3] & (1u << (iChunk & 7)))
                {
                    if (swWrite(&w, pVm->mem.pbRam + off, cbChunk))
                        fRamOk = 0;
                    off += cbChunk;
                }
                else
                {
                    u64 offRun = off;
                    while (off < cb)
                    {
                        u64 cbC = cb - off;
                        if (cbC > VH_LAZY_CHUNK)
                            cbC = VH_LAZY_CHUNK;
                        u32 iC = (u32)(off / VH_LAZY_CHUNK);
                        if (pVm->pbRamLoaded[iC >> 3] & (1u << (iC & 7)))
                            break;
                        if (off - offRun + cbC > 0x100000)
                            break;
                        off += cbC;
                    }
                    u64 cbRun = off - offRun;
                    if (_fseeki64(pVm->pFileLazy, (LONGLONG)(SNAP_RAM_OFF + offRun), SEEK_SET) ||
                        fread(pbBounce, 1, (size_t)cbRun, pVm->pFileLazy) != (size_t)cbRun ||
                        swWrite(&w, pbBounce, cbRun))
                        fRamOk = 0;
                }
            }
            free(pbBounce);
        }
        else if (fRamOk)
        {
            if (swWrite(&w, pVm->mem.pbRam, pVm->mem.cbRam))
                fRamOk = 0;
        }
        if (!fRamOk || swEnd(&w, nTag))
        {
            VhLog("snap: full ram section failed");
            goto save_out;
        }
        cSections++;
    }

#define SNAP_SAVE_TAG(tag, cbType, srcExpr)                                \
    do {                                                                   \
        u64 nTag_;                                                         \
        if (swBegin(&w, SNAP_TAG_##tag, &nTag_) ||                         \
            swWrite(&w, &(srcExpr), sizeof(cbType)) ||                     \
            swEnd(&w, nTag_))                                              \
        {                                                                  \
            VhLog("snap: " #tag " section failed");                        \
            goto save_out;                                                 \
        }                                                                  \
        cSections++;                                                       \
    } while (0)

    /* ---- vCPU ---- */
    {
        u64 nTag = 0;
        if (swBegin(&w, SNAP_TAG_VCPU, &nTag) ||
            swWrite(&w, &cRegs, 4) ||
            swWrite(&w, aRegs, (u64)cRegs * sizeof(SnapRegPair)) ||
            swEnd(&w, nTag))
        {
            VhLog("snap: vcpu section failed");
            goto save_out;
        }
        cSections++;
    }

    /* ---- legacy devices ---- */
    SNAP_SAVE_TAG(PIT,   VH_PIT,   pVm->pit);
    SNAP_SAVE_TAG(RTC,   VH_RTC,   pVm->rtc);
    SNAP_SAVE_TAG(UART,  VH_UART,  pVm->uart);
    SNAP_SAVE_TAG(I8042, VH_I8042, pVm->i8042);
    SNAP_SAVE_TAG(PIC,   VH_PIC,   pVm->irqs.pic);

    /* ---- NE2000 NIC ---- */
    {
        const u8 *pb = NULL;
        int cb = Ne2000SaveState(&pb);
        u64 nTag = 0;
        if (cb <= 0 || swBegin(&w, SNAP_TAG_NE2K, &nTag) ||
            swWrite(&w, pb, (u64)cb) || swEnd(&w, nTag))
        {
            VhLog("snap: ne2k section failed");
            goto save_out;
        }
        cSections++;
    }

    /* ---- virtio-blk whole model ---- */
    {
        const u8 *pb = NULL;
        int cb = VhVirtioBlkSaveState(&pb);
        u64 nTag = 0;
        if (cb <= 0 || swBegin(&w, SNAP_TAG_VBLK, &nTag) ||
            swWrite(&w, pb, (u64)cb) || swEnd(&w, nTag))
        {
            VhLog("snap: vblk section failed");
            goto save_out;
        }
        cSections++;
    }

    /* ---- data virtio-blk state (if attached) ---- */
    if (VhVirtioBlkDataPresent())
    {
        const u8 *pb = NULL;
        int cb = VhVirtioBlkDataSaveState(&pb);
        u64 nTag = 0;
        if (cb <= 0 || swBegin(&w, SNAP_TAG_VBLK_DATA, &nTag) ||
            swWrite(&w, pb, (u64)cb) || swEnd(&w, nTag))
        {
            VhLog("snap: data vblk section failed");
            goto save_out;
        }
        cSections++;
    }

    /* ---- PCI config statics ---- */
    {
        const u32 *pb = NULL;
        u32 cW = VhPciSaveState(&pb);
        u64 nTag = 0;
        if (!cW || swBegin(&w, SNAP_TAG_PCI, &nTag) ||
            swWrite(&w, pb, (u64)cW * 4) || swEnd(&w, nTag))
        {
            VhLog("snap: pci section failed");
            goto save_out;
        }
        cSections++;
    }

    /* ---- memory layout + cmdline ---- */
    {
        SnapMeta meta;
        u64 nTag = 0;
        memset(&meta, 0, sizeof(meta));
        meta.gpaKernel = pVm->mem.gpaKernel;
        meta.cbKernel = pVm->mem.cbKernel;
        meta.gpaInitrd = pVm->mem.gpaInitrd;
        meta.cbInitrd = pVm->mem.cbInitrd;
        meta.gpaFb = pVm->mem.gpaFb;
        meta.cbFb = pVm->mem.cbFb;
        meta.cbCmdLine = (u32)strlen(pVm->szCmdLine);
        if (swBegin(&w, SNAP_TAG_META, &nTag) ||
            swWrite(&w, &meta, sizeof(meta)) ||
            swWrite(&w, pVm->szCmdLine, (u64)meta.cbCmdLine + 1) ||
            swEnd(&w, nTag))
        {
            VhLog("snap: meta section failed");
            goto save_out;
        }
        cSections++;
    }

    /* ---- vmhost-owned MMIO pages (the hook streams via the FILE*) ---- */
    {
        u64 nTag = 0;
        int rcExtra;
        if (swBegin(&w, SNAP_TAG_MMIO, &nTag) || swFlush(&w))
        {
            VhLog("snap: mmio section failed (begin)");
            goto save_out;
        }
        w.cbWritten = (u64)_ftelli64(w.pFile);   /* resync after flush */
        rcExtra = VhHostSnapSaveExtra(pVm, w.pFile);
        w.cbWritten = (u64)_ftelli64(w.pFile);   /* resync after the hook */
        if (rcExtra == 0)
            rcExtra = swEnd(&w, nTag);
        if (rcExtra)
        {
            VhLog("snap: mmio section failed");
            goto save_out;
        }
        cSections++;
    }

    /* ---- scratch hole ---- */
    {
        u64 nTag = 0;
        if (swBegin(&w, SNAP_TAG_SCRATCH, &nTag) ||
            swWrite(&w, pVm->mem.pbScratch, pVm->mem.cbScratch) || swEnd(&w, nTag))
        {
            VhLog("snap: scratch section failed");
            goto save_out;
        }
        cSections++;
    }

    /* ---- RAM is written first, right after the header (see above) ---- */

#undef SNAP_SAVE_TAG

    if (swFlush(&w))
    {
        VhLog("snap: final flush failed");
        goto save_out;
    }

    /* ---- patch the header (total size + tightened CRC) ---- */
    VhLog("snap: closed file at pos %llu", (unsigned long long)w.cbWritten);
    {
        SnapHeader out;
        memset(&out, 0, sizeof(out));
        if (_fseeki64(w.pFile, 0, SEEK_END))
        {
            VhLog("snap: end seek failed");
            goto save_out;
        }
        long long cbFile = _ftelli64(w.pFile);
        memcpy(&out, &hdr, sizeof(out));
        out.cbSnap = (u64)cbFile;
        out.cSections = cSections;
        out.uCrcHeader = 0;
        out.uCrcHeader = Crc32Buf((u8 *)&out, SNAP_HDR_SIZE);
        if (_fseeki64(w.pFile, 0, SEEK_SET))
        {
            VhLog("snap: header seek failed");
            goto save_out;
        }
        if (fwrite(&out, sizeof(out), 1, w.pFile) != 1 || fflush(w.pFile))
        {
            VhLog("snap: header patch failed");
            goto save_out;
        }
    }
    fclose(pFile);
    pFile = NULL;
    fTempOpened = 0;
    swClose(&w);

    /* a lazy session holds the snapshot file open for chunk page-ins; the
       handle must be dropped for the replace-rename to succeed, then the
       same path is reopened (the freshly written file has identical bytes
       for every chunk that was never paged in, so lazy reads stay valid) */
    BOOL fMoved;
    if (pVm->fSnapRamLazy && pVm->pFileLazy)
    {
        fclose(pVm->pFileLazy);
        pVm->pFileLazy = NULL;
        fMoved = MoveFileExA(szTmp, pszPath, MOVEFILE_REPLACE_EXISTING);
        pVm->pFileLazy = SnapOpenSequentialRead(pszPath);
        if (!pVm->pFileLazy)
        {
            VhLog("snap: cannot reopen '%s' for lazy reads - stopping", pszPath);
            rc = 1;
            goto save_out2;
        }
    }
    else
    {
        fMoved = MoveFileExA(szTmp, pszPath, MOVEFILE_REPLACE_EXISTING);
    }
    if (!fMoved)
    {
        VhLog("snap: rename '%s' -> '%s' failed (%lu)", szTmp, pszPath,
              (unsigned long)GetLastError());
        rc = 1;
        goto save_out2;
    }
    rc = 0;
    VhLog("snap: saved %llu sections / %llu bytes in %llu ms",
          (unsigned long long)cSections,
          (unsigned long long)FileSizeOf(pszPath),
          (unsigned long long)(VhGuestNowNs() - ns0) / 1000000ULL);
    goto save_out2;

save_out:
    rc = 1;
    if (fTempOpened && pFile)
    {
        fclose(pFile);
        fTempOpened = 0;
    }
    swClose(&w);

save_out2:
    VhDevUnlock();
    DeleteFileA(szTmp);       /* nothing left behind on failure */
    return rc;
}

/* ------------------------------------------------------------------ */
/* RESTORE                                                              */
/* ------------------------------------------------------------------ */
int VhSnapRestore(VH_VM *pVm, const char *pszPath, const VhSnapImgRefs *pRefs)
{
    SnapHeader hdr;
    int rc = 1;
    u64 ns0 = VhGuestNowNs();
    SnapRegPair *aRegs = NULL;
    u32 cRegs = 0;
    u32 fHasVcpu = 0, fHasRam = 0, fHasMeta = 0, fHasPit = 0, fHasPic = 0;
    u32 fHasNe2k = 0, fHasVblk = 0, fHasPci = 0;
    u64 nsRamMs = 0, nsFixupMs = 0, nsRegMs = 0, nsAdoptMs = 0;
    u64 nsOpenMs = 0, nsHeaderMs = 0, nsStalenessMs = 0, nsRestoreLogMs = 0;
    u64 nsPreflightMs = 0, nsSectionsMs = 0;

    u64 nsOpenStart = VhGuestNowNs();
    FILE *pFile = SnapOpenSequentialRead(pszPath);
    if (!pFile)
    {
        VhLog("snap: cannot open snapshot '%s'", pszPath);
        return 1;
    }
    nsOpenMs = (VhGuestNowNs() - nsOpenStart) / 1000000ULL;
    u64 nsHeaderStart = VhGuestNowNs();

    if (fread(&hdr, 1, sizeof(hdr), pFile) != sizeof(hdr) ||
        hdr.magic != SNAP_MAGIC ||
        (hdr.uVersion != 1 && hdr.uVersion != SNAP_VERSION) ||
        hdr.cbHeader != SNAP_HDR_SIZE)
    {
        VhLog("snap: '%s' is not a valid snapshot", pszPath);
        fclose(pFile);
        return 1;
    }
    {
        u32 uCrcStored = hdr.uCrcHeader;   /* save CRC'd the header with this field = 0 */
        hdr.uCrcHeader = 0;
        u32 uCrc = Crc32Buf((u8 *)&hdr, SNAP_HDR_SIZE);
        if (uCrc != uCrcStored)
        {
            VhLog("snap: header CRC mismatch (%#x != %#x)", uCrc, uCrcStored);
            fclose(pFile);
            return 1;
        }
    }

    if (hdr.cbRam != pVm->mem.cbRam)
    {
        VhLog("snap: RAM size mismatch (snapshot=%llu MiB, now=%llu MiB)",
              (unsigned long long)(hdr.cbRam >> 20),
              (unsigned long long)(pVm->mem.cbRam >> 20));
        fclose(pFile);
        return 1;
    }

    nsHeaderMs = (VhGuestNowNs() - nsHeaderStart) / 1000000ULL;

    /* staleness detection: the backing images must be untouched */
    u64 nsStalenessStart = VhGuestNowNs();
    {
        u64 cbKernel = FileSizeOf(pRefs ? pRefs->pszKernel : NULL);
        u64 cbInitrd = FileSizeOf(pRefs ? pRefs->pszInitrd : NULL);
        u64 cbDisk = FileSizeOf(pRefs ? pRefs->pszDisk : NULL);
        if (!pRefs || !hdr.cbFileKernel || !hdr.cbFileInitrd || !hdr.cbFileDisk ||
            !cbKernel || !cbInitrd || !cbDisk ||
            cbKernel != hdr.cbFileKernel ||
            cbInitrd != hdr.cbFileInitrd ||
            cbDisk != hdr.cbFileDisk)
        {
            VhLog("snap: images changed since the snapshot (kernel %llu vs %llu, initrd %llu vs %llu, disk %llu vs %llu) - stale",
                  (unsigned long long)cbKernel, (unsigned long long)hdr.cbFileKernel,
                  (unsigned long long)cbInitrd, (unsigned long long)hdr.cbFileInitrd,
                  (unsigned long long)cbDisk, (unsigned long long)hdr.cbFileDisk);
            fclose(pFile);
            return 1;
        }
    }
    nsStalenessMs = (VhGuestNowNs() - nsStalenessStart) / 1000000ULL;
    u64 nsRestoreLogStart = VhGuestNowNs();
    VhLog("snap: restoring '%s' (ram=%llu MiB, %llu MiB file, saved %llu s ago)",
          pszPath, (unsigned long long)(hdr.cbRam >> 20),
          (unsigned long long)(hdr.cbSnap >> 20),
          (unsigned long long)(ns0 - hdr.nsSaved) / 1000000000ULL);
    nsRestoreLogMs = (VhGuestNowNs() - nsRestoreLogStart) / 1000000ULL;
    u64 nsSectionsStart = VhGuestNowNs();
    nsPreflightMs = (nsSectionsStart - ns0) / 1000000ULL;

    int fFatal = 0;
    u32 cProcessed = 0;
    while (!fFatal)
    {
        u32 abTag[2];
        if (fread(abTag, 1, 8, pFile) != 8)
            break;
        u32 uTag = abTag[0];
        u32 cbPayload = abTag[1];
        cProcessed++;

        if (uTag == SNAP_TAG_VCPU)
        {
            if (fread(&cRegs, 4, 1, pFile) != 1 || cRegs == 0 || cRegs > SNAP_REG_MAX ||
                (u64)cbPayload != 4 + (u64)cRegs * sizeof(SnapRegPair))
            {
                fFatal = 1;
                break;
            }
            free(aRegs);
            aRegs = (SnapRegPair *)malloc(sizeof(SnapRegPair) * cRegs);
            if (!aRegs || fread(aRegs, sizeof(SnapRegPair), cRegs, pFile) != cRegs)
            {
                fFatal = 1;
                break;
            }
            fHasVcpu = 1;
        }
        else if (uTag == SNAP_TAG_RAM)
        {
            u32 cPages = hdr.cPages;
            u32 uMaskBytes = (cPages + 7) / 8;
            u8 *pbMask = (u8 *)malloc(uMaskBytes ? uMaskBytes : 1);
            u64 cbPacked = 0;
            u64 nsRamStart = VhGuestNowNs();
            if (!pbMask)
            {
                fFatal = 1;
                break;
            }
            if (fread(pbMask, 1, uMaskBytes, pFile) != uMaskBytes)
            {
                free(pbMask);
                fFatal = 1;
                break;
            }
            for (u32 i = 0; i < cPages; i++)
                if (pbMask[i >> 3] & (u8)(1u << (i & 7)))
                    cbPacked += SNAP_PAGE;

            if (uMaskBytes > cbPayload || cbPacked != (u64)cbPayload - uMaskBytes)
            {
                free(pbMask);
                fFatal = 1;
                break;
            }

            /* The packed payload begins at the current stream position and
               consists solely of occupied 4 KiB pages in GPA order, so its
               file spans are computable: load them with worker threads. */
            int rcRam = SnapLoadRamParallel(pszPath, pbMask, cPages,
                                            pVm->mem.pbRam, cbPacked,
                                            (u64)_ftelli64(pFile));
            if (rcRam == 0 &&
                _fseeki64(pFile, (LONGLONG)cbPacked, SEEK_CUR) != 0)
                rcRam = 2;
            if (rcRam != 0)
                rcRam = SnapLoadRamSequential(pFile, pbMask, cPages,
                                              pVm->mem.pbRam);
            nsRamMs = (VhGuestNowNs() - nsRamStart) / 1000000ULL;
            free(pbMask);
            if (rcRam != 0)
                fFatal = 1;
            else
                fHasRam = 1;
        }
        else if (uTag == SNAP_TAG_PAD)
        {
            /* v2: filler that parks the full-RAM payload at a 4 KiB offset */
            if (_fseeki64(pFile, (LONGLONG)cbPayload, SEEK_CUR))
            {
                fFatal = 1;
                break;
            }
        }
        else if (uTag == SNAP_TAG_RAM_FULL)
        {
            /* v2 fast path: the payload is the guest RAM verbatim at file
               offset SNAP_RAM_OFF. Do not copy it: leave guest RAM unmapped
               and page 64 KiB chunks in from this file on first access.
               Restore cost is then independent of RAM size. */
            u64 ns0Lazy = VhGuestNowNs();
            if (cbPayload != hdr.cbRam || pVm->fSnapRamLazy)
            {
                fFatal = 1;
                break;
            }
            u32 cChunks = (u32)((hdr.cbRam + VH_LAZY_CHUNK - 1) / VH_LAZY_CHUNK);
            u32 cbBits = (cChunks + 7) / 8;
            pVm->pbRamLoaded = (u8 *)calloc(cbBits ? cbBits : 1, 1);
            if (!pVm->pbRamLoaded)
            {
                VhLog("snap: no lazy page map memory");
                fFatal = 1;
                break;
            }
            pVm->pFileLazy = pFile;   /* kept open past the restore */
            pVm->fSnapRamLazy = 1;
            if (_fseeki64(pFile, (LONGLONG)cbPayload, SEEK_CUR))
            {
                fFatal = 1;
                break;
            }
            nsAdoptMs = (VhGuestNowNs() - ns0Lazy) / 1000000ULL;
            fHasRam = 1;
        }
        else if (uTag == SNAP_TAG_META)
        {
            if (cbPayload < sizeof(SnapMeta))
            {
                fFatal = 1;
                break;
            }
            SnapMeta meta;
            if (fread(&meta, 1, sizeof(meta), pFile) != sizeof(meta))
            {
                fFatal = 1;
                break;
            }
            if (meta.cbCmdLine >= sizeof(pVm->szCmdLine) ||
                (u64)cbPayload != sizeof(SnapMeta) + (u64)meta.cbCmdLine + 1)
            {
                fFatal = 1;
                break;
            }
            if (meta.cbCmdLine + 1 &&
                fread(pVm->szCmdLine, 1, (size_t)meta.cbCmdLine + 1, pFile) != (size_t)meta.cbCmdLine + 1)
            {
                fFatal = 1;
                break;
            }
            pVm->szCmdLine[meta.cbCmdLine] = '\0';
            pVm->mem.gpaKernel = meta.gpaKernel;
            pVm->mem.cbKernel = meta.cbKernel;
            pVm->mem.gpaInitrd = meta.gpaInitrd;
            pVm->mem.cbInitrd = meta.cbInitrd;
            pVm->mem.gpaFb = meta.gpaFb;
            pVm->mem.cbFb = meta.cbFb;
            fHasMeta = 1;
        }
        else if (uTag == SNAP_TAG_MMIO)
        {
            if (VhHostSnapRestoreExtra(pVm, pFile, cbPayload))
                fFatal = 1;
        }
        else if (uTag == SNAP_TAG_SCRATCH)
        {
            if (cbPayload != pVm->mem.cbScratch ||
                fread(pVm->mem.pbScratch, 1, cbPayload, pFile) != cbPayload)
            {
                VhLog("snap: scratch size mismatch");
                fFatal = 1;
            }
        }
        else if (uTag == SNAP_TAG_PIT)
        {
            if (cbPayload != sizeof(pVm->pit) || fread(&pVm->pit, 1, cbPayload, pFile) != cbPayload)
                fFatal = 1;
            else
                fHasPit = 1;
        }
        else if (uTag == SNAP_TAG_RTC)
        {
            if (cbPayload != sizeof(pVm->rtc) || fread(&pVm->rtc, 1, cbPayload, pFile) != cbPayload)
                fFatal = 1;
        }
        else if (uTag == SNAP_TAG_UART)
        {
            if (cbPayload != sizeof(pVm->uart) || fread(&pVm->uart, 1, cbPayload, pFile) != cbPayload)
                fFatal = 1;
        }
        else if (uTag == SNAP_TAG_I8042)
        {
            if (cbPayload != sizeof(pVm->i8042) || fread(&pVm->i8042, 1, cbPayload, pFile) != cbPayload)
                fFatal = 1;
        }
        else if (uTag == SNAP_TAG_PIC)
        {
            if (cbPayload != sizeof(pVm->irqs.pic) || fread(&pVm->irqs.pic, 1, cbPayload, pFile) != cbPayload)
                fFatal = 1;
            else
                fHasPic = 1;
        }
        else if (uTag == SNAP_TAG_NE2K)
        {
            const u8 *pbSaved = NULL;
            int cbWant = Ne2000SaveState(&pbSaved);
            if (cbPayload != (u32)cbWant)
            {
                fFatal = 1;
                break;
            }
            u8 *pbTmp = (u8 *)malloc((size_t)cbPayload);
            if (!pbTmp)
            {
                fFatal = 1;
                break;
            }
            if (fread(pbTmp, 1, cbPayload, pFile) != cbPayload)
            {
                free(pbTmp);
                fFatal = 1;
                break;
            }
            Ne2000LoadState(pbTmp);
            free(pbTmp);
            fHasNe2k = 1;
        }
        else if (uTag == SNAP_TAG_VBLK)
        {
            const u8 *pbSaved = NULL;
            int cbWant = VhVirtioBlkSaveState(&pbSaved);
            if (cbPayload != (u32)cbWant)
            {
                fFatal = 1;
                break;
            }
            u8 *pbTmp = (u8 *)malloc((size_t)cbPayload);
            if (!pbTmp)
            {
                fFatal = 1;
                break;
            }
            if (fread(pbTmp, 1, cbPayload, pFile) != cbPayload)
            {
                free(pbTmp);
                fFatal = 1;
                break;
            }
            VhVirtioBlkLoadState(pbTmp);
            free(pbTmp);
            fHasVblk = 1;
        }
        else if (uTag == SNAP_TAG_VBLK_DATA)
        {
            const u8 *pbSaved = NULL;
            int cbWant = VhVirtioBlkDataSaveState(&pbSaved);
            if (cbWant <= 0 || cbPayload != (u32)cbWant)
            {
                fFatal = 1;
                break;
            }
            u8 *pbTmp = (u8 *)malloc((size_t)cbPayload);
            if (!pbTmp || fread(pbTmp, 1, cbPayload, pFile) != cbPayload)
            {
                free(pbTmp);
                fFatal = 1;
                break;
            }
            VhVirtioBlkDataLoadState(pbTmp);
            free(pbTmp);
        }
        else if (uTag == SNAP_TAG_PCI)
        {
            const u32 *pbSaved = NULL;
            u32 cWrap = VhPciSaveState(&pbSaved);
            if (cbPayload != cWrap * 4)
            {
                fFatal = 1;
                break;
            }
            u8 *pbTmp = (u8 *)malloc((size_t)cbPayload);
            if (!pbTmp)
            {
                fFatal = 1;
                break;
            }
            if (fread(pbTmp, 1, cbPayload, pFile) != cbPayload)
            {
                free(pbTmp);
                fFatal = 1;
                break;
            }
            VhPciLoadState((const u32 *)pbTmp, cWrap);
            free(pbTmp);
            fHasPci = 1;
        }
        else
        {
            /* unknown section: skip */
            VhLog("snap: skipping unknown section tag=%u cb=%u", uTag, cbPayload);
            if (_fseeki64(pFile, (long long)cbPayload, SEEK_CUR))
                fFatal = 1;
        }

        /* cursor-run sanity: the declared payload must leave the file before
           its declared total size */
        if (!fFatal && _ftelli64(pFile) > (i64)hdr.cbSnap)
        {
            VhLog("snap: file cursor ran past the declared size (@section %u)", cProcessed);
            fFatal = 1;
        }
    }
    nsSectionsMs = (VhGuestNowNs() - nsSectionsStart) / 1000000ULL;

    if (!pVm->fSnapRamLazy)
        fclose(pFile);

    if (fFatal)
    {
        VhLog("snap: '%s' failed at section %u", pszPath, cProcessed);
        rc = 1;
        goto restore_done;
    }
    if (!fHasVcpu || !fHasRam || !fHasMeta || !fHasPit || !fHasPic || !fHasNe2k || !fHasVblk || !fHasPci)
    {
        VhLog("snap: '%s' missing required sections (vcpu=%u ram=%u meta=%u pit=%u pic=%u ne2k=%u vblk=%u pci=%u)",
              pszPath, fHasVcpu, fHasRam, fHasMeta, fHasPit, fHasPic, fHasNe2k, fHasVblk, fHasPci);
        rc = 1;
        goto restore_done;
    }

    /* device fix-ups: initialized afresh by VhDevInit/Ne2000Init at startup */
    u64 nsFixupStart = VhGuestNowNs();
    VhDevPostRestoreFixup(pVm);
    Ne2000BindVm(pVm);
    nsFixupMs = (VhGuestNowNs() - nsFixupStart) / 1000000ULL;

    /* re-anchor the guest clock: the TSC continues from the host clock and
       the PIT catch-up bookkeeping restarts from "now" so the resumed guest
       gets a normal tick stream instead of a backlog burst */
    {
        u64 nsNow = VhGuestNowNs();
        pVm->pit.ch[0].startNs = nsNow;
        pVm->pit.ch[1].startNs = nsNow;
        pVm->pit.ch[2].startNs = nsNow;
        pVm->pit.startNsSeen = nsNow;
        pVm->pit.cPeriods = 0;
        pVm->pit.lastTickNs = nsNow;
    }

    /* vCPU registers */
    {
        u64 nsRegStart = VhGuestNowNs();
        WHV_REGISTER_NAME aNames[SNAP_REG_MAX];
        WHV_REGISTER_VALUE aVals[SNAP_REG_MAX];
        for (u32 i = 0; i < cRegs; i++)
        {
            aNames[i] = aRegs[i].enm;
            aVals[i] = aRegs[i].val;
        }
        if (VhWhvSetRegs(pVm, aNames, aVals, cRegs))
        {
            /* batch rejected: retry individually so a single unsupported
               entry cannot drop the whole vCPU state */
            u32 cSet = 0;
            for (u32 i = 0; i < cRegs; i++)
                if (VhWhvSetReg(pVm, aNames[i], aVals[i]) == 0)
                    cSet++;
            if (cSet < cRegs)
            {
                VhLog("snap: vCPU register restore: only %u/%u accepted", cSet, cRegs);
                if (cSet == 0)
                {
                    rc = 1;
                    goto restore_done;
                }
            }
        }
        nsRegMs = (VhGuestNowNs() - nsRegStart) / 1000000ULL;
        /* time: continue the guest clock at the host rate from now on */
        WHV_REGISTER_VALUE tsc;
        memset(&tsc, 0, sizeof(tsc));
        tsc.Reg64 = VhGuestNowNs() / 100;
        if (VhWhvSetReg(pVm, WHvX64RegisterTsc, tsc))
            VhLog("snap: TSC rebase failed (continuing)");
    }

    if (pVm->fSnapRamLazy)
    {
        /* Event delivery is atomic: an interrupt reads the IDT entry, may
           consult the TSS, and pushes onto the current stack in one step.
           If any of those pages is unmapped at that moment the delivery
           faults and WHPX reports an unrecoverable exception instead of a
           clean memory-access exit. Page in everything the restored
           register state references, plus the low 1 MiB. */
        u64 nsPrePageStart = VhGuestNowNs();
        u64 cPre = 0;
        for (u32 i = 0; i < cRegs; i++)
        {
            u64 uAddr = 0;
            switch (aRegs[i].enm)
            {
            case WHvX64RegisterRip:  uAddr = aRegs[i].val.Reg64; break;
            case WHvX64RegisterRsp:  uAddr = aRegs[i].val.Reg64; break;
            case WHvX64RegisterIdtr: uAddr = aRegs[i].val.Table.Base; break;
            case WHvX64RegisterGdtr: uAddr = aRegs[i].val.Table.Base; break;
            case WHvX64RegisterTr:   uAddr = aRegs[i].val.Table.Base; break;
            default: break;
            }
            if (uAddr && uAddr < pVm->mem.cbRam &&
                !(uAddr >= 0xA0000 && uAddr < 0x100000) &&
                VhSnapLazyLoad(pVm, uAddr) == 0)
                cPre++;
        }
        for (u64 uGpa = 0; uGpa < 0x100000 && uGpa < pVm->mem.cbRam; uGpa += VH_LAZY_CHUNK)
        {
            if (uGpa >= 0xA0000 && uGpa < 0x100000)
                continue;
            if (VhSnapLazyLoad(pVm, uGpa) == 0)
                cPre++;
        }
        /* the framebuffer is read host-side by the frame-copy thread; page
           it in now so the first frame after resume is already correct */
        if (pVm->mem.gpaFb && pVm->mem.cbFb)
        {
            u64 uEnd = pVm->mem.gpaFb + pVm->mem.cbFb;
            if (uEnd > pVm->mem.cbRam)
                uEnd = pVm->mem.cbRam;
            for (u64 uGpa = pVm->mem.gpaFb; uGpa < uEnd; uGpa += VH_LAZY_CHUNK)
                if (VhSnapLazyLoad(pVm, uGpa) == 0)
                    cPre++;
        }
        VhLog("snap: pre-paged %llu chunks in %llu ms",
              (unsigned long long)cPre,
              (unsigned long long)((VhGuestNowNs() - nsPrePageStart) / 1000000ULL));
    }

    rc = 0;
    VhLog("snap: restored in %llu ms (open=%llu header=%llu stale=%llu start-log=%llu pre=%llu sections=%llu RAM=%llu lazy=%llu fixup=%llu regs=%llu ms; cmdline '%s')",
          (unsigned long long)(VhGuestNowNs() - ns0) / 1000000ULL,
          (unsigned long long)nsOpenMs,
          (unsigned long long)nsHeaderMs,
          (unsigned long long)nsStalenessMs,
          (unsigned long long)nsRestoreLogMs,
          (unsigned long long)nsPreflightMs,
          (unsigned long long)nsSectionsMs,
          (unsigned long long)nsRamMs,
          (unsigned long long)nsAdoptMs,
          (unsigned long long)nsFixupMs,
          (unsigned long long)nsRegMs,
          pVm->szCmdLine);
    if (pVm->fSnapRamLazy)
        VhLog("snap: guest RAM will be paged in lazily from the snapshot file");

restore_done:
    if (rc != 0 && pVm->fSnapRamLazy)
    {
        /* failed after the lazy RAM tag: drop the backing so the caller's
           fresh-boot fallback starts from clean, zeroed RAM */
        if (pVm->pFileLazy)
            fclose(pVm->pFileLazy);
        pVm->pFileLazy = NULL;
        free(pVm->pbRamLoaded);
        pVm->pbRamLoaded = NULL;
        pVm->fSnapRamLazy = false;
    }
    free(aRegs);
    return rc;
}

/* ------------------------------------------------------------------ */
/* LAZY RAM PAGE-IN                                                     */
/* ------------------------------------------------------------------ */
/* Called from the run loop when the guest touches an unmapped GPA inside
   guest RAM after a v2 restore. Loads the surrounding 64 KiB chunk from
   the snapshot file into the backing allocation and maps it into the
   partition; the faulting instruction is then re-executed by the caller. */
int VhSnapLazyLoad(VH_VM *pVm, u64 uGpa)
{
    if (!pVm->fSnapRamLazy || !pVm->pFileLazy || !pVm->pbRamLoaded)
        return 1;
    if (uGpa >= 0xA0000 && uGpa < 0x100000)
        return 0;   /* legacy hole: backed by scratch, never by the file */
    u64 uOff = uGpa & ~(VH_LAZY_CHUNK - 1);
    if (uOff >= pVm->mem.cbRam)
        return 1;
    u32 iChunk = (u32)(uOff / VH_LAZY_CHUNK);
    u64 cbChunk = pVm->mem.cbRam - uOff;
    if (cbChunk > VH_LAZY_CHUNK)
        cbChunk = VH_LAZY_CHUNK;
    if (pVm->pbRamLoaded[iChunk >> 3] & (1u << (iChunk & 7)))
        return 0; /* already loaded and mapped, including overlapping prefetches */
    {
        if (_fseeki64(pVm->pFileLazy, (LONGLONG)(SNAP_RAM_OFF + uOff), SEEK_SET) ||
            fread(pVm->mem.pbRam + uOff, 1, (size_t)cbChunk, pVm->pFileLazy) != (size_t)cbChunk)
        {
            VhLog("snap: lazy page-in read failed @ GPA %#llx", (unsigned long long)uGpa);
            return 1;
        }
        if (VhWhvMapRamChunk(pVm, uOff, pVm->mem.pbRam + uOff, cbChunk))
            return 1;
        pVm->pbRamLoaded[iChunk >> 3] |= (u8)(1u << (iChunk & 7));
        pVm->cLazyChunks++;
    }
    return 0;
}
