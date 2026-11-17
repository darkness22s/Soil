using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Text;
using System.Text.Json;

namespace Soil;

// Only the two host types referenced by the CLI are stubbed. The real
// production SoilCli.cs is compiled into this cross-platform smoke harness.
internal sealed class SoilPaths
{
    public string CliEndpointPath { get; }
    public string ComputerTokenPath => Path.Combine(Path.GetDirectoryName(CliEndpointPath)!, "computer.token");
    public SoilPaths(string endpoint) => CliEndpointPath = endpoint;
}

internal static class NativeLinuxAppManager
{
    public const long MaxBytes = 256L * 1024 * 1024;
}

internal static class Program
{
    private const string VmId = "11111111111111111111111111111111";
    private const string AppId = "22222222222222222222222222222222";
    private static readonly MethodInfo CliRun = typeof(SoilCli).GetMethod(
        "RunCommandLine", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new Exception("Cannot find production CLI entry point.");

    public static async Task<int> Main()
    {
        try
        {
            await TestInstallAsync(noLaunch: false, launchFails: false);
            await TestInstallAsync(noLaunch: true, launchFails: false);
            await TestInstallAsync(noLaunch: false, launchFails: true);
            await TestComputerApiAsync();
            TestMcpToolCatalog();
            TestNativeCuaImageForwarding();
            Console.WriteLine("PASS: production Soil CLI imports, launches, and recovers from launch failure");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
    }

    private static async Task TestInstallAsync(bool noLaunch, bool launchFails)
    {
        var temp = Path.Combine(Path.GetTempPath(), "soil-cli-smoke-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temp);
        using var portReservation = new TcpListener(IPAddress.Loopback, 0);
        portReservation.Start();
        var port = ((IPEndPoint)portReservation.LocalEndpoint).Port;
        portReservation.Stop();
        var url = $"http://127.0.0.1:{port}/";
        using var server = new HttpListener();
        server.Prefixes.Add(url);
        server.Start();

        try
        {
            var filePath = Path.Combine(temp, "Example.AppImage");
            var bytes = new byte[4096];
            bytes[0] = 0x7f;
            bytes[1] = (byte)'E';
            bytes[2] = (byte)'L';
            bytes[3] = (byte)'F';
            File.WriteAllBytes(filePath, bytes);
            var endpoint = Path.Combine(temp, "endpoint.json");
            File.WriteAllText(endpoint, JsonSerializer.Serialize(new { url }));
            var received = new List<string>();
            var expectedRequests = noLaunch ? 2 : 3;

            var host = Task.Run(async () =>
            {
                for (var i = 0; i < expectedRequests; i++)
                {
                    var context = await server.GetContextAsync().WaitAsync(TimeSpan.FromSeconds(15));
                    var request = context.Request;
                    received.Add(request.HttpMethod + " " + request.Url!.AbsolutePath);
                    var code = 200;
                    string body;
                    if (request.HttpMethod == "GET" && request.Url.AbsolutePath == "/api/vms")
                    {
                        body = JsonSerializer.Serialize(new[] {
                            new { id = VmId, name = "My Desktop", kind = "desktop", status = "stopped" }
                        });
                    }
                    else if (request.HttpMethod == "POST" &&
                             request.Url.AbsolutePath == $"/api/vms/{VmId}/native-apps")
                    {
                        using var ms = new MemoryStream();
                        await request.InputStream.CopyToAsync(ms);
                        Check(ms.Length == bytes.Length, "The app bytes were not uploaded.");
                        Check(request.Headers["X-Soil-Filename"] == "Example.AppImage",
                            "The source filename was not preserved.");
                        body = JsonSerializer.Serialize(new {
                            id = AppId, name = "Example", format = "AppImage", bytes = ms.Length
                        });
                    }
                    else if (request.HttpMethod == "POST" &&
                             request.Url.AbsolutePath == $"/api/vms/{VmId}/native-apps/{AppId}/launch")
                    {
                        code = launchFails ? 409 : 200;
                        body = launchFails
                            ? "{\"error\":\"Guest launcher failed: missing shared libraries\"}"
                            : JsonSerializer.Serialize(new { id = AppId, name = "Example" });
                    }
                    else
                    {
                        throw new Exception("Unexpected CLI request: " + received[^1]);
                    }
                    var encoded = Encoding.UTF8.GetBytes(body);
                    context.Response.StatusCode = code;
                    context.Response.ContentType = "application/json";
                    context.Response.ContentLength64 = encoded.Length;
                    await context.Response.OutputStream.WriteAsync(encoded);
                    context.Response.Close();
                }
            });

            var arguments = new List<string> { "vm", "app", "install", filePath, "--vm", VmId };
            if (noLaunch)
                arguments.Add("--no-launch");
            var stdout = Console.Out;
            var stderr = Console.Error;
            using var output = new StringWriter();
            using var errors = new StringWriter();
            int code;
            try
            {
                Console.SetOut(output);
                Console.SetError(errors);
                code = (int)(CliRun.Invoke(null, new object[] {
                    arguments.ToArray(), new SoilPaths(endpoint)
                }) ?? throw new Exception("CLI returned no exit code"));
            }
            finally
            {
                Console.SetOut(stdout);
                Console.SetError(stderr);
            }

            await host;
            Check(code == (launchFails ? 1 : 0),
                $"Wrong exit code {code}: {errors}");
            Check(received.Count == expectedRequests, "Unexpected number of requests.");
            Check(received[0] == "GET /api/vms", "Desktop discovery was not first.");
            Check(received[1] == $"POST /api/vms/{VmId}/native-apps", "App was not imported.");
            Check(noLaunch || received[2].EndsWith("/launch"), "Guest launch was not requested.");
            if (launchFails)
            {
                Check(errors.ToString().Contains(AppId), "Retryable app ID was not reported.");
                Check(errors.ToString().Contains("missing shared libraries"),
                    "Guest error was not relayed to the agent.");
            }
            else
            {
                Check(output.ToString().Contains(noLaunch ? "imported" : "launched"),
                    "CLI success status is incorrect.");
            }
        }
        finally
        {
            server.Stop();
            Directory.Delete(temp, recursive: true);
        }
    }

    private static async Task TestComputerApiAsync()
    {
        var temp = Path.Combine(Path.GetTempPath(), "soil-computer-smoke-" +
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temp);
        var endpoint = Path.Combine(temp, "endpoint.json");
        var token = new string('a', 64);
        File.WriteAllText(Path.Combine(temp, "computer.token"), token);
        using var reservation = new TcpListener(IPAddress.Loopback, 0);
        reservation.Start();
        var port = ((IPEndPoint)reservation.LocalEndpoint).Port;
        reservation.Stop();
        var url = $"http://127.0.0.1:{port}/";
        using var server = new HttpListener();
        server.Prefixes.Add(url);
        server.Start();
        File.WriteAllText(endpoint, JsonSerializer.Serialize(new { url }));

        try
        {
            var requests = new List<string>();
            var host = Task.Run(async () =>
            {
                for (var i = 0; i < 2; i++)
                {
                    var ctx = await server.GetContextAsync().WaitAsync(TimeSpan.FromSeconds(15));
                    requests.Add(ctx.Request.HttpMethod + " " + ctx.Request.Url!.AbsolutePath);
                    Check(ctx.Request.Headers["Authorization"] == "Bearer " + token,
                        "Computer endpoint was called without the per-install bearer token.");
                    byte[] payload;
                    if (i == 0)
                    {
                        Check(ctx.Request.HttpMethod == "GET", "Screenshot must be GET.");
                        payload = new byte[64];
                        payload[0] = 0xff; payload[1] = 0xd8; payload[2] = 0xff;
                    }
                    else
                    {
                        Check(ctx.Request.HttpMethod == "POST", "Computer action must be POST.");
                        using var doc = await JsonDocument.ParseAsync(ctx.Request.InputStream);
                        Check(doc.RootElement.GetProperty("action").GetString() == "key",
                            "Computer action body was not forwarded.");
                        payload = Encoding.UTF8.GetBytes("{\"ok\":true}");
                    }
                    ctx.Response.StatusCode = 200;
                    ctx.Response.ContentType = i == 0 ? "image/jpeg" : "application/json";
                    ctx.Response.ContentLength64 = payload.Length;
                    await ctx.Response.OutputStream.WriteAsync(payload);
                    ctx.Response.Close();
                }
            });

            var outputFile = Path.Combine(temp, "capture.jpg");
            var previousOut = Console.Out;
            var previousError = Console.Error;
            using var capturedOut = new StringWriter();
            using var capturedErr = new StringWriter();
            try
            {
                Console.SetOut(capturedOut);
                Console.SetError(capturedErr);
                int screenshot = (int)CliRun.Invoke(null, new object[] {
                    new[] { "vm", "computer", "screenshot", VmId, outputFile },
                    new SoilPaths(endpoint)
                })!;
                Check(screenshot == 0, "Computer screenshot command failed: " + capturedErr);
                Check(File.ReadAllBytes(outputFile)[0] == 0xff, "Screenshot bytes were not saved.");
                int action = (int)CliRun.Invoke(null, new object[] {
                    new[] { "vm", "computer", "action", VmId, "{\"action\":\"key\",\"key\":\"enter\"}" },
                    new SoilPaths(endpoint)
                })!;
                Check(action == 0, "Computer action command failed: " + capturedErr);
                Check(capturedOut.ToString().Contains("ok"), "Action success was not reported.");
            }
            finally
            {
                Console.SetOut(previousOut);
                Console.SetError(previousError);
            }
            await host;
            Check(requests.Count == 2 && requests[0].EndsWith("/computer/screenshot") &&
                  requests[1].EndsWith("/computer/action"),
                "Computer API endpoints were not called in order.");
        }
        finally
        {
            server.Stop();
            Directory.Delete(temp, true);
        }
    }

    private static void TestNativeCuaImageForwarding()
    {
        var blocks = new List<object>();
        var output = JsonSerializer.Serialize(new {
            content = new object[] {
                new { type = "text", text = "Cua desktop screenshot" },
                new { type = "image", mimeType = "image/png", data = "iVBORw0KGgo=" }
            }
        });
        SoilComputerCli.AddCuaContent(blocks, output);
        var json = JsonSerializer.Serialize(blocks);
        Check(blocks.Count == 2 && json.Contains("image/png") &&
              json.Contains("iVBORw0KGgo="),
              "The Cua MCP bridge must return native screenshot image blocks.");
        var plain = new List<object>();
        SoilComputerCli.AddCuaContent(plain, "Cua Driver list-tools");
        Check(JsonSerializer.Serialize(plain).Contains("list-tools"),
              "Driver metadata must be forwarded as readable text.");
    }

    private static void TestMcpToolCatalog()
    {
        var temp = Path.Combine(Path.GetTempPath(), "soil-mcp-smoke-" +
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temp);
        var endpoint = Path.Combine(temp, "endpoint.json");
        File.WriteAllText(endpoint, "{\"url\":\"http://127.0.0.1:12345/\"}");
        var oldIn = Console.In;
        var oldOut = Console.Out;
        using var output = new StringWriter();
        try
        {
            Console.SetIn(new StringReader(
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n" +
                "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}\n"));
            Console.SetOut(output);
            var code = (int)CliRun.Invoke(null, new object[] {
                new[] { "vm", "computer", "mcp" }, new SoilPaths(endpoint)
            })!;
            Check(code == 0, "MCP bridge returned a nonzero exit code.");
            var text = output.ToString();
            Check(text.Contains("soil_vm_screenshot") && text.Contains("soil_vm_action") &&
                  text.Contains("soil_list_vms") && text.Contains("soil_vm_cua_catalog") &&
                  text.Contains("soil_vm_cua_call"), "MCP did not expose its computer tools.");
            Check(text.Split('\n', StringSplitOptions.RemoveEmptyEntries).Length == 2,
                "MCP must return one JSON-RPC message per request.");
        }
        finally
        {
            Console.SetIn(oldIn);
            Console.SetOut(oldOut);
            Directory.Delete(temp, true);
        }
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
            throw new Exception(message);
    }
}
