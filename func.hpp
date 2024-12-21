#pragma once

#include <algorithm> // std::max
#include <concepts> // std::invocable_r
#include <exception> // std::exception
#include <memory> // std::addressof
#include <type_traits> // just in case
#include <utility> // std::size_t

#if defined __GNUC__ // GCC, Clang
    #define VX_UNREACHABLE() __builtin_unreachable()
#elif defined _MSC_VER // MSVC
    #define VX_UNREACHABLE() (__assume(false))
#else 
    #define VX_UNREACHABLE()
#endif

namespace vx {

namespace configuration {
struct function {
    std::size_t SBO { 32 };
    std::size_t alignment { alignof(std::max_align_t) };
    bool allow_return_type_conversion { true };
    bool require_nothrow_relocatable { false }; //!TODO: Implement support for [p1144][p3236]
    bool require_nothrow_invocable { false };
    bool require_nothrow_copyable { false };
    bool require_const_invocable { false };
    bool require_nothrow_movable { true };
    bool enable_typeinfo { false };
    bool can_be_empty { false };
    bool check_empty { false };
    bool allow_heap { true };
    bool copyable { false };
    bool movable { true };

    // Setters
    [[nodiscard]] constexpr function with_nothrow_invocable(bool state) const noexcept {
        auto copy = *this;
        copy.require_nothrow_invocable = state;
        return copy;
    }

    [[nodiscard]] constexpr function with_const_invocable(bool state) const noexcept {
        function copy = *this;
        copy.require_const_invocable = state;
        return copy;
    }

    constexpr bool has_empty_state() const noexcept {
        return can_be_empty || check_empty;
    }
};
} // namespace configuration
namespace cfg = configuration;


struct bad_function_call : std::exception {
    virtual ~bad_function_call() noexcept {}
    const char * what() const noexcept { return "Function's operator() called, but function has not been set or was moved from"; }
};

struct bad_function_operation : std::exception {
    bad_function_operation(const char * const msg) : error_message(msg) {}
    virtual ~bad_function_operation() noexcept {}
    const char * what() const noexcept { return error_message; }
private:
    const char * const error_message = "";
};

namespace detail {

// SBO memory
template <std::size_t capacity, std::size_t alignment>
union memory_SBO {
    alignas(alignment) std::byte sbo [capacity];
    void* ptr;
    const void* const_ptr;

    template <typename F>
    F& as_sbo() {
        return reinterpret_cast<F&>(*this);
    }

    template <typename F>
    F const& as_sbo() const {
        return reinterpret_cast<F const&>(*this);
    }

    template <typename F>
    F*& ptr_to() {
        return reinterpret_cast<F*&>(*this);
    }

    template <typename F>
    const F* const& ptr_to() const {
        return reinterpret_cast<const F* const&>(*this);
    }

    template <typename F>
    void move_into_sbo(memory_SBO * other) {
        new(other) F(std::move(as_sbo<F>()));
    }

    template <typename>
    void move_into_ptr(memory_SBO * other) {
        other->ptr = ptr;
        ptr = nullptr;
    }

    template <typename F>
    void copy_into_sbo(memory_SBO * dest) const noexcept(std::is_nothrow_copy_constructible_v<F>) {
        dest->template as_sbo<F>() = this->as_sbo<F>();
    }

    template <typename F>
    void copy_into_ptr(memory_SBO * dest) const noexcept(std::is_nothrow_copy_constructible_v<F>) {
        dest->template ptr_to<F>() = new F(*this->ptr_to<F>());
    }

    template <typename F>
    void del_sbo() { as_sbo<F>().~F(); }

    template <typename F>
    void del_ptr() { delete ptr_to<F>(); }
};


template <configuration::function cfg, typename R, typename... Args>
class func_base {
public:
    template <typename F>
    static constexpr bool is_sbo_eligible = sizeof(F) <= cfg.SBO && ///< fits into SBO buffer
                                     alignof(F) <= cfg.alignment && ///< and has lower alignment
                                     (cfg.alignment % alignof(F) == 0) && 
                                     (!cfg.require_nothrow_movable || std::is_nothrow_move_constructible_v<F>);

private:
    static constexpr auto bufsize = std::max(cfg.SBO, std::size_t{1});
    using memory = memory_SBO<bufsize, cfg.alignment>;

