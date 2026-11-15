using System.Diagnostics;
using System.Net;
using System.Net.Http.Headers;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Soil;

public sealed record GitHubStatus(
    bool Configured,
    bool Connected,
    string? Login,
    string? AvatarUrl,
    bool Connecting,
    string? Error);

public sealed class GitHubService : IDisposable
{
    private static readonly HttpClient Http = CreateHttpClient();

    private readonly SoilPaths m_paths;
    private readonly string m_sessionPath;
    private readonly object m_lock = new();
    private string m_workerUrl;
    private GitHubSession? m_session;
    private CancellationTokenSource? m_connectCts;
    private bool m_connecting;
    private string? m_connectError;

    public GitHubService(SoilPaths paths)
    {
        m_paths = paths;
        m_paths.Ensure();
        m_sessionPath = Path.Combine(paths.KeysDir, "github-session.json");
        m_workerUrl = ReadWorkerUrl();
        m_session = LoadSession(m_workerUrl);
    }

    public string WorkerUrl
    {
        get
        {
            lock (m_lock)
                return m_workerUrl;
        }
    }

    public bool HasSession
    {
        get
        {
            lock (m_lock)
                return m_session is not null;
        }
    }

    public GitHubStatus Status()
    {
        lock (m_lock)
            return StatusLocked();
    }

    public GitHubStatus SetWorkerUrl(string? value)
    {
        var normalized = NormalizeWorkerUrl(value);
        lock (m_lock)
        {
            if (!string.Equals(normalized, m_workerUrl, StringComparison.OrdinalIgnoreCase))
            {
                m_session = null;
                TryDelete(m_sessionPath);
                m_connectError = null;
                m_workerUrl = normalized;
            }
        }

        WriteWorkerUrl(normalized);
        return Status();
    }

    public GitHubStatus BeginConnect()
    {
        string worker;
        lock (m_lock)
        {
            if (m_connecting)
                return StatusLocked();
            if (m_session is not null)
                return StatusLocked();
            if (string.IsNullOrWhiteSpace(m_workerUrl))
            {
                m_connectError = "GitHub sign-in is not configured for this installation.";
                return StatusLocked();
            }
            m_connecting = true;
            m_connectError = null;
            worker = m_workerUrl;
            m_connectCts = new CancellationTokenSource();
        }

        var token = m_connectCts!.Token;
        _ = Task.Run(() => ConnectAsync(worker, token));
        return Status();
    }

    public async Task<JsonElement> ListReposAsync(CancellationToken ct)
    {
        var (worker, session) = RequireSession();
        using var request = new HttpRequestMessage(HttpMethod.Get, worker + "/api/repos");
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", session.Session);
        using var response = await Http.SendAsync(
            request, HttpCompletionOption.ResponseContentRead, ct);
        var body = await response.Content.ReadAsStringAsync(ct);

        if (response.StatusCode == HttpStatusCode.Unauthorized)
        {
            ClearSession();
            throw new InvalidOperationException("The GitHub session expired. Connect again.");
        }
        if (!response.IsSuccessStatusCode)
            throw new InvalidOperationException(ExtractError(body) ?? "GitHub could not list repositories.");

        using var doctor = JsonDocument.Parse(body);
        return doctor.RootElement.Clone();
    }

