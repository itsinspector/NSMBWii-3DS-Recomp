"""Wii executable formats: DOL, REL (with relocation), LZ11 compression."""
import struct
from dataclasses import dataclass, field


def lz11_decompress(data):
    if data[0] != 0x11:
        raise ValueError("not LZ11")
    size = data[1] | data[2] << 8 | data[3] << 16
    pos = 4
    if size == 0:
        size = struct.unpack_from("<I", data, 4)[0]
        pos = 8
    out = bytearray()
    while len(out) < size:
        flags = data[pos]
        pos += 1
        for bit in range(7, -1, -1):
            if len(out) >= size:
                break
            if not flags >> bit & 1:
                out.append(data[pos])
                pos += 1
                continue
            b0 = data[pos]
            ind = b0 >> 4
            if ind == 0:
                b1, b2 = data[pos + 1], data[pos + 2]
                length = ((b0 & 0xF) << 4 | b1 >> 4) + 0x11
                disp = ((b1 & 0xF) << 8 | b2) + 1
                pos += 3
            elif ind == 1:
                b1, b2, b3 = data[pos + 1:pos + 4]
                length = ((b0 & 0xF) << 12 | b1 << 4 | b2 >> 4) + 0x111
                disp = ((b2 & 0xF) << 8 | b3) + 1
                pos += 4
            else:
                b1 = data[pos + 1]
                length = ind + 1
                disp = ((b0 & 0xF) << 8 | b1) + 1
                pos += 2
            for _ in range(length):
                out.append(out[-disp])
    return bytes(out)


@dataclass
class Section:
    name: str
    addr: int
    data: bytes          # empty for bss
    size: int
    exec: bool

    @property
    def end(self):
        return self.addr + self.size

    def contains(self, a):
        return self.addr <= a < self.end


@dataclass
class Dol:
    sections: list
    entry: int

    @classmethod
    def parse(cls, raw):
        offs = struct.unpack_from(">18I", raw, 0x00)
        addrs = struct.unpack_from(">18I", raw, 0x48)
        sizes = struct.unpack_from(">18I", raw, 0x90)
        bss_addr, bss_size, entry = struct.unpack_from(">III", raw, 0xD8)
        secs = []
        for i in range(18):
            if not sizes[i]:
                continue
            is_text = i < 7
            name = f".text{i}" if is_text else f".data{i - 7}"
            secs.append(Section(name, addrs[i], raw[offs[i]:offs[i] + sizes[i]], sizes[i], is_text))
        # bss overlaps small data sections in the DOL header; keep it as a hint only
        secs.append(Section(".bss", bss_addr, b"", bss_size, False))
        return cls(secs, entry)


# REL relocation types
R_PPC_NONE, R_PPC_ADDR32, R_PPC_ADDR24, R_PPC_ADDR16 = 0, 1, 2, 3
R_PPC_ADDR16_LO, R_PPC_ADDR16_HI, R_PPC_ADDR16_HA = 4, 5, 6
R_PPC_ADDR14, R_PPC_ADDR14_BRTAKEN, R_PPC_ADDR14_BRNTAKEN = 7, 8, 9
R_PPC_REL24, R_PPC_REL14, R_PPC_REL14_BRTAKEN, R_PPC_REL14_BRNTAKEN = 10, 11, 12, 13
R_DOLPHIN_NOP, R_DOLPHIN_SECTION, R_DOLPHIN_END, R_DOLPHIN_MRKREF = 201, 202, 203, 204


@dataclass
class Reloc:
    section: int      # section patched in this module
    offset: int       # offset inside that section
    type: int
    module: int       # target module (0 = DOL)
    target_section: int
    addend: int


