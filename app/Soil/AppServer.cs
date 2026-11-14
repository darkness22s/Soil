using System.Diagnostics;
using System.Net;
using System.Text.Json;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Http.Features;
using Microsoft.AspNetCore.Server.Kestrel.Core;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.FileProviders;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;

namespace Soil;

public sealed class AppServer : IDisposable
{
    private readonly WebApplication m_app;
    private readonly VmManager m_manager;
    private readonly SoilPaths m_paths;
    private readonly GitHubService m_github;
    private readonly AppRuntimeManager m_apps;
    private readonly AgentManager m_agents;
    private readonly NativeLinuxAppManager m_native;
    private readonly UiStateStore m_ui;

    public string Url { get; private set; } = "";

    public AppServer(VmManager manager, SoilPaths paths)
    {
        m_manager = manager;
        m_paths = paths;
        m_github = new GitHubService(paths);
        m_apps = new AppRuntimeManager(manager, paths, m_github);
        m_agents = new AgentManager(manager);
        m_native = new NativeLinuxAppManager(manager, paths);
        manager.RestoreNativeApps = m_native.RestoreAsync;
        m_ui = new UiStateStore(paths);

        var uiRoot = Path.Combine(AppContext.BaseDirectory, "ui");
        var builder = WebApplication.CreateBuilder(new WebApplicationOptions
        {
            ContentRootPath = AppContext.BaseDirectory
        });

        builder.Logging.ClearProviders();
        builder.WebHost.ConfigureKestrel(options =>
        {
            options.Listen(IPAddress.Loopback, 0);
            options.Limits.MaxRequestBodySize = 1024 * 1024;
        });

        builder.Services.AddSingleton(manager);
        m_app = builder.Build();

        m_app.Use(async (ctx, next) =>
        {
            var remote = ctx.Connection.RemoteIpAddress;
            if (remote is not null && !IPAddress.IsLoopback(remote))
            {
                ctx.Response.StatusCode = StatusCodes.Status403Forbidden;
                return;
            }

            var host = ctx.Request.Host.Host;
            if (!host.Equals("127.0.0.1", StringComparison.OrdinalIgnoreCase) &&
                !host.Equals("localhost", StringComparison.OrdinalIgnoreCase) &&
                !host.Equals("::1", StringComparison.OrdinalIgnoreCase))
            {
                ctx.Response.StatusCode = StatusCodes.Status403Forbidden;
                return;
            }

            if (ctx.Request.Headers.TryGetValue("Origin", out var origins) &&
                origins.Count > 0 &&
                !string.Equals(
                    origins[0],
                    $"{ctx.Request.Scheme}://{ctx.Request.Host}",
                    StringComparison.OrdinalIgnoreCase))
            {
                ctx.Response.StatusCode = StatusCodes.Status403Forbidden;
                return;
            }

            ctx.Response.Headers["X-Content-Type-Options"] = "nosniff";
            ctx.Response.Headers["Referrer-Policy"] = "no-referrer";

            await next();
        });

        var defaultFiles = new DefaultFilesOptions
        {
            FileProvider = new PhysicalFileProvider(uiRoot)
        };
        defaultFiles.DefaultFileNames.Clear();
        defaultFiles.DefaultFileNames.Add("app.html");
        m_app.UseDefaultFiles(defaultFiles);
        m_app.UseStaticFiles(new StaticFileOptions
        {
            FileProvider = new PhysicalFileProvider(uiRoot),
            OnPrepareResponse = ctx =>
                ctx.Context.Response.Headers.CacheControl = "no-store"
        });

        MapApi();
    }

