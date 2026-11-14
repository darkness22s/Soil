using System.Text.Json;

namespace Soil;

public sealed class SoilPaths
{
    public string Root { get; }

    public string MachinesDir => Path.Combine(Root, "machines");
    public string MachinesIndexPath => Path.Combine(Root, "machines.json");
    public string AppsDir => Path.Combine(Root, "apps");
    public string SkillsDir => Path.Combine(Root, "skills");
    public string CacheDir => Path.Combine(Root, "cache");
    public string BuildCacheDir
    {
        get
        {
            var shared = Environment.GetEnvironmentVariable("SOIL_BUILD_CACHE");
            if (!string.IsNullOrWhiteSpace(shared))
            {
                try
                {
                    return Path.GetFullPath(shared);
                }
                catch
                {
                }
            }
            return Path.Combine(CacheDir, "builds");
        }
    }
    public string LogsDir => Path.Combine(Root, "logs");
    public string KeysDir => Path.Combine(Root, "keys");
    public string SshDir => Path.Combine(KeysDir, "ssh");
    public string SshKeyPath => Path.Combine(SshDir, "id_ed25519");
    public string WebViewDir => Path.Combine(Root, "webview2");
    public string ImagesDir => Path.Combine(Root, "images");
    public string ConfigPath => Path.Combine(Root, "config.json");
    public string ReadmePath => Path.Combine(Root, "README.txt");
    public string CliEndpointPath => Path.Combine(Root, "cli-endpoint.json");

    public static SoilPaths CreateDefault()
    {
        var profile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (string.IsNullOrWhiteSpace(profile))
            profile = Environment.GetEnvironmentVariable("USERPROFILE") ?? Environment.CurrentDirectory;
        return new SoilPaths(Path.Combine(profile, ".soil"));
    }

    public SoilPaths(string root)
    {
        if (string.IsNullOrWhiteSpace(root))
            throw new ArgumentException("A Soil home is required.", nameof(root));
        Root = Path.GetFullPath(root);
    }

    public void Ensure()
    {
        Directory.CreateDirectory(Root);
        Directory.CreateDirectory(MachinesDir);
        Directory.CreateDirectory(AppsDir);
        Directory.CreateDirectory(SkillsDir);
        Directory.CreateDirectory(CacheDir);
        Directory.CreateDirectory(LogsDir);
        Directory.CreateDirectory(SshDir);
        Directory.CreateDirectory(WebViewDir);
        Directory.CreateDirectory(ImagesDir);

        if (!File.Exists(ReadmePath))
        {
            File.WriteAllText(ReadmePath,
                """
                Soil home
                =========

                Soil keeps user data here.

                  machines/   one folder per registered machine
                  apps/       installed Soil applications
                  skills/     local skills and agent helpers
                  cache/      disposable downloads and build cache
                  logs/       application logs
                  keys/       local keys used by Soil
                  images/     optional VM images placed by the user
                  webview2/   local WebView2 profile

                Do not delete machines/ or apps/ unless you intend to remove
                their contents. Soil creates this folder on first launch and
                from the installer bootstrap.
                """);
        }

        if (!File.Exists(ConfigPath))
        {
            var config = new SoilConfig
            {
                Schema = 1,
                Product = "Soil",
                Layout = 1,
                CreatedUtc = DateTime.UtcNow
            };
            File.WriteAllText(ConfigPath, JsonSerializer.Serialize(
                config, new JsonSerializerOptions { WriteIndented = true }));
        }
    }

    public string MachineDir(string id)
    {
        if (!IsSafeId(id))
            throw new ArgumentException("Invalid machine id.", nameof(id));
        return Path.Combine(MachinesDir, id);
    }

    public string MachineConfigPath(string id) => Path.Combine(MachineDir(id), "machine.json");
    public string MachineLogDir(string id) => Path.Combine(MachineDir(id), "logs");
    public string MachineLogPath(string id) => Path.Combine(MachineLogDir(id), "vm.log");
    public string MachineDataDir(string id) => Path.Combine(MachineDir(id), "data");
    public string MachineDiskPath(string id) => Path.Combine(MachineDataDir(id), "disk.img");

    public string EnsureMachine(string id)
    {
        var dir = MachineDir(id);
        Directory.CreateDirectory(dir);
        Directory.CreateDirectory(MachineLogDir(id));
        Directory.CreateDirectory(MachineDataDir(id));
        return dir;
    }

    public string Resolve(string? relative)
    {
        var rel = (relative ?? "").Replace('\\', '/').Trim('/');
        var segments = rel.Split('/', StringSplitOptions.RemoveEmptyEntries);
        if (segments.Any(s => s is "." or ".." || s.Contains(':')))
            throw new ArgumentException("Invalid Soil-relative path.", nameof(relative));

        var full = Path.GetFullPath(segments.Length == 0
            ? Root
            : Path.Combine([Root, .. segments]));

        if (!IsInside(full))
            throw new ArgumentException("Path escapes the Soil home.", nameof(relative));

        var current = Root;
        foreach (var segment in segments)
        {
            current = Path.Combine(current, segment);
            if (!File.Exists(current) && !Directory.Exists(current))
                continue;
            var attributes = File.GetAttributes(current);
            if ((attributes & FileAttributes.ReparsePoint) != 0)
                throw new ArgumentException("Links are not browsable in the Soil home.", nameof(relative));
        }

        return full;
    }

    public string Relative(string full)
    {
        full = Path.GetFullPath(full);
        if (!IsInside(full))
            throw new ArgumentException("Path escapes the Soil home.", nameof(full));
        var rel = Path.GetRelativePath(Root, full).Replace('\\', '/');
        return rel == "." ? "" : rel;
    }

    public static bool IsSafeId(string? id) =>
        !string.IsNullOrWhiteSpace(id) && Guid.TryParseExact(id, "N", out _);

    private bool IsInside(string full)
    {
        var comparison = OperatingSystem.IsWindows()
            ? StringComparison.OrdinalIgnoreCase
            : StringComparison.Ordinal;
        var prefix = Root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
                     + Path.DirectorySeparatorChar;
        return full.Equals(Root, comparison) || full.StartsWith(prefix, comparison);
    }

    private sealed class SoilConfig
    {
        public int Schema { get; set; }
        public string Product { get; set; } = "Soil";
        public int Layout { get; set; }
        public DateTime CreatedUtc { get; set; }
    }
}
