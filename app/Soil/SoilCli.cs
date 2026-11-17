using System.Net.Http.Json;
using System.Net.Http.Headers;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace Soil;

internal static class SoilCli
{
    private const uint AttachParentProcess = 0xFFFFFFFF;
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool AttachConsole(uint processId);

    public static int Run(string[] args, SoilPaths paths)
    {
        if (!AttachConsole(AttachParentProcess))
            return RunWithoutConsole(args, paths);
        Console.SetOut(new StreamWriter(Console.OpenStandardOutput()) { AutoFlush = true });
        Console.SetError(new StreamWriter(Console.OpenStandardError()) { AutoFlush = true });
        return RunCommandLine(args, paths);
    }

    private static int RunWithoutConsole(string[] args, SoilPaths paths)
    {
        return RunCommandLine(args, paths);
    }

    private static int RunCommandLine(string[] args, SoilPaths paths)
    {
        if (args.Length == 0 || args[0] is "help" or "--help" or "-h")
        {
            PrintHelp();
            return 0;
        }

        if (!File.Exists(paths.CliEndpointPath))
        {
            Console.Error.WriteLine("Soil is not running. Start the Soil desktop app, then retry.");
            return 1;
        }

        try
        {
            using var endpoint = JsonDocument.Parse(File.ReadAllText(paths.CliEndpointPath));
            var baseUrl = endpoint.RootElement.GetProperty("url").GetString();
            if (string.IsNullOrWhiteSpace(baseUrl))
                throw new InvalidOperationException("The Soil CLI endpoint file is invalid.");
            // Configure all HttpClient properties before the first request. Native app
            // import needs enough time for file transfer, guest boot and launch.
            using var client = new HttpClient
            {
                BaseAddress = new Uri(baseUrl),
                Timeout = TimeSpan.FromMinutes(6)
            };
            return RunCommand(client, args);
        }
        catch (Exception ex) when (ex is IOException or JsonException or HttpRequestException or
                                   InvalidOperationException or UriFormatException)
        {
            Console.Error.WriteLine($"Soil command failed: {ex.Message}");
            return 1;
        }
    }

    private static int RunCommand(HttpClient client, string[] args)
    {
        if (args[0] != "vm" || args.Length < 2)
        {
            PrintHelp();
            return 2;
        }

        HttpResponseMessage response;
        switch (args[1])
        {
            case "list" when args.Length == 2:
                response = client.GetAsync("api/vms").GetAwaiter().GetResult();
                break;
            case "create" when args.Length is 3 or 5:
            {
                var size = args.Length == 5 && args[3] == "--size" ? args[4] : "m";
                if (args.Length == 5 && (args[3] != "--size" || size is not ("s" or "m" or "l")))
                {
                    Console.Error.WriteLine("Size must be s, m, or l.");
                    return 2;
                }
                response = client.PostAsJsonAsync("api/vms", new { name = args[2], size })
                    .GetAwaiter().GetResult();
                break;
            }
            case "start" when args.Length == 3:
                response = client.PostAsync($"api/vms/{Uri.EscapeDataString(args[2])}/start", null)
                    .GetAwaiter().GetResult();
                break;
            case "stop" when args.Length == 3:
                response = client.PostAsync($"api/vms/{Uri.EscapeDataString(args[2])}/stop", null)
                    .GetAwaiter().GetResult();
                break;
            case "delete" when args.Length == 3:
                response = client.DeleteAsync($"api/vms/{Uri.EscapeDataString(args[2])}")
                    .GetAwaiter().GetResult();
                break;
            case "app":
                return RunNativeAppCommand(client, args);
            default:
                PrintHelp();
                return 2;
        }

        using (response)
        {
            var content = response.Content.ReadAsStringAsync().GetAwaiter().GetResult();
            if (!response.IsSuccessStatusCode)
            {
                Console.Error.WriteLine(string.IsNullOrWhiteSpace(content)
                    ? $"Soil returned HTTP {(int)response.StatusCode}."
                    : content);
                return 1;
            }
            if (string.IsNullOrWhiteSpace(content))
                return 0;
            using var result = JsonDocument.Parse(content);
            Console.WriteLine(JsonSerializer.Serialize(result.RootElement, JsonOptions));
            return 0;
        }
    }


