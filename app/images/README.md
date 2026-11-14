# Guest images

Place the three guest image files here for a local build:

- `vmlinuz64`
- `core2.gz`
- `soil.squashfs`

`app/build.ps1` copies them into `app/dist/Soil/images` before packaging. You can
also pass `-ImagesDir <path>` instead of using this folder.

The image binaries are private **Git LFS** assets. Their pointer files in
an ordinary source checkout are not usable VM images.

The standard Windows build keeps the original `core2.gz` untouched and
injects `soil-sdk/runtime/soil-native` into the **published** image:
`app/dist/Soil/images/core2.gz`. The existing vmlinuz64 and
soil.squashfs are preserved. This makes the default WHPX VM's Linux
guest able to import and launch native x86-64 ELF / Type 2 AppImage
files through the existing SSH bridge. `app/build.ps1` requires Python
3 to patch the gzip-compressed cpio initramfs; CI verifies it.

The installer bundles these three files from `app/dist/Soil/images`.
Never edit or commit the private LFS source blobs just to add the launcher.
