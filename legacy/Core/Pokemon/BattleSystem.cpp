#include "BattleSystem.h"
#include <cmath>
#include <algorithm>
#include <iostream>

namespace pokemon {

void BattleSystem::StartWild(Pokemon& playerPokemon, const Pokemon& wildPokemon) {
    mPlayerPokemon = &playerPokemon;
    mWildPokemon = wildPokemon;
    mMessageQueue.clear();
    mMessageIndex = 0;
    mCurrentMessage.clear();

    PushMessage("A wild " + mWildPokemon.DisplayName() + " appeared!");
    PushMessage("Go, " + mPlayerPokemon->DisplayName() + "!");
    mState = BattleState::Intro;
    ShowNextMessage();
}

void BattleSystem::SelectFight() {
    if (mState != BattleState::PlayerChoose) return;
    mState = BattleState::MoveSelect;
}

void BattleSystem::SelectMove(int moveIndex) {
    if (mState != BattleState::MoveSelect) return;
    if (moveIndex < 0 || moveIndex >= 4) return;
    if (mPlayerPokemon->moves[moveIndex].empty()) return;
    if (mPlayerPokemon->currentPP[moveIndex] <= 0) return;

    ExecuteTurn(moveIndex);
}

void BattleSystem::SelectRun() {
    if (mState != BattleState::PlayerChoose) return;

    if (TryRun()) {
        PushMessage("Got away safely!");
        mState = BattleState::PlayerRan;
    } else {
        PushMessage("Can't escape!");
        // Wild Pokemon attacks
        int wildMove = -1;
        for (int i = 0; i < 4; i++) {
            if (!mWildPokemon.moves[i].empty() && mWildPokemon.currentPP[i] > 0) {
                wildMove = i;
                break;
            }
        }
        if (wildMove >= 0) {
            auto* mv = PokemonDB::Instance().GetMove(mWildPokemon.moves[wildMove]);
            auto* atkSp = PokemonDB::Instance().GetSpecies(mWildPokemon.speciesName);
            auto* defSp = PokemonDB::Instance().GetSpecies(mPlayerPokemon->speciesName);
            if (mv && atkSp && defSp && mv->power > 0) {
                int dmg = CalculateDamage(mWildPokemon, *mPlayerPokemon, *mv, *atkSp, *defSp);
                mPlayerPokemon->currentHP = std::max(0, mPlayerPokemon->currentHP - dmg);
                mWildPokemon.currentPP[wildMove]--;
                PushMessage("Wild " + mWildPokemon.DisplayName() + " used " + mv->name + "!");
            }
        }
        mState = BattleState::ShowMessages;
    }
    ShowNextMessage();
}

void BattleSystem::AdvanceMessage() {
    if (mState == BattleState::Ended) return;

    if (mMessageIndex < static_cast<int>(mMessageQueue.size())) {
        ShowNextMessage();
    } else {
        // All messages shown — determine next state
        mMessageQueue.clear();
        mMessageIndex = 0;

        if (mState == BattleState::PlayerWon || mState == BattleState::PlayerLost ||
            mState == BattleState::PlayerRan) {
            mState = BattleState::Ended;
        } else if (mState == BattleState::Intro) {
            mState = BattleState::PlayerChoose;
        } else {
            CheckBattleEnd();
            if (mState == BattleState::ShowMessages || mState == BattleState::Executing) {
                mState = BattleState::PlayerChoose;
            }
        }
    }
}

void BattleSystem::Update(float /*dt*/) {
    // Reserved for animations
}

void BattleSystem::ExecuteTurn(int playerMoveIdx) {
    auto& db = PokemonDB::Instance();
    mMessageQueue.clear();
    mMessageIndex = 0;

    auto* playerMove = db.GetMove(mPlayerPokemon->moves[playerMoveIdx]);
    auto* playerSp = db.GetSpecies(mPlayerPokemon->speciesName);
    auto* wildSp = db.GetSpecies(mWildPokemon.speciesName);

    // Pick wild Pokemon's move
    int wildMoveIdx = -1;
    for (int i = 0; i < 4; i++) {
        if (!mWildPokemon.moves[i].empty() && mWildPokemon.currentPP[i] > 0) {
            std::uniform_int_distribution<int> dist(0, 3);
            int pick = dist(mRng);
            if (!mWildPokemon.moves[pick].empty() && mWildPokemon.currentPP[pick] > 0)
                wildMoveIdx = pick;
            else
                wildMoveIdx = i;
            break;
        }
    }
    auto* wildMove = wildMoveIdx >= 0 ? db.GetMove(mWildPokemon.moves[wildMoveIdx]) : nullptr;

    // Determine order by speed
    bool playerFirst = mPlayerPokemon->speed >= mWildPokemon.speed;

    auto doAttack = [&](Pokemon& attacker, Pokemon& defender, const MoveData* move,
                        int moveIdx, bool isPlayer, const SpeciesData* atkSp, const SpeciesData* defSp) {
        if (!move || !attacker.IsAlive() || !defender.IsAlive()) return;

        std::string prefix = isPlayer ? (attacker.DisplayName() + " used ") :
                                        ("Wild " + attacker.DisplayName() + " used ");
        PushMessage(prefix + move->name + "!");

        if (move->power > 0) {
            // Accuracy check
            std::uniform_int_distribution<int> accRoll(1, 100);
            if (accRoll(mRng) > move->accuracy) {
                PushMessage("It missed!");
                return;
            }

            int dmg = CalculateDamage(attacker, defender, *move, *atkSp, *defSp);
            defender.currentHP = std::max(0, defender.currentHP - dmg);

            if (isPlayer) {
                if (moveIdx >= 0) attacker.currentPP[moveIdx]--;
            } else {
                if (moveIdx >= 0) attacker.currentPP[moveIdx]--;
            }

            // Effectiveness message
            float eff = GetCombinedEffectiveness(move->type,
                defSp->type1, defSp->type2);
            if (eff >= 2.0f) PushMessage("It's super effective!");
            else if (eff <= 0.5f && eff > 0.0f) PushMessage("It's not very effective...");
            else if (eff == 0.0f) PushMessage("It had no effect!");

            if (!defender.IsAlive()) {
                if (isPlayer)
                    PushMessage("Wild " + defender.DisplayName() + " fainted!");
                else
                    PushMessage(defender.DisplayName() + " fainted!");
            }
        } else {
            PushMessage("But nothing happened!");
        }
    };

    if (playerFirst) {
        doAttack(*mPlayerPokemon, mWildPokemon, playerMove, playerMoveIdx, true, playerSp, wildSp);
        doAttack(mWildPokemon, *mPlayerPokemon, wildMove, wildMoveIdx, false, wildSp, playerSp);
    } else {
        doAttack(mWildPokemon, *mPlayerPokemon, wildMove, wildMoveIdx, false, wildSp, playerSp);
        doAttack(*mPlayerPokemon, mWildPokemon, playerMove, playerMoveIdx, true, playerSp, wildSp);
    }

    mState = BattleState::ShowMessages;
    ShowNextMessage();
}

int BattleSystem::CalculateDamage(const Pokemon& attacker, const Pokemon& defender,
                                   const MoveData& move, const SpeciesData& atkSpecies,
                                   const SpeciesData& defSpecies) {
    // Simplified Gen III formula
    int atkStat = (move.category == MoveCategory::Physical) ? attacker.attack : attacker.spAttack;
    int defStat = (move.category == MoveCategory::Physical) ? defender.defense : defender.spDefense;

    float baseDmg = ((2.0f * attacker.level / 5.0f + 2.0f) * move.power * atkStat / defStat) / 50.0f + 2.0f;

    // STAB (Same Type Attack Bonus)
    if (move.type == atkSpecies.type1 || move.type == atkSpecies.type2)
        baseDmg *= 1.5f;

    // Type effectiveness
    baseDmg *= GetCombinedEffectiveness(move.type, defSpecies.type1, defSpecies.type2);

    // Critical hit (1/16 chance, 1.5x)
    std::uniform_int_distribution<int> critRoll(1, 16);
    if (critRoll(mRng) == 1)
        baseDmg *= 1.5f;

    // Random factor (85-100%)
    std::uniform_int_distribution<int> randFactor(85, 100);
    baseDmg *= randFactor(mRng) / 100.0f;

    return std::max(1, static_cast<int>(baseDmg));
}

float BattleSystem::GetCombinedEffectiveness(Type moveType, Type def1, Type def2) {
    float eff = GetTypeEffectiveness(moveType, def1);
    if (def1 != def2)
        eff *= GetTypeEffectiveness(moveType, def2);
    return eff;
}

bool BattleSystem::TryRun() {
    // Higher speed = higher escape chance
    int playerSpeed = mPlayerPokemon->speed;
    int wildSpeed = mWildPokemon.speed;
    int chance = (playerSpeed * 128 / std::max(1, wildSpeed)) + 30;
    std::uniform_int_distribution<int> dist(0, 255);
    return dist(mRng) < chance;
}

void BattleSystem::CheckBattleEnd() {
    if (!mWildPokemon.IsAlive()) {
        // Award EXP
        auto* wildSp = PokemonDB::Instance().GetSpecies(mWildPokemon.speciesName);
        if (wildSp && mPlayerPokemon->IsAlive()) {
            int expGain = (wildSp->expYield * mWildPokemon.level) / 7;
            mPlayerPokemon->exp += expGain;
            PushMessage(mPlayerPokemon->DisplayName() + " gained " + std::to_string(expGain) + " EXP!");

            // Check level up
            while (mPlayerPokemon->exp >= mPlayerPokemon->ExpForLevel(mPlayerPokemon->level + 1) &&
                   mPlayerPokemon->level < 100) {
                mPlayerPokemon->level++;
                auto* sp = PokemonDB::Instance().GetSpecies(mPlayerPokemon->speciesName);
                if (sp) {
                    int oldHP = mPlayerPokemon->maxHP;
                    mPlayerPokemon->CalculateStats(sp->baseStats);
                    mPlayerPokemon->currentHP += (mPlayerPokemon->maxHP - oldHP);
                }
                PushMessage(mPlayerPokemon->DisplayName() + " grew to level " +
                           std::to_string(mPlayerPokemon->level) + "!");
            }
        }
        mState = BattleState::PlayerWon;
        ShowNextMessage();
    } else if (!mPlayerPokemon->IsAlive()) {
        PushMessage("You blacked out!");
        mState = BattleState::PlayerLost;
        ShowNextMessage();
    }
}

void BattleSystem::PushMessage(const std::string& msg) {
    mMessageQueue.push_back(msg);
}

void BattleSystem::ShowNextMessage() {
    if (mMessageIndex < static_cast<int>(mMessageQueue.size())) {
        mCurrentMessage = mMessageQueue[mMessageIndex];
        mMessageIndex++;
    }
}

float BattleSystem::GetPlayerHPPercent() const {
    if (!mPlayerPokemon || mPlayerPokemon->maxHP == 0) return 0.0f;
    return static_cast<float>(mPlayerPokemon->currentHP) / mPlayerPokemon->maxHP;
}

float BattleSystem::GetWildHPPercent() const {
    if (mWildPokemon.maxHP == 0) return 0.0f;
    return static_cast<float>(mWildPokemon.currentHP) / mWildPokemon.maxHP;
}

} // namespace pokemon
