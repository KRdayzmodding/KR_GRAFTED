# IDAPython: как ОТЛИЧИТЬ аллокатор строк от соседа, ссылающегося на тот же литерал,
# и каким байтовым признаком его брать в scan.cpp без единого зашитого адреса.
#
#   re.ps1 run salloc2.py -On diag
import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_pro
import ida_segment
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
lines = []


def say(s):
    print("[sa2] %s" % s)
    lines.append(s)


# Сосед по якорю — чем он занят.
try:
    cf = ida_hexrays.decompile(0x140345BD0)
    say("===== СОСЕД sub_140345BD0 =====")
    for line in str(cf).splitlines()[:60]:
        say("  " + line)
except Exception as exc:
    say("!! сосед не декомпилируется: %s" % exc)

# Байты обеих функций вокруг ссылки на литерал — ищем разделяющий признак.
say("")
say("===== ПРИЗНАКИ =====")
for ea in (0x140345BD0, 0x140345C90):
    fn = ida_funcs.get_func(ea)
    body = ida_bytes.get_bytes(fn.start_ea, fn.end_ea - fn.start_ea) or b""
    say("%s @ %#x размер %d" % (idc.get_func_name(ea), ea, len(body)))
    # смещение 736 (0x2E0) — база фрилистов строк
    say("   содержит 0x2E0 (736, база фрилистов): %s" % (b"\xe0\x02\x00\x00" in body))
    say("   содержит 0x360 (864, счётчики):       %s" % (b"\x60\x03\x00\x00" in body))
    say("   вызовов наружу: %d" % sum(1 for i in idautils.FuncItems(ea)
                                      if idc.print_insn_mnem(i) == "call"))
    say("   первые 32 байта: %s" % body[:32].hex())

# Кто читает глобал контекста и как его достать: разбор 50-байтной обёртки.
say("")
say("===== ОБЁРТКА sub_140367020 (из неё берётся адрес глобала контекста) =====")
fn = ida_funcs.get_func(0x140367020)
if fn:
    say("размер %d" % (fn.end_ea - fn.start_ea))
    for i in idautils.FuncItems(0x140367020):
        say("  %#x  %s" % (i, idc.GetDisasm(i)))

# Кто ещё читает этот глобал — чтобы понять, устойчив ли он как якорь.
say("")
say("===== ГЛОБАЛ КОНТЕКСТА =====")
GLOBAL = 0x1412702B0
refs = sorted({ida_funcs.get_func(x.frm).start_ea
               for x in idautils.XrefsTo(GLOBAL, 0) if ida_funcs.get_func(x.frm)})
say("qword_1412702B0: читающих/пишущих функций %d" % len(refs))

# А совпадает ли он с ctx, который приходит в RegisterGlobal? Ищем, откуда его пишут.
say("")
say("===== КТО ПИШЕТ ГЛОБАЛ =====")
for x in idautils.XrefsTo(GLOBAL, 0):
    if idc.print_insn_mnem(x.frm) == "mov" and idc.get_operand_type(x.frm, 0) == idc.o_mem:
        fn2 = ida_funcs.get_func(x.frm)
        say("  запись @ %#x в %s: %s" % (x.frm,
                                          idc.get_func_name(fn2.start_ea) if fn2 else "?",
                                          idc.GetDisasm(x.frm)))

with open(os.path.join(OUT, "salloc2.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
ida_pro.qexit(0)
