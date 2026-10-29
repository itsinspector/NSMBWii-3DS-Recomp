"""Identify SDK functions with Dolphin's signature database (Data/Sys/totaldb.dsy).

The checksum is a port of HashSignatureDB::ComputeCodeChecksum: it keeps opcode
and register fields but drops immediates/branch targets, so it survives
relocation."""
import struct


def code_checksum(words):
    s = 0
    for w in words:
        op = w & 0xFC000000
        op2 = op3 = 0
        aux = op >> 26
        if aux == 4:
            op2 = w & 0x3F
            if op2 in (0, 8, 16, 21, 22):
                op3 = w & 0x7C0
        elif aux in (7, 8, 10, 11, 12, 13, 14, 15):
            op2 = w & 0x03FF0000
        elif aux in (19, 31, 63):
            op2 = w & 0x7FF
        elif aux == 59:
            op2 = w & 0x3F
            if op2 < 16:
                op3 = w & 0x7C0
        elif 32 <= aux < 56:
            op2 = w & 0x03FF0000
        s = ((s << 17) & 0xFFFE0000) | ((s >> 15) & 0x1FFFF)
        s ^= op | op2 | op3
    return s


def load_dsy(path):
    raw = open(path, "rb").read()
    n = struct.unpack_from("<I", raw, 0)[0]
    db = {}
    for k in range(n):
        o = 4 + k * 136
        csum, size = struct.unpack_from("<II", raw, o)
        name = raw[o + 8:o + 136].split(b"\0")[0].decode("latin-1")
        name = name.split("\t")[0].strip()
        if size and name:
            db.setdefault((csum, size), set()).add(name)
    return db


SDK_PREFIXES = ("OS", "__OS", "DVD", "__DVD", "GX", "__GX", "VI", "__VI", "AX", "__AX",
                "AI", "__AI", "DSP", "__DSP", "PAD", "__PAD", "SI", "__SI", "EXI", "__EXI",
                "WPAD", "__WPAD", "KPAD", "WUD", "__WUD", "SC", "__SC", "NAND", "__NAND",
                "IOS_", "__IOS", "IPC", "__ipc", "ISFS", "ES_", "__ES", "CARD", "__CARD",
                "PSMTX", "PSVEC", "PSQUAT", "C_MTX", "C_VEC", "MTX", "VEC", "TPL", "MIX",
                "SYN", "NWC24", "VF", "__VF", "ARC", "BTE", "bta_", "btm_", "l2c", "DB",
                "PPC", "__init", "__start", "__flush", "__copy", "__fill", "_save", "_rest",
                "__div", "__mod", "__cvt", "memcpy", "memset", "memmove", "memcmp", "strlen",
                "strcpy", "strcmp", "strncmp", "strncpy", "strcat", "sprintf", "vsnprintf",
                "snprintf", "printf", "__va_arg", "__ptmf", "__construct", "__destroy",
                "__dt__", "__ct__", "__register_global", "abort", "exit", "atexit", "fwrite",
                "__fwrite", "__ull", "__ll", "__shr", "__shl", "__cvt", "sin", "cos", "tan",
                "atan", "sqrt", "pow", "fmod", "floor", "ceil", "frexp", "ldexp", "modf",
                "acos", "asin", "exp", "log", "rand", "srand", "qsort", "bsearch", "tolower",
                "toupper", "isdigit", "isspace", "LCEnable", "LCDisable", "LCQueue", "LCStore",
                "LCLoad", "LCAlloc", "ICFlash", "ICInvalidate", "DCFlush", "DCStore", "DCInvalidate",
                "DCZero", "DCTouch", "HBM", "ENC", "MEM", "CX", "RFL", "WENC", "SEQ", "STD")


def match_functions(prog, funcs, db, min_size=16, sdk_only=True):
    """Returns {addr: name} for functions whose checksum matches one name.
    Very small functions match too many unrelated entries, so they need
    min_size; sdk_only keeps the SDK/runtime names Dolphin's DB is reliable for."""
    sizes = {s for _, s in db if s >= min_size}
    out = {}
    for fn in funcs:
        insns = fn.insns
        n = len(insns)
        while n and insns[n - 1].w == 0:
            n -= 1
        size = n * 4
        if size not in sizes:
            continue
        names = db.get((code_checksum([i.w for i in insns[:n]]), size))
        if names and len(names) == 1:
            name = next(iter(names))
            if not sdk_only or name.startswith(SDK_PREFIXES):
                out[fn.start] = name
    # a name matched at several addresses is ambiguous: drop it
    count = {}
    for name in out.values():
        count[name] = count.get(name, 0) + 1
    return {a: n for a, n in out.items() if count[n] == 1}
