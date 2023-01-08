#pragma once

#include "mish_core.h"

/* An x86 instruction's opcode is no longer than 3 bytes.
 */
typedef struct __attribute__((packed)) {
  uint8_t len;
  uint8_t op[3];
} opcode;
static_assert(sizeof(opcode) == 4, "opcode should be 4 bytes");

/* An x86 instruction is no longer than 15 bytes,
 * but the longest (potentially) structurally valid x86 instruction
 * is 26 bytes:
 *  4 byte legacy prefix
 *  1 byte prefix
 *  3 byte opcode
 *  1 byte ModR/M
 *  1 byte SIB
 *  8 byte displacement
 *  8 byte immediate
 *
 * We want to be able to "slide" around inside of a structurally valid
 * instruction in order to find errors, so we give ourselves enough space
 * here.
 */
typedef struct {
  uint8_t off;
  uint8_t len;
  uint8_t insn[26];
} insn_candidate;

/* Generate a single fuzzing candidate and populate the given input slot with it.
 * Returns false if the configured mutation mode has been exhausted.
 */
typedef bool (*mutator_t)(input_slot *);

mutator_t mutator_create(const char *name);