    public async Task<HttpResponseMessage?> OpenArchiveAsync(
        string owner, string repo, string? reference, CancellationToken ct)
    {
        string worker;
        GitHubSession? session;
        lock (m_lock)
        {
            if (m_session is null)
                return null;
            worker = m_workerUrl;
            session = m_session;
        }

        var url = worker + "/api/archive?owner=" + Uri.EscapeDataString(owner) +
                  "&repo=" + Uri.EscapeDataString(repo);
        if (!string.IsNullOrWhiteSpace(reference))
            url += "&ref=" + Uri.EscapeDataString(reference);

        var request = new HttpRequestMessage(HttpMethod.Get, url);
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", session.Session);
        var response = await Http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, ct);
        if (response.StatusCode == HttpStatusCode.Unauthorized)
        {
            response.Dispose();
            ClearSession();
            throw new InvalidOperationException("The GitHub session expired. Connect again.");
        }
        if (!response.IsSuccessStatusCode)
        {
            var body = await response.Content.ReadAsStringAsync(ct);
            response.Dispose();
            throw new InvalidOperationException(ExtractError(body) ?? "GitHub could not download that repository.");
        }
        return response;
    }

    public async Task DisconnectAsync(CancellationToken ct)
    {
        GitHubSession? session;
        string worker;
        lock (m_lock)
        {
            session = m_session;
            worker = m_workerUrl;
            m_session = null;
        }
        TryDelete(m_sessionPath);

        if (session is null || string.IsNullOrWhiteSpace(worker))
            return;
        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Post, worker + "/auth/logout");
            request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", session.Session);
            using var response = await Http.SendAsync(request, ct);
        }
        catch
        {
        }
    }

    private async Task ConnectAsync(string worker, CancellationToken ct)
    {
        TcpListener? listener = null;
        try
        {
            listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            var port = ((IPEndPoint)listener.LocalEndpoint).Port;
            var redirect = $"http://127.0.0.1:{port}/oauth/callback";
            var state = RandomToken(24);
            var startUrl = worker + "/auth/github/start?redirect_uri=" +
                           Uri.EscapeDataString(redirect) + "&state=" + Uri.EscapeDataString(state);

            try
            {
                Process.Start(new ProcessStartInfo(startUrl) { UseShellExecute = true });
            }
            catch
            {
                throw new InvalidOperationException("Could not open the GitHub sign-in page.");
            }

            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeout.CancelAfter(TimeSpan.FromMinutes(5));

            var deadline = DateTime.UtcNow.AddMinutes(5);
            while (DateTime.UtcNow < deadline)
            {
                var client = await listener.AcceptTcpClientAsync(timeout.Token);
                using (client)
                {
                    var request = await ReadRequestAsync(client.GetStream(), timeout.Token);
                    if (request is null)
                        continue;

                    var path = request.Split(' ').ElementAtOrDefault(1) ?? "";
                    if (!path.StartsWith("/oauth/callback", StringComparison.Ordinal))
                    {
                        await WriteResponseAsync(client.GetStream(), 404, "Not found.", timeout.Token);
                        continue;
                    }

                    var query = ParseQuery(path);
                    query.TryGetValue("state", out var callbackState);
                    query.TryGetValue("session", out var callbackSession);
                    query.TryGetValue("login", out var callbackLogin);
                    query.TryGetValue("error", out var callbackError);

                    if (!string.Equals(callbackState, state, StringComparison.Ordinal))
                    {
                        await WriteResponseAsync(client.GetStream(), 400, "The sign-in response did not match.", timeout.Token);
                        throw new InvalidOperationException("GitHub returned an invalid sign-in response.");
                    }
                    if (!string.IsNullOrWhiteSpace(callbackError))
                        throw new InvalidOperationException("GitHub did not approve the connection.");
                    if (string.IsNullOrWhiteSpace(callbackSession))
                        throw new InvalidOperationException("GitHub did not return a session.");

                    var session = new GitHubSession
                    {
                        Session = callbackSession,
                        Login = callbackLogin ?? "",
                        WorkerUrl = worker,
                        CreatedUtc = DateTime.UtcNow
                    };
                    lock (m_lock)
                        m_session = session;
                    WriteSession(session);
                    await WriteResponseAsync(
                        client.GetStream(), 200,
                        "GitHub is connected. You can close this tab and return to Soil.",
                        timeout.Token);
                    return;
                }
            }
            throw new InvalidOperationException("GitHub sign-in timed out.");
        }
        catch (OperationCanceledException)
        {
            lock (m_lock)
                m_connectError = "GitHub sign-in timed out.";
        }
        catch (Exception ex)
        {
            lock (m_lock)
                m_connectError = ex.Message;
        }
        finally
        {
            try
            {
                listener?.Stop();
            }
            catch
            {
            }
            lock (m_lock)
            {
                m_connecting = false;
                m_connectCts?.Dispose();
                m_connectCts = null;
            }
        }
    }

    private (string Worker, GitHubSession Session) RequireSession()
    {
        lock (m_lock)
        {
            if (m_session is null)
                throw new InvalidOperationException("Connect GitHub first.");
            return (m_workerUrl, m_session);
        }
    }

    private GitHubStatus StatusLocked() => new(
        !string.IsNullOrWhiteSpace(m_workerUrl),
        m_session is not null,
        m_session?.Login,
        null,
        m_connecting,
        m_connectError);

    private void ClearSession()
    {
        lock (m_lock)
            m_session = null;
        TryDelete(m_sessionPath);
    }

    private GitHubSession? LoadSession(string worker)
    {
        if (string.IsNullOrWhiteSpace(worker) || !File.Exists(m_sessionPath))
            return null;
        try
        {
            var session = JsonSerializer.Deserialize<GitHubSession>(
                File.ReadAllText(m_sessionPath), JsonOptions.Web);
            if (session is null || string.IsNullOrWhiteSpace(session.Session))
                return null;
            if (!string.Equals(session.WorkerUrl, worker, StringComparison.OrdinalIgnoreCase))
                return null;
            return session;
        }
        catch
        {
            return null;
        }
    }

    private void WriteSession(GitHubSession session)
    {
        try
        {
            Directory.CreateDirectory(m_paths.KeysDir);
            File.WriteAllText(m_sessionPath,
                JsonSerializer.Serialize(session, new JsonSerializerOptions { WriteIndented = true }));
            RestrictFile(m_sessionPath);
        }
        catch
        {
        }
    }

    private string ReadWorkerUrl()
    {
        try
        {
            if (!File.Exists(m_paths.ConfigPath))
                return "";
            var node = JsonNode.Parse(File.ReadAllText(m_paths.ConfigPath)) as JsonObject;
            return NormalizeWorkerUrl(node?["githubWorkerUrl"]?.GetValue<string>());
        }
        catch
        {
            return "";
        }
    }

    private void WriteWorkerUrl(string value)
    {
        try
        {
            JsonObject root;
            if (File.Exists(m_paths.ConfigPath))
                root = JsonNode.Parse(File.ReadAllText(m_paths.ConfigPath)) as JsonObject ?? new JsonObject();
            else
                root = new JsonObject();
            root["githubWorkerUrl"] = value;
            File.WriteAllText(m_paths.ConfigPath,
                root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        }
        catch
        {
        }
    }

    private static string NormalizeWorkerUrl(string? value)
    {
        var text = (value ?? "").Trim();
        if (text.Length == 0)
            return "";
        if (!Uri.TryCreate(text, UriKind.Absolute, out var uri))
            throw new ArgumentException("Enter a complete https Worker URL.");
        if (uri.Scheme != Uri.UriSchemeHttps &&
            !(uri.Scheme == Uri.UriSchemeHttp &&
              (uri.Host.Equals("127.0.0.1", StringComparison.OrdinalIgnoreCase) ||
               uri.Host.Equals("localhost", StringComparison.OrdinalIgnoreCase))))
            throw new ArgumentException("The Worker URL must use https.");
        if (!string.IsNullOrEmpty(uri.Query) || !string.IsNullOrEmpty(uri.Fragment))
            throw new ArgumentException("The Worker URL cannot contain a query or fragment.");
        return uri.GetLeftPart(UriPartial.Path).TrimEnd('/');
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

    private static async Task WriteResponseAsync(
        NetworkStream stream, int status, string message, CancellationToken ct)
    {
        var safe = WebUtility.HtmlEncode(message);
        var body = Encoding.UTF8.GetBytes(
            "<!doctype html><meta charset=\"utf-8\"><title>Soil</title>" +
            "<body style=\"font:16px system-ui;padding:48px;max-width:560px;margin:auto\">" +
            "<h1 style=\"font-size:22px\">Soil</h1><p>" + safe + "</p></body>");
        var reason = status == 200 ? "OK" : status == 404 ? "Not Found" : "Bad Request";
        var head = Encoding.ASCII.GetBytes(
            $"HTTP/1.1 {status} {reason}\r\nContent-Type: text/html; charset=utf-8\r\n" +
            $"Content-Length: {body.Length}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
        await stream.WriteAsync(head, ct);
        await stream.WriteAsync(body, ct);
    }

    private static Dictionary<string, string> ParseQuery(string path)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        var index = path.IndexOf('?');
        if (index < 0 || index == path.Length - 1)
            return result;
        foreach (var pair in path[(index + 1)..].Split('&', StringSplitOptions.RemoveEmptyEntries))
        {
            var split = pair.Split('=', 2);
            var key = Uri.UnescapeDataString(split[0].Replace('+', ' '));
            var value = split.Length == 2
                ? Uri.UnescapeDataString(split[1].Replace('+', ' '))
                : "";
            result[key] = value;
        }
        return result;
    }

    private static string RandomToken(int bytes)
    {
        var data = RandomNumberGenerator.GetBytes(bytes);
        return Convert.ToBase64String(data)
            .TrimEnd('=')
            .Replace('+', '-')
            .Replace('/', '_');
    }

    private static string? ExtractError(string body)
    {
        try
        {
            using var doc = JsonDocument.Parse(body);
            if (doc.RootElement.TryGetProperty("error", out var error))
                return error.GetString();
        }
        catch
        {
        }
        return null;
    }

    private static void RestrictFile(string path)
    {
        try
        {
            var user = WindowsIdentity.GetCurrent().Name;
            var psi = new ProcessStartInfo
            {
                FileName = Path.Combine(Environment.SystemDirectory, "icacls.exe"),
                UseShellExecute = false,
                CreateNoWindow = true
            };
            psi.ArgumentList.Add(path);
            psi.ArgumentList.Add("/inheritance:r");
            psi.ArgumentList.Add("/grant:r");
            psi.ArgumentList.Add(user + ":F");
            using var process = Process.Start(psi);
            process?.WaitForExit(3000);
        }
        catch
        {
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

    private static HttpClient CreateHttpClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromSeconds(60) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("Soil/0.3");
        return client;
    }

    public void Dispose()
    {
        lock (m_lock)
        {
            m_connectCts?.Cancel();
            m_connectCts?.Dispose();
            m_connectCts = null;
        }
    }

    private sealed class GitHubSession
    {
        public string Session { get; set; } = "";
        public string Login { get; set; } = "";
        public string WorkerUrl { get; set; } = "";
        public DateTime CreatedUtc { get; set; }
    }
}
