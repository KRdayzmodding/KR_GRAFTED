#pragma once
#include <cstdint>
#include <string>

// Домен graft-модуля: чистая логика натива SeraphGraftPing.
// Держим её отдельно от DLL/движка, чтобы покрыть тестами без запуска игры.
namespace seraph {

// Магия, которую скрипт-тест воспроизводит на своей стороне: reply == token ^ kMagic.
// Совпадение доказывает, что аргумент дошёл до C++ и результат вернулся в скрипт.
inline constexpr std::int32_t kMagic = 0x0005E1AF;  // "SE1AF" ~ Seraph

// Enforce int — 32-битный знаковый, поэтому считаем в int32 и переносим переполнение как есть.
constexpr std::int32_t ping_reply(std::int32_t token) noexcept {
    return token ^ kMagic;
}

// Строка-доказательство, которую натив пишет "наружу игры" (через graft::log).
std::string proof_line(std::int32_t token, std::int32_t reply);

}  // namespace seraph
