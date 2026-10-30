"""PowerPC (Gekko/Broadway) -> C lifter.

Every guest function becomes `void f_XXXXXXXX(PPCContext *c)`. Guest state lives
in the context struct; memory goes through the big-endian accessors defined in
runtime/include/ppc_context.h.
"""
from .analyze import resolve_const
from .ppc import mask

CRB = ("lt", "gt", "eq", "so")


def R(n):
    return f"c->r[{n}]"


def RA0(n):
    return "0" if n == 0 else f"c->r[{n}]"


def F(n):
    return f"c->f[{n}].d"


def FU(n):
    return f"c->f[{n}].u"


def P1(n):
    return f"c->ps1[{n}]"


def crbit(n):
    return f"c->cr[{n >> 2}].{CRB[n & 3]}"


def h32(v):
    return f"0x{v & 0xFFFFFFFF:08X}u"


class Unhandled(Exception):
    pass


# Revolution SDK libraries (AI..IPC/NAND). Their drivers reach hardware through
# pointer variables (__piReg, __cpReg, ...) or base + chan*stride, so every
# access there is checked at runtime.
SDK_RANGE = (0x8019F000, 0x80230000)


class FunctionLifter:
    def __init__(self, prog, fn, func_starts, overrides, returns_twice=()):
        self.prog = prog
        self.fn = fn
        self.starts = func_starts
        self.overrides = overrides
        self.returns_twice = returns_twice
        self.out = []
        self.hw_func = (SDK_RANGE[0] <= fn.start < SDK_RANGE[1] or
                        any(i.opcd == 15 and ((i.simm << 16) & 0xFE000000) == 0xCC000000
                            for i in fn.insns))

    # ------------------------------------------------------------------ utils
    def emit(self, s):
        self.out.append("\t" + s)

    def rc0(self, i, reg_expr):
        if i.rc:
            self.emit(f"ppc_set_cr0(c, {reg_expr});")

    def ea(self, i, update=False):
        if update:
            return f"(c->r[{i.rA}] + {i.d})"
        return f"({RA0(i.rA)} + {i.d})" if i.rA else h32(i.d)

    def eax(self, i):
        return f"({RA0(i.rA)} + c->r[{i.rB}])"

    def mmio_base(self, i):
        """True if the base register is a constant pointing at hardware regs
        (e.g. GX inline writes to WGPIPE at 0xCC008000)."""
        if i.rA == 0:
            return False
        # walk the section, not just this function: a split function keeps
        # its `lis rX,0xCC01` in the previous piece
        stream, idx = self.prog.stream_at(i.addr)
        v = resolve_const(stream, idx, i.rA, stop_at_return=True)
        return v is not None and 0xCC000000 <= v < 0xCE000000

    def call(self, target, ret_addr):
        self.emit(f"c->lr = {h32(ret_addr)};")
        if target in self.returns_twice:
            # OSSaveContext: the setjmp must live in this (caller's) frame so a
            # later OSLoadContext can resume here with r3 = 1.
            self.emit("if (setjmp(*rt_save_context(c))) {}")
        elif target in self.starts:
            self.emit(f"f_{target:08X}(c);")
        else:
            self.emit(f"ppc_call_indirect(c, {h32(target)});")

    def is_spin_loop(self, i):
        """Backward branch over a body without stores or calls: a busy-wait on
        memory that only an interrupt can change (e.g. the scheduler idle loop)."""
        t = i.target
        if not (self.fn.start <= t <= i.addr):
            return False
        if (i.addr - t) // 4 > 24:
            return False
        for a in range(t, i.addr, 4):
            j = self.fn.insns[(a - self.fn.start) // 4]
            op = j.opcd
            if op in (36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55, 60, 61):
                return False
            if op == 31 and j.xo10 in (151, 183, 215, 247, 407, 439, 662, 663, 695, 727, 759, 918, 983, 150):
                return False
            if (j.is_b or j.is_bc or j.is_bclr or j.is_bcctr) and (j.lk or (j.is_b and not self.fn.contains(j.target))):
                return False
        return True

    def tail(self, target, cond=None):
        if self.fn.contains(target) and target in self.fn.labels:
            jump = f"goto L_{target:08X};"
        elif target in self.starts:
            jump = f"{{ f_{target:08X}(c); return; }}"
        else:
            jump = f"{{ ppc_call_indirect(c, {h32(target)}); return; }}"
        self.emit(jump if cond is None else f"if ({cond}) {jump}")

    def bcond(self, i):
        """C expression for the BO/BI condition (decrements CTR if needed)."""
        bo, bi = i.BO, i.BI
        parts = []
        if not bo & 0x04:
            self.emit("c->ctr--;")
            parts.append("c->ctr == 0" if bo & 0x02 else "c->ctr != 0")
        if not bo & 0x10:
            parts.append(crbit(bi) if bo & 0x08 else f"!{crbit(bi)}")
        return " && ".join(parts) if parts else None

    # --------------------------------------------------------------- lifting
    def lift(self):
        fn = self.fn
        name = f"f_{fn.start:08X}"
        hdr = f"void {name}(PPCContext *c) {{"
        if fn.start in self.overrides:
            return [hdr, f"\tRT_TRACE({h32(fn.start)});", f"\t{self.overrides[fn.start]}(c);", "}", ""]
        self.emit(f"RT_TRACE({h32(fn.start)});")
        # trim trailing padding after the last block-ending instruction
        insns = fn.insns
        last = len(insns)
        while last > 0 and insns[last - 1].w == 0:
            last -= 1
        for i in insns[:last]:
            if i.addr in fn.labels:
                self.out.append(f"L_{i.addr:08X}:;")
            try:
                self.insn(i)
            except Unhandled:
                self.emit(f"ppc_unimplemented(c, {h32(i.addr)}, {h32(i.w)});")
        if last == 0 or not insns[last - 1].ends_block:
            if fn.end in self.starts:
                self.emit(f"f_{fn.end:08X}(c);")
            else:
                self.emit(f"ppc_fell_off(c, {h32(fn.end)});")
        return [hdr] + self.out + ["}", ""]

    def insn(self, i):
        op = i.opcd
        self.emit(f"/* {i.addr:08X} {i.w:08X} */")
        h = getattr(self, f"op{op}", None)
        if h is None:
            raise Unhandled
        h(i)

    # ---- branches
    def op18(self, i):          # b / bl
        if i.lk:
            if i.target == i.addr + 4:
                self.emit(f"c->lr = {h32(i.addr + 4)};")
            else:
                self.call(i.target, i.addr + 4)
        else:
            if self.is_spin_loop(i):
                self.emit("rt_poll(c);")
            self.tail(i.target)

    def op16(self, i):          # bc
        if not i.lk and self.is_spin_loop(i):
            self.emit("rt_poll(c);")
        cond = self.bcond(i)
        if i.lk:
            self.emit(f"if ({cond or '1'}) {{")
            self.call(i.target, i.addr + 4)
            self.emit("}")
        else:
            self.tail(i.target, cond)

    def op19(self, i):
        xo = i.xo10
        if xo == 16:            # bclr
            cond = self.bcond(i)
            if i.lk:
                self.emit(f"if ({cond or '1'}) {{ uint32_t t = c->lr; c->lr = {h32(i.addr + 4)}; ppc_call_indirect(c, t); }}")
            else:
                self.emit("return;" if cond is None else f"if ({cond}) return;")
        elif xo == 528:         # bcctr
            cond = None if i.BO & 0x10 else self.bcond(i)
            if i.lk:
                self.emit(f"if ({cond or '1'}) {{ c->lr = {h32(i.addr + 4)}; ppc_call_indirect(c, c->ctr); }}")
                return
            jt = self.fn.jump_tables.get(i.addr)
            body = []
            if jt:
                body.append("switch (c->ctr) {")
                for t in sorted(set(jt)):
                    body.append(f"case {h32(t)}: goto L_{t:08X};")
                body.append("}")
            body.append("ppc_call_indirect(c, c->ctr); return;")
            if cond:
                self.emit(f"if ({cond}) {{")
            for b in body:
                self.emit(b)
            if cond:
                self.emit("}")
        elif xo == 0:           # mcrf
            self.emit(f"c->cr[{i.crfD}] = c->cr[{i.crfS}];")
        elif xo in (33, 129, 193, 225, 257, 289, 417, 449):
            a, b, d = crbit(i.rA), crbit(i.rB), crbit(i.rD)
            expr = {33: f"!({a} | {b})", 129: f"{a} & !{b}", 193: f"{a} ^ {b}",
                    225: f"!({a} & {b})", 257: f"{a} & {b}", 289: f"!({a} ^ {b})",
                    417: f"{a} | !{b}", 449: f"{a} | {b}"}[xo]
            self.emit(f"{d} = {expr};")
        elif xo == 150:         # isync
            pass
        elif xo == 50:          # rfi
            self.emit("ppc_rfi(c); return;")
        else:
            raise Unhandled

    def op17(self, i):
        self.emit(f"ppc_syscall(c, {h32(i.addr)});")

    def op3(self, i):           # twi
        self.emit(f"ppc_trap(c, {h32(i.addr)});")

    # ---- integer immediate
    def op7(self, i):
        self.emit(f"{R(i.rD)} = (uint32_t)((int32_t){R(i.rA)} * {i.simm});")

    def op8(self, i):
        self.emit(f"{{ uint32_t a = {R(i.rA)}; {R(i.rD)} = {h32(i.simm)} - a; "
                  f"c->ca = ((uint64_t)(uint32_t)~a + {h32(i.simm)} + 1) >> 32; }}")

    def op10(self, i):
        self.emit(f"ppc_cmpl(c, {i.crfD}, {R(i.rA)}, {h32(i.uimm)});")

    def op11(self, i):
        self.emit(f"ppc_cmp(c, {i.crfD}, (int32_t){R(i.rA)}, {i.simm});")

    def op12(self, i, rec=False):
        self.emit(f"{{ uint32_t a = {R(i.rA)}; {R(i.rD)} = a + {h32(i.simm)}; c->ca = {R(i.rD)} < a; }}")
        if rec:
            self.emit(f"ppc_set_cr0(c, {R(i.rD)});")

    def op13(self, i):
        self.op12(i, True)

    def op14(self, i):
        self.emit(f"{R(i.rD)} = {RA0(i.rA)} + {h32(i.simm)};" if i.rA else f"{R(i.rD)} = {h32(i.simm)};")

    def op15(self, i):
        v = h32(i.simm << 16)
        self.emit(f"{R(i.rD)} = {RA0(i.rA)} + {v};" if i.rA else f"{R(i.rD)} = {v};")

    def op20(self, i):
        m = mask(i.mb, i.me)
        self.emit(f"{R(i.rA)} = (ROTL32({R(i.rS)}, {i.sh}) & {h32(m)}) | ({R(i.rA)} & {h32(~m)});")
        self.rc0(i, R(i.rA))

    def op21(self, i):
        m = mask(i.mb, i.me)
        self.emit(f"{R(i.rA)} = ROTL32({R(i.rS)}, {i.sh}) & {h32(m)};")
        self.rc0(i, R(i.rA))

    def op23(self, i):
        m = mask(i.mb, i.me)
        self.emit(f"{R(i.rA)} = ROTL32({R(i.rS)}, {R(i.rB)}) & {h32(m)};")
        self.rc0(i, R(i.rA))

    def op24(self, i):
        if i.w == 0x60000000:
            return              # nop
        self.emit(f"{R(i.rA)} = {R(i.rS)} | {h32(i.uimm)};")

    def op25(self, i):
        self.emit(f"{R(i.rA)} = {R(i.rS)} | {h32(i.uimm << 16)};")

    def op26(self, i):
        self.emit(f"{R(i.rA)} = {R(i.rS)} ^ {h32(i.uimm)};")

    def op27(self, i):
        self.emit(f"{R(i.rA)} = {R(i.rS)} ^ {h32(i.uimm << 16)};")

    def op28(self, i):
        self.emit(f"{R(i.rA)} = {R(i.rS)} & {h32(i.uimm)};")
        self.emit(f"ppc_set_cr0(c, {R(i.rA)});")

    def op29(self, i):
        self.emit(f"{R(i.rA)} = {R(i.rS)} & {h32(i.uimm << 16)};")
        self.emit(f"ppc_set_cr0(c, {R(i.rA)});")

    # ---- loads / stores (D-form)
    def acc(self, fn, i=None):
        """Accessor name for a memory op: static MMIO, checked, or plain.
        Stack (r1) and small-data (r2, r13) bases never point at hardware."""
        if i is not None and i.rA in (1, 2, 13):
            return fn
        if i is not None and self.mmio_base(i):
            return "mmio_" + fn
        if self.hw_func:
            return "chk_" + fn
        return fn

    def _load(self, i, fn, sign_cast="", update=False):
        ea = self.ea(i, update)
        fn = self.acc(fn, i)
        if update:
            self.emit(f"{{ uint32_t ea = {ea}; {R(i.rD)} = {sign_cast}{fn}(ea); c->r[{i.rA}] = ea; }}")
        else:
            self.emit(f"{R(i.rD)} = {sign_cast}{fn}({ea});")

    def _store(self, i, fn, val, update=False, ea=None):
        ea = ea or self.ea(i, update)
        fn = self.acc(fn, i)
        if update:
            self.emit(f"{{ uint32_t ea = {ea}; {fn}(ea, {val}); c->r[{i.rA}] = ea; }}")
        else:
            self.emit(f"{fn}({ea}, {val});")

    def op32(self, i): self._load(i, "ld32")
    def op33(self, i): self._load(i, "ld32", update=True)
    def op34(self, i): self._load(i, "ld8")
    def op35(self, i): self._load(i, "ld8", update=True)
    def op40(self, i): self._load(i, "ld16")
    def op41(self, i): self._load(i, "ld16", update=True)
    def op42(self, i): self._load(i, "ld16", "(uint32_t)(int16_t)")
    def op43(self, i): self._load(i, "ld16", "(uint32_t)(int16_t)", True)
    def op36(self, i): self._store(i, "st32", R(i.rS))
    def op37(self, i): self._store(i, "st32", R(i.rS), True)
    def op38(self, i): self._store(i, "st8", f"(uint8_t){R(i.rS)}")
    def op39(self, i): self._store(i, "st8", f"(uint8_t){R(i.rS)}", True)
    def op44(self, i): self._store(i, "st16", f"(uint16_t){R(i.rS)}")
    def op45(self, i): self._store(i, "st16", f"(uint16_t){R(i.rS)}", True)

    def op46(self, i):          # lmw
        self.emit(f"{{ uint32_t ea = {self.ea(i)}; for (int k = {i.rD}; k < 32; k++, ea += 4) c->r[k] = ld32(ea); }}")

    def op47(self, i):          # stmw
        self.emit(f"{{ uint32_t ea = {self.ea(i)}; for (int k = {i.rS}; k < 32; k++, ea += 4) st32(ea, c->r[k]); }}")

    def _lfs(self, i, ea, update):
        self.emit(f"{{ uint32_t ea = {ea}; {F(i.rD)} = {P1(i.rD)} = ldf32(ea);"
                  + (f" c->r[{i.rA}] = ea;" if update else "") + " }")

    def _lfd(self, i, ea, update):
        self.emit(f"{{ uint32_t ea = {ea}; {FU(i.rD)} = ld64(ea);"
                  + (f" c->r[{i.rA}] = ea;" if update else "") + " }")

    def op48(self, i): self._lfs(i, self.ea(i), False)
    def op49(self, i): self._lfs(i, self.ea(i, True), True)
    def op50(self, i): self._lfd(i, self.ea(i), False)
    def op51(self, i): self._lfd(i, self.ea(i, True), True)
    def op52(self, i): self._store(i, "stf32", F(i.rS))
    def op53(self, i): self._store(i, "stf32", F(i.rS), True)
    def op54(self, i): self._store(i, "st64", FU(i.rS))
    def op55(self, i): self._store(i, "st64", FU(i.rS), True)

    # ---- paired-single quantized load/store
    def _psq(self, store, ea, w, gqr, frn, update_ra=None):
        """Inline fast path for float (unquantized) GQR types 0-3, which the
        SDK's matrix and vector code uses; the runtime handles the rest."""
        upd = f" c->r[{update_ra}] = ea;" if update_ra is not None else ""
        if store:
            # checked: GX matrix uploads psq_st into the write-gather pipe
            second = "" if w else f" chk_stf32(ea + 4, {P1(frn)});"
            fast = f"chk_stf32(ea, {F(frn)});{second}"
            cond = f"!(c->gqr[{gqr}] & 4u)"
            slow = f"ppc_psq_st(c, ea, {w}, {gqr}, {frn});"
        else:
            fast = f"{F(frn)} = ldf32(ea); {P1(frn)} = {'1.0' if w else 'ldf32(ea + 4)'};"
            cond = f"!(c->gqr[{gqr}] & 0x40000u)"
            slow = f"ppc_psq_l(c, ea, {w}, {gqr}, {frn});"
        self.emit(f"{{ uint32_t ea = {ea}; if ({cond}) {{ {fast} }} else {slow}{upd} }}")

    def op56(self, i):
        self._psq(False, f"({RA0(i.rA)} + {i.ps_d})", i.ps_w, i.ps_i, i.rD)

    def op57(self, i):
        self._psq(False, f"(c->r[{i.rA}] + {i.ps_d})", i.ps_w, i.ps_i, i.rD, i.rA)

    def op60(self, i):
        self._psq(True, f"({RA0(i.rA)} + {i.ps_d})", i.ps_w, i.ps_i, i.rS)

    def op61(self, i):
        self._psq(True, f"(c->r[{i.rA}] + {i.ps_d})", i.ps_w, i.ps_i, i.rS, i.rA)

    # ---- opcode 31
    def op31(self, i):
        xo = i.xo10
        d, a, b, s = i.rD, i.rA, i.rB, i.rS
        arith = xo & 0x1FF   # strip OE
        simple = {
            28: f"{R(a)} = {R(s)} & {R(b)};",
            60: f"{R(a)} = {R(s)} & ~{R(b)};",
            124: f"{R(a)} = ~({R(s)} | {R(b)});",
            284: f"{R(a)} = ~({R(s)} ^ {R(b)});",
            316: f"{R(a)} = {R(s)} ^ {R(b)};",
            412: f"{R(a)} = {R(s)} | ~{R(b)};",
            444: f"{R(a)} = {R(s)} | {R(b)};",
            476: f"{R(a)} = ~({R(s)} & {R(b)});",
            24: f"{R(a)} = ({R(b)} & 0x20) ? 0 : {R(s)} << ({R(b)} & 31);",
            536: f"{R(a)} = ({R(b)} & 0x20) ? 0 : {R(s)} >> ({R(b)} & 31);",
            26: f"{R(a)} = ppc_clz({R(s)});",
            922: f"{R(a)} = (uint32_t)(int16_t){R(s)};",
            954: f"{R(a)} = (uint32_t)(int8_t){R(s)};",
            792: f"{R(a)} = ppc_sraw(c, {R(s)}, {R(b)});",
            824: f"{R(a)} = ppc_sraw(c, {R(s)}, {i.sh});",
        }
        arith_ops = {
            266: f"{R(d)} = {R(a)} + {R(b)};",
            40: f"{R(d)} = {R(b)} - {R(a)};",
            104: f"{R(d)} = -{R(a)};",
            235: f"{R(d)} = (uint32_t)((int32_t){R(a)} * (int32_t){R(b)});",
            75: f"{R(d)} = (uint32_t)(((int64_t)(int32_t){R(a)} * (int32_t){R(b)}) >> 32);",
            11: f"{R(d)} = (uint32_t)(((uint64_t){R(a)} * {R(b)}) >> 32);",
            491: f"{R(d)} = ppc_divw({R(a)}, {R(b)});",
            459: f"{R(d)} = {R(b)} ? {R(a)} / {R(b)} : 0;",
            10: f"{{ uint64_t t = (uint64_t){R(a)} + {R(b)}; {R(d)} = (uint32_t)t; c->ca = t >> 32; }}",
            8: f"{{ uint64_t t = (uint64_t)(uint32_t)~{R(a)} + {R(b)} + 1; {R(d)} = (uint32_t)t; c->ca = t >> 32; }}",
            138: f"{{ uint64_t t = (uint64_t){R(a)} + {R(b)} + c->ca; {R(d)} = (uint32_t)t; c->ca = t >> 32; }}",
            136: f"{{ uint64_t t = (uint64_t)(uint32_t)~{R(a)} + {R(b)} + c->ca; {R(d)} = (uint32_t)t; c->ca = t >> 32; }}",
            202: f"{{ uint64_t t = (uint64_t){R(a)} + c->ca; {R(d)} = (uint32_t)t; c->ca = t >> 32; }}",
            200: f"{{ uint64_t t = (uint64_t)(uint32_t)~{R(a)} + c->ca; {R(d)} = (uint32_t)t; c->ca = t >> 32; }}",
            234: f"{{ uint64_t t = (uint64_t){R(a)} + c->ca + 0xFFFFFFFFu; {R(d)} = (uint32_t)t; c->ca = t >> 32; }}",
            232: f"{{ uint64_t t = (uint64_t)(uint32_t)~{R(a)} + c->ca + 0xFFFFFFFFu; {R(d)} = (uint32_t)t; c->ca = t >> 32; }}",
        }
        if xo in simple:
            self.emit(simple[xo])
            self.rc0(i, R(a))
            return
        if arith in arith_ops and (arith == xo or arith not in (11, 75)):
            self.emit(arith_ops[arith])
            self.rc0(i, R(d))
            return
        X = self.eax(i)
        loads = {23: ("ld32", ""), 87: ("ld8", ""), 279: ("ld16", ""),
                 343: ("ld16", "(uint32_t)(int16_t)"), 534: ("ld32_le", ""),
                 790: ("ld16_le", ""), 20: ("ld32", "")}
        loads_u = {55: ("ld32", ""), 119: ("ld8", ""), 311: ("ld16", ""),
                   375: ("ld16", "(uint32_t)(int16_t)")}
        stores = {151: ("st32", R(s)), 215: ("st8", f"(uint8_t){R(s)}"),
                  407: ("st16", f"(uint16_t){R(s)}"), 662: ("st32_le", R(s)),
                  918: ("st16_le", f"(uint16_t){R(s)}"), 983: ("st32", f"(uint32_t){FU(s)}"),
                  663: ("stf32", F(s)), 727: ("st64", FU(s))}
        stores_u = {183: ("st32", R(s)), 247: ("st8", f"(uint8_t){R(s)}"),
                    439: ("st16", f"(uint16_t){R(s)}"), 695: ("stf32", F(s)),
                    759: ("st64", FU(s))}
        if xo in loads:
            fn, cast = loads[xo]
            self.emit(f"{R(d)} = {cast}{self.acc(fn)}({X});")
        elif xo in loads_u:
            fn, cast = loads_u[xo]
            self.emit(f"{{ uint32_t ea = {X}; {R(d)} = {cast}{self.acc(fn)}(ea); c->r[{a}] = ea; }}")
        elif xo in stores:
            fn, val = stores[xo]
            self.emit(f"{self.acc(fn)}({X}, {val});")
        elif xo == 150:         # stwcx.
            self.emit(f"st32({X}, {R(s)}); c->cr[0].lt = c->cr[0].gt = 0; c->cr[0].eq = 1; c->cr[0].so = c->so;")
        elif xo in stores_u:
            fn, val = stores_u[xo]
            self.emit(f"{{ uint32_t ea = {X}; {self.acc(fn)}(ea, {val}); c->r[{a}] = ea; }}")
        elif xo == 535:
            self._lfs(i, X, False)
        elif xo == 567:
            self._lfs(i, X, True)
        elif xo == 599:
            self._lfd(i, X, False)
        elif xo == 631:
            self._lfd(i, X, True)
        elif xo == 0:
            self.emit(f"ppc_cmp(c, {i.crfD}, (int32_t){R(a)}, (int32_t){R(b)});")
        elif xo == 32:
            self.emit(f"ppc_cmpl(c, {i.crfD}, {R(a)}, {R(b)});")
        elif xo == 339:
            self.emit(self.mfspr(d, i.spr))
        elif xo == 467:
            self.emit(self.mtspr(s, i.spr))
        elif xo == 371:
            self.emit(f"{R(d)} = ppc_mftb(c, {i.spr});")
        elif xo == 19:
            self.emit(f"{R(d)} = ppc_get_cr(c);")
        elif xo == 144:
            self.emit(f"ppc_set_cr(c, {R(s)}, 0x{(i.w >> 12) & 0xFF:02X});")
        elif xo == 83:
            self.emit(f"{R(d)} = c->msr;")
        elif xo == 146:
            self.emit(f"ppc_mtmsr(c, {R(s)});")
        elif xo == 512:
            self.emit(f"c->cr[{i.crfD}].lt = c->so; c->cr[{i.crfD}].gt = c->ov; "
                      f"c->cr[{i.crfD}].eq = c->ca; c->cr[{i.crfD}].so = 0; c->so = c->ov = c->ca = 0;")
        elif xo == 1014:
            self.emit(f"ppc_dcbz({X});")
        elif xo in (54, 86, 246, 278, 470, 982, 598, 854, 306, 566):
            pass                # cache / sync / tlb: no-op
        elif xo == 4:
            self.emit(f"ppc_trap(c, {h32(i.addr)});")
        elif xo == 597:
            self.emit(f"ppc_lswi(c, {RA0(a)}, {d}, {b or 32});")
        elif xo == 725:
            self.emit(f"ppc_stswi(c, {RA0(a)}, {s}, {b or 32});")
        elif xo in (210, 242, 595, 659):
            pass                # segment registers: unused on Wii
        else:
            raise Unhandled

    def mfspr(self, d, spr):
        if spr == 8:
            return f"{R(d)} = c->lr;"
        if spr == 9:
            return f"{R(d)} = c->ctr;"
        if spr == 1:
            return f"{R(d)} = ppc_get_xer(c);"
        if 912 <= spr <= 919:
            return f"{R(d)} = c->gqr[{spr - 912}];"
        return f"{R(d)} = ppc_mfspr(c, {spr});"

    def mtspr(self, s, spr):
        if spr == 8:
            return f"c->lr = {R(s)};"
        if spr == 9:
            return f"c->ctr = {R(s)};"
        if spr == 1:
            return f"ppc_set_xer(c, {R(s)});"
        if 912 <= spr <= 919:
            return f"c->gqr[{spr - 912}] = {R(s)};"
        return f"ppc_mtspr(c, {spr}, {R(s)});"

    # ---- floating point
    def _fr(self, i, expr, single, ps1_from_ps0=None):
        d = i.rD
        if single:
            self.emit(f"{F(d)} = {P1(d)} = (double)(float)({expr});")
        else:
            self.emit(f"{F(d)} = {expr};")
        if i.rc:
            self.emit("ppc_set_cr1(c);")

    def _fp_arith(self, i, single):
        a, b, cc = F(i.rA), F(i.rB), F(i.rC)
        xo = i.xo5
        e = {18: f"{a} / {b}", 20: f"{a} - {b}", 21: f"{a} + {b}", 25: f"{a} * {cc}",
             28: f"{a} * {cc} - {b}", 29: f"{a} * {cc} + {b}",
             30: f"-({a} * {cc} - {b})", 31: f"-({a} * {cc} + {b})",
             23: f"({a} >= 0.0) ? {cc} : {b}", 24: f"1.0 / {b}",
             26: f"ppc_frsqrte({b})", 22: f"sqrt({b})"}.get(xo)
        if e is None:
            return False
        if xo == 23:
            single = False
        self._fr(i, e, single)
        return True

    def op59(self, i):
        if not self._fp_arith(i, True):
            raise Unhandled

    def op63(self, i):
        xo = i.xo10
        d, b = i.rD, i.rB
        if xo & 0x10 and self._fp_arith(i, False):
            return
        if xo in (0, 32):
            self.emit(f"ppc_fcmp(c, {i.crfD}, {F(i.rA)}, {F(b)});")
        elif xo == 12:
            self.emit(f"{F(d)} = {P1(d)} = (double)(float){F(b)};")
        elif xo in (14, 15):
            self.emit(f"{FU(d)} = ppc_fctiw(c, {F(b)}, {1 if xo == 15 else 0});")
        elif xo == 40:
            self.emit(f"{FU(d)} = {FU(b)} ^ 0x8000000000000000ull;")
        elif xo == 72:
            self.emit(f"{FU(d)} = {FU(b)};")
        elif xo == 136:
            self.emit(f"{FU(d)} = {FU(b)} | 0x8000000000000000ull;")
        elif xo == 264:
            self.emit(f"{FU(d)} = {FU(b)} & 0x7FFFFFFFFFFFFFFFull;")
        elif xo == 583:
            self.emit(f"{FU(d)} = c->fpscr;")
        elif xo == 711:
            self.emit(f"ppc_mtfsf(c, 0x{(i.w >> 17) & 0xFF:02X}, (uint32_t){FU(b)});")
        elif xo == 38:
            self.emit(f"c->fpscr |= 0x80000000u >> {d};")
        elif xo == 70:
            self.emit(f"c->fpscr &= ~(0x80000000u >> {d});")
        elif xo == 134:
            sh = (7 - i.crfD) * 4
            self.emit(f"c->fpscr = (c->fpscr & ~(0xFu << {sh})) | ({(i.w >> 12) & 0xF}u << {sh});")
        elif xo == 64:
            self.emit(f"ppc_mcrfs(c, {i.crfD}, {i.crfS});")
        else:
            raise Unhandled
        if i.rc and xo not in (0, 32, 64):
            self.emit("ppc_set_cr1(c);")

    # ---- paired singles (opcode 4)
    def op4(self, i):
        d, a, b, cc = i.rD, i.rA, i.rB, i.rC
        xo10, xo6, xo5 = i.xo10, (i.w >> 1) & 0x3F, i.xo5
        A0, A1, B0, B1, C0, C1 = F(a), P1(a), F(b), P1(b), F(cc), P1(cc)
        if xo6 in (6, 7, 38, 39):
            store, upd = xo6 & 1, xo6 >= 38
            reg = i.rS if store else d
            self._psq(store, self.eax(i), i.ps_wx, i.ps_ix, reg, a if upd else None)
            return
        pair = None
        if xo10 in (0, 32, 64, 96):
            v = (A0, B0) if xo10 in (0, 32) else (A1, B1)
            self.emit(f"ppc_fcmp(c, {i.crfD}, {v[0]}, {v[1]});")
            return
        if xo10 == 40:
            pair = (f"-{B0}", f"-{B1}")
        elif xo10 == 72:
            pair = (B0, B1)
        elif xo10 == 136:
            pair = (f"-fabs({B0})", f"-fabs({B1})")
        elif xo10 == 264:
            pair = (f"fabs({B0})", f"fabs({B1})")
        elif xo10 == 528:
            pair = (A0, B0)
        elif xo10 == 560:
            pair = (A0, B1)
        elif xo10 == 592:
            pair = (A1, B0)
        elif xo10 == 624:
            pair = (A1, B1)
        elif xo10 == 1014:
            self.emit(f"ppc_dcbz({self.eax(i)});")
            return
        rnd = True
        if pair is not None:
            rnd = xo10 >= 528    # merges round to single, moves do not
        else:
            pair = {
                10: (f"{A0} + {B1}", C1),
                11: (C0, f"{A0} + {B1}"),
                12: (f"{A0} * {C0}", f"{A1} * {C0}"),
                13: (f"{A0} * {C1}", f"{A1} * {C1}"),
                14: (f"{A0} * {C0} + {B0}", f"{A1} * {C0} + {B1}"),
                15: (f"{A0} * {C1} + {B0}", f"{A1} * {C1} + {B1}"),
                18: (f"{A0} / {B0}", f"{A1} / {B1}"),
                20: (f"{A0} - {B0}", f"{A1} - {B1}"),
                21: (f"{A0} + {B0}", f"{A1} + {B1}"),
                23: (f"({A0} >= 0.0) ? {C0} : {B0}", f"({A1} >= 0.0) ? {C1} : {B1}"),
                24: (f"1.0 / {B0}", f"1.0 / {B1}"),
                25: (f"{A0} * {C0}", f"{A1} * {C1}"),
                26: (f"ppc_frsqrte({B0})", f"ppc_frsqrte({B1})"),
                28: (f"{A0} * {C0} - {B0}", f"{A1} * {C1} - {B1}"),
                29: (f"{A0} * {C0} + {B0}", f"{A1} * {C1} + {B1}"),
                30: (f"-({A0} * {C0} - {B0})", f"-({A1} * {C1} - {B1})"),
                31: (f"-({A0} * {C0} + {B0})", f"-({A1} * {C1} + {B1})"),
            }.get(xo5)
            if pair is None:
                raise Unhandled
        cast = "(double)(float)" if rnd else ""
        self.emit(f"{{ double t0 = {cast}({pair[0]}), t1 = {cast}({pair[1]}); {F(d)} = t0; {P1(d)} = t1; }}")
        if i.rc:
            self.emit("ppc_set_cr1(c);")


def lift_function(prog, fn, starts, overrides, returns_twice=()):
    return FunctionLifter(prog, fn, starts, overrides, returns_twice).lift()
