# IDAPython: кто и как обрабатывает исключения в движке.
#   Импорты AddVectoredExceptionHandler / SetUnhandledExceptionFilter / MiniDumpWriteDump
#   -> все места вызова -> псевдокод функций-обработчиков.
import os
import ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_name, ida_pro, idautils, idc

OUT = os.environ.get("RE_OUT", ".")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
lines = []
def say(s):
    print("[veh] %s" % s)
    lines.append(str(s))

WANT = ("AddVectoredExceptionHandler", "SetUnhandledExceptionFilter",
        "MiniDumpWriteDump", "UnhandledExceptionFilter", "TerminateProcess",
        "AddVectoredContinueHandler", "RemoveVectoredExceptionHandler")

targets = {}
for ea, name in idautils.Names():
    short = name.lstrip("_").split("@")[0]
    for w in WANT:
        if short == w or short == "__imp_" + w or name.endswith(w):
            targets.setdefault(w, []).append(ea)

for w in WANT:
    eas = targets.get(w, [])
    say("=== %s: %d символ(ов)" % (w, len(eas)))
    callers = set()
    for ea in eas:
        for xr in idautils.XrefsTo(ea):
            f = ida_funcs.get_func(xr.frm)
            if f:
                callers.add((f.start_ea, xr.frm))
    for start, site in sorted(callers):
        say("   вызов из %s @ %#x (сайт %#x)" % (idc.get_func_name(start), start, site))

# псевдокод регистраторов
for w in ("AddVectoredExceptionHandler", "SetUnhandledExceptionFilter"):
    for ea in targets.get(w, []):
        for xr in idautils.XrefsTo(ea):
            f = ida_funcs.get_func(xr.frm)
            if not f:
                continue
            say("")
            say("######## %s @ %#x  (регистрирует %s)" % (idc.get_func_name(f.start_ea), f.start_ea, w))
            try:
                say(str(ida_hexrays.decompile(f.start_ea)))
            except Exception as exc:
                say("  decompile failed: %s" % exc)

with open(os.path.join(OUT, "veh.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
