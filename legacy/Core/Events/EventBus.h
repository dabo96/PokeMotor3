#pragma once
#include <functional>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include <memory>
#include <queue>

namespace events {

using ListenerID = uint32_t;

class EventBus {
public:
    static EventBus& Instance() {
        static EventBus instance;
        return instance;
    }

    // Subscribe to an event type. Returns a listener ID for unsubscribing.
    template<typename T>
    ListenerID Subscribe(std::function<void(const T&)> callback) {
        auto key = std::type_index(typeid(T));
        ListenerID id = mNextID++;
        mListeners[key].push_back({id, [cb = std::move(callback)](const void* data) {
            cb(*static_cast<const T*>(data));
        }});
        return id;
    }

    // Unsubscribe a specific listener
    void Unsubscribe(ListenerID id) {
        for (auto& [type, listeners] : mListeners) {
            auto it = std::remove_if(listeners.begin(), listeners.end(),
                [id](const Listener& l) { return l.id == id; });
            listeners.erase(it, listeners.end());
        }
    }

    // Emit event immediately to all subscribers
    template<typename T>
    void Emit(const T& event) {
        auto key = std::type_index(typeid(T));
        auto it = mListeners.find(key);
        if (it == mListeners.end()) return;
        // Copy listener list in case callbacks modify subscriptions
        auto listeners = it->second;
        for (auto& listener : listeners) {
            listener.callback(&event);
        }
    }

    // Queue event for deferred dispatch
    template<typename T>
    void Queue(T event) {
        auto key = std::type_index(typeid(T));
        mQueue.push({key, std::make_shared<T>(std::move(event))});
    }

    // Dispatch all queued events
    void DispatchQueued() {
        while (!mQueue.empty()) {
            auto& [type, data] = mQueue.front();
            auto it = mListeners.find(type);
            if (it != mListeners.end()) {
                auto listeners = it->second;
                for (auto& listener : listeners) {
                    listener.callback(data.get());
                }
            }
            mQueue.pop();
        }
    }

    void Clear() {
        mListeners.clear();
        while (!mQueue.empty()) mQueue.pop();
    }

private:
    EventBus() = default;

    struct Listener {
        ListenerID id;
        std::function<void(const void*)> callback;
    };

    struct QueuedEvent {
        std::type_index type;
        std::shared_ptr<void> data;
    };

    ListenerID mNextID = 0;
    std::unordered_map<std::type_index, std::vector<Listener>> mListeners;
    std::queue<QueuedEvent> mQueue;
};

} // namespace events