    private void MapApi()
    {
        m_app.MapGet("/api/health", () => Results.Json(new { ok = true }, JsonOptions.Web));
        m_app.MapGet("/api/skill", () =>
        {
            var skillPath = Path.Combine(AppContext.BaseDirectory, "skill", "soil-control", "SKILL.md");
            return File.Exists(skillPath)
                ? Results.File(skillPath, "text/markdown; charset=utf-8")
                : Results.NotFound();
        });
        m_app.MapPost("/api/skill/install", () =>
        {
            var skillPath = Path.Combine(AppContext.BaseDirectory, "skill", "soil-control", "SKILL.md");
            if (!File.Exists(skillPath))
                return Results.NotFound(new { error = "The Soil skill could not be found." });
            return Results.Json(SkillInstaller.Install(skillPath), JsonOptions.Web);
        });

        m_app.MapGet("/api/info", () => Results.Json(new
        {
            name = "Soil",
            version = typeof(AppServer).Assembly.GetName().Version?.ToString(3) ?? "4.2.1",
            home = m_paths.Root,
            schema = 1
        }, JsonOptions.Web));

        m_app.MapGet("/api/ui", () =>
        {
            var state = m_ui.Get();
            if (!state.OnboardingComplete && m_manager.List().Count > 0)
            {
                state = m_ui.Update(
                    new UiStateUpdate(true, null, null, null, null));
            }
            return Results.Json(state, JsonOptions.Web);
        });

        m_app.MapPost("/api/ui", async (HttpRequest req) =>
        {
            UiStateUpdate? body;
            try
            {
                body = await JsonSerializer.DeserializeAsync<UiStateUpdate>(
                    req.Body, JsonOptions.Web);
            }
            catch (JsonException)
            {
                return Results.BadRequest(new { error = "Invalid JSON." });
            }
            if (body is null)
                return Results.BadRequest(new { error = "UI state is required." });
            return Results.Json(m_ui.Update(body), JsonOptions.Web);
        });

        m_app.MapGet("/api/github/status", () =>
            Results.Json(m_github.Status(), JsonOptions.Web));

        m_app.MapPost("/api/github/config", async (HttpRequest req) =>
        {
            GitHubConfigRequest? body;
            try
            {
                body = await JsonSerializer.DeserializeAsync<GitHubConfigRequest>(
                    req.Body, JsonOptions.Web);
            }
            catch (JsonException)
            {
                return Results.BadRequest(new { error = "Invalid JSON." });
            }
            try
            {
                return Results.Json(m_github.SetWorkerUrl(body?.WorkerUrl), JsonOptions.Web);
            }
            catch (ArgumentException ex)
            {
                return Results.BadRequest(new { error = ex.Message });
            }
        });

        m_app.MapPost("/api/github/connect", () =>
            Results.Json(m_github.BeginConnect(), JsonOptions.Web));

        m_app.MapGet("/api/github/repos", async (CancellationToken ct) =>
        {
            try
            {
                return Results.Json(await m_github.ListReposAsync(ct), JsonOptions.Web);
            }
            catch (Exception ex)
            {
                return GitHubError(ex);
            }
        });

        m_app.MapPost("/api/github/disconnect", async (CancellationToken ct) =>
        {
            await m_github.DisconnectAsync(ct);
            return Results.Json(m_github.Status(), JsonOptions.Web);
        });

        m_app.MapPost("/api/apps", async (HttpRequest req) =>
        {
            AppImportRequest? body;
            try
            {
                body = await JsonSerializer.DeserializeAsync<AppImportRequest>(
                    req.Body, JsonOptions.Web);
            }
            catch (JsonException)
            {
                return Results.BadRequest(new { error = "Invalid JSON." });
            }
            if (body is null)
                return Results.BadRequest(new { error = "Repository details are required." });
            try
            {
                var dto = m_apps.CreateApp(body);
                return Results.Json(dto, JsonOptions.Web);
            }
            catch (ArgumentException ex)
            {
                return Results.BadRequest(new { error = ex.Message });
            }
            catch (InvalidOperationException ex)
            {
                return Results.BadRequest(new { error = ex.Message });
            }
        });

        m_app.MapPost("/api/agents", async (HttpRequest req) =>
        {
            CreateAgentRequest? body;
            try
            {
                body = await JsonSerializer.DeserializeAsync<CreateAgentRequest>(
                    req.Body, JsonOptions.Web);
            }
            catch (JsonException)
            {
                return Results.BadRequest(new { error = "Invalid JSON." });
            }
            if (body is null || string.IsNullOrWhiteSpace(body.Name))
                return Results.BadRequest(new { error = "A name is required." });
            var dto = m_manager.CreateAgent(body.Name.Trim());
            return Results.Json(dto, JsonOptions.Web);
        });

        m_app.MapGet("/api/vms", () => Results.Json(m_manager.List(), JsonOptions.Web));

        m_app.MapPost("/api/vms", async (HttpRequest req) =>
        {
            CreateVmRequest? body;
            try
            {
                body = await JsonSerializer.DeserializeAsync<CreateVmRequest>(
                    req.Body, JsonOptions.Web);
            }
            catch (JsonException)
            {
                return Results.BadRequest(new { error = "Invalid JSON." });
            }
            if (body is null || string.IsNullOrWhiteSpace(body.Name))
                return Results.BadRequest(new { error = "A name is required." });
            var dto = m_manager.Create(body.Name.Trim(), body.Size ?? "m");
            return Results.Json(dto, JsonOptions.Web);
        });

        // Native files are transferred into the bundled guest, never run on Windows.
        // This endpoint is the only exception to the default 1 MiB request body cap.
        m_app.MapGet("/api/vms/{id}/native-apps", (string id) =>
        {
            if (m_manager.FindRecord(id) is null)
                return Results.NotFound();
            try { return Results.Json(m_native.List(id), JsonOptions.Web); }
            catch (ArgumentException ex)
            {
                return Results.BadRequest(new { error = ex.Message });
            }
        });

        m_app.MapPost("/api/vms/{id}/native-apps", async (string id, HttpContext ctx) =>
        {
            if (m_manager.FindRecord(id) is null)
                return Results.NotFound();
            if (!string.Equals(ctx.Request.ContentType, "application/octet-stream",
                               StringComparison.OrdinalIgnoreCase))
                return Results.BadRequest(new { error = "Send raw application/octet-stream bytes." });
            var length = ctx.Request.ContentLength;
            if (!length.HasValue || length.Value < 64 || length.Value > NativeLinuxAppManager.MaxBytes)
                return Results.BadRequest(new { error = "Linux app upload must be 64 bytes to 256 MiB." });

            var requestSize = ctx.Features.Get<IHttpMaxRequestBodySizeFeature>();
            if (requestSize is { IsReadOnly: false })
                requestSize.MaxRequestBodySize = NativeLinuxAppManager.MaxBytes;

            try
            {
                var encoded = ctx.Request.Headers["X-Soil-Filename"].ToString();
                var fileName = Uri.UnescapeDataString(encoded);
                var app = await m_native.ImportAsync(
                    id, fileName, ctx.Request.Body, length.Value, ctx.RequestAborted);
                return Results.Json(app, JsonOptions.Web);
            }
            catch (Exception ex) when (ex is ArgumentException or InvalidDataException
                                           or UriFormatException)
            {
                return Results.BadRequest(new { error = ex.Message });
            }
        });

        m_app.MapPost("/api/vms/{id}/native-apps/{appId}/launch",
            async (string id, string appId, HttpContext ctx) =>
        {
            try
            {
                var app = await m_native.LaunchAsync(id, appId, ctx.RequestAborted);
                return Results.Json(app, JsonOptions.Web);
            }
            catch (FileNotFoundException ex)
            {
                return Results.NotFound(new { error = ex.Message });
            }
            catch (Exception ex) when (ex is ArgumentException or InvalidOperationException)
            {
                return Results.Conflict(new { error = ex.Message });
            }
        });

        m_app.MapDelete("/api/vms/{id}/native-apps/{appId}",
            async (string id, string appId, HttpContext ctx) =>
        {
            try
            {
                await m_native.RemoveAsync(id, appId, ctx.RequestAborted);
                return Results.NoContent();
            }
            catch (FileNotFoundException ex)
            {
                return Results.NotFound(new { error = ex.Message });
            }
            catch (ArgumentException ex)
            {
                return Results.BadRequest(new { error = ex.Message });
            }
        });

        m_app.MapPost("/api/vms/{id}/start", (string id) =>
        {
            var isAgent = string.Equals(
                m_manager.FindRecord(id)?.Kind, "agent", StringComparison.OrdinalIgnoreCase);
            var dto = isAgent ? m_agents.RequestStart(id) : m_apps.RequestStart(id);
            return dto is null ? Results.NotFound() : Results.Json(dto, JsonOptions.Web);
        });

        m_app.MapPost("/api/vms/{id}/stop", (string id) =>
        {
            var isAgent = string.Equals(
                m_manager.FindRecord(id)?.Kind, "agent", StringComparison.OrdinalIgnoreCase);
            var dto = isAgent ? m_agents.RequestStop(id) : m_apps.RequestStop(id);
            return dto is null ? Results.NotFound() : Results.Json(dto, JsonOptions.Web);
        });

        m_app.MapPost("/api/vms/{id}/terminal", (string id) =>
        {
            var record = m_manager.FindRecord(id);
            if (record is null ||
                !string.Equals(record.Kind, "agent", StringComparison.OrdinalIgnoreCase))
                return Results.NotFound();
            return m_agents.OpenTerminal(id)
                ? Results.NoContent()
                : Results.Conflict(new { error = "The SSH tunnel is not ready." });
        });

        m_app.MapDelete("/api/vms/{id}", (string id) =>
        {
            var record = m_manager.FindRecord(id);
            if (string.Equals(record?.Kind, "agent", StringComparison.OrdinalIgnoreCase))
                m_agents.Remove(id);
            else
                m_apps.Remove(id);
            return m_manager.Delete(id)
                ? Results.Json(new { id, deleted = true }, JsonOptions.Web)
                : Results.NotFound();
        });

        m_app.MapPost("/api/vms/{id}/input", async (string id, HttpRequest req) =>
        {
            try
            {
                using var doc = await JsonDocument.ParseAsync(req.Body);
                return m_manager.SendInput(id, doc.RootElement)
                    ? Results.NoContent()
                    : Results.NotFound();
            }
            catch (JsonException)
            {
                return Results.BadRequest();
            }
        });

        m_app.MapGet("/api/vms/{id}/frame.jpg", (string id) =>
        {
            var jpeg = m_manager.FrameJpeg(id);
            return jpeg is null ? Results.NotFound() : Results.File(jpeg, "image/jpeg");
        });

        m_app.MapGet("/api/vms/{id}/stream.mjpg", async (HttpContext ctx, string id) =>
        {
            var runtime = m_manager.FindRuntime(id);
            if (runtime is null)
            {
                ctx.Response.StatusCode = 404;
                return;
            }
            await m_manager.StreamAsync(runtime, ctx, ctx.RequestAborted);
        });

        m_app.MapGet("/api/files", (string? path) =>
        {
            try
            {
                return Results.Json(SoilFiles.List(m_paths, path), JsonOptions.Web);
            }
            catch (Exception ex)
            {
                return FileError(ex);
            }
        });

        m_app.MapGet("/api/files/content", (string? path) =>
        {
            try
            {
                return Results.Json(SoilFiles.Read(m_paths, path), JsonOptions.Web);
            }
            catch (Exception ex)
            {
                return FileError(ex);
            }
        });

        m_app.MapGet("/api/files/raw", (HttpContext ctx, string? path) =>
        {
            try
            {
                var (full, contentType) = SoilFiles.Raw(m_paths, path);
                ctx.Response.Headers["Content-Security-Policy"] = "sandbox";
                ctx.Response.Headers.CacheControl = "no-store";
                return Results.File(full, contentType, enableRangeProcessing: true);
            }
            catch (Exception ex)
            {
                return FileError(ex);
            }
        });

        m_app.MapPost("/api/files/open", (HttpRequest req) =>
        {
            try
            {
                Open(req.Query["path"].ToString());
                return Results.NoContent();
            }
            catch (Exception ex)
            {
                return FileError(ex);
            }
        });

        m_app.MapPost("/api/files/reveal", (HttpRequest req) =>
        {
            try
            {
                Reveal(req.Query["path"].ToString());
                return Results.NoContent();
            }
            catch (Exception ex)
            {
                return FileError(ex);
            }
        });
    }

