# KR_GRAFT

[![CI](https://github.com/KRdayzmodding/KR_GRAFTED/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/KRdayzmodding/KR_GRAFTED/actions/workflows/ci.yml)
[![License: GPL-3.0-or-later with plugin exception](https://img.shields.io/badge/license-GPL--3.0--or--later%20%2B%20plugin%20exception-blue)](#license)

**One C++ entry — both an engine native and a `proto native` for script. No hardcoded addresses.**

**[Contributing](CONTRIBUTING.md)** · **[Discussions](https://github.com/KRdayzmodding/KR_GRAFTED/discussions)** · **[Builds](https://github.com/KRdayzmodding/KR_GRAFTED/releases)** · **[Changelog](CHANGELOG.md)** · **[По-русски](README.md)**

> The full documentation is in Russian: [README.md](README.md). This page is the short
> version for people who just found the project.

GRAFT grafts C++ onto DayZ's Enfusion engine. You don't call the engine from the outside
and you don't patch it by address: your code becomes part of it, and the engine calls you
directly. The graft point is found at runtime by the *names* of vanilla natives — so a
game patch moves addresses, it doesn't break the graft.

## A whole mod

A mod that keeps players with junk nicknames off the server. Enforce has no regular
expressions at all; C++ has them in the standard library. Three files — and the build
writes the middle one.

**C++ — `src/plugin.cpp`, the entire plugin:**

```cpp
#include <regex>
#include <string_view>

#include <graft/native.hpp>

GRAFT_PLUGIN("MYMOD", 1);                       // DLL passport: name and version

// A plain C++ function with plain types: string_view arrives without a copy.
bool IsValidNick(std::string_view nick) {
    static const std::regex ok{R"(^[A-Za-z][A-Za-z0-9_]{2,15}$)"};
    return std::regex_match(nick.begin(), nick.end(), ok);
}

GRAFT_BINDINGS("3_Game") {                      // registration: script name and module
    bind.global<&IsValidNick>("IsValidNick");
}
```

**Declaration — `mod/MYMOD/scripts/3_Game/grafted_natives_MYMOD.c`, printed by the build:**

```c
proto native bool IsValidNick(string p0);
```

**Script — `mod/MYMOD/scripts/5_Mission/mymod.c`, an ordinary DayZ mod:**

```c
modded class MissionServer
{
    override void InvokeOnConnect(PlayerBase player, PlayerIdentity identity)
    {
        super.InvokeOnConnect(player, identity);

        if (!IsValidNick(identity.GetPlainName()))
            GetGame().DisconnectPlayer(identity);
    }
}
```

From the script side `IsValidNick` is indistinguishable from a vanilla native. From the
C++ side it is an ordinary function the engine calls directly, by address. Need code on
every frame too? That's `GRAFT_ON_TICK(dt) { ... }` — see [examples/players](examples/players/).

That ships as:

```
<game>/dwmapi.dll                                host, one per installation
@MYMOD/addons/MYMOD.pbo                          your mod, as usual
@MYMOD/grafted/MYMOD.grafted.dll                 plugin — next to addons/
@MYMOD/scripts/3_Game/grafted_natives_MYMOD.c    declarations, printed by the build
```

Script declarations are a build artifact, like protobuf's `.pb.cc`: nobody writes them by
hand, so they cannot drift from the C++.

## Requirements

Windows, Visual Studio 2022 (for the Windows SDK and STL), clang-cl 17+, CMake 3.30+,
Ninja. Build from the **x64 Native Tools Command Prompt for VS** — clang-cl takes the SDK
paths from that shell's environment.

```bat
cmake --preset release
cmake --build --preset release
```

## Your own plugin

The whole mod project is a single CMakeLists:

```cmake
file(DOWNLOAD https://raw.githubusercontent.com/KRdayzmodding/KR_GRAFTED/main/cmake/graft.boot.cmake
     "${CMAKE_BINARY_DIR}/graft.boot.cmake")
include("${CMAKE_BINARY_DIR}/graft.boot.cmake")
graft_import(graft https://github.com/KRdayzmodding/KR_GRAFTED TAG v0.1.0)

graft_plugin(mymod NAME MYMOD VERSION 1 SOURCES src/plugin.cpp MODULES 3_Game)
```

Pin a tag, never `main` — that's where graft itself is developed. The host verifies
`GRAFT_ABI_VERSION` and `GRAFT_LAYOUT_VERSION` at load time and rejects a plugin built
against different ones, so ABI bumps are announced in [CHANGELOG.md](CHANGELOG.md).

Start from [examples/hello](examples/hello/).

## License

**GPL-3.0-or-later** ([LICENSE](LICENSE)) plus the **GRAFT plugin exception 1.0**
([LICENSE-EXCEPTION](LICENSE-EXCEPTION)), an additional permission under GPL §7 —
mechanically the same as OpenJDK's Classpath Exception or GCC's Runtime Library Exception.

The line it draws:

| what you do | what you owe |
|---|---|
| write a mod on GRAFT — closed, paid, anything | **nothing** |
| inline `graft/*.hpp` into your plugin, load through the host ABI | nothing |
| ship the generated `grafted_natives_MYMOD.c` inside your PBO | nothing |
| modify platform files and distribute the result | GPLv3 on all of it, fork included |
| write your own host instead | GPLv3 — that's not a plugin, the exception doesn't apply |

Making money on what you build **on top of** GRAFT is unrestricted; closing the platform
itself is not. Note that the name is not licensed (GPLv3 §7e), and Bohemia's own rules on
DayZ mod monetization apply on top of all this — GRAFT's license neither grants nor
overrides them.

Third-party code (MinHook and HDE, both BSD-2, linked into `dwmapi.dll`):
[THIRD_PARTY.md](THIRD_PARTY.md).

## Contributing

[CONTRIBUTING.md](CONTRIBUTING.md) — environment, build, the test-first rule, what will
not be merged. [docs/CLA.md](docs/CLA.md) — one-time contributor agreement, signed by a
bot on your first PR. [SECURITY.md](SECURITY.md) — the host runs inside the game and
server process, so vulnerabilities go to a private channel, never to the issue tracker.

Questions about *using* GRAFT belong in
[Discussions](https://github.com/KRdayzmodding/KR_GRAFTED/discussions), not in issues.
