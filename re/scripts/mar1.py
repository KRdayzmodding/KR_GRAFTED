# IDAPython: маршалируемый вызов (proto) — диспетчер, блок аргументов, vtable контейнеров.
#   .\re\re.ps1 run mar1.py -On diag

import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_name
import ida_pro
import ida_search
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()

lines = []


def say(s):
    print("[m1] %s" % s)
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


# ── 1. RTTI-таблицы контейнеров ───────────────────────────────────────────────
say("=== container vftables (RTTI names) ===")
WANT = ("ArrayT<", "MapT<", "SetT<", "BaseArrayClass", "BaseMapClass", "BaseSetClass",
        "TemplatedClass", "StringT", "ManagedNativeInstance")
vts = {}
for ea, nm in idautils.Names():
    dn = ida_name.demangle_name(nm, 0) or nm
    if "vftable" not in dn:
        continue
    if not any(w in dn for w in WANT):
        continue
    row = []
    for i in range(0, 24):
        v = ida_bytes.get_qword(ea + i * 8)
        row.append(v if ida_funcs.get_func(v) else 0)
    vts[dn] = (ea, row)

for dn in sorted(vts):
    ea, row = vts[dn]
    say("")
    say("%s @ %#x" % (dn, ea))
    for i, v in enumerate(row):
        if not v:
            continue
        say("   +%#04x [%2d]  %#x  %s" % (i * 8, i, v, idc.get_func_name(v)))

# ── 2. поиск ветки, различающей маршалируемые (флаг 0x40 в дескрипторе +80) ──
say("")
say("=== sites: test [reg+50h], 40h / 20h ===")
seg_start, seg_end = 0x140001000, 0x140DDC000
for imm, mnem in ((0x40, "40h"), (0x20, "20h")):
    for rex in ("", "41 "):
        for r in range(8):
            for op, tail in (("F6", ""), ("F7", " 00 00 00")):
                pat = "%s%s %02X 50 %02X%s" % (rex, op, 0x40 | r, imm, tail)
                ea = seg_start
                while True:
                    ea = ida_search.find_binary(ea, seg_end, pat, 16, idc.SEARCH_DOWN)
                    if ea == idc.BADADDR:
                        break
                    f = ida_funcs.get_func(ea)
                    say("  %s  %#x  in %s" % (mnem, ea, idc.get_func_name(ea) if f else "?"))
                    ea += 1

# ── 3. ключевые функции VM ────────────────────────────────────────────────────
say("")
say("=== VM helpers ===")
for ea, tag in [
    (0x1403672E0, "sub_1403672E0 (var predicate)"),
    (0x140347CB0, "var assign/copy"),
    (0x140366ED0, "var init"),
    (0x1403517F0, "intern/alloc string"),
    (0x140345C90, "alloc ret buffer"),
    (0x140348F40, "FindFunctionIndex"),
    (0x1403520C0, "FindClass"),
    (0x140352480, "?"),
    (0x140C6E880, "dynamic fastcall thunk"),
    (0x140C6E92B, "float ret helper"),
    (0x1402D5600, "class ctor(name,size,base)"),
    (0x140308B10, "array<int> class factory"),
    (0x140307630, "array<int> instance ctor"),
    (0x1402D33A0, "release strong ref?"),
    (0x1401581C0, "release?"),
    (0x140367430, "?"),
    (0x1403473B0, "?"),
    (0x140315970, "map.Set thunk"),
    (0x140316270, "?"),
    (0x1403161C0, "?"),
    (0x1403162C0, "?"),
    (0x140315CD0, "?"),
    (0x140306520, "typename.GetVariableName?"),
    (0x140306530, "?"),
    (0x140348EF0, "field address"),
    (0x140316090, "Class.StaticType"),
    (0x140304CA0, "Class.Type"),
    (0x140304BD0, "Class.ClassName"),
    (0x140308800, "Class.IsInherited"),
]:
    pseudo(ea, tag)

with open(os.path.join(OUT, "mar1.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