    // Single agent-facing entry point. Soil handles target discovery, import,
    // guest boot, transfer and launch; callers never touch internal SSH keys.
    private static int RunNativeAppCommand(HttpClient client, string[] args)
    {
        var action = args.Length > 2 ? args[2] : "";
        if (action is not ("install" or "list" or "launch" or "remove"))
        {
            PrintHelp();
            return 2;
        }
        var requiresOperand = action != "list";
        if (requiresOperand && args.Length < 4)
        {
            PrintHelp();
            return 2;
        }
        var operand = requiresOperand ? args[3] : "";
        var vmSelector = (string?)null;
        var noLaunch = false;
        for (var i = requiresOperand ? 4 : 3; i < args.Length; i++)
        {
            if (args[i] == "--vm" && i + 1 < args.Length && vmSelector is null)
                vmSelector = args[++i];
            else if (args[i] == "--no-launch" && action == "install" && !noLaunch)
                noLaunch = true;
            else
            {
                Console.Error.WriteLine("Unknown Linux app option. See Soil.exe --help.");
                return 2;
            }
        }

        using var vmsResponse = client.GetAsync("api/vms").GetAwaiter().GetResult();
        if (!vmsResponse.IsSuccessStatusCode)
            return PrintApiError(vmsResponse);
        using var doc = JsonDocument.Parse(
            vmsResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult());
        var desktops = doc.RootElement.EnumerateArray()
            .Where(vm => vm.TryGetProperty("kind", out var kind) &&
                         kind.GetString() == "desktop")
            .ToArray();
        var matches = vmSelector is null ? desktops : desktops.Where(vm =>
            string.Equals(vm.GetProperty("id").GetString(), vmSelector, StringComparison.OrdinalIgnoreCase) ||
            string.Equals(vm.GetProperty("name").GetString(), vmSelector, StringComparison.OrdinalIgnoreCase))
            .ToArray();
        if (matches.Length != 1)
        {
            Console.Error.WriteLine(matches.Length == 0
                ? "No matching desktop VM. Create a Soil desktop VM first, or use --vm <id>."
                : "More than one desktop VM matches. Specify --vm <id>.");
            foreach (var vm in desktops)
                Console.Error.WriteLine("  " + vm.GetProperty("id").GetString() +
                                        "  " + vm.GetProperty("name").GetString());
            return 2;
        }
        var selected = matches[0];
        var vmId = selected.GetProperty("id").GetString()!;
        var basePath = "api/vms/" + Uri.EscapeDataString(vmId) + "/native-apps";

        if (action == "list")
        {
            using var response = client.GetAsync(basePath).GetAwaiter().GetResult();
            return PrintApiResponse(response);
        }
        if (action is "launch" or "remove")
        {
            var target = basePath + "/" + Uri.EscapeDataString(operand);
            using var response = action == "launch"
                ? client.PostAsync(target + "/launch", null).GetAwaiter().GetResult()
                : client.DeleteAsync(target).GetAwaiter().GetResult();
            return PrintApiResponse(response);
        }

        // "install" intentionally requires a local user-chosen file, never a URL.
        string filePath;
        try { filePath = Path.GetFullPath(operand); }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException or PathTooLongException)
        {
            Console.Error.WriteLine("Invalid Linux app path: " + ex.Message);
            return 2;
        }
        if (!File.Exists(filePath))
        {
            Console.Error.WriteLine("Linux app file does not exist: " + filePath);
            return 2;
        }
        var info = new FileInfo(filePath);
        if (info.Length < 64 || info.Length > NativeLinuxAppManager.MaxBytes)
        {
            Console.Error.WriteLine("The Linux executable must be between 64 bytes and 256 MiB.");
            return 2;
        }

        using var request = new HttpRequestMessage(HttpMethod.Post, basePath);
        request.Headers.TryAddWithoutValidation("X-Soil-Filename",
            Uri.EscapeDataString(Path.GetFileName(filePath)));
        request.Content = new StreamContent(File.OpenRead(filePath));
        request.Content.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
        request.Content.Headers.ContentLength = info.Length;
        using var imported = client.SendAsync(request).GetAwaiter().GetResult();
        if (!imported.IsSuccessStatusCode)
            return PrintApiError(imported);

        using var result = JsonDocument.Parse(
            imported.Content.ReadAsStringAsync().GetAwaiter().GetResult());
        var appId = result.RootElement.GetProperty("id").GetString()!;
        var appName = result.RootElement.GetProperty("name").GetString()!;
        if (!noLaunch)
        {
            using var launched = client.PostAsync(
                basePath + "/" + Uri.EscapeDataString(appId) + "/launch", null)
                .GetAwaiter().GetResult();
            if (!launched.IsSuccessStatusCode)
            {
                Console.Error.WriteLine("Imported " + appName + " (app ID " + appId +
                    "), but it could not launch. It remains saved in the VM.");
                Console.Error.WriteLine("Retry: Soil.exe vm app launch " + appId +
                    " --vm " + vmId);
                return PrintApiError(launched);
            }
        }
        Console.WriteLine(JsonSerializer.Serialize(new
        {
            vmId, vmName = selected.GetProperty("name").GetString(),
            appId, appName, status = noLaunch ? "imported" : "launched"
        }, JsonOptions));
        return 0;
    }

    private static int PrintApiResponse(HttpResponseMessage response)
    {
        if (!response.IsSuccessStatusCode)
            return PrintApiError(response);
        var body = response.Content.ReadAsStringAsync().GetAwaiter().GetResult();
        if (!string.IsNullOrWhiteSpace(body))
        {
            using var doc = JsonDocument.Parse(body);
            Console.WriteLine(JsonSerializer.Serialize(doc.RootElement, JsonOptions));
        }
        return 0;
    }

    private static int PrintApiError(HttpResponseMessage response)
    {
        var body = response.Content.ReadAsStringAsync().GetAwaiter().GetResult();
        try
        {
            using var doc = JsonDocument.Parse(body);
            if (doc.RootElement.TryGetProperty("error", out var error))
            {
                Console.Error.WriteLine("Soil: " + error.GetString());
                return 1;
            }
        }
        catch (JsonException) { }
        Console.Error.WriteLine("Soil returned HTTP " + (int)response.StatusCode +
            (string.IsNullOrWhiteSpace(body) ? "." : ": " + body));
        return 1;
    }

    private static void PrintHelp()
    {
        Console.WriteLine("Soil command line");
        Console.WriteLine("  Soil.exe vm list");
        Console.WriteLine("  Soil.exe vm create <name> [--size s|m|l]");
        Console.WriteLine("  Soil.exe vm start <id>");
        Console.WriteLine("  Soil.exe vm stop <id>");
        Console.WriteLine("  Soil.exe vm delete <id>");
        Console.WriteLine("  Soil.exe vm app install <file.AppImage|file.elf> [--vm <id|name>] [--no-launch]");
        Console.WriteLine("  Soil.exe vm app list [--vm <id|name>]");
        Console.WriteLine("  Soil.exe vm app launch <app-id> [--vm <id|name>]");
        Console.WriteLine("  Soil.exe vm app remove <app-id> [--vm <id|name>]");
        Console.WriteLine("Start the Soil desktop app before running VM commands.");
    }
}
