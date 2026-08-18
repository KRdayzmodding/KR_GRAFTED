# IDAPython: сверка маршалируемого ABI на СЕРВЕРНОМ бинаре.
#   idat64 -A -S mar5.py <server idb>
# Ищем по байтам переходники контейнеров (форма снята на diag) и место в интерпретаторе,
# где строится блок {args, argTypes} и зовётся impl(this, block, desc).

import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_pro
import ida_search
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()

lines = []


def say(s):
    print("[m5] %s" % s)
    lines.append(str(s))


def pseudo(ea, tag=""):
    if ea in (0, idc.BADADDR) or not ida_funcs.get_func(ea):
        say("!! no func at %#x (%s)" % (ea, tag))
        return
    say("")
    say("######## %s @ %#x  %s" % (idc.get_func_name(ea), ea, tag))
    try:
        say(str(ida_hexrays.decompile(ea)))
    except Exception as exc:
        say("  decompile failed: %s" % exc)


LO, HI = 0x140001000, 0x140DDC000

# формы переходников с diag
PATS = [
    ("Get-like  (this, ret, idx)", "48 8B 02 4D 8B D0 4C 8B 09 48 8B 10 44 8B 02 49 8B 12 41 FF"),
    ("Set-like  (this, idx, arg)", "4C 8B 02 4C 8B 09 49 8B 00 4D 8B 40 08 8B 10 41 FF"),
    ("Find-like (this, ret, arg)", "48 8B 01 4D 8B C8 4C 8B 02 49 8B 11 4D 8B 00 FF"),
    ("raw fwd   (this, blk, desc)", "48 8B 01 FF"),
    ("Insert-like call+store int", "48 8B 02 4C 8B D8 4C 8B 09 48 8B 10 41 FF 51"),
]
say("=== thunk shapes ===")
for tag, pat in PATS:
    n = 0
    ea = LO
    while n < 40:
        ea = ida_search.find_binary(ea, HI, pat, 16, idc.SEARCH_DOWN)
        if ea == idc.BADADDR:
            break
        f = ida_funcs.get_func(ea)
        if f and f.start_ea == ea:
            say("  %-30s %#x  %s" % (tag, ea, idc.get_func_name(ea)))
            n += 1
        ea += 1
    say("  -> %s: %d starts" % (tag, n))

# регистратор контейнеров: ищем по строкам 'array' 'set' 'map'
say("")
say("=== container registrar (strings array/set/map in one function) ===")
strs = {}
for s in idautils.Strings():
    v = str(s)
    if v in ("array", "set", "map", "Insert", "GetKey", "Reserve", "InsertAt", "GetIteratorKey"):
        strs.setdefault(v, []).append(s.ea)
cand = {}
for v, eas in strs.items():
    for sea in eas:
        for xr in idautils.DataRefsTo(sea):
            f = ida_funcs.get_func(xr)
            if f:
                cand.setdefault(f.start_ea, set()).add(v)
for fea, names in sorted(cand.items(), key=lambda kv: -len(kv[1]))[:6]:
    say("  %#x %s -> %s" % (fea, idc.get_func_name(fea), sorted(names)))
best = sorted(cand.items(), key=lambda kv: -len(kv[1]))
if best:
    pseudo(best[0][0], "container registrar (server)")

with open(os.path.join(OUT, "mar5.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
