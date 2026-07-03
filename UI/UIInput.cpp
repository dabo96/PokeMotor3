// UI/UIInput.cpp — traduce el ActionMap a las acciones de UI (flancos).
#include "UI/UIInput.h"

#include "Input/ActionMap.h"
#include "Input/Input.h"

namespace pk {

UIInput UIInput::fromActions(const Input& in, const ActionMap& a) {
    UIInput u;
    u.up      = a.wasTriggered(in, Action::MoveUp);
    u.down    = a.wasTriggered(in, Action::MoveDown);
    u.left    = a.wasTriggered(in, Action::MoveLeft);
    u.right   = a.wasTriggered(in, Action::MoveRight);
    u.confirm = a.wasTriggered(in, Action::Confirm);
    u.cancel  = a.wasTriggered(in, Action::Cancel);
    return u;
}

}  // namespace pk
