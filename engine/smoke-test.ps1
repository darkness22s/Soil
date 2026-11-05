param(
    [Parameter(Mandatory = $true)]
    [string]$ImagesDir,
    [int]$BootTimeoutSeconds = 180,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$images = (Resolve-Path -LiteralPath $ImagesDir).Path
foreach ($name in @("vmlinuz64", "core2.gz", "soil.squashfs")) {
    if (-not (Test-Path -LiteralPath (Join-Path $images $name) -PathType Leaf)) {
        throw "Required guest image '$name' was not found in '$images'."
    }
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "VM engine build failed."
    }
}

function Get-LoopbackPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try { return ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port }
    finally { $listener.Stop() }
}

function Test-LoopbackPort([int]$Port) {
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $connect = $client.ConnectAsync("127.0.0.1", $Port)
        return $connect.Wait(1000) -and $client.Connected
    } catch {
        return $false
    } finally {
        $client.Dispose()
    }
}

$sshPort = Get-LoopbackPort
$mcpPort = Get-LoopbackPort
while ($mcpPort -eq $sshPort) { $mcpPort = Get-LoopbackPort }
$log = Join-Path $env:TEMP ("soil-vm-smoke-{0}.log" -f [guid]::NewGuid().ToString("N"))
$engine = Join-Path $PSScriptRoot "bin\vmhost.exe"
$arguments = @(
    "--kernel", (Join-Path $images "vmlinuz64"),
    "--initrd", (Join-Path $images "core2.gz"),
    "--disk", (Join-Path $images "soil.squashfs"),
    "--ram", "1024", "--no-ui", "--log", $log,
    "--ssh-port", $sshPort.ToString(), "--mcp-port", $mcpPort.ToString()
)
$quotedArguments = $arguments | ForEach-Object {
    '"' + $_.Replace('"', '\\"') + '"'
}
$process = Start-Process -FilePath $engine -ArgumentList $quotedArguments -PassThru -WindowStyle Hidden
try {
    $deadline = [DateTime]::UtcNow.AddSeconds($BootTimeoutSeconds)
    $sshReady = $false
    $mcpReady = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($process.HasExited) {
            throw "VM engine exited during boot (code $($process.ExitCode))."
        }
        $sshReady = Test-LoopbackPort $sshPort
        $mcpReady = Test-LoopbackPort $mcpPort
        if ($sshReady -and $mcpReady) { break }
        Start-Sleep -Seconds 2
        $process.Refresh()
    }
    if (-not $sshReady -or -not $mcpReady) {
        throw "Guest SSH/MCP forwards did not become reachable before the boot timeout."
    }
    Write-Host "Guest SSH forward is reachable at 127.0.0.1:$sshPort."
    Write-Host "Guest MCP-over-SSH forward is reachable at 127.0.0.1:$mcpPort."
} catch {
    if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 80 | Write-Host }
    throw
} finally {
    if (-not $process.HasExited) {
        $process.Kill()
        $process.WaitForExit(10000) | Out-Null
    }
    $process.Dispose()
    Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue
}
