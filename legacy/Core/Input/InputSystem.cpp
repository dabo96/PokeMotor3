#include "InputSystem.h"
#include <cstring>
#include <cmath>
#include <algorithm>

InputSystem& InputSystem::Instance() {
    static InputSystem instance;
    return instance;
}

void InputSystem::Init() {
    std::memset(mCurrentActions, 0, sizeof(mCurrentActions));
    std::memset(mPreviousActions, 0, sizeof(mPreviousActions));
    std::memset(mCurrentKeys, 0, sizeof(mCurrentKeys));
    std::memset(mPreviousKeys, 0, sizeof(mPreviousKeys));
    std::memset(mCurrentMouseButtons, 0, sizeof(mCurrentMouseButtons));
    std::memset(mPreviousMouseButtons, 0, sizeof(mPreviousMouseButtons));
    std::memset(mCurrentGPButtons, 0, sizeof(mCurrentGPButtons));
    std::memset(mPreviousGPButtons, 0, sizeof(mPreviousGPButtons));
    std::memset(mAxes, 0, sizeof(mAxes));

    SetupDefaultBindings();
    SetupDefaultGamepadBindings();

    // Open any already-connected gamepad
    int count = 0;
    SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
    if (gamepads && count > 0) {
        OpenGamepad(gamepads[0]);
    }
    SDL_free(gamepads);
}

void InputSystem::Update() {
    // Copy current to previous
    std::memcpy(mPreviousActions, mCurrentActions, sizeof(mCurrentActions));
    std::memcpy(mPreviousKeys, mCurrentKeys, sizeof(mCurrentKeys));
    std::memcpy(mPreviousMouseButtons, mCurrentMouseButtons, sizeof(mCurrentMouseButtons));
    std::memcpy(mPreviousGPButtons, mCurrentGPButtons, sizeof(mCurrentGPButtons));

    mMouseDelta = {0.0f, 0.0f};
    mScrollDelta = 0.0f;

    // Read keyboard state from SDL
    int numKeys = 0;
    const bool* keyState = SDL_GetKeyboardState(&numKeys);
    if (keyState) {
        int count = (numKeys < SDL_SCANCODE_COUNT) ? numKeys : SDL_SCANCODE_COUNT;
        for (int i = 0; i < count; ++i)
            mCurrentKeys[i] = keyState[i];
    }

    // Map keys to actions
    std::memset(mCurrentActions, 0, sizeof(mCurrentActions));
    for (auto& [key, action] : mKeyBindings) {
        if (key < SDL_SCANCODE_COUNT && mCurrentKeys[key])
            mCurrentActions[static_cast<int>(action)] = true;
    }

    // Map gamepad buttons to actions
    for (auto& [button, action] : mGamepadBindings) {
        if (button >= 0 && button < GP_BUTTON_COUNT && mCurrentGPButtons[button])
            mCurrentActions[static_cast<int>(action)] = true;
    }

    // Map left stick to movement actions (with deadzone already applied)
    if (mGamepad) {
        float lx = mAxes[static_cast<int>(GamepadAxis::LeftX)];
        float ly = mAxes[static_cast<int>(GamepadAxis::LeftY)];

        if (lx < 0.0f) mCurrentActions[static_cast<int>(InputAction::MoveLeft)] = true;
        if (lx > 0.0f) mCurrentActions[static_cast<int>(InputAction::MoveRight)] = true;
        if (ly < 0.0f) mCurrentActions[static_cast<int>(InputAction::MoveForward)] = true;
        if (ly > 0.0f) mCurrentActions[static_cast<int>(InputAction::MoveBackward)] = true;

        // Triggers to zoom
        float lt = mAxes[static_cast<int>(GamepadAxis::LeftTrigger)];
        float rt = mAxes[static_cast<int>(GamepadAxis::RightTrigger)];
        if (lt > 0.0f) mCurrentActions[static_cast<int>(InputAction::CameraZoomOut)] = true;
        if (rt > 0.0f) mCurrentActions[static_cast<int>(InputAction::CameraZoomIn)] = true;
    }
}