    template <typename F>
    using const_correct = std::conditional_t<(cfg.require_const_invocable), std::add_const_t<F>, F>;

    static constexpr bool is_tagfunc_nothrow_movable = (cfg.require_nothrow_movable || not cfg.movable) && (cfg.require_nothrow_copyable || not cfg.copyable);
    static constexpr bool has_multiple_actions = (cfg.movable || cfg.copyable || cfg.enable_typeinfo);

    enum class dispatch_tag { Dtor, Move, Copy, GetPtr, TypeInfo };
    using p_cleanup = void (*)(memory&) noexcept;
    using p_tagfunc = void (*)(dispatch_tag, memory&, memory*) noexcept(is_tagfunc_nothrow_movable);
    using action_f = std::conditional_t<has_multiple_actions, p_tagfunc, p_cleanup>; 
    using invoke_f = R (*)(const_correct<memory> &, Args...) noexcept(cfg.require_nothrow_invocable);

    template <typename F>
    static constexpr p_cleanup dtor_action =  +[](memory& mem) noexcept { 
        if constexpr (is_sbo_eligible<F>) {
            mem.template del_sbo<F>(); 
        } else {
            mem.template del_ptr<F>();
        }
    };

    template <typename F>
    static constexpr p_tagfunc multiple_actions = +[](dispatch_tag cmd, memory& mem, memory* new_mem=nullptr) noexcept(is_tagfunc_nothrow_movable) { 
        switch (cmd) {
            case dispatch_tag::Dtor: {
                if constexpr (is_sbo_eligible<F>) {
                    mem.template del_sbo<F>();
                } else {
                    mem.template del_ptr<F>();
                }
            } break;

            case dispatch_tag::Move: {
                if constexpr (cfg.movable) {
                    if constexpr (is_sbo_eligible<F>) {
                        mem.template move_into_sbo<F>(new_mem);
                    } else {
                        mem.template move_into_ptr<F>(new_mem);
                    }
                } else {
                    VX_UNREACHABLE();
                }
            } break;

            case dispatch_tag::Copy: {
                if constexpr (cfg.copyable) {
                    if constexpr (is_sbo_eligible<F>) {
                        mem.template copy_into_sbo<F>(new_mem);
                    } else {
                        mem.template copy_into_ptr<F>(new_mem);
                    }
                } else {
                    VX_UNREACHABLE();
                }
            } break;

            case dispatch_tag::GetPtr: {
                if constexpr (cfg.enable_typeinfo) {
                    if constexpr (is_sbo_eligible<F>) { 
                        new_mem->ptr = &mem.template as_sbo<F>(); 
                    } else { 
                        new_mem->ptr = mem.template ptr_to<F>(); 
                    }
                } else {
                    VX_UNREACHABLE();
                }
            } break;

            case dispatch_tag::TypeInfo: {
                if constexpr (cfg.enable_typeinfo) {
                    new_mem->ptr = const_cast<void*>(static_cast<const void*>(&typeid(F)));
                } else {
                    VX_UNREACHABLE();
                }
            } break;
        }
    };

    template <typename F>
    static auto& as_invocable(const_correct<memory>& mem) noexcept {
        if constexpr (is_sbo_eligible<F>) {
            return mem.template as_sbo<F>();
        } else {
            return *mem.template ptr_to<F>();
        }
    }

    template <typename F>
    static constexpr invoke_f caller_for = +[](const_correct<memory>& mem, Args... args) noexcept(cfg.require_nothrow_invocable) {
        auto& f = as_invocable<F>(mem);
        if constexpr (cfg.allow_return_type_conversion) {
            if constexpr (!std::is_void_v<R>) { 
                return R( f(args...) ); 
            } else {
                f(args...);
            }
        } else {
            return f(args...);
        }
    };

    static constexpr p_tagfunc noop_actions = +[](dispatch_tag, memory&, memory*) noexcept {};


public:
    func_base() noexcept requires (cfg.can_be_empty)
    : call{nullptr}
    , actions{noop_actions}
    {}

    func_base(std::nullptr_t) noexcept requires (cfg.can_be_empty)
    : call{nullptr}
    , actions{noop_actions}
    {}


