# Сторонний код

GRAFT под GPL-3.0-or-later (см. [LICENSE](LICENSE), [LICENSE-EXCEPTION](LICENSE-EXCEPTION)).
Ниже — чужой код и его условия. Всё перечисленное совместимо с GPLv3.

| компонент | версия | лицензия | попадает в поставку |
|---|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) | v1.3.3 | BSD-2-Clause | **да**, статически в хост |
| Hacker Disassembler Engine 32/64 (внутри MinHook) | — | BSD-2-Clause | **да**, статически в хост |
| [GoogleTest](https://github.com/google/googletest) | v1.15.2 | BSD-3-Clause | нет, только тесты |
| [KR_UTEST](https://github.com/KRdayzmodding) (`tests/mod/SIXW_GRAFT/scripts/3_Game/uTest.c`) | вендорная копия | MIT | нет, только PBO тестового мода |

BSD-2 требует воспроизвести уведомление в документации при поставке **бинарников**.
Поэтому этот файл обязан ехать вместе с `dwmapi.dll` — не только лежать в репозитории.
GoogleTest не распространяется и в поставке не нуждается, но указан для полноты.

---

## MinHook

MinHook - The Minimalistic API Hooking Library for x64/x86
Copyright (C) 2009-2017 Tsuda Kageyu.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

---

## Hacker Disassembler Engine

Портируется внутри MinHook; хост компилирует `src/hde/hde64.c`, поэтому
уведомление обязательно.

Portions of this software are Copyright (c) 2008-2009, Vyacheslav Patkov.

Hacker Disassembler Engine 32 C
Copyright (c) 2008-2009, Vyacheslav Patkov.
All rights reserved.

Hacker Disassembler Engine 64 C
Copyright (c) 2008-2009, Vyacheslav Patkov.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

---

## GoogleTest

Copyright 2008, Google Inc. All rights reserved. BSD-3-Clause — полный текст
в `build/*/\_deps/googletest-src/LICENSE`. Собирается только целями тестов
(`ctest`), в `dwmapi.dll` и плагины не попадает.

---

## KR_UTEST

Фреймворк скриптовых тестов, одним файлом. Лежит копией в
`tests/mod/SIXW_GRAFT/scripts/3_Game/uTest.c`, чтобы тестовый мод не зависел от чужих
модов; чинить его надо в его репозитории, иначе следующая копия затрёт заплатку.
В `dwmapi.dll`, плагины и в поставку библиотеки не попадает — только в PBO тестового мода.

MIT License

Copyright (c) 2026 KRdayzmodding

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
