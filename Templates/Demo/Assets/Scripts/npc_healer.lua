-- NPC de prueba para el puente Scripting <-> UI (corrutinas + pila de modos).
-- on_interact corre como CORRUTINA: presenta UI y la ESPERA (yield). show_text empuja
-- una caja de diálogo y cede hasta que se cierra; show_choice devuelve el índice elegido
-- (1-based) y el script ramifica; wait(s) cede N segundos. Un error aquí va a la consola,
-- no tumba el motor (corrutina protegida).
--
-- Para probar: en el overworld de demo, ponte de FRENTE a este NPC y pulsa Enter/Espacio.

function on_interact(self)
    show_text("Hola! Soy la enfermera. Curo este puente de UI por scripting.")

    local pick = show_choice({ "Curar equipo", "Esperar un momento", "No, gracias" })

    if pick == 1 then
        -- start_battle / heal_party reales aún no existen: stub que reanuda al instante.
        start_battle("entrenador_demo")
        show_text("Listo! Tu equipo esta como nuevo.")
    elseif pick == 2 then
        wait(1.0)                       -- cede 1 segundo sin UI, luego sigue
        show_text("Gracias por esperar. Vuelve cuando quieras!")
    else
        show_text("De acuerdo. Hasta pronto!")
    end
end
