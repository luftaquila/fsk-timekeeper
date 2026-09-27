#!/usr/bin/env python3
"""Convert a raw .bin into a UF2 image for the Adafruit nRF52 UF2 bootloader.

Minimal, dependency-free implementation of the UF2 block format
(https://github.com/microsoft/uf2). 512-byte blocks, 256-byte payloads, with
the family-ID flag set so the bootloader rejects images for the wrong chip.

  bin2uf2.py --base 0x1000 --family 0xADA52840 in.bin out.uf2
"""
import argparse
import struct

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID = 0x00002000
PAYLOAD = 256


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, help="flash load address, e.g. 0x1000")
    ap.add_argument("--family", required=True, help="UF2 family id, e.g. 0xADA52840")
    ap.add_argument("input")
    ap.add_argument("output")
    args = ap.parse_args()

    base = int(args.base, 0)
    family = int(args.family, 0)

    with open(args.input, "rb") as f:
        data = f.read()

    nblocks = (len(data) + PAYLOAD - 1) // PAYLOAD
    out = bytearray()
    for i in range(nblocks):
        chunk = data[i * PAYLOAD:(i + 1) * PAYLOAD]
        block = struct.pack(
            "<IIIIIIII",
            UF2_MAGIC_START0,
            UF2_MAGIC_START1,
            UF2_FLAG_FAMILY_ID,
            base + i * PAYLOAD,
            len(chunk),
            i,
            nblocks,
            family,
        )
        block += chunk + b"\x00" * (476 - len(chunk))
        block += struct.pack("<I", UF2_MAGIC_END)
        assert len(block) == 512
        out += block

    with open(args.output, "wb") as f:
        f.write(out)
    print(f"bin2uf2: {len(data)} bytes -> {nblocks} blocks @ {hex(base)} -> {args.output}")


if __name__ == "__main__":
    main()
