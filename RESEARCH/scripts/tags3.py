# IDAPython: RTTI-имена типовых классов движка + пачка декомпиляций.
#   $env:RE_NAMES="ArrayClassT,SetClassT,MapClassT,Enum,VarType"
#   $env:RE_ADDRS="0x1402C50F0,..."
import os
import re

import ida_auto
import ida_funcs
import ida_hexrays
import ida_pro
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
PSEUDO = os.path.join(OUT, "pseudo")
ida_auto.auto_wait()
os.makedirs(PSEUDO, exist_ok=True)
have_hr = ida_hexrays.init_hexrays_plugin()
lines = []


def say(s):
    print("[t3] %s" % s)
    lines.append(s)


def safe(n):
    return re.sub(r"[^A-Za-z0-9_.-]", "_", n)[:120]


def dec(ea):
    fn = idc.get_func_name(ea)
    if not fn or not have_hr:
        return None
    p = os.path.join(PSEUDO, safe(fn) + ".c")
    if os.path.exists(p):
        return p
    try:
        cf = ida_hexrays.decompile(ea)
    except Exception as exc:
        say("fail %#x: %s" % (ea, exc))
        return None
    with open(p, "w", encoding="utf-8") as fh:
        fh.write("// %s @ %#x\n%s\n" % (fn, ea, cf))
    return p


for raw in os.environ.get("RE_ADDRS", "").replace(" ", "").split(","):
    if raw:
        say("decomp %s -> %s" % (raw, dec(int(raw, 16))))

KEYS = [k.strip() for k in os.environ.get("RE_NAMES", "").split(",") if k.strip()]
if KEYS:
    say("=== names ===")
    for ea, name in idautils.Names():
        if not any(k in name for k in KEYS):
            continue
        refs = []
        for x in idautils.XrefsTo(ea, 0):
            f = ida_funcs.get_func(x.frm)
            refs.append("%#x" % (f.start_ea if f else x.frm))
            if len(refs) > 6:
                break
        say("%#x  %s   <- %s" % (ea, name, ",".join(sorted(set(refs)))))

with open(os.path.join(OUT, "tags3.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
ida_pro.qexit(0)
