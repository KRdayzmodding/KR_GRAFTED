# IDAPython: путь ОСВОБОЖДЕНИЯ строковой скриптовой переменной — и спрашивает ли он
# про `owned` (бит 0x800 флагов переменной, +0x14).
#
# Грепать константу 0x800 бессмысленно: её в бинаре тысячи. Ищем по надёжной подписи —
# функции, которые маскируют тег переменной (+0x10) через 0xF0000000 и сравнивают с
# 0x50000000 (семейство string). Там и только там решается судьба указателя на текст.
#
#   re.ps1 run owned5.py -On diag
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
    print("[o5] %s" % s)
    lines.append(s)


lo = hi = None
for i in range(ida_segment.get_segm_qty()):
    seg = ida_segment.getnseg(i)
    if ida_segment.get_segm_name(seg) == ".text":
        lo, hi = seg.start_ea, seg.end_ea
img = ida_bytes.get_bytes(lo, hi - lo) or b""

MASK = b"\x00\x00\x00\xf0"      # 0xF0000000 little-endian
STRING = b"\x00\x00\x00\x50"    # 0x50000000

cands = {}
at = 0
while True:
    at = img.find(MASK, at + 1)
    if at < 0:
        break
    # рядом (в пределах 64 байт) должно быть сравнение с семейством string
    window = img[max(0, at - 32) : at + 64]
    if STRING not in window:
        continue
    fn = ida_funcs.get_func(lo + at)
    if fn:
        cands.setdefault(fn.start_ea, 0)
        cands[fn.start_ea] += 1

say("функций, различающих семейство string по тегу: %d" % len(cands))

# Из них интересны те, что читают флаги переменной (+0x14) — там решается `owned`.
def reads_flags(start):
    fn = ida_funcs.get_func(start)
    body = ida_bytes.get_bytes(fn.start_ea, fn.end_ea - fn.start_ea) or b""
    for modrm in range(0x40, 0x48):
        if bytes([0x8B, modrm, 0x14]) in body:
            return True
        if bytes([0xF6, modrm, 0x15]) in body:   # байтовая проверка бита в старшей половине
            return True
        if bytes([0x0F, 0xB6, modrm, 0x15]) in body:
            return True
    return False


picked = [s for s in cands if reads_flags(s)]
say("из них читают флаги переменной (+0x14/+0x15): %d" % len(picked))
for s in sorted(picked, key=lambda x: ida_funcs.get_func(x).end_ea - ida_funcs.get_func(x).start_ea):
    fn = ida_funcs.get_func(s)
    say("  %-22s @ %#-12x размер %d" % (idc.get_func_name(s), s, fn.end_ea - fn.start_ea))

# Самые компактные — это и есть служебные операции над переменной (release/assign).
for start in sorted(picked, key=lambda x: ida_funcs.get_func(x).end_ea - ida_funcs.get_func(x).start_ea)[:5]:
    try:
        cfunc = ida_hexrays.decompile(start)
    except Exception as exc:
        say("!! %#x: %s" % (start, exc))
        continue
    say("")
    say("===== %s @ %#x =====" % (idc.get_func_name(start), start))
    for line in str(cfunc).splitlines()[:90]:
        say("  " + line)

# И отдельно — самые компактные из всех кандидатов, даже если флаги не читают:
# отсутствие проверки `owned` в них само по себе ответ.
say("")
say("### компактные кандидаты БЕЗ чтения флагов (если освобождают безусловно — ответ) ###")
short = sorted((s for s in cands if s not in picked),
               key=lambda x: ida_funcs.get_func(x).end_ea - ida_funcs.get_func(x).start_ea)[:3]
for start in short:
    try:
        cfunc = ida_hexrays.decompile(start)
    except Exception as exc:
        say("!! %#x: %s" % (start, exc))
        continue
    say("")
    say("===== %s @ %#x =====" % (idc.get_func_name(start), start))
    for line in str(cfunc).splitlines()[:70]:
        say("  " + line)

with open(os.path.join(OUT, "owned5.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
ida_pro.qexit(0)
