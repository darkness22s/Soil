using System.Text;

namespace Soil;

public sealed record SoilFileEntry(
    string Name,
    string Path,
    string Kind,
    long Size,
    DateTime ModifiedUtc);

public sealed record SoilFileList(
    string Path,
    string? Parent,
    IReadOnlyList<SoilFileEntry> Entries);

public sealed record SoilFileContent(
    string Kind,
    string? Content,
    long Size,
    bool Truncated);

public static class SoilFiles
{
    private const long MaxPreviewBytes = 512 * 1024;

    public static SoilFileList List(SoilPaths paths, string? relative)
    {
        var full = paths.Resolve(relative);
        if (!Directory.Exists(full))
            throw new FileNotFoundException("Folder not found.", full);

        var info = new DirectoryInfo(full);
        var entries = info.EnumerateFileSystemInfos()
            .Select(e => ToEntry(paths, e))
            .OrderByDescending(e => e.Kind == "folder")
            .ThenBy(e => e.Name, StringComparer.OrdinalIgnoreCase)
            .ToList();

        var parent = full.Equals(paths.Root, StringComparison.OrdinalIgnoreCase)
            ? null
            : paths.Relative(Path.GetDirectoryName(full)!);

        return new SoilFileList(paths.Relative(full), parent, entries);
    }

    public static SoilFileContent Read(SoilPaths paths, string? relative)
    {
        var full = paths.Resolve(relative);
        if (!File.Exists(full))
            throw new FileNotFoundException("File not found.", full);

        var info = new FileInfo(full);
        if (info.Length > MaxPreviewBytes)
            return new SoilFileContent("binary", null, info.Length, true);

        var bytes = File.ReadAllBytes(full);
        if (bytes.Any(b => b == 0))
            return new SoilFileContent("binary", null, info.Length, false);

        try
        {
            var text = new UTF8Encoding(false, true).GetString(bytes);
            return new SoilFileContent("text", text, info.Length, false);
        }
        catch (DecoderFallbackException)
        {
            return new SoilFileContent("binary", null, info.Length, false);
        }
    }

    public static (string FullPath, string ContentType) Raw(SoilPaths paths, string? relative)
    {
        var full = paths.Resolve(relative);
        if (!File.Exists(full))
            throw new FileNotFoundException("File not found.", full);
        return (full, ContentTypeFor(Path.GetExtension(full)));
    }

    private static SoilFileEntry ToEntry(SoilPaths paths, FileSystemInfo info)
    {
        var isLink = (info.Attributes & FileAttributes.ReparsePoint) != 0;
        var isFolder = info is DirectoryInfo;
        var kind = isLink ? "link" : isFolder ? "folder" : "file";
        var size = info is FileInfo file ? file.Length : 0;
        return new SoilFileEntry(
            info.Name, paths.Relative(info.FullName), kind, size, info.LastWriteTimeUtc);
    }

    private static string ContentTypeFor(string extension) => extension.ToLowerInvariant() switch
    {
        ".png" => "image/png",
        ".jpg" or ".jpeg" => "image/jpeg",
        ".gif" => "image/gif",
        ".webp" => "image/webp",
        ".svg" => "image/svg+xml",
        ".txt" or ".md" or ".json" or ".xml" or ".yml" or ".yaml" => "text/plain; charset=utf-8",
        ".html" or ".htm" => "text/html; charset=utf-8",
        ".css" => "text/css; charset=utf-8",
        ".js" or ".mjs" => "text/javascript; charset=utf-8",
        ".pdf" => "application/pdf",
        _ => "application/octet-stream"
    };
}
