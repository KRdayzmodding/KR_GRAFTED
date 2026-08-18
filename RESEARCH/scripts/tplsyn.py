# IDAPython: СИНТАКСИС — таблица ключевых слов и парсер объявления класса.
#   .\re\re.ps1 run tplsyn.py -On diag

import os

import ida_auto
import ida_bytes
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
    print("[ts] %s" % s)
    lines.append(str(s))


KW = ["class", "modded", "sealed", "proto", "native", "typename", "ref", "autoptr",
      "private", "protected", "static", "const", "out", "inout", "notnull", "owned",
      "volatile", "external", "override", "event", "enum", "typedef", "extends",
      "reference", "local", "new", "delete", "super", "this", "NULL", "null",
      "void", "int", "float", "bool", "string", "vector", "func", "thread", "auto",
      "Class", "Managed", "template", "public"]

# 1) точные строки-ключевые слова и их xref'ы
say("=== keyword strings (exact match) ===")
byname = {}
for s in idautils.Strings():
    t = str(s)
    if t in KW:
        byname.setdefault(t, []).append(s.ea)

owners = {}
for k in KW:
    for ea in byname.get(k, []):
        refs = list(idautils.DataRefsTo(ea))
        if not refs:
            continue
        fl = []
        for r in refs:
            f = ida_funcs.get_func(r)
            if f:
                fl.append(f.start_ea)
                owners.setdefault(f.start_ea, set()).add(k)
        say("%-12s %#x  refs=%d  in %s" % (k, ea, len(refs),
                                           ", ".join(sorted(set("%#x" % x for x in fl)))))

say("")
say("=== функции, где сходится много ключевых слов (кандидат в таблицу/парсер) ===")
for ea in sorted(owners, key=lambda e: -len(owners[e])):
    if len(owners[ea]) >= 2:
        say("%#x %-16s (%2d) %s" % (ea, idc.get_func_name(ea), len(owners[ea]),
                                    " ".join(sorted(owners[ea]))))

# 2) кто пишет node+72 (список параметров шаблона) — ищем в парсере
say("")
say("=== mov [reg+48h], reg  (node+72) в 0x14035E000..0x140366000 ===")
lo, hi = 0x14035E000, 0x140366000
ea = lo
while ea < hi:
    d = idc.GetDisasm(ea)
    if d.startswith("mov") and "+48h]," in d and "[r" in d:
        f = ida_funcs.get_func(ea)
        say("  %#x  %-46s  %s" % (ea, d, idc.get_func_name(ea) if f else "?"))
    n = idc.next_head(ea, hi)
    if n <= ea:
        break
    ea = n


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
say("=== decompile: парсер + шаблонная машинерия ===")
for ea, tag in [
    (0x140330F00, "compile instantiation (…while compiling template class)"),
    (0x140332820, "copy members template -> instantiation"),
    (0x140336F80, "?"),
    (0x1403392E0, "compile class body"),
    (0x14033E9F0, "proto/native link (out args, static/external)"),
    (0x1403413B0, "parse type/param"),
    (0x140333670, "FindType by name"),
    (0x14032F020, "resolve type node (template-aware)"),
    (0x140340e70, "?"),
    (0x1403634E0, "parser: statement/decl"),
    (0x140361AE0, "parser: brackets"),
    (0x1403611B0, "parser: expect"),
    (0x140362B50, "parser"),
    (0x140363080, "parser"),
    (0x140363FA0, "parser"),
    (0x140364370, "parser"),
    (0x140364230, "parser"),
    (0x140364770, "parser"),
    (0x140364A90, "parser"),
    (0x14035E1A0, "modded/sealed"),
]:
    dump(ea, tag)

# 3) все функции, ссылающиеся на строку 'class'
say("")
say("=== xrefs to 'class'/'modded' strings, decompiled ===")
for k in ("class", "modded", "sealed", "proto", "native"):
    for ea in byname.get(k, []):
        for r in idautils.DataRefsTo(ea):
            f = ida_funcs.get_func(r)
            if f:
                dump(f.start_ea, "refs '%s'" % k)

with open(os.path.join(OUT, "tplsyn.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
