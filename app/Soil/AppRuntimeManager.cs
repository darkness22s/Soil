using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO.Compression;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Soil;

public sealed record AppImportRequest(
    string? Name,
    string? RepoUrl,
    string? Owner,
    string? Repo,
    string? Ref,
    string? Size);

public sealed class AppRuntimeManager : IDisposable
{
    private static readonly HttpClient Http = CreateHttpClient();
    private static readonly int[] CommonPorts =
        { 3000, 3001, 5173, 4173, 4321, 5000, 8000, 8080, 8888 };
    private static readonly Lazy<string> NodeVersion = new(ReadNodeVersion);

    private readonly VmManager m_vms;
    private readonly SoilPaths m_paths;
    private readonly GitHubService m_github;
    private readonly ConcurrentDictionary<string, AppRuntime> m_runtimes = new(StringComparer.OrdinalIgnoreCase);
    private readonly ConcurrentDictionary<string, SemaphoreSlim> m_buildLocks =
        new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, SemaphoreSlim> m_workspaceLocks =
        new(StringComparer.Ordinal);
    private readonly CancellationTokenSource m_cts = new();

    public AppRuntimeManager(VmManager vms, SoilPaths paths, GitHubService github)
    {
        m_vms = vms;
        m_paths = paths;
        m_github = github;
    }

    public VmDto CreateApp(AppImportRequest request)
    {
        var repo = ParseRepo(request);
        var name = string.IsNullOrWhiteSpace(request.Name)
            ? repo.Name
            : request.Name.Trim();
        var dto = m_vms.CreateApp(
            name, request.Size ?? "m", repo.Url, repo.Owner, repo.Name, request.Ref);
        var runtime = CreateRuntime(dto.Id);
        m_runtimes[dto.Id] = runtime;
        var token = runtime.WorkCts.Token;
        runtime.Task = Task.Run(() => ImportAndStartAsync(runtime, request, token));
        return dto;
    }

    public VmDto? RequestStart(string id)
    {
        var record = m_vms.FindRecord(id);
        if (record is null)
            return null;
        if (!string.Equals(record.Kind, "app", StringComparison.OrdinalIgnoreCase))
            return m_vms.Start(id);

        var runtime = m_runtimes.GetOrAdd(id, _ => CreateRuntime(id));
        if (runtime.Task is null && runtime.WorkCts.IsCancellationRequested)
        {
            runtime.WorkCts.Dispose();
            runtime.WorkCts = CreateWorkToken();
        }
        if (runtime.Task is null)
        {
            var token = runtime.WorkCts.Token;
            runtime.Task = Task.Run(() => StartVmAndOpenAsync(runtime, token));
        }
        var started = m_vms.Start(id);
        return started ?? m_vms.Get(id);
    }

    public VmDto? RequestStop(string id)
    {
        if (m_runtimes.TryGetValue(id, out var runtime))
            StopRuntime(runtime);
        return m_vms.Stop(id);
    }

    public void Remove(string id)
    {
        if (m_runtimes.TryRemove(id, out var runtime))
            StopRuntime(runtime);
    }

    private async Task ImportAndStartAsync(
        AppRuntime runtime, AppImportRequest request, CancellationToken ct)
    {
        try
        {
            await runtime.Gate.WaitAsync(ct);
        }
        catch (OperationCanceledException)
        {
            runtime.Task = null;
            return;
        }

        try
        {
            await PrepareCoreAsync(runtime, request, ct);
            if (runtime.Ready)
                await StartVmAndOpenCoreAsync(runtime, ct);
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception ex)
        {
            Fail(runtime, ex);
        }
        finally
        {
            runtime.Task = null;
            runtime.Gate.Release();
        }
    }

    private async Task StartVmAndOpenAsync(AppRuntime runtime, CancellationToken ct)
    {
        try
        {
            await runtime.Gate.WaitAsync(ct);
        }
        catch (OperationCanceledException)
        {
            runtime.Task = null;
            return;
        }

        try
        {
            await StartVmAndOpenCoreAsync(runtime, ct);
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception ex)
        {
            Fail(runtime, ex);
        }
        finally
        {
            runtime.Task = null;
            runtime.Gate.Release();
        }
    }

    private async Task StartVmAndOpenCoreAsync(AppRuntime runtime, CancellationToken ct)
    {
        if (!IsRuntimeAlive(runtime))
        {
            var record = m_vms.FindRecord(runtime.Id);
            if (record is null)
                return;
            runtime.Ready = false;
            var request = new AppImportRequest(
                record.Name, record.RepoUrl, record.RepoOwner,
                record.RepoName, record.RepoRef, record.Size);
            await PrepareCoreAsync(runtime, request, ct);
        }
        if (!runtime.Ready)
            return;
        if (string.IsNullOrWhiteSpace(runtime.GuestUrl) && string.IsNullOrWhiteSpace(runtime.SolPath))
            return;

        ct.ThrowIfCancellationRequested();
        m_vms.Start(runtime.Id);
        SetState(runtime, "starting",
            string.IsNullOrWhiteSpace(runtime.SolPath) ? "Opening app" : "Installing app");
        if (!string.IsNullOrWhiteSpace(runtime.SolPath))
        {
            if (!File.Exists(runtime.SolPath))
                throw new InvalidOperationException("The app package is missing.");
            if (string.IsNullOrWhiteSpace(runtime.SolAppId))
                throw new InvalidOperationException("The app package ID is missing.");
            await m_vms.OpenGuestSolAsync(
                runtime.Id, runtime.SolPath, runtime.SolAppId, ct);
        }
        else
        {
            await m_vms.OpenGuestUrlAsync(runtime.Id, runtime.GuestUrl!, ct);
        }
        SetState(runtime, "ready", null);
    }

