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

namespace Boxify;

public sealed class AppServer : IDisposable
{
    private readonly WebApplication m_app;
    private readonly VmManager m_manager;

    public string Url { get; private set; } = "";

    public AppServer(VmManager manager)
    {
        m_manager = manager;

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

        m_app.MapGet("/api/health", () => Results.Json(new { ok = true }, JsonOptions.Web));
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
    }

    public void Start()
    {
        m_app.Start();
        Url = m_app.Urls.FirstOrDefault()
            ?? throw new InvalidOperationException("The local UI server did not bind a port.");
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
        m_app.DisposeAsync().AsTask().GetAwaiter().GetResult();
    }
}

public sealed record CreateVmRequest(string? Name, string? Size);

internal static class JsonOptions
{
    public static readonly JsonSerializerOptions Web = new(JsonSerializerDefaults.Web);
}