void InputSystem::ProcessEvent(const SDL_Event& event) {
    switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
            mMouseDelta.x += event.motion.xrel;
            mMouseDelta.y += event.motion.yrel;
            mMousePos.x = event.motion.x;
            mMousePos.y = event.motion.y;
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            mScrollDelta += event.wheel.y;
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button < 6)
                mCurrentMouseButtons[event.button.button] = true;
            break;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button < 6)
                mCurrentMouseButtons[event.button.button] = false;
            break;

        // Gamepad
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (mGamepad && event.gbutton.which == mGamepadID) {
                int btn = static_cast<int>(event.gbutton.button);
                if (btn >= 0 && btn < GP_BUTTON_COUNT)
                    mCurrentGPButtons[btn] = true;
            }
            break;

        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (mGamepad && event.gbutton.which == mGamepadID) {
                int btn = static_cast<int>(event.gbutton.button);
                if (btn >= 0 && btn < GP_BUTTON_COUNT)
                    mCurrentGPButtons[btn] = false;
            }
            break;

        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (mGamepad && event.gaxis.which == mGamepadID) {
                int axis = static_cast<int>(event.gaxis.axis);
                if (axis >= 0 && axis < AXIS_COUNT) {
                    float normalized = static_cast<float>(event.gaxis.value) / 32767.0f;
                    mAxes[axis] = ApplyDeadzone(normalized);
                }
            }
            break;

        case SDL_EVENT_GAMEPAD_ADDED:
            if (!mGamepad)
                OpenGamepad(event.gdevice.which);
            break;

        case SDL_EVENT_GAMEPAD_REMOVED:
            if (mGamepad && event.gdevice.which == mGamepadID)
                CloseGamepad();
            break;
    }
}

// --- Action queries ---

bool InputSystem::IsPressed(InputAction action) const {
    int idx = static_cast<int>(action);
    return mCurrentActions[idx] && !mPreviousActions[idx];
}

bool InputSystem::IsHeld(InputAction action) const {
    return mCurrentActions[static_cast<int>(action)];
}

bool InputSystem::IsReleased(InputAction action) const {
    int idx = static_cast<int>(action);
    return !mCurrentActions[idx] && mPreviousActions[idx];
}

// --- Raw keyboard ---

bool InputSystem::IsKeyHeld(SDL_Scancode key) const {
    return key < SDL_SCANCODE_COUNT && mCurrentKeys[key];
}

bool InputSystem::IsKeyPressed(SDL_Scancode key) const {
    return key < SDL_SCANCODE_COUNT && mCurrentKeys[key] && !mPreviousKeys[key];
}

// --- Mouse ---

bool InputSystem::IsMouseButtonHeld(int button) const {
    return button < 6 && mCurrentMouseButtons[button];
}

bool InputSystem::IsMouseButtonPressed(int button) const {
    return button < 6 && mCurrentMouseButtons[button] && !mPreviousMouseButtons[button];
}

// --- Gamepad ---

std::string InputSystem::GetGamepadName() const {
    if (!mGamepad) return "";
    const char* name = SDL_GetGamepadName(mGamepad);
    return name ? std::string(name) : "";
}

float InputSystem::GetAxis(GamepadAxis axis) const {
    int idx = static_cast<int>(axis);
    if (idx < 0 || idx >= AXIS_COUNT) return 0.0f;
    return mAxes[idx];
}

glm::vec2 InputSystem::GetLeftStick() const {
    return { mAxes[static_cast<int>(GamepadAxis::LeftX)],
             mAxes[static_cast<int>(GamepadAxis::LeftY)] };
}

glm::vec2 InputSystem::GetRightStick() const {
    return { mAxes[static_cast<int>(GamepadAxis::RightX)],
             mAxes[static_cast<int>(GamepadAxis::RightY)] };
}

bool InputSystem::IsGamepadButtonHeld(int button) const {
    return button >= 0 && button < GP_BUTTON_COUNT && mCurrentGPButtons[button];
}

bool InputSystem::IsGamepadButtonPressed(int button) const {
    return button >= 0 && button < GP_BUTTON_COUNT &&
           mCurrentGPButtons[button] && !mPreviousGPButtons[button];
}

bool InputSystem::IsGamepadButtonReleased(int button) const {
    return button >= 0 && button < GP_BUTTON_COUNT &&
           !mCurrentGPButtons[button] && mPreviousGPButtons[button];
}

void InputSystem::Rumble(float lowFreq, float highFreq, uint32_t durationMs) {
    if (!mGamepad) return;
    Uint16 low = static_cast<Uint16>(std::clamp(lowFreq, 0.0f, 1.0f) * 65535.0f);
    Uint16 high = static_cast<Uint16>(std::clamp(highFreq, 0.0f, 1.0f) * 65535.0f);
    SDL_RumbleGamepad(mGamepad, low, high, durationMs);
}

