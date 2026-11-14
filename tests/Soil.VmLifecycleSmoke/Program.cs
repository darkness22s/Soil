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

static void AssertNoVmhost()
{
    using var processes = new ProcessCollection(Process.GetProcessesByName("vmhost"));
    if (processes.Processes.Length != 0)
        throw new InvalidOperationException("Parked VM still has a vmhost.exe process");
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
    AssertNoVmhost();
    if (!File.Exists(paths.MachineSnapshotPath(vmId)))
        throw new InvalidOperationException("IdleAll did not create a checkpoint");
    Console.WriteLine("APP: IdleAll snapshotted and exited; no vmhost.exe remains.");

    var resume = Stopwatch.StartNew();
    manager.Start(vmId);
    await WaitStatusAsync("running", 180);
    Console.WriteLine($"APP: Recalled VM reached running in {resume.Elapsed.TotalMilliseconds:F1} ms");
    if (!File.ReadAllText(paths.MachineLogPath(vmId)).Contains("snap: restored in"))
        throw new InvalidOperationException("App restart was not a snapshot restore.");

    manager.Stop(vmId);
    await WaitStatusAsync("stopped", 120);
    AssertNoVmhost();
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
