/*
 * Soil VM engine - interactive host viewer.
 *
 * Shows the guest framebuffer live in a Win32 window and forwards real host
 * mouse and keyboard input into the guest through the same device entry
 * points used by the scripted input channel (VhDevKbdQueueScancodes /
 * VhDevMouseQueuePacket). Closing the window stops the VM.
 */
#define WIN32_LEAN_AND_MEAN
#include "vmdefs.h"
#include <windows.h>
#include <windowsx.h>
#include <process.h>
#include <string.h>
#include <stdlib.h>

#define VH_VIEWER_CLASSNAME  "SoilViewer"
#define VH_VIEWER_REFRESH_MS 16
#define VH_VIEWER_CHUNK      100

typedef struct VH_VIEWER
{
    VH_VM *pVm;
    HWND hWnd;
    POINT ptLast;
    bool fHaveLast;
    bool fCaptured;
    u8 bButtons;            /* PS/2 button bits: 0=left 1=right 2=middle */
    u16 auKey[256];         /* (extended << 8) | scancode, 0 = key up */
    u8 *pbFrame;            /* host backbuffer: atomic frame snapshots */
} VH_VIEWER;

static VH_VIEWER g_Viewer;

static void VhViewerSendMove(VH_VIEWER *pV, int dx, int dy)
{
    while (dx != 0 || dy != 0)
    {
        int cx = dx, cy = dy;
        if (cx > VH_VIEWER_CHUNK) cx = VH_VIEWER_CHUNK;
        else if (cx < -VH_VIEWER_CHUNK) cx = -VH_VIEWER_CHUNK;
        if (cy > VH_VIEWER_CHUNK) cy = VH_VIEWER_CHUNK;
        else if (cy < -VH_VIEWER_CHUNK) cy = -VH_VIEWER_CHUNK;

        u8 ab[3];
        ab[0] = (u8)(0x08 | pV->bButtons
                     | (cx < 0 ? 0x10 : 0)
                     | (cy < 0 ? 0x20 : 0));
        ab[1] = (u8)(signed char)cx;
        ab[2] = (u8)(signed char)cy;
        VhDevMouseQueuePacket(pV->pVm, ab, 3);

        dx -= cx;
        dy -= cy;
    }
}

static void VhViewerSendButtons(VH_VIEWER *pV)
{
    u8 ab[3];
    ab[0] = (u8)(0x08 | pV->bButtons);
    ab[1] = 0;
    ab[2] = 0;
    VhDevMouseQueuePacket(pV->pVm, ab, 3);
}

static void VhViewerButton(VH_VIEWER *pV, u8 bMask, bool fDown)
{
    if (fDown)
    {
        pV->bButtons |= bMask;
        if (!pV->fCaptured)
        {
            SetCapture(pV->hWnd);
            pV->fCaptured = true;
        }
    }
    else
        pV->bButtons &= (u8)~bMask;
    VhViewerSendButtons(pV);
    if (!fDown && pV->bButtons == 0 && pV->fCaptured)
    {
        ReleaseCapture();
        pV->fCaptured = false;
    }
}

static void VhViewerKey(VH_VIEWER *pV, bool fDown, u8 bSc, bool fExt, UINT uVk)
{
    if (!bSc)
        return;
    u8 ab[2];
    u32 cb = 0;
    if (fExt)
        ab[cb++] = 0xE0;
    ab[cb++] = fDown ? bSc : (u8)(bSc | 0x80);
    VhDevKbdQueueScancodes(pV->pVm, ab, cb);
    if (uVk < 256)
        pV->auKey[uVk] = fDown ? (u16)((fExt ? 0x100 : 0) | bSc) : 0;
}

static void VhViewerReleaseAll(VH_VIEWER *pV)
{
    for (UINT uVk = 0; uVk < 256; uVk++)
    {
        u16 uInfo = pV->auKey[uVk];
        if (!uInfo)
            continue;
        u8 ab[2];
        u32 cb = 0;
        if (uInfo & 0x100)
            ab[cb++] = 0xE0;
        ab[cb++] = (u8)(uInfo | 0x80);
        VhDevKbdQueueScancodes(pV->pVm, ab, cb);
        pV->auKey[uVk] = 0;
    }
    if (pV->bButtons)
    {
        pV->bButtons = 0;
        VhViewerSendButtons(pV);
    }
    if (pV->fCaptured)
    {
        ReleaseCapture();
        pV->fCaptured = false;
    }
    pV->fHaveLast = false;
}

