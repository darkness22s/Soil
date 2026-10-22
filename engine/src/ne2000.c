/*
 * Soil VM engine - RTL8029 (NE2000-compatible) NIC model.
 *
 * Adapted for the Soil engine from qemu-0.14.1 hw/ne2000.c
 * (Copyright (c) 2003-2004 Fabrice Bellard; MIT license).
 */
#include "vmdefs.h"

#define MAX_ETH_FRAME_SIZE 1514

/* Register offsets (page-independent). */
#define E8390_CMD       0x00
#define EN0_STARTPG     0x01
#define EN0_STOPPG      0x02
#define EN0_BOUNDARY    0x03
#define EN0_TSR         0x04
#define EN0_TPSR        0x04
#define EN0_TCNTLO      0x05
#define EN0_TCNTHI      0x06
#define EN0_ISR         0x07
#define EN0_RSARLO      0x08
#define EN0_RSARHI      0x09
#define EN0_RCNTLO      0x0a
#define EN0_RCNTHI      0x0b
#define EN0_RSR         0x0c
#define EN0_RXCR        0x0c
#define EN0_TXCR        0x0d
#define EN0_COUNTER0    0x0d
#define EN0_DCFG        0x0e
#define EN0_IMR         0x0f

#define EN1_PHYS        0x11
#define EN1_CURPAG      0x17
#define EN1_MULT        0x18

#define EN2_STARTPG     0x21
#define EN2_STOPPG      0x22

#define EN3_CONFIG0     0x33
#define EN3_CONFIG1     0x34
#define EN3_CONFIG2     0x35
#define EN3_CONFIG3     0x36

/* Command register bits. */
#define E8390_STOP      0x01
#define E8390_START     0x02
#define E8390_TRANS     0x04
#define E8390_RREAD     0x08
#define E8390_RWRITE    0x10
#define E8390_NODMA     0x20
#define E8390_PAGE0     0x00
#define E8390_PAGE1     0x40
#define E8390_PAGE2     0x80

/* ISR bits. */
#define ENISR_RX        0x01
#define ENISR_TX        0x02
#define ENISR_RX_ERR    0x04
#define ENISR_TX_ERR    0x08
#define ENISR_OVER      0x10
#define ENISR_COUNTERS  0x20
#define ENISR_RDC       0x40
#define ENISR_RESET     0x80

/* Received packet status. */
#define ENRSR_RXOK      0x01
#define ENRSR_PHY       0x20

/* Transmit status. */
#define ENTSR_PTX       0x01

#define NE2000_MEM_SIZE     (32 * 1024 + 16 * 1024)
#define NE2000_PMEM_START   (16 * 1024)
#define NE2000_PMEM_SIZE    (32 * 1024)
#define NE2000_PMEM_END     (NE2000_PMEM_SIZE + NE2000_PMEM_START)

#define NE2000_NIC_IRQ      9

typedef struct NE2000State
{
    u8  rxcr;
    u8  cmd;
    u32 start;
    u32 stop;
    u8  boundary;
    u8  tsr;
    u8  tpsr;
    u16 tcnt;
    u16 rcnt;
    u32 rsar;
    u8  rsr;
    u8  isr;
    u8  dcfg;
    u8  imr;
    u8  phys[6];
    u8  curpag;
    u8  mult[8];
    u8  mem[NE2000_MEM_SIZE];
} NE2000State;

static NE2000State s_ne2k;
static VH_VM *s_pVmNic;
static const u8 s_abMac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

static void ne2000_update_irq(NE2000State *s)
{
    int isr = (s->isr & s->imr) & 0x7f;

    if (!s_pVmNic)
        return;
    if (isr)
        VhDevRaiseIrq(s_pVmNic, NE2000_NIC_IRQ);
    else
        VhDevLowerIrq(s_pVmNic, NE2000_NIC_IRQ);
}

static void ne2000_reset(NE2000State *s)
{
    int i;

    s->isr = ENISR_RESET;
    memcpy(s->mem, s_abMac, 6);
    s->mem[14] = 0x57;
    s->mem[15] = 0x57;

    /* duplicate prom data */
    for (i = 15; i >= 0; i--)
    {
        s->mem[2 * i] = s->mem[i];
        s->mem[2 * i + 1] = s->mem[i];
    }
}

