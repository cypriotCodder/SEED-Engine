#pragma once
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace seed {
class Arena final {
public:
    explicit Arena(std::size_t bytes) : storage_(std::make_unique<std::byte[]>(bytes)), capacity_(bytes) {}
    void* allocate(std::size_t bytes, std::size_t alignment) {
        if (!alignment || (alignment & (alignment - 1)))
            throw std::invalid_argument("Arena alignment must be a power of two");
        void* pointer = storage_.get() + used_;
        std::size_t remaining = capacity_ - used_;
        if (!std::align(alignment, bytes, pointer, remaining)) throw std::bad_alloc();
        used_ = capacity_ - remaining + bytes;
        return pointer;
    }
    template<class T>
    T* array(std::size_t count) {
        static_assert(std::is_trivially_destructible_v<T>);
        if (count > static_cast<std::size_t>(-1) / sizeof(T)) throw std::bad_alloc();
        auto* result = static_cast<T*>(allocate(count * sizeof(T), alignof(T)));
        for (std::size_t i = 0; i < count; ++i)
            std::construct_at(result + i);
        return result;
    }
    void reset() noexcept { used_ = 0; }
    [[nodiscard]] std::size_t used() const noexcept { return used_; }

private:
    std::unique_ptr<std::byte[]> storage_;
    std::size_t capacity_{}, used_{};
};

template<class T, std::size_t Capacity>
class Pool final {
    struct Slot {
        alignas(T) std::byte bytes[sizeof(T)];
        std::size_t next{};
        bool live{};
    };

public:
    Pool() : slots_(std::make_unique<Slot[]>(Capacity)) {
        for (std::size_t i = 0; i < Capacity; ++i)
            slots_[i].next = i + 1;
    }
    ~Pool() {
        for (std::size_t i = 0; i < Capacity; ++i)
            if (slots_[i].live) std::destroy_at(pointer(i));
    }
    template<class... Args>
    std::size_t create(Args&&... args) {
        if (free_ == Capacity) throw std::bad_alloc();
        const auto index = free_;
        std::construct_at(reinterpret_cast<T*>(slots_[index].bytes), std::forward<Args>(args)...);
        slots_[index].live = true;
        free_ = slots_[index].next;
        return index;
    }
    T& get(std::size_t index) {
        if (index >= Capacity || !slots_[index].live) throw std::out_of_range("Invalid pool slot");
        return *pointer(index);
    }
    void destroy(std::size_t index) {
        std::destroy_at(&get(index));
        slots_[index].live = false;
        slots_[index].next = free_;
        free_ = index;
    }

private:
    T* pointer(std::size_t index) { return std::launder(reinterpret_cast<T*>(slots_[index].bytes)); }
    std::unique_ptr<Slot[]> slots_;
    std::size_t free_{};
};
} // namespace seed
