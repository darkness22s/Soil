using System.Diagnostics;
using System.Net;
using System.Text.Json;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
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

    public string Url { get; private set; } = "";

    public AppServer(VmManager manager, SoilPaths paths)
    {
        m_manager = manager;
        m_paths = paths;

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

        m_app.UseDefaultFiles(new DefaultFilesOptions
        {
            FileProvider = new PhysicalFileProvider(uiRoot)
        });
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
            version = typeof(AppServer).Assembly.GetName().Version?.ToString(3) ?? "0.2.0",
            home = m_paths.Root,
            schema = 1
        }, JsonOptions.Web));

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

        m_app.MapPost("/api/vms/{id}/start", (string id) =>
        {
            var dto = m_manager.Start(id);
            return dto is null ? Results.NotFound() : Results.Json(dto, JsonOptions.Web);
        });

        m_app.MapPost("/api/vms/{id}/stop", (string id) =>
        {
            var dto = m_manager.Stop(id);
            return dto is null ? Results.NotFound() : Results.Json(dto, JsonOptions.Web);
        });

        m_app.MapDelete("/api/vms/{id}", (string id) =>
        {
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

internal static class JsonOptions
{
    public static readonly JsonSerializerOptions Web = new(JsonSerializerDefaults.Web);
}
