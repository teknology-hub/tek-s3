//===-- utils.c - utility functions implementation ------------------------===//
//
// Copyright (c) 2025 Nuclearist <nuclearist@teknology-hub.com>
// Part of tek-s3, under the GNU General Public License v3.0 or later
// See https://github.com/teknology-hub/tek-s3/blob/main/COPYING for license
//    information.
// SPDX-License-Identifier: GPL-3.0-or-later
//
//===----------------------------------------------------------------------===//
///
/// @file
/// Implementation of utility functions declared in @ref utils.h.
///
//===----------------------------------------------------------------------===//
#include "utils.h"

#include <stdint.h>

//===-- Tables ------------------------------------------------------------===//

/// Mapping of ASCII characters to their respective Base64 indices.
__extension__ static const uint8_t ts3_base64_dec_table[256] = {
    [0 ... 42] = 64,   [43] = 62,        [44 ... 46] = 64, [47] = 63,
    [48] = 52,         [49] = 53,        [50] = 54,        [51] = 55,
    [52] = 56,         [53] = 57,        [54] = 58,        [55] = 59,
    [56] = 60,         [57] = 61,        [58 ... 64] = 64, [65] = 0,
    [66] = 1,          [67] = 2,         [68] = 3,         [69] = 4,
    [70] = 5,          [71] = 6,         [72] = 7,         [73] = 8,
    [74] = 9,          [75] = 10,        [76] = 11,        [77] = 12,
    [78] = 13,         [79] = 14,        [80] = 15,        [81] = 16,
    [82] = 17,         [83] = 18,        [84] = 19,        [85] = 20,
    [86] = 21,         [87] = 22,        [88] = 23,        [89] = 24,
    [90] = 25,         [91 ... 96] = 64, [97] = 26,        [98] = 27,
    [99] = 28,         [100] = 29,       [101] = 30,       [102] = 31,
    [103] = 32,        [104] = 33,       [105] = 34,       [106] = 35,
    [107] = 36,        [108] = 37,       [109] = 38,       [110] = 39,
    [111] = 40,        [112] = 41,       [113] = 42,       [114] = 43,
    [115] = 44,        [116] = 45,       [117] = 46,       [118] = 47,
    [119] = 48,        [120] = 49,       [121] = 50,       [122] = 51,
    [123 ... 255] = 64};

