using System.Text.Json;

namespace Soil;

/// <summary>
/// VM-scoped computer-use transport. Uses the existing engine framebuffer and
/// PS/2 IPC rather than installing a glibc-dependent daemon into Tiny Core.
/// Input is serialized per VM and is never sent to a stopped/failed guest.
/// </summary>
internal sealed class VmComputerTools
{
    private readonly VmManager m_vms;
    private readonly System.Collections.Concurrent.ConcurrentDictionary<string, SemaphoreSlim> m_gates = new();
    internal const int Width = 1360;
    internal const int Height = 768;

    public VmComputerTools(VmManager vms) => m_vms = vms;

    public byte[] Capture(string id)
    {
        EnsureRunning(id);
        return m_vms.FrameJpeg(id)
            ?? throw new InvalidOperationException(
                "No desktop frame is available yet. Verify the VM has booted, then retry screenshot.");
    }

    public async Task ActionAsync(string id, JsonElement body, CancellationToken ct)
    {
        if (body.ValueKind != JsonValueKind.Object)
            throw new ArgumentException("Expected a computer action object.");
        var gate = m_gates.GetOrAdd(id, _ => new SemaphoreSlim(1, 1));
        await gate.WaitAsync(ct);
        try
        {
            EnsureRunning(id);
            var action = Field(body, "action");
            switch (action)
            {
                case "move":
                    MoveTo(id, Coordinate(body, "x", Width), Coordinate(body, "y", Height));
                    break;
                case "click":
                case "double_click":
                {
                    var x = Coordinate(body, "x", Width);
                    var y = Coordinate(body, "y", Height);
                    var button = body.TryGetProperty("button", out var btn)
                        ? btn.GetString() : "left";
                    var mask = button switch
                    {
                        "left" => 1, "right" => 2, "middle" => 4,
                        _ => throw new ArgumentException("button must be left, right, or middle.")
                    };
                    MoveTo(id, x, y);
                    for (var i = 0; i < (action == "double_click" ? 2 : 1); i++)
                    {
                        Send(id, new { type = "button", buttons = mask });
                        Send(id, new { type = "button", buttons = 0 });
                        if (action == "double_click" && i == 0)
                            await Task.Delay(85, ct);
                    }
                    break;
                }
                case "key":
                {
                    var keys = Field(body, "key").ToLowerInvariant().Split('+');
                    if (keys.Length is < 1 or > 4)
                        throw new ArgumentException("Use a key name or a modifier shortcut, e.g. ctrl+a.");
                    var pressed = new List<int>();
                    try
                    {
                        foreach (var key in keys)
                        {
                            var vk = KeyCode(key);
                            Send(id, new { type = "key", vk, down = true });
                            pressed.Add(vk);
                        }
                    }
                    finally
                    {
                        for (var i = pressed.Count - 1; i >= 0; i--)
                            Send(id, new { type = "key", vk = pressed[i], down = false });
                    }
                    break;
                }
                case "type":
                {
                    var value = Field(body, "text");
                    if (value.Length is < 1 or > 4096)
                        throw new ArgumentException("text must contain 1..4096 characters.");
                    foreach (var ch in value)
                    {
                        ct.ThrowIfCancellationRequested();
                        var (vk, shift) = CharacterCode(ch);
                        if (shift)
                            Send(id, new { type = "key", vk = 0x10, down = true });
                        try
                        {
                            Send(id, new { type = "key", vk, down = true });
                            Send(id, new { type = "key", vk, down = false });
                        }
                        finally
                        {
                            if (shift)
                                Send(id, new { type = "key", vk = 0x10, down = false });
                        }
                    }
                    break;
                }
                default:
                    throw new ArgumentException(
                        "Supported actions: move, click, double_click, key, type. " +
                        "Scroll/drag and Unicode typing require guest driver support and are not yet available.");
            }
        }
        finally { gate.Release(); }
    }

    private void EnsureRunning(string id)
    {
        var vm = m_vms.Get(id);
        if (vm is null)
            throw new FileNotFoundException("The requested VM does not exist.");
        if (vm.Status != "running")
            throw new InvalidOperationException(
                "VM is " + vm.Status + ". Start the selected VM first; computer use never starts VMs automatically.");
    }

