using System.Net;
using System.Net.Sockets;
using System.Text;

namespace Soil;

internal sealed class StaticSiteServer : IDisposable
{
    private readonly string m_root;
    private readonly string m_rootPrefix;
    private readonly TcpListener m_listener;
    private readonly CancellationTokenSource m_cts = new();
    private readonly Task m_loop;

    public int Port { get; }

    public StaticSiteServer(string root)
    {
        m_root = Path.GetFullPath(root);
        m_rootPrefix = m_root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
                       + Path.DirectorySeparatorChar;

        m_listener = new TcpListener(IPAddress.Loopback, 0);
        m_listener.Start();
        Port = ((IPEndPoint)m_listener.LocalEndpoint).Port;
        m_loop = Task.Run(LoopAsync);
    }

    private async Task LoopAsync()
    {
        while (!m_cts.IsCancellationRequested)
        {
            TcpClient client;
            try
            {
                client = await m_listener.AcceptTcpClientAsync(m_cts.Token);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch
            {
                break;
            }

            _ = HandleAsync(client);
        }
    }

    private async Task HandleAsync(TcpClient client)
    {
        using (client)
        {
            try
            {
                using var stream = client.GetStream();
                var request = await ReadRequestAsync(stream, m_cts.Token);
                if (request is null)
                    return;

                var parts = request.Split(' ');
                var method = parts.ElementAtOrDefault(0) ?? "";
                var target = parts.ElementAtOrDefault(1) ?? "/";
                if (method is not ("GET" or "HEAD"))
                {
                    await WriteStatusAsync(stream, 405, "Method Not Allowed", m_cts.Token);
                    return;
                }

                var path = target.Split('?', '#')[0];
                string decoded;
                try
                {
                    decoded = Uri.UnescapeDataString(path);
                }
                catch
                {
                    await WriteStatusAsync(stream, 400, "Bad Request", m_cts.Token);
                    return;
                }

                var full = Resolve(decoded);
                if (full is not null && Directory.Exists(full))
                    full = Path.Combine(full, "index.html");
                if (full is null || !File.Exists(full))
                {
                    if (!Path.HasExtension(decoded))
                        full = Path.Combine(m_root, "index.html");
                    else
                        full = null;
                }
                if (full is null || !File.Exists(full))
                {
                    await WriteStatusAsync(stream, 404, "Not Found", m_cts.Token);
                    return;
                }

                var info = new FileInfo(full);
                var headers = Encoding.ASCII.GetBytes(
                    "HTTP/1.1 200 OK\r\n" +
                    "Content-Type: " + ContentType(full) + "\r\n" +
                    "Content-Length: " + info.Length + "\r\n" +
                    "Cache-Control: no-store\r\n" +
                    "X-Content-Type-Options: nosniff\r\n" +
                    "Connection: close\r\n\r\n");
                await stream.WriteAsync(headers, m_cts.Token);
                if (method == "HEAD")
                    return;

                await using var file = new FileStream(
                    full, FileMode.Open, FileAccess.Read, FileShare.ReadWrite,
                    64 * 1024, FileOptions.Asynchronous | FileOptions.SequentialScan);
                await file.CopyToAsync(stream, m_cts.Token);
            }
            catch
            {
            }
        }
    }

    private string? Resolve(string path)
    {
        var rel = path.TrimStart('/').Replace('/', Path.DirectorySeparatorChar);
        if (rel.IndexOfAny(Path.GetInvalidPathChars()) >= 0)
            return null;
        var full = Path.GetFullPath(Path.Combine(m_root, rel));
        if (!full.Equals(m_root, StringComparison.OrdinalIgnoreCase) &&
            !full.StartsWith(m_rootPrefix, StringComparison.OrdinalIgnoreCase))
            return null;
        return full;
    }

    private static async Task<string?> ReadRequestAsync(NetworkStream stream, CancellationToken ct)
    {
        var buffer = new byte[16384];
        var total = 0;
        while (total < buffer.Length)
        {
            var read = await stream.ReadAsync(buffer.AsMemory(total, buffer.Length - total), ct);
            if (read <= 0)
                return null;
            total += read;
            var text = Encoding.ASCII.GetString(buffer, 0, total);
            if (text.Contains("\r\n\r\n", StringComparison.Ordinal))
                return text.Split("\r\n", 2, StringSplitOptions.None)[0];
        }
        return null;
    }

    private static async Task WriteStatusAsync(
        NetworkStream stream, int status, string reason, CancellationToken ct)
    {
        var body = Encoding.UTF8.GetBytes(reason);
        var head = Encoding.ASCII.GetBytes(
            $"HTTP/1.1 {status} {reason}\r\nContent-Type: text/plain; charset=utf-8\r\n" +
            $"Content-Length: {body.Length}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
        await stream.WriteAsync(head, ct);
        await stream.WriteAsync(body, ct);
    }

    private static string ContentType(string path) => Path.GetExtension(path).ToLowerInvariant() switch
    {
        ".html" or ".htm" => "text/html; charset=utf-8",
        ".css" => "text/css; charset=utf-8",
        ".js" or ".mjs" => "text/javascript; charset=utf-8",
        ".json" or ".map" => "application/json; charset=utf-8",
        ".svg" => "image/svg+xml",
        ".png" => "image/png",
        ".jpg" or ".jpeg" => "image/jpeg",
        ".gif" => "image/gif",
        ".webp" => "image/webp",
        ".ico" => "image/x-icon",
        ".woff" => "font/woff",
        ".woff2" => "font/woff2",
        ".ttf" => "font/ttf",
        ".otf" => "font/otf",
        ".wasm" => "application/wasm",
        ".webmanifest" => "application/manifest+json",
        ".txt" => "text/plain; charset=utf-8",
        ".xml" => "application/xml; charset=utf-8",
        ".mp4" => "video/mp4",
        ".webm" => "video/webm",
        ".mp3" => "audio/mpeg",
        ".ogg" => "audio/ogg",
        ".pdf" => "application/pdf",
        _ => "application/octet-stream"
    };

    public void Dispose()
    {
        try
        {
            m_cts.Cancel();
        }
        catch
        {
        }
        try
        {
            m_listener.Stop();
        }
        catch
        {
        }
        try
        {
            m_loop.Wait(2000);
        }
        catch
        {
        }
        m_cts.Dispose();
    }
}
