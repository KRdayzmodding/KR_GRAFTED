# IDAPython: полная таблица нативов движка — кто, где, с каким impl.
#
#   .\re\re.ps1 run natives_dump.py -On diag
#
# Как: находим RegisterGlobal/RegisterMethod/FindClass (тот же поиск по строкам-маякам,
# что и в discover.py), берём все функции-регистраторы по xref'ам, декомпилируем каждую
# и вытаскиваем из псевдокода вызовы регистрации. Класс метода — из ближайшего
# предшествующего FindClass (регистраторы устроены как RegisterCoreNatives:
#   v = FindClass(ctx, "Имя"); if (v) { RegisterMethod(ctx, v, "Метод", impl, size, 1); ... }
#
# Результат: re\out\<target>\natives_all.json — {class: [{name, impl, ret_buf}]}.
# Нужен, чтобы находить импл конкретного натива (array.Count и т.п.) и по нему
# восстанавливать раскладку структур движка.

import json
import os
import re
import struct

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_nalt
import ida_pro
import ida_segment
import idautils
import idc

OUT = os.environ.get("RE_OUT", ".")

ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
base = ida_nalt.get_imagebase()

# ── тот же поиск точек, что в discover.py ────────────────────────────────────
seg_lo, seg_hi = None, None
for i in range(ida_segment.get_segm_qty()):
    s = ida_segment.getnseg(i)
    seg_lo = s.start_ea if seg_lo is None else min(seg_lo, s.start_ea)
    seg_hi = s.end_ea if seg_hi is None else max(seg_hi, s.end_ea)
img = ida_bytes.get_bytes(seg_lo, seg_hi - seg_lo) or b""


def find_cstr(text):
    needle = text.encode() + b"\x00"
    pos = img.find(needle)
    while pos != -1:
        if pos == 0 or img[pos - 1] == 0:
            return seg_lo + pos
        pos = img.find(needle, pos + 1)
    return None


def lea_sites(opcode, target):
    out = []
    pos = img.find(opcode)
    while pos != -1:
        if pos + 7 <= len(img):
            disp = struct.unpack_from("<i", img, pos + 3)[0]
            if seg_lo + pos + 7 + disp == target:
                out.append(seg_lo + pos)
        pos = img.find(opcode, pos + 1)
    return out


def call_near(ea, back=False, span=0x40):
    off = ea - seg_lo
    rng = range(max(off - span, 0), off) if back else range(off, min(off + span, len(img) - 5))
    hit = None
    for i in rng:
        if img[i] == 0xE8:
            tgt = seg_lo + i + 5 + struct.unpack_from("<i", img, i + 1)[0]
            if back:
                hit = tgt
            else:
                return tgt
    return hit


s_glob = find_cstr("MemoryValidation")
s_meth = find_cstr("GetNumberOfSetBits")
reg_global = call_near(lea_sites(b"\x48\x8d\x15", s_glob)[0])
site = lea_sites(b"\x4c\x8d\x05", s_meth)[0]
reg_method = call_near(site)
find_class = call_near(site, back=True)
print("[dump] RegisterGlobal=%#x RegisterMethod=%#x FindClass=%#x"
      % (reg_global, reg_method, find_class))

# ── все функции, которые регистрируют методы ─────────────────────────────────
registrars = set()
for xref in idautils.CodeRefsTo(reg_method, 0):
    fn = idc.get_func_attr(xref, idc.FUNCATTR_START)
    if fn != idc.BADADDR:
        registrars.add(fn)
print("[dump] registrars: %d" % len(registrars))

name_of = {reg_method: "RegisterMethod", find_class: "FindClass", reg_global: "RegisterGlobal"}
RE_FIND = re.compile(r"(\w+)\s*=\s*sub_%X\(\w+,\s*\"([^\"]+)\"\)" % find_class)
RE_METH = re.compile(
    r"sub_%X\(\w+,\s*[^,]+,\s*\(?[^\"]*\"([^\"]+)\",\s*\(?[^)]*?\)?(sub_[0-9A-F]+|&sub_[0-9A-F]+|"
    r"\w+),\s*(\w+),\s*\d+\)" % reg_method)
RE_ANY = re.compile(r"sub_(%X|%X)\(" % (find_class, reg_method))

classes = {}
for fn in sorted(registrars):
    try:
        text = str(ida_hexrays.decompile(fn))
    except Exception as exc:
        print("[dump] decompile %#x failed: %s" % (fn, exc))
        continue
    current = "?"
    for line in text.splitlines():
        m = RE_FIND.search(line)
        if m:
            current = m.group(2)
            continue
        if ("sub_%X(" % reg_method) in line:
            m = RE_METH.search(line)
            if not m:
                classes.setdefault(current, []).append({"raw": line.strip()})
                continue
            impl = m.group(2).lstrip("&")
            classes.setdefault(current, []).append({
                "name": m.group(1),
                "impl": impl,
                "ret_buf": m.group(3),
            })

with open(os.path.join(OUT, "natives_all.json"), "w", encoding="utf-8") as fh:
    json.dump({"imagebase": "%#x" % base, "classes": classes}, fh, indent=1, ensure_ascii=False)

print("[dump] classes=%d methods=%d"
      % (len(classes), sum(len(v) for v in classes.values())))
print("[dump] " + ", ".join(sorted(classes)[:40]))
ida_pro.qexit(0)
