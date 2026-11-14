using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO.MemoryMappedFiles;
using System.IO.Pipes;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text.Json;
using Microsoft.AspNetCore.Http;

namespace Soil;

public sealed class VmManager
{
    private const int FrameWidth = 1360;
    private const int FrameHeight = 768;
    private const int FrameStride = FrameWidth * 4;
    private const int FrameBytes = FrameStride * FrameHeight;
    private const int SharedFrameOffset = 24;
    private const int CmdOffset = SharedFrameOffset + FrameBytes;
    private const int CmdHaveOffset = CmdOffset + 4;
    private const int CmdPathOffset = CmdOffset + 8;
    private const int CmdPathBytes = 1024;
    private const int SharedSize = CmdPathOffset + CmdPathBytes;
    private const int StopOffset = 4;

    private readonly string m_baseDir;
    private readonly string m_imagesDir;
    private readonly SoilPaths m_paths;
    private readonly string m_vmhost;
    private readonly string m_sshExe;
    private readonly string m_sshKeyPath;
    private readonly object m_lock = new();
    private readonly List<VmRecord> m_records;
    private readonly Dictionary<string, VmRuntime> m_runtimes = new(StringComparer.OrdinalIgnoreCase);

    internal static readonly ImageCodecInfo? JpegCodec =
        ImageCodecInfo.GetImageEncoders().FirstOrDefault(c => c.FormatID == ImageFormat.Jpeg.Guid);

    public VmManager(string baseDir, SoilPaths paths)
    {
        m_baseDir = baseDir;
        m_paths = paths;
        m_paths.Ensure();
        m_imagesDir = ResolveImagesDir(baseDir);
        m_vmhost = Path.Combine(baseDir, "engine", "vmhost.exe");
        m_sshExe = FindSshExecutable();
        m_sshKeyPath = PrepareSshKey();

        m_records = LoadRecords();
        var agentRecordsChanged = false;
        foreach (var record in m_records)
        {
            m_runtimes[record.Id] = new VmRuntime(record);
            if (!string.Equals(record.Kind, "agent", StringComparison.OrdinalIgnoreCase) ||
                (record.RamMb == 512 && record.DiskGb == 40 && record.Size == "s"))
                continue;
            record.RamMb = 512;
            record.DiskGb = 40;
            record.Size = "s";
            agentRecordsChanged = true;
        }
        if (agentRecordsChanged)
            SaveRecordsLocked();

        _ = Task.Run(StatsLoopAsync);
    }

    public SoilPaths Home => m_paths;

    // Runs only when a guest reaches the desktop-ready state; never keeps a VM alive.
    internal Func<string, CancellationToken, Task>? RestoreNativeApps { get; set; }

    public IReadOnlyList<VmDto> List()
    {
        lock (m_lock)
        {
            SyncProcessStatesLocked();
            return m_records
                .OrderBy(r => r.CreatedUtc)
                .Select(BundleLocked)
                .ToList();
        }
    }

    public VmDto Create(string name, string size)
    {
        size = size.ToLowerInvariant();
        var (ram, disk) = SizeSpec(size);

        lock (m_lock)
        {
            var baseName = name.Length > 40 ? name[..40] : name;
            var unique = baseName;
            for (var i = 2; m_records.Any(r => r.Name.Equals(unique, StringComparison.OrdinalIgnoreCase)); i++)
                unique = $"{baseName} {i}";

            var record = new VmRecord
            {
                Id = Guid.NewGuid().ToString("N"),
                Name = unique,
                RamMb = ram,
                Cpu = 1,
                DiskGb = disk,
                Size = size,
                Os = "Soil Desktop",
                CreatedUtc = DateTime.UtcNow
            };
            m_paths.EnsureMachine(record.Id);
            WriteMachineRecordLocked(record);
            m_records.Add(record);
            m_runtimes[record.Id] = new VmRuntime(record);
            SaveRecordsLocked();
            return BundleLocked(record);
        }
    }

    private static (int RamMb, int DiskGb) SizeSpec(string size) => size switch
    {
        "s" or "tiny" => (512, 40),
        "nano" => (2048, 80),
        "standard" => (4096, 80),
        "l" => (2048, 160),
        "pro" => (8192, 160),
        "wide" => (16384, 160),
        _ => (1024, 80)
    };

    public VmDto CreateAgent(string name)
    {
        lock (m_lock)
        {
            var baseName = name.Length > 40 ? name[..40] : name;
            var unique = baseName;
            for (var i = 2; m_records.Any(r => r.Name.Equals(unique, StringComparison.OrdinalIgnoreCase)); i++)
                unique = $"{baseName} {i}";

            var record = new VmRecord
            {
                Id = Guid.NewGuid().ToString("N"),
                Name = unique,
                RamMb = 512,
                Cpu = 1,
                DiskGb = 40,
                Size = "s",
                Os = "Soil Agent",
                Kind = "agent",
                AgentStatus = "queued",
                CreatedUtc = DateTime.UtcNow
            };
            m_paths.EnsureMachine(record.Id);
            WriteMachineRecordLocked(record);
            m_records.Add(record);
            m_runtimes[record.Id] = new VmRuntime(record);
            SaveRecordsLocked();
            return BundleLocked(record);
        }
    }

    public VmDto CreateApp(
        string name, string size, string repoUrl, string owner, string repo, string? reference)
    {
        size = size.ToLowerInvariant();
        var (ram, disk) = SizeSpec(size);

        lock (m_lock)
        {
            var baseName = name.Length > 40 ? name[..40] : name;
            var unique = baseName;
            for (var i = 2; m_records.Any(r => r.Name.Equals(unique, StringComparison.OrdinalIgnoreCase)); i++)
                unique = $"{baseName} {i}";

            var record = new VmRecord
            {
                Id = Guid.NewGuid().ToString("N"),
                Name = unique,
                RamMb = ram,
                Cpu = 1,
                DiskGb = disk,
                Size = size,
                Os = "Soil App",
                Kind = "app",
                RepoUrl = repoUrl,
                RepoOwner = owner,
                RepoName = repo,
                RepoRef = reference,
                AppStatus = "queued",
                CreatedUtc = DateTime.UtcNow
            };
            m_paths.EnsureMachine(record.Id);
            WriteMachineRecordLocked(record);
            m_records.Add(record);
            m_runtimes[record.Id] = new VmRuntime(record);
            SaveRecordsLocked();
            return BundleLocked(record);
        }
    }

    public VmDto? Get(string id)
    {
        lock (m_lock)
        {
            SyncProcessStatesLocked();
            var record = m_records.FirstOrDefault(r => r.Id == id);
            return record is null ? null : BundleLocked(record);
        }
    }

    internal VmRecord? FindRecord(string id)
    {
        lock (m_lock)
            return m_records.FirstOrDefault(r => r.Id == id);
    }

    public void UpdateAppState(
        string id, string status, string? error, string? hostUrl, string? guestUrl, string? log)
    {
        lock (m_lock)
        {
            var record = m_records.FirstOrDefault(r => r.Id == id);
            if (record is null)
                return;
            record.AppStatus = status;
            record.AppError = error;
            record.AppHostUrl = hostUrl;
            record.AppGuestUrl = guestUrl;
            record.AppLog = log;
            WriteMachineRecordLocked(record);
            SaveRecordsLocked();
        }
    }

    public void UpdateAgentState(
        string id,
        string status,
        string? error,
        string? log,
        string? claudeVersion,
        string? codexVersion)
    {
        lock (m_lock)
        {
            var record = m_records.FirstOrDefault(r => r.Id == id);
            if (record is null)
                return;
            record.AgentStatus = status;
            record.AgentError = error;
            record.AgentLog = log;
            record.AgentClaudeVersion = claudeVersion;
            record.AgentCodexVersion = codexVersion;
            WriteMachineRecordLocked(record);
            SaveRecordsLocked();
        }
    }

    internal (string Exe, string Key, int Port)? GetSshEndpoint(string id)
    {
        lock (m_lock)
        {
            var record = m_records.FirstOrDefault(r => r.Id == id);
            if (record is null || !m_runtimes.TryGetValue(id, out var runtime))
                return null;
            if (runtime.Status != "running" || runtime.SshPort <= 0)
                return null;
            if (string.IsNullOrWhiteSpace(m_sshKeyPath) || !File.Exists(m_sshKeyPath))
                return null;
            return (m_sshExe, m_sshKeyPath, runtime.SshPort);
        }
    }

