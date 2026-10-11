/* PCRE2's manual-build defaults, with target-based optional JIT support. */
#ifndef MSGSPEC_PCRE2_CONFIG_H
#define MSGSPEC_PCRE2_CONFIG_H

#include "config.h.generic"
#include "../deps/sljit/sljit_src/sljitConfigCPU.h"

/* Unknown targets and Alpha (no bundled SLJIT backend) use the interpreter. */
#if (!defined(SLJIT_CONFIG_UNSUPPORTED) || !SLJIT_CONFIG_UNSUPPORTED) && \
    (!defined(SLJIT_CONFIG_ALPHA_64) || !SLJIT_CONFIG_ALPHA_64)
#define SUPPORT_JIT 1
#endif

#endif
