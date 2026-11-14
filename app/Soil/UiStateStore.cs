using System.Text.Json;

namespace Soil;

public sealed record UiStateDto(
    bool OnboardingComplete,
    string Purpose,
    List<string> Agents,
    string Package,
    string Theme);

public sealed record UiStateUpdate(
    bool? OnboardingComplete,
    string? Purpose,
    List<string>? Agents,
    string? Package,
    string? Theme);

public sealed class UiStateStore
{
    private static readonly string[] ValidPurposes = ["agents", "testing", "vmlab"];
    private static readonly string[] ValidPackages = ["tiny", "nano", "standard", "pro", "wide"];
    private static readonly string[] ValidThemes = ["light", "dark"];
    private readonly string m_path;
    private readonly object m_lock = new();

    public UiStateStore(SoilPaths paths)
    {
        m_path = Path.Combine(paths.Root, "ui-state.json");
    }

    public UiStateDto Get()
    {
        lock (m_lock)
            return Read();
    }

    public UiStateDto Update(UiStateUpdate update)
    {
        lock (m_lock)
        {
            var state = Read();
            var purpose = Normalize(update.Purpose, ValidPurposes, state.Purpose);
            var package = Normalize(update.Package, ValidPackages, state.Package);
            var theme = Normalize(update.Theme, ValidThemes, state.Theme);
            var agents = update.Agents is null
                ? state.Agents
                : update.Agents
                    .Where(a => !string.IsNullOrWhiteSpace(a))
                    .Select(a => a.Trim().ToLowerInvariant())
                    .Where(a => a is "codex" or "claude" or "antigravity" or "sparky")
                    .Distinct(StringComparer.Ordinal)
                    .Take(8)
                    .ToList();
            if (agents.Count == 0)
                agents = state.Agents;

            var next = new UiStateDto(
                update.OnboardingComplete ?? state.OnboardingComplete,
                purpose,
                agents,
                package,
                theme);
            try
            {
                File.WriteAllText(m_path,
                    JsonSerializer.Serialize(next,
                        new JsonSerializerOptions(JsonSerializerDefaults.Web)
                        {
                            WriteIndented = true
                        }));
            }
            catch
            {
            }
            return next;
        }
    }

    private UiStateDto Read()
    {
        try
        {
            if (File.Exists(m_path))
            {
                var state = JsonSerializer.Deserialize<UiStateDto>(
                    File.ReadAllText(m_path),
                    new JsonSerializerOptions(JsonSerializerDefaults.Web));
                if (state is not null)
                {
                    return state with
                    {
                        Purpose = Normalize(state.Purpose, ValidPurposes, "testing"),
                        Agents = state.Agents is null || state.Agents.Count == 0
                            ? DefaultAgents()
                            : state.Agents,
                        Package = Normalize(state.Package, ValidPackages, "tiny"),
                        Theme = Normalize(state.Theme, ValidThemes, "light")
                    };
                }
            }
        }
        catch
        {
        }
        return new UiStateDto(false, "testing", DefaultAgents(), "tiny", "light");
    }

    private static List<string> DefaultAgents() => ["codex", "claude", "antigravity", "sparky"];

    private static string Normalize(string? value, string[] allowed, string fallback)
    {
        var text = (value ?? "").Trim().ToLowerInvariant();
        return allowed.Contains(text, StringComparer.Ordinal) ? text : fallback;
    }
}
