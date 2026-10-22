/*
 * Soil VM engine - Windows Hypervisor Platform availability probe.
 * Uses the same WHPX user-mode API surface that VirtualBox's NEM backend uses.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <winhvplatform.h>

int main(void)
{
    UINT32 cb = 0;
    WHV_CAPABILITY cap;
    memset(&cap, 0, sizeof(cap));

    HRESULT hr = WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &cap, sizeof(cap), &cb);
    printf("WHvGetCapability(HypervisorPresent): hr=0x%08lx cb=%u present=%d\n",
           (unsigned long)hr, cb, hr == S_OK ? (int)cap.HypervisorPresent : -1);
    if (hr != S_OK || !cap.HypervisorPresent)
    {
        printf("RESULT: WHPX_NOT_AVAILABLE\n");
        return 1;
    }

    WHV_CAPABILITY c;

    memset(&c, 0, sizeof(c));
    if (SUCCEEDED(WHvGetCapability(WHvCapabilityCodeProcessorVendor, &c, sizeof(c), &cb)))
        printf("ProcessorVendor=%u (1=Intel 2=AMD)\n", (unsigned)c.ProcessorVendor);

    memset(&c, 0, sizeof(c));
    if (SUCCEEDED(WHvGetCapability(WHvCapabilityCodePhysicalAddressWidth, &c, sizeof(c), &cb)))
        printf("PhysicalAddressWidth=%u\n", c.PhysicalAddressWidth);

    memset(&c, 0, sizeof(c));
    if (SUCCEEDED(WHvGetCapability(WHvCapabilityCodeExtendedVmExits, &c, sizeof(c), &cb)))
        printf("ExtendedVmExits raw=0x%016llx\n", (unsigned long long)c.ExtendedVmExits.AsUINT64);

    printf("RESULT: WHPX_AVAILABLE\n");
    return 0;
}
