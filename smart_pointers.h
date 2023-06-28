#pragma once
#include <iostream>
#include <memory>
#include <type_traits>

template <typename T>
class SharedPtr;

template <typename T>
class WeakPtr;

template <typename T>
class EnableSharedFromThis;

struct BaseControlBlock {
    size_t shared_cnt;
    size_t weak_cnt;

    BaseControlBlock(size_t sc, size_t wc) : shared_cnt(sc), weak_cnt(wc) {}
    virtual void destroy_object() = 0;
    virtual void deallocate_cb() = 0;
    virtual ~BaseControlBlock() = default;
};

template <typename T, typename Deleter, typename Alloc>
struct RegularControlBlock : BaseControlBlock {
    T* object;
    Deleter del;
    Alloc alloc;

    using AllocTraits = typename std::allocator_traits<Alloc>;
    using BlockAlloc = typename AllocTraits::template rebind_alloc<
        RegularControlBlock<T, Deleter, Alloc>>;
    using BlockAllocTraits = typename AllocTraits::template rebind_traits<
        RegularControlBlock<T, Deleter, Alloc>>;

    RegularControlBlock(size_t sc, size_t wc, T* ptr, const Deleter& d, Alloc a)
        : BaseControlBlock(sc, wc), object(ptr), del(d), alloc(a) {}

    void destroy_object() override {
        del(object);
    }

    void deallocate_cb() override {
        BlockAlloc cb_alloc = alloc;
        BlockAllocTraits::deallocate(cb_alloc, this, 1);
    }

    ~RegularControlBlock() override = default;
};

template <typename T, typename Alloc = std::allocator<T>>
struct MakeSharedControlBlock : BaseControlBlock {
    Alloc alloc;
    T object;

    using AllocTraits = typename std::allocator_traits<Alloc>;
    using BlockAlloc = typename AllocTraits::template rebind_alloc<
        MakeSharedControlBlock<T, Alloc>>;
    using BlockAllocTraits = typename AllocTraits::template rebind_traits<
        MakeSharedControlBlock<T, Alloc>>;
    using ObjAlloc = typename AllocTraits::template rebind_alloc<T>;
    using ObjAllocTraits = typename AllocTraits::template rebind_traits<T>;

    template <typename... Args>
    MakeSharedControlBlock(size_t sc, size_t wc, Alloc a, Args&&... args)
        : BaseControlBlock(sc, wc),
          alloc(a),
          object(std::forward<Args>(args)...) {}

    void destroy_object() override {
        ObjAlloc obj_alloc = alloc;
        ObjAllocTraits::destroy(obj_alloc, &object);
    }

    void deallocate_cb() override {
        BlockAlloc cb_alloc = alloc;
        BlockAllocTraits::deallocate(cb_alloc, this, 1);
    }

    ~MakeSharedControlBlock() override = default;
};

template <typename T>
class SharedPtr {
  public:
    SharedPtr() {}

    template <typename Deleter = std::default_delete<T>,
              typename Alloc = std::allocator<T>>
    SharedPtr(T* ptr, const Deleter& del = Deleter(), Alloc alloc = Alloc())
        : object(ptr) {
        update_enable_shared_from_this();

        using AllocTraits = typename std::allocator_traits<Alloc>;
        using RegularControlBlockType = RegularControlBlock<T, Deleter, Alloc>;
        using RegularControlBlockAlloc =
            typename AllocTraits::template rebind_alloc<
                RegularControlBlockType>;
        using RegularControlBlockAllocTraits =
            typename AllocTraits::template rebind_traits<
                RegularControlBlockType>;
        RegularControlBlockAlloc cb_alloc = alloc;

        auto regular_cb = RegularControlBlockAllocTraits::allocate(cb_alloc, 1);
        new (regular_cb) RegularControlBlockType(1, 0, ptr, del, cb_alloc);
        cb = regular_cb;
    }

    template <typename Y, typename Deleter = std::default_delete<T>,
              typename Alloc = std::allocator<T>>
        requires std::is_convertible_v<Y, T>
    SharedPtr(Y* ptr, const Deleter& del = Deleter(), Alloc alloc = Alloc())
        : object(ptr) {
        update_enable_shared_from_this();

        using AllocTraits = typename std::allocator_traits<Alloc>;
        using RegularControlBlockType = RegularControlBlock<Y, Deleter, Alloc>;
        using RegularControlBlockAlloc =
            typename AllocTraits::template rebind_alloc<
                RegularControlBlockType>;
        using RegularControlBlockAllocTraits =
            typename AllocTraits::template rebind_traits<
                RegularControlBlockType>;
        RegularControlBlockAlloc cb_alloc = alloc;

        auto regular_cb = RegularControlBlockAllocTraits::allocate(cb_alloc, 1);
        new (regular_cb) RegularControlBlockType(1, 0, ptr, del, cb_alloc);
        cb = regular_cb;
    }

