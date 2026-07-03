#include "PlayerTeam.h"
#include "PokemonDB.h"

namespace pokemon {

PlayerTeam& PlayerTeam::Instance() {
    static PlayerTeam instance;
    return instance;
}

bool PlayerTeam::AddPokemon(const Pokemon& pokemon) {
    if (mSize >= MAX_TEAM) return false;
    for (int i = 0; i < MAX_TEAM; i++) {
        if (!mTeam[i].has_value()) {
            mTeam[i] = pokemon;
            mSize++;
            return true;
        }
    }
    return false;
}

void PlayerTeam::RemovePokemon(int slot) {
    if (slot < 0 || slot >= MAX_TEAM || !mTeam[slot].has_value()) return;
    mTeam[slot].reset();
    mSize--;
}

void PlayerTeam::SwapSlots(int a, int b) {
    if (a < 0 || a >= MAX_TEAM || b < 0 || b >= MAX_TEAM) return;
    std::swap(mTeam[a], mTeam[b]);
}

Pokemon* PlayerTeam::GetPokemon(int slot) {
    if (slot < 0 || slot >= MAX_TEAM || !mTeam[slot].has_value()) return nullptr;
    return &mTeam[slot].value();
}

const Pokemon* PlayerTeam::GetPokemon(int slot) const {
    if (slot < 0 || slot >= MAX_TEAM || !mTeam[slot].has_value()) return nullptr;
    return &mTeam[slot].value();
}

Pokemon* PlayerTeam::GetFirstAlive() {
    for (auto& slot : mTeam) {
        if (slot.has_value() && slot->IsAlive()) return &slot.value();
    }
    return nullptr;
}

int PlayerTeam::GetFirstAliveIndex() const {
    for (int i = 0; i < MAX_TEAM; i++) {
        if (mTeam[i].has_value() && mTeam[i]->IsAlive()) return i;
    }
    return -1;
}

bool PlayerTeam::IsWhitedOut() const {
    for (auto& slot : mTeam) {
        if (slot.has_value() && slot->IsAlive()) return false;
    }
    return true;
}

void PlayerTeam::HealAll() {
    auto& db = PokemonDB::Instance();
    for (auto& slot : mTeam) {
        if (!slot.has_value()) continue;
        auto& p = slot.value();
        p.currentHP = p.maxHP;
        for (int i = 0; i < 4; i++) {
            if (p.moves[i].empty()) continue;
            auto* mv = db.GetMove(p.moves[i]);
            p.currentPP[i] = mv ? mv->pp : 35;
        }
    }
}

} // namespace pokemon
