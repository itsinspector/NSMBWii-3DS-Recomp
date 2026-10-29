"""Load DOL + RELs into one flat address space and discover functions."""
import bisect
import glob
import os
import struct

from .binfmt import Dol, Rel, apply_relocs, lz11_decompress
from .ppc import Insn

# Where relocated RELs live in guest memory (top of MEM2). The runtime
# reserves this range so the game's heaps never touch it.
REL_BASE = 0x93400000
REL_ORDER = ["d_profileNP", "d_basesNP", "d_enemiesNP", "d_en_bossNP"]


class Program:
    def __init__(self, game_dir):
        dol = Dol.parse(open(os.path.join(game_dir, "sys", "main.dol"), "rb").read())
        self.entry = dol.entry
        self.sections = [s for s in dol.sections if s.name != ".bss"]
        self.dol_bss = next(s for s in dol.sections if s.name == ".bss")
        self.rels = {}
        rel_paths = {os.path.basename(p).split(".")[0]: p
                     for p in glob.glob(os.path.join(game_dir, "files", "rels", "*.rel*"))}
        addr = REL_BASE
        for name in REL_ORDER:
            raw = open(rel_paths[name], "rb").read()
            if rel_paths[name].endswith(".LZ"):
                raw = lz11_decompress(raw)
            rel = Rel.parse(name, raw)
            addr = (rel.place(addr) + 0xFFF) & ~0xFFF
            self.rels[rel.module_id] = rel
        self.rel_end = addr
        self.branch_targets = {}
        apply_relocs(self.rels, self.branch_targets)
        for rel in self.rels.values():
            self.sections += [s for s in rel.sections if s]
        self.sections.sort(key=lambda s: s.addr)
        self.code = [s for s in self.sections if s.exec]
        self.data = [s for s in self.sections if not s.exec and s.data]
        self._starts = [s.addr for s in self.sections]

    def section_at(self, a):
        i = bisect.bisect_right(self._starts, a) - 1
        if i >= 0 and self.sections[i].contains(a):
            return self.sections[i]
        return None

    def is_code(self, a):
        s = self.section_at(a)
        return s is not None and s.exec and not a & 3

    def word(self, a):
        s = self.section_at(a)
        if s is None or not s.data or a + 4 > s.end:
            return None
        return struct.unpack_from(">I", s.data, a - s.addr)[0]

    def insns(self, sec):
        ws = struct.unpack(f">{len(sec.data) // 4}I", sec.data)
        bt = self.branch_targets
        return [Insn(sec.addr + i * 4, w, bt.get(sec.addr + i * 4)) for i, w in enumerate(ws)]

    def stream_at(self, addr):
        """(instruction list of the whole code section, index of addr)."""
        sec = self.section_at(addr)
        cache = self.__dict__.setdefault("_streams", {})
        if sec.addr not in cache:
            cache[sec.addr] = self.insns(sec)
        return cache[sec.addr], (addr - sec.addr) // 4

    def special_entries(self):
        out = {self.entry}
        for rel in self.rels.values():
            for sym in (rel.prolog, rel.epilog, rel.unresolved):
                if sym[0]:
                    out.add(rel.symbol_addr(sym))
        return out


class Function:
    __slots__ = ("start", "end", "insns", "labels", "jump_tables")

    def __init__(self, start, end, insns):
        self.start, self.end, self.insns = start, end, insns
        self.labels = set()
        self.jump_tables = {}   # bctr addr -> [targets]

    def contains(self, a):
        return self.start <= a < self.end


# opcodes whose rD field (bits 6-10) is a GPR destination
_WRITES_RD = {7, 8, 12, 13, 14, 15, 32, 33, 34, 35, 40, 41, 42, 43}
# D-form ops writing rA: rotates/logical immediates, and update-form loads/stores
_WRITES_RA = {20, 21, 23, 24, 25, 26, 27, 28, 29, 33, 35, 37, 39, 41, 43, 45, 49, 51, 53, 55, 57, 61}
# opcode 31: X-form ops whose destination is rA (logical/shift) ...
_X_DEST_RA = {28, 60, 124, 284, 316, 412, 444, 476, 24, 536, 792, 824, 26, 922, 954}
# ... update-form indexed loads/stores (also write rA)
_X_UPDATE = {55, 119, 311, 375, 183, 247, 439, 567, 631, 695, 759}
# ... stores and other ops without a GPR destination
_X_NO_DEST = {151, 215, 407, 662, 918, 983, 663, 727, 150, 0, 32, 4, 54, 86, 246, 278, 470,
              982, 598, 854, 306, 566, 1014, 467, 144, 146, 210, 242, 725, 661}


