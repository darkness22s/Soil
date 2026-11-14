/*
 * Soil VM engine - host IPC transport for the Soil app.
 *
 * Two channels, both keyed by a caller-supplied token:
 *   Local\SoilFb_<token>     shared framebuffer (header + 1360x768 BGRA)
 *   \\.\pipe\SoilInput_<token> input messages (fixed 16-byte records)
 *
 * The app runs vmhost with --no-ui --ipc <token>; this module then owns the
 * frame pump and the input reader instead of viewer.c.
 */
#define WIN32_LEAN_AND_MEAN
#include "vmdefs.h"
#include <process.h>
#include <string.h>
#include <stdio.h>

typedef struct VH_IPC_INPUT
{
    u32 uType;              /* 1=move, 2=buttons, 3=key, 4=release */
    i32 a;
    i32 b;
    i32 c;
} VH_IPC_INPUT;

static VH_IPC_SHARED *g_pSh;
static VH_VM *g_pVm;

static void VhIpcSendMove(VH_VM *pVm, int dx, int dy, u8 bButtons)
{
    while (dx != 0 || dy != 0)
    {
        int cx = dx, cy = dy;
        if (cx > 100) cx = 100;
        else if (cx < -100) cx = -100;
        if (cy > 100) cy = 100;
        else if (cy < -100) cy = -100;

        u8 ab[3];
        ab[0] = (u8)(0x08 | bButtons
                     | (cx < 0 ? 0x10 : 0)
                     | (cy < 0 ? 0x20 : 0));
        ab[1] = (u8)(signed char)cx;
        ab[2] = (u8)(signed char)cy;
        VhDevMouseQueuePacket(pVm, ab, 3);
        dx -= cx;
        dy -= cy;
    }
}

static void VhIpcInputRecord(VH_VM *pVm, const VH_IPC_INPUT *pIn)
{
    switch (pIn->uType)
    {
        case 1:     /* pointer move, relative guest pixels */
            VhIpcSendMove(pVm, pIn->a, pIn->b, (u8)pIn->c);
            break;

        case 2:     /* buttons changed */
        {
            u8 ab[3];
            ab[0] = (u8)(0x08 | (u8)pIn->c);
            ab[1] = 0;
            ab[2] = 0;
            VhDevMouseQueuePacket(pVm, ab, 3);
            break;
        }

        case 3:     /* key: a = (extended << 8) | scancode, b = down */
        {
            u8 ab[2];
            u32 cb = 0;
            if (pIn->a & 0x100)
                ab[cb++] = 0xE0;
            ab[cb++] = (u8)(pIn->b ? (pIn->a & 0xFF) : ((pIn->a & 0xFF) | 0x80));
            VhDevKbdQueueScancodes(pVm, ab, cb);
            break;
        }

        case 4:     /* release everything */
        {
            u8 ab[3];
            ab[0] = 0x08;
            ab[1] = 0;
            ab[2] = 0;
            VhDevMouseQueuePacket(pVm, ab, 3);
            break;
        }
    }
}

