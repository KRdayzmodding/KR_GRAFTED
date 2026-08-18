# IDAPython: найти строки по подстроке -> функции, которые на них ссылаются -> Hex-Rays.
#
#   $env:RE_QUERY = "Prototype error,Native functions don't support"
#   .\re\re.ps1 run probe.py
#
# Результат: re\out\probe.md (карта строка -> функция) и re\out\pseudo\<func>.c
#
# ponytail: один скрипт-щуп на все случаи. Любой анализ начинается со строки-маяка.

import json
import os
import re

import ida_auto
import ida_hexrays
import ida_pro
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
QUERY = [q.strip() for q in os.environ.get("RE_QUERY", "").split(",") if q.strip()]
PSEUDO = os.path.join(OUT, "pseudo")

ida_auto.auto_wait()
os.makedirs(PSEUDO, exist_ok=True)
have_hexrays = ida_hexrays.init_hexrays_plugin()


def safe(name):
    return re.sub(r"[^A-Za-z0-9_.-]", "_", name)[:120]


def decompile_to_file(ea):
    func = idc.get_func_name(ea)
    if not func:
        return None
    path = os.path.join(PSEUDO, safe(func) + ".c")
    if os.path.exists(path):
        return path
    if not have_hexrays:
        return None
    try:
        cf = ida_hexrays.decompile(ea)
    except ida_hexrays.DecompilationFailure as exc:
        print("[probe] decompile failed %s: %s" % (func, exc))
        return None
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("// %s @ %#x\n%s\n" % (func, ea, cf))
    return path


# RE_ADDRS=0x1402ACBE0,0x140286450 — просто вывалить псевдокод по адресам
for raw in os.environ.get("RE_ADDRS", "").replace(" ", "").split(","):
    if raw:
        print("[probe] %s -> %s" % (raw, decompile_to_file(int(raw, 16))))

report = []
for s in idautils.Strings():
    text = str(s)
    hits = [q for q in QUERY if q.lower() in text.lower()]
    if not hits:
        continue
    entry = {"string": text, "ea": "%#x" % s.ea, "refs": []}
    for xref in idautils.DataRefsTo(s.ea):
        fn = idc.get_func_name(xref)
        if fn:
            entry["refs"].append(
                {"func": fn, "ea": "%#x" % xref, "pseudo": decompile_to_file(xref)}
            )
            continue
        # Ссылка не из кода — значит строка лежит в таблице данных (регистрация
        # нативов выглядит именно так). Смотрим, кто читает саму таблицу.
        for up in idautils.DataRefsTo(xref) or []:
            entry["refs"].append(
                {
                    "func": idc.get_func_name(up) or "<data>",
                    "ea": "%#x" % up,
                    "via_table": "%#x" % xref,
                    "pseudo": decompile_to_file(up),
                }
            )
        if not entry["refs"]:
            entry["refs"].append({"func": "<data>", "ea": "%#x" % xref})
    report.append(entry)

with open(os.path.join(OUT, "probe.json"), "w", encoding="utf-8") as fh:
    json.dump(report, fh, ensure_ascii=False, indent=2)

with open(os.path.join(OUT, "probe.md"), "w", encoding="utf-8") as fh:
    for e in report:
        fh.write("## %s  `%s`\n" % (e["string"], e["ea"]))
        for r in e["refs"]:
            via = (" via table %s" % r["via_table"]) if r.get("via_table") else ""
            fh.write(
                "- %s @ %s%s -> %s\n" % (r["func"], r["ea"], via, r.get("pseudo"))
            )
        fh.write("\n")

print("[probe] strings=%d hexrays=%s" % (len(report), have_hexrays))
ida_pro.qexit(0)
