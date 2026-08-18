# IDAPython: шаг 5 — какой именно call [desc+8] попадает в тонки коллекций.
#   .\re\re.ps1 run tpl5.py -On diag

import os

import ida_auto
import ida_funcs
import ida_hexrays
import ida_pro
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
os.makedirs(os.path.join(OUT, "pseudo"), exist_ok=True)

lines = []


def say(s):
    print("[t5] %s" % s)
    lines.append(str(s))


def window(lo, hi, tag=""):
    say("")
    say("######## WINDOW %#x..%#x  %s" % (lo, hi, tag))
    ea = lo
    while ea < hi:
        say("   %016X  %s" % (ea, idc.GetDisasm(ea)))
        nxt = idc.next_head(ea, hi)
        if nxt <= ea:
            break
        ea = nxt


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


window(0x140369600, 0x1403696D0, "sub_140368880 call [r13+8]")
window(0x14036A730, 0x14036A7C0, "sub_140368880 call [rax+8] #1")
window(0x14036ABE0, 0x14036AC70, "sub_140368880 call [rax+8] #2")
window(0x14036ADC0, 0x14036AE40, "sub_140368880 call [rax+8] #3")
window(0x140368780, 0x140368820, "sub_140368730 call [rax+8]")

for ea, tag in [
    (0x140348480, "call [rbp+8]"),
    (0x140348570, "call [r14+8]"),
    (0x140368730, "call [rax+8]"),
    (0x140367490, "call [rax+8]"),
]:
    pseudo(ea, tag)

# кто вообще ссылается на импл array.Get / array.Insert
for ea in (0x1403056D0, 0x140316390, 0x1403056F0, 0x140305440):
    say("")
    say("xrefs to impl %#x (%s):" % (ea, idc.get_func_name(ea)))
    for x in idautils.XrefsTo(ea):
        say("   from %#x  type=%d  in %s" % (x.frm, x.type, idc.get_func_name(x.frm) or "<data>"))

with open(os.path.join(OUT, "tpl5.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
