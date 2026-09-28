# Запрет зашитых адресов движка — правило уровня проекта.
#
# Адрес внутри exe уезжает при КАЖДОМ патче игры; смещение поля в структуре — почти
# никогда. Поэтому graft ищет всё от якорей (имена нативов и скриптовых классов) и
# сверяет найденное байтовой сигнатурой, а зашитых RVA не держит вовсе. Этот скрипт
# следит, чтобы так и оставалось: зашитый адрес ломает мод на первом же патче игры, и
# ломает молча.
#
# Что считается адресом: шестнадцатеричная константа в 6 или 7 цифр. Именно там живут
# RVA игры (0x100000..0xFFFFFFF). Всё остальное проходит: смещения полей короче,
# а движковые теги типов и коды исключений — восемь цифр (0x20000000, 0xC0000374).
#
#   python .github/no-addresses.py            проверить SRC/ и INCLUDE/
#   python .github/no-addresses.py путь ...   проверить что-то ещё
import pathlib
import re
import sys

ADDRESS = re.compile(r"\b0[xX][0-9A-Fa-f]{6,7}\b")
SUFFIXES = {".c", ".cpp", ".h", ".hpp"}
DEFAULT = ["SRC", "INCLUDE"]

# Строку можно оставить, дописав к ней этот маркер и объяснив почему.
ALLOW = "no-addresses: ok"


def main(argv: list[str]) -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    roots = [pathlib.Path(p) for p in (argv[1:] or DEFAULT)]
    bad: list[str] = []
    for root in roots:
        for path in sorted(root.rglob("*")):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for number, line in enumerate(text.splitlines(), 1):
                if ALLOW in line:
                    continue
                # Комментарии не проверяем: разбор движка их и состоит из адресов,
                # а опасен только адрес, попавший в КОД.
                code = line.split("//", 1)[0]
                for hit in ADDRESS.findall(code):
                    bad.append(f"{path}:{number}: зашитый адрес {hit} — {line.strip()}")

    for line in bad:
        print(line)
    if bad:
        print()
        print(f"адресов найдено: {len(bad)}")
        print("Адрес движка нельзя зашивать: он уедет на первом же патче игры.")
        print("Ищите от якоря (имя натива или класса) и сверяйте сигнатурой — см.")
        print("INCLUDE/graft/scan.hpp.")
        return 1
    print("зашитых адресов нет")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
