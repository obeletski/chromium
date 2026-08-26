#!/usr/bin/env python3
"""Pure-python forward pass over digit_classifier_weights.bin.

This is the CPU oracle the GPU implementation is validated against. It mirrors
what the WGSL shader does, in the same order:

    hidden[j] = relu(dot(W1[j], x) + b1[j])
    logits[i] = dot(W2[i], hidden) + b2[i]
    answer    = argmax(logits)        # softmax omitted, it is monotonic

Input is 28*28*3 NHWC floats in [0,1], matching the canvas-derived buffer the
web API takes.
"""

import math
import struct
import sys

MAGIC = b"DCLS"


def load(path):
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:4] != MAGIC:
        sys.exit(f"bad magic {blob[:4]!r}")
    version, n_in, n_hid, n_out, _ = struct.unpack_from("<5I", blob, 4)
    if version != 1:
        sys.exit(f"unsupported blob version {version}")

    p = 32
    w1 = struct.unpack_from(f"<{n_hid * n_in}f", blob, p)
    p += n_hid * n_in * 4
    b1 = struct.unpack_from(f"<{n_hid}f", blob, p)
    p += n_hid * 4
    w2 = struct.unpack_from(f"<{n_out * n_hid}f", blob, p)
    p += n_out * n_hid * 4
    b2 = struct.unpack_from(f"<{n_out}f", blob, p)
    p += n_out * 4
    assert p == len(blob), (p, len(blob))
    return n_in, n_hid, n_out, w1, b1, w2, b2


def forward(x, n_in, n_hid, n_out, w1, b1, w2, b2):
    hidden = [0.0] * n_hid
    for j in range(n_hid):
        row = j * n_in
        acc = b1[j]
        for k in range(n_in):
            acc += w1[row + k] * x[k]
        hidden[j] = acc if acc > 0.0 else 0.0

    logits = [0.0] * n_out
    for i in range(n_out):
        row = i * n_hid
        acc = b2[i]
        for h in range(n_hid):
            acc += w2[row + h] * hidden[h]
        logits[i] = acc
    return logits


def softmax(z):
    m = max(z)
    e = [math.exp(v - m) for v in z]
    s = sum(e)
    return [v / s for v in e]


def nhwc_from_gray(gray, channels=3):
    """gray is 784 floats in [0,1]; replicate across channels like the JS does."""
    out = []
    for v in gray:
        out.extend([v] * channels)
    return out


def render(gray):
    ramp = " .:-=+*#%@"
    for r in range(28):
        row = "".join(ramp[min(9, int(gray[r * 28 + c] * 9.99))]
                      for c in range(28))
        print("  |" + row + "|")


def draw_one():
    """A centered vertical stroke -- should read as a '1'."""
    g = [0.0] * 784
    for r in range(5, 24):
        for c in (13, 14):
            g[r * 28 + c] = 1.0
    return g


def draw_seven():
    """Top bar plus a descending diagonal -- should read as a '7'."""
    g = [0.0] * 784
    for c in range(8, 21):
        for r in (5, 6):
            g[r * 28 + c] = 1.0
    for i in range(18):
        r = 6 + i
        c = 20 - (i * 11) // 18
        g[r * 28 + c] = 1.0
        g[r * 28 + c - 1] = 1.0
    return g


def draw_zero():
    """An ellipse outline -- should read as a '0'."""
    g = [0.0] * 784
    for t in range(360):
        a = math.radians(t)
        r = int(14 + 8.5 * math.sin(a))
        c = int(14 + 6.0 * math.cos(a))
        if 0 <= r < 28 and 0 <= c < 28:
            g[r * 28 + c] = 1.0
            if c + 1 < 28:
                g[r * 28 + c + 1] = 1.0
    return g


def run(name, gray, params):
    n_in, n_hid, n_out, w1, b1, w2, b2 = params
    x = nhwc_from_gray(gray)
    assert len(x) == n_in, (len(x), n_in)
    logits = forward(x, n_in, n_hid, n_out, w1, b1, w2, b2)
    probs = softmax(logits)
    best = max(range(n_out), key=lambda i: logits[i])

    print(f"\n=== {name} ===")
    render(gray)
    print(f"  ink={sum(gray):.1f} px")
    print("  probs: " + "  ".join(f"{d}:{probs[d]:.3f}" for d in range(n_out)))
    print(f"  -> predicted \"{best}\"  (p={probs[best]:.4f})")
    # argmax(logits) must equal argmax(softmax(logits)); the shader relies on it.
    assert best == max(range(n_out), key=lambda i: probs[i])
    return best


def main(path):
    params = load(path)
    print(f"loaded {path}: input={params[0]} hidden={params[1]} "
          f"output={params[2]}")

    run("synthetic 1", draw_one(), params)
    run("synthetic 7", draw_seven(), params)
    run("synthetic 0", draw_zero(), params)

    # Degenerate input: a blank canvas should still produce a valid
    # distribution (this is the bias-only path through both layers).
    run("blank", [0.0] * 784, params)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1
         else "digit_classifier_weights.bin")
