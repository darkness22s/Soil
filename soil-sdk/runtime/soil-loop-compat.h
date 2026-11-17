/* Linux loop-device UAPI subset for musl toolchains with no linux/loop.h.
 * Struct layout and ioctl numbers follow include/uapi/linux/loop.h.
 * Standard builds use the system-provided header instead.
 */
#ifndef SOIL_LOOP_COMPAT_H
#define SOIL_LOOP_COMPAT_H

#include <stdint.h>

#define LO_NAME_SIZE 64
#define LO_KEY_SIZE 32
#define LO_FLAGS_READ_ONLY 1
#define LO_FLAGS_AUTOCLEAR 4

#define LOOP_SET_FD 0x4C00
#define LOOP_CLR_FD 0x4C01
#define LOOP_SET_STATUS64 0x4C04
#define LOOP_GET_STATUS64 0x4C05
#define LOOP_CTL_GET_FREE 0x4C82

struct loop_info64 {
    uint64_t lo_device;
    uint64_t lo_inode;
    uint64_t lo_rdevice;
    uint64_t lo_offset;
    uint64_t lo_sizelimit;
    uint32_t lo_number;
    uint32_t lo_encrypt_type;
    uint32_t lo_encrypt_key_size;
    uint32_t lo_flags;
    uint8_t lo_file_name[LO_NAME_SIZE];
    uint8_t lo_crypt_name[LO_NAME_SIZE];
    uint8_t lo_encrypt_key[LO_KEY_SIZE];
    uint64_t lo_init[2];
};

#endif
