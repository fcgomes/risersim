/**
 * @file config.hpp
 * @brief Platform portability shims shared by the whole library.
 *
 * Historically this provided an `M_PI` fallback for MSVC (which only defines it under
 * `_USE_MATH_DEFINES`), while GCC/Clang expose it as a non-standard `<cmath>` extension. That
 * whole macro dance is obsolete under C++20: `std::numbers::pi` (`<numbers>`) is a portable,
 * `constexpr double` available on every standard-conforming compiler. Files that need pi should
 * include this header and use `std::numbers::pi` directly.
 */
#ifndef RISERSIM_CONFIG_HPP
#define RISERSIM_CONFIG_HPP

#include <cmath>
#include <numbers>

#endif // RISERSIM_CONFIG_HPP
