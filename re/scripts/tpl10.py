import os
import ida_auto, ida_bytes, ida_funcs, ida_pro, idc
OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
lines = []
def say(s):
    print("[t10] %s" % s); lines.append(str(s))
VTS = [(0x140E24ED0, "array<int> INSTANCE"),
       (0x140E25160, "array<string> INSTANCE"),
       (0x140E24C08, "array<Class> INSTANCE")]
rows = {}
for ea, tag in VTS:
    row = []
    for i in range(0, 21):
        v = ida_bytes.get_qword(ea + i * 8)
        row.append(v if ida_funcs.get_func(v) else 0)
    rows[tag] = row
NAMES = {5:"Count",6:"Clear",7:"Remove",8:"RemoveOrdered",9:"Set",10:"Find",
         11:"Swap",12:"Get",13:"Insert",14:"InsertAt",15:"Copy",16:"Init",
         17:"Resize",18:"Reserve",19:"Sort",20:"-"}
for i in range(0, 21):
    say("+%#04x [%2d] %-14s %s" % (i*8, i, NAMES.get(i, ""),
        "  ".join("%s=%X" % (t.split("<")[1].split(">")[0], rows[t][i]) for _, t in VTS)))
with open(os.path.join(OUT, "tpl10.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
ida_pro.qexit(0)
