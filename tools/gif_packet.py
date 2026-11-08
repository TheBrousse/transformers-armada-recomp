"""Decodes a GIF packet dump (runs/xgkick_*.bin from PS2X_VIF_STATS_DUMP): tags, A+D
register writes and vertices (debugging aid).

    python tools/gif_packet.py runs/xgkick_000_pc2bd0.bin [--max 12]
"""
import argparse
import struct

REGS = {0: "PRIM", 1: "RGBAQ", 2: "ST", 3: "UV", 4: "XYZF2", 5: "XYZ2", 6: "TEX0_1", 7: "TEX0_2", 8: "CLAMP_1",
        9: "CLAMP_2", 0xA: "FOG", 0xC: "XYZF3", 0xD: "XYZ3", 0xE: "A+D", 0xF: "NOP"}
AD = {0x00: "PRIM", 0x06: "TEX0_1", 0x07: "TEX0_2", 0x08: "CLAMP_1", 0x09: "CLAMP_2", 0x14: "TEX1_1", 0x15: "TEX1_2",
      0x18: "XYOFFSET_1", 0x19: "XYOFFSET_2", 0x1A: "PRMODECONT", 0x1B: "PRMODE", 0x22: "SCANMSK", 0x3B: "TEXA",
      0x3F: "TEXFLUSH", 0x40: "SCISSOR_1", 0x41: "SCISSOR_2", 0x42: "ALPHA_1", 0x43: "ALPHA_2", 0x47: "TEST_1",
      0x48: "TEST_2", 0x4A: "FBA_1", 0x4B: "FBA_2", 0x4C: "FRAME_1", 0x4D: "FRAME_2", 0x4E: "ZBUF_1", 0x4F: "ZBUF_2"}

PRIM_TYPES = ["point", "line", "linestrip", "tri", "tristrip", "trifan", "sprite", "?"]


def ad_text(reg, value):
    """An A+D register write, its value readable for the registers that place a draw."""
    name = AD.get(reg, "%02x" % reg)
    if reg in (0x40, 0x41):  # SCISSOR: x0-x1, y0-y1
        return (f"{name}=x {value & 0x7FF}-{(value >> 16) & 0x7FF} "
                f"y {(value >> 32) & 0x7FF}-{(value >> 48) & 0x7FF}")
    if reg in (0x18, 0x19):  # XYOFFSET in 1/16 pixels
        return f"{name}=({(value & 0xFFFF) / 16:.2f},{((value >> 32) & 0xFFFF) / 16:.2f})"
    if reg == 0x00:
        return f"{name}={prim_text(value)}"
    return f"{name}={value:#x}"


def prim_text(prim):
    bits = [PRIM_TYPES[prim & 7]] + [name for bit, name in ((3, "iip"), (4, "tme"), (5, "fge"), (6, "abe"),
                                                            (7, "aa1"), (8, "fst")) if (prim >> bit) & 1]
    return " ".join(bits) + f" ctxt{((prim >> 9) & 1) + 1}"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("packet")
    parser.add_argument("--max", type=int, default=12, help="loops to print per tag")
    args = parser.parse_args()

    data = open(args.packet, "rb").read()
    offset = 0
    while offset + 16 <= len(data):
        lo, hi = struct.unpack_from("<QQ", data, offset)
        nloop, eop, pre = lo & 0x7FFF, (lo >> 15) & 1, (lo >> 46) & 1
        prim, flg, nreg = (lo >> 47) & 0x7FF, (lo >> 58) & 3, ((lo >> 60) & 0xF) or 16
        regs = [(hi >> (4 * i)) & 0xF for i in range(nreg)]
        names = ",".join(REGS.get(reg, f"{reg:x}") for reg in regs)
        print(f"@{offset:#06x} tag nloop={nloop} eop={eop} {'prim=(%s) ' % prim_text(prim) if pre else ''}flg={flg} regs={names}")
        offset += 16
        if flg != 0:
            size = ((nloop * nreg + 1) & ~1) * 8 if flg == 1 else nloop * 16
            print(f"         ({size} bytes of {'REGLIST' if flg == 1 else 'IMAGE'} data)")
            offset += size
        else:
            for loop in range(nloop):
                parts = []
                for reg in regs:
                    w = struct.unpack_from("<4I", data, offset)
                    f = struct.unpack_from("<4f", data, offset)
                    if reg == 0xE:
                        value = w[0] | (w[1] << 32)
                        parts.append(ad_text(w[2] & 0xFF, value))
                    elif reg in (4, 0xC):
                        parts.append(f"xyzf=({(w[0] & 0xFFFF) / 16:.2f},{(w[1] & 0xFFFF) / 16:.2f},{(w[2] >> 4) & 0xFFFFFF:#x})"
                                     f"{'*' if (w[3] >> 15) & 1 or reg == 0xC else ''}")
                    elif reg in (5, 0xD):
                        parts.append(f"xyz=({(w[0] & 0xFFFF) / 16:.2f},{(w[1] & 0xFFFF) / 16:.2f},{w[2]:#x})"
                                     f"{'*' if (w[3] >> 15) & 1 or reg == 0xD else ''}")
                    elif reg == 2:
                        parts.append(f"stq=({f[0]:.4g},{f[1]:.4g},{f[2]:.4g})")
                    elif reg == 1:
                        parts.append(f"rgba={w[0] & 0xFF:02x}{w[1] & 0xFF:02x}{w[2] & 0xFF:02x}{w[3] & 0xFF:02x}")
                    else:
                        parts.append(f"{REGS.get(reg, '%x' % reg)}={w[0]:08x} {w[1]:08x} {w[2]:08x} {w[3]:08x}")
                    offset += 16
                if loop < args.max:
                    print("         " + "  ".join(parts))
                elif loop == args.max:
                    print(f"         ... {nloop - args.max} more")
        if eop:
            break
    print(f"{offset} of {len(data)} bytes ('*' = vertex without a drawing kick)")


if __name__ == "__main__":
    main()
