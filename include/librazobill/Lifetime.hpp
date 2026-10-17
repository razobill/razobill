// =============================================================================
//  file : include/librazobill/Lifetime.hpp
//  role : compile-time lifetime safety attributes and contracts
//  inv  : zero-overhead annotations; portable fallback across all compilers
// =============================================================================

#pragma once

// -- compiler feature detection for C++ standard & vendor attributes ---------

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#define RZ_LIFETIMEBOUND [[clang::lifetimebound]]
#else
#define RZ_LIFETIMEBOUND
#endif

#if __has_cpp_attribute(gsl::Pointer)
#define RZ_GSL_POINTER [[gsl::Pointer]]
#else
#define RZ_GSL_POINTER
#endif

#if __has_cpp_attribute(gsl::Owner)
#define RZ_GSL_OWNER [[gsl::Owner]]
#else
#define RZ_GSL_OWNER
#endif

#if __has_cpp_attribute(clang::noescape)
#define RZ_NOESCAPE [[clang::noescape]]
#else
#define RZ_NOESCAPE
#endif
#else
#define RZ_LIFETIMEBOUND
#define RZ_GSL_POINTER
#define RZ_GSL_OWNER
#define RZ_NOESCAPE
#endif
