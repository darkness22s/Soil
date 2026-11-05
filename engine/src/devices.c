/*
 * Soil VM engine - PC device models.
 *
 * Minimal but faithful emulations of the classic PC chipset pieces the guest
 * needs, conceptually ported from VirtualBox's device models:
 *   - 8259 PIC pair          (DevPIC.cpp)
 *   - 8254 PIT               (DevPIT.cpp / Dev8254.cpp)
 *   - MC146818 RTC           (DevRTC.cpp)
 *   - 16550 UART (COM1)      (DevUART.cpp)
 *   - i8042 PS/2 controller  (DevPS2.cpp)
 */
#define WIN32_LEAN_AND_MEAN
#include "vmdefs.h"
#include <math.h>
#include <time.h>

/* The PIC state and the device FIFOs are touched from the timer thread, the
   stdin thread and the main run thread; guard everything with one recursive
   lock (the device model is tiny, contention is irrelevant). */
static CRITICAL_SECTION s_csDev;
static void devLock(void)   { EnterCriticalSection(&s_csDev); }
static void devUnlock(void) { LeaveCriticalSection(&s_csDev); }

/* exported lock (the net poll thread serializes with device I/O) */
void VhDevLock(void)   { devLock(); }
void VhDevUnlock(void) { devUnlock(); }

/* PIT input clock */
#define PIT_HZ 1193182ULL
#define TICK_NS (1000000000ULL / PIT_HZ)   /* ~838.1 ns; use rational math below */

/* ================================================================== */
/* PIC                                                                */
/* ================================================================== */

static int picBestRequest(const VH_PIC_CHIP *pChip)
{
    /* find highest-priority in service (lowest bit index) */
    int iIsr = -1;
    for (int i = 0; i < 8; i++)
        if (pChip->isr & (1u << i)) { iIsr = i; break; }

    for (int i = 0; i < 8; i++)
    {
        if (!(pChip->irr & ~pChip->imr & (1u << i)))
            continue;
        if (iIsr < 0 || i < iIsr)
            return i;
    }
    return -1;
}

static void picRaise(VH_PIC *pPic, u8 bIrq)
{
    if (bIrq < 8)
        pPic->master.irr |= (u8)(1u << bIrq);
    else
    {
        pPic->slave.irr |= (u8)(1u << (bIrq - 8));
        /* the slave only pulls the cascade line when it has a deliverable irq */
        if (pPic->slave.irr & ~pPic->slave.imr)
        {
            pPic->fSlaveInIrq2 = true;
            pPic->master.irr |= 0x04;
        }
    }
}

static void picLower(VH_PIC *pPic, u8 bIrq)
{
    if (bIrq < 8)
        pPic->master.irr &= (u8)~(1u << bIrq);
    else
    {
        pPic->slave.irr &= (u8)~(1u << (bIrq - 8));
        if ((pPic->slave.irr & ~pPic->slave.imr) == 0)
        {
            pPic->fSlaveInIrq2 = false;
            pPic->master.irr &= (u8)~0x04;
        }
    }
}

static void picWriteChip(VH_PIC_CHIP *pChip, bool fCommand, u8 bVal)
{
    if (fCommand)
    {
        if (bVal & 0x10)
        {
            /* ICW1 */
            pChip->icw1 = bVal;
            pChip->initState = 1;
            pChip->imr = 0xFF;      /* not auto-cleared by hardware, but BIOS leaves masked */
            pChip->irr = 0;
            pChip->isr = 0;
            return;
        }
        if (bVal & 0x08)
        {
            /* OCW3 */
            pChip->readIsr = (bVal & 0x01) ? 1 : 0;
            return;
        }
        /* OCW2 - EOI and priority rotation */
        if ((bVal & 0xE0) == 0x20 || (bVal & 0x60) == 0x60)
        {
            if (bVal & 0x40)
            {
                /* specific EOI: bits 2-0 select level */
                u8 bLevel = bVal & 0x07;
                pChip->isr &= (u8)~(1u << bLevel);
            }
            else
            {
                /* non-specific EOI: clear highest in-service */
                for (int i = 0; i < 8; i++)
                    if (pChip->isr & (1u << i)) { pChip->isr &= (u8)~(1u << i); break; }
            }
        }
        return;
    }

    /* Port base+1 */
    switch (pChip->initState)
    {
        case 1: /* ICW2: vector base */
            pChip->vectorBase = bVal & 0xF8;
            /* SNGL=0 -> ICW3 expected; SNGL=1 -> straight to ICW4 decision */
            if (!(pChip->icw1 & 0x02))
                pChip->initState = 2;
            else
                pChip->initState = (pChip->icw1 & 0x01) ? 3 : 0;
            break;
        case 2: /* ICW3 */
            pChip->initState = (pChip->icw1 & 0x01) ? 3 : 0;  /* ICW4 needed? */
            break;
        case 3: /* ICW4 */
            pChip->initState = 0;
            break;
        default:
            pChip->imr = bVal;
            break;
    }
}

/* ================================================================== */
/* PIT                                                                */
/* ================================================================== */

static u64 pitCountsElapsed(VH_PIT_CHAN *pCh, u64 nsNow)
{
    u64 nsEl = nsNow - pCh->startNs;
    /* split math: avoids the u64 overflow of (ns * PIT_HZ) after ~4.3h uptime */
    return (nsEl / 1000000000ULL) * PIT_HZ + ((nsEl % 1000000000ULL) * PIT_HZ) / 1000000000ULL;
}

