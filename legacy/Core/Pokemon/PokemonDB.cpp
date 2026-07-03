#include "PokemonDB.h"
#include <json.hpp>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

namespace pokemon {

PokemonDB& PokemonDB::Instance() {
    static PokemonDB instance;
    return instance;
}

bool PokemonDB::LoadSpecies(const std::string& jsonPath) {
    std::ifstream file(jsonPath);
    if (!file.is_open()) {
        std::cerr << "[PokemonDB] Failed to open: " << jsonPath << std::endl;
        return false;
    }

    try {
        json data = json::parse(file);
        for (auto& [key, val] : data.items()) {
            SpeciesData sp;
            sp.id = val.value("id", 0);
            sp.name = key;
            sp.type1 = StringToType(val.value("type1", "Normal"));
            sp.type2 = StringToType(val.value("type2", val.value("type1", "Normal")));

            if (val.contains("baseStats")) {
                auto& bs = val["baseStats"];
                sp.baseStats.hp       = bs.value("hp", 45);
                sp.baseStats.attack   = bs.value("attack", 49);
                sp.baseStats.defense  = bs.value("defense", 49);
                sp.baseStats.spAttack = bs.value("spAttack", 65);
                sp.baseStats.spDefense = bs.value("spDefense", 65);
                sp.baseStats.speed    = bs.value("speed", 45);
            }

            if (val.contains("learnset") && val["learnset"].is_array()) {
                for (auto& entry : val["learnset"]) {
                    int lv = entry.value("level", 1);
                    std::string move = entry.value("move", "");
                    if (!move.empty()) sp.learnset.push_back({lv, move});
                }
            }

            sp.evolutionLevel  = val.value("evolutionLevel", 0);
            sp.evolutionTarget = val.value("evolutionTarget", "");
            sp.catchRate       = val.value("catchRate", 255);
            sp.expYield        = val.value("expYield", 64);

            mSpecies[key] = std::move(sp);
        }

        std::cout << "[PokemonDB] Loaded " << mSpecies.size() << " species" << std::endl;
    } catch (const json::exception& e) {
        std::cerr << "[PokemonDB] JSON parse error: " << e.what() << std::endl;
        return false;
    }

    return true;
}

bool PokemonDB::LoadMoves(const std::string& jsonPath) {
    std::ifstream file(jsonPath);
    if (!file.is_open()) {
        std::cerr << "[PokemonDB] Failed to open: " << jsonPath << std::endl;
        return false;
    }

    try {
        json data = json::parse(file);
        for (auto& [key, val] : data.items()) {
            MoveData mv;
            mv.name = key;
            mv.type = StringToType(val.value("type", "Normal"));

            std::string cat = val.value("category", "Physical");
            if (cat == "Special") mv.category = MoveCategory::Special;
            else if (cat == "Status") mv.category = MoveCategory::Status;
            else mv.category = MoveCategory::Physical;

            mv.power    = val.value("power", 40);
            mv.accuracy = val.value("accuracy", 100);
            mv.pp       = val.value("pp", 35);
            mv.description = val.value("description", "");

            mMoves[key] = std::move(mv);
        }

        std::cout << "[PokemonDB] Loaded " << mMoves.size() << " moves" << std::endl;
    } catch (const json::exception& e) {
        std::cerr << "[PokemonDB] JSON parse error: " << e.what() << std::endl;
        return false;
    }

    mLoaded = !mSpecies.empty() && !mMoves.empty();
    return true;
}

const SpeciesData* PokemonDB::GetSpecies(const std::string& name) const {
    auto it = mSpecies.find(name);
    return it != mSpecies.end() ? &it->second : nullptr;
}

const MoveData* PokemonDB::GetMove(const std::string& name) const {
    auto it = mMoves.find(name);
    return it != mMoves.end() ? &it->second : nullptr;
}

Pokemon PokemonDB::CreateWild(const std::string& species, int level) const {
    Pokemon p;
    p.speciesName = species;
    p.level = level;
    p.RandomizeIVs();

    auto* sp = GetSpecies(species);
    if (sp) {
        p.CalculateStats(sp->baseStats);
        p.currentHP = p.maxHP;
        p.exp = p.ExpForLevel(level);
        p.LearnMovesForLevel(sp->learnset);
        SetMovePP(p);
    }
    return p;
}

Pokemon PokemonDB::CreateStarter(const std::string& species, int level) const {
    Pokemon p = CreateWild(species, level);
    // Starters get slightly better IVs (min 10)
    auto boost = [](int& iv) { if (iv < 10) iv = 10; };
    boost(p.ivHP); boost(p.ivAtk); boost(p.ivDef);
    boost(p.ivSpAtk); boost(p.ivSpDef); boost(p.ivSpd);

    auto* sp = GetSpecies(species);
    if (sp) {
        p.CalculateStats(sp->baseStats);
        p.currentHP = p.maxHP;
    }
    return p;
}

void PokemonDB::SetMovePP(Pokemon& pkmn) const {
    for (int i = 0; i < 4; i++) {
        if (pkmn.moves[i].empty()) {
            pkmn.currentPP[i] = 0;
            continue;
        }
        auto* mv = GetMove(pkmn.moves[i]);
        pkmn.currentPP[i] = mv ? mv->pp : 35;
    }
}

} // namespace pokemon