    private async Task PrepareCoreAsync(
        AppRuntime runtime, AppImportRequest request, CancellationToken ct)
    {
        var preparedAt = Stopwatch.StartNew();
        var dir = runtime.SourceDir;
        var marker = ImportMarker(runtime);
        var setupMarker = SetupMarker(runtime);
        string? archiveHash;
        SetState(runtime, "fetching", "Downloading repository");
        if (!File.Exists(marker) ||
            !Directory.Exists(dir) ||
            !Directory.EnumerateFileSystemEntries(dir).Any())
        {
            if (Directory.Exists(dir))
                Directory.Delete(dir, true);
            TryDelete(setupMarker);
            Directory.CreateDirectory(dir);
            var downloadAt = Stopwatch.StartNew();
            archiveHash = await DownloadRepoAsync(runtime, request, dir, ct);
            downloadAt.Stop();
            await File.WriteAllTextAsync(marker, DateTime.UtcNow.ToString("O"), ct);
            if (!string.IsNullOrWhiteSpace(archiveHash))
                await File.WriteAllTextAsync(ArchiveMarker(runtime), archiveHash, ct);
            AppendLog(runtime, "Repository ready in " + FormatDuration(downloadAt.Elapsed) + ".");
        }
        else
        {
            archiveHash = await ReadArchiveHashAsync(runtime, ct);
        }
        runtime.ArchiveHash = archiveHash;
        ct.ThrowIfCancellationRequested();

        SetState(runtime, "detecting", "Detecting app");
        var plan = DetectPlan(dir);
        if (plan.Kind == "none")
            throw new InvalidOperationException(NoRunnableAppMessage(dir));

        runtime.Kind = plan.Kind;
        runtime.Port = plan.Port > 0 ? plan.Port : 0;
        runtime.SpaFallback = plan.SpaFallback;
        runtime.ManifestStart = plan.Start;
        if (!string.IsNullOrWhiteSpace(plan.ProjectDir))
            AppendLog(runtime, "Project: " + RelativeDisplay(dir, plan.ProjectDir));
        if (!string.IsNullOrWhiteSpace(plan.SetupDir) &&
            !PathEquals(plan.SetupDir, plan.ProjectDir ?? dir))
            AppendLog(runtime, "Workspace: " + RelativeDisplay(dir, plan.SetupDir));
        if (!string.IsNullOrWhiteSpace(plan.BuildCommand))
            AppendLog(runtime, "Build: " + plan.BuildCommand);

        var cacheKey = BuildCacheKey(runtime, dir, plan);
        BuildCacheEntry? cachedBuild = null;
        BuildCacheEntry? storedBuild = null;
        SemaphoreSlim? buildGate = null;
        var buildGateHeld = false;
        WorkspaceLease? workspace = null;
        var effectivePlan = plan;
        var buildDir = dir;
        try
        {
            if (cacheKey is not null)
            {
                cachedBuild = FindCachedBuild(cacheKey, archiveHash);
                if (cachedBuild is null)
                {
                    buildGate = m_buildLocks.GetOrAdd(
                        cacheKey, _ => new SemaphoreSlim(1, 1));
                    await buildGate.WaitAsync(ct);
                    buildGateHeld = true;
                    cachedBuild = FindCachedBuild(cacheKey, archiveHash);
                }
            }

            if (cachedBuild is not null)
            {
                AppendLog(runtime, "Using cached build.");
            }
            else if (cacheKey is not null &&
                     !File.Exists(setupMarker) &&
                     CanUseWarmWorkspace(plan))
            {
                workspace = await TryAcquireWorkspaceAsync(
                    runtime, request, dir, plan, ct);
                if (workspace is not null)
                {
                    effectivePlan = workspace.Plan;
                    buildDir = workspace.Root;
                    runtime.SpaFallback = effectivePlan.SpaFallback;
                    runtime.ManifestStart = effectivePlan.Start;
                }
            }

            var useWorkspace = workspace is not null;
            var setupDir = effectivePlan.SetupDir ?? effectivePlan.ProjectDir ?? buildDir;
            if (cachedBuild is null &&
                !effectivePlan.SkipSetup &&
                !string.IsNullOrWhiteSpace(effectivePlan.SetupCommand))
            {
                EnsureSetupTools(effectivePlan.SetupCommand);
                var rerun = useWorkspace || !File.Exists(setupMarker);
                if (!rerun &&
                    effectivePlan.Kind == "static" &&
                    string.IsNullOrWhiteSpace(effectivePlan.StaticRoot))
                    rerun = FindBuildOutput(effectivePlan.ProjectDir ?? buildDir, buildDir) is null;
                if (rerun)
                {
                    SetState(runtime, "installing",
                        effectivePlan.Kind == "static"
                            ? "Installing and building app"
                            : "Installing dependencies");
                    var setupAt = Stopwatch.StartNew();
                    await RunSetupAsync(
                        runtime, setupDir, effectivePlan.SetupCommand, ct);
                    setupAt.Stop();
                    AppendLog(runtime, "Setup finished in " + FormatDuration(setupAt.Elapsed) + ".");
                    if (!useWorkspace)
                        await File.WriteAllTextAsync(
                            setupMarker, DateTime.UtcNow.ToString("O"), ct);
                }
            }
            ct.ThrowIfCancellationRequested();

            if (effectivePlan.Kind == "node")
            {
                if (NeedsNode(effectivePlan.Start))
                    EnsureSetupTools(effectivePlan.Start);
                SetState(runtime, "starting", "Starting app");
                await StartNodeAsync(
                    runtime, effectivePlan.ProjectDir ?? buildDir, effectivePlan, ct);
            }
            else if (effectivePlan.Kind == "static")
            {
                var root = cachedBuild?.DistRoot ??
                           effectivePlan.StaticRoot ??
                           FindBuildOutput(effectivePlan.ProjectDir ?? buildDir, buildDir);
                if (root is null ||
                    !Directory.Exists(root) ||
                    !File.Exists(Path.Combine(root, "index.html")))
                {
                    if (!string.IsNullOrWhiteSpace(effectivePlan.Start))
                    {
                        AppendLog(runtime,
                            "No static build output was found; starting the app server instead.");
                        runtime.Kind = "node";
                        SetState(runtime, "starting", "Starting app");
                        await StartNodeAsync(
                            runtime, effectivePlan.ProjectDir ?? buildDir, effectivePlan, ct);
                    }
                    else
                    {
                        throw new InvalidOperationException(
                            "The app build finished, but no index.html was found in dist, build, or out. " +
                            "Check the build command and its output folder.");
                    }
                }
                else
                {
                    if (cachedBuild is null && cacheKey is not null)
                    {
                        storedBuild = CacheBuild(
                            runtime, cacheKey, archiveHash, dir, plan, root);
                    }

                    if (effectivePlan.PreferSol)
                    {
                        runtime.Kind = "sol";
                        SetState(runtime, "packaging", "Packaging app");
                        if (await TryPackageSolAsync(runtime, root, ct))
                        {
                            runtime.SolPath = SolPackagePath(runtime);
                            runtime.HostUrl = null;
                            runtime.GuestUrl = null;
                        }
                        else
                        {
                            runtime.SolPath = null;
                            runtime.SolAppId = null;
                            AppendLog(runtime,
                                "WARNING: .sol packaging is unavailable; continuing with the host-served app.");
                            runtime.Kind = "static";
                        }
                    }

                    if (runtime.Kind != "sol")
                    {
                        var serveRoot = cachedBuild?.DistRoot ??
                                        storedBuild?.DistRoot ??
                                        root;
                        if (useWorkspace && storedBuild is null)
                            serveRoot = SnapshotStaticRoot(runtime, root);

                        SetState(runtime, "starting", "Serving app");
                        runtime.Static = new StaticSiteServer(serveRoot);
                        runtime.Port = runtime.Static.Port;
                        runtime.HostUrl = $"http://127.0.0.1:{runtime.Port}/";
                        runtime.GuestUrl = $"http://10.0.2.2:{runtime.Port}/";
                    }
                }
            }
            else
            {
                throw new InvalidOperationException(NoRunnableAppMessage(dir));
            }

            ct.ThrowIfCancellationRequested();
            runtime.Ready = true;
            AppendLog(runtime, "App prepared in " + FormatDuration(preparedAt.Elapsed) + ".");
            SetState(runtime, "ready", null);
        }
        finally
        {
            workspace?.Dispose();
            if (buildGateHeld)
                buildGate?.Release();
        }
    }

    private async Task<string> DownloadRepoAsync(
        AppRuntime runtime, AppImportRequest request, string dir, CancellationToken ct)
    {
        var repo = ParseRepo(request);
        var reference = string.IsNullOrWhiteSpace(request.Ref) ? null : request.Ref.Trim();
        var temp = Path.Combine(m_paths.CacheDir, "repo-" + runtime.Id + ".zip");
        Directory.CreateDirectory(m_paths.CacheDir);

        try
        {
            if (m_github.HasSession)
            {
                try
                {
                    using var response = await m_github.OpenArchiveAsync(
                        repo.Owner, repo.Name, reference, ct);
                    if (response is not null)
                    {
                        var hash = await SaveArchiveAsync(response, temp, ct);
                        ExtractArchive(temp, dir);
                        return hash;
                    }
                }
                catch (Exception ex) when (
                    ex is not InvalidOperationException ||
                    !ex.Message.Contains("expired", StringComparison.OrdinalIgnoreCase))
                {
                }
            }

            var publicHash = await DownloadPublicArchiveAsync(
                repo.Owner, repo.Name, reference, temp, ct);
            ExtractArchive(temp, dir);
            return publicHash;
        }
        finally
        {
            try
            {
                if (File.Exists(temp))
                    File.Delete(temp);
            }
            catch
            {
            }
        }
    }

    private static async Task<string> DownloadPublicArchiveAsync(
        string owner, string repo, string? reference, string temp, CancellationToken ct)
    {
        var refs = new List<string>();
        if (!string.IsNullOrWhiteSpace(reference))
            refs.Add(reference);
        var defaultBranch = await GetDefaultBranchAsync(owner, repo, ct);
        if (!string.IsNullOrWhiteSpace(defaultBranch))
            refs.Add(defaultBranch);
        refs.Add("main");
        refs.Add("master");

        var lastStatus = 0;
        foreach (var candidate in refs.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            var encodedRef = string.Join("/", candidate.Split('/').Select(Uri.EscapeDataString));
            var url = "https://codeload.github.com/" + Uri.EscapeDataString(owner) + "/" +
                      Uri.EscapeDataString(repo) + "/zip/refs/heads/" + encodedRef;
            using var response = await Http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct);
            lastStatus = (int)response.StatusCode;
            if (response.IsSuccessStatusCode)
                return await SaveArchiveAsync(response, temp, ct);
            if (response.StatusCode != HttpStatusCode.NotFound)
                break;
        }

