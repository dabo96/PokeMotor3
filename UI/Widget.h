// UI/Widget.h — nodo del árbol retenido de la UI del juego. El base propaga
// update/draw/handleInput a los hijos; cada widget concreto solo implementa SU parte
// (onUpdate/onDraw/onInput). Diseño: MotorGrafico_UIJuego.md.
#pragma once

#include "UI/Rect.h"

#include <memory>
#include <utility>
#include <vector>

namespace pk {

class UIRenderer;
struct UIInput;

class Widget {
public:
    virtual ~Widget() = default;

    Rect rect;                                          // posición/tamaño en pantalla (px)
    bool visible = true;
    std::vector<std::unique_ptr<Widget>> children;

    // Añade un hijo y devuelve un puntero crudo a él (la propiedad la guarda el padre).
    template <typename T>
    T* add(std::unique_ptr<T> child) {
        T* p = child.get();
        children.push_back(std::move(child));
        return p;
    }

    // Recorrido del árbol (no virtuales: la recursión es común a todos). Cada uno llama
    // primero la parte propia del widget y luego desciende a los hijos.
    void update(float dt) {
        if (!visible) return;
        onUpdate(dt);
        for (auto& c : children) c->update(dt);
    }
    void draw(UIRenderer& r) {
        if (!visible) return;
        onDraw(r);
        for (auto& c : children) c->draw(r);
    }
    // Devuelve true si algún widget consumió el input (los hijos tienen prioridad sobre
    // el padre para que un menú anidado capture antes que su contenedor).
    bool handleInput(const UIInput& in) {
        if (!visible) return false;
        for (auto& c : children) if (c->handleInput(in)) return true;
        return onInput(in);
    }

protected:
    virtual void onUpdate(float /*dt*/) {}
    virtual void onDraw(UIRenderer& /*r*/) {}
    virtual bool onInput(const UIInput& /*in*/) { return false; }
};

}  // namespace pk
