import os
import ida_auto, ida_hexrays, ida_pro, idc
OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait(); ida_hexrays.init_hexrays_plugin()
lines = []
def say(s):
    print("[t9] %s" % s); lines.append(str(s))
for ea, tag in [(0x140307630, "array<int> vtbl+0x38"),
                (0x140307730, "array<string> vtbl+0x38"),
                (0x1403076B0, "array<Class> vtbl+0x38"),
                (0x140308B10, "array<int> vtbl+0x60"),
                (0x140308E10, "array<string> vtbl+0x60")]:
    say(""); say("######## %s @ %#x  %s" % (idc.get_func_name(ea), ea, tag))
    try:
        say(str(ida_hexrays.decompile(ea)))
    except Exception as exc:
        say("fail: %s" % exc)
with open(os.path.join(OUT, "tpl9.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
ida_pro.qexit(0)
