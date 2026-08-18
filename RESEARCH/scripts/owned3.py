# IDAPython: `owned` доезжает до ФЛАГОВ ПЕРЕМЕННОЙ (+20, бит 0x800) — это видно в
# компиляторе (sub_14033E9F0). Вопрос: читает ли кто-нибудь этот бит в РАНТАЙМЕ и
# освобождает ли по нему память. Если да — движок забирает наш указатель себе, и
# библиотеке хранить его незачем. Если нет — он только копирует, и арена обязана жить.
#
#   .\re\re.ps1 run owned3.py -On diag
import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_pro
import ida_segment
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
lines = []


def say(s):
    print("[o3] %s" % s)
    lines.append(s)


lo = hi = None
for i in range(ida_segment.get_segm_qty()):
    seg = ida_segment.getnseg(i)
    if ida_segment.get_segm_name(seg) == ".text":
        lo, hi = seg.start_ea, seg.end_ea
img = ida_bytes.get_bytes(lo, hi - lo) or b""

# Чтение флагов переменной по +20 (0x14) с последующей проверкой 0x800.
# mov eax,[rXX+14h]  = 8B 4? 14   затем test/and с 00 08 00 00 неподалёку.
found = {}
at = 0
while True:
    at = img.find(b"\x14", at + 1)
    if at < 0 or at + 24 > len(img):
        break
    if at < 2:
        continue
    if img[at - 2] != 0x8B:
        continue
    window = img[at : at + 24]
    if b"\x00\x08\x00\x00" not in window:
        continue
    ea = lo + at - 2
    fn = ida_funcs.get_func(ea)
    if fn:
        found.setdefault(fn.start_ea, []).append(ea)

say("функций, читающих +0x14 и рядом 0x800: %d" % len(found))
for start, sites in sorted(found.items()):
    fn = ida_funcs.get_func(start)
    say("  %s @ %#x  размер %d  сайтов %d"
        % (idc.get_func_name(start), start, fn.end_ea - fn.start_ea, len(sites)))

for start in sorted(found, key=lambda s: ida_funcs.get_func(s).end_ea - ida_funcs.get_func(s).start_ea)[:5]:
    try:
        cfunc = ida_hexrays.decompile(start)
    except Exception as exc:
        say("!! %#x: %s" % (start, exc))
        continue
    say("")
    say("===== %s @ %#x =====" % (idc.get_func_name(start), start))
    for line in str(cfunc).splitlines():
        say("  " + line)

with open(os.path.join(OUT, "owned3.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
ida_pro.qexit(0)
