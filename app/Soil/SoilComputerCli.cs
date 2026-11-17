using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;

namespace Soil;

/// <summary>
/// Local stdio MCP and shell bridge for Soil's VM-scoped computer capabilities.
/// An agent addresses each VM explicitly; no guest SSH, VNC port, or long-running
/// helper is needed. Console stdout is reserved for MCP protocol messages.
/// </summary>
internal static class SoilComputerCli
{
    private static readonly JsonSerializerOptions Pretty = new() { WriteIndented = true };
    private const string Prefix = "api/vms/";

    public static int Run(HttpClient client, SoilPaths paths, string[] args)
    {
        if (args.Length == 3 && args[2] == "mcp")
            return RunMcp(client, paths);
        if (args.Length >= 4 && args[2] == "screenshot" && args.Length <= 5)
        {
            var vmId = args[3];
            var output = args.Length == 5 ? args[4] : "soil-screen.jpg";
            var bytes = Capture(client, paths, vmId);
            File.WriteAllBytes(output, bytes);
            Console.WriteLine(Path.GetFullPath(output));
            return 0;
        }
        if (args.Length == 5 && args[2] == "action")
        {
            using var doc = JsonDocument.Parse(args[4]);
            var result = Action(client, paths, args[3], doc.RootElement);
            Console.WriteLine(JsonSerializer.Serialize(result, Pretty));
            return 0;
        }
        if (args.Length is 5 or 6 && args[2] == "cua")
        {
            using var doc = JsonDocument.Parse(args.Length == 6 ? args[5] : "{}");
            var result = CuaCall(client, paths, args[3], args[4], doc.RootElement);
            Console.WriteLine(result.GetProperty("output").GetString());
            return 0;
        }
        Console.Error.WriteLine("Usage: Soil.exe vm computer screenshot <vm-id> [file.jpg]");
        Console.Error.WriteLine("       Soil.exe vm computer action <vm-id> '{\"action\":\"key\",\"key\":\"enter\"}'");
        Console.Error.WriteLine("       Soil.exe vm computer cua <vm-id> <tool> [json]");
        Console.Error.WriteLine("       Soil.exe vm computer mcp   (stdio MCP server)");
        return 2;
    }

    private static string Endpoint(string id, string suffix)
    {
        if (id.Length != 32 || !id.All(Uri.IsHexDigit))
            throw new ArgumentException("Expected a 32-character Soil VM ID.");
        return Prefix + id + "/computer/" + suffix;
    }

    private static void Authorize(HttpRequestMessage request, SoilPaths paths)
    {
        var token = File.ReadAllText(paths.ComputerTokenPath).Trim();
        if (token.Length != 64 || !token.All(Uri.IsHexDigit))
            throw new InvalidOperationException("Soil computer-use token is missing or invalid.");
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
    }

