# IDAPython: путь вызова шаблонного метода коллекции.
#   1) все функции-регистраторы нативов -> out/<t>/regs/*.c (ищем array/map/set)
#   2) байт-скан: кто тестирует флаг native (+0x50, бит 0x20)
#   3) строки интерпретатора (кандидаты на обработчик вызова)
#
#   .\re\re.ps1 run tplcall.py -On diag

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
REG_METHOD = int(os.environ.get("RE_REGM", "0x14034B5F0"), 16)
REG_GLOBAL = int(os.environ.get("RE_REGG", "0x14034B6A0"), 16)

ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()

os.makedirs(os.path.join(OUT, "regs"), exist_ok=True)
lines = []


def say(s):
    print("[tpl] %s" % s)
    lines.append(str(s))


def dump(ea, sub):
    name = idc.get_func_name(ea)
    try:
        text = str(ida_hexrays.decompile(ea))
    except Exception as exc:
        say("decompile %#x failed: %s" % (ea, exc))
        return ""
    with open(os.path.join(OUT, sub, name + ".c"), "w", encoding="utf-8") as fh:
        fh.write("// %s @ %#x\n" % (name, ea))
        fh.write(text)
    return text


# ── 1. все регистраторы ──────────────────────────────────────────────────────
registrars = set()
for target in (REG_METHOD, REG_GLOBAL):
    for xref in idautils.CodeRefsTo(target, 0):
        fn = idc.get_func_attr(xref, idc.FUNCATTR_START)
        if fn != idc.BADADDR:
            registrars.add(fn)
say("registrars: %d" % len(registrars))

MARK = ("array", "\"map\"", "\"set\"", "Resize", "Reserve", "InsertAt", "GetKey", "GetElement")
for fn in sorted(registrars):
    text = dump(fn, "regs")
    hit = [m for m in MARK if m in text]
    if hit:
        say("  REG %#x %s -> %s" % (fn, idc.get_func_name(fn), hit))

# ── 2. байт-скан ─────────────────────────────────────────────────────────────
segs = []
for i in range(ida_segment.get_segm_qty()):
    s = ida_segment.getnseg(i)
    if s.perm & ida_segment.SEGPERM_EXEC:
        segs.append((s.start_ea, s.end_ea))
say("code segs: %s" % [("%#x" % a, "%#x" % b) for a, b in segs])

pats = []
for rm in (0, 1, 2, 3, 5, 6, 7):
    m = bytes([0x40 | rm])
    pats.append(b"\xf6" + m + b"\x50\x20")
    pats.append(b"\xf7" + m + b"\x50\x20\x00\x00\x00")
    pats.append(b"\x41\xf6" + m + b"\x50\x20")
    pats.append(b"\x41\xf7" + m + b"\x50\x20\x00\x00\x00")

found = {}
for lo, hi in segs:
    img = ida_bytes.get_bytes(lo, hi - lo) or b""
    for pat in pats:
        pos = img.find(pat)
        while pos != -1:
            ea = lo + pos
            if idc.print_insn_mnem(ea) == "test":
                f = ida_funcs.get_func(ea)
                key = f.start_ea if f else 0
                found.setdefault(key, []).append((ea, idc.GetDisasm(ea)))
            pos = img.find(pat, pos + 1)

say("test [reg+0x50],0x20 : %d funcs" % len(found))
for fn in sorted(found):
    say("  FLG %#x %s (%d)" % (fn, idc.get_func_name(fn), len(found[fn])))
    for ea, dis in found[fn][:3]:
        say("        %#x  %s" % (ea, dis))

# ── 3. строки интерпретатора ─────────────────────────────────────────────────
KEY = ("enf_script", "Stack overflow", "Call stack", "instruction", "Opcode",
       "opcode", "Unknown function", "Null pointer", "NULL pointer",
       "Cannot call", "Can't call", "Native", "native", "Invalid instance",
       "Instance of", "scriptthread", "ScriptThread", "Function '")
for s in idautils.Strings():
    t = str(s)
    if any(k in t for k in KEY) and len(t) < 120:
        refs = [idc.get_func_name(x) for x in idautils.DataRefsTo(s.ea)]
        say("  STR %#x %r  <- %s" % (s.ea, t, sorted(set(refs))[:6]))

with open(os.path.join(OUT, "tplcall.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
