#ifndef MEMOC_ALLOCATORS_H
#define MEMOC_ALLOCATORS_H

#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <new>
#include <chrono>
#include <utility>
#include <type_traits>
#include <concepts>
#include <memory>

#include <erroc/errors.h>
#include <enumoc/enumoc.h>

#include <memoc/blocks.h>

ENUMOC_GENERATE(memoc, Allocator_error,
    invalid_size,
    unknown);

namespace memoc {
    namespace details {
        template <class T>
        concept Allocator = 
            requires
        {
            std::is_default_constructible_v<T>;
            std::is_copy_constructible_v<T>;
            std::is_copy_assignable_v<T>;
            std::is_move_constructible_v<T>;
            std::is_move_assignable_v<T>;
            std::is_destructible_v<T>;
        }&&
            requires (T t, Block<void>::Size_type s, Block<void> b)
        {
            {t.allocate(s)} noexcept -> std::same_as<Block<void>>;
            {t.deallocate(std::ref(b))} noexcept -> std::same_as<void>;
            {t.owns(std::cref(b))} noexcept -> std::same_as<bool>;
        };

        template <Allocator Primary, Allocator Fallback>
        class Fallback_allocator
            : private Primary
            , private Fallback {
        public:

            Fallback_allocator() = default;
            Fallback_allocator(const Fallback_allocator& other) noexcept
                : Primary(other), Fallback(other) {}
            Fallback_allocator operator=(const Fallback_allocator& other) noexcept
            {
                if (this == &other) {
                    return *this;
                }
                Primary::operator=(other);
                Fallback::operator=(other);
                return *this;
            }
            Fallback_allocator(Fallback_allocator&& other) noexcept
                : Primary(std::move(other)), Fallback(std::move(other)) {}
            Fallback_allocator& operator=(Fallback_allocator&& other) noexcept
            {
                if (this == &other) {
                    return *this;
                }
                Primary::operator=(std::move(other));
                Fallback::operator=(std::move(other));
                return *this;
            }
            virtual ~Fallback_allocator() = default;

            [[nodiscard]] Block<void> allocate(Block<void>::Size_type s) noexcept
            {
                Block<void> b = Primary::allocate(s);
                if (b.empty()) {
                    b = Fallback::allocate(s);
                }
                return b;
            }

            void deallocate(Block<void>& b) noexcept
            {
                if (Primary::owns(b)) {
                    return Primary::deallocate(b);
                }
                Fallback::deallocate(b);
            }

            [[nodiscard]] bool owns(const Block<void>& b) const noexcept
            {
                return Primary::owns(b) || Fallback::owns(b);
            }
        };

        class Malloc_allocator {
        public:
            [[nodiscard]] Block<void> allocate(Block<void>::Size_type s) noexcept
            {
                if (s <= 0) {
                    return { s, nullptr };
                }
                return { s, std::malloc(s) };
            }

            void deallocate(Block<void>& b) noexcept
            {
                std::free(b.data());
                b = {};
            }

            [[nodiscard]] bool owns(const Block<void>& b) const noexcept
            {
                return b.data();
            }
        };

        template <Block<void>::Size_type Size>
        class Stack_allocator {
            static_assert(Size > 1 && Size % 2 == 0);
        public:
            Stack_allocator() = default;
            Stack_allocator(const Stack_allocator& other) noexcept
                : p_(d_) {}
            Stack_allocator operator=(const Stack_allocator& other) noexcept
            {
                if (this == &other) {
                    return *this;
                }

                p_ = d_;
                return *this;
            }
            Stack_allocator(Stack_allocator&& other) noexcept
                : p_(d_)
            {
                other.p_ = nullptr;
            }
            Stack_allocator& operator=(Stack_allocator&& other) noexcept
            {
                if (this == &other) {
                    return *this;
                }

                p_ = d_;
                other.p_ = nullptr;
                return *this;
            }
            virtual ~Stack_allocator() = default;

