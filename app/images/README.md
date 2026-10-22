# Guest images

Place the three guest image files here for a local build:

- `vmlinuz64`
- `core2.gz`
- `soil.squashfs`

`app/build.ps1` copies them into `app/dist/Soil/images` before packaging. You can
also pass `-ImagesDir <path>` instead of using this folder.

The image files are intentionally not committed to this repository.
