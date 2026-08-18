# IDAPython: найти регистрацию натива по ИМЕНИ и вытащить адрес его impl.
#   $env:RE_NAMES = "GetHourMinuteSecond,GetPlayers"
#   .\re\re.ps1 run byname.py -On diag
# Печатает строки регистратора, где встречается имя (там же виден impl).

import os
import struct

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_pro
import ida_segment
import idautils
import idc

ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()

lo = hi = None
for i in range(ida_segment.get_segm_qty()):
    s = ida_segment.getnseg(i)
    lo = s.start_ea if lo is None else min(lo, s.start_ea)
    hi = s.end_ea if hi is None else max(hi, s.end_ea)
img = ida_bytes.get_bytes(lo, hi - lo) or b""

for name in os.environ.get("RE_NAMES", "").split(","):
    name = name.strip()
    if not name:
        continue
    needle = name.encode() + b"\x00"
    pos = img.find(needle)
    found = []
    while pos != -1:
        if pos == 0 or img[pos - 1] == 0:
            found.append(lo + pos)
        pos = img.find(needle, pos + 1)
    print("[bn] %s: %d strings %s" % (name, len(found), ["%#x" % f for f in found]))
    funcs = set()
    for ea in found:
        for xref in idautils.DataRefsTo(ea):
            f = ida_funcs.get_func(xref)
            if f:
                funcs.add(f.start_ea)
    for fn in sorted(funcs):
        print("[bn]   in %s (%#x)" % (idc.get_func_name(fn), fn))
        try:
            text = str(ida_hexrays.decompile(fn))
        except Exception as exc:
            print("[bn]     decompile failed: %s" % exc)
            continue
        for line in text.splitlines():
            if '"%s"' % name in line:
                print("[bn]     %s" % line.strip())

ida_pro.qexit(0)
