# IDAPython: сырой листинг функций по адресам.
#   $env:RE_DIS = "0x140306480,0x140348EF0"
import os
import ida_auto
import ida_funcs
import ida_pro
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
os.makedirs(os.path.join(OUT, "asm"), exist_ok=True)

for raw in os.environ.get("RE_DIS", "").replace(" ", "").split(","):
    if not raw:
        continue
    ea = int(raw, 16)
    f = ida_funcs.get_func(ea)
    if not f:
        print("[dis] no func at %s" % raw)
        continue
    name = idc.get_func_name(ea)
    path = os.path.join(OUT, "asm", name + ".asm")
    with open(path, "w", encoding="utf-8") as fh:
        cur = f.start_ea
        while cur < f.end_ea:
            fh.write("%016X  %s\n" % (cur, idc.GetDisasm(cur)))
            cur = idc.next_head(cur, f.end_ea)
    print("[dis] %s -> %s" % (name, path))

ida_pro.qexit(0)
