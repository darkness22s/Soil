#ifndef SOIL_SOL_H
#define SOIL_SOL_H

#include <stddef.h>
#include <stdint.h>

#define SOL_HDR_SIZE 128
#define SOL_MAGIC "S0L1"
#define SOL_FORMAT_VERSION 1

typedef struct {
    uint16_t version;
    uint16_t kind;
    uint32_t header_len;
    uint32_t manifest_len;
    uint64_t payload_offset;
    uint64_t payload_size;
    uint8_t digest[32];
    uint8_t signature[64];
} sol_header;

enum {
    SOL_SIG_NONE = 0,
    SOL_SIG_TRUSTED = 1,
    SOL_SIG_UNTRUSTED = 2,
};

const char *sol_kind_name(int kind);
int sol_kind_from_name(const char *name);

int sol_read_header(const char *path, sol_header *h, char *err, size_t errlen);
int sol_read_manifest(const char *path, const sol_header *h, char **out, size_t *outlen);
int sol_compute_digest(const char *path, const sol_header *h, uint8_t out[32]);
int sol_verify_signature(const char *path, const sol_header *h,
                         char *signer, size_t signer_len);
int sol_is_package_file(const char *path);

#endif