static u16 pitCurrentValue(VH_PIT_CHAN *pCh, u64 nsNow)
{
    u64 uReload = pCh->reload ? pCh->reload : 65536;
    u64 cElapsed = pitCountsElapsed(pCh, nsNow);

    switch (pCh->mode)
    {
        case 0: /* interrupt on terminal count (one shot) */
        case 1: /* hw retriggerable one shot */
        {
            if (cElapsed >= uReload)
                return 0;
            return (u16)(uReload - cElapsed);
        }
        case 2: /* rate generator */
        case 6:
        case 7:
        {
            u64 r = cElapsed % uReload;
            if (r == 0 && cElapsed > 0)
                return (u16)uReload;    /* briefly reloaded */
            return (u16)(uReload - r);
        }
        case 3: /* square wave */
        case 4:
        case 5:
        {
            /* count halves */
            u64 r = cElapsed % uReload;
            return (u16)(uReload - r);
        }
        default:
            return 0;
    }
}

static void pitChannelWrite(VH_PIT_CHAN *pCh, u8 bVal, u64 nsNow, bool fCmd)
{
    if (fCmd)
    {
        u8 bSc   = (bVal >> 6) & 3;
        u8 bRw   = (bVal >> 4) & 3;
        u8 bMode = (bVal >> 1) & 7;
        (void)bSc;
        if (bRw == 0)
        {
            /* latch command: latch current count */
            pCh->latchValue = pitCurrentValue(pCh, nsNow);
            pCh->fLatched = true;
            pCh->readFlip = 0;
            return;
        }
        pCh->rwMode = bRw;
        pCh->mode = bMode > 5 ? 5 : bMode;   /* modes 6/7 behave like 2/3 */
        if (bMode == 6) pCh->mode = 2;
        if (bMode == 7) pCh->mode = 3;
        pCh->writeFlip = 0;
        return;
    }

    /* data write */
    switch (pCh->rwMode)
    {
        case 1:
            pCh->reload = (u16)(pCh->reload & 0xFF00) | bVal;
            pCh->startNs = nsNow;
            pCh->fNullCount = false;
            break;
        case 2:
            pCh->reload = (u16)((bVal << 8) | (pCh->reload & 0x00FF));
            pCh->startNs = nsNow;
            pCh->fNullCount = false;
            break;
        case 3:
            if (pCh->writeFlip == 0)
            {
                pCh->writeLatch = bVal;
                pCh->writeFlip = 1;
            }
            else
            {
                pCh->reload = (u16)((bVal << 8) | pCh->writeLatch);
                pCh->startNs = nsNow;
                pCh->fNullCount = false;
                pCh->writeFlip = 0;
            }
            break;
        default:
            break;
    }
}

static u8 pitChannelRead(VH_PIT_CHAN *pCh, u64 nsNow)
{
    u16 uValue;
    if (pCh->fLatched)
        uValue = pCh->latchValue;
    else
        uValue = pitCurrentValue(pCh, nsNow);

    switch (pCh->rwMode)
    {
        case 1:
            return (u8)(uValue & 0xFF);
        case 2:
            return (u8)(uValue >> 8);
        default:
        {
            u8 b = (pCh->readFlip == 0) ? (u8)(uValue & 0xFF) : (u8)(uValue >> 8);
            pCh->readFlip ^= 1;
            if (pCh->readFlip == 0)
                pCh->fLatched = false;
            return b;
        }
    }
}

void VhDevPitTick(VH_VM *pVm, u64 nsNow)
{
    devLock();
    VH_PIT_CHAN *pCh = &pVm->pit.ch[0];

    if (!pCh->fNullCount && pCh->reload != 0)
    {
        u64 uReload = pCh->reload;
        u64 cElapsed = pitCountsElapsed(pCh, nsNow);

        if (pCh->mode == 0)
        {
            if (cElapsed >= uReload)
            {
                VhDevRaiseIrq(pVm, 0);
                pCh->fNullCount = true;  /* one-shot finished */
            }
        }
        else if (pCh->mode == 2 || pCh->mode == 3)
        {
            /* raise one tick per period boundary crossed since the last raise;
               only when the previous tick has already been delivered (IRQ0 not
               still pending), so ticks are never dropped and a backlog is
               caught up one wake at a time */
            if (pCh->startNs != pVm->pit.startNsSeen)
            {
                pVm->pit.startNsSeen = pCh->startNs;
                pVm->pit.cPeriods = 0;
            }
            u64 cPeriods = cElapsed / uReload;
            if (cPeriods > pVm->pit.cPeriods && !(pVm->irqs.pic.master.irr & 0x01))
            {
                VhDevRaiseIrq(pVm, 0);
                pVm->pit.cPeriods++;
                pVm->cTicks++;
            }
        }
    }
    devUnlock();
}

/* ================================================================== */
/* RTC                                                                */
/* ================================================================== */

static u8 toBcd(u32 v) { return (u8)(((v / 10) << 4) | (v % 10)); }

