/* Windows shims required by the vendored slirp. */
#include "config-host.h"
#include <winsock2.h>
#include <windows.h>
#include "slirp.h"

int inet_aton(const char *cp, struct in_addr *ia)
{
    uint32_t addr = inet_addr(cp);
    if (addr == 0xffffffffu)
        return 0;
    ia->s_addr = addr;
    return 1;
}

int qemu_socket(int domain, int type, int protocol)
{
    return (int)socket(domain, type, protocol);
}