            [[nodiscard]] Block<void> allocate(Block<void>::Size_type s) noexcept
            {
                auto s1 = align(s);
                if (p_ + s1 > d_ + Size || !p_ || s <= 0) {
                    return { 0, nullptr };
                }
                Block<void> b = { s, p_ };
                p_ += s1;
                return b;
            }

            void deallocate(Block<void>& b) noexcept
            {
                if (b.data() == p_ - align(b.size())) {
                    p_ = reinterpret_cast<std::uint8_t*>(b.data());
                }
                b = {};
            }

            [[nodiscard]] bool owns(const Block<void>& b) const noexcept
            {
                return b.data() >= d_ && b.data() < d_ + Size;
            }

        private:
            Block<void>::Size_type align(Block<void>::Size_type s)
            {
                return s % 2 == 0 ? s : s + 1;
            }

            std::uint8_t d_[Size] = { 0 };
            std::uint8_t* p_{ d_ };
        };

        template <
            Allocator Internal_allocator,
            Block<void>::Size_type Min_size, Block<void>::Size_type Max_size, std::int64_t Max_list_size>
            class Free_list_allocator
            : private Internal_allocator {
            static_assert(Min_size > 1 && Min_size % 2 == 0);
            static_assert(Max_size > 1 && Max_size % 2 == 0);
            static_assert(Max_list_size > 0);
            public:
                Free_list_allocator() = default;
                Free_list_allocator(const Free_list_allocator& other) noexcept
                    : Internal_allocator(other), root_(nullptr), list_size_(0) {}
                Free_list_allocator operator=(const Free_list_allocator& other) noexcept
                {
                    if (this == &other) {
                        return *this;
                    }

                    Internal_allocator::operator=(other);
                    root_ = nullptr;
                    list_size_ = 0;
                    return *this;
                }
                Free_list_allocator(Free_list_allocator&& other) noexcept
                    : Internal_allocator(std::move(other)), root_(other.root_), list_size_(other.list_size_)
                {
                    other.root_ = nullptr;
                    other.list_size_ = 0;
                }
                Free_list_allocator& operator=(Free_list_allocator&& other) noexcept
                {
                    if (this == &other) {
                        return *this;
                    }

                    Internal_allocator::operator=(std::move(other));
                    root_ = other.root_;
                    list_size_ = other.list_size_;
                    other.root_ = nullptr;
                    other.list_size_ = 0;
                    return *this;
                }
                // Responsible to release the saved memory blocks
                virtual ~Free_list_allocator() noexcept
                {
                    for (std::int64_t i = 0; i < list_size_; ++i) {
                        Node* n = root_;
                        root_ = root_->next;
                        Block<void> b{ Max_size, n };
                        Internal_allocator::deallocate(b);
                    }
                }

                [[nodiscard]] Block<void> allocate(Block<void>::Size_type s) noexcept
                {
                    if (s >= Min_size && s <= Max_size && list_size_ > 0) {
                        Block<void> b = { s, root_ };
                        root_ = root_->next;
                        --list_size_;
                        return b;
                    }
                    Block<void> b = { s, Internal_allocator::allocate((s < Min_size || s > Max_size) ? s : Max_size).data() };
                    return b;
                }

                void deallocate(Block<void>& b) noexcept
                {
                    if (b.size() < Min_size || b.size() > Max_size || list_size_ > Max_list_size) {
                        Block<void> nb{ Max_size, b.data() };
                        b = {};
                        return Internal_allocator::deallocate(nb);
                    }
                    auto node = reinterpret_cast<Node*>(b.data());
                    node->next = root_;
                    root_ = node;
                    ++list_size_;
                    b = {};
                }

                [[nodiscard]] bool owns(const Block<void>& b) const noexcept
                {
                    return (b.size() >= Min_size && b.size() <= Max_size) || Internal_allocator::owns(b);
                }
            private:
                struct Node {
                    Node* next{ nullptr };
                };

                Node* root_{ nullptr };
                std::int64_t list_size_{ 0 };
        };