static u8 rtcReadReg(VH_VM *pVm, u8 idx)
{
    SYSTEMTIME st;
    GetSystemTime(&st);
    switch (idx)
    {
        case 0x00: return toBcd(st.wSecond);
        case 0x02: return toBcd(st.wMinute);
        case 0x04: return toBcd(st.wHour);
        case 0x06: return toBcd(st.wDayOfWeek + 1);
        case 0x07: return toBcd(st.wDay);
        case 0x08: return toBcd(st.wMonth);
        case 0x09: return toBcd(st.wYear % 100);
        case 0x32: return toBcd(st.wYear / 100);
        case 0x0A: return pVm->rtc.regA;
        case 0x0B: return pVm->rtc.regB;
        case 0x0C: return 0;            /* int flags - read clears */
        case 0x0D: return 0x80;         /* valid RAM */
        default:   return 0x00;
    }
}

/* ================================================================== */
/* UART                                                               */
/* ================================================================== */

static u32 uartRxCount(const VH_UART *pU)
{
    return (pU->iRxHead + VH_UART_FIFO - pU->iRxTail) % VH_UART_FIFO;
}

static void uartUpdateIir(VH_UART *pU)
{
    /* IIR computed on read; see uartRead */
    (void)pU;
}

void VhDevUartGuestOutByte(VH_VM *pVm, u8 b)
{
    VH_UART *pU = &pVm->uart;
    if (pU->pfnOut)
        pU->pfnOut(pU->pvOutUser, b);
}

void VhDevUartWriteGuest(VH_VM *pVm, const void *pv, u32 cb)
{
    devLock();
    VH_UART *pU = &pVm->uart;
    const u8 *pb = (const u8 *)pv;
    for (u32 i = 0; i < cb; i++)
    {
        u32 iNext = (pU->iRxHead + 1) % VH_UART_FIFO;
        if (iNext == pU->iRxTail)
            break;      /* full */
        pU->rxBuf[pU->iRxHead] = pb[i];
        pU->iRxHead = iNext;
    }
    if ((pU->regs[1] & 0x01) && uartRxCount(pU) > 0)
        VhDevRaiseIrq(pVm, VH_UART_IRQ);
    devUnlock();
}

static bool uartRead(VH_VM *pVm, u16 uOffset, u32 *puValue)
{
    VH_UART *pU = &pVm->uart;
    bool fDlab = (pU->regs[3] & 0x80) != 0;

    switch (uOffset)
    {
        case 0:
            if (fDlab) { *puValue = pU->dll; break; }
            if (uartRxCount(pU) > 0)
            {
                *puValue = pU->rxBuf[pU->iRxTail];
                pU->iRxTail = (pU->iRxTail + 1) % VH_UART_FIFO;
                if (uartRxCount(pU) > 0 && (pU->regs[1] & 0x01))
                    VhDevRaiseIrq(pVm, VH_UART_IRQ);
            }
            else
                *puValue = 0;
            break;
        case 1:
            *puValue = fDlab ? pU->dlm : pU->regs[1];
            break;
        case 2:
        {
            u8 bIir = 0x01;
            if (uartRxCount(pU) > 0 && (pU->regs[1] & 0x01))
                bIir = 0x04;                        /* RX data available */
            else if (pU->regs[1] & 0x02)
                bIir = 0x02;                        /* THR empty */
            if (pU->fcr & 0x01)
                bIir |= 0xC0;
            *puValue = bIir;
            break;
        }
        case 3: *puValue = pU->regs[3]; break;
        case 4: *puValue = pU->regs[4]; break;
        case 5:
        {
            u8 bLsr = 0;
            if (uartRxCount(pU) > 0)
                bLsr |= 0x01;                       /* DR */
            bLsr |= 0x60;                           /* THRE | TEMT */
            *puValue = bLsr;
            break;
        }
        case 6: *puValue = 0xB0; break;             /* CTS|DSR|DCD */
        case 7: *puValue = pU->regs[7]; break;
        default: return false;
    }
    return true;
}

static bool uartWrite(VH_VM *pVm, u16 uOffset, u32 uValue)
{
    VH_UART *pU = &pVm->uart;
    u8 b = (u8)uValue;
    bool fDlab = (pU->regs[3] & 0x80) != 0;

    switch (uOffset)
    {
        case 0:
            if (fDlab) { pU->dll = b; break; }
            VhDevUartGuestOutByte(pVm, b);
            if (pU->regs[1] & 0x02)
                VhDevRaiseIrq(pVm, VH_UART_IRQ);
            break;
        case 1:
            if (fDlab) pU->dlm = b;
            else
            {
                pU->regs[1] = b;
                if ((b & 0x01) && uartRxCount(pU) > 0)
                    VhDevRaiseIrq(pVm, VH_UART_IRQ);
                if (b & 0x02)
                    VhDevRaiseIrq(pVm, VH_UART_IRQ);    /* THRI enabled + THRE => immediate IRQ */
            }
            break;
        case 2:
            pU->fcr = b;
            if (b & 0x02) pU->iRxTail = pU->iRxHead;    /* RX FIFO reset */
            break;
        case 3: pU->regs[3] = b; break;
        case 4: pU->regs[4] = b; break;
        case 7: pU->regs[7] = b; break;
        default: break;
    }
    return true;
}

/* ================================================================== */
/* i8042 PS/2                                                         */
/* ================================================================== */

static u32 ps2Count(const VH_PS2_PORT *pP)
{
    return (pP->iHead + VH_PS2_FIFO - pP->iTail) % VH_PS2_FIFO;
}

static void ps2QueueByte(VH_PS2_PORT *pP, u8 b)
{
    u32 iNext = (pP->iHead + 1) % VH_PS2_FIFO;
    if (iNext == pP->iTail)
        return;
    pP->outBuf[pP->iHead] = b;
    pP->iHead = iNext;
}