    private static int Coordinate(JsonElement root, string field, int upper)
    {
        if (!root.TryGetProperty(field, out var element) || !element.TryGetInt32(out var value) ||
            value < 0 || value >= upper)
            throw new ArgumentException(field + " must be an integer in [0, " + (upper - 1) + "].");
        return value;
    }

    private static string Field(JsonElement root, string field)
    {
        if (!root.TryGetProperty(field, out var value) || value.ValueKind != JsonValueKind.String ||
            string.IsNullOrEmpty(value.GetString()))
            throw new ArgumentException("Missing computer action field: " + field);
        return value.GetString()!;
    }

    // The VM engine currently exposes relative PS/2 pointer input only.
    // Anchor at the top-left screen boundary before every absolute move so
    // action order or UI pointer lock cannot leave a stale accumulated offset.
    // Some guests apply mouse acceleration, so verify each click by screenshot.
    private void MoveTo(string id, int x, int y)
    {
        Send(id, new { type = "move", dx = -1600, dy = 1600, buttons = 0 });
        Send(id, new { type = "move", dx = x, dy = -y, buttons = 0 });
    }

    private void Send(string id, object packet)
    {
        if (!m_vms.SendInput(id, JsonSerializer.SerializeToElement(packet)))
            throw new InvalidOperationException(
                "The VM input channel is not available. Check that the VM is running.");
    }

    private static int KeyCode(string key)
    {
        if (key.Length == 1 && char.IsAsciiLetterOrDigit(key[0]))
            return char.ToUpperInvariant(key[0]);
        if (key.Length is 2 or 3 && key[0] == 'f' &&
            int.TryParse(key[1..], out var fn) && fn is >= 1 and <= 12)
            return 0x6F + fn;
        return key switch
        {
            "ctrl" or "control" => 0x11, "shift" => 0x10, "alt" => 0x12,
            "meta" or "win" or "super" => 0x5B,
            "enter" or "return" => 0x0D, "tab" => 0x09, "esc" or "escape" => 0x1B,
            "backspace" => 0x08, "delete" => 0x2E, "space" => 0x20,
            "up" => 0x26, "down" => 0x28, "left" => 0x25, "right" => 0x27,
            "home" => 0x24, "end" => 0x23, "pageup" => 0x21, "pagedown" => 0x22,
            _ => throw new ArgumentException("Unsupported key: " + key)
        };
    }

    private static (int vk, bool shift) CharacterCode(char ch)
    {
        if (ch is >= 'a' and <= 'z') return (char.ToUpperInvariant(ch), false);
        if (ch is >= 'A' and <= 'Z') return (ch, true);
        if (ch is >= '0' and <= '9') return (ch, false);
        if (ch is '\n' or '\r') return (0x0D, false);
        if (ch == '\t') return (0x09, false);
        if (ch == ' ') return (0x20, false);
        return ch switch
        {
            '.' => (0xBE, false), ',' => (0xBC, false), '/' => (0xBF, false),
            '-' => (0xBD, false), '=' => (0xBB, false), ';' => (0xBA, false),
            '\'' => (0xDE, false), '[' => (0xDB, false), ']' => (0xDD, false),
            '\\' => (0xDC, false), '`' => (0xC0, false),
            '!' => (0x31, true), '@' => (0x32, true), '#' => (0x33, true),
            '$' => (0x34, true), '%' => (0x35, true), '^' => (0x36, true),
            '&' => (0x37, true), '*' => (0x38, true), '(' => (0x39, true),
            ')' => (0x30, true), ':' => (0xBA, true), '"' => (0xDE, true),
            '?' => (0xBF, true), '<' => (0xBC, true), '>' => (0xBE, true),
            '_' => (0xBD, true), '+' => (0xBB, true), '{' => (0xDB, true),
            '}' => (0xDD, true), '|' => (0xDC, true), '~' => (0xC0, true),
            _ => throw new ArgumentException(
                "Unicode character U+" + ((int)ch).ToString("X4") +
                " cannot be typed with the current PS/2 keyboard bridge.")
        };
    }
}