    public async Task OpenGuestUrlAsync(string id, string url, CancellationToken ct)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri) ||
            uri.Scheme != Uri.UriSchemeHttp ||
            !uri.Host.Equals("10.0.2.2", StringComparison.Ordinal))
            throw new ArgumentException("Invalid guest app URL.", nameof(url));

        var script = "i=0; while [ $i -lt 60 ]; do DISPLAY=:0 xset q >/dev/null 2>&1 && break; " +
                     "sleep 1; i=$((i+1)); done; DISPLAY=:0 soil-browser " +
                     ShellQuote(url);
        var remote = "nohup sh -c " + ShellQuote(script) +
                     " >/tmp/soil-app-browser.log 2>&1 &";

        for (var attempt = 0; attempt < 90; attempt++)
        {
            ct.ThrowIfCancellationRequested();
            int port;
            string key;
            lock (m_lock)
            {
                if (m_records.FirstOrDefault(r => r.Id == id) is null)
                    return;
                if (!m_runtimes.TryGetValue(id, out var runtime))
                    return;
                port = runtime.SshPort;
                key = m_sshKeyPath;
            }
            if (port > 0 && !string.IsNullOrWhiteSpace(key) && File.Exists(key))
            {
                try
                {
                    var psi = new ProcessStartInfo
                    {
                        FileName = m_sshExe,
                        UseShellExecute = false,
                        CreateNoWindow = true
                    };
                    psi.ArgumentList.Add("-i");
                    psi.ArgumentList.Add(key);
                    psi.ArgumentList.Add("-p");
                    psi.ArgumentList.Add(port.ToString(CultureInfo.InvariantCulture));
                    psi.ArgumentList.Add("-o");
                    psi.ArgumentList.Add("StrictHostKeyChecking=no");
                    psi.ArgumentList.Add("-o");
                    psi.ArgumentList.Add("UserKnownHostsFile=NUL");
                    psi.ArgumentList.Add("-o");
                    psi.ArgumentList.Add("BatchMode=yes");
                    psi.ArgumentList.Add("-o");
                    psi.ArgumentList.Add("ConnectTimeout=5");
                    psi.ArgumentList.Add("-o");
                    psi.ArgumentList.Add("LogLevel=ERROR");
                    psi.ArgumentList.Add("tc@127.0.0.1");
                    psi.ArgumentList.Add(remote);

                    using var ssh = new Process { StartInfo = psi };
                    ssh.Start();
                    using var wait = CancellationTokenSource.CreateLinkedTokenSource(ct);
                    wait.CancelAfter(TimeSpan.FromSeconds(20));
                    await ssh.WaitForExitAsync(wait.Token);
                    if (ssh.ExitCode == 0)
                        return;
                }
                catch (OperationCanceledException)
                {
                }
                catch
                {
                }
            }
            await Task.Delay(1000, ct);
        }
        throw new InvalidOperationException("Could not open the app in the machine.");
    }

    public async Task OpenGuestSolAsync(
        string id, string solPath, string appId, CancellationToken ct)
    {
        if (string.IsNullOrWhiteSpace(solPath) || !File.Exists(solPath))
            throw new ArgumentException("Invalid app package.", nameof(solPath));
        if (string.IsNullOrWhiteSpace(appId))
            throw new ArgumentException("Invalid app ID.", nameof(appId));

        var remote = "/tmp/soil-app-" +
                     id.Replace("-", "", StringComparison.Ordinal) + ".sol";
        var launch = "i=0; while [ $i -lt 60 ]; do " +
                     "DISPLAY=:0 xset q >/dev/null 2>&1 && break; sleep 1; i=$((i+1)); done; " +
                     "DISPLAY=:0 soil launch " + ShellQuote(appId);
        var install = "DISPLAY=:0 soil install " + ShellQuote(remote) +
                      " >/tmp/soil-app-sol.log 2>&1; rc=$?; " +
                      "if [ $rc -ne 0 ]; then exit $rc; fi; " +
                      "nohup sh -c " + ShellQuote(launch) +
                      " >>/tmp/soil-app-sol.log 2>&1 &";

        for (var attempt = 0; attempt < 90; attempt++)
        {
            ct.ThrowIfCancellationRequested();
            int port;
            string key;
            lock (m_lock)
            {
                if (m_records.FirstOrDefault(r => r.Id == id) is null)
                    return;
                if (!m_runtimes.TryGetValue(id, out var runtime))
                    return;
                port = runtime.SshPort;
                key = m_sshKeyPath;
            }

            if (port > 0 && !string.IsNullOrWhiteSpace(key) && File.Exists(key))
            {
                try
                {
                    if (await CopyGuestFileAsync(port, key, solPath, remote, ct) &&
                        await RunGuestCommandAsync(
                            port, key, "command -v soil >/dev/null 2>&1", 10, ct) &&
                        await RunGuestCommandAsync(port, key, install, 120, ct))
                        return;
                }
                catch (OperationCanceledException)
                {
                    throw;
                }
                catch
                {
                }
            }
            await Task.Delay(1000, ct);
        }
        throw new InvalidOperationException(
            "Could not install and launch the app package in the machine.");
    }

    internal async Task DeployNativeLinuxAppAsync(
        string machineId, string appId, string filePath, string displayName,
        bool launch, CancellationToken ct)
    {
        var record = FindRecord(machineId);
        if (record is null || !string.Equals(record.Kind, "desktop", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Choose an existing desktop VM.");
        var endpoint = GetSshEndpoint(machineId);
        if (endpoint is null)
            throw new InvalidOperationException(
                "Start the VM first. The guest SSH tunnel and key must be available to install Linux apps.");
        if (!File.Exists(filePath))
            throw new FileNotFoundException("The saved Linux application is missing.", filePath);

        var (ssh, key, port) = endpoint.Value;
        var remote = "/tmp/soil-native-" + appId;
        if (!await CopyGuestFileAsync(port, key, filePath, remote, ct))
            throw new InvalidOperationException("Could not transfer the app into the Soil Linux VM.");
        var install = "command -v soil-native >/dev/null 2>&1 && " +
                      "soil-native install " + ShellQuote(appId) + " " +
                      ShellQuote(remote) + " " + ShellQuote(displayName) +
                      "; rc=$?; rm -f " + ShellQuote(remote) + "; exit $rc";
        if (!await RunGuestCommandAsync(port, key, install, 30, ct))
            throw new InvalidOperationException(
                "The guest native-app launcher is unavailable or rejected the executable. " +
                "Rebuild/install Soil with the updated bundled guest image.");
        if (launch && !await RunGuestCommandAsync(
                port, key, "soil-native launch " + ShellQuote(appId), 30, ct))
            throw new InvalidOperationException(
                "The Linux application could not start in the guest. It may need shared libraries " +
                "not included in the compact Soil Linux image.");
    }

    internal async Task RemoveNativeLinuxAppAsync(
        string machineId, string appId, CancellationToken ct)
    {
        var endpoint = GetSshEndpoint(machineId);
        if (endpoint is null)
            return;
        var (_, key, port) = endpoint.Value;
        await RunGuestCommandAsync(port, key, "soil-native remove " +
                                   ShellQuote(appId), 15, ct);
    }

    private async Task<bool> CopyGuestFileAsync(
        int port,
        string key,
        string local,
        string remote,
        CancellationToken ct)
    {
        var psi = new ProcessStartInfo
        {
            FileName = m_sshExe,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        psi.ArgumentList.Add("-i");
        psi.ArgumentList.Add(key);
        psi.ArgumentList.Add("-p");
        psi.ArgumentList.Add(port.ToString(CultureInfo.InvariantCulture));
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add("StrictHostKeyChecking=no");
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add("UserKnownHostsFile=NUL");
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add("BatchMode=yes");
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add("ConnectTimeout=10");
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add("LogLevel=ERROR");
        psi.ArgumentList.Add("tc@127.0.0.1");
        psi.ArgumentList.Add("cat > " + ShellQuote(remote));

        using var ssh = new Process { StartInfo = psi, EnableRaisingEvents = true };
        ssh.OutputDataReceived += (_, _) => { };
        ssh.ErrorDataReceived += (_, _) => { };
        ssh.Start();
        ssh.BeginOutputReadLine();
        ssh.BeginErrorReadLine();

        using var wait = CancellationTokenSource.CreateLinkedTokenSource(ct);
        wait.CancelAfter(TimeSpan.FromMinutes(3));
        try
        {
            await using (var input = File.OpenRead(local))
                await input.CopyToAsync(ssh.StandardInput.BaseStream, wait.Token);
            ssh.StandardInput.Close();
            await ssh.WaitForExitAsync(wait.Token);
        }
        catch (OperationCanceledException)
        {
            KillQuietly(ssh);
            if (ct.IsCancellationRequested)
                throw;
            return false;
        }
        catch
        {
            KillQuietly(ssh);
            return false;
        }
        return ssh.ExitCode == 0;
    }

    private async Task<bool> RunGuestCommandAsync(
        int port,
        string key,
        string remote,
        int timeoutSeconds,
        CancellationToken ct)
    {
        var psi = new ProcessStartInfo
        {
            FileName = m_sshExe,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        psi.ArgumentList.Add("-i");
        psi.ArgumentList.Add(key);
        psi.ArgumentList.Add("-p");
        psi.ArgumentList.Add(port.ToString(CultureInfo.InvariantCulture));
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add("StrictHostKeyChecking=no");
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add("UserKnownHostsFile=NUL");
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add("BatchMode=yes");
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add("ConnectTimeout=5");
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add("LogLevel=ERROR");
        psi.ArgumentList.Add("tc@127.0.0.1");
        psi.ArgumentList.Add(remote);

        using var ssh = new Process { StartInfo = psi, EnableRaisingEvents = true };
        ssh.OutputDataReceived += (_, _) => { };
        ssh.ErrorDataReceived += (_, _) => { };
        ssh.Start();
        ssh.BeginOutputReadLine();
        ssh.BeginErrorReadLine();

        using var wait = CancellationTokenSource.CreateLinkedTokenSource(ct);
        wait.CancelAfter(TimeSpan.FromSeconds(timeoutSeconds));
        try
        {
            await ssh.WaitForExitAsync(wait.Token);
        }
        catch (OperationCanceledException)
        {
            KillQuietly(ssh);
            if (ct.IsCancellationRequested)
                throw;
            return false;
        }
        return ssh.ExitCode == 0;
    }

    private static string ShellQuote(string value) =>
        "'" + value.Replace("'", "'\\''") + "'";

    public VmDto? Start(string id)
    {
        VmRuntime runtime;
        lock (m_lock)
        {
            var record = m_records.FirstOrDefault(r => r.Id == id);
            if (record is null)
                return null;
            runtime = m_runtimes[record.Id];
            if (runtime.Deleting)
                return null;
            runtime.ParkPending = false;
            if (runtime.Proc is { HasExited: false } || runtime.Status == "starting")
                return BundleLocked(record);
            runtime.StartCts?.Cancel();
            runtime.StartCts?.Dispose();
            var cts = new CancellationTokenSource();
            runtime.StartCts = cts;
            runtime.StartGeneration++;
            var generation = runtime.StartGeneration;
            runtime.Status = "starting";
            runtime.Error = null;
            runtime.StartedUtc = DateTime.UtcNow;
            runtime.History.Clear();
            runtime.StatsCts?.Cancel();
            runtime.GuestSampleUtc = default;
            runtime.GuestMemMb = 0;
            runtime.GuestCpuPct = 0;
            runtime.GuestInMbps = 0;
            runtime.GuestOutMbps = 0;
            runtime.GuestCpuTotal = 0;
            runtime.GuestCpuIdle = 0;
            runtime.GuestNetRx = 0;
            runtime.GuestNetTx = 0;
            runtime.LaunchTask = Task.Run(() => LaunchAsync(runtime, generation, cts.Token));
            return BundleLocked(record);
        }
    }

    public VmDto? Stop(string id)
    {
        VmRuntime? runtime;
        lock (m_lock)
        {
            var record = m_records.FirstOrDefault(r => r.Id == id);
            if (record is null)
                return null;
            runtime = m_runtimes[record.Id];
            if (runtime.Deleting)
                return null;
            runtime.ParkPending = false;
            runtime.StartCts?.Cancel();
            runtime.StatsCts?.Cancel();
            var proc = runtime.Proc;
            if (proc is null)
            {
                runtime.Status = "stopped";
                runtime.StartedUtc = null;
                runtime.History.Clear();
                return BundleLocked(record);
            }
            runtime.Status = "stopping";
            _ = Task.Run(() => StopAsync(runtime, proc));
            return BundleLocked(record);
        }
    }

    public bool Delete(string id)
    {
        VmRuntime? runtime;
        Process? proc;
        Task? launch;
        lock (m_lock)
        {
            var record = m_records.FirstOrDefault(r => r.Id == id);
            if (record is null)
                return false;
            if (!m_runtimes.TryGetValue(record.Id, out runtime))
            {
                m_records.Remove(record);
                SaveRecordsLocked();
                DeleteMachineFolder(record.Id);
                return true;
            }
            if (runtime.Deleting)
                return true;

            runtime.Deleting = true;
            runtime.Status = "stopping";
            runtime.StartCts?.Cancel();
            runtime.StatsCts?.Cancel();
            runtime.StartGeneration++;
            proc = runtime.Proc;
            launch = runtime.LaunchTask;
        }

        try
        {
            if (IsAlive(proc))
            {
                try
                {
                    runtime.View?.Write(StopOffset, 1);
                }
                catch
                {
                }

                try
                {
                    proc!.WaitForExit(3000);
                }
                catch
                {
                }

                if (IsAlive(proc))
                    KillQuietly(proc);
            }

            if (launch is { IsCompleted: false })
            {
                try
                {
                    launch.Wait(TimeSpan.FromSeconds(5));
                }
                catch
                {
                }
            }

            if (IsAlive(proc))
            {
                KillQuietly(proc);
                try
                {
                    proc!.WaitForExit(3000);
                }
                catch
                {
                }
            }
        }
        finally
        {
            lock (m_lock)
            {
                if (m_runtimes.TryGetValue(id, out var current))
                {
                    try
                    {
                        if (current.Proc is { HasExited: false })
                            KillQuietly(current.Proc);
                    }
                    catch
                    {
                    }
                    CleanupLocked(current);
                    m_runtimes.Remove(id);
                }
                m_records.RemoveAll(r => r.Id == id);
                SaveRecordsLocked();
            }

            DeleteMachineFolder(id);
        }

        return true;
    }

    private void DeleteMachineFolder(string id)
    {
        try
        {
            var dir = m_paths.MachineDir(id);
            if (Directory.Exists(dir))
                Directory.Delete(dir, true);
        }
        catch
        {
        }
    }

    public void StopAll()
    {
        List<VmRuntime> live;
        List<Task> launches;
        lock (m_lock)
        {
            foreach (var runtime in m_runtimes.Values)
            {
                runtime.StartCts?.Cancel();
                runtime.StatsCts?.Cancel();
                runtime.StartGeneration++;
            }
            live = m_runtimes.Values.Where(r => r.Proc is { HasExited: false }).ToList();
            launches = m_runtimes.Values
                .Select(r => r.LaunchTask)
                .Where(t => t is { IsCompleted: false })
                .Cast<Task>()
                .ToList();
        }

        try
        {
            Task.WaitAll(launches.ToArray(), 2000);
        }
        catch
        {
        }

        var syncs = new List<Task>();
        foreach (var runtime in live)
        {
            var port = runtime.SshPort;
            if (port > 0 && File.Exists(m_sshKeyPath))
                syncs.Add(RunGuestCommandAsync(
                    port, m_sshKeyPath, "sync", 2, CancellationToken.None));
        }
        if (syncs.Count > 0)
        {
            try
            {
                Task.WaitAll(syncs.ToArray(), 2500);
            }
            catch
            {
            }
        }

        foreach (var runtime in live)
        {
            try
            {
                runtime.View?.Write(StopOffset, 1);
            }
            catch
            {
            }
        }

        foreach (var runtime in live)
        {
            try
            {
                if (!runtime.Proc!.WaitForExit(3000))
                    runtime.Proc.Kill(true);
            }
            catch
            {
            }
        }

        lock (m_lock)
        {
            foreach (var runtime in m_runtimes.Values)
                CleanupLocked(runtime);
        }
    }

    public void IdleAll()
    {
        lock (m_lock)
        {
            foreach (var runtime in m_runtimes.Values)
            {
                if (runtime.Deleting || runtime.ParkPending || runtime.Status != "running")
                    continue;
                var proc = runtime.Proc;
                if (proc is null || proc.HasExited)
                    continue;
                runtime.ParkPending = true;
                runtime.StartCts?.Cancel();
                runtime.StatsCts?.Cancel();
                runtime.Status = "stopping";
                _ = Task.Run(() => StopAsync(runtime, proc));
            }
        }
    }

    public void ResumeAll()
    {
        List<string> ids;
        lock (m_lock)
        {
            ids = m_runtimes.Values
                .Where(runtime => !runtime.Deleting && runtime.Status == "idle")
                .Select(runtime => runtime.Record.Id)
                .ToList();
        }
        foreach (var id in ids)
            _ = Start(id);
    }

    internal VmRuntime? FindRuntime(string id)
    {
        lock (m_lock)
        {
            if (!m_runtimes.TryGetValue(id, out var runtime))
                return null;
            return runtime.View is null ? null : runtime;
        }
    }

    public byte[]? FrameJpeg(string id)
    {
        var runtime = FindRuntime(id);
        return runtime?.GetJpeg();
    }

    public bool SendInput(string id, JsonElement message)
    {
        VmRuntime? runtime;
        lock (m_lock)
        {
            if (!m_runtimes.TryGetValue(id, out runtime) || runtime.Deleting)
                return false;
        }

        var type = message.TryGetProperty("type", out var typeEl) ? typeEl.GetString() : null;
        if (type is null)
            return false;

        var packet = new byte[16];
        using (var ms = new MemoryStream(packet))
        using (var bw = new BinaryWriter(ms))
        {
            switch (type)
            {
                case "move":
                    bw.Write((uint)1);
                    bw.Write(ReadInt(message, "dx"));
                    bw.Write(ReadInt(message, "dy"));
                    bw.Write(ReadInt(message, "buttons"));
                    break;

                case "button":
                    bw.Write((uint)2);
                    bw.Write(0);
                    bw.Write(0);
                    bw.Write(ReadInt(message, "buttons"));
                    break;

                case "key":
                {
                    var vk = ReadInt(message, "vk");
                    var down = message.TryGetProperty("down", out var d) && d.GetBoolean();
                    var vsc = MapVirtualKey((uint)vk, 4);
                    if (vsc == 0)
                        return false;
                    var sc = (int)(vsc & 0xFF);
                    if ((vsc & 0xE000) != 0 || (vsc & 0xFF00) == 0xE000)
                        sc |= 0x100;
                    bw.Write((uint)3);
                    bw.Write(sc);
                    bw.Write(down ? 1 : 0);
                    bw.Write(0);
                    break;
                }

                case "release":
                    bw.Write((uint)4);
                    bw.Write(0);
                    bw.Write(0);
                    bw.Write(0);
                    break;

                default:
                    return false;
            }
        }

        lock (runtime.InputLock)
        {
            for (var attempt = 0; attempt < 2; attempt++)
            {
                try
                {
                    if (runtime.Input is null)
                    {
                        if (runtime.Proc is null || runtime.Proc.HasExited ||
                            string.IsNullOrWhiteSpace(runtime.Token))
                            return false;

                        var reopened = new NamedPipeClientStream(
                            ".", "SoilInput_" + runtime.Token,
                            PipeDirection.Out, PipeOptions.Asynchronous);
                        try
                        {
                            reopened.Connect(300);
                            runtime.Input = reopened;
                        }
                        catch
                        {
                            reopened.Dispose();
                            return false;
                        }
                    }

                    runtime.Input.Write(packet, 0, packet.Length);
                    runtime.Input.Flush();
                    return true;
                }
                catch (IOException)
                {
                    // The input pipe can disconnect while a VM is still alive.
                    try { runtime.Input?.Dispose(); } catch { }
                    runtime.Input = null;
                }
                catch
                {
                    return false;
                }
            }
            return false;
        }
    }

    internal async Task StreamAsync(VmRuntime runtime, HttpContext ctx, CancellationToken ct)
    {
        ctx.Response.StatusCode = 200;
        ctx.Response.ContentType = "multipart/x-mixed-replace; boundary=frame";
        ctx.Response.Headers.CacheControl = "no-store, no-cache, must-revalidate";
        ctx.Response.Headers.Pragma = "no-cache";

        var lastSent = -1;
        while (!ct.IsCancellationRequested)
        {
            var (seq, jpeg) = runtime.GetJpegWithSequence();
            if (jpeg is not null && seq != lastSent)
            {
                lastSent = seq;
                await ctx.Response.WriteAsync("--frame\r\n", ct);
                await ctx.Response.WriteAsync("Content-Type: image/jpeg\r\n", ct);
                await ctx.Response.WriteAsync($"Content-Length: {jpeg.Length}\r\n\r\n", ct);
                await ctx.Response.Body.WriteAsync(jpeg, ct);
                await ctx.Response.WriteAsync("\r\n", ct);
                await ctx.Response.Body.FlushAsync(ct);
            }
            await Task.Delay(100, ct);
        }
    }

    private async Task LaunchAsync(VmRuntime runtime, int generation, CancellationToken ct)
    {
        Process? proc = null;
        MemoryMappedFile? map = null;
        MemoryMappedViewAccessor? view = null;
        NamedPipeClientStream? pipe = null;
        try
        {
            ct.ThrowIfCancellationRequested();
            if (!File.Exists(m_vmhost))
                throw new FileNotFoundException("The VM engine is missing.", m_vmhost);
            foreach (var image in new[] { "vmlinuz64", "core2.gz", "soil.squashfs" })
            {
                if (!File.Exists(Path.Combine(m_imagesDir, image)))
                    throw new FileNotFoundException($"The VM image '{image}' is missing.", m_imagesDir);
            }

            var token = "v" + Guid.NewGuid().ToString("N");
            var psi = new ProcessStartInfo
            {
                FileName = m_vmhost,
                WorkingDirectory = m_baseDir,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            psi.ArgumentList.Add("--kernel");
            psi.ArgumentList.Add(Path.Combine(m_imagesDir, "vmlinuz64"));
            psi.ArgumentList.Add("--initrd");
            var initrd = Path.Combine(m_imagesDir, "core2.gz");
            if (!string.IsNullOrEmpty(m_sshKeyPath) &&
                File.Exists(m_sshKeyPath + ".pub"))
                initrd = GuestInitrd.Prepare(initrd, m_paths.MachineDir(runtime.Record.Id),
                                            m_sshKeyPath + ".pub");
            psi.ArgumentList.Add(initrd);
            psi.ArgumentList.Add("--disk");
            psi.ArgumentList.Add(Path.Combine(m_imagesDir, "soil.squashfs"));
            var dataGb = runtime.Record.DiskGb > 0 ? runtime.Record.DiskGb : 40;
            psi.ArgumentList.Add("--data-disk");
            psi.ArgumentList.Add(m_paths.MachineDiskPath(runtime.Record.Id));
            psi.ArgumentList.Add("--data-gb");
            psi.ArgumentList.Add(dataGb.ToString(CultureInfo.InvariantCulture));
            psi.ArgumentList.Add("--ram");
            psi.ArgumentList.Add(runtime.Record.RamMb.ToString());
            Directory.CreateDirectory(m_paths.MachineLogDir(runtime.Record.Id));
            psi.ArgumentList.Add("--cmdline");
            psi.ArgumentList.Add("console=ttyS0 quiet loglevel=3 acpi=off noapic nolapic mitigations=off nozswap");
            psi.ArgumentList.Add("--log");
            psi.ArgumentList.Add(m_paths.MachineLogPath(runtime.Record.Id));
            psi.ArgumentList.Add("--no-ui");
            var sshPort = 0;
            var mcpPort = 0;
            if (!string.IsNullOrEmpty(m_sshKeyPath) && File.Exists(m_sshKeyPath))
            {
                sshPort = AllocateLoopbackPort();
                mcpPort = AllocateLoopbackPort();
                psi.ArgumentList.Add("--ssh-port");
                psi.ArgumentList.Add(sshPort.ToString(CultureInfo.InvariantCulture));
                psi.ArgumentList.Add("--mcp-port");
                psi.ArgumentList.Add(mcpPort.ToString(CultureInfo.InvariantCulture));
            }
            else
            {
                psi.ArgumentList.Add("--no-hostfwd");
            }
            psi.ArgumentList.Add("--ipc");
            psi.ArgumentList.Add(token);

            proc = new Process { StartInfo = psi, EnableRaisingEvents = true };
            proc.OutputDataReceived += (_, _) => { };
            proc.ErrorDataReceived += (_, _) => { };
            proc.Start();
            proc.BeginOutputReadLine();
            proc.BeginErrorReadLine();

            var canceled = false;
            lock (m_lock)
            {
                if (ct.IsCancellationRequested || runtime.StartGeneration != generation)
                {
                    canceled = true;
                }
                else
                {
                    runtime.Proc = proc;
                    runtime.Token = token;
                    runtime.SshPort = sshPort;
                    runtime.McpPort = mcpPort;
                }
            }
            if (canceled)
            {
                KillQuietly(proc);
                proc.Dispose();
                return;
            }
            ct.ThrowIfCancellationRequested();

            for (var i = 0; i < 240; i++)
            {
                ct.ThrowIfCancellationRequested();
                if (proc.HasExited)
                    break;
                try
                {
                    map = MemoryMappedFile.OpenExisting(
                        "Local\\SoilFb_" + token, MemoryMappedFileRights.ReadWrite);
                    break;
                }
                catch (FileNotFoundException)
                {
                    await Task.Delay(50, ct);
                }
            }
            if (map is null)
                throw new InvalidOperationException("The VM engine did not create its display channel.");

            view = map.CreateViewAccessor(0, SharedSize, MemoryMappedFileAccess.ReadWrite);

            pipe = new NamedPipeClientStream(
                ".", "SoilInput_" + token, PipeDirection.Out, PipeOptions.Asynchronous);
            try
            {
                await pipe.ConnectAsync(10000, ct);
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch
            {
                throw new InvalidOperationException("The VM engine did not create its input channel.");
            }

            lock (m_lock)
            {
                if (ct.IsCancellationRequested || runtime.StartGeneration != generation)
                    throw new OperationCanceledException(ct);
                runtime.Map = map;
                runtime.View = view;
                runtime.Input = pipe;
            }
            var bootView = view!;
            map = null;
            view = null;
            pipe = null;

            await EnsureGuestGuiReadyAsync(runtime, bootView, proc, ct);
            if (proc.HasExited)
                throw new InvalidOperationException(
                    $"The VM engine exited during startup (code {proc.ExitCode}). " +
                    $"Check {m_paths.MachineLogPath(runtime.Record.Id)}.");

            ct.ThrowIfCancellationRequested();
            var running = false;
            lock (m_lock)
            {
                if (!ct.IsCancellationRequested &&
                    runtime.StartGeneration == generation &&
                    ReferenceEquals(runtime.Proc, proc) &&
                    !proc.HasExited &&
                    runtime.Status == "starting")
                {
                    runtime.Status = "running";
                    running = true;
                }
            }

            if (!running)
            {
                if (proc.HasExited)
                {
                    lock (m_lock)
                    {
                        if (ReferenceEquals(runtime.Proc, proc))
                        {
                            runtime.Status = "stopped";
                            runtime.StartedUtc = null;
                            runtime.History.Clear();
                            CleanupLocked(runtime);
                        }
                    }
                }
                return;
            }

            StartGuestStats(runtime);
            if (RestoreNativeApps is { } restore)
                _ = Task.Run(async () =>
                {
                    try { await restore(runtime.Record.Id, ct); }
                    catch (OperationCanceledException) { }
                    catch (Exception) { /* A failed restore must not crash or stop the VM. */ }
                });
            _ = Task.Run(() => MonitorExitAsync(runtime, proc));
        }
        catch (OperationCanceledException)
        {
            KillQuietly(proc);
            DisposeQuietly(map);
            DisposeQuietly(view);
            DisposeQuietly(pipe);
            var owned = false;
            lock (m_lock)
            {
                owned = ReferenceEquals(runtime.Proc, proc);
                if (runtime.StartGeneration == generation && owned)
                {
                    CleanupLocked(runtime);
                    if (runtime.Status is "starting" or "running" or "stopping")
                    {
                        runtime.Status = "stopped";
                        runtime.StartedUtc = null;
                        runtime.History.Clear();
                    }
                }
            }
            if (!owned)
                proc?.Dispose();
        }
        catch (Exception ex)
        {
            KillQuietly(proc);
            DisposeQuietly(map);
            DisposeQuietly(view);
            DisposeQuietly(pipe);
            var owned = false;
            lock (m_lock)
            {
                owned = ReferenceEquals(runtime.Proc, proc);
                if (runtime.StartGeneration == generation)
                {
                    if (owned)
                        CleanupLocked(runtime);
                    if (runtime.Status is "starting" or "running")
                    {
                        runtime.Status = "stopped";
                        runtime.Error = ex.Message;
                        runtime.StartedUtc = null;
                        runtime.History.Clear();
                    }
                }
            }
            if (!owned)
                proc?.Dispose();
        }
    }

    private async Task MonitorExitAsync(VmRuntime runtime, Process proc)
    {
        try
        {
            await proc.WaitForExitAsync();
        }
        catch
        {
        }

        lock (m_lock)
        {
            if (ReferenceEquals(runtime.Proc, proc))
            {
                if (runtime.Status is "running" or "starting")
                    runtime.Error = $"The VM engine exited unexpectedly (code {proc.ExitCode}). " +
                                    $"Check {m_paths.MachineLogPath(runtime.Record.Id)}.";
                runtime.Status = ConsumeParkLocked(runtime);
                runtime.StartedUtc = null;
                runtime.History.Clear();
                CleanupLocked(runtime);
            }
        }
    }

    private static string ConsumeParkLocked(VmRuntime runtime)
    {
        if (!runtime.ParkPending)
            return "stopped";
        runtime.ParkPending = false;
        return "idle";
    }

    private async Task StopAsync(VmRuntime runtime, Process proc)
    {
        try
        {
            var port = runtime.SshPort;
            if (port > 0 && File.Exists(m_sshKeyPath))
                await RunGuestCommandAsync(
                    port, m_sshKeyPath, "sync", 2, CancellationToken.None);
        }
        catch
        {
        }

        try
        {
            lock (m_lock)
            {
                if (ReferenceEquals(runtime.Proc, proc))
                    runtime.View?.Write(StopOffset, 1);
            }
        }
        catch
        {
        }

        try
        {
            if (!await WaitForExitAsync(proc, TimeSpan.FromSeconds(20)))
                proc.Kill(true);
        }
        catch
        {
        }

        lock (m_lock)
        {
            if (ReferenceEquals(runtime.Proc, proc))
            {
                runtime.Status = ConsumeParkLocked(runtime);
                runtime.StartedUtc = null;
                runtime.History.Clear();
                CleanupLocked(runtime);
            }
        }
    }

    private async Task StatsLoopAsync()
    {
        while (true)
        {
            await Task.Delay(2000);
            lock (m_lock)
            {
                foreach (var runtime in m_runtimes.Values)
                {
                    var proc = runtime.Proc;
                    if (proc is null || runtime.Status != "running")
                        continue;
                    try
                    {
                        var now = DateTime.UtcNow;
                        var fresh = runtime.GuestSampleUtc != default &&
                                    (now - runtime.GuestSampleUtc).TotalSeconds < 8;
                        var cpuPct = fresh ? runtime.GuestCpuPct : 0;
                        var mem = fresh ? runtime.GuestMemMb : 0;
                        var inMbps = fresh ? runtime.GuestInMbps : 0;
                        var outMbps = fresh ? runtime.GuestOutMbps : 0;

                        runtime.History.AddLast(new StatPointInternal(
                            Math.Round(cpuPct, 1),
                            Math.Round(mem, 1),
                            Math.Round(inMbps, 1),
                            Math.Round(outMbps, 1)));
                        while (runtime.History.Count > 24)
                            runtime.History.RemoveFirst();
                    }
                    catch
                    {
                    }
                }
            }
        }
    }

    private VmDto BundleLocked(VmRecord record)
    {
        var runtime = m_runtimes[record.Id];
        if (runtime.Proc is { HasExited: true })
        {
            runtime.Status = ConsumeParkLocked(runtime);
            runtime.StartedUtc = null;
        }

        var uptime = runtime.StartedUtc is null ? 0 :
            (long)Math.Max(0, (DateTime.UtcNow - runtime.StartedUtc.Value).TotalSeconds);
        var series = runtime.History.ToList();

        var app = string.Equals(record.Kind, "app", StringComparison.OrdinalIgnoreCase)
            ? new AppDto(
                record.RepoUrl, record.AppStatus ?? "queued", record.AppError,
                record.AppHostUrl, record.AppGuestUrl, record.AppLog)
            : null;

        TunnelDto? tunnel = null;
        if (string.Equals(record.Kind, "agent", StringComparison.OrdinalIgnoreCase) &&
            runtime.Status == "running" && runtime.SshPort > 0 && runtime.McpPort > 0 &&
            !string.IsNullOrWhiteSpace(m_sshKeyPath) && File.Exists(m_sshKeyPath))
        {
            var command = "ssh -i " + QuoteArgument(m_sshKeyPath) +
                          " -p " + runtime.SshPort.ToString(CultureInfo.InvariantCulture) +
                          " -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL tc@127.0.0.1";
            var mcpCommand = "ssh -i " + QuoteArgument(m_sshKeyPath) +
                             " -p " + runtime.McpPort.ToString(CultureInfo.InvariantCulture) +
                             " -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL tc@127.0.0.1";
            tunnel = new TunnelDto(
                "127.0.0.1", runtime.SshPort, runtime.McpPort, "tc", m_sshKeyPath,
                command, mcpCommand);
        }

        var agent = string.Equals(record.Kind, "agent", StringComparison.OrdinalIgnoreCase)
            ? new AgentDto(
                record.AgentStatus ?? "queued", record.AgentError, record.AgentLog,
                record.AgentClaudeVersion, record.AgentCodexVersion, tunnel)
            : null;

        return new VmDto(
            record.Id, record.Name, runtime.Status, record.RamMb, record.Cpu,
            record.DiskGb, record.Os, uptime, runtime.Error,
            series.Select(p => new StatDto(p.CpuPercent, p.MemMb, p.InMbps, p.OutMbps)).ToList(),
            app, record.Kind, agent);
    }

    private static string QuoteArgument(string value) =>
        "\"" + value.Replace("\"", "\\\"", StringComparison.Ordinal) + "\"";

    private void SyncProcessStatesLocked()
    {
        foreach (var runtime in m_runtimes.Values)
        {
            if (runtime.Proc is { HasExited: true } && runtime.Status != "stopped")
            {
                runtime.Status = ConsumeParkLocked(runtime);
                runtime.StartedUtc = null;
                runtime.History.Clear();
                CleanupLocked(runtime);
            }
        }
    }

    private void Cleanup(VmRuntime runtime)
    {
        lock (m_lock)
            CleanupLocked(runtime);
    }

    private void CleanupLocked(VmRuntime runtime)
    {
        try
        {
            runtime.StatsCts?.Cancel();
        }
        catch
        {
        }
        try
        {
            runtime.StatsProcess?.Kill(true);
        }
        catch
        {
        }
        runtime.StatsProcess = null;
        runtime.StatsCts = null;
        runtime.StatsTask = null;
        runtime.SshPort = 0;
        runtime.McpPort = 0;
        runtime.GuestSampleUtc = default;
        runtime.GuestMemMb = 0;
        runtime.GuestCpuPct = 0;
        runtime.GuestInMbps = 0;
        runtime.GuestOutMbps = 0;
        runtime.GuestCpuTotal = 0;
        runtime.GuestCpuIdle = 0;
        runtime.GuestNetRx = 0;
        runtime.GuestNetTx = 0;

        lock (runtime.InputLock)
        {
            try
            {
                runtime.Input?.Dispose();
            }
            catch
            {
            }
            runtime.Input = null;
        }

        lock (runtime.FrameLock)
        {
            try
            {
                runtime.View?.Dispose();
            }
            catch
            {
            }
            try
            {
                runtime.Map?.Dispose();
            }
            catch
            {
            }
            runtime.View = null;
            runtime.Map = null;
            runtime.CachedJpeg = null;
            runtime.CachedSeq = -1;
        }

        try
        {
            runtime.Proc?.Dispose();
        }
        catch
        {
        }
        try
        {
            runtime.StartCts?.Dispose();
        }
        catch
        {
        }
        runtime.Proc = null;
        runtime.StartCts = null;
        runtime.LaunchTask = null;
    }

    private async Task EnsureGuestGuiReadyAsync(
        VmRuntime runtime,
        MemoryMappedViewAccessor bootView,
        Process proc,
        CancellationToken ct)
    {
        int port;
        string key;
        lock (m_lock)
        {
            port = runtime.SshPort;
            key = m_sshKeyPath;
        }

        if (port <= 0 || string.IsNullOrWhiteSpace(key) || !File.Exists(key))
        {
            var boot = Stopwatch.StartNew();
            while (boot.Elapsed < TimeSpan.FromSeconds(90))
            {
                ct.ThrowIfCancellationRequested();
                if (proc.HasExited || HasVisibleContent(bootView))
                    break;
                await Task.Delay(400, ct);
            }
            await Task.Delay(TimeSpan.FromSeconds(25), ct);
            return;
        }

        const string probe =
            "DISPLAY=:0 xset q >/dev/null 2>&1 && " +
            "ps w | grep -q '[p]cmanfm' && ps w | grep -q '[f]luxbox'";
        var ready = Stopwatch.StartNew();
        while (ready.Elapsed < TimeSpan.FromSeconds(150))
        {
            ct.ThrowIfCancellationRequested();
            if (proc.HasExited)
                return;
            if (await RunGuestCommandAsync(port, key, probe, 8, ct))
            {
                await Task.Delay(700, ct);
                if (await RunGuestCommandAsync(port, key, probe, 8, ct))
                    return;
            }
            await Task.Delay(1000, ct);
        }
        throw new TimeoutException("The desktop did not finish starting.");
    }

    private static bool HasVisibleContent(MemoryMappedViewAccessor view)
    {
        var buffer = new byte[64];
        for (var y = 0; y < FrameHeight; y += 128)
        {
            for (var x = 0; x < FrameWidth; x += 256)
            {
                var offset = SharedFrameOffset + y * FrameStride + x * 4;
                view.ReadArray(offset, buffer, 0, buffer.Length);
                for (var i = 0; i < buffer.Length; i += 4)
                {
                    if (buffer[i] != 0 || buffer[i + 1] != 0 || buffer[i + 2] != 0)
                        return true;
                }
            }
        }
        return false;
    }

    private List<VmRecord> LoadRecords()
    {
        var records = new List<VmRecord>();
        var hasIndex = File.Exists(m_paths.MachinesIndexPath);
        var loadedIndex = false;
        if (hasIndex)
        {
            try
            {
                var store = JsonSerializer.Deserialize<StoreFile>(
                    File.ReadAllText(m_paths.MachinesIndexPath), JsonOptions.Web);
                if (store?.Vms is not null)
                {
                    records.AddRange(store.Vms);
                    loadedIndex = true;
                }
            }
            catch
            {
            }
        }

        if (!loadedIndex)
        {
            var legacy = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "Boxify");
            var oldStore = Path.Combine(legacy, "vms.json");
            try
            {
                if (File.Exists(oldStore))
                {
                    var store = JsonSerializer.Deserialize<StoreFile>(
                        File.ReadAllText(oldStore), JsonOptions.Web);
                    if (store?.Vms is not null)
                        records.AddRange(store.Vms);
                }
            }
            catch
            {
            }

            foreach (var record in records)
            {
                if (!SoilPaths.IsSafeId(record.Id))
                    continue;
                m_paths.EnsureMachine(record.Id);
                var oldLog = Path.Combine(legacy, "logs", record.Id + ".log");
                var newLog = m_paths.MachineLogPath(record.Id);
                try
                {
                    if (File.Exists(oldLog) && !File.Exists(newLog))
                        File.Copy(oldLog, newLog);
                }
                catch
                {
                }
            }
        }

        records = records
            .Where(record => SoilPaths.IsSafeId(record.Id))
            .GroupBy(record => record.Id, StringComparer.OrdinalIgnoreCase)
            .Select(group => group.First())
            .ToList();

        foreach (var record in records)
            EnsureRecordFolder(record);

        if (!loadedIndex)
            WriteMachineIndex(records);

        return records;
    }

    private void EnsureRecordFolder(VmRecord record)
    {
        if (!SoilPaths.IsSafeId(record.Id))
            return;
        m_paths.EnsureMachine(record.Id);
        var machineFile = m_paths.MachineConfigPath(record.Id);
        if (!File.Exists(machineFile))
            WriteJson(machineFile, record);
    }

    private void WriteMachineRecordLocked(VmRecord record)
    {
        if (!SoilPaths.IsSafeId(record.Id))
            return;
        m_paths.EnsureMachine(record.Id);
        WriteJson(m_paths.MachineConfigPath(record.Id), record);
    }

    private void SaveRecordsLocked()
    {
        try
        {
            foreach (var record in m_records)
                WriteMachineRecordLocked(record);
            WriteMachineIndex(m_records);
        }
        catch
        {
        }
    }

    private void WriteMachineIndex(IReadOnlyList<VmRecord> records)
    {
        try
        {
            WriteJson(m_paths.MachinesIndexPath, new StoreFile { Vms = records.ToList() });
        }
        catch
        {
        }
    }

    private static void WriteJson(string path, object value)
    {
        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir))
            Directory.CreateDirectory(dir);
        var temp = path + ".tmp";
        File.WriteAllText(temp, JsonSerializer.Serialize(value, JsonOptions.Web));
        File.Move(temp, path, true);
    }

    private string ResolveImagesDir(string baseDir)
    {
        var installed = Path.Combine(baseDir, "images");
        if (Directory.Exists(installed))
            return installed;
        return m_paths.ImagesDir;
    }

    private static int ReadInt(JsonElement element, string name)
        => element.TryGetProperty(name, out var property) && property.TryGetInt32(out var value)
            ? value
            : 0;

    private static int AllocateLoopbackPort()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        try
        {
            return ((IPEndPoint)listener.LocalEndpoint).Port;
        }
        finally
        {
            listener.Stop();
        }
    }

    private static string FindSshExecutable() => FindExecutable("ssh.exe");

    private static string FindExecutable(string name)
    {
        var bundled = Path.Combine(Environment.SystemDirectory, "OpenSSH", name);
        if (File.Exists(bundled))
            return bundled;
        foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "")
                     .Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            try
            {
                var candidate = Path.Combine(dir.Trim(), name);
                if (File.Exists(candidate))
                    return candidate;
            }
            catch
            {
            }
        }
        return name;
    }

    private string PrepareSshKey()
    {
        try
        {
            Directory.CreateDirectory(m_paths.SshDir);
            var path = m_paths.SshKeyPath;
            using var stream = typeof(VmManager).Assembly
                .GetManifestResourceStream("Soil.keys.id_ed25519");
            if (stream is not null)
            {
                using var memory = new MemoryStream();
                stream.CopyTo(memory);
                var bytes = memory.ToArray();
                if (!File.Exists(path))
                    File.WriteAllBytes(path, bytes);
            }

            if (!File.Exists(path))
            {
                var legacy = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "Boxify", "ssh", "id_ed25519");
                if (File.Exists(legacy))
                    File.Copy(legacy, path);
            }

            if (!File.Exists(path))
            {
                // Fresh installations have no private key bundled. Generate a
                // per-user key; only its public half will enter guest images.
                var generator = new ProcessStartInfo
                {
                    FileName = FindExecutable("ssh-keygen.exe"),
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true
                };
                foreach (var arg in new[] { "-q", "-t", "ed25519", "-f", path,
                                             "-N", "", "-C", "soil-local" })
                    generator.ArgumentList.Add(arg);
                using var created = Process.Start(generator);
                if (created is null || !created.WaitForExit(15000) || created.ExitCode != 0)
                    return "";
            }
            RestrictKeyPermissions(path);
            if (!File.Exists(path + ".pub"))
            {
                var exporter = new ProcessStartInfo
                {
                    FileName = FindExecutable("ssh-keygen.exe"),
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true
                };
                exporter.ArgumentList.Add("-y");
                exporter.ArgumentList.Add("-f");
                exporter.ArgumentList.Add(path);
                using var export = Process.Start(exporter);
                if (export is null || !export.WaitForExit(10000) || export.ExitCode != 0)
                    return "";
                File.WriteAllText(path + ".pub", export.StandardOutput.ReadToEnd());
            }
            return path;
        }
        catch
        {
            return "";
        }
    }

    private static void RestrictKeyPermissions(string path)
    {
        try
        {
            var user = WindowsIdentity.GetCurrent().Name;
            var psi = new ProcessStartInfo
            {
                FileName = Path.Combine(Environment.SystemDirectory, "icacls.exe"),
                UseShellExecute = false,
                CreateNoWindow = true
            };
            psi.ArgumentList.Add(path);
            psi.ArgumentList.Add("/inheritance:r");
            psi.ArgumentList.Add("/grant:r");
            psi.ArgumentList.Add(user + ":F");
            using var proc = Process.Start(psi);
            proc?.WaitForExit(3000);
        }
        catch
        {
        }
    }

    private async Task KickFirstBootSnapshotAsync(VmRuntime runtime, string snapPath, Process proc)
    {
        try
        {
            for (var i = 0; i < 25 && !proc.HasExited; i++)
                await Task.Delay(200);
            if (proc.HasExited || File.Exists(snapPath) || runtime.View is null)
                return;
            var view = runtime.View;
            var path = System.Text.Encoding.ASCII.GetBytes(snapPath);
            if (path.Length == 0 || path.Length >= CmdPathBytes)
                return;
            var buffer = new byte[path.Length + 1];
            path.CopyTo(buffer, 0);
            view.WriteArray(CmdPathOffset, buffer, 0, buffer.Length);
            view.Write(CmdHaveOffset, path.Length);
            view.Write(CmdOffset, 1);
            for (var i = 0; i < 60; i++)
            {
                await Task.Delay(250);
                if (proc.HasExited)
                    return;
                if (view.ReadInt32(CmdOffset) == 2)
                {
                    view.Write(CmdOffset, 0);
                    return;
                }
            }
        }
        catch
        {
        }
    }

    private void StartGuestStats(VmRuntime runtime)
    {
        int port;
        lock (m_lock)
        {
            port = runtime.SshPort;
            runtime.StatsCts?.Cancel();
            runtime.StatsCts = null;
        }
        if (port <= 0 || string.IsNullOrEmpty(m_sshKeyPath) || !File.Exists(m_sshKeyPath))
            return;

        var cts = new CancellationTokenSource();
        lock (m_lock)
            runtime.StatsCts = cts;
        runtime.StatsTask = Task.Run(() => GuestStatsLoopAsync(runtime, port, cts.Token));
    }

    private async Task GuestStatsLoopAsync(VmRuntime runtime, int port, CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            Process? ssh = null;
            try
            {
                var psi = new ProcessStartInfo
                {
                    FileName = m_sshExe,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true
                };
                psi.ArgumentList.Add("-i");
                psi.ArgumentList.Add(m_sshKeyPath);
                psi.ArgumentList.Add("-p");
                psi.ArgumentList.Add(port.ToString(CultureInfo.InvariantCulture));
                psi.ArgumentList.Add("-o");
                psi.ArgumentList.Add("StrictHostKeyChecking=no");
                psi.ArgumentList.Add("-o");
                psi.ArgumentList.Add("UserKnownHostsFile=NUL");
                psi.ArgumentList.Add("-o");
                psi.ArgumentList.Add("BatchMode=yes");
                psi.ArgumentList.Add("-o");
                psi.ArgumentList.Add("ConnectTimeout=3");
                psi.ArgumentList.Add("-o");
                psi.ArgumentList.Add("ServerAliveInterval=10");
                psi.ArgumentList.Add("-o");
                psi.ArgumentList.Add("ServerAliveCountMax=2");
                psi.ArgumentList.Add("-o");
                psi.ArgumentList.Add("LogLevel=ERROR");
                psi.ArgumentList.Add("tc@127.0.0.1");
                psi.ArgumentList.Add(
                    "while :; do echo ===MEM; cat /proc/meminfo; echo ===CPU; head -1 /proc/stat; echo ===NET; cat /proc/net/dev; echo ===END; sleep 2; done");

                ssh = new Process { StartInfo = psi, EnableRaisingEvents = true };
                ssh.ErrorDataReceived += (_, _) => { };
                ssh.Start();
                ssh.BeginErrorReadLine();

                lock (m_lock)
                {
                    if (ct.IsCancellationRequested)
                    {
                        KillQuietly(ssh);
                        return;
                    }
                    runtime.StatsProcess = ssh;
                }

                await ReadGuestStatsAsync(runtime, ssh.StandardOutput, ct);
            }
            catch (OperationCanceledException)
            {
            }
            catch
            {
            }
            finally
            {
                lock (m_lock)
                {
                    if (ReferenceEquals(runtime.StatsProcess, ssh))
                        runtime.StatsProcess = null;
                }
                KillQuietly(ssh);
                ssh?.Dispose();
            }

            if (ct.IsCancellationRequested)
                break;
            try
            {
                await Task.Delay(3000, ct);
            }
            catch (OperationCanceledException)
            {
                break;
            }
        }
    }

    private async Task ReadGuestStatsAsync(
        VmRuntime runtime, StreamReader reader, CancellationToken ct)
    {
        var section = "";
        long total = 0, free = 0, buffers = 0, cached = 0, reclaim = 0;
        long cpuTotal = 0, cpuIdle = 0;
        long rx = 0, tx = 0;

        while (!ct.IsCancellationRequested)
        {
            string? line;
            try
            {
                line = await reader.ReadLineAsync(ct);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            if (line is null)
                break;
            line = line.TrimEnd('\r');

            if (line == "===MEM")
            {
                section = "mem";
                total = free = buffers = cached = reclaim = 0;
                continue;
            }
            if (line == "===CPU")
            {
                section = "cpu";
                continue;
            }
            if (line == "===NET")
            {
                section = "net";
                rx = tx = 0;
                continue;
            }
            if (line == "===END")
            {
                ApplyGuestStats(runtime, total, free, buffers, cached, reclaim,
                    cpuTotal, cpuIdle, rx, tx);
                section = "";
                continue;
            }

            if (section == "mem")
            {
                var colon = line.IndexOf(':');
                if (colon <= 0)
                    continue;
                var key = line[..colon];
                var rest = line[(colon + 1)..].Trim();
                var space = rest.IndexOf(' ');
                var number = space >= 0 ? rest[..space] : rest;
                if (!long.TryParse(number, NumberStyles.Integer, CultureInfo.InvariantCulture,
                        out var value))
                    continue;
                switch (key)
                {
                    case "MemTotal": total = value; break;
                    case "MemFree": free = value; break;
                    case "Buffers": buffers = value; break;
                    case "Cached": cached = value; break;
                    case "SReclaimable": reclaim = value; break;
                }
            }
            else if (section == "cpu" && line.StartsWith("cpu ", StringComparison.Ordinal))
            {
                var parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                long sum = 0;
                for (var i = 1; i < parts.Length && i <= 8; i++)
                {
                    if (long.TryParse(parts[i], NumberStyles.Integer,
                            CultureInfo.InvariantCulture, out var value))
                        sum += value;
                }
                var idle = 0L;
                if (parts.Length > 4 && long.TryParse(parts[4], NumberStyles.Integer,
                        CultureInfo.InvariantCulture, out var idleValue))
                    idle += idleValue;
                if (parts.Length > 5 && long.TryParse(parts[5], NumberStyles.Integer,
                        CultureInfo.InvariantCulture, out var iowaitValue))
                    idle += iowaitValue;
                cpuTotal = sum;
                cpuIdle = idle;
            }
            else if (section == "net")
            {
                var colon = line.IndexOf(':');
                if (colon <= 0)
                    continue;
                var name = line[..colon].Trim();
                if (name == "lo")
                    continue;
                var parts = line[(colon + 1)..]
                    .Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length < 9)
                    continue;
                if (long.TryParse(parts[0], NumberStyles.Integer,
                        CultureInfo.InvariantCulture, out var received))
                    rx += received;
                if (long.TryParse(parts[8], NumberStyles.Integer,
                        CultureInfo.InvariantCulture, out var sent))
                    tx += sent;
            }
        }
    }

    private void ApplyGuestStats(
        VmRuntime runtime, long totalKb, long freeKb, long buffersKb,
        long cachedKb, long reclaimKb, long cpuTotal, long cpuIdle, long rx, long tx)
    {
        var now = DateTime.UtcNow;
        var usedKb = totalKb - freeKb - buffersKb - cachedKb - reclaimKb;
        if (usedKb < 0)
            usedKb = 0;

        double cpuPct = 0, inMbps = 0, outMbps = 0;
        if (runtime.GuestSampleUtc != default)
        {
            var dt = (now - runtime.GuestSampleUtc).TotalSeconds;
            if (dt > 0.2)
            {
                var deltaTotal = cpuTotal - runtime.GuestCpuTotal;
                var deltaIdle = cpuIdle - runtime.GuestCpuIdle;
                if (deltaTotal > 0)
                    cpuPct = Math.Clamp(
                        (1.0 - (double)deltaIdle / deltaTotal) * 100.0, 0, 100);
                inMbps = Math.Max(0,
                    (rx - runtime.GuestNetRx) * 8.0 / dt / 1_000_000.0);
                outMbps = Math.Max(0,
                    (tx - runtime.GuestNetTx) * 8.0 / dt / 1_000_000.0);
            }
        }

        lock (m_lock)
        {
            runtime.GuestMemMb = totalKb > 0 ? usedKb / 1024.0 : 0;
            runtime.GuestCpuPct = cpuPct;
            runtime.GuestInMbps = inMbps;
            runtime.GuestOutMbps = outMbps;
            runtime.GuestCpuTotal = cpuTotal;
            runtime.GuestCpuIdle = cpuIdle;
            runtime.GuestNetRx = rx;
            runtime.GuestNetTx = tx;
            runtime.GuestSampleUtc = now;
        }
    }

    private static bool IsAlive(Process? proc)
    {
        try
        {
            return proc is { HasExited: false };
        }
        catch
        {
            return false;
        }
    }

    private static void KillQuietly(Process? proc)
    {
        try
        {
            if (proc is { HasExited: false })
                proc.Kill(true);
        }
        catch
        {
        }
    }

    private static void DisposeQuietly(IDisposable? disposable)
    {
        try
        {
            disposable?.Dispose();
        }
        catch
        {
        }
    }

    private static async Task<bool> WaitForExitAsync(Process proc, TimeSpan timeout)
    {
        using var cts = new CancellationTokenSource(timeout);
        try
        {
            await proc.WaitForExitAsync(cts.Token);
            return true;
        }
        catch (OperationCanceledException)
        {
            return proc.HasExited;
        }
    }

    [DllImport("user32.dll")]
    private static extern uint MapVirtualKey(uint uCode, uint uMapType);

    private sealed class StoreFile
    {
        public List<VmRecord> Vms { get; set; } = new();
    }
}

