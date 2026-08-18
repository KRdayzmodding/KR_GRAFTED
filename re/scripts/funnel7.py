# IDAPython: последний кусок — примитив копирования строки в контейнере.
# array<string>.Insert/Set/Get все сводятся к sub_1403051A0.
import os

import ida_auto
import ida_funcs
import ida_hexrays
import ida_pro
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
lines = []


def say(s=""):
    print("[f7] %s" % s)
    lines.append(s)


def sz(ea):
    f = ida_funcs.get_func(ea)
    return (f.end_ea - f.start_ea) if f else 0


def dec(ea, cap=80):
    say("===== %s @ %#x (%d байт) =====" % (idc.get_func_name(ea), ea, sz(ea)))
    try:
        for ln in str(ida_hexrays.decompile(ea)).splitlines()[:cap]:
            say("  " + ln)
    except Exception as exc:
        say("  !! %s" % exc)
    say()


def callers(ea):
    out = set()
    for xr in idautils.XrefsTo(ea, 0):
        if xr.type in (16, 17, 19, 21):
            f = ida_funcs.get_func(xr.frm)
            out.add(f.start_ea if f else xr.frm)
    return out


for ea in (0x1403051A0, 0x1403017A0, 0x140305270, 0x1403024C0, 0x1403052E0):
    dec(ea, 70)
    cs = callers(ea)
    say("   вызывающих: %d" % len(cs))
    for c in sorted(cs)[:25]:
        say("      %-26s @ %#-12x размер %d" % (idc.get_func_name(c) or "?", c, sz(c)))
    say()

with open(os.path.join(OUT, "funnel7.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
ida_pro.qexit(0)
