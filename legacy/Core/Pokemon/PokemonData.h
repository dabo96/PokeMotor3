#pragma once
#include "PokemonTypes.h"
#include <string>
#include <vector>
#include <array>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <random>

namespace pokemon {

// ─── Base Stats (per species) ────────────────────────────────────────────────

struct BaseStats {
    int hp = 45;
    int attack = 49;
    int defense = 49;
    int spAttack = 65;
    int spDefense = 65;
    int speed = 45;
};

// ─── Move Definition ─────────────────────────────────────────────────────────

enum class MoveCategory : uint8_t { Physical, Special, Status };

struct MoveData {
    std::string name;
    Type type = Type::Normal;
    MoveCategory category = MoveCategory::Physical;
    int power = 40;         // 0 for status moves
    int accuracy = 100;     // percentage
    int pp = 35;            // max uses
    std::string description;
};

// ─── Species Definition ──────────────────────────────────────────────────────

struct SpeciesData {
    int id = 0;
    std::string name;
    Type type1 = Type::Normal;
    Type type2 = Type::Normal;  // same as type1 if single-type
    BaseStats baseStats;
    std::vector<std::pair<int, std::string>> learnset; // level → move name
    int evolutionLevel = 0;     // 0 = doesn't evolve
    std::string evolutionTarget; // species name to evolve into
    int catchRate = 255;        // 0-255, higher = easier
    int expYield = 64;          // base EXP given on defeat
};

// ─── Individual Pokemon Instance ─────────────────────────────────────────────

struct Pokemon {
    std::string speciesName;
    std::string nickname;       // empty = use species name
    int level = 5;

    // IVs (0-31, random at creation)
    int ivHP = 0, ivAtk = 0, ivDef = 0, ivSpAtk = 0, ivSpDef = 0, ivSpd = 0;

    // EVs (0-255 per stat, 510 total cap)
    int evHP = 0, evAtk = 0, evDef = 0, evSpAtk = 0, evSpDef = 0, evSpd = 0;

    // Moves (max 4, empty string = no move)
    std::array<std::string, 4> moves;
    std::array<int, 4> currentPP; // remaining PP per move

    // Calculated stats (recalculated on level up / EV gain)
    int maxHP = 0;
    int attack = 0, defense = 0, spAttack = 0, spDefense = 0, speed = 0;

    // Runtime state
    int currentHP = 0;
    int exp = 0;

    bool IsAlive() const { return currentHP > 0; }
    const std::string& DisplayName() const { return nickname.empty() ? speciesName : nickname; }

    // Calculate stats from base + IV + EV + level (Gen III formula simplified)
    void CalculateStats(const BaseStats& base) {
        auto calcStat = [&](int baseStat, int iv, int ev) -> int {
            return ((2 * baseStat + iv + ev / 4) * level / 100) + 5;
        };
        maxHP = ((2 * base.hp + ivHP + evHP / 4) * level / 100) + level + 10;
        attack    = calcStat(base.attack,    ivAtk,   evAtk);
        defense   = calcStat(base.defense,   ivDef,   evDef);
        spAttack  = calcStat(base.spAttack,  ivSpAtk, evSpAtk);
        spDefense = calcStat(base.spDefense, ivSpDef, evSpDef);
        speed     = calcStat(base.speed,     ivSpd,   evSpd);
    }

    // EXP needed for next level (medium-fast group)
    int ExpForLevel(int lv) const { return lv * lv * lv; }
    int ExpToNextLevel() const { return ExpForLevel(level + 1) - exp; }

    // Generate random IVs
    void RandomizeIVs() {
        static std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(0, 31);
        ivHP = dist(rng); ivAtk = dist(rng); ivDef = dist(rng);
        ivSpAtk = dist(rng); ivSpDef = dist(rng); ivSpd = dist(rng);
    }

    // Learn moves appropriate for level from learnset
    void LearnMovesForLevel(const std::vector<std::pair<int, std::string>>& learnset) {
        // Collect all moves learnable at or below current level
        std::vector<std::string> available;
        for (auto& [lv, move] : learnset) {
            if (lv <= level) available.push_back(move);
        }
        // Take the last 4 (most recent level-up moves)
        int start = std::max(0, static_cast<int>(available.size()) - 4);
        for (int i = start; i < static_cast<int>(available.size()); i++) {
            moves[i - start] = available[i];
            currentPP[i - start] = 99; // PP set properly when MoveData is loaded
        }
    }
};

} // namespace pokemon
