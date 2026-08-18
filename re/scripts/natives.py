# IDAPython: найти движковую функцию регистрации нативов Enforce Script и выдать RVA,
# которые нужны graft-модулю.
#
#   .\re\re.ps1 run natives.py -On diag
#
# Как ищем (без хардкода адресов, работает на любом билде):
#   строка "MemoryValidation" -> функция, которая на неё ссылается = RegisterCoreNatives.
#   Внутри неё три разных callee: чаще всего вызывается RegisterMethod (~80 раз),
#   реже RegisterGlobal (~15), ещё реже FindClass (~8). Считаем частоты.
#
# Результат: re\out\<target>\natives.json + псевдокод найденной функции.

import json
import os

import ida_auto
import ida_hexrays
import ida_nalt
import ida_pro
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
ANCHOR = os.environ.get("RE_ANCHOR", "MemoryValidation")

ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
base = ida_nalt.get_imagebase()


def find_anchor_func():
    for s in idautils.Strings():
        if str(s) != ANCHOR:
            continue
        for xref in idautils.DataRefsTo(s.ea):
            fn = idc.get_func_attr(xref, idc.FUNCATTR_START)
            if fn != idc.BADADDR:
                return fn
    return None


func = find_anchor_func()
if func is None:
    print("[natives] anchor %r not found" % ANCHOR)
    ida_pro.qexit(1)

calls = {}
for ea in idautils.FuncItems(func):
    if idc.print_insn_mnem(ea) != "call":
        continue
    target = idc.get_operand_value(ea, 0)
    if target and target != idc.BADADDR:
        calls[target] = calls.get(target, 0) + 1

ranked = sorted(calls.items(), key=lambda kv: -kv[1])
roles = ["RegisterMethod", "RegisterGlobal", "FindClass"]
result = {
    "imagebase": "%#x" % base,
    "RegisterCoreNatives": {"ea": "%#x" % func, "rva": "%#x" % (func - base)},
    "callees": [
        {
            "role": roles[i] if i < len(roles) else "?",
            "ea": "%#x" % ea,
            "rva": "%#x" % (ea - base),
            "calls": n,
            "name": idc.get_func_name(ea),
        }
        for i, (ea, n) in enumerate(ranked)
    ],
}

with open(os.path.join(OUT, "natives.json"), "w", encoding="utf-8") as fh:
    json.dump(result, fh, indent=2)

try:
    with open(os.path.join(OUT, "RegisterCoreNatives.c"), "w", encoding="utf-8") as fh:
        fh.write(str(ida_hexrays.decompile(func)))
except Exception as exc:
    print("[natives] decompile failed: %s" % exc)

print("[natives] " + json.dumps(result))
ida_pro.qexit(0)
