#pragma once
#include <SDL3/SDL.h>
#include <unordered_map>
#include <glm/glm.hpp>
#include <string>

enum class InputAction {
    MoveForward,
    MoveBackward,
    MoveLeft,
    MoveRight,
    MoveUp,
    MoveDown,
    Run,
    Jump,
    Interact,
    Confirm,
    Cancel,
    Pause,
    ToggleDebug,
    CameraZoomIn,
    CameraZoomOut,
    Count
};

enum class GamepadAxis {
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
    Count
};

class InputSystem {
public:
    static InputSystem& Instance();

    void Init();
    void Update(); // Call at start of frame, before polling events
    void ProcessEvent(const SDL_Event& event);

    // Action queries (combined keyboard + gamepad)
    bool IsPressed(InputAction action) const;  // Just pressed this frame
    bool IsHeld(InputAction action) const;     // Currently held down
    bool IsReleased(InputAction action) const; // Just released this frame

    // Raw key queries (for camera/debug)
    bool IsKeyHeld(SDL_Scancode key) const;
    bool IsKeyPressed(SDL_Scancode key) const;

    // Mouse
    glm::vec2 GetMouseDelta() const { return mMouseDelta; }
    float GetScrollDelta() const { return mScrollDelta; }
    glm::vec2 GetMousePosition() const { return mMousePos; }
    bool IsMouseButtonHeld(int button) const;
    bool IsMouseButtonPressed(int button) const;

    // Gamepad
    bool IsGamepadConnected() const { return mGamepad != nullptr; }
    std::string GetGamepadName() const;
    float GetAxis(GamepadAxis axis) const;
    glm::vec2 GetLeftStick() const;
    glm::vec2 GetRightStick() const;
    bool IsGamepadButtonHeld(int button) const;
    bool IsGamepadButtonPressed(int button) const;
    bool IsGamepadButtonReleased(int button) const;
    void Rumble(float lowFreq, float highFreq, uint32_t durationMs);
    void SetDeadzone(float deadzone) { mDeadzone = deadzone; }
    float GetDeadzone() const { return mDeadzone; }

    // Mode
    void SetUIMode(bool uiMode) { mUIMode = uiMode; }
    bool IsUIMode() const { return mUIMode; }

    // Rebinding
    void BindKey(SDL_Scancode key, InputAction action);
    void BindGamepadButton(int button, InputAction action);

private:
    InputSystem() = default;

    static constexpr int ACTION_COUNT = static_cast<int>(InputAction::Count);
    static constexpr int AXIS_COUNT = static_cast<int>(GamepadAxis::Count);
    static constexpr int GP_BUTTON_COUNT = SDL_GAMEPAD_BUTTON_COUNT;

    // Actions (combined from keyboard + gamepad)
    bool mCurrentActions[ACTION_COUNT] = {};
    bool mPreviousActions[ACTION_COUNT] = {};

    // Raw keyboard state
    bool mCurrentKeys[SDL_SCANCODE_COUNT] = {};
    bool mPreviousKeys[SDL_SCANCODE_COUNT] = {};

    // Mouse
    bool mCurrentMouseButtons[6] = {};
    bool mPreviousMouseButtons[6] = {};
    glm::vec2 mMouseDelta{0.0f};
    glm::vec2 mMousePos{0.0f};
    float mScrollDelta = 0.0f;

    // Gamepad
    SDL_Gamepad* mGamepad = nullptr;
    SDL_JoystickID mGamepadID = 0;
    bool mCurrentGPButtons[GP_BUTTON_COUNT] = {};
    bool mPreviousGPButtons[GP_BUTTON_COUNT] = {};
    float mAxes[AXIS_COUNT] = {};
    float mDeadzone = 0.15f;

    bool mUIMode = false;

    std::unordered_map<SDL_Scancode, InputAction> mKeyBindings;
    std::unordered_map<int, InputAction> mGamepadBindings;

    void SetupDefaultBindings();
    void SetupDefaultGamepadBindings();
    void OpenGamepad(SDL_JoystickID id);
    void CloseGamepad();
    float ApplyDeadzone(float value) const;
};
