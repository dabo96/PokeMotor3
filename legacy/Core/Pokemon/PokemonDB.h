#pragma once
#include "PokemonData.h"
#include <unordered_map>
#include <string>
#include <optional>

namespace pokemon {

class PokemonDB {
public:
    static PokemonDB& Instance();

    bool LoadSpecies(const std::string& jsonPath);
    bool LoadMoves(const std::string& jsonPath);

    const SpeciesData* GetSpecies(const std::string& name) const;
    const MoveData* GetMove(const std::string& name) const;

    // Create a wild Pokemon of given species and level
    Pokemon CreateWild(const std::string& species, int level) const;

    // Create a starter Pokemon for the player
    Pokemon CreateStarter(const std::string& species, int level = 5) const;

    const std::unordered_map<std::string, SpeciesData>& AllSpecies() const { return mSpecies; }
    const std::unordered_map<std::string, MoveData>& AllMoves() const { return mMoves; }

    bool IsLoaded() const { return mLoaded; }

private:
    PokemonDB() = default;
    std::unordered_map<std::string, SpeciesData> mSpecies;
    std::unordered_map<std::string, MoveData> mMoves;
    bool mLoaded = false;

    void SetMovePP(Pokemon& pkmn) const;
};

} // namespace pokemon
