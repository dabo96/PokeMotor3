// Core/StringID.h — compara strings por hash (FNV-1a), no por contenido.
// Diseño: MotorGrafico_Core.md (Tipos y handles). Claves legibles
// ("player_idle") con la velocidad de un int. Base para buscar assets, nombrar
// eventos y referenciar contenido en la Database del juego.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace pk {

constexpr uint32_t fnv1a(const char* s, uint32_t h = 2166136261u) {
    return (*s == 0) ? h
                     : fnv1a(s + 1, (h ^ static_cast<uint32_t>(static_cast<unsigned char>(*s))) * 16777619u);
}

struct StringID {
    uint32_t hash = 0;

    constexpr StringID() = default;
    constexpr StringID(const char* s) : hash(fnv1a(s)) {}

    constexpr bool operator==(StringID o) const { return hash == o.hash; }
    constexpr bool operator!=(StringID o) const { return hash != o.hash; }
};

constexpr StringID operator""_sid(const char* s, std::size_t) { return StringID(s); }

}  // namespace pk

// Permite usar StringID como clave de unordered_map.
template <>
struct std::hash<pk::StringID> {
    std::size_t operator()(pk::StringID s) const noexcept { return s.hash; }
};