    template <std::invocable<Args...> F>
    func_base (F && callable) requires (
        !std::same_as<std::decay_t<F>, func_base> &&
        (cfg.allow_heap || is_sbo_eligible<std::decay_t<F>>))
    {
        using function_type = std::decay_t<F>;

        if constexpr (cfg.require_nothrow_invocable) {
            static_assert(noexcept(std::invoke(callable, std::declval<Args>()...)),
                "Noexcept callable expected");
        }
        
        if constexpr (is_sbo_eligible<function_type>) { /// SBO case
            new(&data.sbo) function_type(std::forward<F>(callable)); ///< [sbo] created in-place in SBO buffer
        } else { /// dynamic memory allocation case
            static_assert(cfg.allow_heap, 
                "The callable doesn't fit into the SBO buffer [Heap allocation disallowed by the configuration]");
            
            data.ptr = new function_type{std::forward<F>(callable)}; ///< [ptr] allocated on the heap
        }

        call = caller_for<F>;

        /// In-place function case
        if constexpr (not has_multiple_actions) {
            actions = dtor_action<function_type>;
        } else { /// movable and optionally copyable too
            actions = multiple_actions<function_type>;
        }
    }

    // template <configuration::function cfg2>
    // func_base (func_base<cfg2, R, Args...> && other)
    // noexcept(cfg.SBO == 0 || cfg.require_nothrow_movable)
    // requires (
    //     cfg.SBO >= cfg2.SBO && cfg.alignment >= cfg2.alignment  ///< guaranteed to fit into target's SBO
    //     && cfg.allow_return_type_conversion == cfg2.allow_return_type_conversion ///< let's not mess with the return types for now
    //     && cfg.require_nothrow_invocable == cfg2.require_nothrow_invocable ///< preserve nothrow
    //     && cfg.require_const_invocable == cfg2.require_const_invocable ///< preserve constness
    //     && cfg.require_nothrow_movable == cfg2.require_nothrow_movable ///< preserve nothrow move
    //     // && cfg.can_be_empty == cfg2.can_be_empty 
    //     // && cfg.check_empty 
    //     && (cfg2.allow_heap? cfg2.allow_heap == cfg.allow_heap : true) ///< if the moved-from type doesn't allow heap
    //                                                                    ///  and we know that it should fit into our SBO then OK
        
    //     && (cfg.copyable? cfg2.copyable : true) ///< if the target function is copyable 
    //                                             ///  then the moved-from also should provide the copy action
    //     && cfg.movable)
    // : call{other.call}
    // , actions{other.actions}
    // {
    //     if constexpr (cfg2.can_be_empty && !cfg.can_be_empty) {
    //         if (!static_cast<bool>(other)) { throw bad_function_operation{"move constructing from an empty function but this function cannot be empty!"}; } 
    //     }
    //     other.move_into(data);
    //     other.call = nullptr;
    //     other.actions = noop_actions;
    // }

    /// MOVE (if movable == true)
    func_base(func_base&& other)
    noexcept(cfg.SBO == 0 || cfg.require_nothrow_movable)
    requires (cfg.movable)
    {
        other.move_into(data);
        call = std::exchange(other.call, nullptr);
        actions = std::exchange(other.actions, noop_actions);
    }


    func_base& operator= (func_base&& other) 
    noexcept(cfg.SBO == 0 || cfg.require_nothrow_movable)
    requires (cfg.movable) 
    {
        reset();
        other.move_into(data);
        call = std::exchange(other.call, nullptr);
        actions = std::exchange(other.actions, noop_actions);
        return *this;
    }


    /// COPY (if copyable == true)
    func_base(func_base const& other) requires (cfg.copyable) 
    : call{other.call}
    , actions{other.actions} 
    {
        other.copy_into(data);
    }


    func_base& operator= (func_base const& other) requires (cfg.copyable) {
        reset();
        other.copy_into(data);
        call = other.call;
        actions = other.actions;
        return *this;
    }


    void swap(func_base & other) noexcept requires(cfg.movable) {
        if (&other == this) { return; }
        // std::swap(data, other.data); ///< Probably cannot use that...
        
        //!TODO: Check if assembly differs:
        // memory tmp;
        // this->move_into(tmp);
        // other.move_into(data);
        // actions(dispatch_tag::Move, tmp, &other.data);

        memory tmp;
        other.move_into(tmp);
        this->move_into(other.data);
        actions(dispatch_tag::Move, tmp, std::addressof(data));

        std::swap(call, other.call);
        std::swap(actions, other.actions);
    }


