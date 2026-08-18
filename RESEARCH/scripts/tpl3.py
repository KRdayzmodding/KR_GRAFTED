# IDAPython: шаг 3 — маршалинг нативного вызова и фабрика инстанциаций шаблона.
#   .\re\re.ps1 run tpl3.py -On diag

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
    print("[t3] %s" % s)
    lines.append(str(s))


def pseudo(ea, tag=""):
    try:
        text = str(ida_hexrays.decompile(ea))
    except Exception as exc:
        say("decompile %#x failed: %s" % (ea, exc))
        return
    name = idc.get_func_name(ea)
    with open(os.path.join(OUT, "pseudo", name + ".c"), "w", encoding="utf-8") as fh:
        fh.write("// %s @ %#x  %s\n" % (name, ea, tag))
        fh.write(text)
    say("")
    say("######## %s @ %#x  %s" % (name, ea, tag))
    say(text)


def asm(ea, tag=""):
    f = ida_funcs.get_func(ea)
    if not f:
        say("no func at %#x" % ea)
        return
    say("")
    say("######## ASM %s @ %#x  %s  [%d bytes]" % (idc.get_func_name(ea), ea, tag, f.end_ea - f.start_ea))
    cur = f.start_ea
    while cur < f.end_ea:
        say("   %016X  %s" % (cur, idc.GetDisasm(cur)))
        cur = idc.next_head(cur, f.end_ea)


TARGETS = [
    (0x140367C10, "NATIVE INVOKE (desc, this, args)"),
    (0x1403672C0, "arg var accessor used by thunks"),
    (0x140366C60, "used by map.Insert"),
    (0x1403673A0, "used by thunks after call"),
    (0x140367280, "type of var"),
    (0x140367E30, "arg slot address"),
    (0x14030BA50, "BaseArrayClass vtbl+0x60"),
    (0x14030C0B0, "BaseMapClass vtbl+0x60"),
    (0x14030D5A0, "BaseSetClass vtbl+0x60"),
    (0x140352010, "FindClass by name (used by C++)"),
]
for ea, tag in TARGETS:
    pseudo(ea, tag)

asm(0x140367C10, "NATIVE INVOKE")
asm(0x1403672C0, "arg var")

say("")
say("=== callers of NATIVE INVOKE 0x140367C10 ===")
for xref in idautils.CodeRefsTo(0x140367C10, 0):
    say("  %#x in %s" % (xref, idc.get_func_name(xref)))

say("")
say("=== callers of 0x140367F40 (CallNative wrapper) ===")
for xref in idautils.CodeRefsTo(0x140367F40, 0):
    say("  %#x in %s" % (xref, idc.get_func_name(xref)))

# ── vtables инстанциаций ─────────────────────────────────────────────────────
say("")
say("=== instantiation vtables (ArrayT / MapT / SetT) ===")
picked = []
for ea, name in idautils.Names():
    dn = ida_name.demangle_name(name, 0) or name
    if "vftable" not in dn:
        continue
    if "ArrayT<" in dn or "SetT<" in dn or ("MapT<" in dn and "MapClassT" not in dn):
        picked.append((ea, dn))
say("found %d" % len(picked))
for ea, dn in picked[:6]:
    say("")
    say("--- %s @ %#x" % (dn, ea))
    for i in range(0, 24):
        v = ida_bytes.get_qword(ea + i * 8)
        if not ida_funcs.get_func(v):
            break
        say("   +%#04x [%2d]  %#x  %s" % (i * 8, i, v, idc.get_func_name(v)))

say("")
say("=== all ArrayT/SetT RTTI names ===")
n = 0
for s in idautils.Strings():
    t = str(s)
    if t.startswith(".?AV?$ArrayT@") or t.startswith(".?AV?$SetT@") or t.startswith(".?AV?$ArrayClassT@") or t.startswith(".?AV?$SetClassT@"):
        say("%#x  %s" % (s.ea, t))
        n += 1
say("count=%d" % n)

with open(os.path.join(OUT, "tpl3.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
