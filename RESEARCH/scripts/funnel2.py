# IDAPython: ВОПРОС 1, часть 2 — полный набор мест, где движок КОПИРУЕТ char* в
# строковое хранилище, и якоря по именам исходников.
#
# Из funnel1: sub_140367020 -> sub_140347F80 (assign) -> sub_140345C90 (alloc+memcpy)
#                                                     -> sub_140348B50 (free)
#                                                     -> sub_140349000 (чей регион)
# sub_140345C90 — ЕДИНСТВЕННЫЙ аллокатор строковых блоков (в нём memcpy и __FILE__
# "enf_scriptcontext.cpp":2499). Значит его вызывающие — полный список копирований.
#
# Плюс: дамп всех строк «*.cpp» из .rdata и функций, которые на них ссылаются —
# это якоря вместо RVA.
import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_name
import ida_pro
import ida_segment
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
lines = []
files = []


def say(s=""):
    print("[f2] %s" % s)
    lines.append(s)


def dec(ea, cap=120, title=None):
    try:
        cf = ida_hexrays.decompile(ea)
    except Exception as exc:
        say("!! decompile %#x: %s" % (ea, exc))
        return
    say("===== %s @ %#x =====" % (title or idc.get_func_name(ea), ea))
    for ln in str(cf).splitlines()[:cap]:
        say("  " + ln)
    say()


def callers(ea):
    out = {}
    for xr in idautils.XrefsTo(ea, 0):
        if xr.type not in (16, 17, 19, 21):
            continue
        f = ida_funcs.get_func(xr.frm)
        key = f.start_ea if f else xr.frm
        out.setdefault(key, []).append(xr.frm)
    return out


ALLOC = 0x140345C90   # блок строки + memcpy
ASSIGN2 = 0x140347F80  # var <- char*
FREE = 0x140348B50
REGION = 0x140349000
FUNNEL = 0x140367020

for nm, ea in (("sub_140345C90 (alloc+copy)", ALLOC),
               ("sub_140347F80 (assign var<-char*)", ASSIGN2),
               ("sub_140348B50 (free block)", FREE),
               ("sub_140349000 (какому региону принадлежит ptr)", REGION)):
    cs = callers(ea)
    say("######## вызывающие %s ########" % nm)
    say("функций: %d, сайтов: %d" % (len(cs), sum(len(v) for v in cs.values())))
    for s in sorted(cs):
        f = ida_funcs.get_func(s)
        sz = (f.end_ea - f.start_ea) if f else 0
        mark = ""
        if s == FUNNEL:
            mark = "   <-- ВОРОНКА sub_140367020"
        say("  %-28s @ %#-12x размер %-6d сайтов %d%s"
            % (idc.get_func_name(s) or "?", s, sz, len(cs[s]), mark))
    say()

say("######## разбор вызывающих sub_140345C90 (кроме assign) ########")
for s in sorted(callers(ALLOC)):
    if s in (ASSIGN2,):
        continue
    f = ida_funcs.get_func(s)
    if f and f.end_ea - f.start_ea <= 700:
        dec(s, 90)
    else:
        say("(крупная: %s @ %#x, %d байт)" % (idc.get_func_name(s), s, (f.end_ea - f.start_ea) if f else 0))
say()

say("######## разбор вызывающих sub_140347F80 (кроме воронки) ########")
for s in sorted(callers(ASSIGN2)):
    if s == FUNNEL:
        continue
    f = ida_funcs.get_func(s)
    if f and f.end_ea - f.start_ea <= 900:
        dec(s, 110)
    else:
        say("(крупная: %s @ %#x, %d байт)" % (idc.get_func_name(s), s, (f.end_ea - f.start_ea) if f else 0))
say()

# ---- ЯКОРЯ: строки исходников ----
seen = {}
for s in idautils.Strings():
    txt = str(s)
    low = txt.lower()
    if not (low.endswith(".cpp") or low.endswith(".c") or low.endswith(".h")):
        continue
    base = txt.replace("\\", "/").rsplit("/", 1)[-1]
    refs = []
    for xr in idautils.XrefsTo(s.ea, 0):
        f = ida_funcs.get_func(xr.frm)
        if f:
            refs.append(f.start_ea)
    if refs:
        seen.setdefault(base, set()).update(refs)

files.append("### файлы исходников движка как якоря (имя -> сколько функций ссылается) ###")
for base in sorted(seen, key=lambda k: -len(seen[k])):
    files.append("%-46s функций %d" % (base, len(seen[base])))

files.append("")
files.append("### функции, ссылающиеся на скриптовые файлы (полный список) ###")
for base in sorted(seen):
    if "script" not in base.lower() and "enf_" not in base.lower():
        continue
    files.append("")
    files.append("-- %s --" % base)
    for fa in sorted(seen[base]):
        f = ida_funcs.get_func(fa)
        files.append("   %-28s @ %#-12x размер %d"
                     % (idc.get_func_name(fa) or "?", fa, f.end_ea - f.start_ea))

with open(os.path.join(OUT, "funnel2.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
with open(os.path.join(OUT, "srcfiles.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(files))
ida_pro.qexit(0)
