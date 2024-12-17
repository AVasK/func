#include <iostream>
#include <new>
#include <cassert>
#include <sstream>
#include <cstddef> // sized ints
#include <functional>
#include "func.hpp"

using u8 = std::uint8_t;

struct Stats {
    std::string _log {};

    void log(std::string line) {
        _log += (std::move(line) + '\n');
    }

    void track_heap([[maybe_unused]] void * mem, [[maybe_unused]] std::size_t bytes) {
        // noop for now
    }

    void untrack_heap([[maybe_unused]] void * mem) {}
};

static Stats stat;

std::string address_to_str(auto * p) {
    std::stringstream s;
    s << (const void *)p;
    return s.str();
}


template <std::size_t Sz=0, bool log_allocations=false, typename R=void>
struct Test {
    const char * _name;
    [[no_unique_address]] std::byte store[Sz];
    u8 _version = 0;
    u8 _moves = 0;
    bool _moved = false;

    std::string name() const {
        std::string s {};
        if (_moves <= 3) {
            for (u8 i=0; i < _moves; ++i) { s += '.'; }
        } else {
            s += std::to_string(_moves) + '.';
        }
        s += std::string(_name) + (_version == 0 ? std::string("") : std::to_string(_version));
        return s;
    }

    void* operator new(std::size_t bytes) {
        void* mem = ::operator new(bytes);
        if constexpr (log_allocations) {
            stat.log("(+) [heap] new " + std::to_string(bytes) + " bytes @ " + address_to_str(mem));
            stat.track_heap(mem, bytes);
        }
        return mem;
    }

    void* operator new(std::size_t count, void * mem) noexcept {
        if constexpr (log_allocations) {
            stat.log("(^) [placement] new " + std::to_string(count) + " bytes @" + address_to_str(mem));
        }
        return ::operator new(count, mem);
    }
 
    void operator delete(void* mem) {
        if constexpr (log_allocations) {
            stat.log("(-) delete @ " + address_to_str(mem));
            stat.untrack_heap(mem);
        }
        ::operator delete(mem);
    }

    Test(const char * const nm) 
    : _name{nm}
    {
        stat.log("ctor {" + name() + "}");
    }

    Test(Test && other) noexcept
    : _name{other._name} 
    , _moves(other._moves + 1)
    {
        other._moved = true;
        stat.log("move ctor {" + other.name() + "} => {" + name() + "}");
    }

    Test(Test const& other) 
    : _name{other._name}
    , _version(other._version + 1)
    {
        stat.log("copy ctor {" + other.name() + "} => {" + name() + "}");
    }

    Test& operator= (Test&& other) noexcept {
        _name = other._name;
        _version = other._moves + 1;
        other._moved = true;
        stat.log("move assn (" + other.name() + ") -> (" + name() + ")");
        return *this;
    }

    Test& operator= (Test const& other) {
        _name = other._name;
        _version = other._version + 1;
        stat.log("copy assn (" + other.name() + ") -> (" + name() + ")");
        return *this;
    }

    ~Test() {
        if (!_moved) { 
            stat.log("~{" + name() + "}"); 
        }
    }

    auto operator()() {
        if (_moved) {
            stat.log("!" + name() + "() called on moved object");
        } else {
            stat.log(name() + "() called");
        }
        if constexpr (std::is_void_v<R>) {
            return;
        } else {
            return R{};
        }
    }    
};


template <typename func, std::size_t Sz=0>
constexpr auto test1() { 
    {
        std::cerr << "sizeof(Test<Sz>): " << sizeof(Test<Sz>) << "\n";
        func f0 = Test<Sz>{"X"}; // ctor {X}, move ctor {X} => {.X}
        func f1 = Test<Sz>{"A"}; // ctor {A}, move ctor {A} => {.A}
        func f2 = f1; // copy ctor {.A} => {A1}
        f0 = std::move(f2); // ~{.X}, move assn {moved A+} -> (moved moved A+)
        // f0 = f1;
        f0();
        f1();
    }
    const auto log = std::move(stat._log);
    stat._log.clear();   
    return log;
};

template <typename func, std::size_t Sz=0>
constexpr auto test2() { 
    {
        func f0 = Test<Sz>{"X"}; // ctor {X}, move ctor {X} => {.X}
        f0 = Test<Sz>{"Y"};
        f0();
    }
    const auto log = std::move(stat._log);
    stat._log.clear();   
    return log;
};

template <typename F, std::size_t Sz=100>
constexpr auto test3() {
    {
        F fX = Test<Sz>{"X"}; // ctor {X}, move ctor {X} => {.X}
        F fA = Test<Sz>{"A"}; // ctor {A}, move ctor {A} => {.A}
        fX.swap(fA); 
    
        fX(); // -> A
        fA(); // -> X
    }
    const auto log = std::move(stat._log);
    stat._log.clear();
    return log;
}


