# IDAPython: список импортируемых DLL — для выбора цели proxy-DLL.
import os

import ida_auto
import ida_nalt
import ida_pro

ida_auto.auto_wait()
mods = []
for i in range(ida_nalt.get_import_module_qty()):
    name = ida_nalt.get_import_module_name(i)
    if name:
        mods.append(name)

path = os.path.join(os.environ.get("RE_OUT", "."), "imports.txt")
with open(path, "w", encoding="utf-8") as fh:
    fh.write("\n".join(sorted(mods, key=str.lower)))
print("[imports] " + ", ".join(sorted(mods, key=str.lower)))
ida_pro.qexit(0)
