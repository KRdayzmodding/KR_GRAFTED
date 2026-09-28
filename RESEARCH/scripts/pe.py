# Разбор образа игры БЕЗ IDA: PE, дизассемблер, импорты, поиск ссылок.
#
# Остальные скрипты в этой папке — IDAPython, и без лицензии IDA не запускаются.
# Этот — обычный Python + capstone (`pip install capstone`), и его достаточно, чтобы
# перепроверить разбор после патча игры: снять псевдо-раскладку структуры, найти
# вызов импорта, посчитать ссылки на строку.
#
#   python pe.py <exe> dis   0x14032C5B0 0x200      дизассемблировать
#   python pe.py <exe> iat   0x140DDD728            чей это слот импорта
#   python pe.py <exe> calls GetProcAddress         все места вызова импорта
#   python pe.py <exe> str   "Prototype error '%s'" где лежит строка
#   python pe.py <exe> xref  0x14032C5B0            прямые call/jmp на адрес
#
# Адреса — VA при базе из заголовка (у DayZ это 0x140000000). В рантайме образ
# переезжает: RVA = адрес_из_лога - база_процесса. База считается по любому
# известному импл-адресу, например из журнала graft (`Math.Sqrt impl=...`).
import re
import struct
import sys

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
except ImportError:
    Cs = None


