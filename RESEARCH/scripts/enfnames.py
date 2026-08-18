# IDAPython: имена из RTTI/vftable по подстроке + xref'ы на них.
#   $env:RE_NAMES = "BaseArrayClass,BaseMapClass,TemplatedClass"
#   .\re\re.ps1 run enfnames.py -On server
import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_name
import ida_pro
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
KEYS = [k.strip() for k in os.environ.get("RE_NAMES", "").split(",") if k.strip()]

ida_auto.auto_wait()
lines = []


def say(s):
    print("[nm] %s" % s)
    lines.append(s)


for ea, name in idautils.Names():
    if not any(k in name for k in KEYS):
        continue
    say("%#x  %s" % (ea, name))
    n = 0
    for x in idautils.XrefsTo(ea, 0):
        f = ida_funcs.get_func(x.frm)
        say("     xref %#x  %s (start %#x)" % (x.frm, idc.get_func_name(x.frm), f.start_ea if f else 0))
        n += 1
        if n > 30:
            say("     ...")
            break

with open(os.path.join(OUT, "enfnames.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("written")
ida_pro.qexit(0)
