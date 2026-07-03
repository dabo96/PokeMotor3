#pragma once
#include "PokemonData.h"
#include "PokemonDB.h"
#include <string>
#include <vector>
#include <functional>
#include <random>

namespace pokemon {

enum class BattleAction { Fight, Switch, Run };

struct BattleMessage {
    std::string text;
    bool isEffectiveness = false; // "Super effective!" etc.
};

enum class BattleState {
    Intro,          // "A wild X appeared!"
    PlayerChoose,   // Waiting for player action
    MoveSelect,     // Player choosing a move
    Executing,      // Animating turn results
    ShowMessages,   // Displaying text messages
    PlayerWon,
    PlayerLost,
    PlayerRan,
    Ended
};

class BattleSystem {
public:
    // Start a wild encounter
    void StartWild(Pokemon& playerPokemon, const Pokemon& wildPokemon);

    // Player actions
    void SelectFight();
    void SelectMove(int moveIndex); // 0-3
    void SelectRun();

    // Advance message queue (call when player presses button during ShowMessages)
    void AdvanceMessage();

    // Update (for timed animations, etc.)
    void Update(float dt);

    // Getters
    BattleState GetState() const { return mState; }
    const Pokemon& GetPlayerPokemon() const { return *mPlayerPokemon; }
    const Pokemon& GetWildPokemon() const { return mWildPokemon; }
    const std::string& GetCurrentMessage() const { return mCurrentMessage; }
    bool HasMessages() const { return !mMessageQueue.empty(); }

    // HP bar animation targets
    float GetPlayerHPPercent() const;
    float GetWildHPPercent() const;

private:
    Pokemon* mPlayerPokemon = nullptr;
    Pokemon mWildPokemon;
    BattleState mState = BattleState::Ended;

    std::vector<std::string> mMessageQueue;
    int mMessageIndex = 0;
    std::string mCurrentMessage;

    std::mt19937 mRng{std::random_device{}()};

    void ExecuteTurn(int playerMoveIdx);
    int CalculateDamage(const Pokemon& attacker, const Pokemon& defender,
                        const MoveData& move, const SpeciesData& atkSpecies,
                        const SpeciesData& defSpecies);
    float GetCombinedEffectiveness(Type moveType, Type def1, Type def2);
    bool TryRun();
    void CheckBattleEnd();
    void PushMessage(const std::string& msg);
    void ShowNextMessage();
};

} // namespace pokemon
