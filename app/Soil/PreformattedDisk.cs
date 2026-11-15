using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace Soil;

/// <summary>
/// Clones a build-time preformatted ext4 volume into a new, sparse NTFS file.
/// The packed template stores only initialized sectors, not 40 GiB of zeros.
/// This runs only when a machine is explicitly created, never while parked.
/// </summary>
internal static class PreformattedDisk
{
    private const uint FsctlSetSparse = 0x000900c4;
    private const long GiB = 1024L * 1024 * 1024;
    private const int MaxExtentBytes = 1024 * 1024;
    private static readonly byte[] Magic = Encoding.ASCII.GetBytes("SOILX4P1");

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(
        SafeFileHandle handle, uint code, IntPtr input, uint inputBytes,
        IntPtr output, uint outputBytes, out uint bytesReturned, IntPtr overlapped);

    /// <returns>True if a new disk was provisioned. False when this
    /// machine size, filesystem, or installation has no applicable template.</returns>
    internal static bool TryCreate(string imagesDir, string diskPath, int diskGb)
    {
        if (!OperatingSystem.IsWindows() || diskGb is not (40 or 80 or 160) ||
            File.Exists(diskPath))
            return false;

        var source = Path.Combine(imagesDir, $"data-ext4-{diskGb}g.sparse.gz");
        long diskBytes = diskGb * GiB;
        if (!File.Exists(source))
            return false;

        Directory.CreateDirectory(Path.GetDirectoryName(diskPath)!);
        var temp = diskPath + "." + Guid.NewGuid().ToString("N") + ".initializing";
        try
        {
            using (var target = new FileStream(temp, FileMode.CreateNew,
                       FileAccess.ReadWrite, FileShare.None))
            {
                // File.SetLength on a normal Windows file would reserve the
                // whole disk. Fall back to guest mkfs on non-NTFS.
                if (!DeviceIoControl(target.SafeFileHandle, FsctlSetSparse,
                        IntPtr.Zero, 0, IntPtr.Zero, 0, out _, IntPtr.Zero))
                    return false;

                using var gzip = new GZipStream(File.OpenRead(source), CompressionMode.Decompress);
                using var input = new BinaryReader(gzip, Encoding.ASCII);
                if (!input.ReadBytes(Magic.Length).SequenceEqual(Magic))
                    throw new InvalidDataException("Invalid Soil ext4 template header.");
                if (input.ReadInt64() != diskBytes)
                    throw new InvalidDataException("Unexpected Soil ext4 template size.");

                target.SetLength(diskBytes);
                long lastEnd = 0;
                long written = 0;
                while (true)
                {
                    var offset = input.ReadInt64();
                    var count = input.ReadInt32();
                    if (count == 0 && offset == 0)
                        break;
                    if (count < 1 || count > MaxExtentBytes || offset < lastEnd ||
                        offset > diskBytes - count)
                        throw new InvalidDataException("Invalid or overlapping ext4 template extent.");

                    var bytes = input.ReadBytes(count);
                    if (bytes.Length != count)
                        throw new EndOfStreamException("Truncated Soil ext4 template.");
                    target.Position = offset;
                    target.Write(bytes);
                    lastEnd = offset + count;
                    written += count;
                    if (written > 512L * 1024 * 1024)
                        throw new InvalidDataException("Excessive ext4 template data.");
                }
                if (gzip.ReadByte() != -1)
                    throw new InvalidDataException("Trailing data in ext4 template.");

                // ext4 superblock: 0xEF53 at byte 1024 + 0x38.
                target.Position = 1024 + 0x38;
                if (target.ReadByte() != 0x53 || target.ReadByte() != 0xEF)
                    throw new InvalidDataException("Preformatted disk lacks an ext4 superblock.");
                target.Flush(true);
            }

            // Never overwrite user data. A failed initialization leaves no
            // half-created data disk to be mistaken for a valid ext4 volume.
            File.Move(temp, diskPath, overwrite: false);
            return true;
        }
        finally
        {
            if (File.Exists(temp))
                File.Delete(temp);
        }
    }
}
