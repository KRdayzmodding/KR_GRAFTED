#!/usr/bin/env python3
"""Перепроверка песчаной бури (dayz/weather.md) на образе игры.

Проверяет не «где какие строки», а утверждения документа:
  1. парсер маршрута находится по строке startPosition и пишет точки в поля
     объекта: смещения +0x18 (начало), +0x24 (конец), +0x30 (длина);
  2. дефолт отсутствующей секции — обе точки (0, 0, 10000): константа
     10000.0f (00 40 1C 46) встречается в теле парсера дважды;
  3. гейт натива Start — «текущая магнитуда == 0», не маршрут:
     movss xmm0,[rax+0x10] затем ucomiss с нулём;
  4. регистрация SandstormController даёт все 11 нативов;
  5. парсер частиц — ТОЛЬКО в клиентском коде: в dedicated-сервере строк
     particlePath / sandstormIntensity / пути частицы по умолчанию НЕТ,
     в образах с клиентом (DayZ_x64, DayZDiag) — есть. Роль определяется
     по имени файла (содержит «server» — dedicated).

Образ берётся из GRAFT_SERVER_DIR (каталог сервера с DayZServer_x64.exe),
переопределяется первым аргументом. Разбор PE — pe.py из этой же папки.

    py -3.12 RESEARCH/scripts/sandstorm.py [DayZServer_x64.exe|DayZDiag_x64.exe|DayZ_x64.exe]

Выход 0 — все проверки сошлись, 1 — что-то изменилось: документ надо обновлять.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capstone  # noqa: E402
import capstone.x86  # noqa: E402
from pe import PE  # noqa: E402

# ── утверждения документа ────────────────────────────────────────────────────
PARSER_OFFSETS = {0x18: "startPosition", 0x24: "endPosition", 0x30: "длина маршрута"}
DEFAULT_10000F = bytes.fromhex("00401C46")   # 10000.0f, две штуки в теле парсера
GATE_MOVSS = bytes.fromhex("F30F104010")     # movss xmm0,[rax+0x10] — магнитуда
GATE_UCOMISS = re.compile(rb"[\x2e\x2f].", re.S)  # ucomiss/comiss — адрес не важен

# 11 нативов SandstormController из регистрации (weather.md, «карта нативов»).
NATIVES = ["Start", "Stop", "IsActive", "IsPositionAtEnd", "GetDirection",
           "GetPosition", "GetSpeed", "GetPointOnEdge", "GetIntensity",
           "GetMagnitude", "GetRemainingMovementDuration"]

# Парсер частиц — только клиентский код (weather.md, «парсер не одинако́в»).
CLIENT_ONLY_STRINGS = [b"particlePath\x00", b"sandstormIntensity\x00",
                       b"Graphics/Particles/sandstorm/sandstorm"]


def ok(msg):
    print("  ok   " + msg)


def fail(msg):
    print("  FAIL " + msg)


def fn_by_anchor(p, text):
    """Функция, ссылающаяся на строку-якорь (lea rip-relative → .pdata)."""
    for va in p.find_str(text):
        for ref in p.riprefs(va):
            f = p.func_at(ref)
            if f:
                return f
    return None


def disasm(p, span):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    beg, end = span
    return list(md.disasm(p.read(beg, end - beg), beg))


def main(argv):
    exe = argv[1] if len(argv) > 1 else None
    if not exe:
        srv = os.environ.get("GRAFT_SERVER_DIR", "")
        exe = os.path.join(srv, "DayZServer_x64.exe") if srv else ""
    if not exe or not os.path.isfile(exe):
        print("образ не найден: задай GRAFT_SERVER_DIR или путь как аргумент")
        return 1

    p = PE(exe)
    dedicated = "server" in os.path.basename(exe).lower()
    print("образ:", exe, "база:", hex(p.base),
          "| роль:", "dedicated-сервер" if dedicated else "клиент/диаг")
    ret = True

    # ── 1. парсер маршрута и смещения полей ─────────────────────────────────
    print("\n[1] парсер маршрута:")
    parser = fn_by_anchor(p, b"startPosition\x00")
    if not parser:
        fail("парсер по якорю startPosition не найден — якорь уехал")
        return 1
    ok("парсер %#x..%#x (ссылка на startPosition)" % parser)

    body = p.read(parser[0], parser[1] - parser[0])
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    found = {}
    for insn in md.disasm(body, parser[0]):
        for op in insn.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.disp in PARSER_OFFSETS:
                found.setdefault(op.mem.disp,
                                 (insn.address, insn.mnemonic + " " + insn.op_str))
    for off, label in PARSER_OFFSETS.items():
        if off in found:
            ok("%-18s [%#x] @%#x  %s" % (label, off, found[off][0], found[off][1]))
        else:
            fail("%-18s [+%#x] в теле парсера нет — раскладка изменилась" % (label, off))
            ret = False

    hits = body.count(DEFAULT_10000F)
    if hits >= 2:
        ok("дефолт 10000.0f ×%d (обе точки — маршрут нулевой длины)" % hits)
    else:
        fail("дефолт 10000.0f встречен %d раз (<2) — дефолт секции изменился" % hits)
        ret = False

    # ── 2. гейт Start: магнитуда == 0, не маршрут ───────────────────────────
    print("\n[2] гейт натива Start:")
    reg = fn_by_anchor(p, b"SandstormController\x00")
    start_impl = None
    if reg:
        code = disasm(p, reg)
        pending = None
        for insn in code:
            m = re.search(r"rip ([+-]) (0x[0-9a-f]+)", insn.op_str)
            if not m:
                continue
            disp = int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)
            tgt = insn.address + insn.size + disp
            if insn.mnemonic == "lea" and insn.op_str.startswith("r9"):
                pending = tgt
            elif insn.mnemonic == "lea" and insn.op_str.startswith("r8"):
                name = None
                d = p.read(tgt, 48)
                if d and 32 <= d[0] < 127:
                    name = d.split(b"\0")[0].decode(errors="replace")
                if name == "Start" and pending:
                    start_impl = pending
                pending = None
    if not reg or not start_impl:
        fail("регистрация/импл Start не найдены")
        return 1
    sf = p.func_at(start_impl)
    sbody = p.read(sf[0], sf[1] - sf[0])
    gate = sbody.find(GATE_MOVSS)
    after = sbody[gate + len(GATE_MOVSS):gate + len(GATE_MOVSS) + 16] if gate >= 0 else b""
    if gate >= 0 and re.search(rb"(?:\xc5|\x0f)[\x2e\x2f]", after):
        ok("магнитуда == 0: movss @%#x + ucomiss — маршрут не проверяется" % (sf[0] + gate))
    else:
        fail("гейт «магнитуда == 0» не найден в Start — семантика изменилась")
        ret = False

    # ── 3. карта нативов: все 11 из регистрации ─────────────────────────────
    print("\n[3] нативы SandstormController:")
    names = set()
    if reg:
        pending = None
        for insn in disasm(p, reg):
            m = re.search(r"rip ([+-]) (0x[0-9a-f]+)", insn.op_str)
            if not m:
                continue
            disp = int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)
            tgt = insn.address + insn.size + disp
            if insn.mnemonic == "lea" and insn.op_str.startswith("r9"):
                pending = tgt
            elif insn.mnemonic == "lea" and insn.op_str.startswith("r8"):
                d = p.read(tgt, 48)
                if d and 32 <= d[0] < 127:
                    n = d.split(b"\0")[0].decode(errors="replace")
                    if pending and n in NATIVES:
                        names.add((n, pending))
                pending = None
    for want in NATIVES:
        hit = next((a for n, a in names if n == want), None)
        if hit:
            ok("%-26s @%#x" % (want, hit))
        else:
            fail("натив %s не найден в регистрации" % want)
            ret = False

    # ── 4. парсер частиц — только клиентский код ────────────────────────────
    print("\n[4] парсер частиц по роли образа:")
    for s in CLIENT_ONLY_STRINGS:
        n = len(p.find_str(s))
        if dedicated:
            if n == 0:
                ok("%-44r отсутствует (dedicated — как в документе)" % s)
            else:
                fail("%-44r ЕСТЬ в dedicated — документ устарел" % s)
                ret = False
        else:
            if n > 0:
                ok("%-44r присутствует (клиентский код)" % s)
            else:
                fail("%-44r нет в клиентском образе — документ устарел" % s)
                ret = False

    print()
    if not ret:
        print("(!) расхождение с dayz/weather.md — обновить документ")
    return 0 if ret else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