internal sealed class VmRecord
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public int RamMb { get; set; }
    public int Cpu { get; set; }
    public int DiskGb { get; set; }
    public string Size { get; set; } = "m";
    public string Os { get; set; } = "Soil Desktop";
    public string Kind { get; set; } = "desktop";
    public string? RepoUrl { get; set; }
    public string? RepoOwner { get; set; }
    public string? RepoName { get; set; }
    public string? RepoRef { get; set; }
    public string? AppStatus { get; set; }
    public string? AppError { get; set; }
    public string? AppHostUrl { get; set; }
    public string? AppGuestUrl { get; set; }
    public string? AppLog { get; set; }
    public string? AgentStatus { get; set; }
    public string? AgentError { get; set; }
    public string? AgentLog { get; set; }
    public string? AgentClaudeVersion { get; set; }
    public string? AgentCodexVersion { get; set; }
    public DateTime CreatedUtc { get; set; }
}

public sealed record StatDto(double Cpu, double Mem, double In, double Out);

public sealed record AppDto(
    string? RepoUrl,
    string Status,
    string? Error,
    string? HostUrl,
    string? GuestUrl,
    string? Log);

public sealed record TunnelDto(
    string Host,
    int Port,
    int McpPort,
    string User,
    string KeyPath,
    string Command,
    string McpCommand);

