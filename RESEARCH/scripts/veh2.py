# IDAPython: 1) что делает верхний фильтр и писатель минидампа
#            2) карта КАДРА — функции из стека падения, от точки входа потока до нашего
#               трамплина. Смещения взяты из cdb (.ecxr; k) по настоящему минидампу.
import os
import ida_auto, ida_funcs, ida_hexrays, ida_name, ida_pro, idautils, idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
lines = []
def say(s):
    print("[v2] %s" % s)
    lines.append(str(s))

def pseudo(ea, tag=""):
    f = ida_funcs.get_func(ea)
    if not f:
        say("!! нет функции на %#x (%s)" % (ea, tag))
        return
    say("")
    say("######## %s @ %#x  %s" % (idc.get_func_name(f.start_ea), f.start_ea, tag))
    try:
        say(str(ida_hexrays.decompile(f.start_ea)))
    except Exception as exc:
        say("  decompile failed: %s" % exc)

# ── обработка падения ────────────────────────────────────────────────────────
for name in ("TopLevelExceptionFilter", "Handler"):
    ea = ida_name.get_name_ea(idc.BADADDR, name)
    if ea != idc.BADADDR:
        pseudo(ea, "движковый %s" % name)
pseudo(0x14036E8C0, "пишет минидамп")

# ── карта кадра ──────────────────────────────────────────────────────────────
# Стек падения (cdb): снизу вверх — RtlUserThreadStart -> BaseThreadInitThunk ->
# CDPCreateServer+0x4149d2 -> ... -> VM -> наш трамплин.
base = ida_name.get_name_ea(idc.BADADDR, "CDPCreateServer")
if base == idc.BADADDR:
    # экспорт может называться иначе — ищем перебором
    for ea, name in idautils.Names():
        if "CDPCreateServer" in name:
            base = ea
            break
say("")
say("=== CDPCreateServer @ %s" % ("не найден" if base == idc.BADADDR else hex(base)))
HAVE_BASE = base != idc.BADADDR
frames = [] if not HAVE_BASE else [
    (base + 0x4149d2, "кадр 13: сразу под BaseThreadInitThunk — главный цикл потока"),
    (0x140000000 + 0x5d9b3e, "кадр 12"),
    (base + 0x2bdc76, "кадр 11"),
    (0x140000000 + 0x486477, "кадр 10"),
    (base + 0x2b2b84, "кадр 9"),
    (base + 0x2bc74e, "кадр 8"),
    (base + 0x124bbb, "кадр 7"),
    (0x140000000 + 0x3662b5, "кадр 6"),
    (0x140000000 + 0x368e44, "кадр 5"),
    (0x140000000 + 0x36828c, "кадр 4"),
    (0x140000000 + 0x367f11, "кадр 3"),
    (base + 0x2c1f05, "кадр 2: зовёт наш трамплин"),
]
if not HAVE_BASE:
    frames = [(0x140000000 + rva, "кадр по RVA %#x" % rva)
              for rva in (0x5d9b3e, 0x486477, 0x3662b5, 0x368e44, 0x36828c, 0x367f11)]
say("")
say("=== карта кадра: какая функция на каком уровне ===")
for ea, tag in frames:
    f = ida_funcs.get_func(ea)
    say("  %-46s %#x -> %s" % (tag, ea, idc.get_func_name(f.start_ea) if f else "?"))

# Псевдокод верхних уровней: там и живёт «раз в кадр».
for ea, tag in frames[:6]:
    pseudo(ea, tag)

with open(os.path.join(OUT, "veh2.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
