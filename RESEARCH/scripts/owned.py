# IDAPython: кто в рантайме читает бит `owned` (0x800 в маске возврата) и что делает с
# указателем, который вернул натив.
#
# Вопрос, ради которого это пишется: обязана ли библиотека держать возвращённую строку
# живой сама (арена), или движок забирает её себе. Эксперимент в сьюте уже показал, что
# движок ДЕРЖИТ наш указатель до потребления; здесь ищем, копирует ли он его в итоге и
# не освобождает ли — от этого зависит, можно ли отдавать ему память, которую он потом
# сам и отпустит.
#
#   .\re\re.ps1 run owned.py -On diag
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
lines = []


def say(s):
    print("[owned] %s" % s)
    lines.append(s)


text_lo = text_hi = None
for i in range(ida_segment.get_segm_qty()):
    seg = ida_segment.getnseg(i)
    if ida_segment.get_segm_name(seg) == ".text":
        text_lo, text_hi = seg.start_ea, seg.end_ea
img = ida_bytes.get_bytes(text_lo, text_hi - text_lo) or b""
say("=== .text %#x..%#x (%d байт) ===" % (text_lo, text_hi, len(img)))

# Инструкции, проверяющие 0x800: test/and с непосредственным 00 08 00 00.
NEEDLES = [
    (b"\xa9\x00\x08\x00\x00", "test eax, 800h"),
    (b"\x25\x00\x08\x00\x00", "and eax, 800h"),
    (b"\xf7\xc0\x00\x08\x00\x00", "test eax, 800h (long)"),
]
hits = {}
for needle, what in NEEDLES:
    at = img.find(needle)
    while at >= 0:
        ea = text_lo + at
        fn = ida_funcs.get_func(ea)
        if fn:
            hits.setdefault(fn.start_ea, []).append((ea, what))
        at = img.find(needle, at + 1)

# То же для остальных регистров: F7 /0 imm32 (test r/m32, imm32) и 81 /4 (and).
for op, kind in ((0xF7, "test"), (0x81, "and")):
    for reg in range(0xC0, 0xC8):
        modrm = reg if op == 0xF7 else reg + 0x20
        needle = bytes([op, modrm, 0x00, 0x08, 0x00, 0x00])
        at = img.find(needle)
        while at >= 0:
            ea = text_lo + at
            fn = ida_funcs.get_func(ea)
            if fn:
                hits.setdefault(fn.start_ea, []).append((ea, "%s r%d, 800h" % (kind, reg - 0xC0)))
            at = img.find(needle, at + 1)

say("функций, читающих 0x800: %d" % len(hits))

# Из них интересны те, что рядом читают и +0x50 (флаги дескриптора) или +8 (impl).
for start in sorted(hits):
    fn = ida_funcs.get_func(start)
    name = idc.get_func_name(start)
    body = ida_bytes.get_bytes(fn.start_ea, fn.end_ea - fn.start_ea) or b""
    marks = []
    if b"\x50\x00\x00\x00" in body or b"\x8b\x40\x50" in body or b"\x8b\x41\x50" in body:
        marks.append("читает +0x50")
    calls = sum(1 for _ in idautils.CodeRefsFrom(start, 0))
    say("%s @ %#x  размер %d  %s  сайтов 0x800: %d"
        % (name, start, fn.end_ea - fn.start_ea, ",".join(marks) or "-", len(hits[start])))

# Декомпилируем самые компактные — большие это скорее компилятор, а не рантайм вызова.
ida_hexrays.init_hexrays_plugin()
picked = sorted(hits, key=lambda s: ida_funcs.get_func(s).end_ea - ida_funcs.get_func(s).start_ea)
for start in picked[:6]:
    try:
        cfunc = ida_hexrays.decompile(start)
    except Exception as exc:
        say("!! не декомпилируется %#x: %s" % (start, exc))
        continue
    say("")
    say("===== %s @ %#x =====" % (idc.get_func_name(start), start))
    for line in str(cfunc).splitlines():
        say("  " + line)

with open(os.path.join(OUT, "owned.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
ida_pro.qexit(0)
