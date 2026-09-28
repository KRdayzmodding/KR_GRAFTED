# IDAPython: кто зовёт функции из списка -> Hex-Rays каждого вызывающего.
#   $env:RE_ADDRS = "0x1402D37A0,0x1404D2510"
# Результат: callers.md (адрес -> вызывающие) и pseudo\<func>.c
import os
import re

import ida_auto
import ida_funcs
import ida_hexrays
import ida_pro
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
PSEUDO = os.path.join(OUT, "pseudo")
os.makedirs(PSEUDO, exist_ok=True)
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()


def dump(ea):
    f = ida_funcs.get_func(ea)
    if not f:
        return None
    name = idc.get_func_name(f.start_ea)
    path = os.path.join(PSEUDO, re.sub(r"[^A-Za-z0-9_.-]", "_", name)[:120] + ".c")
    if not os.path.exists(path):
        try:
            text = str(ida_hexrays.decompile(f.start_ea))
        except Exception as exc:  # noqa: BLE001
            text = "// decompile failed: %s" % exc
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("// %s @ %#x\n%s\n" % (name, f.start_ea, text))
    return name


lines = []
for raw in os.environ.get("RE_ADDRS", "").replace(" ", "").split(","):
    if not raw:
        continue
    ea = int(raw, 16)
    dump(ea)
    lines.append("## %s %s" % (raw, idc.get_func_name(ea)))
    for x in idautils.XrefsTo(ea):
        lines.append("- %#x in %s" % (x.frm, dump(x.frm)))
with open(os.path.join(OUT, "callers.md"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines) + "\n")
ida_pro.qexit(0)
