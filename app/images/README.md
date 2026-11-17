# Guest images

The Soil Windows installer bundles the private Git LFS guest assets:

- `vmlinuz64` — guest kernel
- `core2.gz` — Tiny Core initramfs
- `soil.squashfs` — Linux desktop and base applications

Run `app/build.ps1` with `-ImagesDir <path>` to select your original images.
An ordinary checkout without LFS downloads contains pointers, **not** valid
images.

## Native Linux executable and .sol support

The build **repackages only the published copy of core2.gz**, not the original
private LFS files. It applies the fastboot patch and injects:

- `/bin/soil-native`, the SSH-accessible launcher for imported ELF/AppImage files.
- `/bin/soil`, a static x86_64 Linux build of the SDK runtime supporting
  native kind-4 `.sol` packages and `soil run`, `install`, `launch`,
  `list`, `verify`, and `remove`.

The `/bin/soil` executable is built from the current C source, not downloaded
from an unversioned binary release. GitHub Actions builds it on Ubuntu using
`musl-gcc -static`, validates the ELF machine and lack of a PT_INTERP
(dynamic linker), then passes that exact artifact into the Windows MSI build.
The Windows machine never needs a Linux compiler or guest VM just to bundle
it.

For manual local Windows builds, obtain the `soil-linux-guest-cli` artifact
from the `Build Soil MSI` or `Build Soil Windows installer` workflow for
your source revision and supply:

```powershell
.\app\build.ps1 -ImagesDir .\app\images -GuestSoilExecutable .\tools\soil-ci\out\soil
```

On Linux, generate the executable with:

```sh
sudo apt-get install musl-tools
bash tools/soil-ci/build-guest-cli.sh tools/soil-ci/out/soil
```

The packaging pipeline fails rather than silently shipping a guest without
`/bin/soil`. It verifies both embedded executables after initramfs creation.
The VM boot acceptance workflow uses the actual LFS images under QEMU and
tests a genuine x86_64 ELF **and** a native SquashFS `.sol` through package
verification, direct run, installation, launch, and removal.

This establishes the native package CLI lifecycle; it does not certify every
external AppImage, graphical app, glibc version, dependency bundle, or desktop
session. Those still need application-specific compatibility testing.

Never overwrite or commit private LFS source blobs to add the launcher.