@dataclass
class Rel:
    name: str
    module_id: int
    version: int
    raw: bytes
    sec_info: list          # (file_offset, size, exec)
    bss_size: int
    prolog: tuple           # (section, offset)
    epilog: tuple
    unresolved: tuple
    align: int
    bss_align: int
    relocs: list = field(default_factory=list)
    sections: list = field(default_factory=list)   # filled by place()

    @classmethod
    def parse(cls, name, raw):
        (mid, _n, _p, nsec, secoff, _no, _ns, ver, bss, reloff, impoff, impsize,
         pro_s, epi_s, unr_s, _bss_s, pro, epi, unr) = struct.unpack_from(">12I4B3I", raw, 0)
        align = bss_align = 32
        if ver >= 2:
            align, bss_align = struct.unpack_from(">II", raw, 0x40)
        sec_info = []
        for i in range(nsec):
            off, size = struct.unpack_from(">II", raw, secoff + i * 8)
            sec_info.append((off & ~3, size, bool(off & 1)))
        rel = cls(name, mid, ver, raw, sec_info, bss, (pro_s, pro), (epi_s, epi),
                  (unr_s, unr), align, bss_align)
        for i in range(impsize // 8):
            tmod, toff = struct.unpack_from(">II", raw, impoff + i * 8)
            rel._parse_relocs(tmod, toff)
        return rel

    def _parse_relocs(self, tmod, pos):
        sec, off = 0, 0
        while True:
            delta, typ, tsec, addend = struct.unpack_from(">HBBI", self.raw, pos)
            pos += 8
            off += delta
            if typ == R_DOLPHIN_END:
                return
            if typ == R_DOLPHIN_SECTION:
                sec, off = tsec, 0
            elif typ in (R_DOLPHIN_NOP, R_PPC_NONE):
                pass
            else:
                self.relocs.append(Reloc(sec, off, typ, tmod, tsec, addend))

    def place(self, base, bss_base=None):
        """Assign virtual addresses to sections. Returns end address."""
        addr = base
        self.sections = []
        for i, (off, size, ex) in enumerate(self.sec_info):
            if off == 0 and size == 0:
                self.sections.append(None)
                continue
            if off == 0:   # bss
                self.sections.append(("bss", i, size))
                continue
            addr = (addr + self.align - 1) & ~(self.align - 1)
            self.sections.append(Section(f"{self.name}.s{i}", addr,
                                         self.raw[off:off + size], size, ex))
            addr += size
        bss_addr = bss_base if bss_base is not None else \
            (addr + self.bss_align - 1) & ~(self.bss_align - 1)
        for i, s in enumerate(self.sections):
            if isinstance(s, tuple):
                self.sections[i] = Section(f"{self.name}.bss", bss_addr, b"", self.bss_size, False)
        if bss_base is None and self.bss_size:
            addr = bss_addr + self.bss_size
        return addr

    def sec_addr(self, idx):
        return self.sections[idx].addr

    def symbol_addr(self, sec_off):
        sec, off = sec_off
        return self.sec_addr(sec) + off if sec else 0


def apply_relocs(modules, branch_targets=None):
    """Patch section data of every REL in `modules` ({id: Rel}).
    Relocations pointing to module 0 use the absolute addend.
    Relative branch relocations are also recorded in `branch_targets`
    ({patched address: target}): our RELs sit too far from the DOL for a
    24-bit displacement, so the lifter takes the target from there."""
    for rel in modules.values():
        data = {i: bytearray(s.data) for i, s in enumerate(rel.sections) if s and s.data}
        for r in rel.relocs:
            if r.module == 0:
                S = r.addend
            elif r.module in modules:
                S = modules[r.module].sec_addr(r.target_section) + r.addend
            else:
                continue
            buf = data[r.section]
            P = rel.sections[r.section].addr + r.offset
            o = r.offset
            word = struct.unpack_from(">I", buf, o)[0] if o + 4 <= len(buf) else 0
            t = r.type
            if t == R_PPC_ADDR32:
                struct.pack_into(">I", buf, o, S & 0xFFFFFFFF)
            elif t == R_PPC_ADDR24:
                struct.pack_into(">I", buf, o, (word & 0xFC000003) | (S & 0x03FFFFFC))
            elif t in (R_PPC_ADDR16, R_PPC_ADDR16_LO):
                struct.pack_into(">H", buf, o, S & 0xFFFF)
            elif t == R_PPC_ADDR16_HI:
                struct.pack_into(">H", buf, o, (S >> 16) & 0xFFFF)
            elif t == R_PPC_ADDR16_HA:
                struct.pack_into(">H", buf, o, ((S + 0x8000) >> 16) & 0xFFFF)
            elif t in (R_PPC_ADDR14, R_PPC_ADDR14_BRTAKEN, R_PPC_ADDR14_BRNTAKEN):
                struct.pack_into(">I", buf, o, (word & 0xFFFF0003) | (S & 0xFFFC))
            elif t == R_PPC_REL24:
                struct.pack_into(">I", buf, o, (word & 0xFC000003) | ((S - P) & 0x03FFFFFC))
                if branch_targets is not None:
                    branch_targets[P] = S & 0xFFFFFFFF
            elif t in (R_PPC_REL14, R_PPC_REL14_BRTAKEN, R_PPC_REL14_BRNTAKEN):
                struct.pack_into(">I", buf, o, (word & 0xFFFF0003) | ((S - P) & 0xFFFC))
                if branch_targets is not None:
                    branch_targets[P] = S & 0xFFFFFFFF
            else:
                raise ValueError(f"{rel.name}: unsupported reloc type {t}")
        for i, b in data.items():
            rel.sections[i].data = bytes(b)
