"""Extract the game partition of an *unencrypted* Wii disc image.

Supports images where header bytes 0x60/0x61 (hash/encryption disable) are set,
i.e. partition data is stored raw and contiguous. Encrypted retail dumps are
not supported on purpose: convert them first (e.g. Dolphin "Convert File..."
or wit with --raw), so no console keys are ever needed here.

Usage: python tools/extract_disc.py nsmbwii.iso game/
"""
import os
import struct
import sys


class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")
        hdr = self.read(0, 0x62)
        if hdr[0x18:0x1C] != b"\x5d\x1c\x9e\xa3":
            sys.exit("not a Wii disc image")
        if hdr[0x60] == 0 or hdr[0x61] == 0:
            sys.exit("image is encrypted; convert it to an unencrypted ISO first")
        self.game_id = hdr[:6].decode()
        self.part_data = self._find_game_partition()

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def _find_game_partition(self):
        for group in range(4):
            cnt, off = struct.unpack(">II", self.read(0x40000 + group * 8, 8))
            for i in range(cnt):
                p_off, p_type = struct.unpack(">II", self.read((off << 2) + i * 8, 8))
                if p_type == 0:
                    p_off <<= 2
                    data_off = struct.unpack(">I", self.read(p_off + 0x2B8, 4))[0] << 2
                    return p_off + data_off
        sys.exit("no game partition found")

    def pread(self, off, n):
        return self.read(self.part_data + off, n)


def extract(disc, out):
    os.makedirs(out, exist_ok=True)
    hdr = disc.pread(0, 0x440)
    dol_off, fst_off, fst_size = (x << 2 for x in struct.unpack(">III", hdr[0x420:0x42C]))

    sys_dir = os.path.join(out, "sys")
    os.makedirs(sys_dir, exist_ok=True)
    with open(os.path.join(sys_dir, "boot.bin"), "wb") as w:
        w.write(hdr)
    with open(os.path.join(sys_dir, "bi2.bin"), "wb") as w:
        w.write(disc.pread(0x440, 0x2000))

    # DOL size = max(section offset + size) over the 18 sections
    dh = disc.pread(dol_off, 0x100)
    offs = struct.unpack(">18I", dh[0x00:0x48])
    sizes = struct.unpack(">18I", dh[0x90:0xD8])
    dol_size = max([0x100] + [o + s for o, s in zip(offs, sizes) if o])
    with open(os.path.join(sys_dir, "main.dol"), "wb") as w:
        w.write(disc.pread(dol_off, dol_size))

    fst = disc.pread(fst_off, fst_size)
    with open(os.path.join(sys_dir, "fst.bin"), "wb") as w:
        w.write(fst)

    n_entries = struct.unpack(">I", fst[8:12])[0]
    names = fst[n_entries * 12:]

    def name_at(o):
        return names[o:names.index(b"\0", o)].decode("shift_jis")

    files_dir = os.path.join(out, "files")
    # stack of (end_index, path)
    stack = [(n_entries, files_dir)]
    os.makedirs(files_dir, exist_ok=True)
    count = 0
    for i in range(1, n_entries):
        while i >= stack[-1][0]:
            stack.pop()
        flags_name, a, b = struct.unpack(">III", fst[i * 12:i * 12 + 12])
        is_dir = flags_name >> 24
        path = os.path.join(stack[-1][1], name_at(flags_name & 0xFFFFFF))
        if is_dir:
            os.makedirs(path, exist_ok=True)
            stack.append((b, path))
        else:
            with open(path, "wb") as w:
                off, size = a << 2, b
                while size:
                    chunk = min(size, 1 << 22)
                    w.write(disc.pread(off, chunk))
                    off += chunk
                    size -= chunk
            count += 1
    print(f"{disc.game_id}: main.dol {dol_size:#x} bytes, {count} files -> {out}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    extract(Disc(sys.argv[1]), sys.argv[2])
