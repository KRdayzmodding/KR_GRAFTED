# IDAPython: шаг 4 — примитив присваивания строки (raw char* -> движковая строка) и релиз.
#   idat64 -A -S mar4.py <idb>

import os

import ida_auto
import ida_funcs
import ida_hexrays
import ida_pro
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()

lines = []


def say(s):
    print("[m4] %s" % s)
    lines.append(str(s))


def pseudo(ea, tag=""):
    if ea in (0, idc.BADADDR) or not ida_funcs.get_func(ea):
        say("!! no func at %#x (%s)" % (ea, tag))
        return
    say("")
    say("######## %s @ %#x  %s" % (idc.get_func_name(ea), ea, tag))
    try:
        say(str(ida_hexrays.decompile(ea)))
    except Exception as exc:
        say("  decompile failed: %s" % exc)


for ea, tag in [
    (0x140347EC0, "string assign primitive (ctx, char*, dst, flag)"),
    (0x140348B50, "string release (ctx, char*)"),
    (0x140349000, "current string pool?"),
    (0x140305180, "assign string to typed slot"),
    (0x140302C60, "string cmp a"),
    (0x140302C90, "string cmp b"),
    (0x140307E30, "map<string,int> instance ctor"),
    (0x14031E710, "MapT<TString,TString>::Begin"),
    (0x14031E940, "MapT<TString,TString>::End"),
    (0x14031ECB0, "MapT<TString,TString>::Next"),
    (0x14031EAA0, "MapT<TString,TString>::GetIteratorKey"),
    (0x14031E830, "MapT<TString,TString>::GetIteratorElement"),
    (0x140320B80, "MapT<TString,TString>::Insert"),
    (0x14031F1F0, "MapT<TString,TString>::GetKey(i)"),
    (0x140348F20, "field address helper"),
    (0x1403483C0, "script error"),
]:
    pseudo(ea, tag)

with open(os.path.join(OUT, "mar4.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
