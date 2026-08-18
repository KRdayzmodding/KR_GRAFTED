# IDAPython: кто зовёт impl маршалируемого натива (call qword ptr [reg+8]) и
# как готовит блок аргументов — нужно для разбора out/inout.
#   .\re\re.ps1 run outargs.py -On diag
#
# Ищем косвенные вызовы через +8 (impl дескриптора) в функциях, которые рядом
# читают флаги дескриптора (+0x50) — это и есть диспетчер вызова.

import os
import struct

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_pro
import ida_segment
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()

lo = hi = None
for i in range(ida_segment.get_segm_qty()):
    s = ida_segment.getnseg(i)
    if ida_segment.get_segm_name(s) == ".text":
        lo, hi = s.start_ea, s.end_ea
img = ida_bytes.get_bytes(lo, hi - lo) or b""
print("[oa] text %#x..%#x" % (lo, hi))

# call qword ptr [reg+8]  =  [REX] FF /2 mod=01 disp8=08
cands = {}
n = len(img)
i = 0
while i < n - 5:
    if img[i] == 0xFF and (img[i + 1] & 0xF8) == 0x50 and (img[i + 1] & 7) != 4 and img[i + 2] == 0x08:
        ea = lo + i
        f = ida_funcs.get_func(ea)
        if f:
            cands.setdefault(f.start_ea, []).append(ea)
    elif img[i] in (0x41, 0x49) and img[i + 1] == 0xFF and (img[i + 2] & 0xF8) == 0x50 \
            and (img[i + 2] & 7) != 4 and img[i + 3] == 0x08:
        ea = lo + i
        f = ida_funcs.get_func(ea)
        if f:
            cands.setdefault(f.start_ea, []).append(ea)
    i += 1

print("[oa] indirect [reg+8] calls in %d funcs" % len(cands))

hits = []
for fn, sites in cands.items():
    # рядом (в той же функции) должно читаться поле +0x50 — флаги дескриптора
    body = ida_bytes.get_bytes(fn, min(ida_funcs.get_func(fn).end_ea - fn, 0x4000)) or b""
    has50 = False
    for j in range(len(body) - 3):
        if body[j] in (0x8B, 0xF6, 0x84, 0x85) and (body[j + 1] & 0xC0) == 0x40 and body[j + 2] == 0x50:
            has50 = True
            break
    if has50:
        hits.append((fn, sites))

print("[oa] dispatcher candidates: %d" % len(hits))
outdir = os.path.join(OUT, "pseudo")
try:
    os.makedirs(outdir)
except OSError:
    pass

for fn, sites in sorted(hits):
    name = idc.get_func_name(fn)
    print("[oa] %#x %s  sites=%s" % (fn, name, ["%#x" % s for s in sites]))
    try:
        text = str(ida_hexrays.decompile(fn))
    except Exception as exc:
        print("[oa]   decompile failed: %s" % exc)
        continue
    with open(os.path.join(outdir, "%s.c" % name), "w", encoding="utf-8") as fh:
        fh.write("// %s @ %#x  dispatcher candidate; indirect calls at %s\n"
                 % (name, fn, ", ".join("%#x" % s for s in sites)))
        fh.write(text)

with open(os.path.join(OUT, "outargs.txt"), "w", encoding="utf-8") as fh:
    for fn, sites in sorted(hits):
        fh.write("%#x %s  %s\n" % (fn, idc.get_func_name(fn), ",".join("%#x" % s for s in sites)))

ida_pro.qexit(0)
