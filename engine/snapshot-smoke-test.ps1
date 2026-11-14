param(
    [Parameter(Mandatory = $true)]
    [string]$ImagesDir,
    [int]$BootTimeoutSeconds = 180,
    [string]$KernelCmdLine = "console=ttyS0 quiet loglevel=3 acpi=off noapic nolapic mitigations=off nozswap idle=poll",
    [switch]$SkipBuild
)

# Real WHPX integration test. Needs an x64 Windows host with virtualization
# enabled and genuine guest images (the Git LFS pointers cannot boot).
$ErrorActionPreference = "Stop"
$images = (Resolve-Path -LiteralPath $ImagesDir).Path
foreach ($name in @("vmlinuz64", "core2.gz", "soil.squashfs")) {
    $item = Get-Item -LiteralPath (Join-Path $images $name) -ErrorAction Stop
    if ($item.Length -lt 4096) {
        throw "Guest image $name is a placeholder or LFS pointer."
    }
}
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1")
    if ($LASTEXITCODE -ne 0) { throw "VM engine build failed." }
}

$engine = Join-Path $PSScriptRoot "bin\vmhost.exe"
$directory = Join-Path $env:TEMP ("soil-snap-smoke-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$snapshot = Join-Path $directory "resume.snap"
$consumed = Join-Path $directory "resume.in-use.snap"
$live = $null

function Get-FreePort {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try { return ([Net.IPEndPoint]$listener.LocalEndpoint).Port }
    finally { $listener.Stop() }
}

function Test-SshBanner([int]$port) {
    $client = [Net.Sockets.TcpClient]::new()
    try {
        $connect = $client.ConnectAsync("127.0.0.1", $port)
        if (-not $connect.Wait(1000) -or -not $client.Connected) { return $false }
        $stream = $client.GetStream()
        $stream.ReadTimeout = 1500
        $reader = [IO.StreamReader]::new($stream)
        try { return $reader.ReadLine() -match '^SSH-2\.0-' }
        finally { $reader.Dispose() }
    } catch { return $false }
    finally { $client.Dispose() }
}

function Start-Guest([string]$logPath, [string]$restorePath) {
    $script:sshPort = Get-FreePort
    $mcpPort = Get-FreePort
    while ($mcpPort -eq $script:sshPort) { $mcpPort = Get-FreePort }
    $script:token = "snap" + [guid]::NewGuid().ToString("N")
    $args = @(
        "--kernel", (Join-Path $images "vmlinuz64"),
        "--initrd", (Join-Path $images "core2.gz"),
        "--disk", (Join-Path $images "soil.squashfs"),
        "--ram", "512", "--cmdline", $KernelCmdLine, "--no-ui", "--ipc", $script:token,
        "--log", $logPath,
        "--ssh-port", $script:sshPort.ToString(),
        "--mcp-port", $mcpPort.ToString(),
        "--snapshot-out", $snapshot
    )
    if ($restorePath) { $args += @("--restore", $restorePath) }
    $quoted = $args | ForEach-Object { '"' + $_.Replace('"', '\\"') + '"' }
    $script:clock = [Diagnostics.Stopwatch]::StartNew()
    $script:live = Start-Process -FilePath $engine -ArgumentList $quoted -PassThru -WindowStyle Hidden
    $until = [DateTime]::UtcNow.AddSeconds($BootTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $until) {
        $script:live.Refresh()
        if ($script:live.HasExited) {
            throw "vmhost exited before guest SSH was ready: $($script:live.ExitCode)"
        }
        if (Test-SshBanner $script:sshPort) {
            $script:clock.Stop()
            Write-Host ("SSH guest-ready after {0:N1} ms" -f $script:clock.Elapsed.TotalMilliseconds)
            return
        }
        Start-Sleep -Milliseconds 30
    }
    throw "Timed out awaiting an actual SSH banner from guest."
}

function Stop-GuestClean {
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    $map = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $map = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting(
                "Local\SoilFb_" + $script:token,
                [IO.MemoryMappedFiles.MemoryMappedFileRights]::ReadWrite)
            break
        } catch [IO.FileNotFoundException] {
            Start-Sleep -Milliseconds 50
        }
    }
    if (-not $map) { throw "VM framebuffer/control mapping not available." }
    try {
        $view = $map.CreateViewAccessor()
        try { $view.Write([long]4, [int]1) }
        finally { $view.Dispose() }
    } finally { $map.Dispose() }

    if (-not $script:live.WaitForExit(45000)) {
        throw "Checkpointing did not stop vmhost within 45 seconds."
    }
    if ($script:live.ExitCode -ne 0) { throw "vmhost stopped with exit code $($script:live.ExitCode)" }
    if (-not (Test-Path -LiteralPath $snapshot -PathType Leaf)) {
        throw "The clean shutdown did not save a snapshot."
    }
    if ((Get-Item -LiteralPath $snapshot).Length -lt 512MB) {
        throw "Snapshot is incomplete: expected at least 512 MiB of RAM."
    }
    Write-Host "Snapshot written; vmhost has exited (no guest CPU or RAM retained)."
    $script:live.Dispose()
    $script:live = $null
}

try {
    $firstLog = Join-Path $directory "first.log"
    Start-Guest $firstLog ""
    Stop-GuestClean

    Move-Item -LiteralPath $snapshot -Destination $consumed
    $secondLog = Join-Path $directory "restore.log"
    Start-Guest $secondLog $consumed
    $text = Get-Content -LiteralPath $secondLog -Raw
    if ($text -notmatch "snap: restored in (\d+) ms") {
        throw "Guest SSH is ready, but the engine did not report a successful restore."
    }
    Write-Host ("Snapshot restore core reported {0} ms; end-to-end guest-ready was {1:N1} ms." -f
        $Matches[1], $script:clock.Elapsed.TotalMilliseconds)
    Stop-GuestClean
    Write-Host "PASS: clean checkpoint -> no vmhost -> lazy restore -> clean checkpoint."
}
catch {
    foreach ($log in @("first.log", "restore.log")) {
        $path = Join-Path $directory $log
        if (Test-Path $path) { Get-Content -LiteralPath $path -Tail 70 | Write-Host }
    }
    throw
}
finally {
    if ($live) {
        try {
            if (-not $live.HasExited) { $live.Kill() }
            $live.WaitForExit(5000) | Out-Null
            $live.Dispose()
        } catch {}
    }
    Remove-Item -LiteralPath $directory -Recurse -Force -ErrorAction SilentlyContinue
}
