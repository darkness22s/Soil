namespace Soil;

internal static class SkillInstaller
{
    private const string SkillName = "soil-control";

    public static IReadOnlyList<SkillInstallResult> Install(string skillSource)
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (string.IsNullOrWhiteSpace(home))
            home = Environment.GetEnvironmentVariable("USERPROFILE") ?? Environment.CurrentDirectory;

        var claudeHome = ConfigRoot("CLAUDE_CONFIG_DIR", Path.Combine(home, ".claude"));
        var codexOverride = Environment.GetEnvironmentVariable("CODEX_HOME");
        var codexHome = ConfigRoot("CODEX_HOME", Path.Combine(home, ".codex"));
        var codexSkills = string.IsNullOrWhiteSpace(codexOverride)
            ? Path.Combine(home, ".agents", "skills")
            : Path.Combine(codexHome, "skills");
        var sparkyHome = ConfigRoot("SPARKY_HOME", Path.Combine(home, ".sparky"));
        var xdgHome = ConfigRoot("XDG_CONFIG_HOME", Path.Combine(home, ".config"));
        var openCodeHome = Path.Combine(xdgHome, "opencode");

        var candidates = new (string Name, bool Detected, string Directory)[]
        {
            ("Claude Code", Directory.Exists(claudeHome), Path.Combine(claudeHome, "skills", SkillName)),
            ("Codex", Directory.Exists(codexHome),
                Path.Combine(codexSkills, SkillName)),
            ("Sparky", Directory.Exists(sparkyHome), Path.Combine(sparkyHome, "skills", SkillName)),
            ("OpenCode", Directory.Exists(openCodeHome), Path.Combine(openCodeHome, "skills", SkillName))
        };

        var results = new List<SkillInstallResult>(candidates.Length);
        foreach (var candidate in candidates)
        {
            if (!candidate.Detected)
            {
                results.Add(new SkillInstallResult(candidate.Name, "not_detected", null,
                    "No configuration folder was found."));
                continue;
            }

            var destination = Path.Combine(candidate.Directory, "SKILL.md");
            try
            {
                Directory.CreateDirectory(candidate.Directory);
                if (File.Exists(destination))
                {
                    var sameSkill = File.ReadAllText(destination).Equals(
                        File.ReadAllText(skillSource), StringComparison.Ordinal);
                    results.Add(new SkillInstallResult(candidate.Name,
                        sameSkill ? "already_installed" : "conflict", destination,
                        sameSkill ? null : "An existing skill was left unchanged."));
                    continue;
                }

                File.Copy(skillSource, destination, overwrite: false);
                results.Add(new SkillInstallResult(candidate.Name, "installed", destination, null));
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or
                                       ArgumentException or NotSupportedException)
            {
                results.Add(new SkillInstallResult(candidate.Name, "error", destination, ex.Message));
            }
        }

        return results;
    }

    private static string ConfigRoot(string variable, string fallback)
    {
        var value = Environment.GetEnvironmentVariable(variable);
        return string.IsNullOrWhiteSpace(value) ? fallback : Path.GetFullPath(value);
    }
}

internal sealed record SkillInstallResult(string Agent, string Status, string? Path, string? Message);
