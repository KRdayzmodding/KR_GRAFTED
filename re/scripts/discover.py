# IDAPython: сухой прогон рантайм-поиска точек регистрации нативов — тот же самый
# байтовый скан, что делает graft-библиотека в загруженном образе (SRC/graft/engine.cpp).
# Хардкода адресов нет: всё от строк-маяков, которые являются частью публичного
# script-API движка и потому переживают патчи.
#
#   .\re\re.ps1 run discover.py -On diag
#   .\re\re.ps1 run discover.py -On server
#
# Алгоритм (в C++ он же, плюс голосование трёх якорей на каждую точку):
#   RegisterGlobal  : строка "MemoryValidation" -> `lea rdx,[rip+str]` -> ближайший call
#   RegisterMethod  : строка "GetNumberOfSetBits" -> `lea r8,[rip+str]` -> ближайший call
#   FindClass       : от того же места назад — ближайший предшествующий call
#                     (в исходнике это FindClass(ctx,"Math") прямо перед RegisterMethod)
#
# Результат: re\out\<target>\discover.json + сверка с natives.json (если он есть).

import json
import os
import struct

import ida_auto
import ida_bytes
import ida_nalt
import ida_pro
import ida_segment

OUT = os.environ.get("RE_OUT", ".")

ida_auto.auto_wait()
base = ida_nalt.get_imagebase()

# образ целиком одним куском — так же его видит C++ в рантайме
seg_lo, seg_hi = None, None
for i in range(ida_segment.get_segm_qty()):
    s = ida_segment.getnseg(i)
    seg_lo = s.start_ea if seg_lo is None else min(seg_lo, s.start_ea)
    seg_hi = s.end_ea if seg_hi is None else max(seg_hi, s.end_ea)
img = ida_bytes.get_bytes(seg_lo, seg_hi - seg_lo) or b""
print("[discover] image %#x..%#x (%d bytes)" % (seg_lo, seg_hi, len(img)))


def find_cstr(text):
    """адрес C-строки text (ровно, не подстрока)"""
    needle = text.encode() + b"\x00"
    pos = img.find(needle)
    while pos != -1:
        if pos == 0 or img[pos - 1] == 0:
            return seg_lo + pos
        pos = img.find(needle, pos + 1)
    return None


def lea_sites(opcode, target_ea):
    """адреса инструкций `lea reg,[rip+d]`, указывающих на target_ea"""
    out = []
    pos = img.find(opcode)
    while pos != -1:
        if pos + 7 <= len(img):
            disp = struct.unpack_from("<i", img, pos + 3)[0]
            if seg_lo + pos + 7 + disp == target_ea:
                out.append(seg_lo + pos)
        pos = img.find(opcode, pos + 1)
    return out


def call_after(ea, span=0x40):
    off = ea - seg_lo
    for i in range(off, min(off + span, len(img) - 5)):
        if img[i] == 0xE8:
            return seg_lo + i + 5 + struct.unpack_from("<i", img, i + 1)[0]
    return None


def call_before(ea, span=0x40):
    off = ea - seg_lo
    hit = None
    for i in range(max(off - span, 0), off):
        if img[i] == 0xE8:
            hit = seg_lo + i + 5 + struct.unpack_from("<i", img, i + 1)[0]
    return hit


LEA_RDX = b"\x48\x8d\x15"
LEA_R8 = b"\x4c\x8d\x05"

result = {"imagebase": "%#x" % base}

s_glob = find_cstr("MemoryValidation")
s_meth = find_cstr("GetNumberOfSetBits")
print("[discover] anchors: MemoryValidation=%s GetNumberOfSetBits=%s"
      % (s_glob and hex(s_glob), s_meth and hex(s_meth)))

if s_glob:
    for site in lea_sites(LEA_RDX, s_glob):
        tgt = call_after(site)
        if tgt:
            result["RegisterGlobal"] = "%#x" % (tgt - base)
            result["RegisterGlobal_site"] = "%#x" % (site - base)
            break

if s_meth:
    for site in lea_sites(LEA_R8, s_meth):
        tgt = call_after(site)
        prev = call_before(site)
        if tgt:
            result["RegisterMethod"] = "%#x" % (tgt - base)
            result["RegisterMethod_site"] = "%#x" % (site - base)
        if prev:
            result["FindClass"] = "%#x" % (prev - base)
        if tgt:
            break

with open(os.path.join(OUT, "discover.json"), "w", encoding="utf-8") as fh:
    json.dump(result, fh, indent=2)

# сверка с тем, что нашёл natives.py (частотный анализ по RegisterCoreNatives)
expect_path = os.path.join(OUT, "natives.json")
if os.path.exists(expect_path):
    with open(expect_path, encoding="utf-8") as fh:
        known = {c["role"]: c["rva"] for c in json.load(fh)["callees"]}
    for role in ("RegisterGlobal", "RegisterMethod", "FindClass"):
        got, exp = result.get(role), known.get(role)
        print("[discover] %-15s got=%s expect=%s %s"
              % (role, got, exp, "OK" if got == exp else "MISMATCH"))

print("[discover] " + json.dumps(result))
ida_pro.qexit(0)
