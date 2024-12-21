# Yet another function (this time with lots of knobs to turn), inspired by [this great article by Arthur O'Dwyer](https://quuxplusone.github.io/blog/2019/03/27/design-space-for-std-function/)

[C++20] 

Uses a configuration aggregate to set all the knobs
```C++
vx::configuration::function {
    std::size_t SBO { 32 };
    std::size_t alignment { alignof(std::max_align_t) };
    bool allow_return_type_conversion { true };
    bool require_nothrow_invocable { false };
    bool require_const_invocable { false };
    bool require_nothrow_movable { true };
    bool enable_typeinfo { false };
    bool can_be_empty { false };
    bool check_empty { false };
    bool allow_heap { true };
    bool copyable { false };
    bool movable { true };
}
```

So, it can be turned into a inplace function by turning a few knobs, as well as move-only function and std::function-like one. 
Preserves const-ness and noexcept
