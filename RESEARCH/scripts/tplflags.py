# IDAPython: чем шаблонный метод коллекции отличается от нешаблонного.
#
#   .\re\re.ps1 run tplflags.py -On server
#
# Что делает:
#  1) ищет тело-диспетчер шаблонного метода (jmp qword ptr [r9+68h]) — все такие функции;
#  2) для каждой — дизасм + все xref'ы (кто ставит их адрес в дескриптор);
#  3) сканирует .text на запись/модификацию поля +0x50 (флаги дескриптора функции)
#     константой: mov/or/and/test [reg+50h], imm — это и есть места, где биты ставятся;
#  4) ищет обращения к полю компилятора +0x170A0 (текущие модификаторы объявления);
#  5) выгружает строки про шаблоны/инстанцирование и функции-владельцы.

import os
import struct

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_nalt
import ida_pro
import ida_segment
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
lines = []


def say(s):
    print("[tpl] %s" % s)
    lines.append(s)


ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()

# ── образ ────────────────────────────────────────────────────────────────────
text_lo = text_hi = None
for i in range(ida_segment.get_segm_qty()):
    s = ida_segment.getnseg(i)
    if ida_segment.get_segm_name(s) == ".text":
        text_lo, text_hi = s.start_ea, s.end_ea
img = ida_bytes.get_bytes(text_lo, text_hi - text_lo) or b""
say("text %#x..%#x (%d bytes)" % (text_lo, text_hi, len(img)))


def fname(ea):
    f = ida_funcs.get_func(ea)
    return (idc.get_func_name(ea), f.start_ea) if f else ("?", 0)


def dump_func(ea, path):
    f = ida_funcs.get_func(ea)
    if not f:
        return
    with open(path, "w", encoding="utf-8") as fh:
        cur = f.start_ea
        while cur < f.end_ea:
            fh.write("%016X  %-30s %s\n" % (cur, ida_bytes.get_bytes(cur, idc.next_head(cur, f.end_ea) - cur).hex(), idc.GetDisasm(cur)))
            cur = idc.next_head(cur, f.end_ea)


os.makedirs(os.path.join(OUT, "asm"), exist_ok=True)

# ── 1. диспетчеры шаблонных методов ──────────────────────────────────────────
say("")
say("=== jmp qword ptr [r9+XX] (диспетчер из кадра) ===")
thunks = set()
for pat, desc in ((b"\x41\xff\x61", "jmp [r9+disp8]"), (b"\x41\xff\xa1", "jmp [r9+disp32]")):
    pos = img.find(pat)
    while pos != -1:
        ea = text_lo + pos
        disp = img[pos + 3] if pat.endswith(b"\x61") else struct.unpack_from("<i", img, pos + 3)[0]
        n, start = fname(ea)
        if start:
            thunks.add(start)
        say("  %#x  %s disp=%#x  in %s (start %#x)" % (ea, desc, disp, n, start))
        pos = img.find(pat, pos + 1)

say("")
say("=== xref'ы на эти функции ===")
for t in sorted(thunks):
    say("  --- %s @ %#x" % (idc.get_func_name(t), t))
    dump_func(t, os.path.join(OUT, "asm", "%s.asm" % idc.get_func_name(t)))
    for x in idautils.XrefsTo(t, 0):
        n, start = fname(x.frm)
        say("      from %#x type=%d in %s (start %#x)" % (x.frm, x.type, n, start))

# ── 2. кто трогает +0x50 у дескриптора константой ────────────────────────────
say("")
say("=== запись/модификация [reg+50h] константой ===")


def modrm_reg_ok(b):
    # mod=01 (disp8), rm != 100 (нет SIB) — [reg+disp8]
    return (b & 0xC0) == 0x40 and (b & 0x07) != 4


seen = {}
i = 0
n = len(img)
while i < n - 8:
    op = img[i]
    if op in (0xC7, 0x81, 0x83, 0x09, 0x21, 0x85, 0xF7):
        m = img[i + 1]
        if modrm_reg_ok(m) and img[i + 2] == 0x50:
            ea = text_lo + i
            reg_field = (m >> 3) & 7
            if op == 0xC7 and reg_field == 0:
                val = struct.unpack_from("<I", img, i + 3)[0]
                kind = "mov"
            elif op == 0x81:
                val = struct.unpack_from("<I", img, i + 3)[0]
                kind = {1: "or", 4: "and", 6: "xor", 7: "cmp", 0: "add", 5: "sub"}.get(reg_field, "?%d" % reg_field)
            elif op == 0x83:
                val = img[i + 3]
                if val > 127:
                    val -= 256
                val &= 0xFFFFFFFF
                kind = {1: "or", 4: "and", 6: "xor", 7: "cmp", 0: "add", 5: "sub"}.get(reg_field, "?%d" % reg_field)
            elif op == 0xF7 and reg_field == 0:
                val = struct.unpack_from("<I", img, i + 3)[0]
                kind = "test"
            else:
                i += 1
                continue
            nm, start = fname(ea)
            key = (kind, val)
            seen.setdefault(key, []).append((ea, nm))
    i += 1

for (kind, val), places in sorted(seen.items(), key=lambda kv: (kv[0][0], kv[0][1])):
    say("  %-5s [x+50h], %#010x   (%d мест)" % (kind, val, len(places)))
    for ea, nm in places[:12]:
        say("        %#x  %s" % (ea, nm))

# ── 3. поле компилятора +0x170A0 (модификаторы объявления) ───────────────────
say("")
say("=== обращения к +0x170A0 / +0x170E0 (модификаторы и тип в парсере) ===")
for off, label in ((0x170A0, "modifiers"), (0x170C8, "cur class"), (0x170E0, "ret type")):
    pat = struct.pack("<I", off)
    pos = img.find(pat)
    hits = 0
    while pos != -1 and hits < 40:
        ea = text_lo + pos
        nm, start = fname(ea)
        if start:
            say("  %s %#x in %s (start %#x)" % (label, ea, nm, start))
            hits += 1
        pos = img.find(pat, pos + 1)

# ── 4. строки про шаблоны ────────────────────────────────────────────────────
say("")
say("=== строки про шаблоны/инстанцирование ===")
for s in idautils.Strings():
    t = str(s)
    low = t.lower()
    if any(k in low for k in ("templ", "generic", "instanti", "type parameter", "unknown type", "cannot be modded", "sealed")):
        say("  %#x  %r" % (s.ea, t))
        for x in idautils.XrefsTo(s.ea, 0):
            nm, start = fname(x.frm)
            say("        xref %#x in %s" % (x.frm, nm))

with open(os.path.join(OUT, "tplflags.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("written %s" % os.path.join(OUT, "tplflags.txt"))
ida_pro.qexit(0)
