# IDAPython: шаг 3 — примитивы строк/ссылок (refcount) и таблицы инстансов map.
#   .\re\re.ps1 run mar3.py -On diag

import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_pro
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()

lines = []


def say(s):
    print("[m3] %s" % s)
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


say("=== string / reference primitives ===")
for ea, tag in [
    (0x1403017A0, "TString init(empty)"),
    (0x1403051A0, "TString assign(dst,src)"),
    (0x140302390, "TString release"),
    (0x140301760, "TString ctor from raw"),
    (0x140305270, "TString set empty"),
    (0x140302B10, "TString assign2"),
    (0x1403024C0, "TString vector copy"),
    (0x140305290, "ref assign(dst,src)"),
    (0x140305000, "ref assign from raw"),
    (0x1403051C0, "ref clear"),
    (0x1402D33A0, "strong ref set"),
    (0x1401581C0, "weak/plain ref set"),
    (0x140347F80, "var string assign path"),
    (0x140344270, "base class ctor(cls,ctx,name,size,base)"),
    (0x1403514A0, "register class in context"),
    (0x140351860, "create primitive type"),
    (0x1403512F0, "create Class type"),
    (0x1403519A0, "create global var"),
]:
    pseudo(ea, tag)

# ── таблицы инстансов map: слот 7 у MapClassT-таблицы = конструктор инстанса ──
MAPCLS = [(0x140E26C48, "map<string,int>"), (0x140E26208, "map<int,int>"),
          (0x140E26ED8, "map<string,string>"), (0x140E27168, "map<string,Class>"),
          (0x140E26498, "map<int,string>")]
say("")
say("=== map class tables -> instance ctor -> instance vtable ===")
inst = []
for ea, tag in MAPCLS:
    ctor = ida_bytes.get_qword(ea + 7 * 8)
    say("")
    say("--- %s class vtbl %#x, slot7 = %#x %s" % (tag, ea, ctor, idc.get_func_name(ctor)))
    pseudo(ctor, "%s instance ctor" % tag)

MAPNAMES = {5: "Count", 6: "Clear", 7: "Remove(key)", 8: "RemoveElement(i)", 9: "?",
            10: "Contains", 11: "Get/Find", 12: "GetElement(i)", 13: "GetKey(i)",
            14: "Insert(k,v)", 15: "Copy", 16: "GetIteratorElement", 17: "GetIteratorKey",
            18: "Begin", 19: "End", 20: "Next"}

# известные vtable инстансов map из RTTI
MAPINST = [(0x140E26E08, "MapT<TString,TString>"),
           (0x140E28F58, "MapT<Instance*,Instance*>")]
say("")
say("=== known MapT instance vtables ===")
for ea, tag in MAPINST:
    say("")
    say("%s @ %#x" % (tag, ea))
    for i in range(0, 22):
        v = ida_bytes.get_qword(ea + i * 8)
        if not ida_funcs.get_func(v):
            continue
        say("   +%#04x [%2d] %-20s %#x %s" % (i * 8, i, MAPNAMES.get(i, ""), v, idc.get_func_name(v)))

for ea, tag in MAPINST[:1]:
    for i in (7, 8, 10, 11, 12, 13, 14, 16, 17, 18, 19, 20):
        pseudo(ida_bytes.get_qword(ea + i * 8), "%s vtbl+%#x [%d] %s" % (tag, i * 8, i, MAPNAMES.get(i, "")))

with open(os.path.join(OUT, "mar3.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
