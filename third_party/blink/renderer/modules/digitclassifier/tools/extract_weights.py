#!/usr/bin/env python3
"""Extract MNIST MLP weights from digit_classifier.tflite into a flat blob.

Run once offline; the emitted .bin is what ships in the APK. The runtime never
parses flatbuffers.

Blob layout (little-endian, 32-byte header then float32 payload):

    0   char[4]  magic "DCLS"
    4   u32      version = 1
    8   u32      input_size  = 2352
    12  u32      hidden_size = 128
    16  u32      output_size = 10
    20  u32[3]   reserved (zero)
    32  f32[hidden*input]  W1, row-major [128][2352]
        f32[hidden]        b1
        f32[output*hidden] W2, row-major [10][128]
        f32[output]        b2
"""

import struct
import sys

import tflite_dump

MAGIC = b"DCLS"
VERSION = 1
INPUT_SIZE = 2352
HIDDEN_SIZE = 128
OUTPUT_SIZE = 10

# (tensor name, expected shape) -> resolved from the subgraph, then written in
# this order. Names come from the MLIR converter and are stable for this file.
EXPECTED = [
    ("sequential/dense/MatMul", [HIDDEN_SIZE, INPUT_SIZE]),
    ("dense/bias", [HIDDEN_SIZE]),
    ("sequential/dense_1/MatMul", [OUTPUT_SIZE, HIDDEN_SIZE]),
    ("dense_1/bias", [OUTPUT_SIZE]),
]


def stats(floats):
    lo = min(floats)
    hi = max(floats)
    mean = sum(floats) / len(floats)
    return lo, hi, mean


def main(model_path, out_path):
    tensors, buf_sizes, buf_pos, data = tflite_dump.main(model_path)
    print()

    by_name = {name: (idx, ttype, shape, bidx, nbytes)
               for (idx, name, ttype, shape, bidx, nbytes) in tensors}

    payload = bytearray()
    for name, want_shape in EXPECTED:
        if name not in by_name:
            sys.exit(f"ERROR: tensor {name!r} not found in model")
        idx, ttype, shape, bidx, nbytes = by_name[name]
        if ttype != "FLOAT32":
            sys.exit(f"ERROR: {name} is {ttype}, expected FLOAT32. "
                     "A quantized model needs dequantization support.")
        if shape != want_shape:
            sys.exit(f"ERROR: {name} has shape {shape}, expected {want_shape}")

        count = 1
        for d in shape:
            count *= d
        if nbytes != count * 4:
            sys.exit(f"ERROR: {name} buffer is {nbytes} bytes, "
                     f"expected {count * 4}")

        start = buf_pos[bidx]
        floats = list(struct.unpack_from(f"<{count}f", data, start))

        if not all(f == f and abs(f) != float("inf") for f in floats):
            sys.exit(f"ERROR: {name} contains NaN or Inf")

        lo, hi, mean = stats(floats)
        print(f"{name:<28} {str(shape):<14} n={count:<8} "
              f"min={lo:+.4f} max={hi:+.4f} mean={mean:+.6f}")

        payload += struct.pack(f"<{count}f", *floats)

    header = MAGIC + struct.pack("<5I", VERSION, INPUT_SIZE, HIDDEN_SIZE,
                                 OUTPUT_SIZE, 0) + struct.pack("<2I", 0, 0)
    assert len(header) == 32, len(header)

    expected_payload = (HIDDEN_SIZE * INPUT_SIZE + HIDDEN_SIZE +
                        OUTPUT_SIZE * HIDDEN_SIZE + OUTPUT_SIZE) * 4
    assert len(payload) == expected_payload, (len(payload), expected_payload)

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(payload)

    print(f"\nwrote {out_path}: {len(header) + len(payload)} bytes "
          f"({len(header)} header + {len(payload)} weights)")


if __name__ == "__main__":
    model = sys.argv[1] if len(sys.argv) > 1 else "digit_classifier.tflite"
    out = sys.argv[2] if len(sys.argv) > 2 else "digit_classifier_weights.bin"
    main(model, out)
