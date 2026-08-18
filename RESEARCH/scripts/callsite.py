# IDAPython: раскрутить ABI натива.
#   1) декомпилировать RegisterMethod (RE_REG) — найти, в какое поле дескриптора
#      кладётся impl-указатель;
#   2) взять конкретный impl (RE_IMPL, напр. string.Length) и найти, кто его CALLает
#      косвенно — это опкод-хендлер VM, который и задаёт calling convention;
#   3) вывалить псевдокод всех участников.
#
#   $env:RE_REG=0x1402CB6E0; $env:RE_IMPL=0x1402ACBE0
#   .\re\re.ps1 run callsite.py -On server

import os

import ida_auto
import ida_funcs
import ida_hexrays
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
REG = int(os.environ.get("RE_REG", "0"), 16)
IMPL = int(os.environ.get("RE_IMPL", "0"), 16)

ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()


def dump(ea, tag):
    f = ida_funcs.get_func(ea)
    if not f:
        print("[callsite] no func at %#x" % ea)
        return
    path = os.path.join(OUT, "%s_%X.c" % (tag, f.start_ea))
    try:
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(str(ida_hexrays.decompile(f.start_ea)))
        print("[callsite] %s -> %s" % (tag, path))
    except Exception as exc:
        print("[callsite] decompile %#x failed: %s" % (ea, exc))


if REG:
    dump(REG, "reg")

# цепочка внутренних вызовов из REG (регистратор -> хранилище дескриптора)
for extra in os.environ.get("RE_CHAIN", "").replace(" ", "").split(","):
    if extra:
        dump(int(extra, 16), "chain")

if IMPL:
    # кто ссылается на impl (кроме строки регистрации) — ищем косвенный вызов
    for xref in idautils.CodeRefsTo(IMPL, 0):
        print("[callsite] code xref to impl from %#x (%s)" % (xref, idc.get_func_name(xref)))
        dump(xref, "caller")
    # impl может попадать в дескриптор как данные; посмотрим, что за функция его читает
    for xref in idautils.DataRefsTo(IMPL):
        fn = idc.get_func_name(xref)
        print("[callsite] data xref to impl from %#x (%s)" % (xref, fn or "<data>"))

# сам impl тоже полезен — как он читает свои аргументы
if IMPL:
    dump(IMPL, "impl")

import ida_pro

ida_pro.qexit(0)
