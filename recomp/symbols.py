"""Symbol names for SMNE01 rev 2 (E2).

Sources (all community-made, PAL v1 = P1 addresses):
  third_party/newer/kamek_pal.x          Newer's symbol list
  third_party/newer/versions-nsmbw.txt   P1 -> other versions address maps
  third_party/nsmbw-decomp/syms.txt      NSMBW-Decomp symbol list
plus Dolphin's SDK signature DB for functions none of those name.

REL symbols in those lists use the community's fixed REL load addresses; we
find the offset to our own REL placement by voting against discovered
function starts.
"""
import bisect
import collections
import os
import re

TP = "third_party"

# .text addresses of the RELs in the community's P1 layout (NSMBW-Decomp slices baseAddr)
REL_TEXT_P1 = {"d_profileNP": 0x807685A0, "d_basesNP": 0x8076D770,
               "d_enemiesNP": 0x809A2D90, "d_en_bossNP": 0x80B1CA10}


class VersionMap:
    def __init__(self, path):
        self.parent = {}
        self.ranges = {}
        cur = None
        for line in open(path, encoding="utf-8"):
            line = line.split("#")[0].strip()
            if not line:
                continue
            m = re.match(r"\[(\w+)\]", line)
            if m:
                cur = m.group(1)
                self.ranges[cur] = []
                continue
            if line.startswith("extend "):
                self.parent[cur] = line.split()[1]
                continue
            m = re.match(r"([0-9a-fA-F]+)-([0-9a-fA-F]+|\*):\s*([+-])0x([0-9a-fA-F]+)", line)
            if m:
                lo = int(m.group(1), 16)
                hi = 0xFFFFFFFF if m.group(2) == "*" else int(m.group(2), 16)
                off = int(m.group(4), 16) * (1 if m.group(3) == "+" else -1)
                self.ranges[cur].append((lo, hi, off))
        for r in self.ranges.values():
            r.sort()

    def _step(self, ver, addr):
        """Map an address of parent(ver) to ver. None if unmapped."""
        r = self.ranges[ver]
        i = bisect.bisect_right(r, (addr, 0xFFFFFFFF, 1 << 40)) - 1
        if i >= 0 and r[i][0] <= addr <= r[i][1]:
            return addr + r[i][2]
        return None

    def from_p1(self, ver, addr):
        chain = []
        v = ver
        while v in self.parent:
            chain.append(v)
            v = self.parent[v]
        for v in reversed(chain):
            addr = self._step(v, addr)
            if addr is None:
                return None
        return addr


def load_p1_symbols():
    syms = {}
    path = os.path.join(TP, "newer", "kamek_pal.x")
    if os.path.exists(path):
        for m in re.finditer(r"^\s*([^\s=]+)\s*=\s*0x([0-9A-Fa-f]+)\s*;", open(path).read(), re.M):
            syms.setdefault(int(m.group(2), 16), m.group(1))
    path = os.path.join(TP, "nsmbw-decomp", "syms.txt")
    if os.path.exists(path):
        for line in open(path):
            if "=" in line:
                name, addr = line.strip().rsplit("=", 1)
                syms[int(addr, 16)] = name      # decomp names take priority
    return syms


def c_ident(name):
    s = re.sub(r"[^0-9A-Za-z_]", "_", name)
    return s[:80]


def build_symbols(prog, funcs, version="E2"):
    """Returns {addr: name} for function starts of our build."""
    vmap = VersionMap(os.path.join(TP, "newer", "versions-nsmbw.txt"))
    p1 = load_p1_symbols()
    starts = {f.start for f in funcs}
    dol_end = max(s.end for s in prog.sections if s.addr < 0x80800000 and s.exec)
    out = {}
    rel_syms = []
    for a, name in p1.items():
        e = vmap.from_p1(version, a)
        if e is None:
            continue
        if e < dol_end:
            if e in starts:
                out[e] = name
        else:
            rel_syms.append((e, name))

    # community REL layout: .text of each module at a fixed P1 address
    for rel in prog.rels.values():
        text = rel.sections[1]
        e2_text = vmap.from_p1(version, REL_TEXT_P1[rel.name])
        delta = text.addr - e2_text
        for e, name in rel_syms:
            a = e + delta
            if text.contains(a) and a in starts:
                out[a] = name
    return out
