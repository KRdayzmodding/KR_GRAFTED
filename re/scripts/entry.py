# Где движок раз в кадр входит в скрипт.
#
# Строка "OnUpdate" в образе есть (0x140e404f8), на неё ссылаются 5 функций. Но ссылка —
# это место, где движок ОДИН РАЗ находит скриптовый метод. Сам вызов раз в кадр идёт уже
# по сохранённому дескриптору. Задача: пройти от якоря до точки вызова.
import idaapi, idautils, idc, ida_funcs, ida_hexrays, ida_bytes, ida_nalt

OUT = []
def say(s):
    OUT.append(s)
    print(s)

STR_ONUPDATE = 0x140e404f8

def deco(ea, tag):
    f = ida_funcs.get_func(ea)
    if not f:
        say("%s: не функция" % tag)
        return None
    try:
        cf = ida_hexrays.decompile(f.start_ea)
    except Exception as oops:
        say("%s: декомпилятор отказал (%s)" % (tag, oops))
        return None
    return str(cf)

# 1. Соседние строки: если рядом лежат другие точки входа скрипта, это таблица.
say("=== что лежит рядом со строкой OnUpdate ===")
ea = STR_ONUPDATE - 200
while ea < STR_ONUPDATE + 200:
    s = ida_bytes.get_strlit_contents(ea, -1, ida_nalt.STRTYPE_C)
    if s and 2 < len(s) < 40:
        say("  %s  %s" % (hex(ea), s.decode("utf-8", "replace")))
        ea += len(s) + 1
    else:
        ea += 1

say("")
say("=== функции, ссылающиеся на OnUpdate ===")
for xr in idautils.XrefsTo(STR_ONUPDATE, 0):
    f = ida_funcs.get_func(xr.frm)
    if not f:
        continue
    name = idc.get_func_name(f.start_ea)
    say("")
    say("######## %s @ %s (ссылка на %s)" % (name, hex(f.start_ea), hex(xr.frm)))
    src = deco(f.start_ea, name)
    if not src:
        continue
    # печатаем только окрестность упоминания OnUpdate
    lines = src.splitlines()
    for i, line in enumerate(lines):
        if "OnUpdate" in line:
            for j in range(max(0, i - 12), min(len(lines), i + 14)):
                say("    %s" % lines[j])
            say("    ---")
    say("    [всего строк %d]" % len(lines))

with open(r"E:\source\KR_GRAFTED\re\out\diag\entry.txt", "w", encoding="utf-8") as fh:
    fh.write("\n".join(OUT) + "\n")
idc.qexit(0)
