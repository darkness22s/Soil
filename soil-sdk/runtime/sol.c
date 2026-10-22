#define _GNU_SOURCE
#include "sol.h"
#include "sha256.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef OPENSSL_BIN
#define OPENSSL_BIN "/usr/local/bin/openssl"
#endif

const char *sol_kind_name(int kind)
{
    switch (kind) {
    case 1: return "webapp";
    case 2: return "installer";
    case 3: return "webapp+native";
    default: return "unknown";
    }
}

int sol_kind_from_name(const char *name)
{
    if (!name) return 0;
    if (!strcmp(name, "webapp")) return 1;
    if (!strcmp(name, "installer")) return 2;
    if (!strcmp(name, "webapp+native")) return 3;
    return 0;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

int sol_read_header(const char *path, sol_header *h, char *err, size_t errlen)
{
    memset(h, 0, sizeof(*h));
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        snprintf(err, errlen, "stat failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    uint8_t buf[SOL_HDR_SIZE];
    ssize_t got = read(fd, buf, sizeof(buf));
    close(fd);
    if (got != SOL_HDR_SIZE) {
        snprintf(err, errlen, "not a soil package (short file)");
        return -1;
    }
    if (memcmp(buf, SOL_MAGIC, 4) != 0) {
        snprintf(err, errlen, "not a soil package (bad magic)");
        return -1;
    }
    h->version = rd16(buf + 4);
    h->kind = rd16(buf + 6);
    h->header_len = rd32(buf + 8);
    h->manifest_len = rd32(buf + 12);
    h->payload_offset = rd64(buf + 16);
    h->payload_size = rd64(buf + 24);
    memcpy(h->digest, buf + 32, 32);
    memcpy(h->signature, buf + 64, 64);
    if (h->version != SOL_FORMAT_VERSION) {
        snprintf(err, errlen, "unsupported format version %u", h->version);
        return -1;
    }
    if (h->kind < 1 || h->kind > 3) {
        snprintf(err, errlen, "unknown package kind %u", h->kind);
        return -1;
    }
    if (h->header_len != SOL_HDR_SIZE + h->manifest_len ||
        h->payload_offset != h->header_len) {
        snprintf(err, errlen, "corrupt header (length fields)");
        return -1;
    }
    if (h->manifest_len == 0 || h->manifest_len > 1024 * 1024) {
        snprintf(err, errlen, "manifest length invalid");
        return -1;
    }
    if (h->payload_offset % 512 != 0) {
        snprintf(err, errlen, "payload offset not sector-aligned");
        return -1;
    }
    if (h->payload_offset + h->payload_size != (uint64_t)st.st_size) {
        snprintf(err, errlen, "size mismatch (header %llu bytes, file %llu)",
                 (unsigned long long)(h->payload_offset + h->payload_size),
                 (unsigned long long)st.st_size);
        return -1;
    }
    return 0;
}

int sol_read_manifest(const char *path, const sol_header *h, char **out, size_t *outlen)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, SOL_HDR_SIZE, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    char *buf = malloc(h->manifest_len + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t got = fread(buf, 1, h->manifest_len, f);
    fclose(f);
    if (got != h->manifest_len) {
        free(buf);
        return -1;
    }
    buf[got] = 0;
    *out = buf;
    if (outlen)
        *outlen = got;
    return 0;
}

int sol_compute_digest(const char *path, const sol_header *h, uint8_t out[32])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    sha256_ctx c;
    sha256_init(&c);
    if (fseek(f, SOL_HDR_SIZE, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    uint64_t remaining = (uint64_t)h->manifest_len + h->payload_size;
    static uint8_t buf[1 << 16];
    while (remaining) {
        size_t want = remaining > sizeof(buf) ? sizeof(buf) : (size_t)remaining;
        size_t got = fread(buf, 1, want, f);
        if (!got) {
            fclose(f);
            return -1;
        }
        sha256_update(&c, buf, got);
        remaining -= got;
    }
    fclose(f);
    sha256_final(&c, out);
    return 0;
}

static int try_openssl(const char *pem, const char *digpath, const char *sigpath)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        execl(OPENSSL_BIN, "openssl", "pkeyutl", "-verify", "-pubin", "-inkey", pem,
              "-rawin", "-in", digpath, "-sigfile", sigpath, (char *)NULL);
        execlp("openssl", "openssl", "pkeyutl", "-verify", "-pubin", "-inkey", pem,
               "-rawin", "-in", digpath, "-sigfile", sigpath, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
}

int sol_verify_signature(const char *path, const sol_header *h,
                         char *signer, size_t signer_len)
{
    static const uint8_t zero[64] = { 0 };
    if (signer && signer_len) signer[0] = 0;
    if (!memcmp(h->signature, zero, 64))
        return SOL_SIG_NONE;

    uint8_t digest[32];
    if (sol_compute_digest(path, h, digest) != 0)
        return SOL_SIG_UNTRUSTED;

    char digpath[] = "/tmp/soil-digest-XXXXXX";
    char sigpath[] = "/tmp/soil-sig-XXXXXX";
    int dfd = mkstemp(digpath);
    int sfd = mkstemp(sigpath);
    if (dfd < 0 || sfd < 0) {
        if (dfd >= 0) { close(dfd); unlink(digpath); }
        if (sfd >= 0) { close(sfd); unlink(sigpath); }
        return SOL_SIG_UNTRUSTED;
    }
    ssize_t wr = write(dfd, digest, 32);
    close(dfd);
    ssize_t wr2 = write(sfd, h->signature, 64);
    close(sfd);
    if (wr != 32 || wr2 != 64) {
        unlink(digpath);
        unlink(sigpath);
        return SOL_SIG_UNTRUSTED;
    }

    const char *home = getenv("HOME");
    char userdir[512];
    userdir[0] = 0;
    if (home && *home)
        snprintf(userdir, sizeof(userdir), "%s/.config/soil/trust", home);
    const char *dirs[2] = { "/usr/local/share/soil/trust", userdir };

    int result = SOL_SIG_UNTRUSTED;
    for (int d = 0; d < 2 && result != SOL_SIG_TRUSTED; d++) {
        if (!dirs[d] || !*dirs[d])
            continue;
        DIR *dp = opendir(dirs[d]);
        if (!dp)
            continue;
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            size_t n = strlen(de->d_name);
            if (n < 5 || strcmp(de->d_name + n - 4, ".pem") != 0)
                continue;
            char pem[1024];
            snprintf(pem, sizeof(pem), "%s/%s", dirs[d], de->d_name);
            if (try_openssl(pem, digpath, sigpath) == 0) {
                result = SOL_SIG_TRUSTED;
                if (signer && signer_len)
                    snprintf(signer, signer_len, "%s", de->d_name);
                break;
            }
        }
        closedir(dp);
    }
    unlink(digpath);
    unlink(sigpath);
    return result;
}

int sol_is_package_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    char magic[4];
    ssize_t n = read(fd, magic, 4);
    close(fd);
    return n == 4 && !memcmp(magic, SOL_MAGIC, 4);
}
