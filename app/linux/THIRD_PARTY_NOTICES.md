# Third-party computer-use component

Linux VM computer control uses **vncdotool 1.4.2**, authored by Marc Sibson and
contributors, from https://github.com/sibson/vncdotool (MIT license).

The unmodified vncdotool package and its declared dependencies are installed
into `vendor/` when building the Linux tarball. The original vncdotool license
is preserved at `third-party/vncdotool-LICENSE.txt`; dependency package metadata
and license files are retained within `vendor/` as installed by pip.

Soil's `computer_use.py` is only a REST-to-vncdo command adapter. Screenshot
capture and mouse/keyboard event delivery are provided by the upstream
vncdotool project, not recreated by Soil.

Upstream: https://github.com/sibson/vncdotool
License: https://github.com/sibson/vncdotool/blob/main/LICENSE.txt