void Ne2000Init(VH_VM *pVm)
{
    NE2000State *s = &s_ne2k;

    s_pVmNic = pVm;
    memset(s, 0, sizeof(*s));
    s->rxcr = 0x0c;
    ne2000_reset(s);
}

#define POLYNOMIAL 0x04c11db6

/* From FreeBSD */
static int compute_mcast_idx(const u8 *ep)
{
    u32 crc;
    int carry, i, j;
    u8 b;

    crc = 0xffffffff;
    for (i = 0; i < 6; i++)
    {
        b = *ep++;
        for (j = 0; j < 8; j++)
        {
            carry = ((crc & 0x80000000u) ? 1 : 0) ^ (b & 0x01);
            crc <<= 1;
            b >>= 1;
            if (carry)
                crc = ((crc ^ POLYNOMIAL) | carry);
        }
    }
    return (crc >> 26);
}

static int ne2000_buffer_full(NE2000State *s)
{
    int avail, index, boundary;

    index = s->curpag << 8;
    boundary = s->boundary << 8;
    if (index < boundary)
        avail = boundary - index;
    else
        avail = (s->stop - s->start) - (index - boundary);
    if (avail < (MAX_ETH_FRAME_SIZE + 4))
        return 1;
    return 0;
}

int Ne2000CanReceive(void)
{
    NE2000State *s = &s_ne2k;

    if (s->cmd & E8390_STOP)
        return 1;
    return !ne2000_buffer_full(s);
}

#define MIN_BUF_SIZE 60

