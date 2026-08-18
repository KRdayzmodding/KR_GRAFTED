# Нужен ли ЖИВОЙ СКРИПТОВЫЙ КАДР, чтобы звать движковый `proto` из C++?
#
# Вопрос не про поиск функции: мы строим блок аргументов сами и зовём impl напрямую,
# минуя интерпретатор. Значит всё решает ОДНО — читает ли сам impl кадр потока.
#
# Кадр потока: TLS -> thread+7248 (вершина в thread+76), см. re/README.md. Признак в коде
# — обращение к gs:[0x58] (TLS-массив) либо вызовы функции, которая его читает.
import idaapi, idautils, idc, ida_funcs, ida_bytes, ida_hexrays

OUT = []
def say(s):
    OUT.append(s)
    print(s)

def fname(ea):
    f = ida_funcs.get_func(ea)
    return idc.get_func_name(f.start_ea) if f else "?"

# Кто вообще читает TLS: собираем множество функций с gs:[58h].
tls_readers = set()
for seg in idautils.Segments():
    ea = idc.get_segm_start(seg)
    end = idc.get_segm_end(seg)
    if idc.get_segm_name(seg) != ".text":
        continue
    ea = idc.next_head(ea, end)
    while ea != idaapi.BADADDR and ea < end:
        d = idc.GetDisasm(ea)
        if "gs:58h" in d or "gs:5Ch" in d:
            f = ida_funcs.get_func(ea)
            if f:
                tls_readers.add(f.start_ea)
        ea = idc.next_head(ea, end)
say("функций, читающих TLS напрямую: %d" % len(tls_readers))

# Функции, которые мы реально зовём снаружи (из callout.hpp) — берём по якорям в README.
TARGETS = {
    "sub_1403672C0": 0x1403672C0,   # чтение аргумента из блока
    "sub_140367280": 0x140367280,   # разрешение типа переменной по var+24
    "sub_140367E30": 0x140367E30,   # вызов натива из интерпретатора
    "sub_1403662A0": 0x1403662A0,   # подготовка кадра
}

def calls_of(ea, depth, seen):
    """множество вызываемых функций до глубины depth"""
    if depth == 0 or ea in seen:
        return set()
    seen.add(ea)
    out = {ea}
    f = ida_funcs.get_func(ea)
    if not f:
        return out
    for item in idautils.FuncItems(f.start_ea):
        for xr in idautils.XrefsFrom(item, 0):
            if xr.type in (idaapi.fl_CN, idaapi.fl_CF):
                out |= calls_of(xr.to, depth - 1, seen)
    return out

for name, ea in TARGETS.items():
    reach = calls_of(ea, 3, set())
    touched = reach & tls_readers
    say("%s: достижимо %d функций, из них читают TLS: %d %s" % (
        name, len(reach), len(touched),
        sorted(hex(x) for x in touched)[:6]))

with open(r"E:\source\KR_GRAFTED\re\out\diag\frame.txt", "w", encoding="utf-8") as fh:
    fh.write("\n".join(OUT) + "\n")
idc.qexit(0)