    R operator() (Args... args) noexcept(cfg.require_nothrow_invocable && !cfg.check_empty) {
        if constexpr (cfg.check_empty) {
            if (call == nullptr) { throw bad_function_call{}; }
        }
        return call(get_data(), std::forward<Args>(args)...);
    } 


    /// If function's signature contains `const` 
    R operator() (Args... args) const noexcept(cfg.require_nothrow_invocable && !cfg.check_empty) 
    requires (cfg.require_const_invocable) {
        if constexpr (cfg.check_empty) {
            if (call == nullptr) { throw bad_function_call{}; }
        }
        return call(get_data(), std::forward<Args>(args)...);
    } 


    const std::type_info& target_type() const noexcept 
    requires(cfg.enable_typeinfo) {
        memory ans;
        actions(dispatch_tag::TypeInfo, const_cast<memory&>(data), &ans);
        return *static_cast<const std::type_info*>(ans.ptr);
    }


    template <typename F>
    F* target() noexcept 
    requires(cfg.enable_typeinfo) {
        if (typeid(F) != target_type()) { return nullptr; }
        memory ans;
        actions(dispatch_tag::GetPtr, const_cast<memory&>(data), &ans);
        return reinterpret_cast<F*>(ans.ptr);
    }


    template <typename F>
    const F* target() const noexcept 
    requires(cfg.enable_typeinfo) {
        if (typeid(F) != target_type()) { return nullptr; }
        memory ans;
        actions(dispatch_tag::GetPtr, const_cast<memory&>(data), &ans);
        return reinterpret_cast<const F*>(ans.ptr);
    }


    operator bool() const noexcept {
        return call != nullptr; 
    }


    template <typename R2, typename... Args2, configuration::function cfg2>
    bool operator== (func_base<cfg2, R2(Args2...)> const &) const = delete;
    

    ~func_base() {
        if constexpr (not has_multiple_actions) {
            actions(get_data());
        } else {
            if (call == nullptr) { return; } /// moved-out
            actions(dispatch_tag::Dtor, get_data(), nullptr);
        }
    }

protected:
    void reset() { 
        actions(dispatch_tag::Dtor, data, nullptr); 
        call = nullptr;
        actions = noop_actions;
    }

    auto& get_data() {
        return data;
    }

    auto const& get_data() const {
        return data;
    }

    void move_into(memory& mem) {
        actions(dispatch_tag::Move, data, std::addressof(mem));
    }

    void copy_into(memory& mem) const {
        actions(dispatch_tag::Copy, const_cast<memory&>(data), std::addressof(mem));
    }

private:
    memory data;
    invoke_f call = nullptr;
    action_f actions = nullptr;
};

} // namespace detail


template <typename Signature, configuration::function cfg = configuration::function{}>
class func;

template <typename R, typename... Args, configuration::function cfg>
class func<R(Args...), cfg> : public detail::func_base<cfg, R, Args...> {
    using detail::func_base<cfg, R, Args...>::func_base;
};

template <typename R, typename... Args, configuration::function cfg>
class func<R(Args...) const, cfg> : public detail::func_base<cfg.with_const_invocable(true), R, Args...> {
    using detail::func_base<cfg.with_const_invocable(true), R, Args...>::func_base;
};

template <typename R, typename... Args, configuration::function cfg>
class func<R(Args...) noexcept, cfg> : public detail::func_base<cfg.with_nothrow_invocable(true), R, Args...> {
    using detail::func_base<cfg.with_nothrow_invocable(true), R, Args...>::func_base;
};

template <typename R, typename... Args, configuration::function cfg>
class func<R(Args...) const noexcept, cfg> : public detail::func_base<cfg.with_const_invocable(true).with_nothrow_invocable(true), R, Args...> {
    using detail::func_base<cfg.with_const_invocable(true).with_nothrow_invocable(true), R, Args...>::func_base;
};


/// @brief A helper trait to check if the type F is sbo eligible (i.e. possible to use with small buffer optimization)
/// @tparam function - func<signature, cfg>
/// @tparam F - type to check sbo eligibility for
template <class function, typename F>
constexpr bool is_sbo_eligible = function::template is_sbo_eligible<F>;

} // namespace vx

#undef VX_UNREACHABLE
