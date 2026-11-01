# Перепроверка разбора песчаной буры по образу игры БЕЗ IDA (python + capstone).
#
# Оперирует теми же якорями, что держат выводы RESEARCH/dayz/weather.md: строки
# секции Sandstorm в мировом конфиге, регистрация методов SandstormController и
# натив Start. Запуск:
#
#   python sandstorm.py <DayZDiag_x64.exe|DayZServer_x64.exe>
#
# Проверялось на DayZDiag_x64.exe 1.30.164.200 (sha256 a500be80…). После патча игры
# адреса уедут — якоря и структура должны остаться; расхождение = повод перечитать
# разбор в weather.md.

import importlib.util
import os
import re
import struct
import sys

spec = importlib.util.spec_from_file_location("pe", os.path.join(os.path.dirname(__file__), "pe.py"))
pe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pe)

import capstone  # noqa: E402


def resolve_str(P, va, minlen=3):
    d = P.read(va, 64)
    if d and 32 <= d[0] < 127:
        s = d.split(b"\0")[0]
        if len(s) >= minlen and all(32 <= c < 127 for c in s):
            return s.decode()
    return None


def main(exe):
    P = pe.PE(exe)
    c = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

    print(f"== образ: {os.path.basename(exe)}, base {P.base:#x}")

    print("\n== якорные строки")
    for s in [b"SandstormController\x00", b"startPosition\x00", b"endPosition\x00",
              b"sandstormIntensity\x00", b"#particlesource",
              b"Graphics/Particles/sandstorm/sandstorm",
              b"Sandstorm state: Position", b"Sandstorm\x00"]:
        hits = P.find_str(s)
        print(f"  {s!r}: {len(hits)} шт. {[hex(h) for h in hits[:3]]}")

    # Парсер свойств: на startPosition ссылается ровно одна функция.
    parser = None
    for va in P.find_str(b"startPosition\x00"):
        refs = P.riprefs(va)
        for r in refs:
            f = P.func_at(r)
            if f:
                parser = f
                print(f"\n== парсер свойств: {f[0]:#x}..{f[1]:#x} (ссылка на startPosition @ {r:#x})")
    if not parser:
        print("\n!! парсер по якорю startPosition не найден — якорь уехал, смотреть в IDA")
        return 1

    # Регистрация методов: класс SandstormController + пары lea r8(имя)/lea r9(импл).
    reg = None
    for va in P.find_str(b"SandstormController\x00"):
        for r in P.riprefs(va):
            f = P.func_at(r)
            if f and f != parser:
                reg = f
                print(f"== регистрация методов: {f[0]:#x}..{f[1]:#x}")
    if reg:
        print("   карта имя -> импл:")
        code = P.read(reg[0], reg[1] - reg[0])
        pending_impl = None
        for i in c.disasm(code, reg[0]):
            m = re.search(r"rip ([+-]) (0x[0-9a-f]+)", i.op_str)
            if not m:
                continue
            disp = int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)
            tgt = i.address + i.size + disp
            # порядок в регистрации: lea r9 (импл) СНАЧАЛА, затем lea r8 (имя метода)
            if i.mnemonic == "lea" and i.op_str.startswith("r9"):
                pending_impl = tgt
            elif i.mnemonic == "lea" and i.op_str.startswith("r8"):
                s = resolve_str(P, tgt)
                if s and pending_impl is not None and s != "SandstormController":
                    print(f"     {s:28s} {pending_impl:#x}")
                pending_impl = None

    # Гейты натива Start: серверный контекст + «позиция == 0», маршрута там нет.
    if reg:
        code = P.read(reg[0], reg[1] - reg[0])
        start_impl = None
        pending_impl = None
        for i in c.disasm(code, reg[0]):
            m = re.search(r"rip ([+-]) (0x[0-9a-f]+)", i.op_str)
            if not m:
                continue
            disp = int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)
            tgt = i.address + i.size + disp
            if i.mnemonic == "lea" and i.op_str.startswith("r9"):
                pending_impl = tgt
            elif i.mnemonic == "lea" and i.op_str.startswith("r8"):
                s = resolve_str(P, tgt)
                if s == "Start" and pending_impl is not None:
                    start_impl = pending_impl
                pending_impl = None
        if start_impl:
            f = P.func_at(start_impl)
            if f:
                print(f"\n== натив Start {f[0]:#x}..{f[1]:#x}")
                body = P.read(f[0], f[1] - f[0])
                for i in c.disasm(body, f[0]):
                    mark = ""
                    if i.mnemonic in ("ucomiss", "comiss"):
                        mark = "   <- гейт «магнитуда/позиция == 0»"
                    if i.mnemonic == "call":
                        mark = "   ; call"
                    print(f"  {i.address:#x}  {i.mnemonic} {i.op_str}{mark}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
