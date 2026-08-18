# sub_140602380 зовёт скриптовый OnUpdate по кэшированному индексу. Вопрос: он ли раз в
# кадр, и как до него дойти рантайм-сканом без единого RVA.
import idaapi, idautils, idc, ida_funcs, ida_hexrays

OUT = []
def say(s):
    OUT.append(s)
    print(s)

def name_of(ea):
    f = ida_funcs.get_func(ea)
    return idc.get_func_name(f.start_ea) if f else "?"

TARGET = 0x140602380

say("=== кто зовёт sub_140602380 ===")
for xr in idautils.XrefsTo(TARGET, 0):
    say("  %s @ %s" % (name_of(xr.frm), hex(xr.frm)))

say("")
say("=== он целиком ===")
try:
    say(str(ida_hexrays.decompile(TARGET)))
except Exception as oops:
    say("декомпилятор отказал: %s" % oops)

say("")
say("=== байты вокруг lea rdx,'OnUpdate' в нём (шаблон для рантайм-скана) ===")
f = ida_funcs.get_func(TARGET)
for item in idautils.FuncItems(f.start_ea):
    if idc.print_insn_mnem(item) == "lea" and "OnUpdate" in idc.GetDisasm(item):
        lo = item - 32
        ea = lo
        while ea < item + 64:
            say("  %s  %-28s %s" % (hex(ea), " ".join("%02X" % idc.get_wide_byte(ea + k)
                                                      for k in range(idc.get_item_size(ea))),
                                    idc.GetDisasm(ea)))
            ea = idc.next_head(ea)

say("")
say("=== сколько вызывающих у функций подготовки/исполнения вызова скрипта ===")
for nm, ea in (("sub_140348F40 (найти индекс по имени)", 0x140348F40),
               ("sub_140346CF0 (подготовить вызов по индексу)", 0x140346CF0),
               ("sub_140366A10 (исполнить)", 0x140366A10)):
    n = len(list(idautils.XrefsTo(ea, 0)))
    say("  %-46s вызывающих: %d" % (nm, n))

with open(r"E:\source\KR_GRAFTED\re\out\diag\entry2.txt", "w", encoding="utf-8") as fh:
    fh.write("\n".join(OUT) + "\n")
idc.qexit(0)
