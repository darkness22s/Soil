using System.Collections.Concurrent;
using System.Text.Json;

namespace Soil;

public sealed record NativeLinuxAppDto(
    string Id, string Name, string FileName, string Format, long Bytes);

/// <summary>
/// Stores imported Linux binaries next to each VM on the Windows host, then
/// copies/executes them ONLY inside the existing bundled Linux guest via SSH.
/// Soil's guest /home is ephemeral: restore the guest launcher entries at
/// every explicit VM start, while keeping the original binary host-side.
/// No native app or VM process runs when the machine is stopped.
/// </summary>
public sealed class NativeLinuxAppManager
{
    public const long MaxBytes = 256L * 1024 * 1024;
    private readonly VmManager m_vms;
    private readonly SoilPaths m_paths;
    private readonly ConcurrentDictionary<string, SemaphoreSlim> m_locks = new();

    public NativeLinuxAppManager(VmManager vms, SoilPaths paths)
    {
        m_vms = vms;
        m_paths = paths;
    }

    private string AppRoot(string vmId)
    {
        if (!SoilPaths.IsSafeId(vmId))
            throw new ArgumentException("Invalid VM identifier.");
        return Path.Combine(m_paths.MachineDir(vmId), "native-apps");
    }

    private static bool AppIdOk(string id) => Guid.TryParseExact(id, "N", out _);

    private string AppFolder(string vmId, string appId)
    {
        if (!AppIdOk(appId))
            throw new ArgumentException("Invalid Linux app identifier.");
        return Path.Combine(AppRoot(vmId), appId);
    }

