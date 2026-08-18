# IDAPython: словарь диагностик компилятора Enforce.
#
#   1) находит логгер ошибок компиляции по строке-маяку
#      "Multiple declaration of function '%s'" (lea ... -> ближайший call);
#   2) обходит ВСЕ вызовы логгера и вытаскивает формат-строку каждого;
#   3) отдельно печатает строки образа по ключевым словам (шаблоны/типы/перегрузки).
#
#   .\re\re.ps1 run errvocab.py -On diag

import os
import re

import ida_auto
import ida_funcs
import ida_pro
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
ANCHOR = os.environ.get("RE_ANCHOR", "Multiple declaration of function")

ida_auto.auto_wait()

lines = []


def say(s):
    print("[err] %s" % s)
    lines.append(s)


def str_at(ea):
    for t in (idc.STRTYPE_C, idc.STRTYPE_C_16):
        s = idc.get_strlit_contents(ea, -1, t)
        if s:
            try:
                return s.decode("utf-8", "replace")
            except Exception:
                return str(s)
    return None


# --- 1. логгер -------------------------------------------------------------
anchor_ea = None
for s in idautils.Strings():
    if ANCHOR in str(s):
        anchor_ea = s.ea
        break
if anchor_ea is None:
    say("anchor string not found: %r" % ANCHOR)
    ida_pro.qexit(1)
say("anchor %r @ %#x" % (ANCHOR, anchor_ea))

logger = None
for xref in idautils.DataRefsTo(anchor_ea):
    ea = xref
    for _ in range(12):
        ea = idc.next_head(ea)
        if idc.print_insn_mnem(ea) == "call":
            tgt = idc.get_operand_value(ea, 0)
            if idc.get_operand_type(ea, 0) == idc.o_near:
                logger = tgt
                say("logger = %#x (from %#x)" % (logger, xref))
                break
    if logger:
        break

if logger is None:
    say("logger not found")
    ida_pro.qexit(1)

# --- 2. все диагностики ----------------------------------------------------
say("")
say("=== callers of logger %#x ===" % logger)
seen = set()
rows = []
for xref in idautils.CodeRefsTo(logger, 0):
    f = ida_funcs.get_func(xref)
    fname = idc.get_func_name(xref) or "?"
    fmt = None
    ea = xref
    for _ in range(24):
        ea = idc.prev_head(ea)
        if ea == idc.BADADDR:
            break
        m = idc.print_insn_mnem(ea)
        if m == "lea":
            op = idc.get_operand_value(ea, 1)
            t = str_at(op)
            if t and len(t) > 3:
                fmt = t
                break
        if m == "call":
            break
    key = (fname, fmt)
    if key in seen:
        continue
    seen.add(key)
    rows.append((fname, xref, fmt))

rows.sort(key=lambda r: (r[0], r[1]))
for fname, xref, fmt in rows:
    say("%-16s %#x  %r" % (fname, xref, fmt))
say("total diagnostics: %d" % len(rows))

# --- 3. интересные строки образа -------------------------------------------
KEYS = [
    "template", "Template", "generic", "Generic", "instanti",
    "overload", "Overload", "Ambiguous", "ambiguous",
    "Unknown type", "unknown type", "Undefined", "undefined type",
    "convert", "Convert", "cast", "Cast",
    "typename", "Typename", "Class ", " Class",
    "parameter", "Parameter", "argument", "Argument",
    "Prototype", "proto", "native", "Native",
    "not a type", "expected", "Expected",
    "Sealed", "modded", "Modded", "inherit", "Inherit", "base class",
]
say("")
say("=== interesting strings ===")
hits = 0
for s in idautils.Strings():
    t = str(s)
    if len(t) < 5 or len(t) > 160:
        continue
    if any(k in t for k in KEYS):
        say("%#x  %r" % (s.ea, t))
        hits += 1
        if hits > 900:
            say("... truncated")
            break
say("total interesting: %d" % hits)

with open(os.path.join(OUT, "errvocab.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))

ida_pro.qexit(0)