def gpr_writes(i):
    """Set of GPRs an instruction writes (calls clobber the volatile ones)."""
    op = i.opcd
    if op in _WRITES_RD:
        out = {i.rD}
        if op in (33, 35, 41, 43):
            out.add(i.rA)
        return out
    if op in _WRITES_RA:
        return {i.rA}
    if op == 46:                               # lmw
        return set(range(i.rD, 32))
    if op == 31:
        xo = i.xo10
        if xo in _X_DEST_RA:
            return {i.rA}
        if xo in _X_UPDATE:
            return {i.rA} | ({i.rD} if xo in (55, 119, 311, 375) else set())
        if xo in _X_NO_DEST:
            return set()
        if xo == 597:                          # lswi
            return set(range(32))
        return {i.rD}
    if i.lk and (i.is_b or i.is_bc or i.is_bcctr or i.is_bclr):
        return {0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}
    return set()


def resolve_const(insns, idx, reg, depth=0, stop_at_return=False):
    """Value of GPR `reg` just before insns[idx], if it is a lis/addi/ori constant.
    Walks backwards linearly, so it is only a heuristic across branches.
    With stop_at_return, `insns` may span several functions: an unconditional
    blr ends the search."""
    if depth > 4:
        return None
    for j in range(idx - 1, -1, -1):
        i = insns[j]
        op = i.opcd
        if stop_at_return and i.is_bclr and i.bc_always and not i.lk:
            return None
        if op in (14, 15) and i.rD == reg:
            base = 0 if i.rA == 0 else resolve_const(insns, j, i.rA, depth + 1, stop_at_return)
            if base is None:
                return None
            return (base + (i.simm << 16 if op == 15 else i.simm)) & 0xFFFFFFFF
        if op in (24, 25) and i.rA == reg:
            base = resolve_const(insns, j, i.rS, depth + 1, stop_at_return)
            if base is None:
                return None
            return base | (i.uimm << 16 if op == 25 else i.uimm)
        if op == 31 and i.xo10 == 444 and i.rA == reg and i.rS == i.rB:   # mr
            return resolve_const(insns, j, i.rS, depth + 1, stop_at_return)
        if reg in gpr_writes(i):
            return None
    return None


def find_jump_table(prog, fn, idx):
    """Given the index of a bctr in fn.insns, recover its jump table targets.
    Matches the CodeWarrior switch idiom:
        cmplwi rX,N ; bgt ; lis rT,tbl@ha ; ... addi rT,rT,tbl@l ; lwzx rY,rT,rI ; mtctr rY ; bctr
    """
    insns = fn.insns
    ctr_reg = table_reg = None
    consts = {}
    bound = None
    lo = max(0, idx - 16)
    for j in range(idx - 1, lo - 1, -1):
        i = insns[j]
        op, xo = i.opcd, i.xo10
        if ctr_reg is None and op == 31 and xo == 467 and i.spr == 9:     # mtctr
            ctr_reg = i.rS
        elif ctr_reg is not None and table_reg is None and op == 31 and xo == 23 and i.rD == ctr_reg:  # lwzx
            table_reg = (i.rA, i.rB)
        elif op == 10 and bound is None and table_reg is not None:          # cmplwi
            bound = i.uimm
    if table_reg is None:
        return None
    lwzx_idx = next(j for j in range(idx - 1, lo - 1, -1)
                    if insns[j].opcd == 31 and insns[j].xo10 == 23)
    base = (resolve_const(insns, lwzx_idx, table_reg[0])
            or resolve_const(insns, lwzx_idx, table_reg[1]))
    if base is None or prog.section_at(base) is None:
        return None
    targets = []
    n = bound + 1 if bound is not None else 1 << 16
    for k in range(n):
        t = prog.word(base + 4 * k)
        if t is None or not fn.contains(t):
            break
        targets.append(t)
    return targets or None


