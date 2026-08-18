# IDAPython: грубый поиск ссылок на адреса (когда IDA не сделала xref).
#   $env:RE_TARGETS = "0x140e2aa98,0x140e2b1d0"
#   $env:RE_RANGE   = "0x140240000-0x140380000"   (по умолчанию весь .text)

import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_pro
import ida_segment
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()

targets = set()
for raw in os.environ.get("RE_TARGETS", "").replace(" ", "").split(","):
    if raw:
        targets.add(int(raw, 16))

rng = os.environ.get("RE_RANGE", "")
if rng:
    lo, hi = [int(x, 16) for x in rng.split("-")]
else:
    seg = ida_segment.get_segm_by_name(".text")
    lo, hi = seg.start_ea, seg.end_ea

print("[ref] scan %#x..%#x for %d targets" % (lo, hi, len(targets)))

ea = lo
n = 0
while ea < hi and ea != idc.BADADDR:
    if ida_bytes.is_code(ida_bytes.get_full_flags(ea)):
        for i in range(3):
            t = idc.get_operand_type(ea, i)
            if t in (idc.o_imm, idc.o_mem, idc.o_displ, idc.o_near, idc.o_far):
                v = idc.get_operand_value(ea, i)
                if v in targets:
                    print("[ref] %#x  %-40s  in %s"
                          % (ea, idc.GetDisasm(ea), idc.get_func_name(ea) or "?"))
                    n += 1
    ea = idc.next_head(ea, hi)

print("[ref] hits=%d" % n)
ida_pro.qexit(0)