    private void RequireDesktop(string vmId)
    {
        var vm = m_vms.FindRecord(vmId);
        if (vm is null || !string.Equals(vm.Kind, "desktop", StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException("Choose an existing Soil desktop VM.");
    }

    private static string SafeName(string value)
    {
        var raw = Path.GetFileNameWithoutExtension(value);
        var chars = raw.Where(c => (c >= 'A' && c <= 'Z') ||
                                   (c >= 'a' && c <= 'z') ||
                                   (c >= '0' && c <= '9') ||
                                   c == ' ' || c == '_' || c == '-' || c == '.').ToArray();
        var cleaned = new string(chars).Trim(' ', '.');
        if (string.IsNullOrWhiteSpace(cleaned))
            return "Linux app";
        return cleaned.Length > 60 ? cleaned[..60] : cleaned;
    }

    private static string ValidateElf(string file, string originalName)
    {
        Span<byte> header = stackalloc byte[64];
        using var input = File.OpenRead(file);
        if (input.Read(header) < 64 || !header[..4].SequenceEqual("\u007fELF"u8) ||
            header[4] != 2 || header[5] != 1 || header[6] != 1 ||
            header[18] != 0x3e || header[19] != 0x00 ||
            (header[16] != 2 && header[16] != 3) || header[17] != 0 ||
            header.Slice(24, 8).SequenceEqual(stackalloc byte[8]))
            throw new ArgumentException(
                "Expected an executable Linux x86_64 ELF file (not a script, package, or ISO).");

        var appImage = header[8] == (byte)'A' && header[9] == (byte)'I' && header[10] == 2;
        if (originalName.EndsWith(".AppImage", StringComparison.OrdinalIgnoreCase) && !appImage)
            throw new ArgumentException("Only Type 2 x86_64 AppImages are supported.");
        return appImage ? "AppImage" : "ELF";
    }

    public IReadOnlyList<NativeLinuxAppDto> List(string vmId)
    {
        RequireDesktop(vmId);
        var root = AppRoot(vmId);
        if (!Directory.Exists(root))
            return Array.Empty<NativeLinuxAppDto>();
        var entries = new List<NativeLinuxAppDto>();
        foreach (var folder in Directory.EnumerateDirectories(root))
        {
            if (!AppIdOk(Path.GetFileName(folder)))
                continue;
            try
            {
                var json = File.ReadAllText(Path.Combine(folder, "meta.json"));
                var app = JsonSerializer.Deserialize<NativeLinuxAppDto>(json, JsonOptions.Web);
                if (app is not null && app.Id == Path.GetFileName(folder) &&
                    File.Exists(Path.Combine(folder, "app")))
                    entries.Add(app);
            }
            catch (IOException) { }
            catch (JsonException) { }
            catch (UnauthorizedAccessException) { }
        }
        return entries.OrderBy(app => app.Name, StringComparer.OrdinalIgnoreCase).ToArray();
    }

    public async Task<NativeLinuxAppDto> ImportAsync(
        string vmId, string fileName, Stream body, long length, CancellationToken ct)
    {
        RequireDesktop(vmId);
        if (length < 64 || length > MaxBytes)
            throw new ArgumentException("Linux executable must be between 64 bytes and 256 MiB.");
        if (string.IsNullOrWhiteSpace(fileName) || fileName.Length > 200 ||
            fileName.Any(c => c < 32 || c == '/' || c == '\\' || c == ':'))
            throw new ArgumentException("Invalid Linux app filename.");
        var extension = Path.GetExtension(fileName);
        if (extension.Length != 0 &&
            !new[] { ".appimage", ".elf", ".bin", ".run" }
                 .Contains(extension.ToLowerInvariant()))
            throw new ArgumentException(
                "Upload a native Linux ELF executable or a Type 2 .AppImage, not a .deb/.rpm/.iso package.");

        var id = Guid.NewGuid().ToString("N");
        var folder = AppFolder(vmId, id);
        Directory.CreateDirectory(folder);
        var binary = Path.Combine(folder, "app");
        try
        {
            await using (var output = new FileStream(
                binary, FileMode.CreateNew, FileAccess.Write, FileShare.None, 64 * 1024,
                FileOptions.Asynchronous))
            {
                var buffer = new byte[64 * 1024];
                var remaining = length;
                while (remaining > 0)
                {
                    var read = await body.ReadAsync(
                        buffer.AsMemory(0, (int)Math.Min(buffer.Length, remaining)), ct);
                    if (read == 0)
                        throw new InvalidDataException("Linux app upload ended before the declared length.");
                    await output.WriteAsync(buffer.AsMemory(0, read), ct);
                    remaining -= read;
                }
            }

            var format = ValidateElf(binary, fileName);
            var dto = new NativeLinuxAppDto(id, SafeName(fileName), fileName, format, length);
            await File.WriteAllTextAsync(
                Path.Combine(folder, "meta.json"),
                JsonSerializer.Serialize(dto, JsonOptions.Web), ct);
            return dto;
        }
        catch
        {
            if (Directory.Exists(folder))
                Directory.Delete(folder, true);
            throw;
        }
    }

    private NativeLinuxAppDto Find(string vmId, string appId)
    {
        return List(vmId).FirstOrDefault(app => app.Id == appId)
               ?? throw new FileNotFoundException("The Linux app was not found in this VM.");
    }

    private SemaphoreSlim Gate(string vmId) =>
        m_locks.GetOrAdd(vmId, _ => new SemaphoreSlim(1, 1));

    public async Task<NativeLinuxAppDto> LaunchAsync(
        string vmId, string appId, CancellationToken ct)
    {
        RequireDesktop(vmId);
        var app = Find(vmId, appId);
        var gate = Gate(vmId);
        await gate.WaitAsync(ct);
        try
        {
            await m_vms.DeployNativeLinuxAppAsync(
                vmId, app.Id, Path.Combine(AppFolder(vmId, app.Id), "app"),
                app.Name, launch: true, ct);
            return app;
        }
        finally { gate.Release(); }
    }

    public async Task RemoveAsync(string vmId, string appId, CancellationToken ct)
    {
        RequireDesktop(vmId);
        Find(vmId, appId);
        var gate = Gate(vmId);
        await gate.WaitAsync(ct);
        try
        {
            // Delete the host copy first: it must not be restored at next boot.
            var folder = AppFolder(vmId, appId);
            Directory.Delete(folder, true);
            await m_vms.RemoveNativeLinuxAppAsync(vmId, appId, ct);
        }
        finally { gate.Release(); }
    }

    public async Task RestoreAsync(string vmId, CancellationToken ct)
    {
        var gate = Gate(vmId);
        await gate.WaitAsync(ct);
        try
        {
            foreach (var app in List(vmId))
            {
                ct.ThrowIfCancellationRequested();
                try
                {
                    await m_vms.DeployNativeLinuxAppAsync(
                        vmId, app.Id, Path.Combine(AppFolder(vmId, app.Id), "app"),
                        app.Name, launch: false, ct);
                }
                catch (OperationCanceledException) { throw; }
                catch (Exception) { /* The VM remains usable if one app fails to restore. */ }
            }
        }
        finally { gate.Release(); }
    }
}
