# IDAPython: таблица модификаторов (слово -> биты) + добор псевдокода.
#   .\re\re.ps1 run tplflag2.py -On diag

import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_pro
import idc

OUT = os.environ.get("RE_OUT", ".")
PSEUDO = os.path.join(OUT, "pseudo")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
os.makedirs(PSEUDO, exist_ok=True)

lines = []


def say(s):
    print("[f2] %s" % s)
    lines.append(str(s))


ORDER = ["volatile", "private", "event", "native", "owned", "out", "inout", "notnull",
         "local", "autoptr", "ref", "static", "const", "reference", "protected",
         "external", "override", "sealed"]

BASE = 0x1411277D0
say("=== таблица модификаторов @ %#x (word, maskA(ret), maskB(|= ctx+94368)) ===" % BASE)
for i in range(0, 20):
    w = ida_bytes.get_qword(BASE + 24 * i)
    a = ida_bytes.get_qword(BASE + 24 * i + 8)
    b = ida_bytes.get_qword(BASE + 24 * i + 16)
    nm = ORDER[i] if i < len(ORDER) else "?"
    say("[%2d] %-10s word=%#018x  maskA=%#010x  maskB=%#010x" % (i, nm, w, a & 0xFFFFFFFF, b & 0xFFFFFFFF))

# вторая таблица — модификаторы метода (sub_140365150) и параметра (sub_140365320)
for nm, ea in (("method-mods sub_140365150", 0x140365150),
               ("param-mods  sub_140365320", 0x140365320),
               ("class-mods  sub_140365120", 0x140365120)):
    say("")
    say("=== ASM %s @ %#x ===" % (nm, ea))
    f = ida_funcs.get_func(ea)
    if not f:
        say("  no func")
        continue
    cur = f.start_ea
    while cur < f.end_ea:
        say("   %016X  %s" % (cur, idc.GetDisasm(cur)))
        cur = idc.next_head(cur, f.end_ea)


def dump(ea, tag=""):
    f = ida_funcs.get_func(ea)
    if not f:
        say("no func @ %#x" % ea)
        return
    name = idc.get_func_name(f.start_ea)
    try:
        text = str(ida_hexrays.decompile(f.start_ea))
    except Exception as exc:
        say("decompile %#x failed: %s" % (ea, exc))
        return
    with open(os.path.join(PSEUDO, name + ".c"), "w", encoding="utf-8") as fh:
        fh.write("// %s @ %#x  %s\n%s" % (name, f.start_ea, tag, text))
    say("wrote pseudo/%s.c  %s" % (name, tag))


say("")
for ea, tag in [
    (0x14032F9B0, "destructor name checks"),
    (0x14033DED0, "function decl (private/protected, override)"),
    (0x140362B50, "parser: function/member decl"),
    (0x1403634E0, "parser: decl list dispatcher"),
    (0x140363FA0, "parser: TypeRef (ref/autoptr/<>)"),
    (0x140332AC0, "find existing function"),
    (0x14032E150, "create Funct"),
    (0x140332820, "copy template members"),
    (0x14033E610, "member function compile"),
    (0x140333810, "find member"),
    (0x14035E1A0, "modded/sealed apply"),
    (0x1403392E0, "compile class body"),
    (0x140336D10, "resolve return type"),
    (0x140365150, "method modifier scan"),
    (0x140365320, "param modifier scan"),
    (0x140365120, "class modifier scan"),
    (0x14032F020, "resolve type node"),
]:
    dump(ea, tag)

with open(os.path.join(OUT, "tplflag2.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
