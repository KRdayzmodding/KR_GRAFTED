#!/usr/bin/env python3
"""Разведка подсистемы Central Economy в DayZServer_x64.exe (hot-reload API).

Ищет код, ссылающийся на якоря экономики (имена файлов и XML-тегов, логи [CE]),
и группирует его по функциям (.pdata). После патча игры повторить и сравнить
кластеры — сдвиг = сигнал для обновления паттернов graft-плагина.

    py -3.12 tools/ce_map_scan.py [путь_к_exe]

Использовано 2026-10-31 (DayZ 1.30.164200): см. Docs/design/dp-storage/.
"""
import bisect
import struct
import sys
from collections import defaultdict

DEFAULT_EXE = r"C:\Program Files (x86)\Steam\steamapps\common\DayZServer\DayZServer_x64.exe"

ANCHORS = [b"cfgeconomycore", b"types.xml", b"nominal", b"quantmin", b"restock",
           b"economy.xml", b"events.xml", b"[CE]", b"dynamic event"]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EXE
    data = open(path, "rb").read()
    e = struct.unpack_from("<I", data, 0x3C)[0]
    coff = e + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
    optsz = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    imgbase = struct.unpack_from("<Q", data, opt + 24)[0]
    secs = []
    for i in range(nsec):
        o = opt + optsz + 40 * i
        name = data[o:o + 8].rstrip(b"\0").decode(errors="replace")
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, o + 8)
        secs.append((name, vaddr, vsize, raddr, rsize))

    def off2rva(off):
        for _, va, vs, ra, rs in secs:
            if ra <= off < ra + rs:
                return va + (off - ra)
        return None

    funcs = []
    _, pva, pvs, pra, prs = [s for s in secs if s[0] == ".pdata"][0]
    for i in range(prs // 12):
        b, en, _ = struct.unpack_from("<III", data, pra + i * 12)
        if b == 0 and en == 0:
            break
        funcs.append((b, en))
    begins = [b for b, _ in funcs]

    def func_of(rva):
        i = bisect.bisect_right(begins, rva) - 1
        return funcs[i][0] if i >= 0 and funcs[i][0] <= rva < funcs[i][1] else None

    amap = {}
    print("якоря (RVA):")
    for s in ANCHORS:
        st, rvas = 0, []
        while len(rvas) < 4:
            i = data.find(s, st)
            if i < 0:
                break
            r = off2rva(i)
            if r is not None:
                rvas.append(r)
            st = i + 1
        print(" ", s.decode(), [hex(x) for x in rvas])
        for r in rvas:
            amap.setdefault(r, s.decode())

    _, tva, tvs, tra, trs = [s for s in secs if s[0] == ".text"][0]
    blob = data[tra:tra + trs]
    byfunc = defaultdict(set)
    i = 0
    n = len(blob)
    while i < n - 7:
        if blob[i] in (0x48, 0x4C) and blob[i + 1] == 0x8D and (blob[i + 2] & 0xC7) == 0x05:
            disp = struct.unpack_from("<i", blob, i + 3)[0]
            lbl = amap.get(tva + i + 7 + disp)
            if lbl:
                f = func_of(tva + i)
                if f is not None:
                    byfunc[f].add(lbl)
            i += 7
        else:
            i += 1

    print("\nфункции-якоря CE (VA, размер, якоря):")
    for f in sorted(byfunc):
        size = next(en for b, en in funcs if b == f) - f
        print(f"  {imgbase + f:#x}  {size:#6x}  {sorted(byfunc[f])}")
    print("всего:", len(byfunc))


if __name__ == "__main__":
    main()