/// Mapping of Base64 indices to their respective ASCII characters.
[[gnu::nonstring]]
static const char ts3_base64_enc_table[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

//===-- Functions ---------------------------------------------------------===//

int ts3_u_base64_decode(const char *restrict input, int input_len,
                        unsigned char *restrict output) {
  // Cast input buffer pointer to unsigned so non-ASCII characters won't be
  //    destructive when used as indices
  auto const u_input = (const unsigned char *)input;
  /// Index of the next byte to be written to output
  register int output_index = 0;
  // Accumulates Base64 indices before producing output
  register uint32_t indices;
  // Tracks how many indices are currently present in indices
  register int state = 0;
  for (int i = 0; i < input_len; ++i) {
    const uint32_t index = ts3_base64_dec_table[u_input[i]];
    // Invalid Base64 characters get index 64 and indicate end of current
    //    string, resetting the state
    if (index == 64) {
      state = 0;
      continue;
    }
    switch (state) {
    case 0:
      indices = index << 18;
      // Bits [18,24) are filled, not enough data for output yet
      ++state;
      break;
    case 1:
      indices |= index << 12;
      // Bits [12,24) are filled, bits [16,24) are written to output
      output[output_index++] = (indices & 0xFF0000) >> 16;
      ++state;
      break;
    case 2:
      indices |= index << 6;
      // Bits [6,24) are filled, bits [8,16) are written to output
      output[output_index++] = (indices & 0xFF00) >> 8;
      ++state;
      break;
    case 3:
      indices |= index;
      // Bits [0,24) are filled, bits [0,8) are written to output
      output[output_index++] = indices & 0xFF;
      state = 0;
    } // switch (state)
  } // for (int i = 0; i < input_len; ++i)
  return output_index;
}

int ts3_u_base64_encode(const unsigned char *restrict input, int input_size,
                        char *restrict output) {
  auto const u_input = (const uint32_t *)input;
  auto u_output = (uint32_t *)output;
  const int num_uints = input_size / sizeof(uint32_t);
  // Stores data reordered such that it can be split into 6-bit indices
  register uint32_t indices = 0;
  // Iterate whole 4-byte chunks
  for (int i = 0; i < num_uints; ++i) {
    register const uint32_t in = u_input[i]; // A 4-byte block of input data
    // This remainder indicates how much data is left in indices after previous
    //    iteration
    switch (i % 3) {
      // Staging block of 4 ASCII characters that is fully computed before
      //    writing to output
      register uint32_t out;
    case 0:
      // indices is empty, get 3 bytes from in
      indices = (in << 16) | (in & 0xFF00) | ((in >> 16) & 0xFF);
      out = ts3_base64_enc_table[indices & 0b111'111] << 24;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111] << 16;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111] << 8;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111];
      *u_output++ = out;
      // Leave the remaining byte in indices
      indices = (in >> 8) & 0xFF0000;
      break;
    case 1:
      // indices contains 1 byte from previous iteration, get the other 2 bytes
      //    from in
      indices |= ((in & 0xFF) << 8) | ((in >> 8) & 0xFF);
      out = ts3_base64_enc_table[indices & 0b111'111] << 24;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111] << 16;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111] << 8;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111];
      *u_output++ = out;
      // Leave the remaining 2 bytes in indices
      indices = ((in >> 16) & 0xFF00) | (in & 0xFF0000);
      break;
    case 2:
      // indices contains 2 bytes from previous iteration, get the last byte
      //    from in
      indices |= (in & 0xFF);
      out = ts3_base64_enc_table[indices & 0b111'111] << 24;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111] << 16;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111] << 8;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111];
      *u_output++ = out;
      // Flush remaining 3 bytes from in
      indices = (in >> 24) | ((in >> 8) & 0xFF00) | ((in << 8) & 0xFF0000);
      out = ts3_base64_enc_table[indices & 0b111'111] << 24;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111] << 16;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111] << 8;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111];
      *u_output++ = out;
    } // switch (i % 3)
  } // for (int i = 0; i < num_uints; ++i)
  // Flush remaining input if any
  const int rem = input_size % sizeof(uint32_t);
  if (rem) {
    register uint32_t in;
    // Read remaining bytes
    switch (rem) {
    case 1:
      in = input[num_uints * sizeof(uint32_t)];
      break;
    case 2:
      in = ((const uint16_t *)
                input)[num_uints * sizeof(uint32_t) / sizeof(uint16_t)];
      break;
    case 3:
      in = ((const uint16_t *)
                input)[num_uints * sizeof(uint32_t) / sizeof(uint16_t)] |
           (input[input_size - 1] << 16);
    }
    // Still check the remainder as some indices may be left from last iteration
    switch (num_uints % 3) {
      register uint32_t out;
    case 0:
      // indices is empty, get all 3 bytes from in
      indices = (in << 16) | (in & 0xFF00) | ((in >> 16) & 0xFF);
      // Set padding character if less than 3 bytes are available
      out = (rem == 3 ? ts3_base64_enc_table[indices & 0b111'111] : '=') << 24;
      indices >>= 6;
      // Set padding character if only 1 byte is available
      out |= (rem == 1 ? '=' : ts3_base64_enc_table[indices & 0b111'111]) << 16;
      indices >>= 6;
      // At least one byte for remaining 2 characters is guaranteed to be
      //    available by if statement
      out |= ts3_base64_enc_table[indices & 0b111'111] << 8;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111];
      *u_output++ = out;
      break;
    case 1:
      // indices contains 1 byte from last iteration, get the other 2 bytes
      //    from in
      indices |= ((in & 0xFF) << 8) | ((in >> 8) & 0xFF);
      // Set padding character if only 1 byte was available from in (resulting
      //    in 2 total)
      out = (rem == 1 ? '=' : ts3_base64_enc_table[indices & 0b111'111]) << 24;
      indices >>= 6;
      // The other 2 bytes are guaranteed to be available
      out |= ts3_base64_enc_table[indices & 0b111'111] << 16;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111] << 8;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111];
      *u_output++ = out;
      // If in has all 3 bytes, one remains to be flushed
      if (rem == 3) {
        indices = (in & 0xFF0000) >> 12;
        // 0x3D3D0000 provides "==" padding at the end
        out = 0x3D3D0000 | (ts3_base64_enc_table[indices & 0b111'111] << 8);
        indices >>= 6;
        out |= ts3_base64_enc_table[indices & 0b111'111];
        *u_output++ = out;
      }
      break;
    case 2:
      // indices contains 2 bytes from last iteration, get the last byte from in
      indices |= (in & 0xFF);
      out = ts3_base64_enc_table[indices & 0b111'111] << 24;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111] << 16;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111] << 8;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111];
      *u_output++ = out;
      // If in has more than 1 byte, flush the remaining one(s)
      if (rem > 1) {
        indices = (((in >> 8) & 0xFF00) | ((in << 8) & 0xFF0000)) >> 6;
        // 0x3D000000 provides "=" padding at the end
        out = 0x3D000000 |
              ((rem == 2 ? '=' : ts3_base64_enc_table[indices & 0b111'111])
               << 16);
        indices >>= 6;
        out |= ts3_base64_enc_table[indices & 0b111'111] << 8;
        indices >>= 6;
        out |= ts3_base64_enc_table[indices & 0b111'111];
        *u_output++ = out;
      }
    } // switch (num_uints % 3)
  } else { // if (rem)
    // No input data remaining, but indices may have data left over from last
    //    iteration
    switch (num_uints % 3) {
      register uint32_t out;
    case 1:
      out = 0x3D3D0000; // "==" padding at the end
      indices >>= 12;
      out |= ts3_base64_enc_table[indices & 0b111'111] << 8;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111];
      *u_output++ = out;
      break;
    case 2:
      out = 0x3D000000; // "=" padding at the end
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111] << 16;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111] << 8;
      indices >>= 6;
      out |= ts3_base64_enc_table[indices & 0b111'111];
      *u_output++ = out;
    }
  } // if (rem) else
  return (char *)u_output - output;
}
