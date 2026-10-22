# Third-party code

## TJpgDec R0.03 (third_party/tjpgd)

Tiny JPEG Decompressor by ChaN, (C) 2021, http://elm-chan.org/fsw/tjpgd/.
Its license (kept at the top of tjpgd.c) allows use, modification and
redistribution in source and binary form without restriction, as long as
the copyright notice stays with the source.

Changes here: the configuration in tjpgdcnf.h (grayscale output, scaling,
fast decode level 1), the byte-swap field some copies carry is gone, and the
public functions are renamed with `cyb_` defines in tjpgdcnf.h because the
ESP32 ROM links an older TJpgDec under the plain names.

## Cy archive packer (tools/cyarchive.py)

The Cy archive (LZ) packer and reader come from the Dragonfruit project's
tools/cybiko_cap.py (MIT, SovGVD).