class PE:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        self.opt = opt = pe + 24
        self.base = struct.unpack_from('<Q', d, opt + 24)[0]
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        so = opt + struct.unpack_from('<H', d, pe + 20)[0]
        self.secs = []
        for i in range(nsec):
            o = so + 40 * i
            name = d[o:o + 8].rstrip(b'\0').decode()
            vs, va, rs, ptr = struct.unpack_from('<IIII', d, o + 8)
            self.secs.append((name, va, vs, ptr, rs))
        self.md = Cs(CS_ARCH_X86, CS_MODE_64) if Cs else None

    # ── адреса ────────────────────────────────────────────────────────────────
    def off(self, va):
        rva = va - self.base
        for _, sva, vs, ptr, rs in self.secs:
            if sva <= rva < sva + max(vs, rs):
                return ptr + (rva - sva)
        return None

    def read(self, va, n):
        o = self.off(va)
        return self.d[o:o + n] if o is not None else b''

    def text(self):
        for s in self.secs:
            if s[0] == '.text':
                return s
        return None

    # ── границы функций из .pdata ─────────────────────────────────────────────
    # На x64 у КАЖДОЙ нефреймовой функции есть запись раскрутки: начало, конец и
    # unwind-инфо. Это точные границы без эвристик «искать int3 назад» — и это
    # единственный способ уверенно взять функцию целиком, зная адрес из её середины.
    def funcs(self):
        if getattr(self, '_funcs', None) is not None:
            return self._funcs
        d = self.d
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        rva, size = struct.unpack_from('<II', d, pe + 24 + 112 + 8 * 3)  # IMAGE_DIRECTORY_ENTRY_EXCEPTION
        o = self.off(self.base + rva)
        out = []
        for i in range(size // 12):
            beg, end, _unw = struct.unpack_from('<III', d, o + 12 * i)
            if beg == 0 and end == 0:
                break
            out.append((self.base + beg, self.base + end))
        out.sort()
        self._funcs = out
        return out

    def func_at(self, va):
        """(начало, конец) функции, внутрь которой попал адрес."""
        import bisect
        fs = self.funcs()
        i = bisect.bisect_right(fs, (va, 1 << 62)) - 1
        if i >= 0 and fs[i][0] <= va < fs[i][1]:
            return fs[i]
        return None

    def dis_func(self, va):
        f = self.func_at(va)
        if not f:
            return 'нет записи в .pdata для 0x%x' % va
        return self.dis(f[0], f[1] - f[0])

    # ── дизассемблер ──────────────────────────────────────────────────────────
    def dis(self, va, n=200):
        if not self.md:
            return 'capstone не установлен: pip install capstone'
        return '\n'.join('0x%x  %-8s %s' % (i.address, i.mnemonic, i.op_str)
                         for i in self.md.disasm(self.read(va, n), va))

    def dis_sync(self, va, n=0x200, back=64):
        """Начало функции неизвестно: пробуем сдвиги и берём самый длинный разбор.
        Нужно, когда адрес получен из середины (стек падения, xref)."""
        best = (-1, '')
        for s in range(back):
            out = self.dis(va - back + s, n + back)
            if out.count('\n') > best[0]:
                best = (out.count('\n'), out)
        return best[1]

    # ── импорты ───────────────────────────────────────────────────────────────
    def iat(self):
        """{VA слота: (dll, имя)}"""
        d = self.d
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        imp_rva = struct.unpack_from('<I', d, pe + 24 + 112 + 8)[0]
        out = {}
        i = 0
        while True:
            o = self.off(self.base + imp_rva) + 20 * i
            oft, _ts, _fc, nm, ft = struct.unpack_from('<IIIII', d, o)
            if oft == 0 and nm == 0 and ft == 0:
                break
            dll = d[self.off(self.base + nm):].split(b'\0')[0].decode()
            t = self.off(self.base + (oft or ft))
            j = 0
            while True:
                e = struct.unpack_from('<Q', d, t + 8 * j)[0]
                if e == 0:
                    break
                if e >> 63:
                    name = 'ord_%d' % (e & 0xffff)
                else:
                    hn = self.off(self.base + (e & 0x7fffffff))
                    name = d[hn + 2:].split(b'\0')[0].decode()
                out[self.base + ft + 8 * j] = (dll, name)
                j += 1
            i += 1
        return out

    def import_calls(self, name):
        """Все `call/jmp qword ptr [rip+disp]`, ведущие в импорт с таким именем."""
        slots = {va for va, (_dll, nm) in self.iat().items() if nm == name}
        _, tva, _vs, tptr, trs = self.text()
        buf = self.d[tptr:tptr + trs]
        out = []
        for m in re.finditer(rb'\xff[\x15\x25]', buf):
            o = m.start()
            if o + 6 > len(buf):
                continue
            disp = struct.unpack_from('<i', buf, o + 2)[0]
            if self.base + tva + o + 6 + disp in slots:
                out.append(self.base + tva + o)
        return out

    # ── поиск ─────────────────────────────────────────────────────────────────
    def find_str(self, s):
        if isinstance(s, str):
            s = s.encode()
        out = []
        for _n, sva, _vs, ptr, rs in self.secs:
            blob = self.d[ptr:ptr + rs]
            for m in re.finditer(re.escape(s), blob):
                out.append(self.base + sva + m.start())
        return out

    def xrefs(self, target):
        """Прямые call/jmp rel32 на адрес."""
        _, tva, _vs, tptr, trs = self.text()
        buf = self.d[tptr:tptr + trs]
        out = []
        for m in re.finditer(rb'[\xe8\xe9]', buf):
            o = m.start()
            if o + 5 > len(buf):
                continue
            disp = struct.unpack_from('<i', buf, o + 1)[0]
            if self.base + tva + o + 5 + disp == target:
                out.append(self.base + tva + o)
        return out

    def riprefs(self, target):
        """Любая rip-относительная ссылка на адрес (lea/mov/cmp/...).
        Инструкция кончается через 0..4 байта после disp32 (там может быть imm),
        поэтому проверяем все пять вариантов длины хвоста."""
        _, tva, _vs, tptr, trs = self.text()
        buf = self.d[tptr:tptr + trs]
        out = []
        for o in range(len(buf) - 4):
            disp = struct.unpack_from('<i', buf, o)[0]
            tail = target - (self.base + tva + o + 4 + disp)
            if 0 <= tail <= 4:
                out.append(self.base + tva + o)
        return out


def main(argv):
    if len(argv) < 3:
        print(__doc__ or '')
        print('usage: pe.py <exe> {dis|iat|calls|str|xref} <arg> [len]')
        return 1
    p = PE(argv[1])
    cmd = argv[2]
    if cmd == 'dis':
        n = int(argv[4], 0) if len(argv) > 4 else 0x100
        print(p.dis(int(argv[3], 0), n))
    elif cmd == 'iat':
        print(p.iat().get(int(argv[3], 0)))
    elif cmd == 'calls':
        for a in p.import_calls(argv[3]):
            print(hex(a))
    elif cmd == 'str':
        for a in p.find_str(argv[3]):
            print(hex(a))
    elif cmd == 'xref':
        for a in p.xrefs(int(argv[3], 0)):
            print(hex(a))
    else:
        print('неизвестная команда', cmd)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
