# Read the canonical factory.rea60config and emit a C++ header that
# exposes its contents as a byte array + size. Re-runs when the resource
# file changes so a fresh export from Frank propagates to the next build
# without manual steps.
#
# Why a byte array instead of a raw string literal: MSVC truncates
# string literals beyond 16380 characters (C2026), and the bundle is
# ~47 KB. A byte array bypasses the limit entirely and works the same
# on Clang / GCC / MSVC.

if(NOT DEFINED SRCFILE OR NOT DEFINED OUTFILE)
    message(FATAL_ERROR "embed_factory_bundle.cmake: SRCFILE and OUTFILE required")
endif()

if(NOT EXISTS ${SRCFILE})
    message(FATAL_ERROR "embed_factory_bundle.cmake: SRCFILE not found: ${SRCFILE}")
endif()

file(READ ${SRCFILE} HEX_CONTENT HEX)
string(LENGTH "${HEX_CONTENT}" HEX_LEN)
math(EXPR BYTE_COUNT "${HEX_LEN} / 2")

# Emit 16 bytes per line for readable diffs.
# ⇨ LINEAR, NOT A BYTE LOOP. The loop this replaces appended one byte at a time
# to a growing string, which CMake copies on every set(): quadratic in the file
# size. With the user manual (321 KB) embedded that one step ran for well over
# ten minutes (2026-11-02). Two regex passes give the same array: first a line
# break after every 16 bytes (32 hex characters), then "0x.., " for every byte.
string(REGEX REPLACE "([0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f])"
       "\\1\n" LINED "${HEX_CONTENT}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " BYTES "${LINED}")
string(REPLACE ", \n" ",\n    " BYTES "${BYTES}")
# No trailing ", " or indentation after the last byte.
string(REGEX REPLACE "[, \n]+$" "" BYTES "${BYTES}")

# Symbol name override for reuse (Windows WinUSB INF uses the same
# helper). Defaults to the factory bundle's name when not overridden.
if(NOT SYMBOL)
    set(SYMBOL "kFactoryBundle")
endif()

set(NEW_CONTENT
"#pragma once
// Auto-generated from ${SRCFILE} — do not edit.
#include <cstddef>
namespace uf8 {
namespace setup_bundle {
inline constexpr unsigned char ${SYMBOL}Bytes[] = {
    ${BYTES}
};
inline constexpr size_t ${SYMBOL}Size = ${BYTE_COUNT};
}
}
")

if(EXISTS ${OUTFILE})
    file(READ ${OUTFILE} OLD_CONTENT)
    if(OLD_CONTENT STREQUAL NEW_CONTENT)
        return()
    endif()
endif()

file(WRITE ${OUTFILE} "${NEW_CONTENT}")
