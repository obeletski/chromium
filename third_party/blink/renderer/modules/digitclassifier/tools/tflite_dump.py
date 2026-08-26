#!/usr/bin/env python3
"""Minimal TFLite flatbuffer reader: dumps tensors, ops, and buffer sizes.

Stdlib only -- the checkout has no `flatbuffers` or `numpy`, and no schema.fbs.
Field indices below come from tensorflow/lite/schema/schema.fbs.
"""

import struct
import sys

TENSOR_TYPE = {
    0: "FLOAT32", 1: "FLOAT16", 2: "INT32", 3: "UINT8", 4: "INT64",
    5: "STRING", 6: "BOOL", 7: "INT16", 8: "COMPLEX64", 9: "INT8",
    10: "FLOAT64", 11: "COMPLEX128", 12: "UINT64", 13: "RESOURCE",
    14: "VARIANT", 15: "UINT32", 16: "UINT16", 17: "INT4",
}

# Only the codes this model can plausibly contain; extend as needed.
BUILTIN_OP = {
    9: "FULLY_CONNECTED", 22: "RESHAPE", 25: "SOFTMAX", 40: "SQUEEZE",
    114: "QUANTIZE", 6: "DEQUANTIZE", 3: "CONV_2D", 17: "MAX_POOL_2D",
}


class Buf:
    def __init__(self, data):
        self.d = data

    def u16(self, p):
        return struct.unpack_from("<H", self.d, p)[0]

    def u32(self, p):
        return struct.unpack_from("<I", self.d, p)[0]

    def i32(self, p):
        return struct.unpack_from("<i", self.d, p)[0]


class Table:
    def __init__(self, buf, pos):
        self.b = buf
        self.pos = pos
        self.vt = pos - buf.i32(pos)
        self.vtsize = buf.u16(self.vt)

    def off(self, fid):
        slot = self.vt + 4 + 2 * fid
        if slot - self.vt >= self.vtsize:
            return 0
        return self.b.u16(slot)

    def u32f(self, fid, default=0):
        o = self.off(fid)
        return self.b.u32(self.pos + o) if o else default

    def u8f(self, fid, default=0):
        o = self.off(fid)
        return self.b.d[self.pos + o] if o else default

    def indirect(self, fid):
        o = self.off(fid)
        if not o:
            return None
        p = self.pos + o
        return p + self.b.u32(p)

    def vector(self, fid):
        """Returns (offset_of_first_element, count)."""
        o = self.off(fid)
        if not o:
            return (None, 0)
        p = self.pos + o
        vp = p + self.b.u32(p)
        return (vp + 4, self.b.u32(vp))

    def string(self, fid):
        p = self.indirect(fid)
        if p is None:
            return None
        n = self.b.u32(p)
        return self.b.d[p + 4:p + 4 + n].decode("utf-8", "replace")


def table_at(buf, vec_start, i):
    p = vec_start + 4 * i
    return Table(buf, p + buf.u32(p))


def int_vector(buf, vec_start, count):
    return [buf.i32(vec_start + 4 * i) for i in range(count)]


def main(path):
    with open(path, "rb") as f:
        data = f.read()
    buf = Buf(data)

    print(f"file: {path}  ({len(data)} bytes)")
    print(f"identifier: {data[4:8]!r}")

    model = Table(buf, buf.u32(0))
    print(f"schema version: {model.u32f(0)}")
    print(f"description: {model.string(3)!r}")

    buffers_start, n_buffers = model.vector(4)
    buf_sizes = []
    buf_data_pos = []
    for i in range(n_buffers):
        t = table_at(buf, buffers_start, i)
        start, count = t.vector(0)  # Buffer.data
        buf_sizes.append(count)
        buf_data_pos.append(start)
    print(f"buffers: {n_buffers}")

    subgraphs_start, n_sg = model.vector(2)
    print(f"subgraphs: {n_sg}\n")

    sg = table_at(buf, subgraphs_start, 0)
    tensors_start, n_tensors = sg.vector(0)
    inputs_start, n_in = sg.vector(1)
    outputs_start, n_out = sg.vector(2)
    ops_start, n_ops = sg.vector(3)

    print(f"inputs:  {int_vector(buf, inputs_start, n_in)}")
    print(f"outputs: {int_vector(buf, outputs_start, n_out)}\n")

    print(f"{'idx':>3}  {'name':<40} {'type':<8} {'shape':<20} {'buf':>4} "
          f"{'bytes':>10}")
    print("-" * 92)
    tensors = []
    for i in range(n_tensors):
        t = table_at(buf, tensors_start, i)
        shp_start, shp_n = t.vector(0)
        shape = int_vector(buf, shp_start, shp_n) if shp_n else []
        ttype = TENSOR_TYPE.get(t.u8f(1), f"?{t.u8f(1)}")
        bidx = t.u32f(2)
        name = t.string(3) or ""
        nbytes = buf_sizes[bidx] if bidx < len(buf_sizes) else 0
        tensors.append((i, name, ttype, shape, bidx, nbytes))
        print(f"{i:>3}  {name[:40]:<40} {ttype:<8} {str(shape):<20} "
              f"{bidx:>4} {nbytes:>10}")

    opcodes_start, n_opcodes = model.vector(1)
    codes = []
    for i in range(n_opcodes):
        t = table_at(buf, opcodes_start, i)
        # deprecated_builtin_code (field 0, byte), builtin_code (field 3, int32)
        bc = t.u32f(3, 0) or t.u8f(0, 0)
        codes.append(bc)

    print(f"\noperators: {n_ops}")
    for i in range(n_ops):
        op = table_at(buf, ops_start, i)
        oc = op.u32f(0)
        ins_s, ins_n = op.vector(1)
        outs_s, outs_n = op.vector(2)
        bc = codes[oc] if oc < len(codes) else -1
        print(f"  [{i}] {BUILTIN_OP.get(bc, f'op#{bc}'):<18} "
              f"in={int_vector(buf, ins_s, ins_n)} "
              f"out={int_vector(buf, outs_s, outs_n)}")

    return tensors, buf_sizes, buf_data_pos, data


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "digit_classifier.tflite")
