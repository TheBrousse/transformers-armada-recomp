"""Reference implementations of the game's VU1 routines (docs/RENDERER.md), checked against
batches saved by PS2X_VU1_CAPTURE: each makes the packet its routine sends from the batch's
input, and the check compares that with the packet VU1 sent, field by field.

    python tools/vu1_routines.py runs/vu1_batch_1.bin [runs/vu1_batch_2.bin ...]

Float arithmetic follows VU1's: single precision rounded toward zero, operands clamped
(exponent 255 reads as +-FLT_MAX), no denormals. A product and a sum are rounded separately
(no fused multiply-add), as the FMAC unit does.
"""
import math
import struct
import sys

from vu1_batch import Batch

FLT_MAX = struct.unpack("<f", struct.pack("<I", 0x7F7FFFFF))[0]


def f32_bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def bits_f32(bits):
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def clamp(value):
    """A VU operand: exponent 255 as +-FLT_MAX, denormals as signed zero."""
    bits = f32_bits(value) if isinstance(value, float) else value
    exponent = (bits >> 23) & 0xFF
    if exponent == 0xFF:
        bits = (bits & 0x80000000) | 0x7F7FFFFF
    elif exponent == 0:
        bits &= 0x80000000
    return bits_f32(bits)


def rz(exact):
    """An exact (double) result to single precision, rounded toward zero, without denormals."""
    if exact == 0.0 or math.isnan(exact):
        return exact
    if abs(exact) >= 2.0 ** 128:
        return math.copysign(FLT_MAX, exact)
    nearest = struct.unpack("<f", struct.pack("<f", exact))[0] if abs(exact) < 3.4028235677973366e38 else math.copysign(FLT_MAX, exact)
    if abs(nearest) > abs(exact):  # rounded away from zero: one step back
        nearest = bits_f32(f32_bits(nearest) - 1)
    if abs(nearest) < 1.1754943508222875e-38:
        return math.copysign(0.0, exact)
    return nearest


def mul(a, b):
    return rz(clamp(a) * clamp(b))


def add(a, b):
    return rz(clamp(a) + clamp(b))


def div(a, b):
    a, b = clamp(a), clamp(b)
    if b == 0.0:
        return math.copysign(FLT_MAX, a) if a != 0.0 else FLT_MAX
    return rz(a / b)


def transform(matrix, position):
    """MULAw/MADDAz/MADDAy/MADDx: row3 * 1, + row2 * z, + row1 * y, + row0 * x, lane by lane."""
    x, y, z = position
    out = []
    for lane in range(4):
        acc = mul(matrix[3][lane], 1.0)
        acc = add(acc, mul(matrix[2][lane], z))
        acc = add(acc, mul(matrix[1][lane], y))
        out.append(add(acc, mul(matrix[0][lane], x)))
    return out


def ftoi4(value):
    scaled = mul(value, 16.0)
    if scaled >= 2147483648.0:
        return 0x7FFFFFFF
    if scaled < -2147483648.0:
        return 0x80000000
    return int(scaled) & 0xFFFFFFFF  # toward zero


def routine_1410(batch):
    """Routine 0x1410's drawing path (q21.x = 0): every vertex through field A's matrix (vf01-vf04)
    and the data block's (field B), divided by w, to 12.4 fixed point; ST times Q; the
    drawing-kick bit from the mask after the vertices. Returns the packet as qwords."""
    count = batch.q(batch.itop, "I")[0] & 0x7FFF
    header_x = batch.q(batch.itop, "I")[0]
    entry = batch.q(5, "I")
    block = entry[1] & 0xFFFF
    field_a = [batch.before.vf[r] for r in (1, 2, 3, 4)]
    field_b = [batch.q(block + r) for r in range(4)]
    tag_tail = batch.q(1023, "I")[1:]
    first = batch.itop + 1
    mask_at = first + 3 * count + 3
    masks = batch.q(mask_at, "I")
    packet = []
    packet += [batch.q(block + 8, "I"), batch.q(block + 9, "I"), batch.q(block + 10, "I")]
    packet.append((count,) + tuple(tag_tail))

    def vertices(matrix):
        out = []
        for v in range(count):
            st = batch.q(first + 3 * v)  # converted in place by the batch type 3 path
            st = [add(st[k], -batch.q(batch.itop)[2]) for k in range(3)]
            colour = batch.q(first + 3 * v + 1, "I")
            position = batch.q(first + 3 * v + 2)[:3]
            clip = transform(matrix, position)
            q = div(1.0, clip[3])
            stq = [f32_bits(mul(st[k], q)) for k in range(3)]
            xyz = [ftoi4(mul(clip[k], q)) for k in range(3)]
            word = masks[0] if v < 16 else masks[1]
            bit = v if v < 16 else v - 16
            shifted = (word << bit) & 0xFFFF
            adc = shifted - 0x10000 if shifted & 0x8000 else shifted  # MFIR: sign extended
            out.append((stq[0], stq[1], stq[2], f32_bits(st[3]) if False else None))
            out.append(colour)
            out.append((xyz[0], xyz[1], xyz[2], adc & 0xFFFFFFFF))
        return out

    packet += vertices(field_a)
    packet += [batch.q(block + 12, "I"), batch.q(block + 13, "I"), batch.q(block + 14, "I")]
    packet.append((header_x,) + tuple(tag_tail))
    packet += vertices(field_b)
    return packet


def compare(batch, expected, made, name):
    sent = [struct.unpack_from("<4I", batch.packets[0], 16 * k) for k in range(len(batch.packets[0]) // 16)]
    if len(sent) != len(made):
        print(f"  {name}: {len(made)} qwords made, {len(sent)} sent")
    differences = 0
    for k, (a, b) in enumerate(zip(made, sent)):
        for lane in range(4):
            if a[lane] is None:
                continue
            if a[lane] != b[lane]:
                differences += 1
                if differences <= 12:
                    print(f"  {name}: qword {k} lane {lane}: made {a[lane]:#010x}, sent {b[lane]:#010x}")
    print(f"  {name}: {differences} words differ")


def main():
    for path in sys.argv[1:]:
        batch = Batch(path)
        print(f"{path}: {batch.routines}, {len(batch.packets)} packets")
        if not batch.packets:
            continue
        if batch.routines.startswith("1410/") and batch.q(21, "I")[0] == 0:
            compare(batch, batch.packets[0], routine_1410(batch), "1410")


if __name__ == "__main__":
    main()
