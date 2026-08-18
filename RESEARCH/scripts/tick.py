# Чем МОЖНО зацепиться за кадр, не зная ни одного RVA.
#
# Карта кадра (re/README.md) снята с минидампа и содержит адреса — для ИССЛЕДОВАНИЯ это
# годится, для рабочего кода нет: адреса живут до первого патча. Поэтому ищем не функцию,
# а ИМПОРТ, который эта функция зовёт: имя импорта переживает любую пересборку игры.
import idaapi, idautils, idc, ida_funcs, ida_name

OUT = []
def say(s):
    OUT.append(s)
    print(s)

CHAIN = [
    ("sub_1405D9A70", 0x1405D9A70),
    ("sub_140C6A5D0", 0x140C6A5D0),
    ("sub_1404863C0", 0x1404863C0),
    ("sub_140C5F540", 0x140C5F540),
    ("sub_140C68F30", 0x140C68F30),
    ("sub_140AD1060", 0x140AD1060),
    ("sub_1403662A0", 0x1403662A0),
]

# Все импорты образа: адрес слота -> имя
imports = {}
def on_import(ea, name, ordinal):
    if name:
        imports[ea] = name
    return True
for i in range(idaapi.get_import_module_qty()):
    mod = idaapi.get_import_module_name(i) or "?"
    idaapi.enum_import_names(i, lambda ea, name, ordinal, m=mod: on_import(ea, "%s!%s" % (m, name), ordinal))
say("импортов в образе: %d" % len(imports))

def api_calls(start, depth, seen):
    """какие импорты зовёт функция (и её вызовы до depth)"""
    if depth == 0 or start in seen:
        return set()
    seen.add(start)
    found = set()
    f = ida_funcs.get_func(start)
    if not f:
        return found
    for item in idautils.FuncItems(f.start_ea):
        m = idc.print_insn_mnem(item)
        if m not in ("call", "jmp"):
            continue
        for xr in idautils.XrefsFrom(item, 0):
            nm = imports.get(xr.to)
            if nm:
                found.add(nm)
            elif xr.type in (idaapi.fl_CN, idaapi.fl_CF, idaapi.fl_JN, idaapi.fl_JF):
                found |= api_calls(xr.to, depth - 1, seen)
    return found

say("")
say("=== какие Win32-импорты зовёт каждый уровень кадра (глубина 2) ===")
for name, ea in CHAIN:
    got = api_calls(ea, 2, set())
    interesting = sorted(a for a in got
                         if any(k in a for k in ("Sleep", "Performance", "timeGet", "TickCount",
                                                 "SwitchToThread", "WaitFor", "GetSystemTime",
                                                 "Yield", "Query")))
    say("%-16s всего %3d, из них по времени/сну: %s" % (name, len(got), interesting or "—"))

say("")
say("=== кто в образе вообще зовёт таймерные API (кандидаты в кадр) ===")
for want in ("Sleep", "QueryPerformanceCounter", "timeGetTime", "GetTickCount64", "SwitchToThread"):
    callers = set()
    for ea, nm in imports.items():
        if not nm.endswith("!" + want):
            continue
        for xr in idautils.XrefsTo(ea, 0):
            f = ida_funcs.get_func(xr.frm)
            if f:
                callers.add(f.start_ea)
    say("%-24s зовут %d функций" % (want, len(callers)))

with open(r"E:\source\KR_GRAFTED\re\out\diag\tick.txt", "w", encoding="utf-8") as fh:
    fh.write("\n".join(OUT) + "\n")
idc.qexit(0)