static void i8042RaiseIfData(VH_VM *pVm)
{
    /* decide which IRQ to raise for the byte sitting in the output buffer */
    if (ps2Count(&pVm->i8042.aux) > 0 && (pVm->i8042.config & 0x02))
        VhDevRaiseIrq(pVm, 12);
    if (ps2Count(&pVm->i8042.kbd) > 0 && (pVm->i8042.config & 0x01))
        VhDevRaiseIrq(pVm, 1);
}

void VhDevKbdQueueScancodes(VH_VM *pVm, const u8 *pb, u32 cb)
{
    devLock();
    for (u32 i = 0; i < cb; i++)
        ps2QueueByte(&pVm->i8042.kbd, pb[i]);
    i8042RaiseIfData(pVm);
    devUnlock();
}

void VhDevMouseQueuePacket(VH_VM *pVm, const u8 *pb, u32 cb)
{
    devLock();
    if (pVm->i8042.fMouseStreaming)
    {
        for (u32 i = 0; i < cb; i++)
            ps2QueueByte(&pVm->i8042.aux, pb[i]);
        i8042RaiseIfData(pVm);
    }
    devUnlock();
}

static void ps2Reply(VH_PS2_PORT *pP, const u8 *pb, u32 cb)
{
    for (u32 i = 0; i < cb; i++)
        ps2QueueByte(pP, pb[i]);
}

static void ps2Command(VH_VM *pVm, VH_PS2_PORT *pP, bool fIsAux, u8 bCmd)
{
    static const u8 s_bAck = 0xFA;
    static const u8 s_bResend = 0xFE;

    if (pP->pendingCmd)
    {
        u8 bPend = pP->pendingCmd;
        pP->pendingCmd = 0;
        switch (bPend)
        {
            case 0xF3: pP->sampleRate = bCmd; ps2Reply(pP, &s_bAck, 1); break;
            case 0xE8: pP->resolution = bCmd; ps2Reply(pP, &s_bAck, 1); break;
            case 0xED: ps2Reply(pP, &s_bAck, 1); break;      /* LEDs */
            case 0xF0:                                       /* select scan code set */
                ps2Reply(pP, &s_bAck, 1);
                break;
            case 0xF2: break;
            default:
                ps2Reply(pP, &s_bAck, 1);
                break;
        }
        i8042RaiseIfData(pVm);
        return;
    }

    switch (bCmd)
    {
        case 0xFF: /* reset */
            if (fIsAux)
            {
                pP->iHead = pP->iTail = 0;
                pP->sampleRate = 100;
                pP->resolution = 2;
                pVm->i8042.fMouseStreaming = false;
            }
            ps2Reply(pP, &s_bAck, 1);
            ps2Reply(pP, (const u8[]){ fIsAux ? 0xAA : 0xAB, fIsAux ? 0x00 : 0x83 }, 2);
            break;
        case 0xF6: ps2Reply(pP, &s_bAck, 1); break;              /* set defaults */
        case 0xF4:                                               /* enable */
            if (fIsAux)
            {
                pP->iHead = pP->iTail = 0;       /* drop stale bytes on stream start */
                pVm->i8042.fMouseStreaming = true;
            }
            ps2Reply(pP, &s_bAck, 1);
            break;
        case 0xF5: ps2Reply(pP, &s_bAck, 1); break;              /* disable */
        case 0xF3: pP->pendingCmd = 0xF3; ps2Reply(pP, &s_bAck, 1); break;
        case 0xE8: pP->pendingCmd = 0xE8; ps2Reply(pP, &s_bAck, 1); break;
        case 0xE6: case 0xE7: ps2Reply(pP, &s_bAck, 1); break;   /* scaling */
        case 0xE9:                                               /* status request */
            ps2Reply(pP, &s_bAck, 1);
            if (fIsAux)
                ps2Reply(pP, (const u8[]){ 0x00, 0x00, 0x00 }, 3);
            else
                ps2Reply(pP, (const u8[]){ 0x02, 0x00, 0x00 }, 3);
            break;
        case 0xF2: /* get id */
            ps2Reply(pP, &s_bAck, 1);
            if (fIsAux)
                ps2Reply(pP, (const u8[]){ 0x00 }, 1);
            else
                ps2Reply(pP, (const u8[]){ 0xAB, 0x83 }, 2);
            break;
        case 0xF0: pP->pendingCmd = 0xF0; ps2Reply(pP, &s_bAck, 1); break;
        case 0xEE:
            ps2Reply(pP, &s_bAck, 1);
            ps2Reply(pP, &bCmd, 1);
            break;
        case 0xFE:
            ps2Reply(pP, &s_bResend, 1);
            break;
        default:
            ps2Reply(pP, &s_bAck, 1);
            break;
    }
    i8042RaiseIfData(pVm);
}

