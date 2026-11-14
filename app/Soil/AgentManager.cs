using System.Collections.Concurrent;
using System.Diagnostics;

namespace Soil;

public sealed class AgentManager : IDisposable
{
    private const string AgentRoot = "/opt/soil-agent";
    private const string CodexVendor = AgentRoot + "/vendor/x86_64-unknown-linux-musl";

    private readonly VmManager m_vms;
    private readonly ConcurrentDictionary<string, AgentWork> m_work =
        new(StringComparer.OrdinalIgnoreCase);
    private readonly CancellationTokenSource m_cts = new();

    public AgentManager(VmManager vms)
    {
        m_vms = vms;
    }

    public VmDto? RequestStart(string id)
    {
        var before = m_vms.Get(id);
        if (before is null || !string.Equals(before.Kind, "agent", StringComparison.OrdinalIgnoreCase))
            return null;

        var work = m_work.GetOrAdd(id, _ => new AgentWork());
        var started = m_vms.Start(id);
        if (started is null)
            return null;

        lock (work)
        {
            if (work.Task is { IsCompleted: false } &&
                work.Cts is { IsCancellationRequested: false })
                return started;
            if (before.Status == "running" && started.Status == "running" &&
                string.Equals(started.Agent?.Status, "ready", StringComparison.OrdinalIgnoreCase))
                return started;

            work.Cts?.Cancel();
            work.Cts?.Dispose();
            work.Cts = CancellationTokenSource.CreateLinkedTokenSource(m_cts.Token);
            work.Generation++;
            var generation = work.Generation;
            var token = work.Cts.Token;
            m_vms.UpdateAgentState(id, "starting", null, null, null, null);
            work.Task = Task.Run(() => ProvisionAsync(id, generation, token));
        }
        return started;
    }

    public VmDto? RequestStop(string id)
    {
        if (m_work.TryGetValue(id, out var work))
        {
            lock (work)
            {
                work.Cts?.Cancel();
                work.Generation++;
            }
            m_vms.UpdateAgentState(id, "stopped", null, null, null, null);
        }
        return m_vms.Stop(id);
    }

    public void Remove(string id)
    {
        if (!m_work.TryRemove(id, out var work))
            return;
        lock (work)
        {
            work.Cts?.Cancel();
            work.Cts?.Dispose();
            work.Cts = null;
            work.Generation++;
        }
    }

    public bool OpenTerminal(string id)
    {
        var record = m_vms.FindRecord(id);
        if (record is null ||
            !string.Equals(record.AgentStatus, "ready", StringComparison.OrdinalIgnoreCase))
            return false;

        var endpoint = m_vms.GetSshEndpoint(id);
        if (endpoint is null)
            return false;
        try
        {
            var sshArgs = new List<string>
            {
                "-i", endpoint.Value.Key,
                "-p", endpoint.Value.Port.ToString(),
                "-o", "StrictHostKeyChecking=no",
                "-o", "UserKnownHostsFile=NUL",
                "tc@127.0.0.1"
            };
            var wt = FindWindowsTerminal();
            ProcessStartInfo psi;
            if (wt is not null)
            {
                psi = new ProcessStartInfo
                {
                    FileName = wt,
                    UseShellExecute = false,
                    CreateNoWindow = true
                };
                psi.ArgumentList.Add(endpoint.Value.Exe);
                foreach (var arg in sshArgs)
                    psi.ArgumentList.Add(arg);
            }
            else
            {
                psi = new ProcessStartInfo
                {
                    FileName = endpoint.Value.Exe,
                    UseShellExecute = true
                };
                psi.Arguments = string.Join(" ", sshArgs.Select(Quote));
            }
            using var process = Process.Start(psi);
            return process is not null;
        }
        catch
        {
            return false;
        }
    }

    public void Dispose()
    {
        m_cts.Cancel();
        foreach (var work in m_work.Values)
        {
            lock (work)
            {
                work.Cts?.Cancel();
                work.Cts?.Dispose();
                work.Cts = null;
            }
        }
        m_cts.Dispose();
    }

