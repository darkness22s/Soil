/*
 * Soil VM engine - guest loader.
 *
 * Loads a Linux bzImage (vmlinuz) + initrd (core.gz) into guest RAM and
 * prepares the boot_params (zero page) so the kernel can be entered directly
 * at its 32-bit entry point using the Linux x86 boot protocol - the same
 * "start the kernel directly" approach VirtualBox uses to hand a VM to the
 * hypervisor, minus any BIOS/firmware emulation.
 */
#define WIN32_LEAN_AND_MEAN
#include "vmdefs.h"
#include <string.h>

/* ---- little helpers ------------------------------------------------- */
static u16 r16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 r32(const u8 *p) { return (u32)(p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24)); }
static void w8(u8 *p, u32 off, u8 v)  { p[off] = v; }
static void w16(u8 *p, u32 off, u16 v) { p[off] = (u8)v; p[off + 1] = (u8)(v >> 8); }
static void w32(u8 *p, u32 off, u32 v) { p[off] = (u8)v; p[off+1] = (u8)(v >> 8); p[off+2] = (u8)(v >> 16); p[off+3] = (u8)(v >> 24); }
static void w64(u8 *p, u32 off, u64 v) { w32(p, off, (u32)v); w32(p, off + 4, (u32)(v >> 32)); }

u64 VhGuestNowNs(void)
{
    static u64 s_freq = 0;
    if (!s_freq)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        s_freq = (u64)f.QuadPart;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    u64 cTicks = (u64)now.QuadPart;
    /* split math: avoids the u64 overflow of (ticks * 1e9) above ~18s at 10MHz */
    return (cTicks / s_freq) * 1000000000ULL + ((cTicks % s_freq) * 1000000000ULL) / s_freq;
}

/* ---- boot_params field offsets (mirrors struct boot_params) --------- */
#define BP_SCREEN_INFO      0x000
#define BP_EXT_CMDLINE_PTR  0x0C8
#define BP_ALT_MEM_K        0x1E0
#define BP_E820_ENTRIES     0x1E8
#define BP_SENTINEL         0x1EF
#define BP_SETUP_HDR        0x1F1
#define BP_E820_TABLE       0x2D0
#define BP_E820_ENTRY_SZ    20

/* setup_header offsets (absolute within boot_params) */
#define HDR_SETUP_SECTS     0x1F1
#define HDR_SYSSIZE         0x1F4
#define HDR_BOOT_FLAG       0x1FE
#define HDR_MAGIC           0x202
#define HDR_VERSION         0x206
#define HDR_TYPE_OF_LOADER  0x210
#define HDR_LOADFLAGS       0x211
#define HDR_CODE32_START    0x214
#define HDR_RAMDISK_IMAGE   0x218
#define HDR_RAMDISK_SIZE    0x21C
#define HDR_CMD_LINE_PTR    0x228
#define HDR_XLOADFLAGS      0x236
#define HDR_KERNEL_INFO     0x268

#define BOOT_FLAG_VALUE     0xAA55
#define HDR_MAGIC_VALUE     0x53726448U   /* "HdrS" */

#define VIDEO_TYPE_VLFB     0x23

/* screen_info offsets */
#define SI_ORIG_VIDEO_COLS  0x07
#define SI_ORIG_VIDEO_LINES 0x0E
#define SI_ISVGA           0x0F
#define SI_LFB_WIDTH        0x12
#define SI_LFB_HEIGHT       0x14
#define SI_LFB_DEPTH        0x16
#define SI_LFB_BASE         0x18
#define SI_LFB_SIZE         0x1C
#define SI_LFB_LINELENGTH   0x24
#define SI_RED_SIZE         0x26
#define SI_RED_POS          0x27
#define SI_GREEN_SIZE       0x28
#define SI_GREEN_POS        0x29
#define SI_BLUE_SIZE        0x2A
#define SI_BLUE_POS         0x2B
#define SI_RSVD_SIZE        0x2C
#define SI_RSVD_POS         0x2D

