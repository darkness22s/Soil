using System.Diagnostics;
using Soil;

if (args.Length != 1)
    throw new ArgumentException("Usage: Soil.VmLifecycleSmoke <published Soil base directory>");

var baseDir = Path.GetFullPath(args[0]);
var home = Path.Combine(Path.GetTempPath(), "soil-vm-lifecycle-" + Guid.NewGuid().ToString("N"));
var paths = new SoilPaths(home);
var manager = new VmManager(baseDir, paths);
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
        await Task.Delay(250);
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
    if (!File.ReadAllText(paths.MachineLogPath(vmId)).Contains("snap: restored in"))
        throw new InvalidOperationException("App restart was not a snapshot restore.");
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
