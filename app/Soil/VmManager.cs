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
    private const int FrameWidth = 1024;
    private const int FrameHeight = 768;
    private const int FrameStride = FrameWidth * 4;
    private const int FrameBytes = FrameStride * FrameHeight;
    private const int SharedFrameOffset = 24;
    // command tail after the frame region (engine VH_IPC_SHARED layout):
    // uCmd, cCmdHave, then a NUL-terminated 1 KiB path buffer. The view
    // must span the whole struct so the tail stays addressable.
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
        foreach (var record in m_records)
            m_runtimes[record.Id] = new VmRuntime(record);

        _ = Task.Run(StatsLoopAsync);
    }

    public SoilPaths Home => m_paths;

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
        var (ram, disk) = size switch
        {
            "s" => (512, 40),
            "l" => (2048, 160),
            _ => (1024, 80)
        };

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
            runtime.StartCts?.Cancel();
            runtime.StatsCts?.Cancel();
            runtime.ParkPending = false;
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
                    proc!.WaitForExit(8000);
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
                    proc!.WaitForExit(8000);
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
                if (!runtime.Proc!.WaitForExit(8000))
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
            if (!m_runtimes.TryGetValue(id, out runtime) || runtime.Input is null)
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

        try
        {
            lock (runtime.InputLock)
            {
                runtime.Input.Write(packet, 0, packet.Length);
                runtime.Input.Flush();
            }
            return true;
        }
        catch
        {
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
            await Task.Delay(66, ct);
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
            psi.ArgumentList.Add(Path.Combine(m_imagesDir, "core2.gz"));
            psi.ArgumentList.Add("--disk");
            psi.ArgumentList.Add(Path.Combine(m_imagesDir, "soil.squashfs"));
            psi.ArgumentList.Add("--ram");
            psi.ArgumentList.Add(runtime.Record.RamMb.ToString());
            Directory.CreateDirectory(m_paths.MachineLogDir(runtime.Record.Id));
            psi.ArgumentList.Add("--cmdline");
            psi.ArgumentList.Add("console=ttyS0 quiet loglevel=3 acpi=off noapic nolapic mitigations=off nozswap");
            psi.ArgumentList.Add("--log");
            psi.ArgumentList.Add(m_paths.MachineLogPath(runtime.Record.Id));
            psi.ArgumentList.Add("--no-ui");
            var sshPort = 0;
            if (!string.IsNullOrEmpty(m_sshKeyPath) && File.Exists(m_sshKeyPath))
            {
                sshPort = AllocateLoopbackPort();
                psi.ArgumentList.Add("--ssh-port");
                psi.ArgumentList.Add(sshPort.ToString(CultureInfo.InvariantCulture));
                psi.ArgumentList.Add("--mcp-port");
                psi.ArgumentList.Add("0");
            }
            else
            {
                psi.ArgumentList.Add("--no-hostfwd");
            }
            psi.ArgumentList.Add("--ipc");
            psi.ArgumentList.Add(token);

            // fastboot: save guest state to boot.snap on every clean stop, and
            // resume from it when it exists. A stale or unreadable snapshot
            // makes the engine fall back to a regular boot on its own.
            var snapPath = Path.Combine(m_paths.MachineDir(runtime.Record.Id), "boot.snap");
            var restoreBoot = false;
            if (File.Exists(snapPath))
            {
                try
                {
                    restoreBoot = new FileInfo(snapPath).Length > 0;
                }
                catch (IOException)
                {
                    restoreBoot = false;
                }
            }
            psi.ArgumentList.Add("--snapshot-out");
            psi.ArgumentList.Add(snapPath);
            if (restoreBoot)
            {
                psi.ArgumentList.Add("--restore");
                psi.ArgumentList.Add(snapPath);
            }

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

            var boot = Stopwatch.StartNew();
            while (boot.Elapsed < TimeSpan.FromSeconds(90))
            {
                ct.ThrowIfCancellationRequested();
                if (proc.HasExited)
                    break;
                if (HasVisibleContent(bootView) || boot.Elapsed > TimeSpan.FromSeconds(20))
                    break;
                await Task.Delay(400, ct);
            }

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
            _ = Task.Run(() => MonitorExitAsync(runtime, proc));
            if (!restoreBoot)
                _ = Task.Run(() => KickFirstBootSnapshotAsync(runtime, snapPath, proc));
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
        int generation;
        lock (m_lock)
        {
            generation = runtime.StartGeneration;
        }
        try
        {
            await proc.WaitForExitAsync();
        }
        catch
        {
        }

        lock (m_lock)
        {
            if (ReferenceEquals(runtime.Proc, proc) &&
                runtime.StartGeneration == generation)
            {
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
        int generation = runtime.StartGeneration;
        try
        {
            lock (m_lock)
            {
                generation = runtime.StartGeneration;
                if (ReferenceEquals(runtime.Proc, proc))
                    runtime.View?.Write(StopOffset, 1);
            }
        }
        catch
        {
        }

        try
        {
            if (!await WaitForExitAsync(proc, TimeSpan.FromSeconds(15)))
                proc.Kill(true);
        }
        catch
        {
        }

        lock (m_lock)
        {
            // a newer start overrides this stop's terminal bookkeeping
            if (ReferenceEquals(runtime.Proc, proc) &&
                runtime.StartGeneration == generation)
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
            runtime.Status = "stopped";
            runtime.StartedUtc = null;
        }

        var uptime = runtime.StartedUtc is null ? 0 :
            (long)Math.Max(0, (DateTime.UtcNow - runtime.StartedUtc.Value).TotalSeconds);
        var series = runtime.History.ToList();

        return new VmDto(
            record.Id, record.Name, runtime.Status, record.RamMb, record.Cpu,
            record.DiskGb, record.Os, uptime, runtime.Error,
            series.Select(p => new StatDto(p.CpuPercent, p.MemMb, p.InMbps, p.OutMbps)).ToList());
    }

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

    // Idle mode (parked): the guest was snapshotted and the engine process
    // exited, so the "on" machine consumes no CPU, allocation, or RAM at all.
    // Any later start resumes the saved state in a few hundred ms.
    public void IdleAll()
    {
        lock (m_lock)
        {
            foreach (var runtime in m_runtimes.Values)
            {
                if (runtime.Deleting || runtime.ParkPending)
                    continue;
                var proc = runtime.Proc;
                if (proc is null || proc.HasExited || runtime.Status != "running")
                    continue;
                runtime.StartCts?.Cancel();
                runtime.StatsCts?.Cancel();
                runtime.ParkPending = true;
                runtime.Status = "stopping";
                _ = Task.Run(() => StopAsync(runtime, proc));
            }
        }
    }

    public void ResumeAll()
    {
        List<VmRecord> targets;
        lock (m_lock)
        {
            targets = m_runtimes.Values
                .Where(rt => !rt.Deleting && rt.Status == "idle")
                .Select(rt => rt.Record)
                .ToList();
        }
        foreach (var record in targets)
            _ = Start(record.Id);
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

    private static string FindSshExecutable()
    {
        var bundled = Path.Combine(Environment.SystemDirectory, "OpenSSH", "ssh.exe");
        if (File.Exists(bundled))
            return bundled;
        foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "")
                     .Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            try
            {
                var candidate = Path.Combine(dir.Trim(), "ssh.exe");
                if (File.Exists(candidate))
                    return candidate;
            }
            catch
            {
            }
        }
        return "ssh.exe";
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
                if (!File.Exists(path) || !File.ReadAllBytes(path).SequenceEqual(bytes))
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
                return "";
            RestrictKeyPermissions(path);
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
            proc?.WaitForExit(8000);
        }
        catch
        {
        }
    }

    // First boot of a machine has no boot.snap yet: ask the engine through
    // the IPC command tail to park the guest and save one soon after the
    // desktop is up, so the very next start resumes instantly. Later boots
    // refresh the file on clean stop, so this only fires when it must.
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

            // engine protocol: path + NUL in szCmd, byte count without NUL in
            // cCmdHave, then uCmd=1 to go. The engine zeroes uCmd when it
            // consumes the request and writes uCmd=2 once the save settled.
            var buf = new byte[path.Length + 1];
            path.CopyTo(buf, 0);
            view.WriteArray(CmdPathOffset, buf, 0, buf.Length);
            view.Write(CmdHaveOffset, path.Length);
            view.Write(CmdOffset, 1);

            for (var i = 0; i < 60; i++)
            {
                await Task.Delay(250);
                var ack = view.ReadInt32(CmdOffset);
                if (ack == 2)
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
    public DateTime CreatedUtc { get; set; }
}

public sealed record StatDto(double Cpu, double Mem, double In, double Out);

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
    List<StatDto> Series);

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
    // set when parking (idle mode): on the engine's clean exit the runtime
    // settles into "idle" instead of "stopped"; consumed exactly once
    internal bool ParkPending;
    internal string Status = "stopped";
    internal bool Deleting;
    internal string? Error;
    internal DateTime? StartedUtc;
    internal int SshPort;
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
            parameters.Param[0] = new EncoderParameter(Encoder.Quality, 68L);
            bitmap.Save(stream, VmManager.JpegCodec, parameters);
        }
        return stream.ToArray();
    }

    private const int FrameWidthInternal = 1024;
    private const int FrameHeightInternal = 768;
    private const int FrameBytes = FrameWidthInternal * 4 * FrameHeightInternal;
    private const int SharedFrameOffset = 24;
}

internal sealed record StatPointInternal(double CpuPercent, double MemMb, double InMbps, double OutMbps);
