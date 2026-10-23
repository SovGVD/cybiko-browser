/* Raw Deflate (RFC 1951) for the Browser application's pages. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Packs in[0..n) into out; its length, or 0 if it did not fit in cap. */
size_t cy_deflate(const uint8_t *in, size_t n, uint8_t *out, size_t cap);
