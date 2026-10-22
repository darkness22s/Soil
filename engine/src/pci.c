/*
 * Soil VM engine - minimal PCI configuration space.
 *
 * Presents an i440FX-style host bridge at 00:00.0 and a Realtek RTL8029
 * (NE2000-compatible) NIC at 00:01.0 so the guest's ne2k-pci driver binds.
 */
#include "vmdefs.h"

#define VH_PCI_CONFIG_ADDR_PORT 0xCF8
#define VH_PCI_CONFIG_DATA_PORT 0xCFC
#define VH_PCI_RESET_PORT       0xCF9

#define VH_PCI_NIC_DEVFN        0x08
#define VH_PCI_NIC_BAR_IO       0x0301u

#define VH_PCI_BLK_DEVFN        0x10
#define VH_PCI_BLK_BAR_IO       0xC001u    /* I/O base 0xC000, 0x40-byte window */

static u32 s_uCfgAddr;
static u16 s_uNicCommand;
static u32 s_uNicBar;       /* BAR0 as last written (0xFFFFFF01 while probing) */
static u16 s_uNicBase;      /* decoded I/O base of the NIC window (0 = off) */
static u16 s_uBlkCommand;
static u32 s_uBlkBar;       /* BAR0 as last written (0xFFFFFFC1 while probing) */
static u16 s_uBlkBase;      /* decoded I/O base of the blk window (0 = off) */

void VhPciInit(void)
{
    s_uCfgAddr = 0;
    s_uNicCommand = 0;
    s_uNicBar = VH_PCI_NIC_BAR_IO;
    s_uNicBase = (u16)(VH_PCI_NIC_BAR_IO & 0xFF00);
    s_uBlkCommand = 0;
    s_uBlkBar = VH_PCI_BLK_BAR_IO;
    s_uBlkBase = (u16)(VH_PCI_BLK_BAR_IO & 0xFF00);
}

u16 VhPciNicIoBase(void)
{
    return s_uNicBase;
}

u16 VhPciBlkIoBase(void)
{
    return VhVirtioBlkPresent() ? s_uBlkBase : 0;
}

static u32 pciNicRead(u8 bReg)
{
    switch (bReg & 0xFC)
    {
    case 0x00: return 0x802910ECu;                       /* 10EC:8029 */
    case 0x04: return (u32)s_uNicCommand | 0x02000000u;  /* status: medium devsel */
    case 0x08: return 0x02000000u;                       /* ethernet controller, rev 0 */
    case 0x0C: return 0x00000000u;                       /* header type 0 */
    case 0x10: return s_uNicBar;                         /* BAR0 */
    case 0x2C: return 0x802910ECu;                       /* subsystem id / vendor */
    case 0x3C: return 0x00000109u;                       /* int line 9, pin INTA# */
    default:   return 0;
    }
}

static u32 pciHostRead(u8 bReg)
{
    switch (bReg & 0xFC)
    {
    case 0x00: return 0x12378086u;                       /* 8086:1237 */
    case 0x04: return 0x02000000u;
    case 0x08: return 0x06000002u;                       /* host bridge, rev 2 */
    default:   return 0;
    }
}

static u32 pciBlkRead(u8 bReg)
{
    switch (bReg & 0xFC)
    {
    case 0x00: return 0x10011AF4u;                       /* 1AF4:1001 (legacy virtio-blk) */
    case 0x04: return (u32)s_uBlkCommand | 0x02000000u;  /* status: medium devsel */
    case 0x08: return 0x01800000u;                       /* mass storage, rev 0 */
    case 0x0C: return 0x00000000u;                       /* header type 0 */
    case 0x10: return s_uBlkBar;                         /* BAR0 (I/O) */
    case 0x2C: return 0x00021AF4u;                       /* subsystem: blk (0x0002) */
    case 0x3C: return (u32)VH_VBLK_IRQ | 0x0100u;   /* int line 11, pin INTA# */
    default:   return 0;
    }
}

static void pciBlkWrite(u8 bReg, u32 uValue)
{
    switch (bReg & 0xFC)
    {
    case 0x04:
        s_uBlkCommand = (u16)uValue;
        break;
    case 0x10:
        if ((uValue & 0xFFFFFFC0u) == 0xFFFFFFC0u)
        {
            /* size probe: expose the I/O size mask (0x40 bytes) */
            s_uBlkBar = 0xFFFFFFC1u;
            s_uBlkBase = 0;
        }
        else if ((uValue & 0xFFFFFFC0u) == 0)
        {
            s_uBlkBar = 0;
            s_uBlkBase = 0;
        }
        else
        {
            s_uBlkBar = uValue & 0xFFFFFFC1u;
            s_uBlkBase = (u16)(uValue & 0xFFFFFFC0u);
        }
        break;
    default:
        break;
    }
}

