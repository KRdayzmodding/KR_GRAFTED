# IDAPython: кто использует строку 'array<string>' и как рядом ищется класс.
#   .\re\re.ps1 run tplstr.py -On diag

import os

import ida_auto
import ida_funcs
import ida_hexrays
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
EA = int(os.environ.get("RE_STR", "0x140e29eb0"), 16)

ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()

for xref in idautils.DataRefsTo(EA):
    f = ida_funcs.get_func(xref)
    print("[str] xref %#x in %s" % (xref, idc.get_func_name(xref)))
    if f:
        try:
            path = os.path.join(OUT, "pseudo", "sub_%X.c" % f.start_ea)
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(str(ida_hexrays.decompile(f.start_ea)))
            print("[str]   -> %s" % path)
        except Exception as exc:
            print("[str]   decompile failed: %s" % exc)

# заодно: строки, похожие на имена шаблонных классов вообще (широкий фильтр)
for s in idautils.Strings():
    t = str(s)
    if 6 < len(t) < 60 and t.count("<") == 1 and t.endswith(">") and " " not in t:
        print("[str] generic-ish %#x %r" % (s.ea, t))

import ida_pro

ida_pro.qexit(0)
