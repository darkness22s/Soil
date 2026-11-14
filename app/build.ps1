param(
    [string]$Configuration = "Release",
    [string]$ImagesDir = "",
    [string]$Gcc = "",
    [switch]$SkipEngine,
    [switch]$SkipMsi
)

$ErrorActionPreference = "Stop"
$app = $PSScriptRoot
$root = Split-Path -Parent $app
$out = Join-Path $app "dist\Soil"

if (-not $SkipEngine) {
    & (Join-Path $root "engine\build.ps1") -Gcc $Gcc
    if (-not $?) {
        throw "Engine build failed."
    }
}

New-Item -ItemType Directory -Force -Path $out | Out-Null
& dotnet publish (Join-Path $app "Soil\Soil.csproj") `
    -c $Configuration -r win-x64 --self-contained true -o $out --nologo
if ($LASTEXITCODE -ne 0) {
    throw "App publish failed."
}

$engineDir = Join-Path $out "engine"
New-Item -ItemType Directory -Force -Path $engineDir | Out-Null
Copy-Item -LiteralPath (Join-Path $root "engine\bin\vmhost.exe") `
    -Destination (Join-Path $engineDir "vmhost.exe") -Force

if ([string]::IsNullOrWhiteSpace($ImagesDir)) {
    $candidates = @(
        (Join-Path $app "images"),
        (Join-Path $app "dist\Boxify\images"),
        (Join-Path $out "images")
    )
    $ImagesDir = $candidates | Where-Object {
        Test-Path -LiteralPath (Join-Path $_ "soil.squashfs")
    } | Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($ImagesDir)) {
    throw "Guest images not found. Pass -ImagesDir with vmlinuz64, core2.gz, and soil.squashfs."
}
$imagesSource = $ImagesDir
if (-not (Test-Path -LiteralPath $imagesSource)) {
    throw "Guest images not found at $imagesSource."
}
$imagesDir = Join-Path $out "images"
New-Item -ItemType Directory -Force -Path $imagesDir | Out-Null
if ((Resolve-Path -LiteralPath $imagesSource).Path -ne (Resolve-Path -LiteralPath $imagesDir).Path) {
    Copy-Item -Path (Join-Path $imagesSource "*") -Destination $imagesDir -Force
}

$readme = @"
Soil 4.2.1 - private local build

Private build. Not for public release or upload.

Requirements
- Windows 10/11 x64
- Microsoft Edge WebView2 Runtime (usually included with Windows 11)
- Hardware virtualization and the Windows Hypervisor Platform feature enabled
- Node.js 20 or newer on the PC for JavaScript apps (optional)
- Python 3.8 or newer to package imported apps for the native .sol runtime (optional)

Use
- Launch Soil from the Start Menu.
- Setup starts with no machines. Create your own VM with New VM, choose resources, press Start when ready, and click the screen to capture input.
- Use the terminal + to create a 512 MB agent machine with Claude Code and Codex CLI, then open its SSH tunnel.
- Agent home directories and sign-in state live on each machine data disk and survive restarts.
- Desktop machines park after 90 seconds without use or when the app stays unfocused. A parked machine writes a checkpoint and fully exits, releasing guest CPU and RAM. It restarts only when requested. Background app/agent workers are not automatically parked on UI focus loss. Data disks persist.
- Sign-in pages open in the machine's built-in browser.
- Use the branch button to import a GitHub repository into its own app machine.
- Browse machines, apps, skills, cache, and logs from Files.
- Machine data lives in %USERPROFILE%\.soil.
- GitHub can be connected or reconnected from the main app with the GitHub button. Private repositories require the optional OAuth Worker in app\auth-worker.

Uninstall
- Settings > Apps > Installed apps > Soil
- Your files in %USERPROFILE%\.soil are not removed automatically.

Packaging
- Self-contained .NET 8 x64 application.
- Bundled VM engine and guest images.
- No telemetry or external fonts.
- Optional self-hosted Cloudflare OAuth relay for private GitHub repositories.
"@
Set-Content -LiteralPath (Join-Path $out "README.txt") -Value $readme -Encoding UTF8

if (-not $SkipMsi) {
    & (Join-Path $app "installer\build-msi.ps1")
}

Write-Host "Build ready: $out"
