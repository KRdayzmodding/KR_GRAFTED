# IDAPython: шаг 6 — полный листинг VM-петли + сборка кадра аргументов.
#   .\re\re.ps1 run tpl6.py -On diag

import os

import ida_auto
import ida_funcs
import ida_hexrays
import ida_pro
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
os.makedirs(os.path.join(OUT, "asm"), exist_ok=True)
os.makedirs(os.path.join(OUT, "pseudo"), exist_ok=True)

VM = 0x140368880
f = ida_funcs.get_func(VM)
path = os.path.join(OUT, "asm", "sub_140368880.asm")
with open(path, "w", encoding="utf-8") as fh:
    cur = f.start_ea
    while cur < f.end_ea:
        fh.write("%016X  %s\n" % (cur, idc.GetDisasm(cur)))
        cur = idc.next_head(cur, f.end_ea)
print("[t6] VM %#x..%#x -> %s" % (f.start_ea, f.end_ea, path))

for ea in (0x1403663B0, 0x1403662A0, 0x140347740, 0x1403682E0, 0x140349630, 0x140345780):
    try:
        text = str(ida_hexrays.decompile(ea))
    except Exception as exc:
        print("[t6] decompile %#x failed: %s" % (ea, exc))
        continue
    name = idc.get_func_name(ea)
    with open(os.path.join(OUT, "pseudo", name + ".c"), "w", encoding="utf-8") as fh:
        fh.write("// %s @ %#x\n" % (name, ea))
        fh.write(text)
    print("[t6] wrote pseudo/%s.c" % name)

ida_pro.qexit(0)