def discover(prog, extra_starts=()):
    code_insns = {}
    for sec in prog.code:
        for i in prog.insns(sec):
            code_insns[i.addr] = i
    starts = set(prog.special_entries()) | set(extra_starts)
    starts |= {s.addr for s in prog.code}

    # direct call targets and code-address constants built with lis/addi.
    # lis values are tracked linearly; volatile registers are forgotten at
    # calls and everything at returns, approximating function boundaries.
    regs = {}
    for a in sorted(code_insns):
        i = code_insns[a]
        if i.is_b or i.is_bc:
            if i.lk and prog.is_code(i.target) and i.target != a + 4:
                starts.add(i.target)
        if i.opcd == 15 and i.rA == 0:
            regs[i.rD] = i.simm << 16 & 0xFFFFFFFF
            continue
        if i.opcd in (14, 24) and (i.rA if i.opcd == 14 else i.rS) in regs and (i.rA if i.opcd == 14 else i.rS):
            src = i.rA if i.opcd == 14 else i.rS
            hi = regs[src]
            v = (hi + i.simm) & 0xFFFFFFFF if i.opcd == 14 else hi | i.uimm
            if prog.is_code(v):
                starts.add(v)
        if i.is_bclr or i.is_b and not i.lk:
            regs.clear()
        else:
            for r in gpr_writes(i):
                regs.pop(r, None)
    # code right after a terminator that no branch targets can only be entered
    # through a pointer: a function start (checked against jump tables below)
    branch_targets = {i.target for i in code_insns.values() if i.is_b or i.is_bc}
    after_term = set()
    for a, i in code_insns.items():
        if not i.ends_block:
            continue
        # skip alignment padding (zero words) between functions
        b = a + 4
        while b in code_insns and code_insns[b].w == 0 and b - a <= 32:
            b += 4
        n = code_insns.get(b)
        if n is None or b in branch_targets:
            continue
        # only with a real prologue: stwu r1,-N(r1) or mflr r0
        if (n.w & 0xFFFF0000) == 0x94210000 or n.w == 0x7C0802A6:
            after_term.add(b)

    # pointers stored in data (vtables, callbacks, jump tables)
    data_ptrs = set()
    for sec in prog.data:
        ws = struct.unpack(f">{len(sec.data) // 4}I", sec.data[:len(sec.data) & ~3])
        data_ptrs.update(w for w in ws if prog.is_code(w))

    for _ in range(8):
        funcs = _partition(prog, code_insns, starts)
        jt_targets = set()
        for fn in funcs:
            for idx, i in enumerate(fn.insns):
                if i.is_bcctr and not i.lk:
                    t = find_jump_table(prog, fn, idx)
                    if t:
                        fn.jump_tables[i.addr] = t
                        jt_targets.update(t)
        new = set()
        for p in data_ptrs - jt_targets:
            new.add(p)
        # switch case labels are data pointers (jump table entries): never split there
        for p in after_term - data_ptrs:
            new.add(p)
        for fn in funcs:
            for i in fn.insns:
                if (i.is_b or i.is_bc) and not i.lk:
                    t = i.target
                    if not fn.contains(t) and prog.is_code(t):
                        new.add(t)
        if new <= starts:
            break
        starts |= new
    for fn in funcs:
        _labels(fn)
    return funcs


def _partition(prog, code_insns, starts):
    funcs = []
    for sec in prog.code:
        ss = sorted(s for s in starts if sec.contains(s))
        for k, s in enumerate(ss):
            e = ss[k + 1] if k + 1 < len(ss) else sec.end
            funcs.append(Function(s, e, [code_insns[a] for a in range(s, e, 4)]))
    return funcs


def _labels(fn):
    for i in fn.insns:
        if (i.is_b or i.is_bc) and not i.lk and fn.contains(i.target):
            fn.labels.add(i.target)
    for t in fn.jump_tables.values():
        fn.labels.update(t)
