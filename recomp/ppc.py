"""Gekko/Broadway instruction field helpers and control-flow classification."""


class Insn:
    __slots__ = ("addr", "w", "reloc_target")

    def __init__(self, addr, w, reloc_target=None):
        self.addr = addr
        self.w = w
        self.reloc_target = reloc_target   # branch target from a REL relocation

    opcd = property(lambda s: s.w >> 26)
    rD = rS = property(lambda s: (s.w >> 21) & 31)
    rA = property(lambda s: (s.w >> 16) & 31)
    rB = property(lambda s: (s.w >> 11) & 31)
    rC = property(lambda s: (s.w >> 6) & 31)
    mb = property(lambda s: (s.w >> 6) & 31)
    me = property(lambda s: (s.w >> 1) & 31)
    sh = property(lambda s: (s.w >> 11) & 31)
    uimm = property(lambda s: s.w & 0xFFFF)
    xo10 = property(lambda s: (s.w >> 1) & 0x3FF)
    xo5 = property(lambda s: (s.w >> 1) & 0x1F)
    rc = property(lambda s: s.w & 1)
    lk = property(lambda s: s.w & 1)
    aa = property(lambda s: (s.w >> 1) & 1)
    BO = property(lambda s: (s.w >> 21) & 31)
    BI = property(lambda s: (s.w >> 16) & 31)
    crfD = property(lambda s: (s.w >> 23) & 7)
    crfS = property(lambda s: (s.w >> 18) & 7)

    @property
    def simm(self):
        v = self.w & 0xFFFF
        return v - 0x10000 if v & 0x8000 else v

    @property
    def spr(self):
        v = (self.w >> 11) & 0x3FF
        return (v >> 5) | ((v & 31) << 5)

    @property
    def d(self):
        return self.simm

    @property
    def ps_d(self):   # 12-bit displacement for psq_l/psq_st
        v = self.w & 0xFFF
        return v - 0x1000 if v & 0x800 else v

    @property
    def ps_i(self):
        return (self.w >> 12) & 7

    @property
    def ps_w(self):
        return (self.w >> 15) & 1

    @property
    def ps_ix(self):  # indexed forms
        return (self.w >> 7) & 7

    @property
    def ps_wx(self):
        return (self.w >> 10) & 1

    # ---- branches ----
    @property
    def is_b(self):
        return self.opcd == 18

    @property
    def is_bc(self):
        return self.opcd == 16

    @property
    def is_bclr(self):
        return self.opcd == 19 and self.xo10 == 16

    @property
    def is_bcctr(self):
        return self.opcd == 19 and self.xo10 == 528

    @property
    def target(self):
        if self.reloc_target is not None:
            return self.reloc_target
        if self.is_b:
            li = self.w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
        else:
            li = self.w & 0xFFFC
            if li & 0x8000:
                li -= 0x10000
        return (li if self.aa else self.addr + li) & 0xFFFFFFFF

    @property
    def bc_always(self):
        return (self.BO & 0x14) == 0x14

    @property
    def ends_block(self):
        """No fallthrough to the next instruction."""
        if self.lk:
            return False
        if self.is_b:
            return True
        if self.is_bc or self.is_bclr or self.is_bcctr:
            return self.bc_always
        return self.opcd == 19 and self.xo10 == 50   # rfi


def mask(mb, me):
    if mb <= me:
        m = ((0xFFFFFFFF >> mb) & (0xFFFFFFFF << (31 - me))) & 0xFFFFFFFF
    else:
        m = ((0xFFFFFFFF >> mb) | (0xFFFFFFFF << (31 - me))) & 0xFFFFFFFF
    return m
