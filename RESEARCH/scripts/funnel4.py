# IDAPython: (A) куда девается возврат натива — не попадает ли наш указатель в
# стек выражения как «временная строка движка», которую движок потом ОСВОБОДИТ.
# (B) ВОПРОС 2: точка покоя — входы в байткод-машину и кадровый цикл, по якорям.
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
    print("[f4] %s" % s)
    lines.append(s)


def sz(ea):
    f = ida_funcs.get_func(ea)
    return (f.end_ea - f.start_ea) if f else 0


def dec(ea, cap=160):
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


def tree(ea, depth, seen=None, pad=""):
    if seen is None:
        seen = set()
    if depth == 0 or ea in seen:
        return
    seen.add(ea)
    for up in sorted(callers(ea)):
        say("%s%-28s @ %#-12x размер %d" % (pad, idc.get_func_name(up) or "?", up, sz(up)))
        tree(up, depth - 1, seen, pad + "   ")


say("################ A. подготовка аргументов / возврат натива ################")
say()
for ea in (0x1403663B0, 0x140366160, 0x140347740, 0x14034ED10, 0x140350620, 0x140367AD0):
    dec(ea, 150)

say("################ B1. входы в байткод-машину, вверх по стеку ################")
say()
for ea in (0x140348870, 0x140348960, 0x140365FD0):
    say("---- вход %s @ %#x (%d байт) ----" % (idc.get_func_name(ea), ea, sz(ea)))
    dec(ea, 120)
    say("   вызывающие (3 уровня):")
    tree(ea, 3, pad="     ")
    say()

say("################ B2. якоря-строки для кадрового цикла ################")
say()
WANT = ("OnUpdate", "OnFrame", "MissionGameplay", "CallQueue", "EndFrame", "BeginFrame",
        "World::", "Simulate", "GameUpdate", "MainLoop", "Frame", "Tick", "OnRPC",
        "UpdateInputDevices", "ScriptCallQueue", "EnScript", "ScriptModule",
        "CGame", "DayZGame", "Mission", "PreloadEvent")
hits = {}
for s in idautils.Strings():
    txt = str(s)
    if len(txt) > 80:
        continue
    for w in WANT:
        if w in txt:
            refs = set()
            for xr in idautils.XrefsTo(s.ea, 0):
                f = ida_funcs.get_func(xr.frm)
                if f:
                    refs.add(f.start_ea)
            if refs:
                hits.setdefault(txt, set()).update(refs)
            break
for txt in sorted(hits):
    say('  "%s"' % txt)
    for fa in sorted(hits[txt])[:6]:
        say("       %-28s @ %#-12x размер %d" % (idc.get_func_name(fa) or "?", fa, sz(fa)))
say()

with open(os.path.join(OUT, "funnel4.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
ida_pro.qexit(0)