static LRESULT CALLBACK VhViewerWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    VH_VIEWER *pV = &g_Viewer;

    switch (uMsg)
    {
        case WM_CLOSE:
            VhLog("viewer: window closed - stopping the VM");
            pV->pVm->fCleanExit = true;
            pV->pVm->fStop = true;
            VhWhvCancelRun(pV->pVm);
            DestroyWindow(hWnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_TIMER:
            if (pV->pbFrame)
                VhFbSnapshot(pV->pVm, pV->pbFrame);
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);
            BITMAPINFO bi;
            memset(&bi, 0, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = VH_FB_W;
            bi.bmiHeader.biHeight = -VH_FB_H;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            const u8 *pbBits = NULL;
            if (pV->pVm->mem.pbRam && pV->pVm->mem.gpaFb)
                pbBits = pV->pbFrame ? pV->pbFrame : pV->pVm->mem.pbRam + pV->pVm->mem.gpaFb;
            if (pbBits)
                StretchDIBits(hdc, 0, 0, rc.right, rc.bottom, 0, 0, VH_FB_W, VH_FB_H,
                              pbBits, &bi, DIB_RGB_COLORS, SRCCOPY);
            else
                FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            if (!pV->fHaveLast)
            {
                pV->ptLast.x = x;
                pV->ptLast.y = y;
                pV->fHaveLast = true;
                return 0;
            }
            int dx = x - pV->ptLast.x;
            int dy = -(y - pV->ptLast.y);      /* PS/2: positive dy = up */
            pV->ptLast.x = x;
            pV->ptLast.y = y;
            if (dx || dy)
                VhViewerSendMove(pV, dx, dy);
            return 0;
        }

        case WM_LBUTTONDOWN: VhViewerButton(pV, 0x01, true);  return 0;
        case WM_LBUTTONUP:   VhViewerButton(pV, 0x01, false); return 0;
        case WM_RBUTTONDOWN: VhViewerButton(pV, 0x02, true);  return 0;
        case WM_RBUTTONUP:   VhViewerButton(pV, 0x02, false); return 0;
        case WM_MBUTTONDOWN: VhViewerButton(pV, 0x04, true);  return 0;
        case WM_MBUTTONUP:   VhViewerButton(pV, 0x04, false); return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            VhViewerKey(pV, true, (u8)((lParam >> 16) & 0xFF),
                        (lParam & (1u << 24)) != 0, (UINT)wParam);
            return 0;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            VhViewerKey(pV, false, (u8)((lParam >> 16) & 0xFF),
                        (lParam & (1u << 24)) != 0, (UINT)wParam);
            return 0;

        case WM_KILLFOCUS:
            VhViewerReleaseAll(pV);
            return 0;

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE)
                pV->fHaveLast = false;
            return 0;
    }
    return DefWindowProcA(hWnd, uMsg, wParam, lParam);
}

static unsigned __stdcall VhViewerThreadProc(void *pvArg)
{
    VH_VIEWER *pV = (VH_VIEWER *)pvArg;
    HINSTANCE hInst = GetModuleHandleA(NULL);

    SetProcessDPIAware();

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = VhViewerWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = VH_VIEWER_CLASSNAME;
    if (!RegisterClassA(&wc))
    {
        VhLog("viewer: RegisterClass failed (%lu)", (unsigned long)GetLastError());
        return 0;
    }

    DWORD dwStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc;
    rc.left = 0;
    rc.top = 0;
    rc.right = VH_FB_W;
    rc.bottom = VH_FB_H;
    AdjustWindowRect(&rc, dwStyle, FALSE);

    pV->hWnd = CreateWindowExA(0, VH_VIEWER_CLASSNAME,
                               "Soil VM - move your mouse in here",
                               dwStyle, CW_USEDEFAULT, CW_USEDEFAULT,
                               rc.right - rc.left, rc.bottom - rc.top,
                               NULL, NULL, hInst, NULL);
    if (!pV->hWnd)
    {
        VhLog("viewer: CreateWindowEx failed (%lu)", (unsigned long)GetLastError());
        return 0;
    }

    pV->pbFrame = (u8 *)malloc(VH_FB_SIZE);
    if (pV->pbFrame)
    {
        memset(pV->pbFrame, 0, VH_FB_SIZE);
        VhFbSnapshot(pV->pVm, pV->pbFrame);
    }

    ShowWindow(pV->hWnd, SW_SHOW);
    UpdateWindow(pV->hWnd);
    SetForegroundWindow(pV->hWnd);
    SetTimer(pV->hWnd, 1, VH_VIEWER_REFRESH_MS, NULL);
    VhLog("viewer: live window created - click it and move the real mouse");

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0)
        DispatchMessageA(&msg);
    return 0;
}

void VhViewerStart(VH_VM *pVm)
{
    memset(&g_Viewer, 0, sizeof(g_Viewer));
    g_Viewer.pVm = pVm;
    _beginthreadex(NULL, 0, VhViewerThreadProc, &g_Viewer, 0, NULL);
}