    SharedPtr(const SharedPtr& shptr) noexcept
        : object(shptr.object), cb(shptr.cb) {
        update_enable_shared_from_this();
        if (cb != nullptr) {
            ++cb->shared_cnt;
        }
    }

    template <typename Y>
        requires std::is_convertible_v<Y, T>
    SharedPtr(const SharedPtr<Y>& shptr) noexcept
        : object(shptr.object), cb(shptr.cb) {
        update_enable_shared_from_this();
        if (cb != nullptr) {
            ++cb->shared_cnt;
        }
    }

    template <typename Y>
        requires std::is_convertible_v<Y, T>
    SharedPtr(SharedPtr<Y>&& shptr) noexcept
        : object(shptr.object), cb(shptr.cb) {
        update_enable_shared_from_this();
        shptr.object = nullptr;
        shptr.cb = nullptr;
    }

    SharedPtr& operator=(const SharedPtr& shptr) noexcept {
        if (this == &shptr) {
            return *this;
        }
        SharedPtr copy(shptr);
        swap(copy);
        return *this;
    }

    template <typename Y>
        requires std::is_convertible_v<Y, T>
    SharedPtr& operator=(const SharedPtr<Y>& shptr) noexcept {
        SharedPtr copy(shptr);
        swap(copy);
        return *this;
    }

    SharedPtr& operator=(SharedPtr&& shptr) noexcept {
        SharedPtr copy(std::move(shptr));
        swap(copy);
        return *this;
    }

    template <typename Y>
        requires std::is_convertible_v<Y, T>
    SharedPtr& operator=(SharedPtr<Y>&& shptr) noexcept {
        SharedPtr copy(std::move(shptr));
        swap(copy);
        return *this;
    }

    T& operator*() const noexcept {
        if (object) {
            return *object;
        }
        return static_cast<MakeSharedControlBlock<T>*>(cb)->object;
    }

    T* operator->() const noexcept {
        if (object) {
            return object;
        }
        return &(static_cast<MakeSharedControlBlock<T>*>(cb)->object);
    }

    ~SharedPtr() {
        clear();
    }

    size_t use_count() const {
        return cb->shared_cnt;
    }

    T* get() const {
        return object;
    }

    void reset() {
        SharedPtr<T>().swap(*this);
    }

    template <typename Y, typename Deleter = std::default_delete<T>,
              typename Alloc = std::allocator<T>>
        requires std::is_convertible_v<Y, T>
    void reset(Y* ptr, Deleter del = Deleter(), Alloc alloc = Alloc()) {
        SharedPtr<T>(ptr, del, alloc).swap(*this);
    }

    void swap(SharedPtr& other) {
        std::swap(object, other.object);
        std::swap(cb, other.cb);
    }

  private:
    template <typename Alloc>
    SharedPtr(MakeSharedControlBlock<T, Alloc>* make_shared_cb)
        : object(nullptr), cb(make_shared_cb) {
        update_enable_shared_from_this();
    }

    SharedPtr(WeakPtr<T> wptr) : object(wptr.object), cb(wptr.cb) {
        if (cb != nullptr) {
            ++cb->shared_cnt;
        }
    }

    void clear() {
        if (cb == nullptr) {
            return;
        }
        --cb->shared_cnt;
        if (cb->shared_cnt == 0) {
            cb->destroy_object();
            if (cb->weak_cnt == 0) {
                cb->deallocate_cb();
            }
        }
    }

    void update_enable_shared_from_this() {
        if (cb == nullptr) {
            return;
        }

        if constexpr (std::is_base_of_v<EnableSharedFromThis<T>, T>) {
            if (object != nullptr) {
                object->wptr = *this;
            } else {
                static_cast<MakeSharedControlBlock<T>*>(cb)->object.wptr =
                    *this;
            }
        }
    }

    T* object = nullptr;
    BaseControlBlock* cb = nullptr;

    template <typename Y>
    friend class WeakPtr;

    template <typename Y>
    friend class SharedPtr;

    template <typename Y, typename... Args>
    friend SharedPtr<Y> makeShared(Args&&...);

    template <typename Y, typename Alloc, typename... Args>
    friend SharedPtr<Y> allocateShared(Alloc, Args&&...);
};