    private async Task ProvisionAsync(string id, int generation, CancellationToken ct)
    {
        var log = "";
        string? claude = null;
        string? codex = null;
        try
        {
            log = Append(log, "Waiting for the machine shell.");
            Report(id, "starting", null, log, claude, codex);

            var endpoint = await WaitForSshAsync(id, ct);
            log = Append(log, "SSH tunnel is listening on 127.0.0.1:" + endpoint.Port + ".");
            Report(id, "preparing", null, log, claude, codex);

            var present = await RunAsync(
                endpoint,
                "test -x " + AgentRoot + "/claude && " +
                "test -x " + CodexVendor + "/bin/codex && echo ready",
                20, ct);
            if (present.Code != 0 ||
                !present.Output.Contains("ready", StringComparison.Ordinal))
                throw new InvalidOperationException(
                    "This Soil build is missing its agent tools.");

            log = Append(log, "Linking Claude Code and Codex CLI.");
            var links = await RunAsync(
                endpoint,
                "mkdir -p /home/tc/.local/bin; " +
                "ln -sf " + AgentRoot + "/claude /home/tc/.local/bin/claude; " +
                "ln -sf " + CodexVendor + "/bin/codex /home/tc/.local/bin/codex; " +
                "ln -sf " + CodexVendor + "/codex-path/rg /home/tc/.local/bin/rg; " +
                "ln -sf " + CodexVendor + "/codex-resources/bwrap /home/tc/.local/bin/bwrap; " +
                "printf '%s\\n' '#!/bin/sh' " +
                "'[ -n \"$DISPLAY\" ] || export DISPLAY=:0' " +
                "'i=0' " +
                "'while [ $i -lt 60 ]; do' " +
                "'  xset q >/dev/null 2>&1 && break' " +
                "'  sleep 1; i=$((i+1))' " +
                "'done' " +
                "'exec /usr/local/bin/soil-browser \"$@\"' " +
                "> /home/tc/.local/bin/xdg-open; " +
                "chmod 755 /home/tc/.local/bin/xdg-open; " +
                "ln -sf xdg-open /home/tc/.local/bin/open; " +
                "test -x /home/tc/.local/bin/claude && test -x /home/tc/.local/bin/codex && " +
                "test -x /home/tc/.local/bin/xdg-open",
                30, ct);
            if (links.Code != 0)
                throw new InvalidOperationException(
                    "Could not link the agent tools: " + FirstError(links));

            var versions = await RunAsync(
                endpoint,
                "export PATH=/home/tc/.local/bin:$PATH; " +
                "claude --version 2>&1; codex --version 2>&1",
                60, ct);
            var lines = (versions.Output + "\n" + versions.Errors)
                .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            claude = lines.FirstOrDefault(line => line.Length > 0 && char.IsDigit(line[0]));
            codex = lines.FirstOrDefault(line =>
                line.StartsWith("codex-cli", StringComparison.OrdinalIgnoreCase));
            if (versions.Code != 0 || string.IsNullOrWhiteSpace(claude) ||
                string.IsNullOrWhiteSpace(codex))
                throw new InvalidOperationException(
                    "The agent tools did not start: " + FirstError(versions));

            if (!IsCurrent(id, generation))
                return;

            log = Append(log, "Claude Code " + claude + " and Codex " + codex + " are ready.");
            Report(id, "ready", null, log, claude, codex);
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception ex)
        {
            if (!IsCurrent(id, generation))
                return;
            log = Append(log, ex.Message);
            Report(id, "failed", ex.Message, log, claude, codex);
        }
    }

    private bool IsCurrent(string id, int generation)
    {
        if (!m_work.TryGetValue(id, out var work))
            return false;
        lock (work)
            return work.Generation == generation;
    }

