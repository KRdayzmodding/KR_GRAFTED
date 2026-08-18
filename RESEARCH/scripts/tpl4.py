# IDAPython: шаг 4 — второй (маршалируемый) путь вызова и линковщик коллекций.
#   .\re\re.ps1 run tpl4.py -On diag

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
os.makedirs(os.path.join(OUT, "pseudo"), exist_ok=True)

lines = []


def say(s):
    print("[t4] %s" % s)
    lines.append(str(s))


def pseudo(ea, tag=""):
    try:
        text = str(ida_hexrays.decompile(ea))
    except Exception as exc:
        say("decompile %#x failed: %s" % (ea, exc))
        return
    name = idc.get_func_name(ea)
    with open(os.path.join(OUT, "pseudo", name + ".c"), "w", encoding="utf-8") as fh:
        fh.write("// %s @ %#x  %s\n" % (name, ea, tag))
        fh.write(text)
    say("")
    say("######## %s @ %#x  %s" % (name, ea, tag))
    say(text)


def asm(ea, tag="", limit=400):
    f = ida_funcs.get_func(ea)
    if not f:
        say("no func at %#x" % ea)
        return
    say("")
    say("######## ASM %s @ %#x %s [%d bytes]" % (idc.get_func_name(ea), ea, tag, f.end_ea - f.start_ea))
    cur, n = f.start_ea, 0
    while cur < f.end_ea and n < limit:
        say("   %016X  %s" % (cur, idc.GetDisasm(cur)))
        cur = idc.next_head(cur, f.end_ea)
        n += 1


for ea, tag in [
    (0x140352C10, "linker used by collection registrar 14034B650"),
    (0x140368220, "2nd caller of native invoke"),
    (0x140348670, "caller of CallNative wrapper"),
    (0x140352FA0, "create-class-on-demand (template instantiation?)"),
    (0x1402EBDE0, "TemplatedClass vtbl+0x60 (generic instantiate)"),
]:
    pseudo(ea, tag)

asm(0x140C6E880, "raw fastcall trampoline")
asm(0x140368220, "2nd invoke")

# ── все косвенные call [reg+8] в модуле скриптового потока ───────────────────
say("")
say("=== call qword ptr [reg+8] in 0x140340000..0x140370000 ===")
lo, hi = 0x140340000, 0x140370000
img = ida_bytes.get_bytes(lo, hi - lo) or b""
pats = []
for rm in (0, 1, 2, 3, 5, 6, 7):
    pats.append(bytes([0xFF, 0x50 | rm, 0x08]))
    pats.append(bytes([0x41, 0xFF, 0x50 | rm, 0x08]))
for pat in pats:
    pos = img.find(pat)
    while pos != -1:
        ea = lo + pos
        if idc.print_insn_mnem(ea) == "call":
            say("  %#x  %s   in %s" % (ea, idc.GetDisasm(ea), idc.get_func_name(ea)))
        pos = img.find(pat, pos + 1)

with open(os.path.join(OUT, "tpl4.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
