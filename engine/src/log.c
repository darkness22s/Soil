/*
 * Soil VM engine - logging.
 */
#define WIN32_LEAN_AND_MEAN
#include "vmdefs.h"
#include <stdarg.h>
#include <time.h>

static FILE *g_pLog = NULL;

void VhLogSetFile(FILE *pFile)
{
    g_pLog = pFile;
}

void VhLog(const char *pszFmt, ...)
{
    FILE *pOut = g_pLog ? g_pLog : stderr;
    va_list va;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(pOut, "[%02u:%02u:%02u.%03u] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(va, pszFmt);
    vfprintf(pOut, pszFmt, va);
    va_end(va);
    fputc('\n', pOut);
    /* deliberately no fflush: log lines land in the CRT buffer and are
     * written in 4KB chunks. A per-line flush turns every device-access
     * trace into a syscall on the vCPU thread and dilates guest time. */
}