template <typename T, typename... Args>
SharedPtr<T> makeShared(Args&&... args) {
    auto cb = new MakeSharedControlBlock<T, std::allocator<T>>(
        1, 0, std::allocator<T>(), std::forward<Args>(args)...);
    return SharedPtr<T>(cb);
}

template <typename T, typename Alloc, typename... Args>
SharedPtr<T> allocateShared(Alloc alloc, Args&&... args) {
    using AllocTraits = typename std::allocator_traits<Alloc>;
    using MakeSharedBlockAlloc = typename AllocTraits::template rebind_alloc<
        MakeSharedControlBlock<T, Alloc>>;
    using MakeSharedBlockAllocTraits =
        typename AllocTraits::template rebind_traits<
            MakeSharedControlBlock<T, Alloc>>;
    MakeSharedBlockAlloc cb_alloc = alloc;

    auto cb = MakeSharedBlockAllocTraits::allocate(cb_alloc, 1);
    MakeSharedBlockAllocTraits::construct(cb_alloc, cb, 1, 0, cb_alloc,
                                          std::forward<Args>(args)...);
    return SharedPtr<T>(cb);
}

template <typename T>
class WeakPtr {
  public:
    WeakPtr() {}

    WeakPtr(const SharedPtr<T>& sp) : object(sp.object), cb(sp.cb) {
        if (cb != nullptr) {
            ++cb->weak_cnt;
        }
    }

    template <typename Y>
        requires std::is_convertible_v<Y, T>
    WeakPtr(const SharedPtr<Y>& sp) : object(sp.object), cb(sp.cb) {
        if (cb != nullptr) {
            ++cb->weak_cnt;
        }
    }

    WeakPtr(const WeakPtr& wptr) : object(wptr.object), cb(wptr.cb) {
        if (cb != nullptr) {
            ++cb->weak_cnt;
        }
    }

    template <typename Y>
        requires std::is_convertible_v<Y, T>
    WeakPtr(const WeakPtr<Y>& wptr) : object(wptr.object), cb(wptr.cb) {
        if (cb != nullptr) {
            ++cb->weak_cnt;
        }
    }

    WeakPtr(WeakPtr&& wptr) : object(wptr.object), cb(wptr.cb) {
        wptr.object = nullptr;
        wptr.cb = nullptr;
    }

    template <typename Y>
        requires std::is_convertible_v<Y, T>
    WeakPtr(WeakPtr<Y>&& wptr) : object(wptr.object), cb(wptr.cb) {
        wptr.object = nullptr;
        wptr.cb = nullptr;
    }

    WeakPtr& operator=(const WeakPtr& wptr) {
        if (this == &wptr) {
            return *this;
        }
        WeakPtr copy(wptr);
        swap(copy);
        return *this;
    }

    template <typename Y>
        requires std::is_convertible_v<Y, T>
    WeakPtr& operator=(const WeakPtr<Y>& wptr) {
        WeakPtr copy(wptr);
        swap(copy);
        return *this;
    }

    WeakPtr& operator=(WeakPtr&& wptr) {
        WeakPtr copy(std::move(wptr));
        swap(copy);
        return *this;
    }

    template <typename Y>
        requires std::is_convertible_v<Y, T>
    WeakPtr& operator=(WeakPtr<Y>&& wptr) {
        WeakPtr copy(std::move(wptr));
        swap(copy);
        return *this;
    }

    ~WeakPtr() {
        clear();
    }

    size_t use_count() const noexcept {
        if (cb == nullptr) {
            return 0;
        }
        return cb->shared_cnt;
    }

    bool expired() const noexcept {
        return use_count() == 0;
    }

    SharedPtr<T> lock() const noexcept {
        if (expired()) {
            return SharedPtr<T>();
        }
        return SharedPtr<T>(*this);
    }

  private:
    void swap(WeakPtr& other) {
        std::swap(object, other.object);
        std::swap(cb, other.cb);
    }

    void clear() {
        if (cb == nullptr) {
            return;
        }
        --cb->weak_cnt;
        if (cb->shared_cnt == 0 && cb->weak_cnt == 0) {
            cb->deallocate_cb();
        }
    }

    T* object = nullptr;
    BaseControlBlock* cb = nullptr;

    template <typename Y>
    friend class WeakPtr;

    template <typename Y>
    friend class SharedPtr;
};

template <typename T>
class EnableSharedFromThis {
  public:
    SharedPtr<T> shared_from_this() const noexcept {
        return wptr.lock();
    }

  private:
    WeakPtr<T> wptr;
};
