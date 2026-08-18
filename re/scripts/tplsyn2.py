# IDAPython: парсер объявления класса — кто сравнивает токен с 'class'/'modded'/'ref'…
#   .\re\re.ps1 run tplsyn2.py -On diag

import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_pro
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")
PSEUDO = os.path.join(OUT, "pseudo")
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
os.makedirs(PSEUDO, exist_ok=True)

lines = []


def say(s):
    print("[s2] %s" % s)
    lines.append(str(s))


# интернированные ключевые слова: имя глобала -> слово (из sub_1401444E0)
KWG = {
    0x141256C38: "proto", 0x141256C50: "typedef", 0x141256C60: "event",
    0x141256C68: "notnull", 0x141256C70: "reference", 0x141256C80: "thread",
    0x141256B78: "class", 0x141256B80: "enum", 0x141256BA8: "extends",
    0x141256BE0: "static", 0x141256BE8: "const", 0x141256BF0: "local",
    0x141256BF8: "volatile", 0x141256C00: "private", 0x141256CC0: "protected",
    0x141256C88: "native", 0x141256C90: "owned", 0x141256C98: "out",
    0x141256CA0: "inout", 0x141256CA8: "autoptr", 0x141256CB0: "auto",
    0x141256CB8: "ref", 0x141256C08: "modded", 0x141256C10: "sealed",
    0x141256C18: "external", 0x141256C20: "override", 0x141256C28: "Cast",
    0x141256B30: "~", 0x141256AF0: ",", 0x141256AE0: "{", 0x141256AE8: "}",
    0x141256AD0: "(", 0x141256AD8: ")", 0x141256B10: "[", 0x141256B18: "]",
    0x141256AF8: ";", 0x141256B00: ":",
}

# неименованные односимвольные токены — прочитаем байты
say("=== опознание односимвольных токенов ===")
for nm in ("dword_140DEA98C", "dword_140DEA994", "dword_140DEA99C", "byte_140DEA998",
           "byte_140DEA978", "dword_140DEA974", "word_140DEA990", "byte_140DEA988",
           "byte_140DEA980", "dword_140DEA97C", "dword_140DEAC5C", "byte_140DEA984",
           "byte_140DF19B8", "byte_140DEA970"):
    ea = idc.get_name_ea_simple(nm)
    if ea == idc.BADADDR:
        say("%-18s not found" % nm)
        continue
    b = ida_bytes.get_bytes(ea, 6) or b""
    say("%-18s %#x  %r" % (nm, ea, b))

say("")
say("=== xrefs по интернированным словам ===")
owners = {}
for ea, word in KWG.items():
    refs = list(idautils.DataRefsTo(ea))
    fs = set()
    for r in refs:
        f = ida_funcs.get_func(r)
        if f:
            fs.add(f.start_ea)
            owners.setdefault(f.start_ea, set()).add(word)
    say("%-10s %#x refs=%2d  %s" % (word, ea, len(refs), ", ".join("%#x" % x for x in sorted(fs))))

say("")
say("=== функции-парсеры (по числу разных ключевых слов) ===")
for ea in sorted(owners, key=lambda e: -len(owners[e])):
    say("%#x %-16s (%2d) %s" % (ea, idc.get_func_name(ea), len(owners[ea]),
                                " ".join(sorted(owners[ea]))))


def dump(ea, tag=""):
    f = ida_funcs.get_func(ea)
    if not f:
        return
    name = idc.get_func_name(f.start_ea)
    try:
        text = str(ida_hexrays.decompile(f.start_ea))
    except Exception as exc:
        say("decompile %#x failed: %s" % (ea, exc))
        return
    with open(os.path.join(PSEUDO, name + ".c"), "w", encoding="utf-8") as fh:
        fh.write("// %s @ %#x  %s\n%s" % (name, f.start_ea, tag, text))
    say("wrote pseudo/%s.c  %s" % (name, tag))


say("")
for ea in sorted(owners):
    dump(ea, "parser: " + " ".join(sorted(owners[ea])))

with open(os.path.join(OUT, "tplsyn2.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
say("done")
ida_pro.qexit(0)
