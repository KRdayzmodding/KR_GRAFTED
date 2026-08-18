# IDAPython: добить ВОПРОС 2 — кадровый путь до планировщика скриптов sub_140365FD0,
# и подтвердить, что косвенные вызовы натива лежат ВНУТРИ sub_140368880 (значит адрес
# возврата из нашего же тханка — готовый якорь вместо RVA).
import os

import ida_auto
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
    print("[f5] %s" % s)
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
    out = set()
    for xr in idautils.XrefsTo(ea, 0):
        if xr.type not in (16, 17, 19, 21):
            continue
        f = ida_funcs.get_func(xr.frm)
        out.add(f.start_ea if f else xr.frm)
    return out


def tree(ea, depth, pad="  ", seen=None):
    if seen is None:
        seen = set()
    if depth == 0 or ea in seen:
        return
    seen.add(ea)
    for up in sorted(callers(ea)):
        say("%s%-28s @ %#-12x размер %d" % (pad, idc.get_func_name(up) or "?", up, sz(up)))
        tree(up, depth - 1, pad + "   ", seen)


VM = 0x140368880
VM_END = ida_funcs.get_func(VM).end_ea

say("################ 1. путь до планировщика скриптов ################")
say()
for ea in (0x140306930, 0x140AD1060):
    dec(ea, 140 if ea == 0x140306930 else 0)
    say("вызывающие %s @ %#x:" % (idc.get_func_name(ea), ea))
    tree(ea, 4)
    say()

say("################ 2. вызовы натива внутри машины ################")
say()
say("sub_140368880 занимает %#x..%#x" % (VM, VM_END))
for item in idautils.FuncItems(VM):
    if idc.print_insn_mnem(item) != "call":
        continue
    if idc.get_operand_type(item, 0) in (5, 6, 7):
        continue
    say("  косвенный call @ %#x: %s" % (item, idc.GetDisasm(item)))
    say("    -- контекст --")
    at = item
    for _ in range(6):
        at = idc.prev_head(at)
    for _ in range(9):
        say("      %#012x  %s" % (at, idc.GetDisasm(at)))
        at = idc.next_head(at, VM_END)
    say()

say("################ 3. re-entry: sub_140349630 ################")
say()
dec(0x140349630, 120)
say("вызывающие sub_140349630:")
tree(0x140349630, 2)
say()

say("################ 4. кто ещё пишет qword_1412702B0 ################")
say()
G = 0x1412702B0
for xr in idautils.XrefsTo(G, 0):
    f = ida_funcs.get_func(xr.frm)
    mnem = idc.print_insn_mnem(xr.frm)
    if mnem == "mov" and idc.get_operand_type(xr.frm, 0) == 2:  # mov [mem], reg
        say("  ЗАПИСЬ @ %#x в %-24s : %s"
            % (xr.frm, idc.get_func_name(xr.frm) if f else "?", idc.GetDisasm(xr.frm)))
say()

with open(os.path.join(OUT, "funnel5.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
ida_pro.qexit(0)
