#pragma once

// Thin forward-declaration of the libturi C embedding API.
//
// The shipped public headers (turi/eval.h, turi/env.h) transitively include
// internal compiler/runtime headers (compiler/diag.h, runtime/arena.h, ...)
// that are not part of the prebuilt archive's include/ directory.  value.h is
// self-contained (only <stdint.h>, <stdbool.h>, <stdio.h>) and defines every
// type we need (TuriValue, TuriEnv, TuriNativeFn, ...), so we include it and
// forward-declare the C functions we call.  This keeps the build working
// without the internal headers while staying faithful to the real API
// signatures (copied verbatim from turi/eval.h and turi/env.h).

// turi/value.h is a C header that uses C99 compound literals and designated
// initializers, which clang flags as -Wc99-extensions / -Wc99-designator under
// C++ with -Werror.  Suppress those for the include only.  It must also be
// inside extern "C" so its non-static function declarations (turi_error,
// turi_val_strdup, ...) get C linkage matching the library.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc99-extensions"
#pragma clang diagnostic ignored "-Wc99-designator"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include "turi/value.h"

// TuriCaps is defined in turi/env.h as uint32_t with bit-flag constants.
// We re-declare the type and the flags we use here to avoid pulling in
// env.h's internal dependencies.
typedef uint32_t TuriCaps;
#define TURI_CAP_NONE  ((TuriCaps)0)
#define TURI_CAP_ALL   (~(TuriCaps)0)
#define TURI_CAP_IO       (1u << 0)
#define TURI_CAP_FFI      (1u << 1)
#define TURI_CAP_ASYNC    (1u << 3)
#define TURI_CAP_IMPORT   (1u << 5)
#define TURI_CAP_FS       (1u << 6)
#define TURI_CAP_PROC     (1u << 7)
#define TURI_CAP_ENV      (1u << 8)

// turi/eval.h
void turi_init(bool use_color);
TuriValue turi_eval(TuriEnv* env, const char* src);
TuriValue turi_eval_file(TuriEnv* env, const char* path);
TuriValue turi_eval_with_path(TuriEnv* env, const char* src, const char* path);
TuriValue turi_eval_typed(TuriEnv* env, const char* src,
                          char* out_type_tag, size_t tag_cap);
void turi_value_repr(char* buf, size_t cap, TuriValue v);
TuriValue turi_call(TuriEnv* env, TuriValue fn, TuriValue* args, uint32_t n_args);
void turi_run_event_loop(TuriEnv* env);
TuriValue turi_task_spawn(TuriEnv* env, const char* src);

void turi_env_register_native(TuriEnv* env, const char* name,
                               TuriNativeFn fn, void* ud);
void turi_env_register_native_caps(TuriEnv* env, const char* name,
                                   TuriNativeFn fn, void* ud, TuriCaps required);

// turi/env.h
TuriEnv* turi_env_new(void);
TuriEnv* turi_env_new_sandboxed(void);
void turi_env_free(TuriEnv* env);
void turi_env_reset(TuriEnv* env);
void turi_env_set_module_base_dir(TuriEnv* env, const char* path);
void turi_env_set_fuel(TuriEnv* env, uint64_t steps);
void turi_env_allow(TuriEnv* env, TuriCaps cap);
void turi_env_deny(TuriEnv* env, TuriCaps cap);
TuriValue turi_env_get(TuriEnv* env, const char* name);
void turi_env_set(TuriEnv* env, const char* name, TuriValue value);

#ifdef __cplusplus
}  // extern "C"
#endif

#ifdef __clang__
#pragma clang diagnostic pop
#endif
