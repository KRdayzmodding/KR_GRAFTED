# IDAPython: разведка по шаблонным скриптовым классам.
#   1) кто ещё зовёт линковщик proto (RE_LINKER) кроме RegisterMethod;
#   2) есть ли в образе строки-имена инстанцирований ("array<", "Link<", "<Class").
#
#   $env:RE_LINKER=0x140352DB0
#   .\re\re.ps1 run templates.py -On diag

import os

import ida_auto
import ida_bytes
import ida_funcs
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
LINKER = int(os.environ.get("RE_LINKER", "0x140352DB0"), 16)

ida_auto.auto_wait()

lines = []


def say(s):
    print("[tpl] %s" % s)
    lines.append(s)


say("linker %#x callers:" % LINKER)
for xref in idautils.CodeRefsTo(LINKER, 0):
    f = ida_funcs.get_func(xref)
    say("  from %#x in %s (func start %#x)"
        % (xref, idc.get_func_name(xref), f.start_ea if f else 0))

say("string literals with template markers:")
hits = 0
for s in idautils.Strings():
    t = str(s)
    if "<" in t and (
        t.startswith("array<") or t.startswith("set<") or t.startswith("map<")
        or t.startswith("Link<") or "<Class " in t or t.startswith("Scr::")
    ):
        say("  %#x  %r" % (s.ea, t))
        hits += 1
        if hits > 200:
            say("  ... truncated")
            break
say("total template-ish strings: %d" % hits)

with open(os.path.join(OUT, "templates.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))

import ida_pro

ida_pro.qexit(0)
