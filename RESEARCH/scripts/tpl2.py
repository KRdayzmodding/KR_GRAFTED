# IDAPython: выгрузить псевдокод по списку адресов с внятной диагностикой.
#
#   $env:RE_ADDRS = "0x140333670,0x140337260"
#   .\re\re.ps1 run tpl2.py -On diag
#
# В отличие от probe.py не молчит при отказе Hex-Rays и не пропускает адреса,
# которые IDA не считает началом функции.

import os

import ida_auto
import ida_funcs
import ida_hexrays
import ida_pro
import idc

OUT = os.environ.get("RE_OUT", ".")
PSEUDO = os.path.join(OUT, "pseudo")

ida_auto.auto_wait()
os.makedirs(PSEUDO, exist_ok=True)
ok = ida_hexrays.init_hexrays_plugin()
print("[tpl2] hexrays=%s" % ok)

for raw in os.environ.get("RE_ADDRS", "").replace(" ", "").split(","):
    if not raw:
        continue
    try:
        ea = int(raw, 16)
    except ValueError:
        print("[tpl2] bad addr %r" % raw)
        continue
    f = ida_funcs.get_func(ea)
    if not f:
        print("[tpl2] %#x: no function here" % ea)
        continue
    name = idc.get_func_name(f.start_ea) or ("sub_%X" % f.start_ea)
    path = os.path.join(PSEUDO, name + ".c")
    try:
        cf = ida_hexrays.decompile(f.start_ea)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("// %s @ %#x\n%s\n" % (name, f.start_ea, cf))
        print("[tpl2] %#x -> %s" % (ea, path))
    except Exception as exc:
        print("[tpl2] %#x (%s): decompile failed: %s" % (ea, name, exc))

ida_pro.qexit(0)
