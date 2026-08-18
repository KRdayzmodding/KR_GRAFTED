# К ЧЕМУ ПРИКРЕПИТЬСЯ, чтобы получить вызов в начале кадра.
#
# Проект находит движковые функции ОДНИМ способом (SRC/graft/scan.cpp): якорь — строковый
# литерал, который есть в любом билде, дальше `lea` на него и ближайший `call`. Так найдены
# RegisterGlobal, RegisterMethod, FindClass. Вопрос: есть ли такой же якорь у функции,
# которая раз в кадр зовёт скриптовый OnUpdate.
import idaapi, idautils, idc, ida_funcs, ida_bytes, ida_nalt

OUT = []
def say(s):
    OUT.append(s)
    print(s)

def fname(ea):
    f = ida_funcs.get_func(ea)
    return idc.get_func_name(f.start_ea) if f else "?"

# 1. Кто ссылается на строку "OnUpdate": движок обязан искать скриптовый метод по имени.
say("=== ссылки на строковые якоря точек входа скрипта ===")
for want in ("OnUpdate", "OnUpdateEnd", "Update", "MissionGameplay", "CGame", "DayZGame"):
    hits = []
    for seg in idautils.Segments():
        name = idc.get_segm_name(seg)
        if name not in (".rdata", ".data", ".text"):
            continue
        ea = idc.get_segm_start(seg)
        end = idc.get_segm_end(seg)
        while ea < end:
            ea = idc.find_binary(ea, idaapi.SEARCH_DOWN, " ".join("%02X" % c for c in (want + "\x00").encode()))
            if ea == idaapi.BADADDR or ea >= end:
                break
            # начало строки, а не хвост чужой
            if idc.get_wide_byte(ea - 1) == 0:
                for xr in idautils.XrefsTo(ea, 0):
                    hits.append((ea, xr.frm, fname(xr.frm)))
            ea += 1
    say('"%s": строк с ссылками %d' % (want, len(hits)))
    seen = set()
    for saddr, frm, fn in hits[:12]:
        if fn in seen:
            continue
        seen.add(fn)
        say("    %s  <- %s (строка %s)" % (fn, hex(frm), hex(saddr)))

# 2. Какие строки видит каждая функция карты кадра — это и есть кандидаты в якоря.
say("")
say("=== строковые литералы внутри функций карты кадра ===")
CHAIN = [("sub_1405D9A70", 0x1405D9A70), ("sub_140C6A5D0", 0x140C6A5D0),
         ("sub_1404863C0", 0x1404863C0), ("sub_140C5F540", 0x140C5F540),
         ("sub_140C68F30", 0x140C68F30), ("sub_140AD1060", 0x140AD1060)]
for name, ea in CHAIN:
    f = ida_funcs.get_func(ea)
    if not f:
        say("%s: не функция" % name)
        continue
    strings = []
    for item in idautils.FuncItems(f.start_ea):
        for xr in idautils.XrefsFrom(item, 0):
            s = ida_bytes.get_strlit_contents(xr.to, -1, ida_nalt.STRTYPE_C)
            if s and 3 < len(s) < 60:
                strings.append(s.decode("utf-8", "replace"))
    say("%s: %s" % (name, sorted(set(strings))[:10] or "нет строк"))

with open(r"E:\source\KR_GRAFTED\re\out\diag\anchor.txt", "w", encoding="utf-8") as fh:
    fh.write("\n".join(OUT) + "\n")
idc.qexit(0)
