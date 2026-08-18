# IDAPython: шаг 8 — 8 vtable классов-инстанциаций array<T> и фабрика инстансов.
#   .\re\re.ps1 run tpl8.py -On diag

import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_pro
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()

lines = []


def say(s):
    print("[t8] %s" % s)
    lines.append(str(s))


VTS = [
    (0x140E24FA0, "array<int/bool>"),
    (0x140E250E8, "array<float>"),
    (0x140E25230, "array<string>"),
    (0x140E25378, "array<vector>"),
    (0x140E254C0, "array<@Class> (owned ref)"),
    (0x140E25608, "array<Class managed>"),
    (0x140E25680, "array<Class>"),
    (0x140E257C8, "array<typename>"),
]

table = {}
for ea, tag in VTS:
    row = []
    for i in range(0, 16):
        v = ida_bytes.get_qword(ea + i * 8)
        row.append(v if ida_funcs.get_func(v) else 0)
    table[tag] = row
    say("%-28s @ %#x  %s" % (tag, ea, " ".join("%X" % (x & 0xFFFFFFF) for x in row)))

say("")
say("=== по слотам ===")
for i in range(0, 16):
    say("  +%#04x [%2d]  %s" % (i * 8, i, "  ".join(
        "%s=%X" % (tag.split("<")[1].rstrip(">"), table[tag][i] & 0xFFFFFF) for tag, _ in
        [(t, 0) for t, _ in [(k, 0) for k in table]])))

say("")
for ea, tag in VTS[:3]:
    slot = ida_bytes.get_qword(ea + 4 * 8)   # +0x20 — кандидат на "создать инстанс"
    say("%-28s vtbl+0x20 -> %#x %s" % (tag, slot, idc.get_func_name(slot)))
    try:
        say(str(ida_hexrays.decompile(slot)))
    except Exception as exc:
        say("  decompile failed: %s" % exc)

with open(os.path.join(OUT, "tpl8.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