        template <typename T, Allocator Internal_allocator>
            requires (!std::is_reference_v<T>)
        class Stl_adapter_allocator
            : private Internal_allocator {
        public:
            using value_type = T;

            Stl_adapter_allocator() = default;
            Stl_adapter_allocator(const Stl_adapter_allocator& other) noexcept
                : Internal_allocator(other) {}
            Stl_adapter_allocator operator=(const Stl_adapter_allocator& other) noexcept
            {
                if (this == &other) {
                    return *this;
                }
                Internal_allocator::operator=(other);
                return *this;
            }
            Stl_adapter_allocator(Stl_adapter_allocator&& other) noexcept
                : Internal_allocator(std::move(other)) {}
            Stl_adapter_allocator& operator=(Stl_adapter_allocator&& other) noexcept
            {
                if (this == &other) {
                    return *this;
                }
                Internal_allocator::operator=(std::move(other));
                return *this;
            }
            virtual ~Stl_adapter_allocator() = default;

            template <typename U>
                requires (!std::is_reference_v<U>)
            constexpr Stl_adapter_allocator(const Stl_adapter_allocator<U, Internal_allocator>&) noexcept {}

            [[nodiscard]] T* allocate(std::size_t n)
            {
                Block<void> b = Internal_allocator::allocate(n * MEMOC_SSIZEOF(T));
                if (b.empty()) {
                    throw std::bad_alloc{};
                }
                return reinterpret_cast<T*>(b.data());
            }

            void deallocate(T* p, std::size_t n) noexcept
            {
                Block<void> b = { safe_64_unsigned_to_signed_cast(n) * MEMOC_SSIZEOF(T), reinterpret_cast<void*>(p) };
                Internal_allocator::deallocate(b);
            }
        };

        template <Allocator Internal_allocator, std::int64_t Number_of_records>
        class Stats_allocator
            : private Internal_allocator {
        public:
            struct Record {
                void* record_address{ nullptr };
                void* request_address{ nullptr };
                Block<void>::Size_type amount{ 0 };
                std::chrono::time_point<std::chrono::system_clock> time;
                Record* next{ nullptr };
            };

            Stats_allocator() = default;
            Stats_allocator(const Stats_allocator& other) noexcept
                : Internal_allocator(other)
            {
                for (Record* r = other.root_; r != nullptr; r = r->next) {
                    add_record(r->request_address, r->amount - MEMOC_SSIZEOF(Record), r->time);
                }
            }
            Stats_allocator operator=(const Stats_allocator& other) noexcept
            {
                if (this == &other) {
                    return *this;
                }
                Internal_allocator::operator=(other);
                for (Record* r = other.root_; r != nullptr; r = r->next) {
                    add_record(r->request_address, r->amount - MEMOC_SSIZEOF(Record), r->time);
                }
                return *this;
            }
            Stats_allocator(Stats_allocator&& other) noexcept
                : Internal_allocator(std::move(other)), number_of_records_(other.number_of_records_), total_allocated_(other.total_allocated_), root_(other.root_), tail_(other.tail_)
            {
                other.number_of_records_ = other.total_allocated_ = 0;
                other.root_ = other.tail_ = nullptr;
            }
            Stats_allocator& operator=(Stats_allocator&& other) noexcept
            {
                if (this == &other) {
                    return *this;
                }
                Internal_allocator::operator=(std::move(other));
                number_of_records_ = other.number_of_records_;
                total_allocated_ = other.total_allocated_;
                root_ = other.root_;
                tail_ = other.tail_;
                other.number_of_records_ = other.total_allocated_ = 0;
                other.root_ = other.tail_ = nullptr;
                return *this;
            }
            virtual ~Stats_allocator() noexcept
            {
                Record* c = root_;
                while (c) {
                    Record* n = c->next;
                    Block<void> b{ MEMOC_SSIZEOF(Record), c->record_address };
                    Internal_allocator::deallocate(b);
                    c = n;
                }
            }

            [[nodiscard]] Block<void> allocate(Block<void>::Size_type s) noexcept
            {
                Block<void> b = Internal_allocator::allocate(s);
                if (!b.empty()) {
                    add_record(b.data(), b.size());
                }
                return b;
            }

            void deallocate(Block<void>& b) noexcept
            {
                Block<void> bc{ b };
                Internal_allocator::deallocate(b);
                if (b.empty()) {
                    add_record(bc.data(), -bc.size());
                }
            }

            [[nodiscard]] bool owns(const Block<void>& b) const noexcept
            {
                return Internal_allocator::owns(b);
            }

            const Record* stats_list() const noexcept {
                return root_;
            }

            std::int64_t stats_list_size() const noexcept {
                return number_of_records_;
            }

            Block<void>::Size_type total_allocated() const noexcept {
                return total_allocated_;
            }

        private:
            void add_record(void* p, Block<void>::Size_type a, std::chrono::time_point<std::chrono::system_clock> time = std::chrono::system_clock::now()) {
                if (number_of_records_ >= Number_of_records) {
                    tail_->next = root_;
                    root_ = root_->next;
                    tail_ = tail_->next;
                    tail_->next = nullptr;
                    tail_->request_address = p;
                    tail_->amount = MEMOC_SSIZEOF(Record) + a;
                    tail_->time = time;

                    total_allocated_ += tail_->amount;

                    return;
                }

                Block<void> b1 = Internal_allocator::allocate(MEMOC_SSIZEOF(Record));
                if (b1.empty()) {
                    return;
                }

                if (!root_) {
                    root_ = reinterpret_cast<Record*>(b1.data());
                    tail_ = root_;
                }
                else {
                    tail_->next = reinterpret_cast<Record*>(b1.data());
                    tail_ = tail_->next;
                }
                tail_->record_address = b1.data();
                tail_->request_address = p;
                tail_->amount = b1.size() + a;
                tail_->time = time;
                tail_->next = nullptr;

                total_allocated_ += tail_->amount;

                ++number_of_records_;
            }

            std::int64_t number_of_records_{ 0 };
            Block<void>::Size_type total_allocated_{ 0 };
            Record* root_{ nullptr };
            Record* tail_{ nullptr };
        };

