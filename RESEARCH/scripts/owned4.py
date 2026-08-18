# IDAPython: КТО ЧИТАЕТ `owned` В РАНТАЙМЕ — на этот раз честно.
#
# Прошлый заход искал `test r32, 800h` и нашёл ноль. Это плохой поиск: проверку ОДНОГО
# бита компилятор кодирует байтовой формой — `test byte ptr [reg+15h], 8` (бит 11 это
# бит 3 второго байта), а ещё бывают movzx+and, bt, and с 16-битным непосредственным.
# Здесь перебираются все эти формы, и отдельно — все читатели поля флагов вообще.
#
#   re.ps1 run owned4.py -On diag
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
    print("[o4] %s" % s)
    lines.append(s)


lo = hi = None
for i in range(ida_segment.get_segm_qty()):
    seg = ida_segment.getnseg(i)
    if ida_segment.get_segm_name(seg) == ".text":
        lo, hi = seg.start_ea, seg.end_ea
img = ida_bytes.get_bytes(lo, hi - lo) or b""
say("=== .text %#x..%#x ===" % (lo, hi))


def note(store, ea, what):
    fn = ida_funcs.get_func(ea)
    if fn:
        store.setdefault(fn.start_ea, []).append((ea, what))


# ── Форма 1: test byte ptr [reg + disp], imm8 — F6 /0 disp8 imm8 ─────────────
# Бит 11 (0x800) это бит 3 байта по смещению +1 от начала поля флагов.
# Поле флагов переменной = +0x14, значит интересен байт +0x15 с маской 8.
bit_forms = {}
for modrm in range(0x40, 0x48):          # [reg+disp8], /0
    for disp in (0x15, 0x16, 0x51, 0x52):  # флаги переменной (+0x14) и дескриптора (+0x50)
        needle = bytes([0xF6, modrm, disp, 0x08])
        at = img.find(needle)
        while at >= 0:
            note(bit_forms, lo + at, "test byte [r+%#x], 8" % disp)
            at = img.find(needle, at + 1)

# ── Форма 2: bt r/m32, imm8 — 0F BA /4 imm8, бит 11 ─────────────────────────
at = 0
while True:
    at = img.find(b"\x0f\xba", at + 1)
    if at < 0 or at + 4 > len(img):
        break
    if (img[at + 2] & 0x38) == 0x20 and img[at + 3] == 11:
        note(bit_forms, lo + at, "bt r/m, 11")

# ── Форма 3: непосредственное 0x800 любой ширины ────────────────────────────
for needle, what in ((b"\x00\x08\x00\x00", "imm32 0x800"), (b"\x00\x08", "imm16 0x800")):
    at = img.find(needle)
    while at >= 0:
        ea = lo + at
        if ida_funcs.get_func(ea) and idc.print_insn_mnem(idc.prev_head(ea)) in ("test", "and", "or", "xor", "cmp"):
            note(bit_forms, ea, what)
        at = img.find(needle, at + 1)

say("функций с проверкой бита 11 / 0x800: %d" % len(bit_forms))
for start, sites in sorted(bit_forms.items(), key=lambda kv: -len(kv[1]))[:20]:
    fn = ida_funcs.get_func(start)
    say("  %-22s @ %#-12x размер %-7d сайтов %d  (%s)"
        % (idc.get_func_name(start), start, fn.end_ea - fn.start_ea, len(sites),
           sites[0][1]))

# ── Все читатели поля флагов переменной (+0x14), чтобы видеть общую картину ──
readers = {}
for modrm_base in (0x40, 0x80):  # [reg+disp8], [reg+disp32]
    for op, kind in ((0x8B, "mov r32"), (0x0F, "movzx")):
        for reg in range(8):
            if op == 0x8B:
                needle = bytes([0x8B, modrm_base + reg, 0x14]) if modrm_base == 0x40 else None
            else:
                needle = bytes([0x0F, 0xB7, modrm_base + reg, 0x14]) if modrm_base == 0x40 else None
            if not needle:
                continue
            at = img.find(needle)
            while at >= 0:
                note(readers, lo + at, kind)
                at = img.find(needle, at + 1)
say("")
say("функций, читающих +0x14 (любой формой): %d" % len(readers))

# Пересечение: кто и читает флаги, и проверяет бит 11.
both = sorted(set(bit_forms) & set(readers))
say("и то и другое: %d — %s" % (len(both), ", ".join("%#x" % b for b in both[:10])))

for start in both[:4]:
    try:
        cfunc = ida_hexrays.decompile(start)
    except Exception as exc:
        say("!! %#x: %s" % (start, exc))
        continue
    say("")
    say("===== %s @ %#x =====" % (idc.get_func_name(start), start))
    for line in str(cfunc).splitlines()[:120]:
        say("  " + line)

with open(os.path.join(OUT, "owned4.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
ida_pro.qexit(0)
