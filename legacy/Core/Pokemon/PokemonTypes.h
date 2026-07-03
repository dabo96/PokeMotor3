#pragma once
#include <string>
#include <unordered_map>
#include <cstdint>

namespace pokemon {

enum class Type : uint8_t {
    Normal = 0,
    Fire,
    Water,
    Grass,
    Electric,
    Ground,
    Flying,
    Poison,
    Bug,
    COUNT
};

inline const char* TypeToString(Type t) {
    switch (t) {
        case Type::Normal:   return "Normal";
        case Type::Fire:     return "Fire";
        case Type::Water:    return "Water";
        case Type::Grass:    return "Grass";
        case Type::Electric: return "Electric";
        case Type::Ground:   return "Ground";
        case Type::Flying:   return "Flying";
        case Type::Poison:   return "Poison";
        case Type::Bug:      return "Bug";
        default:             return "???";
    }
}

inline Type StringToType(const std::string& s) {
    static const std::unordered_map<std::string, Type> map = {
        {"Normal", Type::Normal}, {"Fire", Type::Fire}, {"Water", Type::Water},
        {"Grass", Type::Grass}, {"Electric", Type::Electric}, {"Ground", Type::Ground},
        {"Flying", Type::Flying}, {"Poison", Type::Poison}, {"Bug", Type::Bug}
    };
    auto it = map.find(s);
    return it != map.end() ? it->second : Type::Normal;
}

// Type effectiveness multiplier: attacker type vs defender type
// Returns 2.0 (super effective), 1.0 (normal), 0.5 (not very), 0.0 (immune)
inline float GetTypeEffectiveness(Type attack, Type defend) {
    static const float table[9][9] = {
        //          Nor  Fir  Wat  Gra  Ele  Gnd  Fly  Poi  Bug
        /* Nor */ { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 },
        /* Fir */ { 1.0, 0.5, 0.5, 2.0, 1.0, 1.0, 1.0, 1.0, 2.0 },
        /* Wat */ { 1.0, 2.0, 0.5, 0.5, 1.0, 2.0, 1.0, 1.0, 1.0 },
        /* Gra */ { 1.0, 0.5, 2.0, 0.5, 1.0, 2.0, 0.5, 0.5, 0.5 },
        /* Ele */ { 1.0, 1.0, 2.0, 0.5, 0.5, 0.0, 2.0, 1.0, 1.0 },
        /* Gnd */ { 1.0, 2.0, 1.0, 0.5, 2.0, 1.0, 0.0, 2.0, 0.5 },
        /* Fly */ { 1.0, 1.0, 1.0, 2.0, 0.5, 1.0, 1.0, 1.0, 2.0 },
        /* Poi */ { 1.0, 1.0, 1.0, 2.0, 1.0, 0.5, 1.0, 0.5, 1.0 },
        /* Bug */ { 1.0, 0.5, 1.0, 2.0, 1.0, 1.0, 0.5, 0.5, 1.0 },
    };
    return table[static_cast<int>(attack)][static_cast<int>(defend)];
}

} // namespace pokemon
