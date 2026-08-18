# IDAPython: таблица модификаторов объявления (private/static/proto/native/...)
# из sub_1402B6D50: массив по RE_TABLE из троек {имя, флаги типа, флаги функции}.
# Плюс: кто инициализирует глобалы-строки RE_STRLO..RE_STRHI.
#
#   $env:RE_TABLE=0x140E31440 ; $env:RE_STRLO=0x140F0D5E0 ; $env:RE_STRHI=0x140F0D6C8
#   .\re\re.ps1 run modtable.py -On server

import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_pro
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
TABLE = int(os.environ.get("RE_TABLE", "0x140E31440"), 16)
LO = int(os.environ.get("RE_STRLO", "0x140F0D5E0"), 16)
HI = int(os.environ.get("RE_STRHI", "0x140F0D6C8"), 16)

ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
lines = []


def say(s):
    print("[mt] %s" % s)
    lines.append(s)


say("=== таблица %#x (24 байта на запись) ===" % TABLE)
for i in range(24):
    ea = TABLE + 24 * i
    name = ida_bytes.get_qword(ea)
    f1 = ida_bytes.get_qword(ea + 8)
    f2 = ida_bytes.get_qword(ea + 16)
    say("  [%2d] %#x  name=%#x  typeflags=%#x  declflags=%#x" % (i, ea, name, f1, f2))

say("")
say("=== кто пишет в глобалы-строки %#x..%#x ===" % (LO, HI))
writers = set()
ea = LO
while ea < HI:
    for x in idautils.XrefsTo(ea, 0):
        f = ida_funcs.get_func(x.frm)
        if f:
            writers.add(f.start_ea)
        say("  %#x <- ref %#x in %s" % (ea, x.frm, idc.get_func_name(x.frm)))
    ea += 8

for w in sorted(writers):
    path = os.path.join(OUT, "pseudo", "%s.c" % idc.get_func_name(w))
    try:
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(str(ida_hexrays.decompile(w)))
        say("  decompiled %s -> %s" % (idc.get_func_name(w), path))
    except Exception as exc:
        say("  decompile %#x failed: %s" % (w, exc))

with open(os.path.join(OUT, "modtable.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
ida_pro.qexit(0)
