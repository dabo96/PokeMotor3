#pragma once

namespace ecs {

class Registry;

class System {
public:
    virtual ~System() = default;
    virtual void Update(Registry& registry, float dt) = 0;
};

} // namespace ecs
