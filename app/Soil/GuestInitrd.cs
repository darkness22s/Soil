using System.Globalization;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;

namespace Soil;

/// <summary>
/// Cache a per-VM initramfs with the Soil installation's SSH public key.
/// Linux unpacks concatenated newc cpio archives into its rootfs; the original
/// bundled initrd is not modified. Only a unique PUBLIC key enters the guest.
/// The private key stays in the user's restricted Soil home.
/// </summary>
internal static class GuestInitrd
{
    private static byte[] Pad4(byte[] data)
    {
        if (data.Length % 4 == 0)
            return data;
        Array.Resize(ref data, (data.Length + 3) & ~3);
        return data;
    }

    private static byte[] Entry(string path, byte[] data, int mode, int inode,
                                int uid = 1001, int gid = 50)
    {
        var name = Encoding.ASCII.GetBytes(path + "\0");
        var fields = new int[]
        {
            inode, mode, uid, gid, 1, 0, data.Length,
            0, 0, 0, 0, name.Length, 0
        };
        var header = "070701" + string.Concat(
            fields.Select(value => value.ToString("x8", CultureInfo.InvariantCulture)));
        using var entry = new MemoryStream();
        entry.Write(Encoding.ASCII.GetBytes(header));
        entry.Write(name);
        while (entry.Position % 4 != 0)
            entry.WriteByte(0);
        entry.Write(data);
        while (entry.Position % 4 != 0)
            entry.WriteByte(0);
        return entry.ToArray();
    }

    public static string Prepare(string source, string vmFolder, string publicKeyPath)
    {
        var publicKey = File.ReadAllText(publicKeyPath).Trim();
        if (!publicKey.StartsWith("ssh-ed25519 ", StringComparison.Ordinal) ||
            publicKey.Any(c => c == '\n' || c == '\r') || publicKey.Length > 512)
            throw new InvalidDataException("Expected a valid single-line SSH Ed25519 public key.");

        var cacheDir = Path.Combine(vmFolder, "boot");
        Directory.CreateDirectory(cacheDir);
        var destination = Path.Combine(cacheDir, "core2-ssh.gz");
        var stampPath = Path.Combine(cacheDir, "core2-ssh.stamp");
        // Reuse on every boot; only regenerate after a bundled-image/key change.
        using var hash = SHA256.Create();
        using var image = File.OpenRead(source);
        var imageHash = Convert.ToHexString(hash.ComputeHash(image));
        var keyHash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(publicKey)));
        var stamp = imageHash + ":" + keyHash;
        if (File.Exists(destination) && File.Exists(stampPath) &&
            File.ReadAllText(stampPath) == stamp)
            return destination;

        var temporary = destination + "." + Guid.NewGuid().ToString("N") + ".tmp";
        var stampTemporary = stampPath + ".tmp";
        try
        {
            using (var original = File.OpenRead(source))
            using (var unzip = new GZipStream(original, CompressionMode.Decompress))
            using (var output = File.Create(temporary))
            using (var gzip = new GZipStream(output, CompressionLevel.Optimal))
            {
                unzip.CopyTo(gzip);
                // cpio entries must start on a 4-byte boundary. The Tiny Core
                // original already ends aligned to cpio / 512-byte padding.
                // Guest initramfs is a newc stream, not ISO9660.
                gzip.Write(Entry("home/tc/.ssh", Array.Empty<byte>(), 0x41c0, 91011)); // 040700
                gzip.Write(Entry("home/tc/.ssh/authorized_keys",
                                 Encoding.ASCII.GetBytes(publicKey + "\n"),
                                 0x8180, 91012)); // 0100600
                gzip.Write(Entry("TRAILER!!!", Array.Empty<byte>(), 0, 91013, 0, 0));
            }
            File.WriteAllText(stampTemporary, stamp);
            File.Move(temporary, destination, overwrite: true);
            File.Move(stampTemporary, stampPath, overwrite: true);
            return destination;
        }
        finally
        {
            if (File.Exists(temporary))
                File.Delete(temporary);
            if (File.Exists(stampTemporary))
                File.Delete(stampTemporary);
        }
    }
}