    private void Open(string path)
    {
        var full = m_paths.Resolve(path);
        if (!Directory.Exists(full) && !File.Exists(full))
            throw new FileNotFoundException("Path not found.", full);
        Process.Start(new ProcessStartInfo(full) { UseShellExecute = true });
    }

    private void Reveal(string path)
    {
        var full = m_paths.Resolve(path);
        if (!Directory.Exists(full) && !File.Exists(full))
            throw new FileNotFoundException("Path not found.", full);

        if (Directory.Exists(full))
        {
            Process.Start(new ProcessStartInfo(full) { UseShellExecute = true });
            return;
        }

        var psi = new ProcessStartInfo("explorer.exe")
        {
            UseShellExecute = true,
            Arguments = $"/select,\"{full}\""
        };
        Process.Start(psi);
    }

    private static IResult GitHubError(Exception ex) => ex switch
    {
        ArgumentException or InvalidOperationException =>
            Results.BadRequest(new { error = ex.Message }),
        HttpRequestException =>
            Results.Json(new { error = "The GitHub service could not be reached." }, statusCode: 502),
        _ => Results.Problem(ex.Message)
    };

    private static IResult FileError(Exception ex) => ex switch
    {
        FileNotFoundException or DirectoryNotFoundException =>
            Results.NotFound(new { error = "That item no longer exists." }),
        UnauthorizedAccessException =>
            Results.Json(new { error = "Access denied." }, statusCode: StatusCodes.Status403Forbidden),
        ArgumentException =>
            Results.BadRequest(new { error = ex.Message }),
        _ => Results.Problem(ex.Message)
    };

