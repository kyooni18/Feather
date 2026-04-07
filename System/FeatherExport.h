#ifndef FEATHER_EXPORT_H
#define FEATHER_EXPORT_H

#if defined(__cplusplus)
#define FEATHER_EXTERN_C_BEGIN extern "C" {
#define FEATHER_EXTERN_C_END }
#else
#define FEATHER_EXTERN_C_BEGIN
#define FEATHER_EXTERN_C_END
#endif

#if defined(FEATHER_STATIC_DEFINE)
#define FEATHER_API
#elif defined(_WIN32) || defined(__CYGWIN__)
#if defined(FEATHER_BUILDING_LIBRARY)
#define FEATHER_API __declspec(dllexport)
#else
#define FEATHER_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define FEATHER_API __attribute__((visibility("default")))
#else
#define FEATHER_API
#endif

#endif
