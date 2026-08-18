# IDAPython: раскладка vftable по адресу (слот -> функция).
#   $env:RE_VT="0x140b8eab8:32,0x140b8eb88:32,0x140b8ed80:4"
#   .\re\re.ps1 run vtdump.py -On server
import os

import ida_auto
import ida_bytes
import ida_hexrays
import ida_pro
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
lines = []


def say(s):
    print("[vt] %s" % s)
    lines.append(s)


for spec in os.environ.get("RE_VT", "").split(","):
    if not spec.strip():
        continue
    addr, _, cnt = spec.partition(":")
    base = int(addr, 16)
    n = int(cnt or "24")
    say("=== %s (%#x) ===" % (idc.get_name(base) or "?", base))
    for i in range(n):
        ea = base + 8 * i
        fn = ida_bytes.get_qword(ea)
        say("  +%#04x [%2d]  %#x  %s" % (8 * i, i, fn, idc.get_func_name(fn) or ""))

for a in os.environ.get("RE_DEC", "").replace(" ", "").split(","):
    if not a:
        continue
    ea = int(a, 16)
    try:
        say("--- decompile %#x (%s)" % (ea, idc.get_func_name(ea)))
        say(str(ida_hexrays.decompile(ea)))
    except Exception as exc:
        say("  failed: %s" % exc)

with open(os.path.join(OUT, "vtdump.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
ida_pro.qexit(0)
