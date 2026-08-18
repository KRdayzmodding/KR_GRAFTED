# IDAPython: точечно — что делает движок с возвратом натива, помеченного `owned`
# (бит 0x800 маски возврата). sub_14033E9F0 читает его дважды и лежит в том же районе,
# что и регистрация нативов, линковщик и поиск метода.
#
#   .\re\re.ps1 run owned2.py -On diag
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


def say(s):
    print("[o2] %s" % s)
    lines.append(s)


TARGETS = [0x14033E9F0, 0x140883120, 0x140698950, 0x14023AB70]

for ea in TARGETS:
    fn = ida_funcs.get_func(ea)
    if not fn:
        say("!! нет функции @ %#x" % ea)
        continue
    say("")
    say("################ %s @ %#x (%d байт) ################"
        % (idc.get_func_name(ea), ea, fn.end_ea - fn.start_ea))
    # Кто её зовёт — по этому видно, из какого места движка она работает.
    callers = sorted({ida_funcs.get_func(x.frm).start_ea
                      for x in idautils.XrefsTo(fn.start_ea, 0)
                      if ida_funcs.get_func(x.frm)})
    say("зовут: " + ", ".join("%s@%#x" % (idc.get_func_name(c), c) for c in callers[:10]))
    try:
        cfunc = ida_hexrays.decompile(ea)
    except Exception as exc:
        say("!! не декомпилируется: %s" % exc)
        continue
    for line in str(cfunc).splitlines():
        say("  " + line)

with open(os.path.join(OUT, "owned2.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
ida_pro.qexit(0)
