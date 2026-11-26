# Third-party code

Magnesium OS vendors these single-file libraries unmodified (the upstream
headers are copied byte-for-byte; see the pinned commits below).

| File | Project | Pinned commit | License |
|------|---------|---------------|---------|
| `stb_image.h` (v2.34) | https://github.com/nothings/stb | `6b5799bae8a1fe3792691ef4b72ae26434c3e799` | Public domain or MIT (see `LICENSE.stb`) |
| `pl_mpeg.h` | https://github.com/phoboslab/pl_mpeg | `c871f2be022ece7ef4f64230b4fb8e1fb9eb6023` | MIT (SPDX header in the file; Dominic Szablewski) |

`stb_image` decodes PNG, JPEG, BMP, GIF, TGA, PSD, HDR, PIC and PNM.
`pl_mpeg` decodes MPEG-1 video (.mpg/.mpeg) and MP2 audio (audio is not played yet).
