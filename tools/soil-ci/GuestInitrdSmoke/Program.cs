using System.IO.Compression;
using System.Text;
using Soil;

if (args.Length != 1 || !File.Exists(args[0]))
    throw new ArgumentException("Provide published core2.gz path.");

var root = Path.Combine(Path.GetTempPath(), "soil-guest-key-test-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
try
{
    var source = args[0];
    var publicPath = Path.Combine(root, "id_ed25519.pub");
    var firstKey = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA soil-test-a";
    var secondKey = firstKey.Replace("soil-test-a", "soil-test-b");
    File.WriteAllText(publicPath, firstKey + "\n");
    var vm = Path.Combine(root, "machine");
    var originalHash = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(source)));
    var result = GuestInitrd.Prepare(source, vm, publicPath);
    Check(result, firstKey);
    var firstImage = File.ReadAllBytes(result);
    var repeat = GuestInitrd.Prepare(source, vm, publicPath);
    if (repeat != result || !firstImage.AsSpan().SequenceEqual(File.ReadAllBytes(repeat)))
        throw new InvalidDataException("Cached per-VM guest image changed.");
    File.WriteAllText(publicPath, secondKey + "\n");
    var updated = GuestInitrd.Prepare(source, vm, publicPath);
    Check(updated, secondKey);
    if (firstImage.AsSpan().SequenceEqual(File.ReadAllBytes(updated)))
        throw new InvalidDataException("Guest image did not change with SSH key.");
    if (originalHash != Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(source))))
        throw new InvalidDataException("Original bundled core2.gz was modified.");
    Console.WriteLine("PASS: per-VM SSH authorization is embedded, cache invalidates and source is unchanged.");
}
finally
{
    Directory.Delete(root, recursive: true);
}

static void Check(string image, string key)
{
    using var stream = File.OpenRead(image);
    using var unzip = new GZipStream(stream, CompressionMode.Decompress);
    using var memory = new MemoryStream();
    unzip.CopyTo(memory);
    var archive = memory.ToArray();
    var path = Encoding.ASCII.GetBytes("home/tc/.ssh/authorized_keys\0");
    var offset = archive.AsSpan().LastIndexOf(path);
    if (offset < 110)
        throw new InvalidDataException("Missing SSH authorized_keys in initramfs.");
    var headerOffset = offset - 110;
    if (Encoding.ASCII.GetString(archive, headerOffset, 6) != "070701")
        throw new InvalidDataException("Invalid cpio header.");
    var mode = Convert.ToInt32(Encoding.ASCII.GetString(archive, headerOffset + 14, 8), 16);
    if (mode != 0x8180)
        throw new InvalidDataException("authorized_keys should have 0600 owner-only permissions.");
    var contentOffset = (offset + path.Length + 3) & ~3;
    var expected = Encoding.ASCII.GetBytes(key + "\n");
    if (!archive.AsSpan(contentOffset, expected.Length).SequenceEqual(expected))
        throw new InvalidDataException("SSH key content mismatch.");
}
