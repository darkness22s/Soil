/* Minimal qemu-common.h stub for the vendored slirp. */
#ifndef QEMU_COMMON_H
#define QEMU_COMMON_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#define CONFIG_SLIRP 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#ifndef _SYS_UIO_H
#define _SYS_UIO_H
struct iovec
{
    void  *iov_base;
    size_t iov_len;
};
#endif
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

typedef struct Monitor Monitor;

#ifdef __GNUC__
#define GCC_FMT_ATTR(n, m) __attribute__((format(printf, n, m)))
#else
#define GCC_FMT_ATTR(n, m)
#endif

static inline void *qemu_malloc(size_t cb)
{
    return malloc(cb ? cb : 1);
}

static inline void *qemu_mallocz(size_t cb)
{
    return calloc(1, cb ? cb : 1);
}

static inline void qemu_free(void *pv)
{
    free(pv);
}

static inline char *qemu_strdup(const char *psz)
{
    size_t cb = strlen(psz) + 1;
    char *pszNew = (char *)malloc(cb);
    if (pszNew)
        memcpy(pszNew, psz, cb);
    return pszNew;
}

static inline void pstrcpy(char *pszDst, int cbDst, const char *pszSrc)
{
    char *q = pszDst;
    if (cbDst <= 0)
        return;
    for (;;)
    {
        int c = *pszSrc++;
        if (c == 0 || q >= pszDst + cbDst - 1)
            break;
        *q++ = (char)c;
    }
    *q = '\0';
}

#endif /* QEMU_COMMON_H */