static bool i8042Read(VH_VM *pVm, u16 uPort, u32 *puValue)
{
    if (uPort == 0x60)
    {
        VH_I8042 *pC = &pVm->i8042;
        *puValue = 0xFF;
        if (ps2Count(&pC->kbd) > 0)
        {
            *puValue = pC->kbd.outBuf[pC->kbd.iTail];
            pC->kbd.iTail = (pC->kbd.iTail + 1) % VH_PS2_FIFO;
        }
        else if (ps2Count(&pC->aux) > 0)
        {
            *puValue = pC->aux.outBuf[pC->aux.iTail];
            pC->aux.iTail = (pC->aux.iTail + 1) % VH_PS2_FIFO;
        }
        i8042RaiseIfData(pVm);
        return true;
    }

    /* status */
    {
        VH_I8042 *pC = &pVm->i8042;
        u8 bStatus = 0;
        bool fAuxData = false;
        if (ps2Count(&pC->kbd) > 0)
            ;
        else if (ps2Count(&pC->aux) > 0)
            fAuxData = true;
        if (ps2Count(&pC->kbd) > 0 || ps2Count(&pC->aux) > 0)
            bStatus |= 0x01;                        /* OBF */
        bStatus |= 0x04;                            /* system flag */
        bStatus |= 0x10;                            /* keylock not active */
        if (fAuxData)
            bStatus |= 0x20;                        /* AUX data */
        bStatus |= 0x08;                            /* last write was command (sticky) */
        *puValue = bStatus;
        return true;
    }
}

static bool i8042Write(VH_VM *pVm, u16 uPort, u32 uValue)
{
    VH_I8042 *pC = &pVm->i8042;
    u8 b = (u8)uValue;

    if (uPort == 0x60)
    {
        if (pC->port60Target == 2)
            ps2Command(pVm, &pC->aux, true, b);
        else if (pC->port60Target == 3)
            pC->config = b;                         /* controller config write */
        else if (pC->port60Target == 4)             /* aux loopback (0xD3) */
        {
            ps2QueueByte(&pC->aux, b);
            i8042RaiseIfData(pVm);
        }
        else if (pC->port60Target == 5)             /* kbd loopback (0xD2) */
        {
            ps2QueueByte(&pC->kbd, b);
            i8042RaiseIfData(pVm);
        }
        else                                        /* default: keyboard data */
            ps2Command(pVm, &pC->kbd, false, b);
        pC->port60Target = 0;
        return true;
    }

    /* command port 0x64 */
    switch (b)
    {
        case 0x20:  /* read config */
            ps2QueueByte(&pC->kbd, pC->config);
            i8042RaiseIfData(pVm);
            break;
        case 0x60:  /* write config: next 0x60 write */
            pC->port60Target = 3;
            break;
        case 0xAA:  /* self test */
            ps2QueueByte(&pC->kbd, 0x55);
            i8042RaiseIfData(pVm);
            break;
        case 0xAB:  /* interface test */
            ps2QueueByte(&pC->kbd, 0x00);
            i8042RaiseIfData(pVm);
            break;
        case 0xAD: pC->config |= 0x10; break;       /* disable kbd clock (bit4 set = disabled) */
        case 0xAE: pC->config &= (u8)~0x10; break;  /* enable kbd clock */
        case 0xA7: pC->config |= 0x20; break;       /* disable aux clock */
        case 0xA8: pC->config &= (u8)~0x20; break;  /* enable aux clock */
        case 0xA9: ps2QueueByte(&pC->kbd, 0x00); i8042RaiseIfData(pVm); break;
        case 0xD2: pC->port60Target = 5; break;     /* next write loops back via kbd */
        case 0xD3: pC->port60Target = 4; break;     /* next write loops back via aux */
        case 0xD4: pC->port60Target = 2; break;     /* next write goes to aux */
        case 0xFE:  /* pulse reset line: treat as guest reboot request */
            VhLog("i8042: guest requested reset (0xFE) - stopping VM");
            pVm->fStop = true;
            break;
        default:
            break;
    }
    (void)uartUpdateIir;
    return true;
}

/* ================================================================== */
/* IRQ glue                                                           */
/* ================================================================== */

void VhDevRaiseIrq(VH_VM *pVm, u8 bIrq)
{
    static u32 s_cRaiseLog = 0;
    if (bIrq >= 8 && s_cRaiseLog < 40)
    {
        VhLog("irq: raise %u (m.imr=%#x s.imr=%#x m.irr=%#x s.irr=%#x)",
              bIrq, pVm->irqs.pic.master.imr, pVm->irqs.pic.slave.imr,
              pVm->irqs.pic.master.irr, pVm->irqs.pic.slave.irr);
        s_cRaiseLog++;
    }
    devLock();
    picRaise(&pVm->irqs.pic, bIrq);
    pVm->irqs.fIrqPending = true;
    devUnlock();
}

void VhDevLowerIrq(VH_VM *pVm, u8 bIrq)
{
    devLock();
    picLower(&pVm->irqs.pic, bIrq);
    devUnlock();
}

bool VhDevHasPendingIrq(VH_VM *pVm)
{
    devLock();
    VH_PIC *pPic = &pVm->irqs.pic;
    bool fPending = false;
    int iMaster = picBestRequest(&pPic->master);
    int iSlave  = picBestRequest(&pPic->slave);
    if (iMaster >= 0 && !(iMaster == 2 && iSlave < 0))
        fPending = true;
    else if (iSlave >= 0 && !(pPic->master.imr & 0x04))
        fPending = true;
    devUnlock();
    return fPending;
}

