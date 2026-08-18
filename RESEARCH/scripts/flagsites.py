# IDAPython: все места, где ЧИТАЮТ поле +0x50 (флаги дескриптора) и сразу
# проверяют биты — чтобы найти, кто в рантайме решает, как звать натив.
#   .\re\re.ps1 run flagsites.py -On server

import os
import struct

import ida_auto
import ida_bytes
import ida_funcs
import ida_pro
import ida_segment
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
lines = []


def say(s):
    print("[fs] %s" % s)
    lines.append(s)


lo = hi = None
for i in range(ida_segment.get_segm_qty()):
    s = ida_segment.getnseg(i)
    if ida_segment.get_segm_name(s) == ".text":
        lo, hi = s.start_ea, s.end_ea
img = ida_bytes.get_bytes(lo, hi - lo) or b""

WANT = (0x20, 0x40, 0x60, 0x400, 0x460, 0x800, 0x4, 0x4000, 0x100, 0x200)

i = 0
n = len(img)
hits = 0
while i < n - 8:
    # mov r32, [reg+50h]  = 8B /r mod=01 disp8=0x50
    if img[i] == 0x8B and (img[i + 1] & 0xC0) == 0x40 and (img[i + 1] & 7) != 4 and img[i + 2] == 0x50:
        ea = lo + i
        text = []
        cur = ea
        for _ in range(6):
            text.append(idc.GetDisasm(cur))
            cur = idc.next_head(cur, ea + 48)
            if cur == idc.BADADDR or cur >= ea + 48:
                break
        blob = " | ".join(text)
        low = blob.lower()
        if any(("20h" in low or "40h" in low or "460h" in low or "60h" in low) for _ in (0,)) and (
            "test" in low or "and " in low or " bt " in low or "shr" in low
        ):
            f = ida_funcs.get_func(ea)
            say("%#x  %s  ::  %s" % (ea, idc.get_func_name(ea), blob))
            hits += 1
    i += 1

say("total %d" % hits)
with open(os.path.join(OUT, "flagsites.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
ida_pro.qexit(0)
