#pragma once

#include <algorithm> // std::max
#include <concepts> // std::invocable_r
#include <utility> // std::size_t
#include <exception> // std::exception
#include <type_traits> // just in case

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
    bool require_nothrow_invocable {false};
    bool require_const_invocable {false};
    bool require_nothrow_movable {true};
    bool can_be_empty {false};
    bool check_empty { false };
    bool allow_heap { true };
    bool copyable { false };
    bool movable { true };

    // Setters
    constexpr function set_nothrow_invocable(bool state=true) const noexcept {
        auto copy = *this;
        copy.require_nothrow_invocable = state;
        return copy;
    }

    constexpr function set_const_invocable(bool state=true) const noexcept {
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


enum class DispatchTag {
    Dtor,
    Move,
    Copy
};


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


template <configuration::function cfg, typename R, typename... Args>
class FunctionContainer {
    static constexpr auto bufsize = std::max(cfg.SBO, std::size_t{1});
    using buffer_t = std::byte[bufsize]; //std::array<std::byte, bufsize>;

    union memory {
        alignas(cfg.alignment) buffer_t sbo;
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
    };

    using dispatch_tag = DispatchTag;
    using p_cleanup = void (*)(memory&) noexcept;
    using p_tagfunc = void (*)(dispatch_tag, memory&, memory*);
    using action_f = std::conditional_t<(cfg.movable || cfg.copyable), p_tagfunc, p_cleanup>; 
    using invoke_f = std::conditional_t<(cfg.require_const_invocable), 
        R (*)(memory const&, Args...),
        R (*)(memory&, Args...)>;

    template <typename F>
    using const_correct = std::conditional_t<(cfg.require_const_invocable),
        const F, 
        F>;


    template <typename F>
    static constexpr p_cleanup sbo_dtor_action =  +[](memory& mem) noexcept { 
        F& data = mem.template as_sbo<F>(); 
        data.~F(); 
    };

    template <typename F>
    static constexpr p_cleanup ptr_dtor_action = +[](memory& mem) noexcept { 
        F* f = mem.template ptr_to<F>();
        delete f;
    };

    template <typename F>
    static constexpr p_tagfunc sbo_multiple_actions = +[](dispatch_tag cmd, memory& mem, memory* new_mem=nullptr) { 
        F& data = mem.template as_sbo<F>(); 
        switch (cmd) {
            case dispatch_tag::Dtor: {
                data.~F(); 
            } break;

            case dispatch_tag::Move: {
                new(new_mem) F(std::move(data));
            } break;

            case dispatch_tag::Copy: {
                if constexpr (cfg.copyable) {
                    auto& new_data = new_mem->template as_sbo<F>();
                    new_data = data;
                } else {
                    VX_UNREACHABLE();
                }
            } break;
        }
    };

    template <typename F>
    static constexpr p_tagfunc ptr_multiple_actions = +[](dispatch_tag cmd, memory& mem, memory* new_mem=nullptr) { 
        F* f = mem.template ptr_to<F>(); 
        switch (cmd) {
            case dispatch_tag::Dtor: {
                delete f;
            } break;

            case dispatch_tag::Move: {
                // new(new_mem) (F*)(f);
                new_mem->ptr = f;
                f = nullptr;
            } break;

            case dispatch_tag::Copy: {
                if constexpr (cfg.copyable) {
                    F*& new_ptr = new_mem->template ptr_to<F>();
                    new_ptr = new F(*f);
                }
            } break;
        }
    };

    static constexpr p_tagfunc noop_actions = +[](dispatch_tag, memory&, memory*) noexcept {};


public:

    FunctionContainer() noexcept requires (cfg.can_be_empty)
    : call{nullptr}
    , actions{nullptr}
    {}

    FunctionContainer(std::nullptr_t) noexcept requires (cfg.can_be_empty)
    : call{nullptr}
    , actions{nullptr}
    {}

    template <std::invocable<Args...> F>
    FunctionContainer (F && callable) 
    // requires (cfg.allow_return_type_conversion ? 
        // (invocable<F, R(Args...)> || std::is_void_v<R>) 
        // : 
        // std::is_invocable_r_v<R, F, Args...> && std::is_same_v<std::invoke_result_t<F, Args...>, R>)
    {
        using function_type = std::decay_t<F>;

        if constexpr (cfg.require_nothrow_invocable) {
            static_assert(noexcept(std::invoke(callable, std::declval<Args>()...)),
                "Noexcept callable expected");
        }
        /// SBO case
        if constexpr (sizeof(function_type) <= cfg.SBO && ///< fits into SBO buffer
                      alignof(function_type) <= cfg.alignment && ///< and has lower alignment
                      (cfg.alignment % alignof(function_type) == 0) && 
                      (!cfg.require_nothrow_movable || std::is_nothrow_move_constructible_v<function_type>)) { 
            /// [sbo] created in-place in SBO buffer
            new(&data.sbo) function_type(std::forward<F>(callable));

            call = +[](const_correct<memory>& mem, Args... args) {
                auto& f = mem.template as_sbo<function_type>();
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

            /// In-place function case
            if constexpr (!cfg.movable && !cfg.copyable) {
                actions = sbo_dtor_action<function_type>;
            
            /// movable and optionally copyable too
            } else { 
                actions = sbo_multiple_actions<function_type>;
            }

        /// dynamic memory allocation case
        } else { 
            static_assert(cfg.allow_heap, 
                "The callable doesn't fit into the SBO buffer [Heap allocation disallowed by the configuration]");
            
            /// [ptr] allocated on the heap
            data.ptr = new function_type{std::forward<F>(callable)};

            call = +[](const_correct<memory>& mem, Args... args) {
                auto& f = *mem.template ptr_to<function_type>();
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

            if constexpr (!cfg.movable && !cfg.copyable) {
                actions = ptr_dtor_action<function_type>;
            } else { 
                /// movable and optionally copyable too
                actions = ptr_multiple_actions<function_type>;
            }
        }
    }


    // template <configuration::function cfg2>
    // FunctionContainer (FunctionContainer<cfg2, R, Args...> && other)
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
    FunctionContainer(FunctionContainer&& other)
    noexcept(cfg.SBO == 0 || cfg.require_nothrow_movable)
    requires (cfg.movable)
    : call{other.call}
    , actions{other.actions}
    {
        other.move_into(data);
        other.call = nullptr;
        other.actions = +[](dispatch_tag, memory&, memory*) noexcept {};
    }


    FunctionContainer& operator= (FunctionContainer&& other) 
    noexcept(cfg.SBO == 0 || cfg.require_nothrow_movable)
    requires (cfg.movable) 
    {
        reset();
        other.move_into(data);
        call = other.call;
        actions = other.actions;
        other.call = nullptr;
        other.actions = nullptr; //+[](dispatch_tag, memory&, memory*) noexcept {};
        return *this;
    }


    /// COPY (if copyable == true)
    FunctionContainer(FunctionContainer const& other) requires (cfg.copyable) 
    : call{other.call}
    , actions{other.actions} 
    {
        other.copy_into(data);
    }


    FunctionContainer& operator= (FunctionContainer const& other) requires (cfg.copyable) {
        reset();
        other.copy_into(data);
        call = other.call;
        actions = other.actions;
        return *this;
    }


    void swap(FunctionContainer & other) noexcept requires(cfg.movable) {
        if (&other == this) { return; }
        // std::swap(data, other.data); ///< Probably cannot use that...
        memory tmp;
        this->move_into(tmp);
        other.move_into(data);
        actions(dispatch_tag::Move, tmp, &other.data);

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


    operator bool() const noexcept {
        return call != nullptr; 
    }


    template <typename R2, typename... Args2, configuration::function cfg2>
    bool operator== (FunctionContainer<cfg2, R2(Args2...)> const &) const = delete;
    

    ~FunctionContainer() {
        if constexpr (!cfg.movable && !cfg.copyable) {
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
        actions(dispatch_tag::Move, data, &mem);
    }

    void copy_into(memory& mem) const {
        actions(dispatch_tag::Copy, const_cast<memory&>(data), &mem);
    }

private:
    memory data;
    invoke_f call = nullptr;
    action_f actions = nullptr;
};


template <typename Signature, configuration::function cfg = configuration::function{}>
class func;

// template <typename R, typename... Args, configuration::function cfg>
// class func<R(Args...), cfg> {
// public:
//     explicit func(invocable<R(Args...)> auto&& f) 
//     : func_{std::forward<decltype(f)>(f)} {}

//     R operator() (Args... args) {
//         return func_(args...);
//     }
// private:
//     FunctionContainer<cfg, R, Args...> func_;
// };

template <typename R, typename... Args, configuration::function cfg>
class func<R(Args...), cfg> : public FunctionContainer<cfg, R, Args...> {
    using FunctionContainer<cfg, R, Args...>::FunctionContainer;
};

template <typename R, typename... Args, configuration::function cfg>
class func<R(Args...) const, cfg> : public FunctionContainer<cfg.set_const_invocable(), R, Args...> {
    using FunctionContainer<cfg.set_const_invocable(), R, Args...>::FunctionContainer;
};

template <typename R, typename... Args, configuration::function cfg>
class func<R(Args...) noexcept, cfg> : public FunctionContainer<cfg.set_nothrow_invocable(), R, Args...> {
    using FunctionContainer<cfg.set_nothrow_invocable(), R, Args...>::FunctionContainer;
};

template <typename R, typename... Args, configuration::function cfg>
class func<R(Args...) const noexcept, cfg> : public FunctionContainer<cfg.set_const_invocable().set_nothrow_invocable(), R, Args...> {
    using FunctionContainer<cfg.set_const_invocable().set_nothrow_invocable(), R, Args...>::FunctionContainer;
};

} // namespace vx

#undef VX_UNREACHABLE