        template <Allocator Internal_allocator, std::int64_t id = -1>
        class Shared_allocator {
        public:
            [[nodiscard]] Block<void> allocate(Block<void>::Size_type s) noexcept
            {
                return allocator_.allocate(s);
            }

            void deallocate(Block<void>& b) noexcept
            {
                allocator_.deallocate(b);
            }

            [[nodiscard]] bool owns(const Block<void>& b) const noexcept
            {
                return allocator_.owns(b);
            }
        private:
            inline static Internal_allocator allocator_{};
        };

        // Allocators API

        template <Allocator T>
        [[nodiscard]] inline T create() noexcept
        {
            return T();
        }

        template <Allocator T>
        [[nodiscard]] inline erroc::Expected<Block<void>, Allocator_error> allocate(T& allocator, Block<void>::Size_type size) noexcept
        {
            if (size < 0) {
                return erroc::Unexpected(Allocator_error::invalid_size);
            }

            if (size == 0) {
                return Block<void>();
            }

            Block<void> b = allocator.allocate(size);
            if (b.empty()) {
                return erroc::Unexpected(Allocator_error::unknown);
            }
            return b;
        }

        template <Allocator T>
        inline void deallocate(T& allocator, Block<void>& block) noexcept
        {
            allocator.deallocate(block);
        }

        template <Allocator T>
        [[nodiscard]] inline bool owns(const T& allocator, const Block<void>& block) noexcept
        {
            return allocator.owns(block);
        }
    }

    using details::Allocator;
    using details::Fallback_allocator;
    using details::Free_list_allocator;
    using details::Malloc_allocator;
    using details::Malloc_allocator;
    using details::Shared_allocator;
    using details::Stack_allocator;
    using details::Stats_allocator;
    using details::Stl_adapter_allocator;
    using details::create;
    using details::allocate;
    using details::deallocate;
    using details::owns;
}

#endif // MEMOC_ALLOCATORS_H