typedef struct E820ENT
{
    u64 uAddr;
    u64 cbSize;
    u32 uType;
} E820ENT;

static void addE820(E820ENT *paEnt, u32 *pcEnt, u64 uAddr, u64 cbSize, u32 uType)
{
    if (!cbSize)
        return;
    paEnt[*pcEnt].uAddr = uAddr;
    paEnt[*pcEnt].cbSize = cbSize;
    paEnt[*pcEnt].uType = uType;
    (*pcEnt)++;
}

static int readFileAll(const char *pszPath, u8 **ppbData, u64 *pcbData)
{
    FILE *pFile = fopen(pszPath, "rb");
    if (!pFile)
    {
        VhLog("guest: cannot open '%s'", pszPath);
        return 1;
    }
    if (_fseeki64(pFile, 0, SEEK_END) != 0)
    {
        fclose(pFile);
        return 1;
    }
    __int64 cb = _ftelli64(pFile);
    if (cb <= 0)
    {
        fclose(pFile);
        return 1;
    }
    if (_fseeki64(pFile, 0, SEEK_SET) != 0)
    {
        fclose(pFile);
        return 1;
    }
    u8 *pb = (u8 *)malloc((size_t)cb);
    if (!pb)
    {
        fclose(pFile);
        return 1;
    }
    if (fread(pb, 1, (size_t)cb, pFile) != (size_t)cb)
    {
        free(pb);
        fclose(pFile);
        return 1;
    }
    fclose(pFile);
    *ppbData = pb;
    *pcbData = (u64)cb;
    return 0;
}