static void pciNicWrite(u8 bReg, u32 uValue)
{
    switch (bReg & 0xFC)
    {
    case 0x04:
        s_uNicCommand = (u16)uValue;
        break;
    case 0x10:
        if ((uValue & 0xFFFFFFFCu) == 0xFFFFFFFCu)
        {
            /* size probe: expose the I/O size mask (0x100 bytes) */
            s_uNicBar = 0xFFFFFF01u;
            s_uNicBase = 0;
        }
        else if ((uValue & 0xFFFFFFFCu) == 0)
        {
            s_uNicBar = 0;
            s_uNicBase = 0;
        }
        else
        {
            s_uNicBar = uValue & 0xFFFFFF01u;
            s_uNicBase = (u16)(uValue & 0xFFFFFF00u);
        }
        break;
    default:
        break;
    }
}

bool VhPciIo(VH_VM *pVm, u16 uPort, bool fWrite, u32 cbWidth, u32 *puValue)
{
    (void)pVm;

    if (uPort >= VH_PCI_CONFIG_ADDR_PORT && uPort <= VH_PCI_CONFIG_ADDR_PORT + 3)
    {
        u32 cbOff = (u32)(uPort - VH_PCI_CONFIG_ADDR_PORT);
        u32 uMask = (cbWidth == 4) ? 0xFFFFFFFFu : (cbWidth == 2) ? 0xFFFFu : 0xFFu;
        u32 uShift = (cbWidth == 4) ? 0 : cbOff * 8;
        if (fWrite)
        {
            if (cbWidth == 4)
                s_uCfgAddr = *puValue;
            else
                s_uCfgAddr = (s_uCfgAddr & ~(uMask << uShift)) | ((*puValue & uMask) << uShift);
        }
        else
        {
            *puValue = (s_uCfgAddr >> uShift) & uMask;
        }
        return true;
    }

    if (uPort >= VH_PCI_CONFIG_DATA_PORT && uPort <= VH_PCI_CONFIG_DATA_PORT + 3)
    {
        u32 cbOff = (u32)(uPort - VH_PCI_CONFIG_DATA_PORT);
        u32 uMask = (cbWidth == 4) ? 0xFFFFFFFFu : (cbWidth == 2) ? 0xFFFFu : 0xFFu;
        u32 uShift = (cbWidth == 4) ? 0 : cbOff * 8;
        u8  bBus, bDevFn, bReg;
        u32 uVal;

        if (!(s_uCfgAddr & 0x80000000u))
        {
            if (!fWrite)
                *puValue = uMask;
            return true;
        }

        bBus = (u8)((s_uCfgAddr >> 16) & 0xFF);
        bDevFn = (u8)((s_uCfgAddr >> 8) & 0xFF);
        bReg = (u8)(s_uCfgAddr & 0xFC);

        if (bBus != 0 || (bDevFn != 0x00 && bDevFn != VH_PCI_NIC_DEVFN && bDevFn != VH_PCI_BLK_DEVFN))
        {
            /* no device on this bus: reads return all ones, writes ignored */
            if (!fWrite)
                *puValue = uMask;
            return true;
        }

        if (bDevFn == VH_PCI_BLK_DEVFN && !VhVirtioBlkPresent())
        {
            /* no disk attached: hide the device entirely */
            if (!fWrite)
                *puValue = uMask;
            return true;
        }

        if (bDevFn == VH_PCI_NIC_DEVFN)
        {
            uVal = pciNicRead(bReg);
            if (fWrite)
            {
                if (cbWidth == 4)
                    pciNicWrite(bReg, *puValue);
                else
                    pciNicWrite(bReg, (uVal & ~(uMask << uShift)) | ((*puValue & uMask) << uShift));
            }
            else
            {
                *puValue = (uVal >> uShift) & uMask;
            }
        }
        else if (bDevFn == VH_PCI_BLK_DEVFN)
        {
            uVal = pciBlkRead(bReg);
            if (fWrite)
            {
                if (cbWidth == 4)
                    pciBlkWrite(bReg, *puValue);
                else
                    pciBlkWrite(bReg, (uVal & ~(uMask << uShift)) | ((*puValue & uMask) << uShift));
            }
            else
            {
                *puValue = (uVal >> uShift) & uMask;
            }
        }
        else
        {
            uVal = pciHostRead(bReg);
            if (!fWrite)
                *puValue = (uVal >> uShift) & uMask;
        }
        return true;
    }

    if (uPort == VH_PCI_RESET_PORT)
    {
        if (!fWrite)
            *puValue = 0;
        return true;
    }
    return false;
}
