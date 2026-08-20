#!/usr/bin/env python3
"""Следующий rc-тег для черновика релиза: v<версия проекта>-rc.<N+1>.

Версия берётся из `project(graft VERSION ...)` — единственного места, где она
записана. Счётчик rc — из имени последнего релиза (черновики тоже считаются).

    python3 .github/next-version.py "$(gh release list -L 1 --json name --jq '.[0].name // ""')"

Печатает тег в stdout и, если есть GITHUB_ENV, кладёт его туда как TAG_NAME.
"""

import os
import re
import sys


def project_version(cmakelists: str) -> str:
    m = re.search(r"project\s*\(\s*graft\s+VERSION\s+(\d+\.\d+\.\d+)", cmakelists)
    if not m:
        raise SystemExit("не нашёл project(graft VERSION ...) в CMakeLists.txt")
    return m.group(1)


def next_rc(version: str, latest: str) -> str:
    base = "v" + version
    latest = latest.strip()
    if latest == base:
        raise SystemExit(f"{base} уже выпущен — подними project(graft VERSION) в CMakeLists.txt")
    m = re.fullmatch(re.escape(base) + r"-rc\.(\d+)", latest)
    return f"{base}-rc.{int(m.group(1)) + 1}" if m else f"{base}-rc.1"


# Проверка на месте: правило нумерации ломается молча, а замечают это через месяц.
assert next_rc("0.0.1", "") == "v0.0.1-rc.1"
assert next_rc("0.0.1", "v0.0.1-rc.7") == "v0.0.1-rc.8"
assert next_rc("0.0.1", "v0.0.1-rc.9 ") == "v0.0.1-rc.10"
assert next_rc("0.1.0", "v0.0.1-rc.7") == "v0.1.0-rc.1"  # версию проекта подняли
assert next_rc("0.0.1", "v0.0.2-rc.1") == "v0.0.1-rc.1"  # чужой ряд не продолжаем

if __name__ == "__main__":
    latest = sys.argv[1] if len(sys.argv) > 1 else ""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(root, "CMakeLists.txt"), encoding="utf-8") as f:
        tag = next_rc(project_version(f.read()), latest)

    print(tag)
    if os.getenv("GITHUB_ENV"):
        with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8") as env:
            env.write(f"TAG_NAME={tag}\n")
