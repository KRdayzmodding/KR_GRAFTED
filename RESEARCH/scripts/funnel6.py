# IDAPython: контейнеры. Из tpl10.txt известны string-специализации методов array<>:
#   Insert 0x14031FE30, Set 0x140324180, Get 0x14031A5B0, Copy 0x140317760,
#   Find 0x140319EA0, Remove 0x140321FE0, InsertAt 0x140321D90, Init 0x14031F8D0
# Вопрос: каким примитивом они копируют строку.
import os

import ida_auto
import ida_funcs
import ida_hexrays
import ida_pro
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
lines = []

KNOWN = {
    0x140345C90: "ALLOC+memcpy (блок строки)",
    0x140347F80: "ASSIGN var<-char*",
    0x140347EC0: "ASSIGN (с флагом force)",
    0x140347800: "CONCAT",
    0x1403052E0: "ASSIGN по длине",
    0x140305370: "ASSIGN по длине (2)",
    0x140348B50: "FREE блока",
    0x140349000: "REGION(ptr)",
    0x140367020: "воронка sub_140367020",
    0x140366CC0: "release var",
    0x140366ED0: "init var",
}

TARGETS = [
    (0x14031FE30, "array<string>.Insert"),
    (0x140324180, "array<string>.Set"),
    (0x14031A5B0, "array<string>.Get"),
    (0x140317760, "array<string>.Copy"),
    (0x140319EA0, "array<string>.Find"),
    (0x140321D90, "array<string>.InsertAt"),
    (0x14031F8D0, "array<string>.Init"),
    (0x140321FE0, "array<string>.Remove"),
    (0x140366C60, "map.Insert (общая)"),
]


def say(s=""):
    print("[f6] %s" % s)
    lines.append(s)


def sz(ea):
    f = ida_funcs.get_func(ea)
    return (f.end_ea - f.start_ea) if f else 0


def calls_of(ea, depth=2, seen=None, acc=None):
    if seen is None:
        seen, acc = set(), []
    if depth == 0 or ea in seen:
        return acc
    seen.add(ea)
    f = ida_funcs.get_func(ea)
    if not f:
        return acc
    for item in idautils.FuncItems(f.start_ea):
        for xr in idautils.XrefsFrom(item, 0):
            if xr.type not in (16, 17):
                continue
            if xr.to in KNOWN:
                if xr.to not in acc:
                    acc.append(xr.to)
            else:
                calls_of(xr.to, depth - 1, seen, acc)
    return acc


for ea, name in TARGETS:
    f = ida_funcs.get_func(ea)
    if not f:
        say("!! нет функции на %#x (%s)" % (ea, name))
        continue
    say("######## %s @ %#x (%d байт) ########" % (name, ea, sz(ea)))
    found = calls_of(ea, 3)
    if found:
        for k in found:
            say("   достигает: %-28s %s" % (idc.get_func_name(k), KNOWN[k]))
    else:
        say("   ни один из известных строковых примитивов не достигнут (глубина 3)")
    try:
        txt = str(ida_hexrays.decompile(f.start_ea))
        for ln in txt.splitlines()[:70]:
            say("  " + ln)
    except Exception as exc:
        say("  !! %s" % exc)
    say()

with open(os.path.join(OUT, "funnel6.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
ida_pro.qexit(0)
