# IDAPython: ВОПРОС 1 — сколько воронок копирования const char* в скриптовую строку.
#
# Отправная точка: sub_140367020 — «присвоить строку из char*» (найдено owned5.py).
# Здесь: что она делает внутри, кто её зовёт, и есть ли обходные пути (другой
# примитив, который копирует char* в строковое хранилище движка).
#
#   $env:RE_OUT=...; idat64 -A -S"funnel1.py" ...
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


def say(s=""):
    print("[f1] %s" % s)
    lines.append(s)


def dec(ea, cap=200, title=None):
    try:
        cf = ida_hexrays.decompile(ea)
    except Exception as exc:
        say("!! decompile %#x: %s" % (ea, exc))
        return ""
    txt = str(cf)
    say("===== %s @ %#x =====" % (title or idc.get_func_name(ea), ea))
    for ln in txt.splitlines()[:cap]:
        say("  " + ln)
    say()
    return txt


def callees(ea):
    """Прямые вызовы из функции ea."""
    out = []
    f = ida_funcs.get_func(ea)
    if not f:
        return out
    for item in idautils.FuncItems(f.start_ea):
        for xr in idautils.XrefsFrom(item, 0):
            if xr.type in (16, 17):  # fl_CN, fl_CF
                if xr.to not in out:
                    out.append(xr.to)
    return out


def callers(ea):
    out = {}
    for xr in idautils.XrefsTo(ea, 0):
        if xr.type not in (16, 17, 19, 21):  # call / jump-thunk
            continue
        f = ida_funcs.get_func(xr.frm)
        if f:
            out.setdefault(f.start_ea, []).append(xr.frm)
        else:
            out.setdefault(xr.frm, []).append(xr.frm)
    return out


ASSIGN = 0x140367020
GUARD = 0x1403673A0

say("################ 1. САМА ВОРОНКА ################")
say()
body = dec(ASSIGN, 220)

say("---- дизасм sub_140367020 (первые 90 инструкций) ----")
f = ida_funcs.get_func(ASSIGN)
n = 0
for item in idautils.FuncItems(ASSIGN):
    say("  %#012x  %s" % (item, idc.GetDisasm(item)))
    n += 1
    if n > 90:
        break
say()

say("---- её callees ----")
sub = callees(ASSIGN)
for c in sub:
    nm = idc.get_func_name(c) or ida_name.get_name(c) or "?"
    fn = ida_funcs.get_func(c)
    sz = (fn.end_ea - fn.start_ea) if fn else 0
    say("  %-30s @ %#-12x размер %d" % (nm, c, sz))
say()

say("---- разбор callees (компактные) ----")
for c in sub:
    fn = ida_funcs.get_func(c)
    if not fn:
        continue
    if fn.end_ea - fn.start_ea > 400:
        say("(пропуск %s @ %#x — %d байт)" % (idc.get_func_name(c), c, fn.end_ea - fn.start_ea))
        continue
    dec(c, 80)

say("################ 2. КТО ЗОВЁТ ВОРОНКУ ################")
say()
cs = callers(ASSIGN)
say("всего вызывающих функций: %d, всего call-сайтов: %d"
    % (len(cs), sum(len(v) for v in cs.values())))
for s in sorted(cs):
    say("  %-28s @ %#-12x  сайтов %d" % (idc.get_func_name(s) or "?", s, len(cs[s])))
say()

say("################ 3. ОХРАННИК sub_1403673A0 ################")
say()
gs = callers(GUARD)
say("вызывающих функций у охранника: %d, сайтов: %d"
    % (len(gs), sum(len(v) for v in gs.values())))
inter = sorted(set(gs) & set(cs))
only_guard = sorted(set(gs) - set(cs))
say("зовут И охранника И воронку: %d" % len(inter))
say("зовут охранника, но НЕ воронку (другой путь потребления?): %d" % len(only_guard))
for s in only_guard[:60]:
    fn = ida_funcs.get_func(s)
    sz = (fn.end_ea - fn.start_ea) if fn else 0
    say("  %-28s @ %#-12x размер %d" % (idc.get_func_name(s) or "?", s, sz))
say()

with open(os.path.join(OUT, "funnel1.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
ida_pro.qexit(0)