    private static byte[] Capture(HttpClient client, SoilPaths paths, string vmId)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, Endpoint(vmId, "screenshot"));
        Authorize(request, paths);
        using var response = client.Send(request);
        EnsureSuccess(response);
        var bytes = response.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult();
        if (bytes.Length is < 64 or > 16_777_216 ||
            bytes[0] != 0xFF || bytes[1] != 0xD8)
            throw new InvalidDataException("Soil returned an invalid or oversized JPEG screenshot.");
        return bytes;
    }

    private static JsonElement Action(HttpClient client, SoilPaths paths, string vmId, JsonElement body)
    {
        using var request = new HttpRequestMessage(HttpMethod.Post, Endpoint(vmId, "action"));
        Authorize(request, paths);
        request.Content = new StringContent(body.GetRawText(), Encoding.UTF8, "application/json");
        using var response = client.Send(request);
        EnsureSuccess(response);
        using var doc = JsonDocument.Parse(response.Content.ReadAsStringAsync().GetAwaiter().GetResult());
        return doc.RootElement.Clone();
    }

    private static JsonElement CuaCall(
        HttpClient client, SoilPaths paths, string vmId, string tool, JsonElement body)
    {
        if (tool.Length > 40 || !tool.All(ch => char.IsAsciiLetterOrDigit(ch) || ch == '_'))
            throw new ArgumentException("Invalid upstream Cua Driver tool name.");
        using var request = new HttpRequestMessage(
            HttpMethod.Post, Endpoint(vmId, "cua/" + Uri.EscapeDataString(tool)));
        Authorize(request, paths);
        request.Content = new StringContent(body.GetRawText(), Encoding.UTF8, "application/json");
        using var response = client.Send(request);
        EnsureSuccess(response);
        using var doc = JsonDocument.Parse(response.Content.ReadAsStringAsync().GetAwaiter().GetResult());
        return doc.RootElement.Clone();
    }

    private static void EnsureSuccess(HttpResponseMessage response)
    {
        if (response.IsSuccessStatusCode)
            return;
        var body = response.Content.ReadAsStringAsync().GetAwaiter().GetResult();
        try
        {
            using var doc = JsonDocument.Parse(body);
            if (doc.RootElement.TryGetProperty("error", out var error))
                throw new InvalidOperationException(error.GetString() ?? "Computer tool failed.");
        }
        catch (JsonException) { }
        throw new InvalidOperationException("Soil computer API HTTP " + (int)response.StatusCode);
    }

    private static int RunMcp(HttpClient client, SoilPaths paths)
    {
        string? line;
        while ((line = Console.ReadLine()) is not null)
        {
            if (string.IsNullOrWhiteSpace(line))
                continue;
            JsonElement id = default;
            try
            {
                using var doc = JsonDocument.Parse(line);
                var root = doc.RootElement;
                if (root.TryGetProperty("id", out var idValue))
                    id = idValue.Clone();
                if (!root.TryGetProperty("method", out var methodValue))
                    throw new ArgumentException("Missing MCP method.");
                var method = methodValue.GetString();
                object result = method switch
                {
                    "initialize" => new {
                        protocolVersion = "2025-06-18",
                        capabilities = new { tools = new { listChanged = false } },
                        serverInfo = new { name = "soil-vm-computer", version = "1.0.0" }
                    },
                    "ping" => new { },
                    "tools/list" => new { tools = Tools() },
                    "tools/call" => InvokeTool(client, paths, root.GetProperty("params")),
                    "notifications/initialized" => new { },
                    _ => throw new ArgumentException("Unknown MCP method: " + method)
                };
                if (id.ValueKind != JsonValueKind.Undefined)
                    Console.WriteLine(JsonSerializer.Serialize(new { jsonrpc = "2.0", id, result }));
            }
            catch (Exception error) when (error is JsonException or ArgumentException or
                                          IOException or HttpRequestException or InvalidOperationException)
            {
                if (id.ValueKind != JsonValueKind.Undefined)
                    Console.WriteLine(JsonSerializer.Serialize(new {
                        jsonrpc = "2.0", id,
                        error = new { code = -32603, message = error.Message }
                    }));
                else
                    Console.Error.WriteLine("Soil MCP: " + error.Message);
            }
        }
        return 0;
    }

    private static object[] Tools() => new object[]
    {
        new {
            name = "soil_list_vms",
            description = "List Soil VMs and their states. Only use the VM the user selected.",
            inputSchema = new { type = "object", properties = new { }, additionalProperties = false }
        },
        new {
            name = "soil_vm_screenshot",
            description = "Observe the selected running VM desktop as a JPEG. Do this before and after actions.",
            inputSchema = new {
                type = "object",
                properties = new { vm_id = new { type = "string", description = "Soil VM ID (32 hex digits)" } },
                required = new[] { "vm_id" },
                additionalProperties = false
            }
        },
        new {
            name = "soil_vm_cua_call",
            description = "Run an actual pinned upstream open-source Cua Driver tool INSIDE the selected VM. " +
                "Use when its X11 desktop is ready. Supports get_desktop_state, get_window_state, " +
                "list_apps, click, double_click, move_cursor, scroll, type_text, press_key, get_cursor_position. " +
                "The guest driver is preloaded in Soil's bundled guest image; errors mean it cannot " +
                "reach the desktop, and should not be interpreted as success.",
            inputSchema = new {
                type = "object",
                properties = new {
                    vm_id = new { type = "string" },
                    tool = new { type = "string" },
                    arguments = new { type = "object" }
                },
                required = new[] { "vm_id", "tool" },
                additionalProperties = false
            }
        },
        new {
            name = "soil_vm_action",
            description = "Send mouse/keyboard input into the chosen running Soil VM. " +
                "Actions: move, click, double_click (x,y), key (key name/ctrl+a), type (ASCII text). " +
                "Pixel coordinates refer to a 1360x768 screenshot; verify after clicking. " +
                "Mouse is relative PS/2 and may be inaccurate on accelerated guests.",
            inputSchema = new {
                type = "object",
                properties = new {
                    vm_id = new { type = "string" },
                    action = new { type = "string", @enum = new[] { "move", "click", "double_click", "key", "type" } },
                    x = new { type = "integer" },
                    y = new { type = "integer" },
                    button = new { type = "string", @enum = new[] { "left", "middle", "right" } },
                    key = new { type = "string" },
                    text = new { type = "string" }
                },
                required = new[] { "vm_id", "action" },
                additionalProperties = false
            }
        }
    };

    private static object InvokeTool(HttpClient client, SoilPaths paths, JsonElement parameters)
    {
        var name = parameters.GetProperty("name").GetString();
        var args = parameters.TryGetProperty("arguments", out var a) ? a :
            JsonSerializer.SerializeToElement(new { });
        var content = new List<object>();
        try
        {
            switch (name)
            {
                case "soil_list_vms":
                {
                    using var response = client.GetAsync("api/vms").GetAwaiter().GetResult();
                    EnsureSuccess(response);
                    content.Add(new {
                        type = "text",
                        text = response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
                    });
                    break;
                }
                case "soil_vm_screenshot":
                {
                    var vmId = args.GetProperty("vm_id").GetString()!;
                    content.Add(new {
                        type = "image", mimeType = "image/jpeg",
                        data = Convert.ToBase64String(Capture(client, paths, vmId))
                    });
                    break;
                }
                case "soil_vm_cua_call":
                {
                    var vmId = args.GetProperty("vm_id").GetString()!;
                    var tool = args.GetProperty("tool").GetString()!;
                    var body = args.TryGetProperty("arguments", out var provided)
                        ? provided.Clone() : JsonSerializer.SerializeToElement(new { });
                    var result = CuaCall(client, paths, vmId, tool, body);
                    content.Add(new { type = "text", text = result.GetProperty("output").GetString() });
                    break;
                }
                case "soil_vm_action":
                {
                    var vmId = args.GetProperty("vm_id").GetString()!;
                    using var doc = JsonDocument.Parse(args.GetRawText());
                    var action = JsonSerializer.SerializeToElement(
                        doc.RootElement.EnumerateObject().Where(p => p.Name != "vm_id")
                            .ToDictionary(p => p.Name, p => p.Value.Clone()));
                    content.Add(new { type = "text", text = Action(client, paths, vmId, action).GetRawText() });
                    // Return a fresh observation with every action, as in CUA
                    // screenshot-action-verify loops. The agent can compare
                    // successive images rather than assuming input succeeded.
                    Thread.Sleep(150);
                    content.Add(new {
                        type = "image", mimeType = "image/jpeg",
                        data = Convert.ToBase64String(Capture(client, paths, vmId))
                    });
                    break;
                }
                default: throw new ArgumentException("Unknown Soil computer tool: " + name);
            }
            return new { content, isError = false };
        }
        catch (Exception error) when (error is JsonException or ArgumentException or
                                      IOException or InvalidDataException or
                                      HttpRequestException or InvalidOperationException)
        {
            return new {
                content = new object[] { new { type = "text", text = "Soil computer: " + error.Message } },
                isError = true
            };
        }
    }
}
