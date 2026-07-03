// Core/Memory.h — allocators sobre los que construir sin pedir al SO en caliente.
// Diseño: MotorGrafico_Core.md (Memoria). Empezar simple: un allocator lineal
// (arena). Pool/stack se añaden solo cuando el perfilado lo pida.
#pragma once

#include "Core/Assert.h"
#include <cstddef>
#include <cstdint>
#include <new>

namespace pk {

// Allocator de avance (bump/arena). reset() libera todo de una. No llama a
// destructores: pensado para POD/datos de frame.
class LinearAllocator {
public:
    explicit LinearAllocator(std::size_t bytes)
        : m_base(static_cast<uint8_t*>(::operator new(bytes))),
          m_size(bytes), m_offset(0) {}

    ~LinearAllocator() { ::operator delete(m_base); }

    LinearAllocator(const LinearAllocator&) = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;

    void* allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t)) {
        std::size_t aligned = (m_offset + (align - 1)) & ~(align - 1);
        ASSERT(aligned + bytes <= m_size, "LinearAllocator sin memoria");
        void* p = m_base + aligned;
        m_offset = aligned + bytes;
        return p;
    }

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        return new (allocate(sizeof(T), alignof(T))) T(static_cast<Args&&>(args)...);
    }

    void reset() { m_offset = 0; }
    std::size_t used() const { return m_offset; }
    std::size_t capacity() const { return m_size; }

private:
    uint8_t*    m_base;
    std::size_t m_size;
    std::size_t m_offset;
};

}  // namespace pk
