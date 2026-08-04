/**
 * @file config.hpp
 * @brief Platform portability shims shared by the whole library.
 */
#ifndef RISERSIM_CONFIG_HPP
#define RISERSIM_CONFIG_HPP

// Ensure M_PI is available on all platforms (especially MSVC)
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include <cmath>

#ifndef M_PI
/** @brief Fallback definition of pi for compilers that don't provide `M_PI` (e.g. MSVC without `_USE_MATH_DEFINES`). */
#define M_PI 3.14159265358979323846
#endif

#endif // RISERSIM_CONFIG_HPP