int VhGuestLoad(VH_VM *pVm, const char *pszKernel, const char *pszInitrd, const char *pszCmdLine)
{
    u8 *pbKernel = NULL;
    u64 cbKernel = 0;
    u8 *pbInitrd = NULL;
    u64 cbInitrd = 0;
    int rc = 1;

    if (readFileAll(pszKernel, &pbKernel, &cbKernel))
        return 1;
    if (readFileAll(pszInitrd, &pbInitrd, &cbInitrd))
    {
        free(pbKernel);
        return 1;
    }

    if (cbKernel < 1024 || r16(pbKernel + HDR_BOOT_FLAG) != BOOT_FLAG_VALUE ||
        r32(pbKernel + HDR_MAGIC) != HDR_MAGIC_VALUE)
    {
        VhLog("guest: '%s' is not a bzImage (bad boot flag / header magic)", pszKernel);
        goto out;
    }

    u16 uProtocol = r16(pbKernel + HDR_VERSION);
    u8  cbSetupSects = pbKernel[HDR_SETUP_SECTS];
    u32 uSysSize = r32(pbKernel + HDR_SYSSIZE);
    u32 uCode32 = r32(pbKernel + HDR_CODE32_START);
    u8  bLoadFlags = pbKernel[HDR_LOADFLAGS];
    u16 uXloadFlags = r16(pbKernel + HDR_XLOADFLAGS);

    VhLog("guest: bzImage ver %u.%02u, setup_sects=%u, syssize=%u KiB, code32=%#x, loadflags=%#x",
          uProtocol >> 8, uProtocol & 0xff, cbSetupSects, (uSysSize * 16) >> 10, uCode32, bLoadFlags);

    if (uProtocol < 0x0200)
    {
        VhLog("guest: boot protocol too old (%#x)", uProtocol);
        goto out;
    }
    if (uXloadFlags & 0x0001)   /* XLF_KERNEL_64 */
    {
        VhLog("guest: 64-bit kernel detected - booting via the protected-mode entry");
    }
    if (uCode32 < VH_KERNEL_GPA)
        uCode32 = (u32)VH_KERNEL_GPA;

    u64 cbKernelImage = (u64)uSysSize * 16;
    u64 offKernelImage = ((u64)cbSetupSects + 1) * 512;
    if (offKernelImage >= cbKernel)
    {
        VhLog("guest: kernel file too small for its own header");
        goto out;
    }
    if (offKernelImage + cbKernelImage > cbKernel)
        cbKernelImage = cbKernel - offKernelImage;

    if (uCode32 + cbKernelImage >= VH_INITRD_GPA)
    {
        VhLog("guest: protected kernel (%#x+%llu KiB) would overlap the initrd region",
              uCode32, (unsigned long long)(cbKernelImage >> 10));
        goto out;
    }
    if (VH_INITRD_GPA + cbInitrd >= pVm->mem.cbRam - VH_FB_TAIL)
    {
        VhLog("guest: initrd does not fit below the framebuffer (ram=%llu MiB)", (unsigned long long)(pVm->mem.cbRam >> 20));
        goto out;
    }

    /* ---- memory: kernel + initrd ---- */
    memcpy(pVm->mem.pbRam + uCode32, pbKernel + offKernelImage, (size_t)cbKernelImage);
    memcpy(pVm->mem.pbRam + VH_INITRD_GPA, pbInitrd, (size_t)cbInitrd);
    pVm->mem.gpaKernel = uCode32;
    pVm->mem.cbKernel = cbKernelImage;
    pVm->mem.gpaInitrd = VH_INITRD_GPA;
    pVm->mem.cbInitrd = cbInitrd;

    /* framebuffer at top of RAM */
    pVm->mem.gpaFb = pVm->mem.cbRam - VH_FB_TAIL;
    pVm->mem.cbFb = VH_FB_SIZE;
    memset(pVm->mem.pbRam + pVm->mem.gpaFb, 0x1f, (size_t)pVm->mem.cbFb);

    /* ---- boot_params ---- */
    u8 *pbBp = pVm->mem.pbRam + VH_BOOTPARAMS_GPA;
    memset(pbBp, 0, VH_PAGE);

    /* copy the setup header verbatim (hdr starts at 0x1F1; 0x96 bytes covers through kernel_info_offset) */
    memcpy(pbBp + BP_SETUP_HDR, pbKernel + BP_SETUP_HDR, 0x96);
    w8(pbBp, BP_SENTINEL, 0xFF);

    /* overrides */
    w8(pbBp, HDR_TYPE_OF_LOADER, 0xFF);                       /* "unknown bootloader" */
    w32(pbBp, HDR_CMD_LINE_PTR, (u32)VH_CMDLINE_GPA);
    w32(pbBp, BP_EXT_CMDLINE_PTR, 0);
    w32(pbBp, HDR_RAMDISK_IMAGE, (u32)VH_INITRD_GPA);
    w32(pbBp, HDR_RAMDISK_SIZE, (u32)cbInitrd);
    w32(pbBp, HDR_CODE32_START, uCode32);

    /* cmdline */
    memset(pVm->mem.pbRam + VH_CMDLINE_GPA, 0, VH_CMDLINE_SIZE);
    strncpy(pVm->szCmdLine, pszCmdLine, sizeof(pVm->szCmdLine) - 1);
    strcpy((char *)(pVm->mem.pbRam + VH_CMDLINE_GPA), pVm->szCmdLine);

    /* ---- screen_info: linear framebuffer (VIDEO_TYPE_VLFB) ---- */
    {
        u8 *pbSi = pbBp + BP_SCREEN_INFO;
        w8(pbSi, SI_ORIG_VIDEO_COLS, 80);
        w8(pbSi, SI_ORIG_VIDEO_LINES, 25);
        w8(pbSi, SI_ISVGA, VIDEO_TYPE_VLFB);
        w16(pbSi, SI_LFB_WIDTH, VH_FB_W);
        w16(pbSi, SI_LFB_HEIGHT, VH_FB_H);
        w16(pbSi, SI_LFB_DEPTH, VH_FB_BPP);
        w32(pbSi, SI_LFB_BASE, (u32)pVm->mem.gpaFb);
        w32(pbSi, SI_LFB_SIZE, (u32)((VH_FB_SIZE + 0xFFFF) >> 16)); /* 64 KiB units */
        w16(pbSi, SI_LFB_LINELENGTH, VH_FB_STRIDE);
        w8(pbSi, SI_RED_SIZE, 8);   w8(pbSi, SI_RED_POS, 16); /* X8R8G8B8 */
        w8(pbSi, SI_GREEN_SIZE, 8); w8(pbSi, SI_GREEN_POS, 8);
        w8(pbSi, SI_BLUE_SIZE, 8);  w8(pbSi, SI_BLUE_POS, 0);
        w8(pbSi, SI_RSVD_SIZE, 8);  w8(pbSi, SI_RSVD_POS, 24);
    }

    /* ---- e820 map ---- */
    {
        E820ENT aEnt[16];
        u32 cEnt = 0;
        u64 cbInitrdEnd = VH_ALIGN_UP(VH_INITRD_GPA + cbInitrd, VH_PAGE);
        u64 uFbStart = pVm->mem.gpaFb;
        u64 uFbEnd = pVm->mem.gpaFb + pVm->mem.cbFb;

        addE820(aEnt, &cEnt, 0x000000, 0x09FC00, 1);                    /* low RAM */
        addE820(aEnt, &cEnt, 0x09FC00, 0x000400, 2);                    /* EBDA */
        addE820(aEnt, &cEnt, 0x0A0000, 0x060000, 2);                    /* VGA/BIOS hole */
        addE820(aEnt, &cEnt, 0x100000, VH_INITRD_GPA - 0x100000, 1);    /* main RAM */
        addE820(aEnt, &cEnt, VH_INITRD_GPA, cbInitrdEnd - VH_INITRD_GPA, 2); /* initrd */
        addE820(aEnt, &cEnt, cbInitrdEnd, uFbStart - cbInitrdEnd, 1);   /* RAM above initrd */
        addE820(aEnt, &cEnt, uFbStart, uFbEnd - uFbStart, 2);           /* framebuffer */
        addE820(aEnt, &cEnt, uFbEnd, pVm->mem.cbRam - uFbEnd, 2);       /* tail */

        w8(pbBp, BP_E820_ENTRIES, (u8)cEnt);
        w32(pbBp, BP_ALT_MEM_K, (u32)((pVm->mem.cbRam - 0x100000) >> 10));
        u8 *pbEnt = pbBp + BP_E820_TABLE;
        for (u32 i = 0; i < cEnt; i++)
        {
            w64(pbEnt, i * BP_E820_ENTRY_SZ + 0,  aEnt[i].uAddr);
            w64(pbEnt, i * BP_E820_ENTRY_SZ + 8,  aEnt[i].cbSize);
            w32(pbEnt, i * BP_E820_ENTRY_SZ + 16, aEnt[i].uType);
        }
        VhLog("guest: e820 entries=%u, ram=%llu MiB, initrd=%llu KiB, fb @ %#llx (%u KiB)",
              cEnt, (unsigned long long)(pVm->mem.cbRam >> 20),
              (unsigned long long)(cbInitrd >> 10),
              (unsigned long long)pVm->mem.gpaFb, (unsigned)(pVm->mem.cbFb >> 10));
    }

    VhLog("guest: kernel %llu KiB @ %#llx, cmdline '%s'",
          (unsigned long long)(cbKernelImage >> 10), (unsigned long long)uCode32, pVm->szCmdLine);
    rc = 0;

out:
    free(pbKernel);
    free(pbInitrd);
    return rc;
}