public sealed record AgentDto(
    string Status,
    string? Error,
    string? Log,
    string? ClaudeVersion,
    string? CodexVersion,
    TunnelDto? Tunnel);

public sealed record VmDto(
    string Id,
    string Name,
    string Status,
    int RamMb,
    int Cpu,
    int DiskGb,
    string Os,
    long UptimeSec,
    string? Error,
    List<StatDto> Series,
    AppDto? App,
    string Kind,
    AgentDto? Agent);

internal sealed class VmRuntime
{
    internal VmRuntime(VmRecord record)
    {
        Record = record;
    }

    internal VmRecord Record { get; }
    internal Process? Proc;
    internal MemoryMappedFile? Map;
    internal MemoryMappedViewAccessor? View;
    internal NamedPipeClientStream? Input;
    internal string Token = "";
    internal CancellationTokenSource? StartCts;
    internal Task? LaunchTask;
    internal int StartGeneration;
    internal bool ParkPending;
    internal string Status = "stopped";
    internal bool Deleting;
    internal string? Error;
    internal DateTime? StartedUtc;
    internal int SshPort;
    internal int McpPort;
    internal CancellationTokenSource? StatsCts;
    internal Task? StatsTask;
    internal Process? StatsProcess;
    internal double GuestMemMb;
    internal double GuestCpuPct;
    internal double GuestInMbps;
    internal double GuestOutMbps;
    internal long GuestCpuTotal;
    internal long GuestCpuIdle;
    internal long GuestNetRx;
    internal long GuestNetTx;
    internal DateTime GuestSampleUtc;
    internal readonly LinkedList<StatPointInternal> History = new();
    internal readonly object InputLock = new();
    internal readonly object FrameLock = new();
    internal byte[]? FrameBuffer;
    internal byte[]? CachedJpeg;
    internal int CachedSeq = -1;