u8 VhDevPicGetVector(VH_VM *pVm)
{
    devLock();
    VH_PIC *pPic = &pVm->irqs.pic;
    u8 bVector = 0;

    int iMaster = picBestRequest(&pPic->master);
    int iSlave  = picBestRequest(&pPic->slave);

    /* slave request wins if it beats the master's best (bit 2 cascade position) */
    bool fUseSlave = false;
    if (iSlave >= 0 && !(pPic->master.imr & 0x04))
    {
        if (iMaster < 0 || iMaster > 2 || (pPic->master.isr & 0x04) == 0)
            fUseSlave = true;   /* deliver slave unless a higher-priority master irq is pending */
        if (iMaster >= 0 && iMaster < 2)
            fUseSlave = false;  /* irq0/irq1 outrank cascade */
    }

    if (fUseSlave)
    {
        pPic->slave.isr |= (u8)(1u << iSlave);
        pPic->slave.irr &= (u8)~(1u << iSlave);
        pPic->master.isr |= 0x04;
        pPic->master.irr &= (u8)~0x04;
        if ((pPic->slave.irr & ~pPic->slave.imr) == 0)
            pPic->fSlaveInIrq2 = false;
        bVector = (u8)(pPic->slave.vectorBase + iSlave);
    }
    else if (iMaster >= 0 && !(iMaster == 2 && iSlave < 0))
    {
        if (iMaster == 2 && iSlave >= 0)
        {
            /* cascade requested but slave had nothing better: deliver slave */
            pPic->slave.isr |= (u8)(1u << iSlave);
            pPic->slave.irr &= (u8)~(1u << iSlave);
            pPic->master.isr |= 0x04;
            pPic->master.irr &= (u8)~0x04;
            bVector = (u8)(pPic->slave.vectorBase + iSlave);
        }
        else
        {
            pPic->master.isr |= (u8)(1u << iMaster);
            pPic->master.irr &= (u8)~(1u << iMaster);
            bVector = (u8)(pPic->master.vectorBase + iMaster);
        }
    }

    devUnlock();
    return bVector;
}

void VhDevRegisterInjection(VH_VM *pVm, u8 bVector)
{
    (void)pVm; (void)bVector;
}

void VhDevPicRequeueVector(VH_VM *pVm, u8 bVector)
{
    devLock();
    VH_PIC *pPic = &pVm->irqs.pic;
    u8 bSlaveBase = pPic->slave.vectorBase;
    u8 bMasterBase = pPic->master.vectorBase;

    if (bVector >= bSlaveBase && bVector < (u8)(bSlaveBase + 8))
    {
        u8 bBit = (u8)(1u << (bVector - bSlaveBase));
        pPic->slave.isr &= (u8)~bBit;
        pPic->slave.irr |= bBit;
        pPic->master.isr &= (u8)~0x04;
        pPic->master.irr |= 0x04;
        pPic->fSlaveInIrq2 = true;
    }
    else if (bVector >= bMasterBase && bVector < (u8)(bMasterBase + 8))
    {
        u8 bBit = (u8)(1u << (bVector - bMasterBase));
        pPic->master.isr &= (u8)~bBit;
        pPic->master.irr |= bBit;
    }
    devUnlock();
}

void VhDevPicAckVector(VH_VM *pVm, u8 bVector)
{
    devLock();
    VH_PIC *pPic = &pVm->irqs.pic;
    u8 bSlaveBase = pPic->slave.vectorBase;
    u8 bMasterBase = pPic->master.vectorBase;

    if (bVector >= bSlaveBase && bVector < (u8)(bSlaveBase + 8))
    {
        u8 bBit = (u8)(1u << (bVector - bSlaveBase));
        pPic->slave.irr &= (u8)~bBit;
        pPic->slave.isr |= bBit;
        pPic->master.irr &= (u8)~0x04;
        pPic->master.isr |= 0x04;
    }
    else if (bVector >= bMasterBase && bVector < (u8)(bMasterBase + 8))
    {
        u8 bBit = (u8)(1u << (bVector - bMasterBase));
        pPic->master.irr &= (u8)~bBit;
        pPic->master.isr |= bBit;
    }
    devUnlock();
}

/* ================================================================== */
/* Port dispatch                                                      */
/* ================================================================== */

static bool VhDevIoPortLocked(VH_VM *pVm, u16 uPort, bool fWrite, u32 cbWidth, u32 *puValue);

bool VhDevIoPort(VH_VM *pVm, u16 uPort, bool fWrite, u32 cbWidth, u32 *puValue)
{
    devLock();
    bool fRet = VhDevIoPortLocked(pVm, uPort, fWrite, cbWidth, puValue);
    devUnlock();
    return fRet;
}