int VhGuestSetupCpu(VH_VM *pVm)
{
    int rc = 0;

    /* general purpose */
    {
        WHV_REGISTER_NAME  aenm[] = {
            WHvX64RegisterRax, WHvX64RegisterRbx, WHvX64RegisterRcx, WHvX64RegisterRdx,
            WHvX64RegisterRsi, WHvX64RegisterRdi, WHvX64RegisterRbp, WHvX64RegisterRsp,
            WHvX64RegisterR8, WHvX64RegisterR9, WHvX64RegisterR10, WHvX64RegisterR11,
            WHvX64RegisterR12, WHvX64RegisterR13, WHvX64RegisterR14, WHvX64RegisterR15,
        };
        WHV_REGISTER_VALUE av[16];
        memset(av, 0, sizeof(av));
        av[4].Reg64 = VH_BOOTPARAMS_GPA;        /* RSI = boot_params */
        av[7].Reg64 = VH_ENTRY_STACK;           /* RSP */
        rc |= VhWhvSetRegs(pVm, aenm, av, 16);
    }

    {
        WHV_REGISTER_NAME aenm[] = {
            WHvX64RegisterRip, WHvX64RegisterRflags, WHvX64RegisterCr0, WHvX64RegisterCr2,
            WHvX64RegisterCr3, WHvX64RegisterCr4, WHvX64RegisterCr8, WHvX64RegisterEfer,
            WHvX64RegisterDr0, WHvX64RegisterDr1, WHvX64RegisterDr2, WHvX64RegisterDr3,
            WHvX64RegisterDr6, WHvX64RegisterDr7, WHvX64RegisterTsc,
        };
        WHV_REGISTER_VALUE av[15];
        memset(av, 0, sizeof(av));
        av[0].Reg64  = pVm->mem.gpaKernel;      /* RIP = protected-mode entry */
        av[1].Reg64  = 0x2;                     /* RFLAGS: reserved bit 1 */
        av[2].Reg64  = 0x00000011;              /* CR0: PE | ET */
        av[4].Reg64  = 0;                       /* CR3 */
        av[5].Reg64  = 0;                       /* CR4 */
        av[6].Reg64  = 0;                       /* CR8 */
        av[7].Reg64  = 0;                       /* EFER (no long mode) */
        av[12].Reg64 = 0xFFFF0FF0;              /* DR6 */
        av[13].Reg64 = 0x00000400;              /* DR7 */
        av[14].Reg64 = VhGuestNowNs() / 100;    /* TSC (ns/100 = 10 MHz-ish) */
        rc |= VhWhvSetRegs(pVm, aenm, av, 15);
    }

    /* tables: left empty; the kernel loads its own before use */
    {
        WHV_REGISTER_NAME aenm[] = { WHvX64RegisterGdtr, WHvX64RegisterIdtr };
        WHV_REGISTER_VALUE av[2];
        memset(av, 0, sizeof(av));
        av[0].Table.Limit = 0xFFFF;
        av[1].Table.Limit = 0xFFFF;
        rc |= VhWhvSetRegs(pVm, aenm, av, 2);
    }

    /* flat segmentation */
    rc |= VhWhvSetSegment(pVm, WHvX64RegisterCs, 0, 0xFFFFFFFF, 0x0008, 0x0B, false, true, true, 0);
    rc |= VhWhvSetSegment(pVm, WHvX64RegisterDs, 0, 0xFFFFFFFF, 0x0010, 0x03, false, true, true, 0);
    rc |= VhWhvSetSegment(pVm, WHvX64RegisterEs, 0, 0xFFFFFFFF, 0x0010, 0x03, false, true, true, 0);
    rc |= VhWhvSetSegment(pVm, WHvX64RegisterFs, 0, 0xFFFFFFFF, 0x0010, 0x03, false, true, true, 0);
    rc |= VhWhvSetSegment(pVm, WHvX64RegisterGs, 0, 0xFFFFFFFF, 0x0010, 0x03, false, true, true, 0);
    rc |= VhWhvSetSegment(pVm, WHvX64RegisterSs, 0, 0xFFFFFFFF, 0x0010, 0x03, false, true, true, 0);

    if (rc)
        VhLog("guest: CPU setup failed");
    else
        VhLog("guest: vCPU ready at %#llx, ESI=%#llx (Linux boot protocol)", (unsigned long long)pVm->mem.gpaKernel, (unsigned long long)VH_BOOTPARAMS_GPA);
    return rc;
}
