# IDAPython: кто создаёт базовые типы (kind) и какие kind реально ходят по switch'ам.
#
#   $env:RE_XREF = "0x1402CE5F0,0x1402C5030,0x1402CE080,0x1402C4610"
#   $env:RE_ADDRS = "0x1402C3900,..."
#   .\re\re.ps1 run tags2.py -On server

import os
import re
from collections import Counter

import ida_auto
import ida_funcs
import ida_hexrays
import ida_nalt
import ida_pro
import idaapi
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
PSEUDO = os.path.join(OUT, "pseudo")
ida_auto.auto_wait()
os.makedirs(PSEUDO, exist_ok=True)
have_hr = ida_hexrays.init_hexrays_plugin()
lines = []


def say(s):
    print("[t2] %s" % s)
    lines.append(s)


def safe(n):
    return re.sub(r"[^A-Za-z0-9_.-]", "_", n)[:120]


def dec(ea):
    fn = idc.get_func_name(ea)
    if not fn or not have_hr:
        return None
    p = os.path.join(PSEUDO, safe(fn) + ".c")
    if os.path.exists(p):
        return p
    try:
        cf = ida_hexrays.decompile(ea)
    except Exception as exc:
        say("decompile fail %#x: %s" % (ea, exc))
        return None
    with open(p, "w", encoding="utf-8") as fh:
        fh.write("// %s @ %#x\n%s\n" % (fn, ea, cf))
    return p


for raw in os.environ.get("RE_ADDRS", "").replace(" ", "").split(","):
    if raw:
        say("decomp %s -> %s" % (raw, dec(int(raw, 16))))

# ---- кто зовёт фабрики типов и с каким kind (третий аргумент, r8) ---------
for raw in os.environ.get("RE_XREF", "").replace(" ", "").split(","):
    if not raw:
        continue
    tgt = int(raw, 16)
    say("=== callers of %#x (%s) ===" % (tgt, idc.get_func_name(tgt) or "?"))
    for x in idautils.CodeRefsTo(tgt, 0):
        f = ida_funcs.get_func(x)
        # назад по 40 инструкций собираем последние присвоения в аргументные регистры
        regs = {}
        ea = x
        for _ in range(40):
            ea = idc.prev_head(ea, f.start_ea if f else x - 400)
            if ea == idc.BADADDR:
                break
            m = idc.print_insn_mnem(ea)
            if m in ("mov", "lea", "xor") and idc.get_operand_type(ea, 0) == idc.o_reg:
                r = idc.print_operand(ea, 0)
                if r not in regs:
                    op1 = idc.print_operand(ea, 1)
                    regs[r] = op1
        say("  call %#x in %s  | rcx=%s rdx=%s r8=%s r9=%s"
            % (x, idc.get_func_name(x) or "?", regs.get("rcx", regs.get("ecx", "?")),
               regs.get("rdx", regs.get("edx", "?")), regs.get("r8", regs.get("r8d", "?")),
               regs.get("r9", regs.get("r9d", "?"))))

# ---- все switch'и/сравнения после shr ,28 --------------------------------
kinds = Counter()
sw_funcs = []
for seg in idautils.Segments():
    if idc.get_segm_name(seg) != ".text":
        continue
    end = idc.get_segm_end(seg)
    ea = seg
    while ea != idc.BADADDR and ea < end:
        if idc.print_insn_mnem(ea) in ("shr", "sar") and idc.get_operand_type(ea, 1) == idc.o_imm \
                and idc.get_operand_value(ea, 1) == 28:
            cur = ea
            for _ in range(40):
                cur = idc.next_head(cur, end)
                if cur == idc.BADADDR:
                    break
                mn = idc.print_insn_mnem(cur)
                if mn in ("cmp", "sub") and idc.get_operand_type(cur, 1) == idc.o_imm:
                    v = idc.get_operand_value(cur, 1)
                    if v <= 31:
                        kinds[v] += 1
                if mn == "jmp":
                    si = idaapi.get_switch_info(cur)
                    if si:
                        f = ida_funcs.get_func(cur)
                        sw_funcs.append((f.start_ea if f else cur, si.ncases, si.lowcase))
                    break
        ea = idc.next_head(ea, end)

say("=== immediates compared after `shr ,28` ===")
for v, n in sorted(kinds.items()):
    say("  kind %2d : %d" % (v, n))
say("=== switch tables right after shr,28 ===")
for f, n, lo in sw_funcs:
    say("  %#x %s ncases=%d low=%d" % (f, idc.get_func_name(f) or "?", n, lo))

with open(os.path.join(OUT, "tags2.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
ida_pro.qexit(0)
