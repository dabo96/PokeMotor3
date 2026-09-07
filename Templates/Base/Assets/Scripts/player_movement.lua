-- Movimiento del JUGADOR por casillas (Fase B). Vive en Lua porque el jugador ya es
-- una entidad del ECS: es incoherente tener su control en C++. move_axis() lee el
-- teclado y self:try_step avanza UNA celda si es transitable (is_walkable contra el
-- TileMap del overworld), deslizándose suave hasta ella.
--
-- Este script SOLO decide a dónde pisa el jugador. La animación (dirección/caminar),
-- la cámara que lo sigue y los encuentros los infiere el OverworldMode LEYENDO el
-- Transform; el script no sabe nada de eso.
-- Demo de exports (puedes borrarlos): aparecen en el inspector → sección Script
-- como DragFloat / Checkbox / campo de texto. No afectan al movimiento todavía.
exports = {
    velocidad = 4.0,    -- número   → DragFloat
    correr    = true,  -- booleano → Checkbox
    nombre    = "bulbasaur" -- texto    → campo de texto
}

function on_update(self, dt)
    local dx, dy = move_axis()
    if dx ~= 0 then dy = 0 end       -- un eje por paso: sin diagonales en la grilla
    if dx ~= 0 or dy ~= 0 then
        self:try_step(dx, dy)        -- no-op si ya hay un paso en curso o si choca
    end
end
