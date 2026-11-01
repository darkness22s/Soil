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
    char szLine[8192];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int cchPrefix = snprintf(szLine, sizeof(szLine), "[%02u:%02u:%02u.%03u] ",
                             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (cchPrefix < 0 || (size_t)cchPrefix >= sizeof(szLine) - 2)
        return;

    va_list va;
    va_start(va, pszFmt);
    int cchBody = vsnprintf(szLine + cchPrefix, sizeof(szLine) - (size_t)cchPrefix - 2,
                            pszFmt, va);
    va_end(va);
    if (cchBody < 0)
        return;

    size_t cbBody = (size_t)cchBody;
    size_t cbMaxBody = sizeof(szLine) - (size_t)cchPrefix - 2;
    if (cbBody > cbMaxBody)
        cbBody = cbMaxBody;
    size_t cbLine = (size_t)cchPrefix + cbBody;
    szLine[cbLine++] = '\n';
    (void)fwrite(szLine, 1, cbLine, pOut);
}
