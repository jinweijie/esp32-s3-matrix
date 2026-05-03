#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Decode next UTF-8 codepoint; advances *s past the consumed bytes.
 * Returns bytes consumed, or 0 on invalid input (caller may skip 1 byte).
 */
int utf8_decode(const char **s, uint32_t *cp_out);

/** Copy 8 scan rows (MSB = left pixel) if `cp` is in the embedded subset; `rows` may be NULL to probe only. */
bool cjk8_lookup(uint32_t cp, uint8_t *rows);