void Ne2000Receive(const u8 *pb, u32 cb)
{
    NE2000State *s = &s_ne2k;
    static u32 s_cRx = 0;
    if (s_cRx < 12) { VhLog("ne2k rx: len=%u rxcr=%#x cmd=%#x", cb, s->rxcr, s->cmd); s_cRx++; }
    int size = (int)cb;
    const u8 *buf = pb;
    u8 buf1[60];
    static const u8 broadcast_macaddr[6] =
        { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    u32 total_len, next, avail, len, index;

    if (s->cmd & E8390_STOP || ne2000_buffer_full(s))
        return;

    if (s->rxcr & 0x10)
    {
        /* promiscuous: receive all */
    }
    else
    {
        if (!memcmp(buf, broadcast_macaddr, 6))
        {
            /* broadcast address */
            if (!(s->rxcr & 0x04))
                return;
        }
        else if (buf[0] & 0x01)
        {
            /* multicast */
            u32 mcast_idx;
            if (!(s->rxcr & 0x08))
                return;
            mcast_idx = (u32)compute_mcast_idx(buf);
            if (!(s->mult[mcast_idx >> 3] & (1 << (mcast_idx & 7))))
                return;
        }
        else if (s->mem[0] == buf[0] &&
                 s->mem[2] == buf[1] &&
                 s->mem[4] == buf[2] &&
                 s->mem[6] == buf[3] &&
                 s->mem[8] == buf[4] &&
                 s->mem[10] == buf[5])
        {
            /* match */
        }
        else
        {
            return;
        }
    }

    /* if too small buffer, then expand it */
    if (size < MIN_BUF_SIZE)
    {
        memcpy(buf1, buf, (size_t)size);
        memset(buf1 + size, 0, (size_t)(MIN_BUF_SIZE - size));
        buf = buf1;
        size = MIN_BUF_SIZE;
    }

    index = s->curpag << 8;
    /* 4 bytes for header */
    total_len = (u32)size + 4;
    /* address for next packet (4 bytes for CRC) */
    next = index + ((total_len + 4 + 255) & ~0xffu);
    if (next >= s->stop)
        next -= (s->stop - s->start);
    /* prepare packet header */
    s->rsr = ENRSR_RXOK; /* receive status */
    if (buf[0] & 0x01)
        s->rsr |= ENRSR_PHY;
    s->mem[index] = s->rsr;
    s->mem[index + 1] = (u8)(next >> 8);
    s->mem[index + 2] = (u8)total_len;
    s->mem[index + 3] = (u8)(total_len >> 8);
    index += 4;

    /* write packet data */
    while (size > 0)
    {
        if (index <= s->stop)
            avail = s->stop - index;
        else
            avail = 0;
        len = (u32)size;
        if (len > avail)
            len = avail;
        memcpy(s->mem + index, buf, len);
        buf += len;
        index += len;
        if (index == s->stop)
            index = s->start;
        size -= (int)len;
    }
    s->curpag = (u8)(next >> 8);

    /* now we can signal we have received something */
    s->isr |= ENISR_RX;
    ne2000_update_irq(s);
}

static void ne2000_ioport_write(NE2000State *s, u32 uOffset, u32 val)
{
    int addr = (int)(uOffset & 0xf);
    int page, offset, index;

    if (addr == E8390_CMD)
    {
        /* control register */
        s->cmd = (u8)val;
        if (!(val & E8390_STOP))   /* START bit makes no sense on RTL8029... */
        {
            s->isr &= ~ENISR_RESET;
            /* test specific case: zero length transfer */
            if ((val & (E8390_RREAD | E8390_RWRITE)) &&
                s->rcnt == 0)
            {
                s->isr |= ENISR_RDC;
                ne2000_update_irq(s);
            }
            if (val & E8390_TRANS)
            {
                index = (s->tpsr << 8);
                /* next 2 lines are a hack to make netware 3.11 work */
                if (index >= NE2000_PMEM_END)
                    index -= NE2000_PMEM_SIZE;
                /* fail safe: check range on the transmitted length */
                if (index + s->tcnt <= NE2000_PMEM_END)
                {
                    VhNetInput(s->mem + index, s->tcnt);
                }
                /* signal end of transfer */
                s->tsr = ENTSR_PTX;
                s->isr |= ENISR_TX;
                s->cmd &= ~E8390_TRANS;
                ne2000_update_irq(s);
            }
        }
    }
    else
    {
        page = s->cmd >> 6;
        offset = addr | (page << 4);
        switch (offset)
        {
        case EN0_STARTPG:
            s->start = val << 8;
            break;
        case EN0_STOPPG:
            s->stop = val << 8;
            break;
        case EN0_BOUNDARY:
            s->boundary = (u8)val;
            break;
        case EN0_IMR:
            s->imr = (u8)val;
            ne2000_update_irq(s);
            break;
        case EN0_TPSR:
            s->tpsr = (u8)val;
            break;
        case EN0_TCNTLO:
            s->tcnt = (u16)((s->tcnt & 0xff00) | (val & 0xff));
            break;
        case EN0_TCNTHI:
            s->tcnt = (u16)((s->tcnt & 0x00ff) | ((val & 0xff) << 8));
            break;
        case EN0_RSARLO:
            s->rsar = (s->rsar & 0xff00) | (val & 0xff);
            break;
        case EN0_RSARHI:
            s->rsar = (s->rsar & 0x00ff) | ((u32)(val & 0xff) << 8);
            break;
        case EN0_RCNTLO:
            s->rcnt = (u16)((s->rcnt & 0xff00) | (val & 0xff));
            break;
        case EN0_RCNTHI:
            s->rcnt = (u16)((s->rcnt & 0x00ff) | ((val & 0xff) << 8));
            break;
        case EN0_RXCR:
            s->rxcr = (u8)val;
            break;
        case EN0_DCFG:
            s->dcfg = (u8)val;
            break;
        case EN0_ISR:
            s->isr &= ~((u8)val & 0x7f);
            ne2000_update_irq(s);
            break;
        case EN1_PHYS:
        case EN1_PHYS + 1:
        case EN1_PHYS + 2:
        case EN1_PHYS + 3:
        case EN1_PHYS + 4:
        case EN1_PHYS + 5:
            s->phys[offset - EN1_PHYS] = (u8)val;
            break;
        case EN1_CURPAG:
            s->curpag = (u8)val;
            break;
        case EN1_MULT:
        case EN1_MULT + 1:
        case EN1_MULT + 2:
        case EN1_MULT + 3:
        case EN1_MULT + 4:
        case EN1_MULT + 5:
        case EN1_MULT + 6:
        case EN1_MULT + 7:
            s->mult[offset - EN1_MULT] = (u8)val;
            break;
        }
    }
}

static u32 ne2000_ioport_read(NE2000State *s, u32 uOffset)
{
    int addr = (int)(uOffset & 0xf);
    int page, offset, ret;

    if (addr == E8390_CMD)
    {
        ret = s->cmd;
    }
    else
    {
        page = s->cmd >> 6;
        offset = addr | (page << 4);
        switch (offset)
        {
        case EN0_TSR:
            ret = s->tsr;
            break;
        case EN0_BOUNDARY:
            ret = s->boundary;
            break;
        case EN0_ISR:
            ret = s->isr;
            break;
        case EN0_RSARLO:
            ret = s->rsar & 0x00ff;
            break;
        case EN0_RSARHI:
            ret = s->rsar >> 8;
            break;
        case EN1_PHYS:
        case EN1_PHYS + 1:
        case EN1_PHYS + 2:
        case EN1_PHYS + 3:
        case EN1_PHYS + 4:
        case EN1_PHYS + 5:
            ret = s->phys[offset - EN1_PHYS];
            break;
        case EN1_CURPAG:
            ret = s->curpag;
            break;
        case EN1_MULT:
        case EN1_MULT + 1:
        case EN1_MULT + 2:
        case EN1_MULT + 3:
        case EN1_MULT + 4:
        case EN1_MULT + 5:
        case EN1_MULT + 6:
        case EN1_MULT + 7:
            ret = s->mult[offset - EN1_MULT];
            break;
        case EN0_RSR:
            ret = s->rsr;
            break;
        case EN2_STARTPG:
            ret = s->start >> 8;
            break;
        case EN2_STOPPG:
            ret = s->stop >> 8;
            break;
        case EN0_RCNTLO:        /* RTL8029ID0 */
            ret = 0x50;
            break;
        case EN0_RCNTHI:        /* RTL8029ID1 */
            ret = 0x43;
            break;
        case EN3_CONFIG0:
            ret = 0;            /* 10baseT media */
            break;
        case EN3_CONFIG2:
            ret = 0x40;         /* 10baseT active */
            break;
        case EN3_CONFIG3:
            ret = 0x40;         /* Full duplex */
            break;
        default:
            ret = 0x00;
            break;
        }
    }
    return (u32)ret;
}

static void ne2000_mem_writeb(NE2000State *s, u32 addr, u32 val)
{
    if (addr < 32 ||
        (addr >= NE2000_PMEM_START && addr < NE2000_MEM_SIZE))
    {
        s->mem[addr] = (u8)val;
    }
}

static void ne2000_mem_writew(NE2000State *s, u32 addr, u32 val)
{
    addr &= ~1u;
    if (addr < 32 ||
        (addr >= NE2000_PMEM_START && addr < NE2000_MEM_SIZE))
    {
        s->mem[addr] = (u8)val;
        s->mem[addr + 1] = (u8)(val >> 8);
    }
}

static void ne2000_mem_writel(NE2000State *s, u32 addr, u32 val)
{
    addr &= ~1u;
    if (addr < 32 ||
        (addr >= NE2000_PMEM_START && addr < NE2000_MEM_SIZE))
    {
        s->mem[addr] = (u8)val;
        s->mem[addr + 1] = (u8)(val >> 8);
        s->mem[addr + 2] = (u8)(val >> 16);
        s->mem[addr + 3] = (u8)(val >> 24);
    }
}

static u32 ne2000_mem_readb(NE2000State *s, u32 addr)
{
    if (addr < 32 ||
        (addr >= NE2000_PMEM_START && addr < NE2000_MEM_SIZE))
    {
        return s->mem[addr];
    }
    return 0xff;
}

static u32 ne2000_mem_readw(NE2000State *s, u32 addr)
{
    addr &= ~1u;
    if (addr < 32 ||
        (addr >= NE2000_PMEM_START && addr < NE2000_MEM_SIZE))
    {
        return (u32)s->mem[addr] | ((u32)s->mem[addr + 1] << 8);
    }
    return 0xffff;
}

static u32 ne2000_mem_readl(NE2000State *s, u32 addr)
{
    addr &= ~1u;
    if (addr < 32 ||
        (addr >= NE2000_PMEM_START && addr < NE2000_MEM_SIZE))
    {
        return (u32)s->mem[addr] |
               ((u32)s->mem[addr + 1] << 8) |
               ((u32)s->mem[addr + 2] << 16) |
               ((u32)s->mem[addr + 3] << 24);
    }
    return 0xffffffff;
}

static void ne2000_dma_update(NE2000State *s, int len)
{
    s->rsar += (u32)len;

    /* wrap */
    if (s->rsar == s->stop)
        s->rsar = s->start;

    if (s->rcnt <= (u16)len)
    {
        if (s->rcnt != 0)
            VhLog("ne2k: RDC SET (rcnt %u -> 0, len %d)", s->rcnt, len);
        s->rcnt = 0;
        /* signal end of transfer */
        s->isr |= ENISR_RDC;
        ne2000_update_irq(s);
    }
    else
    {
        s->rcnt = (u16)(s->rcnt - len);
    }
}

static void ne2000_asic_write(NE2000State *s, u32 val)
{
    if (s->rcnt == 0)
        return;
    if (s->dcfg & 0x01)
    {
        /* 16 bit access */
        ne2000_mem_writew(s, s->rsar, val);
        ne2000_dma_update(s, 2);
    }
    else
    {
        /* 8 bit access */
        ne2000_mem_writeb(s, s->rsar, val);
        ne2000_dma_update(s, 1);
    }
}

static u32 ne2000_asic_read(NE2000State *s)
{
    u32 ret;

    if (s->dcfg & 0x01)
    {
        /* 16 bit access */
        ret = ne2000_mem_readw(s, s->rsar);
        ne2000_dma_update(s, 2);
    }
    else
    {
        /* 8 bit access */
        ret = ne2000_mem_readb(s, s->rsar);
        ne2000_dma_update(s, 1);
    }
    return ret;
}

static void ne2000_asic_writel(NE2000State *s, u32 val)
{
    if (s->rcnt == 0)
        return;
    /* 32 bit access */
    ne2000_mem_writel(s, s->rsar, val);
    ne2000_dma_update(s, 4);
}

static u32 ne2000_asic_readl(NE2000State *s)
{
    u32 ret;

    /* 32 bit access */
    ret = ne2000_mem_readl(s, s->rsar);
    ne2000_dma_update(s, 4);
    return ret;
}

bool Ne2000IoAccess(VH_VM *pVm, u32 uOffset, bool fWrite, u32 cbWidth, u32 *puValue)
{
    NE2000State *s = &s_ne2k;
    static u32 s_cTrace = 0;
    (void)pVm;

    {
        bool fLog = (s_cTrace < 60);
        if (fLog)
        {
            VhLog("ne2k: off=%#04x w=%u %s val=%#x rcnt=%u isr=%02x cmd=%02x",
                  uOffset, cbWidth, fWrite ? "W" : "R", *puValue,
                  s->rcnt, s->isr, s->cmd);
            s_cTrace++;
        }
    }

    if (uOffset < 0x10)
    {
        if (cbWidth == 1)
        {
            if (fWrite)
                ne2000_ioport_write(s, uOffset, *puValue);
            else
                *puValue = ne2000_ioport_read(s, uOffset);
        }
        else
        {
            /* byte-split wide accesses like qemu's port dispatch */
            u32 i;
            u32 uVal = 0;
            for (i = 0; i < cbWidth; i++)
            {
                if (fWrite)
                    ne2000_ioport_write(s, uOffset + i, (*puValue >> (8 * i)) & 0xFF);
                else
                    uVal |= (ne2000_ioport_read(s, uOffset + i) & 0xFF) << (8 * i);
            }
            if (!fWrite)
                *puValue = uVal;
        }
        return true;
    }

    if (uOffset == 0x10)
    {
        if (cbWidth == 4)
        {
            if (fWrite)
                ne2000_asic_writel(s, *puValue);
            else
                *puValue = ne2000_asic_readl(s);
        }
        else
        {
            if (fWrite)
                ne2000_asic_write(s, *puValue);
            else
                *puValue = ne2000_asic_read(s);
        }
        return true;
    }

    if (uOffset == 0x1F)
    {
        /* read triggers reset; write ends the reset pulse */
        if (!fWrite)
        {
            ne2000_reset(s);
            *puValue = 0;
        }
        return true;
    }

    if (uOffset <= 0x1E)
    {
        if (!fWrite)
            *puValue = 0xFFFFFFFFu;
        return true;
    }

    return true;
}
