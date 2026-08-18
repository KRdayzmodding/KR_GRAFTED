# IDAPython: Hex-Rays по списку адресов.
#   $env:RE_ADDRS = "0x14033CFA0,0x1403052E0"
#   .\re\re.ps1 run decomp.py -On diag
# Результат: re\out\<target>\pseudo\<func>.c

import os

import ida_auto
import ida_funcs
import ida_hexrays
import ida_pro
import idc

OUT = os.path.join(os.environ.get("RE_OUT", "."), "pseudo")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
try:
    os.makedirs(OUT)
except OSError:
    pass

for raw in os.environ.get("RE_ADDRS", "").replace(" ", "").split(","):
    if not raw:
        continue
    ea = int(raw, 16)
    f = ida_funcs.get_func(ea)
    if not f:
        print("[dc] no func at %s" % raw)
        continue
    name = idc.get_func_name(f.start_ea)
    try:
        text = str(ida_hexrays.decompile(f.start_ea))
    except Exception as exc:
        print("[dc] %s decompile failed: %s" % (name, exc))
        continue
    path = os.path.join(OUT, name + ".c")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("// %s @ %#x\n" % (name, f.start_ea))
        fh.write(text)
    print("[dc] %s -> %s" % (name, path))

ida_pro.qexit(0)
