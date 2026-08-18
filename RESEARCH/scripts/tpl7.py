# IDAPython: шаг 7 — сигнатуры виртуальных методов контейнера + ArrayT vtable.
#   .\re\re.ps1 run tpl7.py -On diag

import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_name
import ida_pro
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
os.makedirs(os.path.join(OUT, "pseudo"), exist_ok=True)

lines = []


def say(s):
    print("[t7] %s" % s)
    lines.append(str(s))


def pseudo(ea, tag=""):
    try:
        text = str(ida_hexrays.decompile(ea))
    except Exception as exc:
        say("decompile %#x failed: %s" % (ea, exc))
        return
    say("")
    say("######## %s @ %#x  %s" % (idc.get_func_name(ea), ea, tag))
    say(text)


for ea, tag in [
    (0x140319D30, "MapT::Count  (vtbl+0x28)"),
    (0x140320350, "MapT<int,int>::Insert (vtbl+0x70)"),
    (0x14031A650, "MapT<int,int>::Get/Find (vtbl+0x58)"),
    (0x14031DC40, "MapT<int,int> vtbl+0x60"),
    (0x140C355E0, "purecall?"),
    (0x140316670, "MapT<int,int>::Clear (vtbl+0x30)"),
]:
    pseudo(ea, tag)

# массивные инстанциации: таблицы классов, которые ставит фабрика
say("")
say("=== array instantiation class vtables ===")
for name in ("off_140E24FA0", "off_140E250E8", "off_140E25230", "off_140E25378",
             "off_140E254C0", "off_140E25608", "off_140E25680", "off_140E257C8"):
    ea = idc.get_name_ea_simple(name)
    if ea == idc.BADADDR:
        say("%s: not found" % name)
        continue
    say("%s @ %#x" % (name, ea))

# найти vtable инстанциаций массива через RTTI-имена ArrayT
say("")
say("=== ArrayT vtables via names ===")
for ea, nm in idautils.Names():
    dn = ida_name.demangle_name(nm, 0) or nm
    if "vftable" in dn and "ArrayT<" in dn:
        say("%#x  %s" % (ea, dn))
        for i in range(0, 21):
            v = ida_bytes.get_qword(ea + i * 8)
            if not ida_funcs.get_func(v):
                break
            say("   +%#04x [%2d]  %#x  %s" % (i * 8, i, v, idc.get_func_name(v)))
        say("")

with open(os.path.join(OUT, "tpl7.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
