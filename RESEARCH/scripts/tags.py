# IDAPython: перепись ТЕГОВ типов скриптовой переменной Enforce.
#
#   $env:RE_ADDRS = "0x1402C5030,0x1402C4610"   # что декомпилировать
#   .\re\re.ps1 run tags.py -On server
#
# 1) гистограмма `and 0F0000000h` + ближайший `cmp imm` — какие теги реально сравнивают
# 2) сайты `shr reg, 1Ch` — где тег раскладывают в switch по kind
# 3) декомпиляция адресов из RE_ADDRS

import os
import re
from collections import Counter

import ida_auto
import ida_funcs
import ida_hexrays
import ida_pro
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
PSEUDO = os.path.join(OUT, "pseudo")
ida_auto.auto_wait()
os.makedirs(PSEUDO, exist_ok=True)
have_hr = ida_hexrays.init_hexrays_plugin()

lines = []


def say(s):
    print("[tag] %s" % s)
    lines.append(s)


def safe(name):
    return re.sub(r"[^A-Za-z0-9_.-]", "_", name)[:120]


def decompile_to_file(ea):
    fn = idc.get_func_name(ea)
    if not fn or not have_hr:
        return None
    path = os.path.join(PSEUDO, safe(fn) + ".c")
    if os.path.exists(path):
        return path
    try:
        cf = ida_hexrays.decompile(ea)
    except Exception as exc:
        say("decompile failed %#x: %s" % (ea, exc))
        return None
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("// %s @ %#x\n%s\n" % (fn, ea, cf))
    return path


for raw in os.environ.get("RE_ADDRS", "").replace(" ", "").split(","):
    if raw:
        say("decomp %s -> %s" % (raw, decompile_to_file(int(raw, 16))))

# ---- скан .text -----------------------------------------------------------
cmp_hist = Counter()          # значение тега -> сколько сравнений
cmp_sites = {}                # значение -> [адреса функций]
shr28 = Counter()
mask_only = 0

for seg in idautils.Segments():
    if idc.get_segm_name(seg) != ".text":
        continue
    end = idc.get_segm_end(seg)
    ea = seg
    while ea != idc.BADADDR and ea < end:
        m = idc.print_insn_mnem(ea)
        if m == "and" and idc.get_operand_type(ea, 1) == idc.o_imm \
                and (idc.get_operand_value(ea, 1) & 0xFFFFFFFF) == 0xF0000000:
            mask_only += 1
            nxt = ea
            for _ in range(14):
                nxt = idc.next_head(nxt, end)
                if nxt == idc.BADADDR:
                    break
                m2 = idc.print_insn_mnem(nxt)
                if m2 in ("cmp", "sub", "xor", "mov") and idc.get_operand_type(nxt, 1) == idc.o_imm:
                    val = idc.get_operand_value(nxt, 1) & 0xFFFFFFFF
                    if val & 0x0FFFFFFF == 0:      # только 0xN0000000
                        cmp_hist[val] += 1
                        f = ida_funcs.get_func(ea)
                        cmp_sites.setdefault(val, []).append(f.start_ea if f else ea)
                    break
                if m2 in ("jmp", "ret", "call"):
                    break
        elif m in ("shr", "sar") and idc.get_operand_type(ea, 1) == idc.o_imm \
                and idc.get_operand_value(ea, 1) == 28:
            f = ida_funcs.get_func(ea)
            shr28[f.start_ea if f else ea] += 1
        ea = idc.next_head(ea, end)

say("=== mask 0xF0000000 sites: %d ===" % mask_only)
say("=== compared tag values ===")
for val, n in sorted(cmp_hist.items()):
    fs = sorted(set(cmp_sites[val]))
    say("  %#010x  kind=%2d  cmp=%4d  funcs=%d  e.g. %s"
        % (val, val >> 28, n, len(fs), ", ".join("%#x" % a for a in fs[:6])))

say("=== shr/sar ,28 (kind switch candidates), top 40 ===")
for ea, n in shr28.most_common(40):
    say("  %#x  %s  x%d" % (ea, idc.get_func_name(ea) or "?", n))

with open(os.path.join(OUT, "tags.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
print("[tag] done")
ida_pro.qexit(0)
