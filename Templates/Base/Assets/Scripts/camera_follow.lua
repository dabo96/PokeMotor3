-- Seguimiento de la CÁMARA al jugador. La cámara es una entidad del ECS (CameraComponent
-- + Transform); su CENTRO es self:x()/self:y() (el Transform de esta entidad). El motor ya
-- no mueve la cámara en C++: la maneja este script, igual que player_movement.lua maneja al
-- jugador. Así en "play" la cámara la gobierna el script, y en el editor puedes dejarla
-- LIBRE apagando 'follow' aquí abajo (o moviendo su Posición en el inspector).
--
-- player_pos() devuelve la posición lógica del jugador (o nil si no hay). Suavizamos hacia
-- su centro de celda (+0.5) con un seguimiento exponencial (independiente del framerate).
exports = {
    follow    = true,    -- Checkbox: apágalo para dejar la cámara libre (no sigue al jugador)
    smoothing = 12.0,    -- DragFloat: mayor = la cámara alcanza al jugador más rápido
}

function on_update(self, dt)
    if not exports.follow then return end
    local px, py = player_pos()
    if px == nil then return end
    local k = 1.0 - math.exp(-exports.smoothing * dt)
    self:set_position(self:x() + (px + 0.5 - self:x()) * k,
                      self:y() + (py + 0.5 - self:y()) * k)
end