static bool VhDevIoPortLocked(VH_VM *pVm, u16 uPort, bool fWrite, u32 cbWidth, u32 *puValue)
{
    static u32 s_cPicM = 0;
    static u32 s_cPicS = 0;
    static u32 s_cCasc = 0;
    u64 nsNow = VhGuestNowNs();

    /* 8259 PIC */
    if (uPort == 0x20 || uPort == 0x21)
    {
        if (fWrite)
        {
            u8 bOld = pVm->irqs.pic.master.imr;
            picWriteChip(&pVm->irqs.pic.master, uPort == 0x20, (u8)*puValue);
            if (pVm->irqs.pic.master.imr != bOld && (bOld ^ pVm->irqs.pic.master.imr) != 0x01 && s_cPicM < 50)
            {
                VhLog("pic: master imr %#x -> %#x", bOld, pVm->irqs.pic.master.imr);
                s_cPicM++;
            }
        }
        else if (uPort == 0x20) *puValue = pVm->irqs.pic.master.readIsr ? pVm->irqs.pic.master.isr : pVm->irqs.pic.master.irr;
        else *puValue = pVm->irqs.pic.master.imr;
        return true;
    }
    if (uPort == 0xA0 || uPort == 0xA1)
    {
        if (fWrite)
        {
            u8 bOld = pVm->irqs.pic.slave.imr;
            picWriteChip(&pVm->irqs.pic.slave, uPort == 0xA0, (u8)*puValue);
            if (pVm->irqs.pic.slave.imr != bOld && (bOld ^ pVm->irqs.pic.slave.imr) != 0x10 && s_cPicS < 50)
            {
                VhLog("pic: slave imr %#x -> %#x (m.irr=%#x s.irr=%#x)",
                      bOld, pVm->irqs.pic.slave.imr,
                      pVm->irqs.pic.master.irr, pVm->irqs.pic.slave.irr);
                s_cPicS++;
            }
            /* re-evaluate cascade: slave pulls INT2 only while a pending irq is unmasked */
            if (pVm->irqs.pic.slave.irr & ~pVm->irqs.pic.slave.imr)
            {
                if (!(pVm->irqs.pic.master.irr & 0x04) && s_cCasc < 40)
                {
                    VhLog("pic: cascade asserted (s.irr=%#x s.imr=%#x)",
                          pVm->irqs.pic.slave.irr, pVm->irqs.pic.slave.imr);
                    s_cCasc++;
                }
                pVm->irqs.pic.fSlaveInIrq2 = true;
                pVm->irqs.pic.master.irr |= 0x04;
            }
            else if (!(pVm->irqs.pic.master.isr & 0x04))
            {
                pVm->irqs.pic.fSlaveInIrq2 = false;
                pVm->irqs.pic.master.irr &= (u8)~0x04;
            }
        }
        else if (uPort == 0xA0) *puValue = pVm->irqs.pic.slave.readIsr ? pVm->irqs.pic.slave.isr : pVm->irqs.pic.slave.irr;
        else *puValue = pVm->irqs.pic.slave.imr;
        return true;
    }

    /* 8254 PIT */
    if (uPort >= 0x40 && uPort <= 0x43)
    {
        if (fWrite)
        {
            if (uPort == 0x43)
            {
                u8 bSc = (u8)((*puValue >> 6) & 3);
                if (bSc == 3)
                {
                    /* read-back: latch selected channels (count only) */
                    if (*puValue & 0x20) { pVm->pit.ch[0].latchValue = pitCurrentValue(&pVm->pit.ch[0], nsNow); pVm->pit.ch[0].fLatched = true; pVm->pit.ch[0].readFlip = 0; }
                    if (*puValue & 0x10) { pVm->pit.ch[1].latchValue = pitCurrentValue(&pVm->pit.ch[1], nsNow); pVm->pit.ch[1].fLatched = true; pVm->pit.ch[1].readFlip = 0; }
                    if (*puValue & 0x08) { pVm->pit.ch[2].latchValue = pitCurrentValue(&pVm->pit.ch[2], nsNow); pVm->pit.ch[2].fLatched = true; pVm->pit.ch[2].readFlip = 0; }
                    if (*puValue & 0x02) { pVm->pit.ch[0].latchValue = pitCurrentValue(&pVm->pit.ch[0], nsNow); pVm->pit.ch[0].fLatched = true; pVm->pit.ch[0].readFlip = 0; }
                    if (*puValue & 0x01) { pVm->pit.ch[0].latchValue = pitCurrentValue(&pVm->pit.ch[0], nsNow); pVm->pit.ch[0].fLatched = true; pVm->pit.ch[0].readFlip = 0; }
                }
                else
                    pitChannelWrite(&pVm->pit.ch[bSc], (u8)*puValue, nsNow, true);
            }
            else
                pitChannelWrite(&pVm->pit.ch[uPort - 0x40], (u8)*puValue, nsNow, false);
        }
        else
        {
            if (uPort == 0x43) *puValue = 0xFF;
            else *puValue = pitChannelRead(&pVm->pit.ch[uPort - 0x40], nsNow);
        }
        return true;
    }

    /* port 0x61 speaker/gate */
    if (uPort == 0x61)
    {
        if (fWrite) pVm->pit.speakerPort = (u8)(*puValue & 0x03);
        else *puValue = (u8)(pVm->pit.speakerPort | 0x0C);
        return true;
    }

    /* RTC */
    if (uPort == 0x70)
    {
        if (fWrite) pVm->rtc.index = (u8)(*puValue & 0x7F);
        else *puValue = 0xFF;
        return true;
    }
    if (uPort == 0x71)
    {
        if (fWrite)
        {
            if (pVm->rtc.index == 0x0A) pVm->rtc.regA = (u8)*puValue;
            else if (pVm->rtc.index == 0x0B) pVm->rtc.regB = (u8)*puValue;
        }
        else
            *puValue = rtcReadReg(pVm, pVm->rtc.index);
        return true;
    }

    /* UART COM1 */
    if (uPort >= 0x3F8 && uPort <= 0x3FF)
        return fWrite ? uartWrite(pVm, uPort - 0x3F8, *puValue)
                      : uartRead(pVm, uPort - 0x3F8, puValue);

    /* i8042 */
    if (uPort == 0x60 || uPort == 0x64)
        return fWrite ? i8042Write(pVm, uPort, *puValue) : i8042Read(pVm, uPort, puValue);

    /* System control port A (A20 fast gate) - swallow */
    if (uPort == 0x92)
    {
        if (!fWrite) *puValue = 0x02;
        return true;
    }

    /* PCI config space (CF8/CFC) + reset port CF9 */
    if ((uPort >= 0xCF8 && uPort <= 0xCFF) || uPort == 0xCF9)
        return VhPciIo(pVm, uPort, fWrite, cbWidth, puValue);

    /* PCI RTL8029 NIC I/O window (BAR0) */
    {
        u16 uNicBase = VhPciNicIoBase();
        if (uNicBase && uPort >= uNicBase && uPort < (u16)(uNicBase + 0x20))
            return Ne2000IoAccess(pVm, (u32)(uPort - uNicBase), fWrite, cbWidth, puValue);
    }

    /* PCI virtio-blk I/O window (legacy BAR0) */
    {
        u16 uBlkBase = VhPciBlkIoBase();
        if (uBlkBase && uPort >= uBlkBase && uPort < (u16)(uBlkBase + 0x40))
            return VhVirtioBlkIo(pVm, (u32)(uPort - uBlkBase), fWrite, cbWidth, puValue);
    }

    /* PCI writable data-disk virtio-blk I/O window */
    {
        u16 uBlkBase = VhPciBlkDataIoBase();
        if (uBlkBase && uPort >= uBlkBase && uPort < (u16)(uBlkBase + 0x40))
            return VhVirtioBlkDataIo(pVm, (u32)(uPort - uBlkBase), fWrite, cbWidth, puValue);
    }

    /* POST/delay ports */
    if (uPort == 0x80 || uPort == 0xED)
    {
        if (!fWrite) *puValue = 0;
        return true;
    }

    /* VGA/MDA CRT controller + status (vgacon pokes these; swallow quietly) */
    if (uPort == 0x3D4 || uPort == 0x3B4)
    {
        if (fWrite) pVm->uVgaCrtIndex = (u8)*puValue;
        else *puValue = pVm->uVgaCrtIndex;
        return true;
    }
    if (uPort == 0x3D5 || uPort == 0x3B5)
    {
        if (!fWrite) *puValue = 0;
        return true;
    }
    if (uPort == 0x3DA || uPort == 0x3BA)
    {
        if (!fWrite) *puValue = 0x09;   /* display enabled + vertical retrace */
        return true;
    }
    if (uPort == 0x3C0 || uPort == 0x3C1)
    {
        if (!fWrite) *puValue = 0xFF;
        return true;
    }

    return false;
}

