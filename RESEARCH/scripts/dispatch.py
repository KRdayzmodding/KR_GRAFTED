# IDAPython: пересечение двух признаков диспетчера вызова натива —
#   (а) читает флаги дескриптора (+0x50) и проверяет биты 0x20/0x40/0x60,
#   (б) делает косвенный вызов через impl (+8).
#   .\re\re.ps1 run dispatch.py -On server
# Результат: re\out\<target>\dispatch.txt + pseudo\<func>.c

import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_pro
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()

lines = []
hits = []

for fn_ea in idautils.Functions():
    f = ida_funcs.get_func(fn_ea)
    if not f or f.end_ea - f.start_ea > 0x6000:
        continue
    read50 = False
    icall8 = False
    flagbits = set()
    ea = f.start_ea
    pending = 0
    while ea < f.end_ea and ea != idc.BADADDR:
        mnem = idc.print_insn_mnem(ea)
        op0 = idc.print_operand(ea, 0)
        op1 = idc.print_operand(ea, 1)
        if "+50h]" in op0 or "+50h]" in op1:
            pending = 8
        elif pending:
            pending -= 1
        if mnem in ("test", "and", "or") and pending:
            for op in (op1, op0):
                v = idc.get_operand_value(ea, 1)
            v = idc.get_operand_value(ea, 1)
            if v in (0x20, 0x40, 0x60, 0x460):
                read50 = True
                flagbits.add(v)
        if mnem == "call" and ("+8]" in op0) and op0.startswith("qword"):
            icall8 = True
        ea = idc.next_head(ea, f.end_ea)
    if read50 and icall8:
        hits.append(fn_ea)
        lines.append("%#x %s  bits=%s" % (fn_ea, idc.get_func_name(fn_ea),
                                          ",".join(hex(b) for b in sorted(flagbits))))
        print("[dp] " + lines[-1])

pseudo = os.path.join(OUT, "pseudo")
try:
    os.makedirs(pseudo)
except OSError:
    pass
for fn_ea in hits:
    name = idc.get_func_name(fn_ea)
    if not name.startswith("sub_"):
        continue
    try:
        text = str(ida_hexrays.decompile(fn_ea))
    except Exception as exc:
        print("[dp] %s decompile failed: %s" % (name, exc))
        continue
    with open(os.path.join(pseudo, name + ".c"), "w", encoding="utf-8") as fh:
        fh.write("// %s @ %#x  native-call dispatcher candidate\n" % (name, fn_ea))
        fh.write(text)

with open(os.path.join(OUT, "dispatch.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
print("[dp] total %d" % len(hits))
ida_pro.qexit(0)