int main() {
    constexpr vx::cfg::function cfg {
        .SBO = 16,
        .require_nothrow_movable = true,
        .check_empty = true,
        // .allow_heap = false,
        .copyable = true,
        .movable = true
    };

    {
        const auto my_log = test1<vx::func<void(), cfg>, 100>();
        const auto std_log = test1<std::function<void()>, 100>();
        std::cerr << my_log << "\n\n" << std_log;
        assert(my_log == std_log);
    }

    {
        const auto my_log = test2<vx::func<void(), cfg>, 100>();
        const auto std_log = test2<std::function<void()>, 100>();
        std::cerr << "my:\n" << my_log << "\n\nstd:\n" << std_log;
        // assert(my_log == std_log);
    }

    constexpr auto cfg2 = [cfg]{ 
        auto c = cfg; 
        // c.allow_heap=false;
        c.SBO = 0;
        c.copyable = false;
        return c; 
    }();

    assert((test3<vx::func<void(), cfg2>>() == test3<std::function<void()>>()));

    /// non-copyable inplace func
    {
        constexpr vx::cfg::function inplace_cfg = {
            .can_be_empty = false,
            .check_empty = false,
            .copyable = false,
            .movable = false
        };

        static_assert( not std::is_default_constructible_v<vx::func<int() const, inplace_cfg>>);
        static_assert( not std::is_copy_constructible_v<vx::func<int() const, inplace_cfg>>);
        static_assert( not std::is_move_constructible_v<vx::func<int() const, inplace_cfg>>);
        static_assert( not std::is_copy_assignable_v<vx::func<int() const, inplace_cfg>>); // const vx::func<int() const, inplace_cfg> f2 = f; // Error: no copy ctor
        static_assert( not std::is_move_assignable_v<vx::func<int() const, inplace_cfg>>);

        const vx::func<int() const, inplace_cfg> f = []{ return 42; };
        assert(f() == 42);
    }

    /// move-only func
    {
        constexpr vx::cfg::function move_only_cfg = {
            .can_be_empty = false,
            .check_empty = false,
            .copyable = false,
            .movable = true
        };

        static_assert( not std::is_default_constructible_v<vx::func<int() const, move_only_cfg>>);
        static_assert( not std::is_copy_constructible_v<vx::func<int() const, move_only_cfg>>);
        static_assert( std::is_move_constructible_v<vx::func<int() const, move_only_cfg>>);
        static_assert( not std::is_copy_assignable_v<vx::func<int() const, move_only_cfg>>);
        static_assert( std::is_move_assignable_v<vx::func<int() const, move_only_cfg>>);

        vx::func<int() const, move_only_cfg> f = []{ return 42; };
        const vx::func<int() const, move_only_cfg> f2 = std::move(f);
        assert(f2() == 42);
    }

    /// copy- and move- enabled
    {
        constexpr vx::cfg::function copy_and_move_cfg = {
            .can_be_empty = false,
            .check_empty = false,
            .copyable = true,
            .movable = true
        };

        static_assert( not std::is_default_constructible_v<vx::func<int() const, copy_and_move_cfg>>);
        static_assert( std::is_copy_constructible_v<vx::func<int() const, copy_and_move_cfg>>);
        static_assert( std::is_move_constructible_v<vx::func<int() const, copy_and_move_cfg>>);
        static_assert( std::is_copy_assignable_v<vx::func<int() const, copy_and_move_cfg>>);
        static_assert( std::is_move_assignable_v<vx::func<int() const, copy_and_move_cfg>>);

        vx::func<int() const, copy_and_move_cfg> f = []{ return 42; };
        const vx::func<int() const, copy_and_move_cfg> f2 = std::move(f);
        assert(f2() == 42);
    }
    
    /// default-constructible 
    {
        constexpr vx::cfg::function empty_cfg = {
            .can_be_empty = true,
            .check_empty = true
        };

        static_assert( std::is_default_constructible_v<vx::func<int() const, empty_cfg>>);
        static_assert( not std::is_copy_constructible_v<vx::func<int() const, empty_cfg>>);
        static_assert( std::is_move_constructible_v<vx::func<int() const, empty_cfg>>);
        static_assert( not std::is_copy_assignable_v<vx::func<int() const, empty_cfg>>);
        static_assert( std::is_move_assignable_v<vx::func<int() const, empty_cfg>>);

        vx::func<int() const, empty_cfg> f;
        try {
            f(); 
            assert(false);
        } catch (vx::bad_function_call) {
            assert(true);
        }
    }
}
