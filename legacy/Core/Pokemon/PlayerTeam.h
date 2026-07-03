#pragma once
#include "PokemonData.h"
#include <array>
#include <optional>

namespace pokemon {

class PlayerTeam {
public:
    static PlayerTeam& Instance();

    static constexpr int MAX_TEAM = 6;

    bool AddPokemon(const Pokemon& pokemon);
    void RemovePokemon(int slot);
    void SwapSlots(int a, int b);

    Pokemon* GetPokemon(int slot);
    const Pokemon* GetPokemon(int slot) const;
    Pokemon* GetFirstAlive(); // First Pokemon with HP > 0
    int GetFirstAliveIndex() const;

    int Size() const { return mSize; }
    bool IsFull() const { return mSize >= MAX_TEAM; }
    bool IsWhitedOut() const; // All Pokemon fainted

    void HealAll(); // Restore HP and PP

private:
    PlayerTeam() = default;
    std::array<std::optional<Pokemon>, MAX_TEAM> mTeam;
    int mSize = 0;
};

} // namespace pokemon
