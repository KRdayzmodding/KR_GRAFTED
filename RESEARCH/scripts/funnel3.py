# IDAPython: ВОПРОС 2 — точка покоя. И заодно: как интерпретатор обходится с
# возвратом натива (sub_140368880 — байткод-машина, 16 КБ).
#
# Якоря: sub_140345C90 ссылается на "enf_scriptcontext.cpp", sub_140365E10 — на
# "enf_scriptthread.cpp". От них поднимаемся к входу в скриптовую машину.
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
lines = []


def say(s=""):
    print("[f3] %s" % s)
    lines.append(s)


def sz(ea):
    f = ida_funcs.get_func(ea)
    return (f.end_ea - f.start_ea) if f else 0


def dec_to(ea, path):
    try:
        txt = str(ida_hexrays.decompile(ea))
    except Exception as exc:
        say("!! decompile %#x: %s" % (ea, exc))
        return None
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("// %s @ %#x  (%d байт)\n" % (idc.get_func_name(ea), ea, sz(ea)))
        fh.write(txt)
    say("   -> %s (%d строк)" % (path, len(txt.splitlines())))
    return txt


def dec(ea, cap=140):
    try:
        cf = ida_hexrays.decompile(ea)
    except Exception as exc:
        say("!! decompile %#x: %s" % (ea, exc))
        return
    say("===== %s @ %#x (%d байт) =====" % (idc.get_func_name(ea), ea, sz(ea)))
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


def show_callers(ea, label, limit=80):
    cs = callers(ea)
    say("---- вызывающие %s @ %#x : функций %d, сайтов %d ----"
        % (label, ea, len(cs), sum(len(v) for v in cs.values())))
    for s in sorted(cs)[:limit]:
        say("   %-28s @ %#-12x размер %-6d сайтов %d"
            % (idc.get_func_name(s) or "?", s, sz(s), len(cs[s])))
    if len(cs) > limit:
        say("   ... ещё %d" % (len(cs) - limit))
    say()
    return cs


VM = 0x140368880        # байткод-машина
THREAD = 0x140365E10    # enf_scriptthread.cpp
FRAME = 0x1403682E0     # 1102 байта, зовёт assign + free

say("################ A. enf_scriptthread.cpp ################")
say()
dec(THREAD, 120)
show_callers(THREAD, "sub_140365E10")

say("################ B. байткод-машина sub_140368880 ################")
say()
vm_txt = dec_to(VM, os.path.join(OUT, "vm.c"))
say("размер: %d байт" % sz(VM))
vmc = show_callers(VM, "sub_140368880")

say("################ C. кто зовёт вызывающих машины (2-й уровень) ################")
say()
for s in sorted(vmc):
    say("-- над %s @ %#x --" % (idc.get_func_name(s), s))
    for up in sorted(callers(s))[:40]:
        say("     %-28s @ %#-12x размер %d" % (idc.get_func_name(up) or "?", up, sz(up)))
    say()

say("################ D. sub_1403682E0 ################")
say()
dec_to(FRAME, os.path.join(OUT, "frame.c"))
dec(FRAME, 200)
show_callers(FRAME, "sub_1403682E0")

say("################ E. косвенные вызовы внутри машины ################")
say()
n = 0
for item in idautils.FuncItems(VM):
    m = idc.print_insn_mnem(item)
    if m == "call" and idc.get_operand_type(item, 0) not in (5, 6, 7):  # не прямой адрес
        say("  %#012x  %s" % (item, idc.GetDisasm(item)))
        n += 1
say("итого косвенных call: %d" % n)
say()

with open(os.path.join(OUT, "funnel3.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
ida_pro.qexit(0)
