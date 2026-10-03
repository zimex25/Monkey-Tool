# Third-party notices

Monkey Tool's own code is under the [MIT License](LICENSE). The pieces below belong to other projects and stay under their own licenses.

## Included in this repository

### LZHAM decompressor: `src/lzham/`

Rich Geldreich's LZHAM decoder, compiled into the tool through `src/nfsnl_lzham.cpp`.
Public domain (see [`src/lzham/LICENSE`](src/lzham/LICENSE)), with a two-line portability fix.
<https://github.com/richgel999/lzham_codec>

### Brotli decoder: `src/brotli/`

Google's reference Brotli decoder, compiled into the tool through `src/nfsnl_brotli.cpp`.
MIT License, Copyright (c) 2009, 2010, 2013-2016 by the Brotli Authors. Full text: [`src/brotli/LICENSE`](src/brotli/LICENSE).
<https://github.com/google/brotli>

### vgmstream: `tools/vgmstream/`

Unmodified Windows binaries of vgmstream-cli and the DLLs it ships with. The tool runs it to play and convert Wwise sounds.

* **vgmstream**: ISC-style license, Copyright (c) 2008-2025 Adam Gashlin, Fastelbja, Ronny Elfert, bnnm and others. Full text: [`tools/vgmstream/COPYING`](tools/vgmstream/COPYING). <https://github.com/vgmstream/vgmstream>
* The DLLs next to it are other projects' libraries, as distributed by vgmstream:
  * FFmpeg (`avcodec`, `avformat`, `avutil`, `swresample`): LGPL 2.1 or later
  * mpg123 (`libmpg123-0.dll`): LGPL 2.1
  * libvorbis: BSD-style
  * Speex: BSD-style
  * CELT: BSD-style
  * LibAtrac9: MIT
  * G.719 decoder: ITU-T reference code

  Their sources are linked from the vgmstream project. The LGPL libraries are unmodified dynamic libraries and can be replaced with other builds.

## Downloaded at build time (not in this repository)

| Project | Used for | License |
|---------|----------|---------|
| [zstd](https://github.com/facebook/zstd) (`libzstd.dll`) | 3D models, texture cabinets | BSD 3-Clause (or GPLv2) |
| [w64devkit](https://github.com/skeeto/w64devkit) | the compiler `BUILD.bat` fetches when none is installed; build only, not shipped | Unlicense / public domain, with GCC under the GPL with the runtime exception |

When you publish a release that includes `libzstd.dll`, include its license text too. The GitHub workflow does this for you.

## Game content

No game files are included. The splash pictures, game logos and icons under `src/` (`splash_*.jpg`, `logo_*.png`, `icon_*.png`) show the games the tool supports. They are the property of their owners (Electronic Arts and its studios) and are **not** covered by the MIT License.
