# IDAPython: где в компиляторе живут ШАБЛОННЫЕ ОБЪЯВЛЕНИЯ.
#   .\re\re.ps1 run tplgen.py -On diag
#
# Ищем строки по ТЕКСТУ (устойчиво к сборке), тянем функции-владельцы,
# декомпилируем их и их вызывателей.

import os

import ida_auto
import ida_funcs
import ida_hexrays
import ida_pro
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
PSEUDO = os.path.join(OUT, "pseudo")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
os.makedirs(PSEUDO, exist_ok=True)

lines = []


def say(s):
    print("[tg] %s" % s)
    lines.append(str(s))


WANT = [
    "Wrong number of template parameters",
    "Can't compile template class",
    "Template error",
    "while compiling template class",
    "autoptr/ref in template is not supported",
    "Prototype error",
    "Native functions don't support",
    "cannot be modded",
    "can't be modded",
    "Multiple declaration of function",
    "Static array of 'typename'",
    "Unknown type '%s'",
    "Syntax error",
    "Expected '%s' or '%s'",
    "Invalid identifier",
    "linked as static/external",
    "Linking non-existing",
    "Destructor",
    "destructor",
    "private",
]

# 1) строки -> адреса
found = {}
for s in idautils.Strings():
    t = str(s)
    for w in WANT:
        if w in t:
            found.setdefault(w, []).append((s.ea, t))

say("=== matched strings ===")
owners = {}
for w in WANT:
    for ea, t in found.get(w, [])[:12]:
        refs = list(idautils.DataRefsTo(ea))
        say("%-42s %#x %r  refs=%d" % (w[:42], ea, t[:90], len(refs)))
        for r in refs:
            f = ida_funcs.get_func(r)
            if f:
                owners.setdefault(f.start_ea, set()).add(t[:70])

say("")
say("=== owner functions (%d) ===" % len(owners))
for ea in sorted(owners):
    say("%#x %-16s :: %s" % (ea, idc.get_func_name(ea), " | ".join(sorted(owners[ea]))[:220]))


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
    say("wrote pseudo/%s.c  (%d bytes)  %s" % (name, len(text), tag))


say("")
say("=== decompiling owners + callers ===")
todo = set(owners)
for ea in sorted(owners):
    for x in idautils.CodeRefsTo(ea, 0):
        cf = ida_funcs.get_func(x)
        if cf:
            todo.add(cf.start_ea)

for ea in sorted(todo):
    dump(ea, "owner/caller")

say("")
say("=== callers map ===")
for ea in sorted(owners):
    cs = set()
    for x in idautils.CodeRefsTo(ea, 0):
        cf = ida_funcs.get_func(x)
        if cf:
            cs.add(cf.start_ea)
    say("%#x <- %s" % (ea, ", ".join("%#x" % c for c in sorted(cs)) or "<none>"))

with open(os.path.join(OUT, "tplgen.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
