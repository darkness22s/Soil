using System.Text.Json;

namespace Soil;

// Compile and exercise the real VM computer-use backend without Windows WHPX.
internal sealed record VmDto(string Status);
internal sealed class VmManager
{
    public readonly List<JsonElement> Packets = new();
    public string Status = "running";
    public bool InputAvailable = true;
    public VmDto? Get(string id) => id == "vm" ? new VmDto(Status) : null;
    public byte[]? FrameJpeg(string id) => Get(id) is null ? null :
        new byte[] { 0xff, 0xd8, 0xff, 0xd9 };
    public bool SendInput(string id, JsonElement data)
    {
        if (!InputAvailable || Status != "running") return false;
        Packets.Add(data.Clone());
        return true;
    }
}

internal static class Program
{
    public static async Task<int> Main()
    {
        try
        {
            var vm = new VmManager();
            var computer = new VmComputerTools(vm);
            Check(computer.Capture("vm")[0] == 0xff, "VM screen not captured.");
            await computer.ActionAsync("vm", JsonSerializer.SerializeToElement(
                new { action = "click", x = 200, y = 180 }), default);
            Check(vm.Packets.Count == 4, "Click must anchor, move, press and release.");
            Check(vm.Packets[0].GetProperty("dx").GetInt32() == -1600, "Mouse not anchored.");
            Check(vm.Packets[1].GetProperty("dy").GetInt32() == -180, "Y coordinate not inverted.");
            Check(vm.Packets[2].GetProperty("buttons").GetInt32() == 1, "Mouse press missing.");
            Check(vm.Packets[3].GetProperty("buttons").GetInt32() == 0, "Mouse released incorrectly.");
            vm.Packets.Clear();
            await computer.ActionAsync("vm", JsonSerializer.SerializeToElement(
                new { action = "key", key = "ctrl+a" }), default);
            Check(vm.Packets.Count == 4, "Modifier keys not pressed and released.");
            Check(vm.Packets[0].GetProperty("vk").GetInt32() == 0x11, "Ctrl key missing.");
            Check(!vm.Packets[^1].GetProperty("down").GetBoolean(), "Ctrl modifier still held.");
            vm.Packets.Clear();
            await computer.ActionAsync("vm", JsonSerializer.SerializeToElement(
                new { action = "type", text = "Hi!" }), default);
            Check(vm.Packets.Count == 10, "ASCII typing and shift events not delivered.");
            Check(vm.Packets.All(p => p.GetProperty("type").GetString() == "key"),
                "Type must use the keyboard transport.");
            vm.Status = "stopped";
            await ExpectError(() => computer.ActionAsync("vm",
                JsonSerializer.SerializeToElement(new { action = "click", x = 1, y = 2 }),
                default), "stopped");
            vm.Status = "running";
            await ExpectError(() => computer.ActionAsync("vm",
                JsonSerializer.SerializeToElement(new { action = "click", x = 1400, y = 2 }),
                default), "x");
            await ExpectError(() => computer.ActionAsync("vm",
                JsonSerializer.SerializeToElement(new { action = "scroll", x = 1, y = 2 }),
                default), "Scroll");
            vm.InputAvailable = false;
            await ExpectError(() => computer.ActionAsync("vm",
                JsonSerializer.SerializeToElement(new { action = "key", key = "enter" }),
                default), "input channel");
            Console.WriteLine("PASS: real VM computer transport validates, captures and delivers bounded input");
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex);
            return 1;
        }
    }

    private static async Task ExpectError(Func<Task> operation, string fragment)
    {
        try { await operation(); }
        catch (Exception error)
        {
            Check(error.Message.Contains(fragment, StringComparison.OrdinalIgnoreCase),
                "Wrong computer tool error: " + error.Message);
            return;
        }
        throw new Exception("Expected computer tool error: " + fragment);
    }

    private static void Check(bool success, string message)
    {
        if (!success) throw new Exception(message);
    }
}
