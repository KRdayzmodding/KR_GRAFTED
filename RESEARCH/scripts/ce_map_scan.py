#!/usr/bin/env python3
"""Перепроверка карты Central Economy (dayz/ce-economy.md) на образе сервера.

Проверяет не «где какие строки», а утверждения документа:
  1. раскладку type entry: nominal +0x54, min +0x58, счётчики +0x2c/+0x30
     читаются в формуле респавна из одного объекта, гейт — бит 5 в +0x88;
  2. писца счётчиков: Estimate-коллбек инкрементирует [entry+0x2c] и
     [+0x30] объекта по [entry+0x98], категории — байт count_in_* на +0x74;
  3. поиск типа по имени: полиномиальный хеш ×0x25 (нижний регистр) и
     open-addressing проба по бакетам 0x18;
  4. привязку CE-объекта: геттер находится ВНУТРИ SpawnRandomLoot
     (call; mov rcx,[rax+0x20]) — не фиксированным отступом по образу;
  5. цикл респавна: гейты LootRespawner::Update (initial-done, root-флаг,
     контекст-гейты, состояние 0xf0000), sweep по хвостовому call и vtable
     (слоты Update/sweep через 4 слота в данных образа).

Образ берётся из GRAFT_SERVER_DIR (каталог сервера с DayZServer_x64.exe),
переопределяется первым аргументом. Разбор PE — pe.py из этой же папки.

    py -3.12 RESEARCH/scripts/ce_map_scan.py [DayZServer_x64.exe]

Выход 0 — все проверки сошлись, 1 — что-то изменилось: карту надо обновлять.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402

H = bytes.fromhex

# ── якоря (как в scan.hpp: строка → lea → функция по .pdata) ─────────────────
ANCHORS = [b"cfgeconomycore", b"types.xml", b"nominal", b"quantmin", b"restock",
           b"economy.xml", b"events.xml", b"[CE]", b"dynamic event"]

# Формула респавна (respawn candidate, chained-фрагмент .pdata).
# Nominal, min и оба счётчика снимаются с одного объекта (rax==rbx — запись).
FORMULA = {
    "nominal [entry+0x54]":   [H("8B7054"), H("448B5B54")],
    "min     [entry+0x58]":   [H("448B7858")],
    "счётчик [entry+0x2c]":   [H("8B532C")],
    "счётчик [entry+0x30]":   [H("8B4B30")],
    "сумма lea eax,[rdx+rcx]": [H("8D040A")],
    "гейт flags [entry+0x88]": [H("8B8088000000"), H("448B8B88000000")],
    "гейт бит 5 shr;test":     [H("C1E805"), H("A801")],
}

# Estimate-коллбек: инкременты счётчиков и флаги count_in_*.
# Окно — якорный фрагмент и смежные chained-фрагменты (0x240 хватает).
WRITER = {
    "флаги count_in_* [+0x74]":          [H("F6437401"), H("F6437402")],
    "инкремент [entry+0x2c]":            [H("FF432C")],
    "второй объект [entry+0x98]":        [H("488B8398000000")],
    "инкремент [объект+0x30]":           [H("FF4030")],
}

# Поиск типа по имени: хеш ×0x25 по нижнему регистру + проба бакетов.
LOOKUP = {
    "хеш имени imul ×0x25":        [H("456BC925")],
    "проба lea rcx,[r10+r10*2]":   [H("4B8D0C52")],
    "бакет cmp [r8+rcx*8+4],0":    [H("41837CC80400")],
}

# Цикл респавна (LootRespawner::Update): гейты на этом объекте (rdi) и на
# втором аргументе — контексте вызова (rbx). Живой замер 2026-11-02: все
# гейты могли быть открыты, а сам Update движком не вызывался.
UPDATE_GATES = {
    "initial-done [this+0x30]":    [H("807F3000")],
    "root-флаг shr eax,4;test":    [H("8B4008C1E804")],
    "ctx-гейт [+0x3e]":            [H("807B3E00")],
    "ctx-таймеры [0x28]>=/+0x24]": [H("F30F1043280F2F4324")],
    "ctx-гейт [+0x3f]":            [H("807B3F00")],
    "ctx-гейт [+0x40]":            [H("807B4000")],
    "состояние and 0xf0000":       [H("2500000F003D00000100")],
}

# Парсер группы mapgroupproto (0x1406bafc0): маски группы (+0x58 usage,
# +0x5c value) и контейнера (+0x28 category, +0x2c tag) — байты живого образа.
GROUP_MASKS = {
    "usage [group+0x58]":    [H("41894558")],
    "value [group+0x5c]":    [H("4189455C")],
    "category [cont+0x28]":  [H("41894728")],
    "tag [cont+0x2c]":       [H("4189472C")],
}

# Парсер cfgspawnabletypes: флаги дескриптора класса [desc+0x24] —
# bts 0xb «есть конфиг», or 2 unique, or 4 hoarder (живой замер).
SPAWNABLE_FLAGS = {
    "сброс flags [desc+0x24]":      [H("C7462400000000")],
    "bts 0xb; mov [desc+0x24]":     [H("0FBAE80B894624")],
    "бит unique (or eax,2)":        [H("83C802")],
    "бит hoarder (or eax,4)":       [H("83C804")],
}

# Парсер <event> (events.xml): запись полей в Event* (rdi) — раскладка
# записи: flags +0x8, nominal +0x14, min +0x18, max +0x1c, lifetime +0x20,
# restock +0x24, saferadius +0x2c (+копия в +0x38), distanceradius +0x30,
# cleanupradius +0x34. Живой дамп реестра совпал с events.xml.
EVENT_FIELDS = {
    "flags   [ev+0x08]":  [H("897708")],
    "nominal [ev+0x14]":  [H("896F14")],
    "min     [ev+0x18]":  [H("44897F18")],
    "max     [ev+0x1c]":  [H("44896F1C")],
    "lifetime [ev+0x20]": [H("F30F114720")],
    "restock [ev+0x24]":  [H("F30F114724")],
    "safe    [ev+0x2c]":  [H("F30F11472C")],
    "safe-коп [ev+0x38]": [H("F30F114738")],
    "dist    [ev+0x30]":  [H("F30F114730")],
    "cleanup [ev+0x34]":  [H("F30F114734")],
}


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


def check_patterns(blob, base, table, what):
    good = True
    for label, pats in table.items():
        hit = next((base + blob.find(pat) for pat in pats if pat in blob), None)
        if hit:
            ok("%-30s @%#x" % (label, hit))
        else:
            fail("%-30s не найдено" % label)
            good = False
    if not good:
        print("  (!) %s: раскладка изменилась — обновить карту" % what)
    return good


def main(argv):
    exe = argv[1] if len(argv) > 1 else None
    if not exe:
        srv = os.environ.get("GRAFT_SERVER_DIR", "")
        exe = os.path.join(srv, "DayZServer_x64.exe") if srv else ""
    if not exe or not os.path.isfile(exe):
        print("образ не найден: задай GRAFT_SERVER_DIR или путь как аргумент")
        return 1
    p = PE(exe)
    print("образ:", exe, "база:", hex(p.base))
    ret = True

    # ── 1. карта функций по якорям (контекст, информационно) ────────────────
    print("\n[1] функции-якоря CE (информационно):")
    byfunc = {}
    for s in ANCHORS:
        for va in p.find_str(s)[:4]:
            for ref in p.riprefs(va):
                f = p.func_at(ref)
                if f:
                    byfunc.setdefault(f[0], set()).add(s.decode(errors="replace"))
    ends = {b: e for b, e in p.funcs()}
    for f in sorted(byfunc):
        print("  %#012x %#6x  %s" % (f, ends[f] - f, ", ".join(sorted(byfunc[f]))))
    print("  всего:", len(byfunc))

    # ── 2. формула респавна: раскладка entry ────────────────────────────────
    print("\n[2] формула респавна (respawn candidate):")
    cand = fn_by_anchor(p, b"[CE][LootRespawner] (%s) :: [RESPAWN CANDIDATE] %s "
                          b"missing:%d [cnt:%d, nom:%d, min:%d]")
    if not cand:
        fail("функция по якорю не найдена")
        ret = False
    else:
        print("  кандидат: %#012x..%#012x (chained-фрагмент .pdata)" % cand)
        ret &= check_patterns(p.read(cand[0], cand[1] - cand[0]), cand[0],
                              FORMULA, "формула")

    # ── 3. писец счётчиков (Estimate-коллбек) ───────────────────────────────
    print("\n[3] писец счётчиков (Estimate):")
    est = fn_by_anchor(p, b"!!! [CE] :: FOUND ThingEffectLight during Estimate")
    if not est:
        fail("функция по якорю не найдена")
        ret = False
    else:
        print("  estimate: %#012x..%#012x (+ chained-окно)" % est)
        ret &= check_patterns(p.read(est[0], 0x240), est[0], WRITER, "писец")

        # ── 4. поиск типа по имени: lookup, вызываемый из Estimate ──────────
        print("\n[4] поиск типа по имени (lookup из Estimate):")
        win = p.read(est[0], 0x240)
        lookup = None
        for i in range(len(win) - 5):
            if win[i] != 0xE8:
                continue
            disp = struct.unpack_from("<i", win, i + 1)[0]
            tgt = est[0] + i + 5 + disp
            # lookup тоже цепочкой .pdata: фрагменты 0x73+0x82 — окно 0x120
            f = p.func_at(tgt)
            if not f or f[0] != tgt:
                continue
            blob = p.read(tgt, 0x120)
            if all(any(pat in blob for pat in pats) for pats in LOOKUP.values()):
                lookup = (tgt, tgt + 0x120)
                break
        if not lookup:
            fail("lookup (хеш ×0x25 + проба бакетов) из Estimate не найден")
            ret = False
        else:
            print("  lookup:   %#012x (окно 0x120, chained-фрагменты)" % lookup[0])
            ret &= check_patterns(p.read(lookup[0], 0x120), lookup[0],
                                  LOOKUP, "lookup")

    # ── 5. CE-объект: геттер внутри SpawnRandomLoot ─────────────────────────
    print("\n[5] геттер CE-объекта (привязка к вызывающей функции):")
    spawn = fn_by_anchor(
        p, b"[CE][SpawnRandomLoot] (%s) :: !!! Attempt to spawn unknown class :: %s")
    if not spawn:
        fail("SpawnRandomLoot по якорю не найден")
        ret = False
    else:
        print("  SpawnRandomLoot: %#012x..%#012x" % spawn)
        blob = p.read(spawn[0], spawn[1] - spawn[0])
        movs = []
        pos = 0
        while True:
            i = blob.find(H("488B4820E8"), pos)  # mov rcx,[rax+0x20]; call …
            if i < 0:
                break
            pos = i + 1
            va_mov = spawn[0] + i
            for back in range(5, 9):  # геттер вызван незадолго до mov
                e8 = va_mov - back
                if p.read(e8, 1) != b"\xe8":
                    continue
                disp = struct.unpack_from("<i", p.read(e8 + 1, 4), 0)[0]
                tgt = e8 + 5 + disp
                g = p.read(tgt, 8)
                if g[:3] == b"\x48\x8b\x05" and g[7:8] == b"\xc3":
                    gdisp = struct.unpack_from("<i", g, 3)[0]
                    movs.append((va_mov, tgt, tgt + 7 + gdisp))
                    break
        if not movs:
            fail("цепочка call;mov rcx,[rax+0x20] → геттер не нашлась")
            ret = False
        for va_mov, tgt, glob in movs:
            ok("mov@%#012x → геттер %#012x → глобал CE %#012x" % (va_mov, tgt, glob))
        # историческая сверка: отступ до lookup-пролога; НЕ паттерн для кода —
        # между сборками 164200/164078 совпал случайно, при перекомпоновке уплывает
        off = spawn[0] - 0x157B0
        pro = p.read(off, 8) == H("4883EC0883792800")
        print("  info отступ -0x157b0 → %#012x, пролог: %s"
              % (off, "сходится" if pro else
                 "НЕ сходится (ожидаемо после перекомпоновки)"))

    # ── 6. цикл респавна: гейты Update + vtable LootRespawner ───────────────
    # Update ищется прологом в окне ±0x2000 от спавн-теста (якорь «exceeded
    # max tests»); sweep — rel32 последнего call в Update; vtable — слоты
    # в не-исполняемых секциях, Update и sweep отстоят на 4 слота.
    print("\n[6] цикл респавна (Update: гейты, sweep, vtable):")
    stest = fn_by_anchor(
        p, b"[CE][LootRespawner] (%s) :: Item exceeded max tests: \"%s\"")
    if not stest:
        fail("спавн-тест по якорю не найден")
        ret = False
    else:
        print("  спавн-тест: %#012x..%#x" % (stest[0], stest[1]))
        lo = stest[0] - 0x2000
        blob = p.read(lo, 0x4000)
        pro = blob.find(H("48895C24085748 83EC20488BDA488BF9E8".replace(" ", "")))
        upd = lo + pro if pro >= 0 else None
        if upd is None:
            fail("пролог Update (mov [rsp+8],rbx; push rdi; sub rsp,0x20; …) не найден")
            ret = False
        else:
            body = p.read(upd, 0x120)
            check_patterns(body, upd, UPDATE_GATES, "гейты Update")
            ok("Update: %#012x (спавн-тест %+#x)" % (upd, upd - stest[0]))
            # sweep: rel32 последнего E8 в теле Update (хвостовой вызов)
            sweep = None
            for i in range(len(body) - 5, 0, -1):
                if body[i] == 0xE8:
                    disp = struct.unpack_from("<i", body, i + 1)[0]
                    sweep = upd + i + 5 + disp
                    break
            if not sweep:
                fail("хвостовой call sweep не найден")
                ret = False
            else:
                ok("sweep (хвостовой call): %#012x" % sweep)
                # vtable: qword-слоты с адресами Update/sweep в данных образа
                def slots(fn_va):
                    pat = struct.pack("<Q", fn_va)
                    out = []
                    for name, sva, vs, ptr, rs in p.secs:
                        if name == ".text" or rs == 0:
                            continue
                        hay = p.d[ptr:ptr + rs]
                        pos = hay.find(pat)
                        while pos >= 0:
                            out.append(p.base + sva + pos)
                            pos = hay.find(pat, pos + 1)
                    return out
                pairs = [(a, b) for a in slots(upd) for b in slots(sweep)
                         if b - a == 0x20]
                if pairs:
                    ok("vtable: слот Update %#012x (+0x58 от базы), sweep +0x78"
                       % pairs[0][0])
                else:
                    fail("слоты vtable (Update и sweep через 4 слота) не найдены")
                    ret = False

    # ── 7. events.xml: парсер <event> и раскладка записи ────────────────────
    print("\n[7] парсер <event> (events.xml):")
    evp = fn_by_anchor(
        p, b"[CE][offlineDB] :: Unknown value of position element when parsing"
          b" event %s.")
    if not evp:
        fail("парсер <event> по якорю не найден")
        ret = False
    else:
        print("  парсер: %#012x..%#x" % (evp[0], evp[1]))
        ret &= check_patterns(p.read(evp[0], evp[1] - evp[0]), evp[0],
                              EVENT_FIELDS, "поля события")

    # ── 8. лимиты: парсер cfglimitsdefinition + держатель [CE+0x10] ─────────
    print("\n[8] лимиты (cfglimitsdefinition):")
    lim = fn_by_anchor(p, b"cfglimitsdefinitionuser.xml")
    if not lim:
        fail("парсер лимитов по якорю не найден")
        ret = False
    else:
        print("  парсер: %#012x..%#x" % (lim[0], lim[1]))
        # вызывающий передаёт держатель: mov rbp,rax; mov r15,[rax+0x10]
        # сразу после call геттера CE — ищем E8 → парсер и проверяем хвост
        tsec = p.text()  # (имя, rva, size, ptr, rawsize) — va нужны полные
        tbase = p.base + tsec[1] if tsec else 0
        tblob = p.read(tbase, tsec[2]) if tsec else b""
        found_holder = False
        i = 0
        while True:
            i = tblob.find(b"\xe8", i)
            if i < 0 or i + 5 > len(tblob):
                break
            disp = struct.unpack_from("<i", tblob, i + 1)[0]
            va = tbase + i
            if va + 5 + disp == lim[0]:
                win = p.read(va - 0x40, 0x60)
                if H("488BE84C8B7810") in win:  # mov rbp,rax; mov r15,[rax+0x10]
                    found_holder = True
                    ok("держатель [CE+0x10] передаётся парсеру (окно 0x%x)" % va)
                    break
            i += 1
        if not found_holder:
            fail("цепочка геттер CE → [CE+0x10] у парсера лимитов не найдена")
            ret = False

    # ── 9. cfgspawnabletypes: флаги дескриптора класса ──────────────────────
    print("\n[9] cfgspawnabletypes (флаги +0x24):")
    spn = fn_by_anchor(p, b"[CE][offlineDB] :: Failed to read cfgspawnabletypes"
                          b" file '%s'.")
    if not spn:
        fail("парсер cfgspawnabletypes по якорю не найден")
        ret = False
    else:
        print("  парсер: %#012x..%#x" % (spn[0], spn[1]))
        ret &= check_patterns(p.read(spn[0], spn[1] - spn[0]), spn[0],
                              SPAWNABLE_FLAGS, "флаги spawnable")

    # ── 10. mapgroupproto: маски групп/контейнеров ──────────────────────────
    print("\n[10] mapgroupproto (маски групп):")
    grp = fn_by_anchor(p, b"keepInvalidPoints")  # якорь загрузчика прототипов
    if not grp:
        fail("загрузчик прототипов групп по якорю не найден")
        ret = False
    else:
        print("  загрузчик: %#012x..%#x" % (grp[0], grp[1]))
        # маски пишутся в парсере группы; паттерны сверяем в смежном окне
        blob = p.read(grp[0], 0x4000)
        good = True
        for label, pats in GROUP_MASKS.items():
            if any(pat in blob for pat in pats):
                ok(label)
            else:
                # парсер группы (0x1406bafc0) — отдельная функция; ищем в ней
                g2 = None
                for va in p.riprefs(grp[0]):
                    f = p.func_at(va)
                    if f:
                        b2 = p.read(f[0], f[1] - f[0])
                        if b2 and any(pat in b2 for pat in pats):
                            g2 = f
                            break
                if g2:
                    ok("%s (в парсере группы %#x)" % (label, g2[0]))
                else:
                    fail("%s не найдено" % label)
                    good = False
        if not good:
            print("  (!) маски групп: раскладка изменилась — обновить карту")

    print("\nитог:", "все проверки сошлись" if ret else "ЕСТЬ РАСХОЖДЕНИЯ — обновить карту")
    return 0 if ret else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
