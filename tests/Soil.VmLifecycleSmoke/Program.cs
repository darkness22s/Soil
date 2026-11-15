using System.Diagnostics;
using Soil;

if (args.Length != 1)
    throw new ArgumentException("Usage: Soil.VmLifecycleSmoke <published Soil base directory>");

var baseDir = Path.GetFullPath(args[0]);
var home = Path.Combine(Path.GetTempPath(), "soil-vm-lifecycle-" + Guid.NewGuid().ToString("N"));
var paths = new SoilPaths(home);
// Exercise the desktop fast-display path with SSH forwarding enabled.
// Native guest support now needs a valid private/public Ed25519 key pair;
// verify the new-install key generator rather than supplying a bogus key.
var manager = new VmManager(baseDir, paths);
if (!File.Exists(paths.SshKeyPath) || !File.Exists(paths.SshKeyPath + ".pub"))
    throw new InvalidOperationException("Fresh installation did not generate SSH credentials.");
var vmId = "";

async Task<VmDto> WaitStatusAsync(string expected, int timeoutSeconds)
{
    var timer = Stopwatch.StartNew();
    while (timer.Elapsed < TimeSpan.FromSeconds(timeoutSeconds))
    {
        var current = manager.List().Single(x => x.Id == vmId);
        if (current.Status == expected)
            return current;
        if (current.Status == "stopped" && current.Error is not null)
            throw new InvalidOperationException($"VM failed during {expected}: {current.Error}");
        await Task.Delay(25);
    }
    throw new TimeoutException($"VM never reached {expected}; last = " +
        manager.List().Single(x => x.Id == vmId).Status);
}

static async Task AssertNoVmhostAsync()
{
    var deadline = Stopwatch.StartNew();
    int[] lastPids = [];
    while (deadline.Elapsed < TimeSpan.FromSeconds(10))
    {
        using var processes = new ProcessCollection(Process.GetProcessesByName("vmhost"));
        lastPids = processes.Processes.Where(p => !p.HasExited).Select(p => p.Id).ToArray();
        if (lastPids.Length == 0)
            return;
        await Task.Delay(200);
    }
    throw new InvalidOperationException("VM idle/stop left live vmhost.exe PIDs: " +
                                        string.Join(", ", lastPids));
}

// Dispose Process objects returned by Process.GetProcessesByName.
try
{
    var created = manager.Create("Lifecycle Smoke", "s");
    vmId = created.Id;
    if (created.Status != "stopped")
        throw new InvalidOperationException("A newly created VM must not auto-start.");
    var dataDisk = paths.MachineDiskPath(vmId);
    if (!File.Exists(dataDisk) ||
        new FileInfo(dataDisk).Length != 40L * 1024 * 1024 * 1024)
        throw new InvalidOperationException("Create did not prepare a full 40-GiB sparse ext4 volume.");
    using (var disk = new FileStream(dataDisk, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
    {
        disk.Position = 1024 + 0x38;
        if (disk.ReadByte() != 0x53 || disk.ReadByte() != 0xEF)
            throw new InvalidOperationException("The preformatted machine disk is not ext4.");
    }
    Console.WriteLine("APP: Create provisioned the 40-GiB ext4 volume without booting a VM.");

    // Default and large tiers must also provision fully sized files, without
    // starting hidden VMs or requiring mkfs inside the guest on first boot.
    foreach (var (size, gigabytes) in new[] { ("m", 80), ("l", 160) })
    {
        var provisionTimer = Stopwatch.StartNew();
        var other = manager.Create("Provision " + size, size);
        var otherPath = paths.MachineDiskPath(other.Id);
        if (other.Status != "stopped" || !File.Exists(otherPath) ||
            new FileInfo(otherPath).Length != (long)gigabytes * 1024 * 1024 * 1024)
            throw new InvalidOperationException($"Create did not provision the {gigabytes}-GiB disk.");
        using var volume = new FileStream(otherPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        volume.Position = 1024 + 0x38;
        if (volume.ReadByte() != 0x53 || volume.ReadByte() != 0xEF)
            throw new InvalidOperationException($"The {gigabytes}-GiB image is not ext4.");
        Console.WriteLine($"APP: Provisioned {gigabytes}-GiB ext4 in {provisionTimer.Elapsed.TotalMilliseconds:F1} ms.");
    }
    await AssertNoVmhostAsync();

    var initial = Stopwatch.StartNew();
    manager.Start(vmId);
    await WaitStatusAsync("running", 180);
    Console.WriteLine($"APP: First Start reached running in {initial.Elapsed.TotalMilliseconds:F1} ms");

    manager.IdleAll();
    await WaitStatusAsync("idle", 120);
    await AssertNoVmhostAsync();
    if (!File.Exists(paths.MachineSnapshotPath(vmId)))
        throw new InvalidOperationException("IdleAll did not create a checkpoint");
    Console.WriteLine("APP: IdleAll snapshotted and exited; no vmhost.exe remains.");

    var resume = Stopwatch.StartNew();
    manager.Start(vmId);
    await WaitStatusAsync("running", 180);
    Console.WriteLine($"APP: Recalled VM reached running in {resume.Elapsed.TotalMilliseconds:F1} ms");
    manager.Stop(vmId);
    await WaitStatusAsync("stopped", 120);
    await AssertNoVmhostAsync();
    // MinGW's live log handle denies concurrent readers on Windows.
    // Inspect it only after vmhost has fully exited and closed the file.
    var vmLog = File.ReadAllText(paths.MachineLogPath(vmId));
    if (!vmLog.Contains("snap: restored in"))
        throw new InvalidOperationException("App restart was not a snapshot restore.");
    if (!vmLog.Contains("net: hostfwd 127.0.0.1:"))
        throw new InvalidOperationException("SSH-enabled desktop start did not enable forwarding.");
    if (!File.Exists(paths.MachineSnapshotPath(vmId)))
        throw new InvalidOperationException("Explicit Stop did not create a checkpoint");
    Console.WriteLine("APP: Explicit Stop checkpointed and fully released the VM process.");
    Console.WriteLine("PASS: Soil app Start -> IdleAll -> recall -> Stop.");
}
finally
{
    manager.StopAll();
    if (Directory.Exists(home))
        Directory.Delete(home, true);
}

internal sealed class ProcessCollection : IDisposable
{
    internal Process[] Processes { get; }
    internal ProcessCollection(Process[] processes) => Processes = processes;
    public void Dispose()
    {
        foreach (var process in Processes)
            process.Dispose();
    }
}
