# IDAPython: шаг 2 — реализации методов контейнеров (vtable инстанса), инстанциация map/set,
# конструктор класса, разбор кадра вызова.
#   .\re\re.ps1 run mar2.py -On diag

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
    print("[m2] %s" % s)
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


NAMES = {5: "Count", 6: "Clear", 7: "Remove", 8: "RemoveOrdered", 9: "Set", 10: "Find",
         11: "Swap", 12: "Get", 13: "Insert", 14: "InsertAt", 15: "Copy", 16: "Init",
         17: "Resize", 18: "Reserve", 19: "Sort", 20: "-"}
# для map слоты те же адреса, но другие имена (из тумблеров-переходников)
MAPNAMES = {5: "Count", 6: "Clear", 7: "Remove(key)", 8: "RemoveElement(i)", 9: "?",
            10: "Contains", 11: "Get/Find", 12: "GetElement(i)", 13: "GetKey(i)",
            14: "Insert(k,v)", 15: "Copy", 16: "GetIteratorElement", 17: "GetIteratorKey",
            18: "Begin", 19: "End", 20: "Next"}

ARR = [(0x140E24ED0, "array<int> instance"),
       (0x140E25160, "array<string> instance"),
       (0x140E24C08, "array<Class> instance")]

say("=== array instance vtables: decompile slots ===")
for base, tag in ARR:
    for i in (7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19):
        ea = ida_bytes.get_qword(base + i * 8)
        pseudo(ea, "%s  vtbl+%#x [%d] %s" % (tag, i * 8, i, NAMES.get(i, "")))

say("")
say("=== template instantiation & class ctor ===")
for ea, tag in [(0x14030C0B0, "BaseMapClass vtbl+0x60 = instantiate map<K,V>"),
                (0x14030D5A0, "BaseSetClass vtbl+0x60 = instantiate set<T>"),
                (0x1402D5600, "class ctor(cls, ctx, name, size, base)"),
                (0x140348BE0, "site of test [X+50h],40h"),
                (0x140366160, "push call frame for method"),
                (0x1403682E0, "pop call frame"),
                (0x140347740, "?"),
                (0x1402D4C70, "BaseArrayClass vtbl+0x38"),
                (0x14034A690, "BaseArrayClass vtbl+0x68"),
                (0x14030BA50, "instantiate array<T> (для сверки)")]:
    pseudo(ea, tag)

# map instance vtables — вытащим из инстанциатора все off_ адреса
say("")
say("=== map/set instance vtables referenced from instantiators ===")
for fn in (0x14030C0B0, 0x14030D5A0):
    f = ida_funcs.get_func(fn)
    seen = []
    ea = f.start_ea
    while ea < f.end_ea:
        for i in range(idc.get_item_size(ea)):
            pass
        mnem = idc.print_insn_mnem(ea)
        if mnem == "lea":
            op = idc.get_operand_value(ea, 1)
            if op and idc.get_segm_name(op) in (".rdata", ".data") and op not in seen:
                nm = idc.get_name(op) or ""
                if nm.startswith("off_") or "vftable" in (nm or ""):
                    seen.append(op)
        ea = idc.next_head(ea, f.end_ea)
    say("")
    say("--- %s: %d candidate tables" % (idc.get_func_name(fn), len(seen)))
    for op in seen:
        say("  %#x  %s" % (op, idc.get_name(op)))
        for i in range(0, 22):
            v = ida_bytes.get_qword(op + i * 8)
            if not ida_funcs.get_func(v):
                continue
            say("     +%#04x [%2d] %-20s %#x %s" % (i * 8, i, MAPNAMES.get(i, ""), v, idc.get_func_name(v)))

with open(os.path.join(OUT, "mar2.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