    internal byte[]? GetJpeg()
        => GetJpegWithSequence().Jpeg;

    internal (int Seq, byte[]? Jpeg) GetJpegWithSequence()
    {
        lock (FrameLock)
        {
            var view = View;
            if (view is null)
                return (-1, null);

            for (var attempt = 0; attempt < 3; attempt++)
            {
                int seq;
                try
                {
                    seq = view.ReadInt32(0);
                    if (seq <= 0)
                        return (-1, null);
                    if (seq == CachedSeq && CachedJpeg is not null)
                        return (seq, CachedJpeg);

                    FrameBuffer ??= new byte[FrameBytes];
                    view.ReadArray(SharedFrameOffset, FrameBuffer, 0, FrameBytes);
                    if (view.ReadInt32(0) != seq)
                        continue;
                }
                catch
                {
                    return (-1, null);
                }

                try
                {
                    CachedJpeg = EncodeJpeg(FrameBuffer);
                    CachedSeq = seq;
                    return (seq, CachedJpeg);
                }
                catch
                {
                    return (-1, null);
                }
            }
            return (-1, null);
        }
    }

    private static byte[] EncodeJpeg(byte[] bgra)
    {
        using var bitmap = new Bitmap(FrameWidthInternal, FrameHeightInternal, PixelFormat.Format32bppRgb);
        var rect = new Rectangle(0, 0, bitmap.Width, bitmap.Height);
        var data = bitmap.LockBits(rect, ImageLockMode.WriteOnly, PixelFormat.Format32bppRgb);
        try
        {
            var stride = FrameWidthInternal * 4;
            if (data.Stride == stride)
            {
                Marshal.Copy(bgra, 0, data.Scan0, stride * FrameHeightInternal);
            }
            else
            {
                for (var y = 0; y < FrameHeightInternal; y++)
                {
                    Marshal.Copy(bgra, y * stride,
                        IntPtr.Add(data.Scan0, y * data.Stride), stride);
                }
            }
        }
        finally
        {
            bitmap.UnlockBits(data);
        }

        using var stream = new MemoryStream(512 * 1024);
        if (VmManager.JpegCodec is null)
            bitmap.Save(stream, ImageFormat.Jpeg);
        else
        {
            using var parameters = new EncoderParameters(1);
            parameters.Param[0] = new EncoderParameter(Encoder.Quality, 60L);
            bitmap.Save(stream, VmManager.JpegCodec, parameters);
        }
        return stream.ToArray();
    }

    private const int FrameWidthInternal = 1360;
    private const int FrameHeightInternal = 768;
    private const int FrameBytes = FrameWidthInternal * 4 * FrameHeightInternal;
    private const int SharedFrameOffset = 24;
}

internal sealed record StatPointInternal(double CpuPercent, double MemMb, double InMbps, double OutMbps);
