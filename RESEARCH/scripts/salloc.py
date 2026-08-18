# IDAPython: строковый аллокатор движка целиком — можно ли отдавать наружу ЕГО блоки
# вместо своей арены.
#
# Что нужно выяснить:
#   1. Точная сигнатура и семантика sub_140345C90 (выделение + копия).
#   2. Кто и как освобождает: sub_140348B50.
#   3. Потокобезопасность: берёт ли кто-то из них блокировку, и где живут фрилисты.
#   4. Якорь без RVA: литерал enf_scriptcontext.cpp и кто на него ссылается.
#   5. Где движковые нативы, возвращающие owned string, берут память — тем же путём?
#
#   re.ps1 run salloc.py -On diag
import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_name
import ida_pro
import ida_segment
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
lines = []


def say(s):
    print("[sa] %s" % s)
    lines.append(s)


def dump(ea, title, limit=200):
    fn = ida_funcs.get_func(ea)
    if not fn:
        say("!! нет функции @ %#x" % ea)
        return
    say("")
    say("=" * 78)
    say("%s — %s @ %#x (%d байт)" % (title, idc.get_func_name(ea), ea, fn.end_ea - fn.start_ea))
    say("=" * 78)
    callers = sorted({ida_funcs.get_func(x.frm).start_ea
                      for x in idautils.XrefsTo(fn.start_ea, 0) if ida_funcs.get_func(x.frm)})
    say("вызывающих: %d" % len(callers))
    try:
        cfunc = ida_hexrays.decompile(ea)
    except Exception as exc:
        say("!! не декомпилируется: %s" % exc)
        return
    for line in str(cfunc).splitlines()[:limit]:
        say("  " + line)


ALLOC = 0x140345C90
FREE = 0x140348B50
REGION = 0x140349000

dump(ALLOC, "АЛЛОКАТОР")
dump(FREE, "ОСВОБОЖДЕНИЕ")
dump(REGION, "ЧЕЙ УКАЗАТЕЛЬ", 120)

# ── Блокировки: есть ли синхронизация внутри аллокатора и рядом ──────────────
say("")
say("### СИНХРОНИЗАЦИЯ ###")
SYNC = ("EnterCriticalSection", "LeaveCriticalSection", "TryEnterCriticalSection",
        "AcquireSRWLock", "ReleaseSRWLock", "InterlockedCompareExchange")
for ea, title in ((ALLOC, "аллокатор"), (FREE, "освобождение")):
    fn = ida_funcs.get_func(ea)
    hits = []
    for item in idautils.FuncItems(ea):
        for x in idautils.XrefsFrom(item, 0):
            nm = ida_name.get_ea_name(x.to) or ""
            for s in SYNC:
                if s in nm:
                    hits.append("%s @ %#x" % (nm, item))
    say("%s: %s" % (title, ", ".join(hits) if hits else "НИ ОДНОГО вызова синхронизации"))

# ── Якорь: литерал с путём исходника ────────────────────────────────────────
say("")
say("### ЯКОРЬ БЕЗ RVA ###")
for seg_i in range(ida_segment.get_segm_qty()):
    seg = ida_segment.getnseg(seg_i)
    if ida_segment.get_segm_name(seg) not in (".rdata", ".data"):
        continue
    blob = ida_bytes.get_bytes(seg.start_ea, seg.end_ea - seg.start_ea) or b""
    needle = b"enf_scriptcontext.cpp"
    at = blob.find(needle)
    while at >= 0:
        # начало строки
        start = blob.rfind(b"\x00", 0, at) + 1
        ea = seg.start_ea + start
        text = ida_bytes.get_strlit_contents(ea, -1, 0)
        refs = sorted({ida_funcs.get_func(x.frm).start_ea
                       for x in idautils.XrefsTo(ea, 0) if ida_funcs.get_func(x.frm)})
        say("литерал @ %#x: %s" % (ea, (text or b"")[-60:]))
        say("   ссылаются: %s" % ", ".join("%s@%#x" % (idc.get_func_name(r), r) for r in refs))
        at = blob.find(needle, at + 1)

# ── Как берут память движковые нативы, возвращающие owned string ────────────
say("")
say("### ЧЕМ ПОЛЬЗУЮТСЯ САМИ ДВИЖКОВЫЕ НАТИВЫ ###")
say("Вызывающие аллокатора (кто и зачем выделяет строку):")
for x in idautils.XrefsTo(ALLOC, 0):
    fn = ida_funcs.get_func(x.frm)
    if fn:
        say("  %s @ %#x  (сайт %#x)" % (idc.get_func_name(fn.start_ea), fn.start_ea, x.frm))

with open(os.path.join(OUT, "salloc.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
ida_pro.qexit(0)
