#!/usr/bin/env python3
"""Turn a base64 radio capture from MQTT into a C array fixture.

The reader publishes raw oversampled captures to everblu/cyble/capture/<what>,
base64 because 684 bytes of hex does not fit an MQTT packet. This converts one
back into the form ADR-0003 wants checked in: a byte array the native test suite
can feed straight to decode_4bitpbit_serial, with no radio and no meter involved.

    mosquitto_sub -h broker -t 'everblu/cyble/capture/resp' -C 1 \
        | python scripts/decode_capture.py --name resp_capture

    python scripts/decode_capture.py --name ack_capture < capture.b64
"""

import argparse
import base64
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--name",
        default="capture",
        help="identifier for the generated array (default: capture)",
    )
    parser.add_argument(
        "--per-line",
        type=int,
        default=12,
        help="bytes per source line (default: 12)",
    )
    parser.add_argument(
        "input",
        nargs="?",
        type=argparse.FileType("r"),
        default=sys.stdin,
        help="file holding the base64 payload (default: stdin)",
    )
    args = parser.parse_args()

    payload = "".join(args.input.read().split())
    if not payload:
        print("no base64 payload on input", file=sys.stderr)
        return 1

    try:
        raw = base64.b64decode(payload, validate=True)
    except Exception as exc:
        print(f"not valid base64: {exc}", file=sys.stderr)
        return 1

    # The capture is oversampled 4x over 11 bits per byte, so this is what the
    # frame would be if every sample decoded. Printed as a sanity check against
    # what the reader claimed it received, not used for anything.
    frame_bytes = len(raw) * 8 // (4 * 11)
    print(f"// {len(raw)} raw bytes, ~{frame_bytes} decoded bytes at 4x oversampling")
    print(f"static const uint8_t {args.name}[] = {{")
    for offset in range(0, len(raw), args.per_line):
        chunk = raw[offset : offset + args.per_line]
        body = " ".join(f"0x{b:02X}," for b in chunk)
        print(f"    {body}")
    print("};")
    print(f"static const uint32_t {args.name}_len = {len(raw)};")

    return 0


if __name__ == "__main__":
    sys.exit(main())