    public void Start()
    {
        m_app.Start();
        Url = m_app.Urls.FirstOrDefault()
            ?? throw new InvalidOperationException("The local UI server did not bind a port.");
        File.WriteAllText(m_paths.CliEndpointPath,
            JsonSerializer.Serialize(new { url = Url }));
    }

    public void Dispose()
    {
        try
        {
            m_app.StopAsync(TimeSpan.FromSeconds(2)).GetAwaiter().GetResult();
        }
        catch
        {
        }
        try
        {
            m_apps.Dispose();
        }
        catch
        {
        }
        try
        {
            m_agents.Dispose();
        }
        catch
        {
        }
        m_github.Dispose();
        try
        {
            if (File.Exists(m_paths.CliEndpointPath))
            {
                using var endpoint = JsonDocument.Parse(File.ReadAllText(m_paths.CliEndpointPath));
                if (string.Equals(endpoint.RootElement.GetProperty("url").GetString(), Url,
                        StringComparison.OrdinalIgnoreCase))
                    File.Delete(m_paths.CliEndpointPath);
            }
        }
        catch
        {
        }
        m_app.DisposeAsync().AsTask().GetAwaiter().GetResult();
    }
}

public sealed record CreateVmRequest(string? Name, string? Size);

public sealed record CreateAgentRequest(string? Name);

public sealed record GitHubConfigRequest(string? WorkerUrl);

internal static class JsonOptions
{
    public static readonly JsonSerializerOptions Web = new(JsonSerializerDefaults.Web);
}
