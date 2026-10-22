# Builds the Soil VM engine (vmhost.exe) with the WinLibs mingw-w64 toolchain.
param(
    [string]$Gcc = "C:\Users\Lenovo\winlibs\mingw64\bin\gcc.exe"
)

$ErrorActionPreference = "Continue"
$src = Join-Path $PSScriptRoot "src"
$bin = Join-Path $PSScriptRoot "bin"
New-Item -ItemType Directory -Force -Path $bin | Out-Null

$slirp = Join-Path $src "slirp"
$sources = @(
    (Join-Path $src "vmhost.c"),
    (Join-Path $src "winhv.c"),
    (Join-Path $src "devices.c"),
    (Join-Path $src "viewer.c"),
    (Join-Path $src "ipc.c"),
    (Join-Path $src "guest.c"),
    (Join-Path $src "log.c"),
    (Join-Path $src "pci.c"),
    (Join-Path $src "ne2000.c"),
    (Join-Path $src "virtio_blk.c"),
    (Join-Path $src "netglue.c"),
    (Join-Path $slirp "bootp.c"),
    (Join-Path $slirp "cksum.c"),
    (Join-Path $slirp "if.c"),
    (Join-Path $slirp "ip_icmp.c"),
    (Join-Path $slirp "ip_input.c"),
    (Join-Path $slirp "ip_output.c"),
    (Join-Path $slirp "mbuf.c"),
    (Join-Path $slirp "misc.c"),
    (Join-Path $slirp "sbuf.c"),
    (Join-Path $slirp "slirp.c"),
    (Join-Path $slirp "socket.c"),
    (Join-Path $slirp "tcp_input.c"),
    (Join-Path $slirp "tcp_output.c"),
    (Join-Path $slirp "tcp_subr.c"),
    (Join-Path $slirp "tcp_timer.c"),
    (Join-Path $slirp "tftp.c"),
    (Join-Path $slirp "udp.c"),
    (Join-Path $slirp "win32_compat.c")
)
$out = Join-Path $bin "vmhost.exe"

$args = @(
    "-m64", "-O2", "-std=gnu11", "-fgnu89-inline",
    "-Wall", "-Wextra", "-Wno-unused-parameter",
    "-I$slirp",
    "-o", $out
) + $sources + @("-lwinhvplatform", "-lgdi32", "-luser32", "-lws2_32", "-liphlpapi", "-lwinmm")

Write-Host "gcc $($args -join ' ')"
$p = Start-Process -FilePath $Gcc -ArgumentList $args -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput (Join-Path $bin "build.out.log") `
    -RedirectStandardError (Join-Path $bin "build.err.log")
if ($p.ExitCode -ne 0) {
    Get-Content (Join-Path $bin "build.err.log") | Write-Host
    Write-Host "BUILD FAILED ($($p.ExitCode))"
    exit $p.ExitCode
}
Write-Host "BUILD OK: $out"
exit 0
