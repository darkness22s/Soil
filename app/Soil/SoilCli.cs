using System.Net.Http.Json;
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
            using var client = new HttpClient { BaseAddress = new Uri(baseUrl) };
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

    private static void PrintHelp()
    {
        Console.WriteLine("Soil command line");
        Console.WriteLine("  Soil.exe vm list");
        Console.WriteLine("  Soil.exe vm create <name> [--size s|m|l]");
        Console.WriteLine("  Soil.exe vm start <id>");
        Console.WriteLine("  Soil.exe vm stop <id>");
        Console.WriteLine("  Soil.exe vm delete <id>");
        Console.WriteLine("Start the Soil desktop app before running VM commands.");
    }
}