    private async Task<(string Exe, string Key, int Port)> WaitForSshAsync(
        string id, CancellationToken ct)
    {
        for (var attempt = 0; attempt < 180; attempt++)
        {
            ct.ThrowIfCancellationRequested();
            var endpoint = m_vms.GetSshEndpoint(id);
            if (endpoint is not null)
            {
                var probe = await RunAsync(endpoint.Value, "true", 8, ct);
                if (probe.Code == 0)
                    return endpoint.Value;
            }

            var vm = m_vms.Get(id);
            if (vm is not null && vm.Status == "stopped")
            {
                throw new InvalidOperationException(
                    string.IsNullOrWhiteSpace(vm.Error)
                        ? "The machine stopped before its shell was reachable."
                        : vm.Error);
            }
            await Task.Delay(1000, ct);
        }
        throw new TimeoutException("The machine shell did not become reachable.");
    }

    private async Task<(int Code, string Output, string Errors)> RunAsync(
        (string Exe, string Key, int Port) endpoint,
        string remote,
        int timeoutSeconds,
        CancellationToken ct)
    {
        var psi = SshInfo(endpoint);
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError = true;
        psi.ArgumentList.Add(remote);

        using var ssh = new Process { StartInfo = psi, EnableRaisingEvents = true };
        ssh.Start();
        var output = ssh.StandardOutput.ReadToEndAsync();
        var errors = ssh.StandardError.ReadToEndAsync();
        using var wait = CancellationTokenSource.CreateLinkedTokenSource(ct);
        wait.CancelAfter(TimeSpan.FromSeconds(timeoutSeconds));
        try
        {
            await ssh.WaitForExitAsync(wait.Token);
            return (ssh.ExitCode, await output, await errors);
        }
        catch (OperationCanceledException)
        {
            KillQuietly(ssh);
            if (ct.IsCancellationRequested)
                throw;
            return (-1, await output, "Timed out.");
        }
    }

    private static ProcessStartInfo SshInfo((string Exe, string Key, int Port) endpoint)
    {
        var psi = new ProcessStartInfo
        {
            FileName = endpoint.Exe,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        psi.ArgumentList.Add("-i");
        psi.ArgumentList.Add(endpoint.Key);
        psi.ArgumentList.Add("-p");
        psi.ArgumentList.Add(endpoint.Port.ToString());
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add("StrictHostKeyChecking=no");
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add("UserKnownHostsFile=NUL");
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add("BatchMode=yes");
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add("ConnectTimeout=8");
        psi.ArgumentList.Add("-o");
        psi.ArgumentList.Add("LogLevel=ERROR");
        psi.ArgumentList.Add("tc@127.0.0.1");
        return psi;
    }

    private void Report(
        string id, string status, string? error, string? log,
        string? claudeVersion, string? codexVersion)
    {
        m_vms.UpdateAgentState(id, status, error, log, claudeVersion, codexVersion);
    }

    private static string Append(string log, string line) =>
        string.IsNullOrEmpty(log) ? line : log + "\n" + line;

    private static string FirstError((int Code, string Output, string Errors) result)
    {
        var text = (result.Errors + "\n" + result.Output).Trim();
        if (string.IsNullOrEmpty(text))
            return "exit code " + result.Code;
        return text.Length > 1200 ? text[..1200] : text;
    }

    private static string Quote(string value) =>
        "\"" + value.Replace("\"", "\\\"", StringComparison.Ordinal) + "\"";

    private static string? FindWindowsTerminal()
    {
        var local = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Microsoft", "WindowsApps", "wt.exe");
        if (File.Exists(local))
            return local;
        var path = Environment.GetEnvironmentVariable("PATH") ?? "";
        foreach (var part in path.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            try
            {
                var candidate = Path.Combine(part.Trim(), "wt.exe");
                if (File.Exists(candidate))
                    return candidate;
            }
            catch
            {
            }
        }
        return null;
    }

    private static void KillQuietly(Process process)
    {
        try
        {
            if (!process.HasExited)
                process.Kill(true);
        }
        catch
        {
        }
    }

    private sealed class AgentWork
    {
        public CancellationTokenSource? Cts;
        public Task? Task;
        public int Generation;
    }
}