        throw new InvalidOperationException(lastStatus == 404
            ? "Repository not found. Connect GitHub for private repositories."
            : "GitHub could not download that repository.");
    }

    private static async Task<string?> GetDefaultBranchAsync(
        string owner, string repo, CancellationToken ct)
    {
        try
        {
            var url = "https://api.github.com/repos/" + Uri.EscapeDataString(owner) + "/" +
                      Uri.EscapeDataString(repo);
            using var response = await Http.GetAsync(url, ct);
            if (!response.IsSuccessStatusCode)
                return null;
            using var stream = await response.Content.ReadAsStreamAsync(ct);
            using var doc = await JsonDocument.ParseAsync(stream, cancellationToken: ct);
            return doc.RootElement.TryGetProperty("default_branch", out var branch)
                ? branch.GetString()
                : null;
        }
        catch
        {
            return null;
        }
    }

    private static async Task<string> SaveArchiveAsync(
        HttpResponseMessage response, string path, CancellationToken ct)
    {
        await using var input = await response.Content.ReadAsStreamAsync(ct);
        await using var output = new FileStream(
            path, FileMode.Create, FileAccess.Write, FileShare.None,
            128 * 1024, FileOptions.Asynchronous | FileOptions.SequentialScan);
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buffer = new byte[128 * 1024];
        long total = 0;
        while (true)
        {
            var read = await input.ReadAsync(buffer, ct);
            if (read <= 0)
                break;
            total += read;
            if (total > 512L * 1024 * 1024)
                throw new InvalidOperationException("The repository archive is larger than 512 MB.");
            hash.AppendData(buffer, 0, read);
            await output.WriteAsync(buffer.AsMemory(0, read), ct);
        }
        return Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
    }

    private static void ExtractArchive(string archivePath, string destination)
    {
        if (Directory.Exists(destination))
            Directory.Delete(destination, true);
        Directory.CreateDirectory(destination);

        var root = Path.GetFullPath(destination);
        var prefix = root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
                     + Path.DirectorySeparatorChar;

        using var archive = ZipFile.OpenRead(archivePath);
        var names = archive.Entries
            .Where(entry => !string.IsNullOrEmpty(entry.Name))
            .Select(entry => entry.FullName.Replace('\\', '/'))
            .ToList();
        if (names.Count == 0)
            throw new InvalidOperationException("The repository archive was empty.");

        var firstSegments = names
            .Select(name => name.Split('/', StringSplitOptions.RemoveEmptyEntries).FirstOrDefault())
            .Where(name => !string.IsNullOrWhiteSpace(name))
            .Distinct(StringComparer.Ordinal)
            .ToList();
        var stripRoot = firstSegments.Count == 1 &&
                        names.All(name => name.Contains('/', StringComparison.Ordinal));

        long total = 0;
        var count = 0;
        foreach (var entry in archive.Entries)
        {
            if (++count > 60000)
                throw new InvalidOperationException("The repository has too many files.");

            var name = entry.FullName.Replace('\\', '/');
            if (string.IsNullOrEmpty(name) || name.EndsWith("/", StringComparison.Ordinal))
                continue;
            if (name.StartsWith("__MACOSX/", StringComparison.Ordinal))
                continue;
            if (stripRoot)
            {
                var slash = name.IndexOf('/');
                if (slash < 0)
                    continue;
                name = name[(slash + 1)..];
            }
            if (name.Length == 0 || name.Contains("..", StringComparison.Ordinal))
                continue;

            var mode = (entry.ExternalAttributes >> 16) & 0xF000;
            if (mode == 0xA000)
                continue;

            var full = Path.GetFullPath(Path.Combine(destination, name.Replace('/', Path.DirectorySeparatorChar)));
            if (!full.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                continue;

            total += entry.Length;
            if (total > 512L * 1024 * 1024)
                throw new InvalidOperationException("The repository is larger than 512 MB.");

            Directory.CreateDirectory(Path.GetDirectoryName(full) ?? destination);
            using var input = entry.Open();
            using var output = new FileStream(full, FileMode.Create, FileAccess.Write, FileShare.None);
            input.CopyTo(output);
        }
    }

    private static AppPlan DetectPlan(string dir)
    {
        var manifest = DetectManifestPlan(dir);
        if (manifest is not null)
            return manifest;

        var projects = FindPackageProjects(dir)
            .Select(item => ReadProject(item.Path, item.Depth, dir))
            .Where(project => project is not null)
            .Select(project => project!)
            .Where(project => project.HasRunnableScript)
            .OrderByDescending(project => project.Score)
            .ThenBy(project => project.Depth)
            .ToList();

        var electron = projects.FirstOrDefault(project => project.IsElectron);
        ProjectCandidate? project;
        if (electron is not null)
        {
            project = projects
                .Where(candidate => candidate.IsWeb &&
                                    candidate.BuildScript is not null &&
                                    !candidate.IsElectron &&
                                    !candidate.IsAuxiliaryWebsite)
                .OrderByDescending(candidate =>
                    candidate.Score + ElectronRendererBonus(candidate, electron))
                .ThenBy(candidate => candidate.Depth)
                .FirstOrDefault();
            if (project is null)
                throw new InvalidOperationException(ElectronNoRendererMessage());
        }
        else
        {
            project = projects.FirstOrDefault(p => p.IsWeb && p.BuildScript is not null)
                      ?? projects.FirstOrDefault(p => p.IsWeb)
                      ?? projects.FirstOrDefault(p => p.StartScript is not null);
        }
        if (project is not null)
        {
            var manager = DetectPackageManager(project.Directory, dir);
            var port = FindFreePort();
            var setupDir = project.WorkspaceRoot;
            var start = string.IsNullOrWhiteSpace(project.StartScript)
                ? ""
                : NodeRunCommand(manager, project.StartScript, project.Framework, port);
            if (!string.IsNullOrWhiteSpace(project.BuildScript))
            {
                var workspace = !string.IsNullOrWhiteSpace(project.WorkspaceRoot);
                var build = workspace
                    ? WorkspaceRunCommand(manager, project.Name, "build")
                    : PackageRunCommand(manager, "build");
                var setup = CombineCommands(
                    PackageInstallCommand(
                        manager, setupDir ?? project.Directory,
                        workspace ? project.Name : null),
                    build);
                return new AppPlan(
                    "static", setup, start, port, null, true, false,
                    ProjectDir: project.Directory,
                    BuildCommand: build,
                    PackageManager: manager,
                    PreferSol: true,
                    SetupDir: setupDir);
            }
            if (!string.IsNullOrWhiteSpace(start))
            {
                var setup = PackageInstallCommand(
                    manager, setupDir ?? project.Directory,
                    !string.IsNullOrWhiteSpace(project.WorkspaceRoot) ? project.Name : null);
                return new AppPlan(
                    "node", setup, start, port, null, false,
                    string.IsNullOrWhiteSpace(setup),
                    ProjectDir: project.Directory,
                    PackageManager: manager,
                    SetupDir: setupDir);
            }
        }

        foreach (var candidate in new[] { ".", "dist", "build", "out", "public" })
        {
            var root = candidate == "." ? dir : Path.Combine(dir, candidate);
            if (File.Exists(Path.Combine(root, "index.html")))
                return new AppPlan(
                    "static", "", "", 0, root, false, true, root, null, null, true);
        }

        return new AppPlan("none", "", "", 0, null, false, true);
    }

    private static IEnumerable<(string Path, int Depth)> FindPackageProjects(string root)
    {
        var queue = new Queue<(string Directory, int Depth)>();
        queue.Enqueue((root, 0));
        var found = 0;
        while (queue.Count > 0 && found < 200)
        {
            var (directory, depth) = queue.Dequeue();
            var package = Path.Combine(directory, "package.json");
            if (File.Exists(package))
            {
                found++;
                yield return (package, depth);
            }
            if (depth >= 6)
                continue;
            foreach (var child in SafeDirectories(directory))
            {
                if (SkipProjectDirectory(Path.GetFileName(child)))
                    continue;
                try
                {
                    if ((File.GetAttributes(child) & FileAttributes.ReparsePoint) != 0)
                        continue;
                }
                catch
                {
                    continue;
                }
                queue.Enqueue((child, depth + 1));
            }
        }
    }

    private static IEnumerable<string> SafeDirectories(string path)
    {
        try
        {
            return Directory.EnumerateDirectories(path).ToList();
        }
        catch
        {
            return Array.Empty<string>();
        }
    }

    private static bool SkipProjectDirectory(string name) =>
        name.Equals("node_modules", StringComparison.OrdinalIgnoreCase) ||
        name.Equals(".git", StringComparison.OrdinalIgnoreCase) ||
        name.Equals(".hg", StringComparison.OrdinalIgnoreCase) ||
        name.Equals(".svn", StringComparison.OrdinalIgnoreCase) ||
        name.Equals(".next", StringComparison.OrdinalIgnoreCase) ||
        name.Equals(".output", StringComparison.OrdinalIgnoreCase) ||
        name.Equals(".cache", StringComparison.OrdinalIgnoreCase) ||
        name.Equals("dist", StringComparison.OrdinalIgnoreCase) ||
        name.Equals("build", StringComparison.OrdinalIgnoreCase) ||
        name.Equals("out", StringComparison.OrdinalIgnoreCase) ||
        name.Equals("coverage", StringComparison.OrdinalIgnoreCase) ||
        name.Equals("vendor", StringComparison.OrdinalIgnoreCase) ||
        name.Equals("node", StringComparison.OrdinalIgnoreCase) ||
        name.Equals("bin", StringComparison.OrdinalIgnoreCase) ||
        name.Equals("obj", StringComparison.OrdinalIgnoreCase) ||
        name.Equals("target", StringComparison.OrdinalIgnoreCase) ||
        name.Equals(".venv", StringComparison.OrdinalIgnoreCase) ||
        name.Equals("venv", StringComparison.OrdinalIgnoreCase) ||
        name.Equals("__pycache__", StringComparison.OrdinalIgnoreCase);

    private static ProjectCandidate? ReadProject(
        string packagePath, int depth, string repoRoot)
    {
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(packagePath));
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
                return null;

            var deps = ReadDependencyNames(root);
            var build = ReadScript(root, "build");
            var dev = ReadScript(root, "dev");
            var start = ReadScript(root, "start");
            if (build is null && dev is null && start is null)
                return null;
            var startScript = start is not null ? "start" :
                dev is not null ? "dev" : null;

            var projectDir = Path.GetDirectoryName(packagePath) ?? "";
            var name = GetString(root, "name");
            var framework = DetectFramework(deps, build, dev, start);
            var isWeb = framework != "generic" ||
                        LooksLikeWebCommand(build) ||
                        LooksLikeWebCommand(dev) ||
                        LooksLikeWebCommand(start);
            var isElectron = deps.Contains("electron") ||
                             deps.Contains("electron-builder") ||
                             deps.Contains("electron-vite") ||
                             deps.Contains("@electron-forge/cli") ||
                             deps.Contains("@electron-forge/maker-squirrel");
            var isAuxiliaryWebsite =
                IsAuxiliaryWebsite(Path.GetFileName(projectDir)) ||
                IsAuxiliaryWebsite(name);
            var workspaceRoot = FindWorkspaceRoot(projectDir, repoRoot);
            var score = -depth * 5;
            if (deps.Contains("react") || deps.Contains("react-dom"))
                score += 60;
            if (deps.Contains("vite"))
                score += 25;
            if (deps.Contains("next"))
                score += 20;
            if (deps.Contains("astro"))
                score += 15;
            if (build is not null)
                score += 40;
            if (dev is not null)
                score += 5;
            if (start is not null)
                score += 5;
            if (isElectron)
                score -= 100;

            return new ProjectCandidate(
                projectDir,
                depth,
                build,
                startScript,
                framework,
                isWeb,
                score,
                name,
                isElectron,
                workspaceRoot,
                isAuxiliaryWebsite);
        }
        catch
        {
            return null;
        }
    }

    private static string? ReadScript(JsonElement root, string name)
    {
        if (!root.TryGetProperty("scripts", out var scripts) ||
            scripts.ValueKind != JsonValueKind.Object ||
            !scripts.TryGetProperty(name, out var value) ||
            value.ValueKind != JsonValueKind.String)
            return null;
        var text = value.GetString();
        return string.IsNullOrWhiteSpace(text) ? null : text.Trim();
    }

    private static string DetectFramework(
        HashSet<string> deps, string? build, string? dev, string? start)
    {
        var commands = string.Join(" ", build, dev, start).ToLowerInvariant();
        if (deps.Contains("next") || commands.Contains("next "))
            return "next";
        if (deps.Contains("vite") ||
            deps.Contains("astro") ||
            deps.Contains("@sveltejs/kit") ||
            deps.Contains("nuxt") ||
            deps.Contains("@remix-run/dev") ||
            deps.Contains("parcel") ||
            commands.Contains("vite") ||
            commands.Contains("astro") ||
            commands.Contains("webpack"))
            return "vite";
        if (deps.Contains("react") ||
            deps.Contains("react-dom") ||
            commands.Contains("react-scripts"))
            return "react";
        return "generic";
    }

    private static bool LooksLikeWebCommand(string? command)
    {
        if (string.IsNullOrWhiteSpace(command))
            return false;
        return Regex.IsMatch(
            command,
            @"(vite|react-scripts|next|astro|nuxt|svelte|parcel|webpack|remix|gatsby)",
            RegexOptions.IgnoreCase);
    }

    private static bool IsAuxiliaryWebsite(string value)
    {
        if (string.IsNullOrWhiteSpace(value))
            return false;
        foreach (var token in Regex.Split(value.ToLowerInvariant(), "[^a-z0-9]+"))
        {
            if (token is "website" or "site" or "marketing" or "landing" or
                "landingpage" or "www" or "docs" or "documentation" or "blog" or
                "homepage" or "promo")
                return true;
        }
        return false;
    }

    private static int ElectronRendererBonus(
        ProjectCandidate candidate, ProjectCandidate electron)
    {
        var bonus = 0;
        if (!string.IsNullOrWhiteSpace(candidate.WorkspaceRoot) &&
            string.Equals(
                candidate.WorkspaceRoot, electron.WorkspaceRoot,
                StringComparison.OrdinalIgnoreCase))
            bonus += 40;

        try
        {
            var candidateParent = Directory.GetParent(candidate.Directory)?.FullName;
            var electronParent = Directory.GetParent(electron.Directory)?.FullName;
            if (candidateParent is not null && electronParent is not null &&
                string.Equals(candidateParent, electronParent, StringComparison.OrdinalIgnoreCase))
                bonus += 20;
        }
        catch
        {
        }

        var name = Path.GetFileName(candidate.Directory).ToLowerInvariant();
        if (name.Contains("renderer", StringComparison.Ordinal) ||
            name.Contains("web", StringComparison.Ordinal) ||
            name.Contains("client", StringComparison.Ordinal) ||
            name.Contains("frontend", StringComparison.Ordinal) ||
            name.Contains("ui", StringComparison.Ordinal))
            bonus += 25;
        return bonus;
    }

    private static string ElectronNoRendererMessage() =>
        "An Electron app was detected, but no buildable web renderer project was found. " +
        "Soil packages the renderer UI that Electron loads, not Electron itself. " +
        "Add a web app with a build script or a soil.json; website/marketing projects are ignored.";

    private static string? FindWorkspaceRoot(string projectDir, string repoRoot)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(projectDir) ||
                string.IsNullOrWhiteSpace(repoRoot))
                return null;
            var current = Path.GetFullPath(projectDir);
            var root = Path.GetFullPath(repoRoot);
            while (IsWithin(current, root))
            {
                if (HasWorkspaceMarker(current))
                    return current;
                if (PathEquals(current, root))
                    break;
                var parent = Directory.GetParent(current)?.FullName;
                if (string.IsNullOrWhiteSpace(parent))
                    break;
                current = parent;
            }
        }
        catch
        {
        }
        return null;
    }

    private static bool HasWorkspaceMarker(string dir)
    {
        return File.Exists(Path.Combine(dir, "pnpm-workspace.yaml")) ||
               File.Exists(Path.Combine(dir, "lerna.json")) ||
               File.Exists(Path.Combine(dir, "nx.json")) ||
               File.Exists(Path.Combine(dir, "turbo.json")) ||
               File.Exists(Path.Combine(dir, "rush.json")) ||
               HasPackageWorkspaces(dir);
    }

    private static bool HasPackageWorkspaces(string dir)
    {
        var packagePath = Path.Combine(dir, "package.json");
        if (!File.Exists(packagePath))
            return false;
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(packagePath));
            return doc.RootElement.TryGetProperty("workspaces", out var workspaces) &&
                   (workspaces.ValueKind == JsonValueKind.Array ||
                    workspaces.ValueKind == JsonValueKind.Object);
        }
        catch
        {
            return false;
        }
    }

    private static bool IsWithin(string path, string root)
    {
        try
        {
            var relative = Path.GetRelativePath(root, path);
            return relative == "." ||
                   (!relative.StartsWith("..", StringComparison.Ordinal) &&
                    !Path.IsPathRooted(relative));
        }
        catch
        {
            return false;
        }
    }

    private static bool PathEquals(string left, string right)
    {
        try
        {
            return string.Equals(
                Path.GetFullPath(left).TrimEnd(Path.DirectorySeparatorChar),
                Path.GetFullPath(right).TrimEnd(Path.DirectorySeparatorChar),
                StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    private static string DetectPackageManager(string projectDir, string repoDir)
    {
        var directories = new List<string>();
        try
        {
            var current = Path.GetFullPath(projectDir);
            var root = Path.GetFullPath(repoDir);
            while (IsWithin(current, root))
            {
                directories.Add(current);
                if (PathEquals(current, root))
                    break;
                var parent = Directory.GetParent(current)?.FullName;
                if (string.IsNullOrWhiteSpace(parent))
                    break;
                current = parent;
            }
        }
        catch
        {
        }
        directories.Add(repoDir);

        foreach (var directory in directories
                     .Where(path => !string.IsNullOrWhiteSpace(path))
                     .Distinct(StringComparer.OrdinalIgnoreCase))
        {
            if (File.Exists(Path.Combine(directory, "pnpm-lock.yaml")))
                return "pnpm";
            if (File.Exists(Path.Combine(directory, "yarn.lock")))
                return "yarn";
            if (File.Exists(Path.Combine(directory, "bun.lockb")) ||
                File.Exists(Path.Combine(directory, "bun.lock")))
                return "bun";
            if (File.Exists(Path.Combine(directory, "package-lock.json")) ||
                File.Exists(Path.Combine(directory, "npm-shrinkwrap.json")))
                return "npm";
        }
        return "npm";
    }

    private static string PackageInstallCommand(
        string manager, string dir, string? workspacePackage = null)
    {
        var hasLock = manager switch
        {
            "pnpm" => File.Exists(Path.Combine(dir, "pnpm-lock.yaml")),
            "yarn" => File.Exists(Path.Combine(dir, "yarn.lock")),
            "bun" => File.Exists(Path.Combine(dir, "bun.lockb")) ||
                     File.Exists(Path.Combine(dir, "bun.lock")),
            _ => File.Exists(Path.Combine(dir, "package-lock.json")) ||
                 File.Exists(Path.Combine(dir, "npm-shrinkwrap.json"))
        };

        if (manager == "pnpm" &&
            !string.IsNullOrWhiteSpace(workspacePackage))
        {
            var filter = " --filter=" + workspacePackage + "...";
            return hasLock
                ? "pnpm install" + filter + " --frozen-lockfile || pnpm install" + filter
                : "pnpm install" + filter;
        }

        return manager switch
        {
            "pnpm" => hasLock ? "pnpm install --frozen-lockfile || pnpm install" : "pnpm install",
            "yarn" => hasLock ? "yarn install --frozen-lockfile" : "yarn install",
            "bun" => hasLock ? "bun install --frozen-lockfile" : "bun install",
            _ => hasLock
                ? "npm ci --no-audit --no-fund || npm install --no-audit --no-fund"
                : "npm install --no-audit --no-fund"
        };
    }

    private static string PackageRunCommand(string manager, string script) =>
        manager switch
        {
            "pnpm" => "pnpm run " + script,
            "yarn" => "yarn run " + script,
            "bun" => "bun run " + script,
            _ => "npm run " + script
        };

    private static string WorkspaceRunCommand(
        string manager, string packageName, string script)
    {
        if (string.IsNullOrWhiteSpace(packageName))
            return PackageRunCommand(manager, script);
        return manager switch
        {
            "pnpm" => "pnpm --filter=" + packageName + " run " + script,
            "yarn" => "yarn workspace " + packageName + " run " + script,
            "bun" => "bun run --filter=" + packageName + " " + script,
            _ => "npm run " + script + " --workspace=" + packageName
        };
    }

    private static string NodeRunCommand(
        string manager, string script, string framework, int port)
    {
        var command = PackageRunCommand(manager, script);
        if (framework == "next")
            return command + " -- -H 127.0.0.1 -p " + port;
        if (framework == "vite" &&
            script.Equals("dev", StringComparison.OrdinalIgnoreCase))
            return command + " -- --host 127.0.0.1 --port " + port;
        return command;
    }

    private static AppPlan? DetectManifestPlan(string dir)
    {
        foreach (var manifestName in new[] { "soil.json", "soil.app.json" })
        {
            var path = Path.Combine(dir, manifestName);
            if (!File.Exists(path))
                continue;
            try
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(path));
                var root = doc.RootElement;

                var start = GetString(root, "start");
                if (!string.IsNullOrWhiteSpace(start))
                {
                    var setup = GetString(root, "setup");
                    var rootPath = GetString(root, "root");
                    var staticRoot = string.IsNullOrWhiteSpace(rootPath)
                        ? null
                        : SafeSubdir(dir, rootPath);
                    var isStatic = string.IsNullOrWhiteSpace(setup) &&
                                   staticRoot is not null &&
                                   File.Exists(Path.Combine(staticRoot, "index.html")) &&
                                   start.Contains("static", StringComparison.OrdinalIgnoreCase);
                    if (!isStatic && string.IsNullOrWhiteSpace(setup))
                        setup = NpmInstallCommand(dir);
                    return new AppPlan(
                        isStatic ? "static" : "node",
                        setup,
                        start,
                        GetInt(root, "port"),
                        staticRoot,
                        false,
                        isStatic && string.IsNullOrWhiteSpace(setup),
                        PreferSol: isStatic);
                }

                var buildCommand = "";
                var buildRoot = (string?)null;
                if (root.TryGetProperty("build", out var build) &&
                    build.ValueKind == JsonValueKind.Object)
                {
                    buildCommand = GetString(build, "command");
                    var webDir = GetString(build, "webDir");
                    var entry = GetString(build, "entry");
                    if (string.IsNullOrWhiteSpace(entry))
                        entry = "index.html";
                    buildRoot = string.IsNullOrWhiteSpace(webDir)
                        ? null
                        : SafeSubdir(dir, webDir);
                    var builtEntry = buildRoot is null
                        ? null
                        : SafeSubdir(buildRoot, entry);
                    if (builtEntry is not null && File.Exists(builtEntry))
                    {
                        return new AppPlan(
                            "static", "", "", 0, buildRoot, true, true,
                            PreferSol: true);
                    }
                }

                if (root.TryGetProperty("dev", out var dev) &&
                    dev.ValueKind == JsonValueKind.Object)
                {
                    var command = GetString(dev, "command");
                    if (!string.IsNullOrWhiteSpace(command))
                    {
                        var setup = NpmInstallCommand(dir);
                        var port = PortFromUrl(GetString(dev, "url"));
                        if (port <= 0)
                            port = FindFreePort();
                        return new AppPlan(
                            "node", setup, command, port, null, false,
                            string.IsNullOrWhiteSpace(setup));
                    }
                }

                if (!string.IsNullOrWhiteSpace(buildCommand) && buildRoot is not null)
                {
                    var setup = CombineCommands(NpmInstallCommand(dir), buildCommand);
                    return new AppPlan(
                        "static", setup, "", 0, buildRoot, true,
                        string.IsNullOrWhiteSpace(setup),
                        PreferSol: true);
                }

                var entryPath = GetString(root, "entry");
                if (!string.IsNullOrWhiteSpace(entryPath))
                {
                    var entry = SafeSubdir(dir, entryPath);
                    if (entry is not null && File.Exists(entry))
                    {
                        var entryRoot = Path.GetDirectoryName(entry) ?? dir;
                        return new AppPlan(
                            "static", "", "", 0, entryRoot, false, true,
                            PreferSol: true);
                    }
                }
            }
            catch
            {
            }
        }
        return null;
    }

    private static string NpmInstallCommand(string dir)
    {
        if (!File.Exists(Path.Combine(dir, "package.json")))
            return "";
        return PackageInstallCommand("npm", dir);
    }

    private static string? FindBuildOutput(string projectDir, string repoDir)
    {
        var roots = new List<string>();
        foreach (var baseDir in new[] { projectDir, repoDir }
                     .Where(path => !string.IsNullOrWhiteSpace(path))
                     .Distinct(StringComparer.OrdinalIgnoreCase))
        {
            foreach (var sub in new[]
                     {
                         "dist", "build", "out", ".output/public", "storybook-static"
                     })
                roots.Add(Path.Combine(baseDir, sub));
        }

        foreach (var root in roots.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            if (File.Exists(Path.Combine(root, "index.html")))
                return root;
        }

        foreach (var baseDir in new[] { projectDir, repoDir }
                     .Where(path => !string.IsNullOrWhiteSpace(path))
                     .Distinct(StringComparer.OrdinalIgnoreCase))
        {
            foreach (var sub in new[] { "dist", "build", "out" })
            {
                var directory = Path.Combine(baseDir, sub);
                if (!Directory.Exists(directory))
                    continue;
                try
                {
                    var index = Directory
                        .EnumerateFiles(directory, "index.html", SearchOption.AllDirectories)
                        .FirstOrDefault();
                    if (index is not null)
                        return Path.GetDirectoryName(index);
                }
                catch
                {
                }
            }
        }
        return null;
    }

    private static string NoRunnableAppMessage(string dir)
    {
        var names = new List<string>();
        try
        {
            names = Directory.EnumerateFileSystemEntries(dir)
                .Select(Path.GetFileName)
                .Where(name => !string.IsNullOrWhiteSpace(name))
                .Take(12)
                .ToList()!;
        }
        catch
        {
        }

        return names.Count == 0
            ? "No runnable app was found. The repository is empty."
            : "No runnable app was found. Repository root contains: " +
              string.Join(", ", names) +
              ". Add a package.json with a build script or an index.html.";
    }

    private static string RelativeDisplay(string root, string path)
    {
        var relative = Path.GetRelativePath(root, path);
        return relative == "." ? "." : relative.Replace('\\', '/');
    }

    private static string CombineCommands(params string[] commands) =>
        string.Join(" && ", commands
            .Where(command => !string.IsNullOrWhiteSpace(command))
            .Select(command => command.Contains("||", StringComparison.Ordinal)
                ? "(" + command + ")"
                : command));

    private static int PortFromUrl(string value)
    {
        if (Uri.TryCreate(value, UriKind.Absolute, out var uri) &&
            uri.Port is > 0 and < 65536)
        {
            return uri.Port;
        }
        var match = Regex.Match(value ?? "", @":(\d{2,5})(?:/|$)", RegexOptions.IgnoreCase);
        return match.Success && int.TryParse(match.Groups[1].Value, out var port) &&
               port is > 0 and < 65536
            ? port
            : 0;
    }

    private static bool NeedsNode(string command) =>
        !string.IsNullOrWhiteSpace(command) &&
        Regex.IsMatch(
            command,
            @"(^|[\s/\\])""?(node|npm|npx|yarn|pnpm|bun|corepack|vite|next|astro|nuxt)(\.cmd|\.exe)?(""|\s|$)",
            RegexOptions.IgnoreCase);

    private static void EnsureSetupTools(string command)
    {
        if (!NeedsNode(command))
            return;
        EnsureNodeAvailable();
        foreach (var tool in new[] { "pnpm", "yarn", "bun" })
        {
            if (Regex.IsMatch(
                    command,
                    @"(^|[\s/\\])" + tool + @"(\.cmd|\.exe)?(\s|$)",
                    RegexOptions.IgnoreCase))
                EnsureOnPath(tool);
        }
    }

    private static void EnsureOnPath(string tool)
    {
        if (FindOnPath(tool + ".cmd") is not null ||
            FindOnPath(tool + ".exe") is not null ||
            FindOnPath(tool) is not null)
            return;
        throw new InvalidOperationException(
            tool + " is required for this app. Install it and try again.");
    }

    private static HashSet<string> ReadDependencyNames(JsonElement root)
    {
        var result = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var name in new[] { "dependencies", "devDependencies" })
        {
            if (root.TryGetProperty(name, out var deps) && deps.ValueKind == JsonValueKind.Object)
            {
                foreach (var property in deps.EnumerateObject())
                    result.Add(property.Name);
            }
        }
        return result;
    }

    private async Task RunSetupAsync(
        AppRuntime runtime, string dir, string command, CancellationToken ct)
    {
        if (string.IsNullOrWhiteSpace(command))
            return;
        AppendLog(runtime, "$ " + command);
        using var persist = new System.Threading.Timer(
            _ =>
            {
                try
                {
                    m_vms.UpdateAppState(
                        runtime.Id, runtime.Status, null,
                        runtime.HostUrl, runtime.GuestUrl, runtime.Log);
                }
                catch
                {
                }
            },
            null, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(2));
        var result = await RunProcessAsync(
            "cmd.exe",
            new[] { "/d", "/s", "/c", command },
            dir,
            new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
            {
                ["CI"] = "false",
                ["PUBLIC_URL"] = ".",
                ["GENERATE_SOURCEMAP"] = "false",
                ["NO_COLOR"] = "1"
            },
            15 * 60 * 1000,
            runtime,
            ct);
        if (result != 0)
            throw new InvalidOperationException(
                "The app install or build step failed. Review the log above for details.");
    }

    private async Task StartNodeAsync(
        AppRuntime runtime, string dir, AppPlan plan, CancellationToken ct)
    {
        var port = plan.Port > 0 ? plan.Port : FindFreePort();
        var command = string.IsNullOrWhiteSpace(plan.Start)
            ? "npm start"
            : plan.Start.Replace("{port}", port.ToString(), StringComparison.Ordinal);
        var env = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["PORT"] = port.ToString(),
            ["HOST"] = "127.0.0.1",
            ["BROWSER"] = "none",
            ["NO_OPEN"] = "1",
            ["NO_COLOR"] = "1"
        };

        var psi = new ProcessStartInfo
        {
            FileName = "cmd.exe",
            WorkingDirectory = dir,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        psi.ArgumentList.Add("/d");
        psi.ArgumentList.Add("/s");
        psi.ArgumentList.Add("/c");
        psi.ArgumentList.Add(command);
        foreach (var pair in env)
            psi.Environment[pair.Key] = pair.Value;

        var process = new Process { StartInfo = psi, EnableRaisingEvents = true };
        process.OutputDataReceived += (_, args) => AppendLog(runtime, args.Data);
        process.ErrorDataReceived += (_, args) => AppendLog(runtime, args.Data);
        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        runtime.Process = process;

        var startedAt = DateTime.UtcNow;
        var deadline = startedAt.AddMinutes(3);
        var actualPort = port;
        while (DateTime.UtcNow < deadline)
        {
            ct.ThrowIfCancellationRequested();
            if (process.HasExited)
                throw new InvalidOperationException("The app process exited before it became ready.");
            if (TryReadPort(runtime.Log, out var loggedPort))
                actualPort = loggedPort;

            var candidates = new List<int> { actualPort };
            if ((DateTime.UtcNow - startedAt).TotalSeconds >= 5)
                candidates.AddRange(CommonPorts);
            foreach (var candidate in candidates.Distinct())
            {
                if (!await IsPortOpenAsync(candidate, ct))
                    continue;
                runtime.Port = candidate;
                runtime.HostUrl = $"http://127.0.0.1:{candidate}/";
                runtime.GuestUrl = $"http://10.0.2.2:{candidate}/";
                return;
            }
            await Task.Delay(400, ct);
        }
        throw new InvalidOperationException("The app did not start listening in time.");
    }

    private async Task<int> RunProcessAsync(
        string fileName,
        IReadOnlyList<string> args,
        string workingDirectory,
        IReadOnlyDictionary<string, string>? environment,
        int timeoutMs,
        AppRuntime runtime,
        CancellationToken ct)
    {
        var psi = new ProcessStartInfo
        {
            FileName = fileName,
            WorkingDirectory = workingDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        foreach (var arg in args)
            psi.ArgumentList.Add(arg);
        if (environment is not null)
        {
            foreach (var pair in environment)
                psi.Environment[pair.Key] = pair.Value;
        }

        using var process = new Process { StartInfo = psi, EnableRaisingEvents = true };
        process.OutputDataReceived += (_, e) => AppendLog(runtime, e.Data);
        process.ErrorDataReceived += (_, e) => AppendLog(runtime, e.Data);
        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();

        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeout.CancelAfter(timeoutMs);
        try
        {
            await process.WaitForExitAsync(timeout.Token);
        }
        catch (OperationCanceledException)
        {
            try
            {
                process.Kill(true);
            }
            catch
            {
            }
            if (ct.IsCancellationRequested)
                throw;
            throw new InvalidOperationException("The setup step timed out.");
        }
        return process.ExitCode;
    }

    private async Task<bool> TryPackageSolAsync(
        AppRuntime runtime, string staticRoot, CancellationToken ct)
    {
        var tools = Path.Combine(AppContext.BaseDirectory, "soil-tools");
        var soilmk = Path.Combine(tools, "soilmk.py");
        var mksquashfs = Path.Combine(tools, "mksquashfs.py");
        var icon = Path.Combine(tools, "icon.png");
        var python = FindPython();

        if (python is null)
        {
            AppendLog(runtime,
                "WARNING: Python 3.8+ is required to build .sol packages. " +
                "Install Python and retry to use the native Soil runtime.");
            return false;
        }
        if (!File.Exists(soilmk) || !File.Exists(mksquashfs))
        {
            AppendLog(runtime, "WARNING: The bundled Soil SDK packager is missing.");
            return false;
        }
        if (!File.Exists(icon))
        {
            AppendLog(runtime, "WARNING: The bundled app icon is missing.");
            return false;
        }

        var stage = Path.Combine(m_paths.MachineDataDir(runtime.Id), "sol-stage");
        var output = SolPackagePath(runtime);
        try
        {
            if (Directory.Exists(stage))
                Directory.Delete(stage, true);
            Directory.CreateDirectory(stage);
            CopyDirectory(staticRoot, stage);
            File.Copy(icon, Path.Combine(stage, "icon.png"), true);

            var record = m_vms.FindRecord(runtime.Id);
            var name = string.IsNullOrWhiteSpace(record?.Name)
                ? "Imported app"
                : record.Name.Trim();
            if (name.Length > 80)
                name = name[..80];
            var appId = "org.soil." + Regex
                .Replace(runtime.Id.ToLowerInvariant(), "[^a-z0-9]+", "-")
                .Trim('-');
            if (appId.Length > 64)
                appId = appId[..64].TrimEnd('-', '.');
            if (!Regex.IsMatch(appId, @"^[a-z0-9][a-z0-9._-]{1,63}$"))
                appId = "org.soil.app";

            var manifest = new
            {
                schema = 1,
                id = appId,
                name,
                version = "1.0.0",
                kind = "webapp",
                description = "Imported from GitHub.",
                author = "",
                entry = "index.html",
                icon = "icon.png",
                window = new { width = 1152, height = 648, resizable = true },
                permissions = new[]
                {
                    "clipboard", "dialogs", "notifications", "openExternal"
                },
                exec = Array.Empty<string>(),
                install = (object?)null,
                min_ram_mb = 128
            };
            await File.WriteAllTextAsync(
                Path.Combine(stage, "soil.json"),
                JsonSerializer.Serialize(manifest, new JsonSerializerOptions
                {
                    WriteIndented = true
                }),
                new UTF8Encoding(false),
                ct);

            TryDelete(output);
            var args = new List<string>();
            if (Path.GetFileName(python).Equals("py.exe", StringComparison.OrdinalIgnoreCase))
                args.Add("-3");
            args.Add(soilmk);
            args.Add("pack");
            args.Add(stage);
            args.Add("-o");
            args.Add(output);

            SetState(runtime, "packaging", "Packaging app");
            AppendLog(runtime, "Packaging " + Path.GetFileName(output) + "...");
            var result = await RunProcessAsync(
                python,
                args,
                Path.GetDirectoryName(output) ?? m_paths.MachineDataDir(runtime.Id),
                new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
                {
                    ["PYTHONUTF8"] = "1",
                    ["PYTHONIOENCODING"] = "utf-8"
                },
                5 * 60 * 1000,
                runtime,
                ct);
            if (result != 0 || !File.Exists(output) ||
                new FileInfo(output).Length <= 128)
            {
                AppendLog(runtime, "WARNING: The .sol packager did not produce a package.");
                return false;
            }

            runtime.SolAppId = appId;
            AppendLog(runtime,
                "Packaged " + Path.GetFileName(output) + " (" +
                Math.Max(1, new FileInfo(output).Length / 1024) + " KB).");
            return true;
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception ex)
        {
            AppendLog(runtime, "WARNING: .sol packaging failed: " + ex.Message);
            return false;
        }
        finally
        {
            try
            {
                if (Directory.Exists(stage))
                    Directory.Delete(stage, true);
            }
            catch
            {
            }
        }
    }

    private static void CopyDirectory(string source, string destination)
    {
        var skipped = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            ".git", ".hg", ".svn", "node_modules", "__pycache__", ".cache", "coverage"
        };
        long total = 0;
        foreach (var file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
        {
            var relative = Path.GetRelativePath(source, file);
            var segments = relative.Split(
                new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
                StringSplitOptions.RemoveEmptyEntries);
            if (segments.Any(segment => skipped.Contains(segment)))
                continue;
            if (Path.GetExtension(file).Equals(".sol", StringComparison.OrdinalIgnoreCase))
                continue;

            var info = new FileInfo(file);
            total += info.Length;
            if (total > 512L * 1024 * 1024)
                throw new InvalidOperationException("The app payload is larger than 512 MB.");

            var target = Path.Combine(destination, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(target) ?? destination);
            File.Copy(file, target, true);
        }
    }

    private static string? FindPython()
    {
        foreach (var name in new[] { "py.exe", "python.exe", "python3.exe" })
        {
            var path = FindOnPath(name);
            if (path is not null)
                return path;
        }
        return null;
    }

    private void SetState(AppRuntime runtime, string status, string? message)
    {
        runtime.Status = status;
        runtime.Error = null;
        if (!string.IsNullOrWhiteSpace(message))
            AppendLog(runtime, message + "...");
        m_vms.UpdateAppState(runtime.Id, status, null, runtime.HostUrl, runtime.GuestUrl, runtime.Log);
    }

    private void Fail(AppRuntime runtime, Exception error)
    {
        if (string.Equals(runtime.Status, "stopped", StringComparison.Ordinal))
            return;
        runtime.Ready = false;
        runtime.Status = "failed";
        runtime.Error = error.Message;
        AppendLog(runtime, "ERROR: " + error.Message);
        CleanupRuntime(runtime);
        m_vms.UpdateAppState(
            runtime.Id, "failed", error.Message, null, null, runtime.Log);
    }

    private static void AppendLog(AppRuntime runtime, string? line)
    {
        if (string.IsNullOrWhiteSpace(line))
            return;
        lock (runtime.LogLock)
        {
            var text = runtime.Log + line.TrimEnd() + "\n";
            runtime.Log = text.Length > 12000 ? text[^12000..] : text;
        }
    }

    private void StopRuntime(AppRuntime runtime)
    {
        runtime.Ready = false;
        try
        {
            runtime.WorkCts.Cancel();
        }
        catch
        {
        }
        CleanupRuntime(runtime);
        runtime.Status = "stopped";
        if (m_vms.FindRecord(runtime.Id) is not null)
            m_vms.UpdateAppState(runtime.Id, "stopped", null, null, null, runtime.Log);
    }

    private static void CleanupRuntime(AppRuntime runtime)
    {
        try
        {
            runtime.Static?.Dispose();
        }
        catch
        {
        }
        runtime.Static = null;
        try
        {
            if (runtime.Process is { HasExited: false })
                runtime.Process.Kill(true);
        }
        catch
        {
        }
        try
        {
            runtime.Process?.Dispose();
        }
        catch
        {
        }
        runtime.Process = null;
        runtime.HostUrl = null;
        runtime.GuestUrl = null;
        runtime.SolPath = null;
        runtime.SolAppId = null;
    }

    private static bool IsRuntimeAlive(AppRuntime runtime)
    {
        if (!runtime.Ready)
            return false;
        if (runtime.Kind == "sol")
            return !string.IsNullOrWhiteSpace(runtime.SolPath) &&
                   File.Exists(runtime.SolPath);
        if (runtime.Kind == "static")
            return runtime.Static is not null;
        if (runtime.Kind == "node")
            return runtime.Process is { HasExited: false };
        return false;
    }

    private static bool TryReadPort(string log, out int port)
    {
        port = 0;
        var match = Regex.Match(
            log,
            @"https?://(?:localhost|127\.0\.0\.1|0\.0\.0\.0|\[::1?\]):(\d{2,5})",
            RegexOptions.IgnoreCase);
        return match.Success && int.TryParse(match.Groups[1].Value, out port);
    }

    private static async Task<bool> IsPortOpenAsync(int port, CancellationToken ct)
    {
        try
        {
            using var client = new TcpClient();
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeout.CancelAfter(500);
            await client.ConnectAsync(IPAddress.Loopback, port, timeout.Token);
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static int FindFreePort()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        return port;
    }

    private static void EnsureNodeAvailable()
    {
        if (FindOnPath("node.exe") is null || FindOnPath("npm.cmd") is null)
            throw new InvalidOperationException(
                "Node.js is required for this app. Install Node.js 20 or newer and try again.");
    }

    private static string? FindOnPath(string fileName)
    {
        foreach (var directory in (Environment.GetEnvironmentVariable("PATH") ?? "")
                     .Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            try
            {
                var candidate = Path.Combine(directory.Trim('"'), fileName);
                if (File.Exists(candidate))
                    return candidate;
            }
            catch
            {
            }
        }
        return null;
    }

    private static RepoRef ParseRepo(AppImportRequest request)
    {
        var owner = (request.Owner ?? "").Trim();
        var repo = (request.Repo ?? "").Trim();
        if (owner.Length > 0 && repo.Length > 0)
        {
            ValidatePart(owner, "repository owner");
            ValidatePart(repo, "repository name");
            return new RepoRef(owner, repo, $"https://github.com/{owner}/{repo}");
        }

        var text = (request.RepoUrl ?? "").Trim();
        if (text.Length == 0)
            throw new ArgumentException("Choose a repository or paste its GitHub URL.");
        if (text.StartsWith("github.com/", StringComparison.OrdinalIgnoreCase))
            text = "https://" + text;
        if (Uri.TryCreate(text, UriKind.Absolute, out var uri) &&
            uri.Host.Equals("github.com", StringComparison.OrdinalIgnoreCase))
        {
            var parts = uri.AbsolutePath.Trim('/').Split('/');
            if (parts.Length >= 2)
            {
                owner = parts[0];
                repo = parts[1].EndsWith(".git", StringComparison.OrdinalIgnoreCase)
                    ? parts[1][..^4]
                    : parts[1];
                ValidatePart(owner, "repository owner");
                ValidatePart(repo, "repository name");
                return new RepoRef(owner, repo, $"https://github.com/{owner}/{repo}");
            }
        }

        var pieces = text.Trim('/').Split('/');
        if (pieces.Length == 2)
        {
            owner = pieces[0];
            repo = pieces[1];
            ValidatePart(owner, "repository owner");
            ValidatePart(repo, "repository name");
            return new RepoRef(owner, repo, $"https://github.com/{owner}/{repo}");
        }
        throw new ArgumentException("Enter a GitHub repository URL like https://github.com/owner/repo.");
    }

    private static void ValidatePart(string value, string label)
    {
        if (!Regex.IsMatch(value, @"^[A-Za-z0-9_.-]{1,100}$"))
            throw new ArgumentException("Invalid " + label + ".");
    }

    private static string GetString(JsonElement root, string name)
    {
        return root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? ""
            : "";
    }

    private static int GetInt(JsonElement root, string name)
    {
        return root.TryGetProperty(name, out var value) &&
               value.ValueKind == JsonValueKind.Number &&
               value.TryGetInt32(out var result)
            ? result
            : 0;
    }

    private static string? SafeSubdir(string root, string relative)
    {
        if (relative.Contains("..", StringComparison.Ordinal))
            return null;
        var full = Path.GetFullPath(Path.Combine(root, relative));
        var prefix = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) +
                     Path.DirectorySeparatorChar;
        return full.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) ? full : null;
    }

    private string AppDir(string id) => Path.Combine(m_paths.MachineDataDir(id), "app");

    private string SolPackagePath(AppRuntime runtime) =>
        Path.Combine(m_paths.MachineDataDir(runtime.Id), "app.sol");

    private string ImportMarker(AppRuntime runtime) =>
        Path.Combine(m_paths.MachineDataDir(runtime.Id), ".app-imported");

    private string SetupMarker(AppRuntime runtime) =>
        Path.Combine(m_paths.MachineDataDir(runtime.Id), ".app-setup");

    private string ArchiveMarker(AppRuntime runtime) =>
        Path.Combine(m_paths.MachineDataDir(runtime.Id), ".app-archive");

    private async Task<string?> ReadArchiveHashAsync(
        AppRuntime runtime, CancellationToken ct)
    {
        var path = ArchiveMarker(runtime);
        if (!File.Exists(path))
            return null;
        var value = (await File.ReadAllTextAsync(path, ct)).Trim().ToLowerInvariant();
        return value.Length == 64 && value.All(Uri.IsHexDigit) ? value : null;
    }

    private BuildCacheEntry? FindCachedBuild(string key, string? archiveHash)
    {
        try
        {
            var root = Path.Combine(m_paths.BuildCacheDir, key);
            var dist = Path.Combine(root, "dist");
            var metadataPath = Path.Combine(root, "meta.json");

            if (File.Exists(Path.Combine(dist, "index.html")))
            {
                if (!File.Exists(metadataPath))
                    return new BuildCacheEntry(key, dist, null);

                var metadata = JsonSerializer.Deserialize<BuildCacheMetadata>(
                    File.ReadAllText(metadataPath));
                if (metadata is null ||
                    metadata.Schema != 2 ||
                    !string.Equals(metadata.Key, key, StringComparison.Ordinal) ||
                    !string.Equals(
                        metadata.ArchiveHash, archiveHash,
                        StringComparison.OrdinalIgnoreCase))
                    return null;
                return new BuildCacheEntry(key, dist, metadata);
            }

            if (File.Exists(Path.Combine(root, "index.html")))
                return new BuildCacheEntry(key, root, null);
        }
        catch
        {
        }
        return null;
    }

    private BuildCacheEntry? CacheBuild(
        AppRuntime runtime,
        string key,
        string? archiveHash,
        string repoDir,
        AppPlan plan,
        string root)
    {
        if (string.IsNullOrWhiteSpace(archiveHash))
            return null;

        var existing = FindCachedBuild(key, archiveHash);
        if (existing is not null)
            return existing;

        var cacheRoot = m_paths.BuildCacheDir;
        var target = Path.Combine(cacheRoot, key);
        var staging = target + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            var dist = Path.Combine(staging, "dist");
            Directory.CreateDirectory(dist);
            CopyDirectory(root, dist);
            if (!File.Exists(Path.Combine(dist, "index.html")))
            {
                Directory.Delete(staging, true);
                return null;
            }

            var measured = MeasureDirectory(dist);
            var metadata = new BuildCacheMetadata
            {
                Schema = 2,
                Key = key,
                ArchiveHash = archiveHash ?? "",
                Project = PlanPath(repoDir, plan.ProjectDir),
                Setup = PlanPath(repoDir, plan.SetupDir ?? plan.ProjectDir),
                StaticRoot = PlanPath(repoDir, plan.StaticRoot),
                SetupCommand = plan.SetupCommand,
                BuildCommand = plan.BuildCommand ?? "",
                PackageManager = plan.PackageManager ?? "",
                Toolchain = ToolchainSignature(plan),
                CreatedUtc = DateTime.UtcNow,
                Files = measured.Files,
                Bytes = measured.Bytes
            };
            File.WriteAllText(
                Path.Combine(staging, "meta.json"),
                JsonSerializer.Serialize(
                    metadata, new JsonSerializerOptions { WriteIndented = true }),
                new UTF8Encoding(false));

            Directory.CreateDirectory(cacheRoot);
            var raced = FindCachedBuild(key, archiveHash);
            if (raced is not null)
            {
                Directory.Delete(staging, true);
                return raced;
            }
            if (Directory.Exists(target))
                Directory.Delete(target, true);
            Directory.Move(staging, target);

            AppendLog(runtime,
                "Cached build (" + Math.Max(1, measured.Bytes / 1024) + " KB).");
            return new BuildCacheEntry(
                key, Path.Combine(target, "dist"), metadata);
        }
        catch (Exception ex)
        {
            try
            {
                if (Directory.Exists(staging))
                    Directory.Delete(staging, true);
            }
            catch
            {
            }
            AppendLog(runtime, "WARNING: Could not cache the build: " + ex.Message);
            return null;
        }
    }

    private static string? BuildCacheKey(
        AppRuntime runtime, string repoDir, AppPlan plan)
    {
        if (string.IsNullOrWhiteSpace(runtime.ArchiveHash) ||
            plan.Kind != "static" ||
            string.IsNullOrWhiteSpace(plan.BuildCommand) ||
            !plan.PreferSol)
            return null;

        var value = string.Join(
            "\n",
            "soil-build-cache-v3",
            runtime.ArchiveHash,
            PlanPath(repoDir, plan.ProjectDir),
            PlanPath(repoDir, plan.SetupDir ?? plan.ProjectDir),
            PlanPath(repoDir, plan.StaticRoot),
            plan.SetupCommand,
            plan.BuildCommand,
            ToolchainSignature(plan),
            Environment.GetEnvironmentVariable("SOIL_BUILD_CACHE_SALT") ?? "");
        return Convert.ToHexString(
            SHA256.HashData(Encoding.UTF8.GetBytes(value))).ToLowerInvariant();
    }

    private static string ToolchainSignature(AppPlan plan) =>
        "node=" + NodeVersion.Value +
        ";dotnet=" + Environment.Version +
        ";arch=" + RuntimeInformation.ProcessArchitecture +
        ";manager=" + (plan.PackageManager ?? "");

    private static string PlanPath(string root, string? path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return ".";
        try
        {
            return RelativeDisplay(root, path);
        }
        catch
        {
            return path ?? ".";
        }
    }

    private static (int Files, long Bytes) MeasureDirectory(string root)
    {
        var files = 0;
        long bytes = 0;
        foreach (var file in Directory.EnumerateFiles(
                     root, "*", SearchOption.AllDirectories))
        {
            files++;
            try
            {
                bytes += new FileInfo(file).Length;
            }
            catch
            {
            }
        }
        return (files, bytes);
    }

    private static string ReadNodeVersion()
    {
        try
        {
            var info = new ProcessStartInfo
            {
                FileName = "node",
                Arguments = "--version",
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            using var process = Process.Start(info);
            if (process is null)
                return "";
            var value = process.StandardOutput.ReadToEnd().Trim();
            process.WaitForExit(2000);
            return value;
        }
        catch
        {
            return "";
        }
    }

    private static string FormatDuration(TimeSpan value) =>
        value.TotalSeconds < 1
            ? value.TotalMilliseconds.ToString("0") + " ms"
            : value.TotalSeconds.ToString("0.0") + " s";

    private static bool CanUseWarmWorkspace(AppPlan plan) =>
        plan.Kind == "static" &&
        plan.PreferSol &&
        !string.IsNullOrWhiteSpace(plan.BuildCommand) &&
        !string.IsNullOrWhiteSpace(plan.SetupCommand);

    private async Task<WorkspaceLease?> TryAcquireWorkspaceAsync(
        AppRuntime runtime,
        AppImportRequest request,
        string sourceDir,
        AppPlan sourcePlan,
        CancellationToken ct)
    {
        var key = WorkspaceKey(request);
        var gate = m_workspaceLocks.GetOrAdd(key, _ => new SemaphoreSlim(1, 1));
        await gate.WaitAsync(ct);
        try
        {
            var root = Path.Combine(m_paths.CacheDir, "workspaces", key);
            Directory.CreateDirectory(root);
            SyncWorkspaceSource(sourceDir, root);

            var workspacePlan = DetectPlan(root);
            if (workspacePlan.Kind != "static" ||
                !SamePlanShape(sourceDir, sourcePlan, root, workspacePlan))
            {
                AppendLog(runtime,
                    "Warm workspace was not usable; using the machine copy.");
                gate.Release();
                return null;
            }

            AppendLog(runtime,
                "Warm workspace ready; reusing installed dependencies.");
            return new WorkspaceLease(root, workspacePlan, gate);
        }
        catch (OperationCanceledException)
        {
            gate.Release();
            throw;
        }
        catch (Exception ex)
        {
            gate.Release();
            AppendLog(runtime,
                "WARNING: Warm workspace unavailable: " + ex.Message);
            return null;
        }
    }

    private static string WorkspaceKey(AppImportRequest request)
    {
        var repo = ParseRepo(request);
        var reference = string.IsNullOrWhiteSpace(request.Ref)
            ? "default"
            : request.Ref.Trim();
        var value = repo.Url.ToLowerInvariant() + "\n" + reference.ToLowerInvariant();
        return Convert.ToHexString(
            SHA256.HashData(Encoding.UTF8.GetBytes(value))).ToLowerInvariant();
    }

    private static bool SamePlanShape(
        string sourceRoot,
        AppPlan source,
        string workspaceRoot,
        AppPlan workspace)
    {
        return workspace.Kind == "static" &&
               workspace.PreferSol == source.PreferSol &&
               string.Equals(
                   source.BuildCommand, workspace.BuildCommand,
                   StringComparison.Ordinal) &&
               string.Equals(
                   source.SetupCommand, workspace.SetupCommand,
                   StringComparison.Ordinal) &&
               string.Equals(
                   source.PackageManager, workspace.PackageManager,
                   StringComparison.OrdinalIgnoreCase) &&
               string.Equals(
                   PlanPath(sourceRoot, source.ProjectDir),
                   PlanPath(workspaceRoot, workspace.ProjectDir),
                   StringComparison.OrdinalIgnoreCase) &&
               string.Equals(
                   PlanPath(sourceRoot, source.SetupDir ?? source.ProjectDir),
                   PlanPath(workspaceRoot, workspace.SetupDir ?? workspace.ProjectDir),
                   StringComparison.OrdinalIgnoreCase) &&
               string.Equals(
                   PlanPath(sourceRoot, source.StaticRoot),
                   PlanPath(workspaceRoot, workspace.StaticRoot),
                   StringComparison.OrdinalIgnoreCase);
    }

    private static void SyncWorkspaceSource(string source, string destination)
    {
        Directory.CreateDirectory(destination);
        DeleteWorkspaceSource(destination);
        CopyDirectory(source, destination);
    }

    private static void DeleteWorkspaceSource(string directory)
    {
        foreach (var entry in Directory.EnumerateFileSystemEntries(directory))
        {
            var name = Path.GetFileName(entry);
            if (name.Equals("node_modules", StringComparison.OrdinalIgnoreCase))
                continue;

            if (Directory.Exists(entry))
            {
                DeleteWorkspaceSource(entry);
                try
                {
                    if (!Directory.EnumerateFileSystemEntries(entry).Any())
                        Directory.Delete(entry);
                }
                catch
                {
                }
            }
            else
            {
                try
                {
                    File.Delete(entry);
                }
                catch
                {
                }
            }
        }
    }

    private string SnapshotStaticRoot(AppRuntime runtime, string root)
    {
        var snapshot = Path.Combine(m_paths.MachineDataDir(runtime.Id), "static");
        try
        {
            if (Directory.Exists(snapshot))
                Directory.Delete(snapshot, true);
            CopyDirectory(root, snapshot);
            return snapshot;
        }
        catch (Exception ex)
        {
            AppendLog(runtime,
                "WARNING: Could not snapshot the warm build: " + ex.Message);
            return root;
        }
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path))
                File.Delete(path);
        }
        catch
        {
        }
    }

    private AppRuntime CreateRuntime(string id) =>
        new(id, AppDir(id), CreateWorkToken());

    private CancellationTokenSource CreateWorkToken() =>
        CancellationTokenSource.CreateLinkedTokenSource(m_cts.Token);

    private static HttpClient CreateHttpClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("Soil/0.3");
        return client;
    }

    public void Dispose()
    {
        try
        {
            m_cts.Cancel();
        }
        catch
        {
        }
        foreach (var runtime in m_runtimes.Values)
            StopRuntime(runtime);
        m_cts.Dispose();
    }

    private sealed record RepoRef(string Owner, string Name, string Url);

    private sealed record AppPlan(
        string Kind,
        string SetupCommand,
        string Start,
        int Port,
        string? StaticRoot,
        bool SpaFallback,
        bool SkipSetup,
        string? ProjectDir = null,
        string? BuildCommand = null,
        string? PackageManager = null,
        bool PreferSol = false,
        string? SetupDir = null);

    private sealed record BuildCacheEntry(
        string Key,
        string DistRoot,
        BuildCacheMetadata? Metadata);

    private sealed class BuildCacheMetadata
    {
        public BuildCacheMetadata()
        {
        }

        public int Schema { get; set; }
        public string Key { get; set; } = "";
        public string ArchiveHash { get; set; } = "";
        public string Project { get; set; } = "";
        public string Setup { get; set; } = "";
        public string StaticRoot { get; set; } = "";
        public string SetupCommand { get; set; } = "";
        public string BuildCommand { get; set; } = "";
        public string PackageManager { get; set; } = "";
        public string Toolchain { get; set; } = "";
        public DateTime CreatedUtc { get; set; }
        public int Files { get; set; }
        public long Bytes { get; set; }
    }

    private sealed class WorkspaceLease : IDisposable
    {
        private readonly SemaphoreSlim m_gate;
        private int m_disposed;

        public WorkspaceLease(string root, AppPlan plan, SemaphoreSlim gate)
        {
            Root = root;
            Plan = plan;
            m_gate = gate;
        }

        public string Root { get; }
        public AppPlan Plan { get; }

        public void Dispose()
        {
            if (Interlocked.Exchange(ref m_disposed, 1) == 0)
                m_gate.Release();
        }
    }

    private sealed record ProjectCandidate(
        string Directory,
        int Depth,
        string? BuildScript,
        string? StartScript,
        string Framework,
        bool IsWeb,
        int Score,
        string Name,
        bool IsElectron,
        string? WorkspaceRoot,
        bool IsAuxiliaryWebsite)
    {
        public bool HasRunnableScript =>
            BuildScript is not null || StartScript is not null;
    }

    private sealed class AppRuntime
    {
        public AppRuntime(string id, string sourceDir, CancellationTokenSource workCts)
        {
            Id = id;
            SourceDir = sourceDir;
            WorkCts = workCts;
        }

        public string Id { get; }
        public string SourceDir { get; }
        public CancellationTokenSource WorkCts { get; set; }
        public SemaphoreSlim Gate { get; } = new(1, 1);
        public object LogLock { get; } = new();
        public Task? Task { get; set; }
        public Process? Process { get; set; }
        public StaticSiteServer? Static { get; set; }
        public string Kind { get; set; } = "none";
        public string Status { get; set; } = "queued";
        public string? Error { get; set; }
        public string Log { get; set; } = "";
        public string? HostUrl { get; set; }
        public string? GuestUrl { get; set; }
        public string? SolPath { get; set; }
        public string? SolAppId { get; set; }
        public string? ManifestStart { get; set; }
        public string? ArchiveHash { get; set; }
        public bool SpaFallback { get; set; }
        public bool Ready { get; set; }
        public int Port { get; set; }
    }
}
