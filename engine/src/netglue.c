/*
 * Soil VM engine - slirp <-> NE2000 glue.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include "vmdefs.h"
#include "slirp/libslirp.h"

static VH_VM *s_pVmNet;
static Slirp *s_pSlirp;
static int s_sshPort = 2222;
static int s_mcpPort = 2223;

void VhNetSetHostFwdPorts(int sshPort, int mcpPort)
{
    s_sshPort = sshPort;
    s_mcpPort = mcpPort;
}

/* provided for slirp: */
int slirp_can_output(void *opaque)
{
    (void)opaque;
    return Ne2000CanReceive();
}

void slirp_output(void *opaque, const uint8_t *pkt, int pkt_len)
{
    (void)opaque;
    Ne2000Receive(pkt, (u32)pkt_len);
}

void VhNetInit(VH_VM *pVm, bool fHostForward)
{
    struct in_addr aNet, aMask, aHost, aDhcp, aDns;

    VhPciInit();
    Ne2000Init(pVm);
    s_pVmNet = pVm;

    aNet.s_addr  = htonl(0x0A000200);   /* 10.0.2.0     */
    aMask.s_addr = htonl(0xFFFFFF00);   /* 255.255.255.0 */
    aHost.s_addr = htonl(0x0A000202);   /* 10.0.2.2 (gateway + host) */
    aDhcp.s_addr = htonl(0x0A00020F);   /* 10.0.2.15 (guest, for bootp only) */
    aDns.s_addr  = htonl(0x0A000203);   /* 10.0.2.3 (slirp DNS) */

    s_pSlirp = slirp_init(0, aNet, aMask, aHost,
                          NULL, NULL, NULL, aDhcp, aDns, NULL);
    VhLog("net: init slirp=%p", (void *)s_pSlirp);

    if (s_pSlirp && fHostForward) {
        /* guest sshd for the MCP-over-ssh entry point; host-side loopback
           only, so the guest is not reachable from the network */
        struct in_addr aLo, aGuest;
        int rc;
        aLo.s_addr = htonl(0x7F000001);     /* 127.0.0.1 (host loopback) */
        aGuest.s_addr = htonl(0x0A00020F);  /* 10.0.2.15 */
        if (s_sshPort > 0 && s_sshPort < 65536) {
            rc = slirp_add_hostfwd(s_pSlirp, 0, aLo, (int)s_sshPort, aGuest, 22);
            VhLog("net: hostfwd 127.0.0.1:%d -> 10.0.2.15:22 rc=%d", s_sshPort, rc);
        }
        if (s_mcpPort > 0 && s_mcpPort < 65536) {
            rc = slirp_add_hostfwd(s_pSlirp, 0, aLo, (int)s_mcpPort, aGuest, 2223);
            VhLog("net: hostfwd 127.0.0.1:%d -> 10.0.2.15:2223 rc=%d", s_mcpPort, rc);
        }
    }
}

int VhNetInput(const void *pv, int cb)
{
    static u32 s_cTx = 0;
    if (s_cTx < 12) {
        const u8 *p = (const u8 *)pv;
        VhLog("net: tx len=%d slirp=%d p=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
              cb, s_pSlirp != NULL,
              p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
              p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
        if (s_cTx == 0)
            VhLog("net: tx48 %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                  p[16], p[17], p[18], p[19], p[20], p[21], p[22], p[23],
                  p[24], p[25], p[26], p[27], p[28], p[29], p[30], p[31],
                  p[32], p[33], p[34], p[35], p[36], p[37], p[38], p[39],
                  p[40], p[41], p[42], p[43], p[44], p[45], p[46], p[47]);
        s_cTx++;
    }
    if (s_pSlirp && cb > 0)
        slirp_input(s_pSlirp, (const uint8_t *)pv, cb);
    return 0;
}

void VhNetPoll(void)
{
    int nfds = 0;
    int fErr = 0;
    fd_set rfds, wfds, xfds;
    struct timeval tv;

    if (!s_pSlirp)
        return;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&xfds);

    VhDevLock();
    slirp_select_fill(&nfds, &rfds, &wfds, &xfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (select(nfds, &rfds, &wfds, &xfds, &tv) < 0)
        fErr = 1;
    slirp_select_poll(&rfds, &wfds, &xfds, fErr);
    VhDevUnlock();
}