static unsigned __stdcall VhIpcInputThread(void *pvArg)
{
    const char *pszToken = (const char *)pvArg;
    VH_VM *pVm = g_pVm;
    char szPipe[160];
    sprintf(szPipe, "\\\\.\\pipe\\SoilInput_%s", pszToken);

    for (;;)
    {
        HANDLE hPipe = CreateNamedPipeA(
            szPipe,
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 0, 64 * 1024, 0, NULL);
        if (hPipe == INVALID_HANDLE_VALUE)
        {
            VhLog("ipc: CreateNamedPipe failed (%lu)", (unsigned long)GetLastError());
            return 0;
        }

        if (!ConnectNamedPipe(hPipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)
        {
            VhLog("ipc: ConnectNamedPipe failed (%lu)", (unsigned long)GetLastError());
            CloseHandle(hPipe);
            return 0;
        }

        VhLog("ipc: input client connected");
        u8 buf[sizeof(VH_IPC_INPUT) * 4];
        u32 cbHave = 0;
        for (;;)
        {
            DWORD cbRead = 0;
            if (!ReadFile(hPipe, buf + cbHave, (DWORD)(sizeof(buf) - cbHave), &cbRead, NULL) || cbRead == 0)
                break;
            cbHave += cbRead;
            while (cbHave >= sizeof(VH_IPC_INPUT))
            {
                VH_IPC_INPUT in;
                memcpy(&in, buf, sizeof(in));
                memmove(buf, buf + sizeof(in), cbHave - sizeof(in));
                cbHave -= sizeof(in);
                VhIpcInputRecord(pVm, &in);
            }
        }
        CloseHandle(hPipe);
        if (g_pVm->fStop)
            break;

        VhLog("ipc: input client disconnected - stopping VM");
        g_pVm->fStop = true;
        VhWhvCancelRun(g_pVm);
        break;
    }
    return 0;
}

static unsigned __stdcall VhIpcFrameThread(void *pvArg)
{
    VH_VM *pVm = (VH_VM *)pvArg;
    int snapPendingAck = 0;
    u64 snapWaitStart = 0;

    while (!pVm->fStop)
    {
        if (g_pSh && g_pSh->uStop)
        {
            VhLog("ipc: host requested stop");
            pVm->fStop = true;
            VhWhvCancelRun(pVm);
            break;
        }

        if (g_pSh && !snapPendingAck &&
            InterlockedCompareExchange(&g_pSh->uCmd, 0, 1) == 1)
        {
            LONG cbPath = InterlockedCompareExchange(&g_pSh->cCmdHave, 0, 0);
            if (cbPath > 0 && cbPath < (LONG)sizeof(pVm->szSnapPath) &&
                g_pSh->szCmd[cbPath] == '\0')
            {
                memcpy(pVm->szSnapPath, g_pSh->szCmd, (size_t)cbPath + 1);
                pVm->fSnapWanted = true;
                snapPendingAck = 1;
                snapWaitStart = VhGuestNowNs();
            }
            else
                InterlockedExchange(&g_pSh->uCmd, 0);
        }
        if (g_pSh && snapPendingAck && !pVm->fSnapWanted && !pVm->fSnapWorking)
        {
            InterlockedExchange(&g_pSh->uCmd, 2);
            snapPendingAck = 0;
        }
        (void)snapWaitStart;

        if (g_pSh && pVm->mem.pbRam && pVm->mem.gpaFb)
        {
            VhFbSnapshot(pVm, g_pSh->abFrame);
            InterlockedIncrement((volatile LONG *)&g_pSh->uSeq);
        }
        Sleep(66);
    }
    return 0;
}

void VhIpcStart(VH_VM *pVm, const char *pszToken)
{
    char szMap[160];
    sprintf(szMap, "Local\\SoilFb_%s", pszToken);

    g_pVm = pVm;
    HANDLE hMap = CreateFileMappingA(
        INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
        (DWORD)sizeof(VH_IPC_SHARED), szMap);
    if (!hMap)
    {
        VhLog("ipc: CreateFileMapping failed (%lu)", (unsigned long)GetLastError());
        return;
    }

    g_pSh = (VH_IPC_SHARED *)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VH_IPC_SHARED));
    if (!g_pSh)
    {
        VhLog("ipc: MapViewOfFile failed (%lu)", (unsigned long)GetLastError());
        CloseHandle(hMap);
        return;
    }

    memset(g_pSh, 0, sizeof(*g_pSh));
    g_pSh->uWidth = VH_FB_W;
    g_pSh->uHeight = VH_FB_H;
    g_pSh->uStride = VH_FB_STRIDE;

    VhLog("ipc: shared framebuffer ready (%s)", szMap);

    /* token must outlive the threads */
    char *pszCopy = (char *)malloc(strlen(pszToken) + 1);
    if (pszCopy)
        strcpy(pszCopy, pszToken);

    _beginthreadex(NULL, 0, VhIpcFrameThread, pVm, 0, NULL);
    _beginthreadex(NULL, 0, VhIpcInputThread, pszCopy ? pszCopy : (char *)pszToken, 0, NULL);
}