/* ================================================================== */
/* Init                                                               */
/* ================================================================== */

static void uartOutThunk(void *pvUser, u8 b)
{
    VH_VM *pVm = (VH_VM *)pvUser;
    /* buffered on purpose: a per-byte fflush makes every serial character a
     * host syscall and throttles the vCPU to ~20% during verbose boot phases */
    fwrite(&b, 1, 1, stdout);
    (void)pVm;
}

void VhDevInit(VH_VM *pVm)
{
    InitializeCriticalSection(&s_csDev);
    VhDevResetState(pVm);
}

void VhDevResetState(VH_VM *pVm)
{
    memset(&pVm->irqs, 0, sizeof(pVm->irqs));
    memset(&pVm->pit, 0, sizeof(pVm->pit));
    memset(&pVm->rtc, 0, sizeof(pVm->rtc));
    memset(&pVm->uart, 0, sizeof(pVm->uart));
    memset(&pVm->i8042, 0, sizeof(pVm->i8042));

    /* PIC: BIOS-ish state - all masked, vector bases 0x08/0x70 until reprogrammed */
    pVm->irqs.pic.master.imr = 0xFF;
    pVm->irqs.pic.slave.imr = 0xFF;
    pVm->irqs.pic.master.vectorBase = 0x08;
    pVm->irqs.pic.slave.vectorBase = 0x70;

    /* PIT: channel 0 mode 3, reload 0xFFFF */
    pVm->pit.ch[0].mode = 3;
    pVm->pit.ch[0].rwMode = 3;
    pVm->pit.ch[0].reload = 0xFFFF;
    pVm->pit.ch[0].startNs = VhGuestNowNs();
    pVm->pit.ch[1].mode = 3; pVm->pit.ch[1].rwMode = 3; pVm->pit.ch[1].reload = 0xFFFF;
    pVm->pit.ch[2].mode = 3; pVm->pit.ch[2].rwMode = 3; pVm->pit.ch[2].reload = 0xFFFF;
    pVm->pit.lastTickNs = VhGuestNowNs();

    /* RTC */
    pVm->rtc.regA = 0x26;
    pVm->rtc.regB = 0x02;

    /* UART */
    pVm->uart.regs[5] = 0x60;
    pVm->uart.regs[6] = 0xB0;
    pVm->uart.pfnOut = uartOutThunk;
    pVm->uart.pvOutUser = pVm;

    /* i8042: translation on, both interrupts enabled; enabled ports */
    pVm->i8042.config = 0x45;   /* bit0 kbd int, bit2 sys, bit6 translation */
    pVm->i8042.kbd.fEnabled = 1;
    pVm->i8042.aux.fEnabled = 1;
    pVm->i8042.statusFlags = 0x04;
}

void VhDevPostRestoreFixup(VH_VM *pVm)
{
    pVm->uart.pfnOut = uartOutThunk;
    pVm->uart.pvOutUser = pVm;
}