// --- Rebinding ---

void InputSystem::BindKey(SDL_Scancode key, InputAction action) {
    mKeyBindings[key] = action;
}

void InputSystem::BindGamepadButton(int button, InputAction action) {
    mGamepadBindings[button] = action;
}

// --- Private ---

void InputSystem::SetupDefaultBindings() {
    mKeyBindings.clear();

    // Movement
    mKeyBindings[SDL_SCANCODE_W] = InputAction::MoveForward;
    mKeyBindings[SDL_SCANCODE_S] = InputAction::MoveBackward;
    mKeyBindings[SDL_SCANCODE_A] = InputAction::MoveLeft;
    mKeyBindings[SDL_SCANCODE_D] = InputAction::MoveRight;
    mKeyBindings[SDL_SCANCODE_Q] = InputAction::MoveUp;
    mKeyBindings[SDL_SCANCODE_E] = InputAction::MoveDown;

    // Actions
    mKeyBindings[SDL_SCANCODE_LSHIFT] = InputAction::Run;
    mKeyBindings[SDL_SCANCODE_SPACE] = InputAction::Jump;
    mKeyBindings[SDL_SCANCODE_F] = InputAction::Interact;
    mKeyBindings[SDL_SCANCODE_RETURN] = InputAction::Confirm;
    mKeyBindings[SDL_SCANCODE_ESCAPE] = InputAction::Cancel;
    mKeyBindings[SDL_SCANCODE_P] = InputAction::Pause;
    mKeyBindings[SDL_SCANCODE_F3] = InputAction::ToggleDebug;

    // Camera zoom
    mKeyBindings[SDL_SCANCODE_KP_PLUS] = InputAction::CameraZoomIn;
    mKeyBindings[SDL_SCANCODE_KP_MINUS] = InputAction::CameraZoomOut;
}

void InputSystem::SetupDefaultGamepadBindings() {
    mGamepadBindings.clear();

    // DPad → movement
    mGamepadBindings[SDL_GAMEPAD_BUTTON_DPAD_UP]    = InputAction::MoveForward;
    mGamepadBindings[SDL_GAMEPAD_BUTTON_DPAD_DOWN]  = InputAction::MoveBackward;
    mGamepadBindings[SDL_GAMEPAD_BUTTON_DPAD_LEFT]  = InputAction::MoveLeft;
    mGamepadBindings[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = InputAction::MoveRight;

    // Face buttons
    mGamepadBindings[SDL_GAMEPAD_BUTTON_SOUTH] = InputAction::Confirm;   // A
    mGamepadBindings[SDL_GAMEPAD_BUTTON_EAST]  = InputAction::Cancel;    // B
    mGamepadBindings[SDL_GAMEPAD_BUTTON_WEST]  = InputAction::Interact;  // X
    mGamepadBindings[SDL_GAMEPAD_BUTTON_NORTH] = InputAction::Jump;      // Y

    // Shoulders
    mGamepadBindings[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = InputAction::Run;

    // Start/Back
    mGamepadBindings[SDL_GAMEPAD_BUTTON_START] = InputAction::Pause;
    mGamepadBindings[SDL_GAMEPAD_BUTTON_BACK]  = InputAction::ToggleDebug;
}

void InputSystem::OpenGamepad(SDL_JoystickID id) {
    if (mGamepad) return;
    mGamepad = SDL_OpenGamepad(id);
    if (mGamepad) {
        mGamepadID = id;
        const char* name = SDL_GetGamepadName(mGamepad);
        SDL_Log("Gamepad connected: %s", name ? name : "Unknown");
    }
}

void InputSystem::CloseGamepad() {
    if (!mGamepad) return;
    SDL_Log("Gamepad disconnected: %s", SDL_GetGamepadName(mGamepad));
    SDL_CloseGamepad(mGamepad);
    mGamepad = nullptr;
    mGamepadID = 0;
    std::memset(mCurrentGPButtons, 0, sizeof(mCurrentGPButtons));
    std::memset(mPreviousGPButtons, 0, sizeof(mPreviousGPButtons));
    std::memset(mAxes, 0, sizeof(mAxes));
}

float InputSystem::ApplyDeadzone(float value) const {
    if (std::abs(value) < mDeadzone) return 0.0f;
    float sign = (value > 0.0f) ? 1.0f : -1.0f;
    return sign * (std::abs(value) - mDeadzone) / (1.0f - mDeadzone);
}
