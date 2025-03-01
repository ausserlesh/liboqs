/*
 *  This file is part of the optimized implementation of the Picnic signature scheme.
 *  See the accompanying documentation for complete details.
 *
 *  The code is provided under the MIT license, see LICENSE for
 *  more details.
 *  SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "bitstream.h"
#include "compat.h"
#include "io.h"
#include "kdf_shake.h"
#include "lowmc.h"
#include "lowmc_pars.h"
#include "mpc_lowmc.h"
#include "picnic_impl.h"
#include <oqs/rand.h>

#include <limits.h>
#include <math.h>
#include <stdlib.h>

typedef struct {
  uint8_t* seeds[SC_PROOF];
  uint8_t* commitments[SC_PROOF];
  uint8_t* gs[SC_PROOF];
  uint8_t* input_shares[SC_PROOF];
  uint8_t* communicated_bits[SC_PROOF];
  uint8_t* output_shares[SC_PROOF];
} proof_round_t;

typedef struct {
  uint8_t* challenge;
  uint8_t* salt;
  proof_round_t round[];
} sig_proof_t;

typedef struct {
  proof_round_t* round;
  unsigned int round_number;
} sorting_helper_t;

// Prefix values for domain separation
const uint8_t HASH_PREFIX_0 = 0;
const uint8_t HASH_PREFIX_1 = 1;
const uint8_t HASH_PREFIX_2 = 2;
const uint8_t HASH_PREFIX_3 = 3;
const uint8_t HASH_PREFIX_4 = 4;
const uint8_t HASH_PREFIX_5 = 5;

#define LOWMC_UNSPECFIED_ARG UINT32_MAX

/**
 * Collapse challenge from one char per challenge to bit array.
 */
static void collapse_challenge(uint8_t* collapsed, const picnic_instance_t* pp,
                               const uint8_t* challenge) {
  bitstream_t bs;
  bs.buffer.w = collapsed;
  bs.position = 0;

  for (unsigned int i = 0; i < pp->num_rounds; ++i) {
    // flip challenge bits according to spec
    bitstream_put_bits_8(&bs, (challenge[i] >> 1) | ((challenge[i] & 1) << 1), 2);
  }
}

/**
 * Expand challenge from bit array to one char per challenge.
 */
static bool expand_challenge(uint8_t* challenge, const picnic_instance_t* pp,
                             const uint8_t* collapsed) {
  bitstream_t bs;
  bs.buffer.r = collapsed;
  bs.position = 0;

  for (unsigned int i = 0; i < pp->num_rounds; ++i) {
    const uint8_t ch = bitstream_get_bits_8(&bs, 2);
    if (ch == 3) {
      return false;
    }
    // flip challenge bits according to spec
    challenge[i] = (ch & 1) << 1 | (ch >> 1);
  }

  const size_t remaining_bits = (pp->collapsed_challenge_size << 3) - bs.position;
  if (remaining_bits && bitstream_get_bits(&bs, remaining_bits)) {
    return false;
  }

  return true;
}

#define ALIGNT(s, t) (((s) + sizeof(t) - 1) & ~(sizeof(t) - 1))
#define ALIGNU64T(s) ALIGNT(s, uint64_t)

static sig_proof_t* proof_new(const picnic_instance_t* pp) {
  const size_t digest_size                    = pp->digest_size;
  const size_t seed_size                      = pp->seed_size;
  const size_t num_rounds                     = pp->num_rounds;
  const size_t input_size                     = pp->input_size;
  const size_t output_size                    = pp->output_size;
  const size_t view_size                      = ALIGNU64T(pp->view_size);
  const size_t unruh_with_input_bytes_size    = pp->unruh_with_input_bytes_size;
  const size_t unruh_without_input_bytes_size = pp->unruh_without_input_bytes_size;

  sig_proof_t* prf = calloc(1, sizeof(sig_proof_t) + num_rounds * sizeof(proof_round_t));

  size_t per_round_mem =
      SC_PROOF * (seed_size + digest_size + input_size + output_size + view_size);
  if (pp->transform == TRANSFORM_UR) {
    per_round_mem += (SC_PROOF - 1) * unruh_without_input_bytes_size + unruh_with_input_bytes_size;
  }

  // in memory:
  // - challenge (aligned to uint64_t)
  // - seeds
  // - salt
  // - commitments
  // - input shares
  // - communicated bits (aligned to uint64_t)
  // - output shares
  // - Gs
  //
  // Since seeds size, commitment size, input share size and output share size are all divisible by
  // the alignment of uint64_t, this means, that up to the memory of the Gs, everything is
  // uint64_t-aligned.
  uint8_t* slab  = calloc(1, num_rounds * per_round_mem + ALIGNU64T(num_rounds) + SALT_SIZE);
  prf->challenge = slab;
  slab += ALIGNU64T(num_rounds);

  for (uint32_t r = 0; r < num_rounds; ++r) {
    for (uint32_t i = 0; i < SC_PROOF; ++i) {
      prf->round[r].seeds[i] = slab;
      slab += seed_size;
    }
  }
  prf->salt = slab;
  slab += SALT_SIZE;

  for (uint32_t r = 0; r < num_rounds; ++r) {
    for (uint32_t i = 0; i < SC_PROOF; ++i) {
      prf->round[r].commitments[i] = slab;
      slab += digest_size;
    }
  }

  for (uint32_t r = 0; r < num_rounds; ++r) {
    for (uint32_t i = 0; i < SC_PROOF; ++i) {
      prf->round[r].input_shares[i] = slab;
      slab += input_size;
    }
  }

  for (uint32_t r = 0; r < num_rounds; ++r) {
    for (uint32_t i = 0; i < SC_PROOF; ++i) {
      prf->round[r].communicated_bits[i] = slab;
      slab += view_size;
    }
  }

  for (uint32_t r = 0; r < num_rounds; ++r) {
    for (uint32_t i = 0; i < SC_PROOF; ++i) {
      prf->round[r].output_shares[i] = slab;
      slab += output_size;
    }
  }

  if (pp->transform == TRANSFORM_UR) {
    for (uint32_t r = 0; r < num_rounds; ++r) {
      for (uint32_t i = 0; i < SC_PROOF - 1; ++i) {
        prf->round[r].gs[i] = slab;
        slab += unruh_without_input_bytes_size;
      }
      prf->round[r].gs[SC_PROOF - 1] = slab;
      slab += unruh_with_input_bytes_size;
    }
  }

  return prf;
}

static sig_proof_t* proof_new_verify(const picnic_instance_t* pp, uint8_t** rslab) {
  const size_t digest_size                 = pp->digest_size;
  const size_t num_rounds                  = pp->num_rounds;
  const size_t input_size                  = pp->input_size;
  const size_t output_size                 = pp->output_size;
  const size_t view_size                   = ALIGNU64T(pp->view_size);
  const size_t unruh_with_input_bytes_size = pp->unruh_with_input_bytes_size;

  sig_proof_t* proof = calloc(1, sizeof(sig_proof_t) + num_rounds * sizeof(proof_round_t));

  size_t per_round_mem = SC_VERIFY * digest_size;
  if (pp->transform == TRANSFORM_UR) {
    // we don't know what we actually need, so allocate more than needed
    per_round_mem += SC_VERIFY * pp->unruh_with_input_bytes_size;
  }
  per_round_mem += SC_VERIFY * input_size + SC_PROOF * output_size + view_size;

  uint8_t* slab    = calloc(1, num_rounds * per_round_mem + ALIGNU64T(num_rounds) + SALT_SIZE);
  proof->challenge = slab;
  slab += ALIGNU64T(num_rounds);

  proof->salt = slab;
  slab += SALT_SIZE;

  for (uint32_t r = 0; r < num_rounds; ++r) {
    for (uint32_t i = 0; i < SC_VERIFY; ++i) {
      proof->round[r].commitments[i] = slab;
      slab += digest_size;
    }
  }

  for (uint32_t r = 0; r < num_rounds; ++r) {
    proof->round[r].communicated_bits[0] = slab;
    slab += view_size;
  }

  for (uint32_t r = 0; r < num_rounds; ++r) {
    proof->round[r].output_shares[0] = slab;
    slab += output_size;
    proof->round[r].output_shares[1] = slab;
    slab += output_size;
    proof->round[r].output_shares[2] = slab;
    slab += output_size;
  }

  if (pp->transform == TRANSFORM_UR) {
    for (uint32_t r = 0; r < num_rounds; ++r) {
      for (uint32_t i = 0; i < SC_VERIFY; ++i) {
        proof->round[r].gs[i] = slab;
        slab += unruh_with_input_bytes_size;
      }
    }
  }

  *rslab = slab;
  return proof;
}

static void proof_free(sig_proof_t* prf) {
  free(prf->challenge);
  free(prf);
}

static void kdf_shake_update_key_intLE(kdf_shake_t* kdf, uint16_t x) {
  const uint16_t x_le = htole16(x);
  kdf_shake_update_key(kdf, (const uint8_t*)&x_le, sizeof(x_le));
}

static void kdf_shake_x4_update_key_intLE(kdf_shake_x4_t* kdf, uint16_t x) {
  const uint16_t x_le   = htole16(x);
  const uint8_t* ptr[4] = {(const uint8_t*)&x_le, (const uint8_t*)&x_le, (const uint8_t*)&x_le,
                           (const uint8_t*)&x_le};
  kdf_shake_x4_update_key(kdf, ptr, sizeof(x_le));
}

static void kdf_shake_x4_update_key_intLE_round(kdf_shake_x4_t* kdf, const uint16_t x[4]) {

  const uint16_t x0_le  = htole16(x[0]);
  const uint16_t x1_le  = htole16(x[1]);
  const uint16_t x2_le  = htole16(x[2]);
  const uint16_t x3_le  = htole16(x[3]);
  const uint8_t* ptr[4] = {(const uint8_t*)&x0_le, (const uint8_t*)&x1_le, (const uint8_t*)&x2_le,
                           (const uint8_t*)&x3_le};
  kdf_shake_x4_update_key(kdf, ptr, sizeof(x0_le));
}

static void kdf_init_from_seed(kdf_shake_t* kdf, const uint8_t* seed, const uint8_t* salt,
                               uint16_t round_number, uint16_t player_number,
                               bool include_input_size, const picnic_instance_t* pp) {
  // Hash the seed with H_2.
  kdf_shake_init_prefix(kdf, pp, HASH_PREFIX_2);
  kdf_shake_update_key(kdf, seed, pp->seed_size);
  kdf_shake_finalize_key(kdf);

  uint8_t tmp[MAX_DIGEST_SIZE];
  kdf_shake_get_randomness(kdf, tmp, pp->digest_size);
  kdf_shake_clear(kdf);

  // Initialize KDF with H_2(seed) || salt || round_number || player_number || output_size.
  kdf_shake_init(kdf, pp);
  kdf_shake_update_key(kdf, tmp, pp->digest_size);
  kdf_shake_update_key(kdf, salt, SALT_SIZE);
  kdf_shake_update_key_intLE(kdf, round_number);
  kdf_shake_update_key_intLE(kdf, player_number);
  kdf_shake_update_key_intLE(kdf, pp->view_size + (include_input_size ? pp->input_size : 0));
  kdf_shake_finalize_key(kdf);
}

static void kdf_init_x4_from_seed(kdf_shake_x4_t* kdf, const uint8_t** seed, const uint8_t* salt,
                                  const uint16_t round_number[4], const uint16_t player_number,
                                  bool include_input_size, const picnic_instance_t* pp) {
  // Hash the seed with H_2.
  kdf_shake_x4_init_prefix(kdf, pp, HASH_PREFIX_2);
  kdf_shake_x4_update_key(kdf, seed, pp->seed_size);
  kdf_shake_x4_finalize_key(kdf);

  uint8_t tmp[4][MAX_DIGEST_SIZE];
  uint8_t* tmpptr[4]             = {tmp[0], tmp[1], tmp[2], tmp[3]};
  const uint8_t* tmpptr_const[4] = {tmp[0], tmp[1], tmp[2], tmp[3]};
  kdf_shake_x4_get_randomness(kdf, tmpptr, pp->digest_size);
  kdf_shake_x4_clear(kdf);

  // Initialize KDF with H_2(seed) || salt || round_number || player_number || output_size.
  kdf_shake_x4_init(kdf, pp);
  kdf_shake_x4_update_key(kdf, tmpptr_const, pp->digest_size);
  const uint8_t* saltptr[4] = {salt, salt, salt, salt};
  kdf_shake_x4_update_key(kdf, saltptr, SALT_SIZE);
  kdf_shake_x4_update_key_intLE_round(kdf, round_number);
  kdf_shake_x4_update_key_intLE(kdf, player_number);
  kdf_shake_x4_update_key_intLE(kdf, pp->view_size + (include_input_size ? pp->input_size : 0));
  kdf_shake_x4_finalize_key(kdf);
}

static void uint64_to_bitstream_10(bitstream_t* bs, const uint64_t v) {
  bitstream_put_bits_32(bs, v >> (64 - 30), 30);
}

static uint64_t uint64_from_bitstream_10(bitstream_t* bs) {
  return ((uint64_t)bitstream_get_bits_32(bs, 30)) << (64 - 30);
}

static void uint64_to_bitstream_1(bitstream_t* bs, const uint64_t v) {
  bitstream_put_bits_8(bs, v >> (64 - 3), 3);
}

static uint64_t uint64_from_bitstream_1(bitstream_t* bs) {
  return ((uint64_t)bitstream_get_bits_8(bs, 3)) << (64 - 3);
}

static void compress_view(uint8_t* dst, const picnic_instance_t* pp, const view_t* views,
                          const unsigned int idx) {
  const size_t num_views = pp->lowmc->r;

  bitstream_t bs;
  bs.buffer.w = dst;
  bs.position = 0;

  const view_t* v = &views[0];
  if (pp->lowmc->m == 10) {
    for (size_t i = 0; i < num_views; ++i, ++v) {
      uint64_to_bitstream_10(&bs, v->t[idx]);
    }
  } else if (pp->lowmc->m == 1) {
    for (size_t i = 0; i < num_views; ++i, ++v) {
      uint64_to_bitstream_1(&bs, v->t[idx]);
    }
  }
}

static void decompress_view(view_t* views, const picnic_instance_t* pp, const uint8_t* src,
                            const unsigned int idx) {
  const size_t num_views = pp->lowmc->r;

  bitstream_t bs;
  bs.buffer.r = src;
  bs.position = 0;

  view_t* v = &views[0];
  if (pp->lowmc->m == 10) {
    for (size_t i = 0; i < num_views; ++i, ++v) {
      v->t[idx] = uint64_from_bitstream_10(&bs);
    }
  } else if (pp->lowmc->m == 1) {
    for (size_t i = 0; i < num_views; ++i, ++v) {
      v->t[idx] = uint64_from_bitstream_1(&bs);
    }
  }
}

static void decompress_random_tape(rvec_t* rvec, const picnic_instance_t* pp, const uint8_t* src,
                                   const unsigned int idx) {
  const size_t num_views = pp->lowmc->r;

  bitstream_t bs;
  bs.buffer.r = src;
  bs.position = 0;

  rvec_t* rv = &rvec[0];

  if (pp->lowmc->m == 10) {
    for (size_t i = 0; i < num_views; ++i, ++rv) {
      rv->t[idx] = uint64_from_bitstream_10(&bs);
    }
  } else if (pp->lowmc->m == 1) {
    for (size_t i = 0; i < num_views; ++i, ++rv) {
      rv->t[idx] = uint64_from_bitstream_1(&bs);
    }
  }
}

/**
 * Compute commitment to a view.
 */
static void hash_commitment(const picnic_instance_t* pp, proof_round_t* prf_round,
                            const unsigned int vidx) {
  const size_t hashlen = pp->digest_size;

  hash_context ctx;
  // hash the seed
  hash_init_prefix(&ctx, pp, HASH_PREFIX_4);
  hash_update(&ctx, prf_round->seeds[vidx], pp->seed_size);
  hash_final(&ctx);
  uint8_t tmp[MAX_DIGEST_SIZE];
  hash_squeeze(&ctx, tmp, hashlen);

  // compute H_0(H_4(seed), view)
  hash_init_prefix(&ctx, pp, HASH_PREFIX_0);
  hash_update(&ctx, tmp, hashlen);
  // hash input share
  hash_update(&ctx, prf_round->input_shares[vidx], pp->input_size);
  // hash communicated bits
  hash_update(&ctx, prf_round->communicated_bits[vidx], pp->view_size);
  // hash output share
  hash_update(&ctx, prf_round->output_shares[vidx], pp->output_size);
  hash_final(&ctx);
  hash_squeeze(&ctx, prf_round->commitments[vidx], hashlen);
}

/**
 * Compute commitment to 4 views.
 */
static void hash_commitment_x4(const picnic_instance_t* pp, proof_round_t* prf_round,
                               const unsigned int vidx) {
  const size_t hashlen = pp->digest_size;

  hash_context_x4 ctx;
  // hash the seed
  hash_init_prefix_x4(&ctx, pp, HASH_PREFIX_4);
  const uint8_t* seeds[4] = {prf_round[0].seeds[vidx], prf_round[1].seeds[vidx],
                             prf_round[2].seeds[vidx], prf_round[3].seeds[vidx]};
  hash_update_x4(&ctx, seeds, pp->seed_size);
  hash_final_x4(&ctx);
  uint8_t tmp[4][MAX_DIGEST_SIZE];
  uint8_t* tmpptr[4]             = {tmp[0], tmp[1], tmp[2], tmp[3]};
  const uint8_t* tmpptr_const[4] = {tmp[0], tmp[1], tmp[2], tmp[3]};
  hash_squeeze_x4(&ctx, tmpptr, hashlen);

  // compute H_0(H_4(seed), view)
  hash_init_prefix_x4(&ctx, pp, HASH_PREFIX_0);
  hash_update_x4(&ctx, tmpptr_const, hashlen);
  // hash input share
  const uint8_t* input_shares[4] = {
      prf_round[0].input_shares[vidx], prf_round[1].input_shares[vidx],
      prf_round[2].input_shares[vidx], prf_round[3].input_shares[vidx]};
  hash_update_x4(&ctx, input_shares, pp->input_size);
  // hash communicated bits
  const uint8_t* communicated_bits[4] = {
      prf_round[0].communicated_bits[vidx], prf_round[1].communicated_bits[vidx],
      prf_round[2].communicated_bits[vidx], prf_round[3].communicated_bits[vidx]};
  hash_update_x4(&ctx, communicated_bits, pp->view_size);
  // hash output share
  const uint8_t* output_shares[4] = {
      prf_round[0].output_shares[vidx], prf_round[1].output_shares[vidx],
      prf_round[2].output_shares[vidx], prf_round[3].output_shares[vidx]};
  hash_update_x4(&ctx, output_shares, pp->output_size);
  hash_final_x4(&ctx);
  uint8_t* commitments[4] = {prf_round[0].commitments[vidx], prf_round[1].commitments[vidx],
                             prf_round[2].commitments[vidx], prf_round[3].commitments[vidx]};
  hash_squeeze_x4(&ctx, commitments, hashlen);
}
/**
 * Compute commitment to 4 views, for verification
 */
static void hash_commitment_x4_verify(const picnic_instance_t* pp, sorting_helper_t* helper,
                                      const unsigned int vidx) {
  const size_t hashlen = pp->digest_size;

  hash_context_x4 ctx;
  // hash the seed
  hash_init_prefix_x4(&ctx, pp, HASH_PREFIX_4);
  const uint8_t* seeds[4] = {helper[0].round->seeds[vidx], helper[1].round->seeds[vidx],
                             helper[2].round->seeds[vidx], helper[3].round->seeds[vidx]};
  hash_update_x4(&ctx, seeds, pp->seed_size);
  hash_final_x4(&ctx);
  uint8_t tmp[4][MAX_DIGEST_SIZE];
  uint8_t* tmpptr[4]             = {tmp[0], tmp[1], tmp[2], tmp[3]};
  const uint8_t* tmpptr_const[4] = {tmp[0], tmp[1], tmp[2], tmp[3]};
  hash_squeeze_x4(&ctx, tmpptr, hashlen);

  // compute H_0(H_4(seed), view)
  hash_init_prefix_x4(&ctx, pp, HASH_PREFIX_0);
  hash_update_x4(&ctx, tmpptr_const, hashlen);
  // hash input share
  const uint8_t* input_shares[4] = {
      helper[0].round->input_shares[vidx], helper[1].round->input_shares[vidx],
      helper[2].round->input_shares[vidx], helper[3].round->input_shares[vidx]};
  hash_update_x4(&ctx, input_shares, pp->input_size);
  // hash communicated bits
  const uint8_t* communicated_bits[4] = {
      helper[0].round->communicated_bits[vidx], helper[1].round->communicated_bits[vidx],
      helper[2].round->communicated_bits[vidx], helper[3].round->communicated_bits[vidx]};
  hash_update_x4(&ctx, communicated_bits, pp->view_size);
  // hash output share
  const uint8_t* output_shares[4] = {
      helper[0].round->output_shares[vidx], helper[1].round->output_shares[vidx],
      helper[2].round->output_shares[vidx], helper[3].round->output_shares[vidx]};
  hash_update_x4(&ctx, output_shares, pp->output_size);
  hash_final_x4(&ctx);
  uint8_t* commitments[4] = {helper[0].round->commitments[vidx], helper[1].round->commitments[vidx],
                             helper[2].round->commitments[vidx],
                             helper[3].round->commitments[vidx]};
  hash_squeeze_x4(&ctx, commitments, hashlen);
}

/**
 * Compute challenge from transform dependent hash - outputs {1,2 or 3}^t
 */
static void H3_compute(const picnic_instance_t* pp, uint8_t* hash, uint8_t* ch) {
  const size_t digest_size      = pp->digest_size;
  const size_t digest_size_bits = digest_size << 3;

  // Pick bits from hash
  uint8_t* eof   = ch + pp->num_rounds;
  size_t bit_idx = 0;
  while (ch < eof) {
    if (bit_idx >= digest_size_bits) {
      hash_context ctx;
      hash_init_prefix(&ctx, pp, HASH_PREFIX_1);
      hash_update(&ctx, hash, digest_size);
      hash_final(&ctx);
      hash_squeeze(&ctx, hash, digest_size);
      bit_idx = 0;
    }

    const uint8_t twobits = (hash[bit_idx >> 3] >> ((6 - (bit_idx & 0x7)))) & 0x3;
    if (twobits != 0x3) {
      *ch++ = twobits;
    }
    bit_idx += 2;
  }
}

/**
 * Hash public key, salt and message
 */
static void H3_public_key_message(hash_context* ctx, const picnic_instance_t* pp,
                                  const uint8_t* salt, const uint8_t* circuit_output,
                                  const uint8_t* circuit_input, const uint8_t* m, size_t m_len) {
  // hash circuit out and input (public key)
  hash_update(ctx, circuit_output, pp->output_size);
  hash_update(ctx, circuit_input, pp->input_size);
  // hash salt
  hash_update(ctx, salt, SALT_SIZE);
  // hash message
  hash_update(ctx, m, m_len);
}

/**
 * Re-compute challenge for verification
 */
static void H3_verify(const picnic_instance_t* pp, sig_proof_t* prf, const uint8_t* circuit_output,
                      const uint8_t* circuit_input, const uint8_t* m, size_t m_len, uint8_t* ch) {
  const size_t digest_size = pp->digest_size;
  const size_t num_rounds  = pp->num_rounds;
  const size_t output_size = pp->output_size;

  hash_context ctx;
  hash_init_prefix(&ctx, pp, HASH_PREFIX_1);

  // hash output shares
  proof_round_t* round = prf->round;
  for (size_t i = 0; i < num_rounds; ++i, ++round) {
    switch (prf->challenge[i]) {
    case 0: {
      hash_update(&ctx, round->output_shares[0], output_size);
      hash_update(&ctx, round->output_shares[1], output_size);
      hash_update(&ctx, round->output_shares[2], output_size);
      break;
    }
    case 1: {
      hash_update(&ctx, round->output_shares[2], output_size);
      hash_update(&ctx, round->output_shares[0], output_size);
      hash_update(&ctx, round->output_shares[1], output_size);
      break;
    }
    default: {
      hash_update(&ctx, round->output_shares[1], output_size);
      hash_update(&ctx, round->output_shares[2], output_size);
      hash_update(&ctx, round->output_shares[0], output_size);
      break;
    }
    }
  }

  // hash commitments
  round = prf->round;
  for (size_t i = 0; i < num_rounds; ++i, ++round) {
    switch (prf->challenge[i]) {
    case 0: {
      hash_update(&ctx, round->commitments[0], digest_size);
      hash_update(&ctx, round->commitments[1], digest_size);
      hash_update(&ctx, round->commitments[2], digest_size);
      break;
    }
    case 1: {
      hash_update(&ctx, round->commitments[2], digest_size);
      hash_update(&ctx, round->commitments[0], digest_size);
      hash_update(&ctx, round->commitments[1], digest_size);
      break;
    }
    default: {
      hash_update(&ctx, round->commitments[1], digest_size);
      hash_update(&ctx, round->commitments[2], digest_size);
      hash_update(&ctx, round->commitments[0], digest_size);
      break;
    }
    }
  }

  if (pp->transform == TRANSFORM_UR) {
    const size_t without_input_bytes_size = pp->unruh_without_input_bytes_size;
    const size_t with_input_bytes_size    = pp->unruh_with_input_bytes_size;

    // hash commitments
    round = prf->round;
    for (size_t i = 0; i < num_rounds; ++i, ++round) {
      switch (prf->challenge[i]) {
      case 0: {
        hash_update(&ctx, round->gs[0], without_input_bytes_size);
        hash_update(&ctx, round->gs[1], without_input_bytes_size);
        hash_update(&ctx, round->gs[2], with_input_bytes_size);
        break;
      }
      case 1: {
        hash_update(&ctx, round->gs[2], without_input_bytes_size);
        hash_update(&ctx, round->gs[0], without_input_bytes_size);
        hash_update(&ctx, round->gs[1], with_input_bytes_size);
        break;
      }
      default: {
        hash_update(&ctx, round->gs[1], without_input_bytes_size);
        hash_update(&ctx, round->gs[2], without_input_bytes_size);
        hash_update(&ctx, round->gs[0], with_input_bytes_size);
        break;
      }
      }
    }
  }
  // hash public key, salt, and message
  H3_public_key_message(&ctx, pp, prf->salt, circuit_output, circuit_input, m, m_len);
  hash_final(&ctx);

  uint8_t hash[MAX_DIGEST_SIZE];
  hash_squeeze(&ctx, hash, digest_size);
  H3_compute(pp, hash, ch);
}

/**
 * Compute challenge
 */
static void H3(const picnic_instance_t* pp, sig_proof_t* prf, const uint8_t* circuit_output,
               const uint8_t* circuit_input, const uint8_t* m, size_t m_len) {
  const size_t num_rounds = pp->num_rounds;

  hash_context ctx;
  hash_init_prefix(&ctx, pp, HASH_PREFIX_1);

  // hash output shares
  hash_update(&ctx, prf->round[0].output_shares[0], pp->output_size * num_rounds * SC_PROOF);
  // hash all commitments C
  hash_update(&ctx, prf->round[0].commitments[0], pp->digest_size * num_rounds * SC_PROOF);
  if (pp->transform == TRANSFORM_UR) {
    // hash all commitments G
    hash_update(&ctx, prf->round[0].gs[0],
                num_rounds * ((SC_PROOF - 1) * pp->unruh_without_input_bytes_size +
                              pp->unruh_with_input_bytes_size));
  }
  // hash public key, salt, and message
  H3_public_key_message(&ctx, pp, prf->salt, circuit_output, circuit_input, m, m_len);
  hash_final(&ctx);

  uint8_t hash[MAX_DIGEST_SIZE];
  hash_squeeze(&ctx, hash, pp->digest_size);
  H3_compute(pp, hash, prf->challenge);
}

/*
 * G permutation for Unruh transform
 */
static void unruh_G(const picnic_instance_t* pp, proof_round_t* prf_round, unsigned int vidx,
                    bool include_is) {
  hash_context ctx;

  const size_t outputlen =
      include_is ? pp->unruh_with_input_bytes_size : pp->unruh_without_input_bytes_size;
  const uint16_t size_le   = htole16(outputlen);
  const size_t digest_size = pp->digest_size;
  const size_t seedlen     = pp->seed_size;

  // Hash the seed with H_5, store digest in output
  hash_init_prefix(&ctx, pp, HASH_PREFIX_5);
  hash_update(&ctx, prf_round->seeds[vidx], seedlen);
  hash_final(&ctx);

  uint8_t tmp[MAX_DIGEST_SIZE];
  hash_squeeze(&ctx, tmp, digest_size);

  // Hash H_5(seed), the view, and the length
  hash_init(&ctx, pp);
  hash_update(&ctx, tmp, digest_size);
  if (include_is) {
    hash_update(&ctx, prf_round->input_shares[vidx], pp->input_size);
  }
  hash_update(&ctx, prf_round->communicated_bits[vidx], pp->view_size);
  hash_update(&ctx, (const uint8_t*)&size_le, sizeof(uint16_t));
  hash_final(&ctx);
  hash_squeeze(&ctx, prf_round->gs[vidx], outputlen);
}

/*
 * 4x G permutation for Unruh transform
 */
static void unruh_G_x4(const picnic_instance_t* pp, proof_round_t* prf_round, unsigned int vidx,
                       bool include_is) {
  hash_context_x4 ctx;

  const size_t outputlen =
      include_is ? pp->unruh_with_input_bytes_size : pp->unruh_without_input_bytes_size;
  const uint16_t size_le   = htole16(outputlen);
  const size_t digest_size = pp->digest_size;
  const size_t seedlen     = pp->seed_size;

  // Hash the seed with H_5, store digest in output
  hash_init_prefix_x4(&ctx, pp, HASH_PREFIX_5);
  const uint8_t* seeds[4] = {prf_round[0].seeds[vidx], prf_round[1].seeds[vidx],
                             prf_round[2].seeds[vidx], prf_round[3].seeds[vidx]};
  hash_update_x4(&ctx, seeds, seedlen);
  hash_final_x4(&ctx);

  uint8_t tmp[4][MAX_DIGEST_SIZE];
  uint8_t* tmpptr[4]             = {tmp[0], tmp[1], tmp[2], tmp[3]};
  const uint8_t* tmpptr_const[4] = {tmp[0], tmp[1], tmp[2], tmp[3]};
  hash_squeeze_x4(&ctx, tmpptr, digest_size);

  // Hash H_5(seed), the view, and the length
  hash_init_x4(&ctx, pp);
  hash_update_x4(&ctx, tmpptr_const, digest_size);
  if (include_is) {
    const uint8_t* input_shares[4] = {
        prf_round[0].input_shares[vidx], prf_round[1].input_shares[vidx],
        prf_round[2].input_shares[vidx], prf_round[3].input_shares[vidx]};
    hash_update_x4(&ctx, input_shares, pp->input_size);
  }
  const uint8_t* communicated_bits[4] = {
      prf_round[0].communicated_bits[vidx],
      prf_round[1].communicated_bits[vidx],
      prf_round[2].communicated_bits[vidx],
      prf_round[3].communicated_bits[vidx],
  };
  hash_update_x4(&ctx, communicated_bits, pp->view_size);
  const uint8_t* sizes[4] = {(const uint8_t*)&size_le, (const uint8_t*)&size_le,
                             (const uint8_t*)&size_le, (const uint8_t*)&size_le};
  hash_update_x4(&ctx, sizes, sizeof(uint16_t));
  hash_final_x4(&ctx);
  uint8_t* gs[4] = {prf_round[0].gs[vidx], prf_round[1].gs[vidx], prf_round[2].gs[vidx],
                    prf_round[3].gs[vidx]};
  hash_squeeze_x4(&ctx, gs, outputlen);
}

/*
 * 4x G permutation for Unruh transform, for verification
 */
static void unruh_G_x4_verify(const picnic_instance_t* pp, sorting_helper_t* helper,
                              unsigned int vidx, bool include_is) {
  hash_context_x4 ctx;

  const size_t outputlen =
      include_is ? pp->unruh_with_input_bytes_size : pp->unruh_without_input_bytes_size;
  const uint16_t size_le   = htole16(outputlen);
  const size_t digest_size = pp->digest_size;
  const size_t seedlen     = pp->seed_size;

  // Hash the seed with H_5, store digest in output
  hash_init_prefix_x4(&ctx, pp, HASH_PREFIX_5);
  const uint8_t* seeds[4] = {helper[0].round->seeds[vidx], helper[1].round->seeds[vidx],
                             helper[2].round->seeds[vidx], helper[3].round->seeds[vidx]};
  hash_update_x4(&ctx, seeds, seedlen);
  hash_final_x4(&ctx);

  uint8_t tmp[4][MAX_DIGEST_SIZE];
  uint8_t* tmpptr[4]             = {tmp[0], tmp[1], tmp[2], tmp[3]};
  const uint8_t* tmpptr_const[4] = {tmp[0], tmp[1], tmp[2], tmp[3]};
  hash_squeeze_x4(&ctx, tmpptr, digest_size);

  // Hash H_5(seed), the view, and the length
  hash_init_x4(&ctx, pp);
  hash_update_x4(&ctx, tmpptr_const, digest_size);
  if (include_is) {
    const uint8_t* input_shares[4] = {
        helper[0].round->input_shares[vidx], helper[1].round->input_shares[vidx],
        helper[2].round->input_shares[vidx], helper[3].round->input_shares[vidx]};
    hash_update_x4(&ctx, input_shares, pp->input_size);
  }
  const uint8_t* communicated_bits[4] = {
      helper[0].round->communicated_bits[vidx],
      helper[1].round->communicated_bits[vidx],
      helper[2].round->communicated_bits[vidx],
      helper[3].round->communicated_bits[vidx],
  };
  hash_update_x4(&ctx, communicated_bits, pp->view_size);
  const uint8_t* sizes[4] = {(const uint8_t*)&size_le, (const uint8_t*)&size_le,
                             (const uint8_t*)&size_le, (const uint8_t*)&size_le};
  hash_update_x4(&ctx, sizes, sizeof(uint16_t));
  hash_final_x4(&ctx);
  uint8_t* gs[4] = {helper[0].round->gs[vidx], helper[1].round->gs[vidx], helper[2].round->gs[vidx],
                    helper[3].round->gs[vidx]};
  hash_squeeze_x4(&ctx, gs, outputlen);
}

// serilization helper functions
static int sig_proof_to_char_array(const picnic_instance_t* pp, const sig_proof_t* prf,
                                   uint8_t* result, size_t* siglen) {
  const uint32_t num_rounds                   = pp->num_rounds;
  const uint32_t seed_size                    = pp->seed_size;
  const uint32_t challenge_size               = pp->collapsed_challenge_size;
  const uint32_t digest_size                  = pp->digest_size;
  const transform_t transform                 = pp->transform;
  const size_t view_size                      = pp->view_size;
  const size_t input_size                     = pp->input_size;
  const size_t unruh_with_input_bytes_size    = pp->unruh_with_input_bytes_size;
  const size_t unruh_without_input_bytes_size = pp->unruh_without_input_bytes_size;

  uint8_t* tmp = result;

  // write challenge
  collapse_challenge(tmp, pp, prf->challenge);
  tmp += challenge_size;

  // write salt
  memcpy(tmp, prf->salt, SALT_SIZE);
  tmp += SALT_SIZE;

  const proof_round_t* round = prf->round;
  for (unsigned i = 0; i < num_rounds; ++i, ++round) {
    const unsigned int a = prf->challenge[i];
    const unsigned int b = (a + 1) % 3;
    const unsigned int c = (a + 2) % 3;

    // write commitment
    memcpy(tmp, round->commitments[c], digest_size);
    tmp += digest_size;

    // write unruh G
    if (transform == TRANSFORM_UR) {
      const uint32_t unruh_g_size =
          a ? unruh_without_input_bytes_size : unruh_with_input_bytes_size;
      memcpy(tmp, round->gs[c], unruh_g_size);
      tmp += unruh_g_size;
    }

    // write views
    memcpy(tmp, round->communicated_bits[b], view_size);
    tmp += view_size;

    // write seeds
    memcpy(tmp, round->seeds[a], seed_size);
    tmp += seed_size;
    memcpy(tmp, round->seeds[b], seed_size);
    tmp += seed_size;

    if (a) {
      // write input share
      memcpy(tmp, round->input_shares[SC_PROOF - 1], input_size);
      tmp += input_size;
    }
  }

  *siglen = tmp - result;
  return 0;
}

static sig_proof_t* sig_proof_from_char_array(const picnic_instance_t* pp, const uint8_t* data,
                                              size_t len) {
  const size_t digest_size              = pp->digest_size;
  const size_t seed_size                = pp->seed_size;
  const size_t num_rounds               = pp->num_rounds;
  const size_t challenge_size           = pp->collapsed_challenge_size;
  const transform_t transform           = pp->transform;
  const size_t input_size               = pp->input_size;
  const size_t view_size                = pp->view_size;
  const size_t without_input_bytes_size = pp->unruh_without_input_bytes_size;
  const size_t with_input_bytes_size    = pp->unruh_with_input_bytes_size;

  uint8_t* slab      = NULL;
  sig_proof_t* proof = proof_new_verify(pp, &slab);
  if (!proof) {
    return NULL;
  }

  size_t remaining_len = len;
  const uint8_t* tmp   = data;

  // read and process challenge
  if (sub_overflow_size_t(remaining_len, challenge_size, &remaining_len)) {
    goto err;
  }
  if (!expand_challenge(proof->challenge, pp, tmp)) {
    goto err;
  }
  tmp += challenge_size;

  // read salt
  if (sub_overflow_size_t(remaining_len, SALT_SIZE, &remaining_len)) {
    goto err;
  }
  memcpy(proof->salt, tmp, SALT_SIZE);
  tmp += SALT_SIZE;

  const size_t base_size = digest_size + view_size + 2 * seed_size;
  proof_round_t* round   = proof->round;
  for (unsigned int i = 0; i < num_rounds; ++i, ++round) {
    const unsigned char ch      = proof->challenge[i];
    const size_t unruh_g_len    = ch ? without_input_bytes_size : with_input_bytes_size;
    const size_t requested_size = base_size + unruh_g_len + (ch ? input_size : 0);
    if (sub_overflow_size_t(remaining_len, requested_size, &remaining_len)) {
      goto err;
    }

    // read commitments
    round->commitments[2] = (uint8_t*)tmp;
    tmp += digest_size;

    // read unruh G
    if (transform == TRANSFORM_UR) {
      round->gs[2] = (uint8_t*)tmp;
      tmp += unruh_g_len;
    }

    // read view
    round->communicated_bits[1] = (uint8_t*)tmp;
    tmp += view_size;

    // read seeds
    round->seeds[0] = (uint8_t*)tmp;
    tmp += seed_size;
    round->seeds[1] = (uint8_t*)tmp;
    tmp += seed_size;

    // read input shares
    switch (ch) {
    case 0:
      round->input_shares[0] = slab;
      slab += input_size;
      round->input_shares[1] = slab;
      slab += input_size;
      break;
    case 1:
      round->input_shares[0] = slab;
      slab += input_size;
      round->input_shares[1] = (uint8_t*)tmp;
      tmp += input_size;
      break;
    default:
      round->input_shares[0] = (uint8_t*)tmp;
      tmp += input_size;
      round->input_shares[1] = slab;
      slab += input_size;
    }
  }

  if (remaining_len) {
    goto err;
  }

  return proof;

err:
  proof_free(proof);
  return NULL;
}

static void generate_seeds(const picnic_instance_t* pp, const uint8_t* private_key,
                           const uint8_t* plaintext, const uint8_t* public_key, const uint8_t* m,
                           size_t m_len, uint8_t* seeds, uint8_t* salt) {
  const lowmc_t* lowmc     = pp->lowmc;
  const size_t seed_size   = pp->seed_size;
  const size_t num_rounds  = pp->num_rounds;
  const size_t input_size  = pp->input_size;
  const size_t output_size = pp->output_size;
  const size_t lowmc_n     = lowmc->n;

  kdf_shake_t ctx;
  kdf_shake_init(&ctx, pp);
  // sk || m || C || p
  kdf_shake_update_key(&ctx, private_key, input_size);
  kdf_shake_update_key(&ctx, m, m_len);
  kdf_shake_update_key(&ctx, public_key, output_size);
  kdf_shake_update_key(&ctx, plaintext, output_size);
  // N as 16 bit LE integer
  const uint16_t size_le = htole16(lowmc_n);
  kdf_shake_update_key(&ctx, (const uint8_t*)&size_le, sizeof(size_le));
#if defined(WITH_EXTRA_RANDOMNESS)
  // Add extra randomn bytes for fault attack mitigation
  unsigned char buffer[2 * MAX_DIGEST_SIZE];
  rand_bytes(buffer, 2 * seed_size);
  kdf_shake_update_key(&ctx, buffer, 2 * seed_size);
#endif
  kdf_shake_finalize_key(&ctx);

  // Generate seeds and salt
  kdf_shake_get_randomness(&ctx, seeds, seed_size * num_rounds * SC_PROOF);
  kdf_shake_get_randomness(&ctx, salt, SALT_SIZE);
  kdf_shake_clear(&ctx);
}

static int sign_impl(const picnic_instance_t* pp, const uint8_t* private_key,
                     const lowmc_key_t* lowmc_key, const uint8_t* plaintext, const mzd_local_t* p,
                     const uint8_t* public_key, const uint8_t* m, size_t m_len, uint8_t* sig,
                     size_t* siglen) {
  const lowmc_t* lowmc        = pp->lowmc;
  const size_t num_rounds     = pp->num_rounds;
  const transform_t transform = pp->transform;
  const size_t input_size     = pp->input_size;
  const size_t output_size    = pp->output_size;
  const size_t view_count     = lowmc->r;
  const size_t lowmc_k        = lowmc->k;
  const size_t lowmc_n        = lowmc->n;
  const size_t lowmc_r        = lowmc->r;
  const size_t view_size      = pp->view_size;

  const zkbpp_lowmc_implementation_f lowmc_impl       = pp->impls.zkbpp_lowmc;
  const lowmc_store_implementation_f lowmc_store_impl = pp->impls.lowmc_store;
  const zkbpp_share_implementation_f mzd_share        = pp->impls.mzd_share;

  // Perform LowMC evaluation and record state before AND gates
  recorded_state_t recorded_state;
  recorded_state.state = calloc(lowmc_r + 1, sizeof(mzd_local_t*));
  mzd_local_init_multiple_ex(recorded_state.state, lowmc_r + 1, 1, lowmc_n, false);
  lowmc_store_impl(lowmc_key, p, &recorded_state);

  sig_proof_t* prf = proof_new(pp);
  view_t* views    = calloc(sizeof(view_t), view_count);

  in_out_shares_t in_out_shares[2];
  mzd_local_init_multiple_ex(in_out_shares[0].s, SC_PROOF, 1, lowmc_k, false);
  mzd_local_init_multiple_ex(in_out_shares[1].s, SC_PROOF, 1, lowmc_n, false);

  // Generate seeds
  generate_seeds(pp, private_key, plaintext, public_key, m, m_len, prf->round[0].seeds[0],
                 prf->salt);

  mzd_local_t* shared_key[SC_PROOF];
  mzd_local_init_multiple(shared_key, SC_PROOF, 1, lowmc_k);

  rvec_t* rvec = calloc(sizeof(rvec_t), lowmc_r); // random tapes for AND-gates

  uint8_t* tape_bytes  = malloc(view_size);
  proof_round_t* round = prf->round;
  // use 4 parallel instances of keccak for speedup
  uint8_t* tape_bytes_x4[SC_PROOF][4];
  for (unsigned k = 0; k < SC_PROOF; k++) { /* OQS note: changed i to k to avoid shadowing i */
    for (unsigned j = 0; j < 4; j++) {
      tape_bytes_x4[k][j] = malloc(view_size);
    }
  }
  unsigned int i = 0;
  for (; i < (num_rounds / 4) * 4; i += 4, round += 4) {
    kdf_shake_x4_t kdfs[SC_PROOF];
    for (unsigned int j = 0; j < SC_PROOF; ++j) {
      const bool include_input_size   = (j != SC_PROOF - 1);
      const uint8_t* seeds[4]         = {round[0].seeds[j], round[1].seeds[j], round[2].seeds[j],
                                 round[3].seeds[j]};
      const uint16_t round_numbers[4] = {i, i + 1, i + 2, i + 3};
      kdf_init_x4_from_seed(&kdfs[j], seeds, prf->salt, round_numbers, j, include_input_size, pp);
    }

    // compute sharing
    for (unsigned int j = 0; j < SC_PROOF - 1; ++j) {
      uint8_t* input_shares[4] = {round[0].input_shares[j], round[1].input_shares[j],
                                  round[2].input_shares[j], round[3].input_shares[j]};
      kdf_shake_x4_get_randomness(&kdfs[j], input_shares, input_size);
    }
    // compute random tapes
    for (unsigned int j = 0; j < SC_PROOF; ++j) {
      kdf_shake_x4_get_randomness(&kdfs[j], tape_bytes_x4[j], view_size);
      kdf_shake_x4_clear(&kdfs[j]);
    }

    for (unsigned int round_offset = 0; round_offset < 4; round_offset++) {
      for (unsigned int j = 0; j < SC_PROOF - 1; ++j) {
        mzd_from_char_array(shared_key[j], round[round_offset].input_shares[j], input_size);
      }
      mzd_share(shared_key[2], shared_key[0], shared_key[1], lowmc_key);
      mzd_to_char_array(round[round_offset].input_shares[SC_PROOF - 1], shared_key[SC_PROOF - 1],
                        input_size);

      for (unsigned int j = 0; j < SC_PROOF; ++j) {
        decompress_random_tape(rvec, pp, tape_bytes_x4[j][round_offset], j);
      }

      // perform ZKB++ LowMC evaluation
      lowmc_impl(shared_key, p, views, in_out_shares, rvec, &recorded_state);

      for (unsigned int j = 0; j < SC_PROOF; ++j) {
        mzd_to_char_array(round[round_offset].output_shares[j], in_out_shares[1].s[j], output_size);
        compress_view(round[round_offset].communicated_bits[j], pp, views, j);
      }
    }

    // commitments
    for (unsigned int j = 0; j < SC_PROOF; ++j) {
      hash_commitment_x4(pp, round, j);
    }

    // unruh G
    if (transform == TRANSFORM_UR) {
      for (unsigned int j = 0; j < SC_PROOF; ++j) {
        unruh_G_x4(pp, round, j, j == SC_PROOF - 1);
      }
    }
  }
  for (; i < num_rounds; ++i, ++round) {
    kdf_shake_t kdfs[SC_PROOF];
    for (unsigned int j = 0; j < SC_PROOF; ++j) {
      const bool include_input_size = (j != SC_PROOF - 1);
      kdf_init_from_seed(&kdfs[j], round->seeds[j], prf->salt, i, j, include_input_size, pp);
    }

    // compute sharing
    for (unsigned int j = 0; j < SC_PROOF - 1; ++j) {
      kdf_shake_get_randomness(&kdfs[j], round->input_shares[j], input_size);
      mzd_from_char_array(shared_key[j], round->input_shares[j], input_size);
    }
    mzd_share(shared_key[2], shared_key[0], shared_key[1], lowmc_key);
    mzd_to_char_array(round->input_shares[SC_PROOF - 1], shared_key[SC_PROOF - 1], input_size);

    // compute random tapes
    for (unsigned int j = 0; j < SC_PROOF; ++j) {
      kdf_shake_get_randomness(&kdfs[j], tape_bytes, view_size);
      decompress_random_tape(rvec, pp, tape_bytes, j);
    }

    for (unsigned int j = 0; j < SC_PROOF; ++j) {
      kdf_shake_clear(&kdfs[j]);
    }

    // perform ZKB++ LowMC evaluation
    lowmc_impl(shared_key, p, views, in_out_shares, rvec, &recorded_state);

    // commitments
    for (unsigned int j = 0; j < SC_PROOF; ++j) {
      mzd_to_char_array(round->output_shares[j], in_out_shares[1].s[j], output_size);
      compress_view(round->communicated_bits[j], pp, views, j);
      hash_commitment(pp, round, j);
    }

    // unruh G
    if (transform == TRANSFORM_UR) {
      for (unsigned int j = 0; j < SC_PROOF; ++j) {
        unruh_G(pp, round, j, j == SC_PROOF - 1);
      }
    }
  }
  H3(pp, prf, public_key, plaintext, m, m_len);

  const int ret = sig_proof_to_char_array(pp, prf, sig, siglen);

  // clean up
  for (unsigned k = 0; k < SC_PROOF; k++) {
    for (unsigned j = 0; j < 4; j++) {
      free(tape_bytes_x4[k][j]);
    }
  }
  free(tape_bytes);
  free(rvec);
  free(views);
  mzd_local_free_multiple(shared_key);
  mzd_local_free_multiple(in_out_shares[1].s);
  mzd_local_free_multiple(in_out_shares[0].s);
  proof_free(prf);

  mzd_local_free_multiple(recorded_state.state);
  free(recorded_state.state);

  return ret;
}

static int verify_impl(const picnic_instance_t* pp, const uint8_t* plaintext, mzd_local_t const* p,
                       const uint8_t* ciphertext, mzd_local_t const* c, const uint8_t* m,
                       size_t m_len, const uint8_t* sig, size_t siglen) {
  const size_t num_rounds     = pp->num_rounds;
  const lowmc_t* lowmc        = pp->lowmc;
  const transform_t transform = pp->transform;
  const size_t input_size     = pp->input_size;
  const size_t output_size    = pp->output_size;
  const size_t view_count     = lowmc->r;
  const size_t lowmc_k        = lowmc->k;
  const size_t lowmc_n        = lowmc->n;
  const size_t lowmc_r        = lowmc->r;
  const size_t view_size      = pp->view_size;

  const zkbpp_lowmc_verify_implementation_f lowmc_verify_impl = pp->impls.zkbpp_lowmc_verify;
  const zkbpp_share_implementation_f mzd_share                = pp->impls.mzd_share;

  sig_proof_t* prf = sig_proof_from_char_array(pp, sig, siglen);
  if (!prf) {
    return -1;
  }

  in_out_shares_t in_out_shares[2];
  mzd_local_init_multiple_ex(in_out_shares[0].s, SC_VERIFY, 1, lowmc_k, false);
  mzd_local_init_multiple_ex(in_out_shares[1].s, SC_PROOF, 1, lowmc_n, false);
  view_t* views       = calloc(sizeof(view_t), view_count);
  rvec_t* rvec        = calloc(sizeof(rvec_t), lowmc_r); // random tapes for and-gates
  uint8_t* tape_bytes = malloc(view_size);

  // sort the different challenge rounds based on their H3 index, so we can use the 4x Keccak when
  // verifying since all of this is public information, there is no leakage
  uint8_t* tape_bytes_x4[SC_VERIFY][4];
  for (unsigned i = 0; i < SC_VERIFY; i++) {
    for (unsigned j = 0; j < 4; j++) {
      tape_bytes_x4[i][j] = malloc(view_size);
    }
  }
  sorting_helper_t* sorted_rounds = malloc(sizeof(sorting_helper_t) * num_rounds);
  for (unsigned current_chal = 0; current_chal < 3; current_chal++) {
    unsigned int num_current_rounds = 0;
    for (unsigned r = 0; r < num_rounds; r++) {
      if (prf->challenge[r] == current_chal) {
        sorted_rounds[num_current_rounds].round        = &prf->round[r];
        sorted_rounds[num_current_rounds].round_number = r;
        num_current_rounds++;
      }
    }
    unsigned int i           = 0;
    sorting_helper_t* helper = sorted_rounds;
    for (; i < (num_current_rounds / 4) * 4; i += 4, helper += 4) {
      const unsigned int a_i = current_chal;
      const unsigned int b_i = (a_i + 1) % 3;
      const unsigned int c_i = (a_i + 2) % 3;

      kdf_shake_x4_t kdfs[SC_VERIFY];
      for (unsigned int j = 0; j < SC_VERIFY; ++j) {
        const bool include_input_size    = (j == 0 && b_i) || (j == 1 && c_i);
        const unsigned int player_number = (j == 0) ? a_i : b_i;
        const uint8_t* seeds[4]          = {helper[0].round->seeds[j], helper[1].round->seeds[j],
                                   helper[2].round->seeds[j], helper[3].round->seeds[j]};
        const uint16_t round_numbers[4]  = {helper[0].round_number, helper[1].round_number,
                                           helper[2].round_number, helper[3].round_number};
        kdf_init_x4_from_seed(&kdfs[j], seeds, prf->salt, round_numbers, player_number,
                              include_input_size, pp);
      }

      // compute input shares if necessary
      if (b_i) {
        uint8_t* input_shares[4] = {
            helper[0].round->input_shares[0], helper[1].round->input_shares[0],
            helper[2].round->input_shares[0], helper[3].round->input_shares[0]};
        kdf_shake_x4_get_randomness(&kdfs[0], input_shares, input_size);
      }
      if (c_i) {
        uint8_t* input_shares[4] = {
            helper[0].round->input_shares[1], helper[1].round->input_shares[1],
            helper[2].round->input_shares[1], helper[3].round->input_shares[1]};
        kdf_shake_x4_get_randomness(&kdfs[1], input_shares, input_size);
      }
      // compute random tapes
      for (unsigned int j = 0; j < SC_VERIFY; ++j) {
        kdf_shake_x4_get_randomness(&kdfs[j], tape_bytes_x4[j], view_size);
        kdf_shake_clear(&kdfs[j]);
      }
      for (unsigned int round_offset = 0; round_offset < 4; round_offset++) {
        mzd_from_char_array(in_out_shares[0].s[0], helper[round_offset].round->input_shares[0],
                            input_size);
        mzd_from_char_array(in_out_shares[0].s[1], helper[round_offset].round->input_shares[1],
                            input_size);

        for (unsigned int j = 0; j < SC_VERIFY; ++j) {
          decompress_random_tape(rvec, pp, tape_bytes_x4[j][round_offset], j);
        }

        decompress_view(views, pp, helper[round_offset].round->communicated_bits[1], 1);
        // perform ZKB++ LowMC evaluation
        lowmc_verify_impl(p, views, in_out_shares, rvec, a_i);
        compress_view(helper[round_offset].round->communicated_bits[0], pp, views, 0);

        mzd_share(in_out_shares[1].s[2], in_out_shares[1].s[0], in_out_shares[1].s[1], c);
        // recompute commitments
        for (unsigned int j = 0; j < SC_VERIFY; ++j) {
          mzd_to_char_array(helper[round_offset].round->output_shares[j], in_out_shares[1].s[j],
                            output_size);
        }
        mzd_to_char_array(helper[round_offset].round->output_shares[SC_VERIFY],
                          in_out_shares[1].s[SC_VERIFY], output_size);
      }
      for (unsigned int j = 0; j < SC_VERIFY; ++j) {
        hash_commitment_x4_verify(pp, helper, j);
      }
      if (transform == TRANSFORM_UR) {
        // apply Unruh G permutation
        for (unsigned int j = 0; j < SC_VERIFY; ++j) {
          unruh_G_x4_verify(pp, helper, j, (a_i == 1 && j == 1) || (a_i == 2 && j == 0));
        }
      }
    }
    for (; i < num_current_rounds; ++i, ++helper) {
      const unsigned int a_i = current_chal;
      const unsigned int b_i = (a_i + 1) % 3;
      const unsigned int c_i = (a_i + 2) % 3;

      kdf_shake_t kdfs[SC_VERIFY];
      for (unsigned int j = 0; j < SC_VERIFY; ++j) {
        const bool include_input_size    = (j == 0 && b_i) || (j == 1 && c_i);
        const unsigned int player_number = (j == 0) ? a_i : b_i;
        kdf_init_from_seed(&kdfs[j], helper->round->seeds[j], prf->salt, helper->round_number,
                           player_number, include_input_size, pp);
      }

      // compute input shares if necessary
      if (b_i) {
        kdf_shake_get_randomness(&kdfs[0], helper->round->input_shares[0], input_size);
      }
      if (c_i) {
        kdf_shake_get_randomness(&kdfs[1], helper->round->input_shares[1], input_size);
      }

      mzd_from_char_array(in_out_shares[0].s[0], helper->round->input_shares[0], input_size);
      mzd_from_char_array(in_out_shares[0].s[1], helper->round->input_shares[1], input_size);

      // compute random tapes
      for (unsigned int j = 0; j < SC_VERIFY; ++j) {
        kdf_shake_get_randomness(&kdfs[j], tape_bytes, view_size);
        decompress_random_tape(rvec, pp, tape_bytes, j);
      }

      for (unsigned int j = 0; j < SC_VERIFY; ++j) {
        kdf_shake_clear(&kdfs[j]);
      }

      decompress_view(views, pp, helper->round->communicated_bits[1], 1);
      // perform ZKB++ LowMC evaluation
      lowmc_verify_impl(p, views, in_out_shares, rvec, a_i);
      compress_view(helper->round->communicated_bits[0], pp, views, 0);

      mzd_share(in_out_shares[1].s[2], in_out_shares[1].s[0], in_out_shares[1].s[1], c);
      // recompute commitments
      for (unsigned int j = 0; j < SC_VERIFY; ++j) {
        mzd_to_char_array(helper->round->output_shares[j], in_out_shares[1].s[j], output_size);
        hash_commitment(pp, helper->round, j);
      }
      mzd_to_char_array(helper->round->output_shares[SC_VERIFY], in_out_shares[1].s[SC_VERIFY],
                        output_size);

      if (transform == TRANSFORM_UR) {
        // apply Unruh G permutation
        for (unsigned int j = 0; j < SC_VERIFY; ++j) {
          unruh_G(pp, helper->round, j, (a_i == 1 && j == 1) || (a_i == 2 && j == 0));
        }
      }
    }
  }
  unsigned char challenge[MAX_NUM_ROUNDS] = {0};
  H3_verify(pp, prf, ciphertext, plaintext, m, m_len, challenge);
  const int success_status = memcmp(challenge, prf->challenge, pp->num_rounds);

  // clean up
  free(sorted_rounds);
  for (unsigned i = 0; i < SC_VERIFY; i++) {
    for (unsigned j = 0; j < 4; j++) {
      free(tape_bytes_x4[i][j]);
    }
  }
  free(tape_bytes);
  free(rvec);
  free(views);
  mzd_local_free_multiple(in_out_shares[1].s);
  mzd_local_free_multiple(in_out_shares[0].s);

  proof_free(prf);

  return success_status;
}

int impl_sign(const picnic_instance_t* pp, const uint8_t* plaintext, const uint8_t* private_key,
              const uint8_t* public_key, const uint8_t* msg, size_t msglen, uint8_t* sig,
              size_t* siglen) {
  mzd_local_t* m_plaintext  = mzd_local_init_ex(1, pp->lowmc->n, false);
  mzd_local_t* m_privatekey = mzd_local_init_ex(1, pp->lowmc->k, false);

  mzd_from_char_array(m_plaintext, plaintext, pp->output_size);
  mzd_from_char_array(m_privatekey, private_key, pp->input_size);

  const int result = sign_impl(pp, private_key, m_privatekey, plaintext, m_plaintext, public_key,
                               msg, msglen, sig, siglen);

  mzd_local_free(m_privatekey);
  mzd_local_free(m_plaintext);

  return result;
}

int impl_verify(const picnic_instance_t* pp, const uint8_t* plaintext, const uint8_t* public_key,
                const uint8_t* msg, size_t msglen, const uint8_t* sig, size_t siglen) {
  mzd_local_t* m_plaintext = mzd_local_init_ex(1, pp->lowmc->n, false);
  mzd_local_t* m_publickey = mzd_local_init_ex(1, pp->lowmc->n, false);

  mzd_from_char_array(m_plaintext, plaintext, pp->output_size);
  mzd_from_char_array(m_publickey, public_key, pp->output_size);

  const int result =
      verify_impl(pp, plaintext, m_plaintext, public_key, m_publickey, msg, msglen, sig, siglen);

  mzd_local_free(m_publickey);
  mzd_local_free(m_plaintext);

  return result;
}

/* cropped unused visualize_signature */

// instance handling

// L1, L3, and L5 LowMC instances
#if defined(WITH_LOWMC_128_128_20)
#include "lowmc_128_128_20.h"
#define LOWMC_L1_OR_NULL &lowmc_128_128_20
#else
#define LOWMC_L1_OR_NULL NULL
#endif
#if defined(WITH_LOWMC_192_192_30)
#include "lowmc_192_192_30.h"
#define LOWMC_L3_OR_NULL &lowmc_192_192_30
#else
#define LOWMC_L3_OR_NULL NULL
#endif
#if defined(WITH_LOWMC_256_256_38)
#include "lowmc_256_256_38.h"
#define LOWMC_L5_OR_NULL &lowmc_256_256_38
#else
#define LOWMC_L5_OR_NULL NULL
#endif

// L1, L3, and L5 lowmc instances with 1 SBOX
#if defined(WITH_LOWMC_128_128_182)
#include "lowmc_128_128_182.h"
#define LOWMC_L1_1_OR_NULL &lowmc_128_128_182
#else
#define LOWMC_L1_1_OR_NULL NULL
#endif
#if defined(WITH_LOWMC_192_192_284)
#include "lowmc_192_192_284.h"
#define LOWMC_L3_1_OR_NULL &lowmc_192_192_284
#else
#define LOWMC_L3_1_OR_NULL NULL
#endif
#if defined(WITH_LOWMC_256_256_363)
#include "lowmc_256_256_363.h"
#define LOWMC_L5_1_OR_NULL &lowmc_256_256_363
#else
#define LOWMC_L5_1_OR_NULL NULL
#endif

#if defined(MUL_M4RI)
static bool lowmc_instances_initialized[6];
static lowmc_t* const lowmc_instances[6] = {
#else
static const lowmc_t* const lowmc_instances[6] = {
#endif
    LOWMC_L1_OR_NULL,   LOWMC_L3_OR_NULL,   LOWMC_L5_OR_NULL,
    LOWMC_L1_1_OR_NULL, LOWMC_L3_1_OR_NULL, LOWMC_L5_1_OR_NULL};

#define NULL_FNS                                                                                   \
  { NULL, NULL, NULL, NULL, NULL, NULL, NULL }

static picnic_instance_t instances[PARAMETER_SET_MAX_INDEX] = {
    {NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, PARAMETER_SET_INVALID, TRANSFORM_FS, NULL_FNS},
    {LOWMC_L1_OR_NULL, 32, 16, 219, 219, 3, 16, 16, 75, 30, 55, 0, 0,
     PICNIC_SIGNATURE_SIZE_Picnic_L1_FS, Picnic_L1_FS, TRANSFORM_FS, NULL_FNS},
    {LOWMC_L1_OR_NULL, 32, 16, 219, 219, 3, 16, 16, 75, 30, 55, 91, 107,
     PICNIC_SIGNATURE_SIZE_Picnic_L1_UR, Picnic_L1_UR, TRANSFORM_UR, NULL_FNS},
    {LOWMC_L3_OR_NULL, 48, 24, 329, 329, 3, 24, 24, 113, 30, 83, 0, 0,
     PICNIC_SIGNATURE_SIZE_Picnic_L3_FS, Picnic_L3_FS, TRANSFORM_FS, NULL_FNS},
    {LOWMC_L3_OR_NULL, 48, 24, 329, 329, 3, 24, 24, 113, 30, 83, 137, 161,
     PICNIC_SIGNATURE_SIZE_Picnic_L3_UR, Picnic_L3_UR, TRANSFORM_UR, NULL_FNS},
    {LOWMC_L5_OR_NULL, 64, 32, 438, 438, 3, 32, 32, 143, 30, 110, 0, 0,
     PICNIC_SIGNATURE_SIZE_Picnic_L5_FS, Picnic_L5_FS, TRANSFORM_FS, NULL_FNS},
    {LOWMC_L5_OR_NULL, 64, 32, 438, 438, 3, 32, 32, 143, 30, 110, 175, 207,
     PICNIC_SIGNATURE_SIZE_Picnic_L5_UR, Picnic_L5_UR, TRANSFORM_UR, NULL_FNS},
    // Picnic2 params
    {LOWMC_L1_OR_NULL, 32, 16, 343, 27, 64, 16, 16, 75, 30, 55, 0, 0,
     PICNIC_SIGNATURE_SIZE_Picnic2_L1_FS, Picnic2_L1_FS, TRANSFORM_FS, NULL_FNS},
    {LOWMC_L3_OR_NULL, 48, 24, 570, 39, 64, 24, 24, 113, 30, 83, 0, 0,
     PICNIC_SIGNATURE_SIZE_Picnic2_L3_FS, Picnic2_L3_FS, TRANSFORM_FS, NULL_FNS},
    {LOWMC_L5_OR_NULL, 64, 32, 803, 50, 64, 32, 32, 143, 30, 110, 0, 0,
     PICNIC_SIGNATURE_SIZE_Picnic2_L5_FS, Picnic2_L5_FS, TRANSFORM_FS, NULL_FNS},
    // Picnic with LowMC with m=1
    {LOWMC_L1_1_OR_NULL, 32, 16, 219, 219, 3, 16, 16, 69, 3, 55, 0, 0,
     PICNIC_SIGNATURE_SIZE_Picnic_L1_1_FS, Picnic_L1_1_FS, TRANSFORM_FS, NULL_FNS},
    {LOWMC_L1_1_OR_NULL, 32, 16, 219, 219, 3, 16, 16, 69, 3, 55, 87, 103,
     PICNIC_SIGNATURE_SIZE_Picnic_L1_1_UR, Picnic_L1_1_UR, TRANSFORM_UR, NULL_FNS},
    {LOWMC_L3_1_OR_NULL, 48, 24, 329, 329, 3, 24, 24, 107, 3, 83, 0, 0,
     PICNIC_SIGNATURE_SIZE_Picnic_L3_1_FS, Picnic_L3_1_FS, TRANSFORM_FS, NULL_FNS},
    {LOWMC_L3_1_OR_NULL, 48, 24, 329, 329, 3, 24, 24, 107, 3, 83, 131, 155,
     PICNIC_SIGNATURE_SIZE_Picnic_L3_1_UR, Picnic_L3_1_UR, TRANSFORM_UR, NULL_FNS},
    {LOWMC_L5_1_OR_NULL, 64, 32, 438, 438, 3, 32, 32, 137, 3, 110, 0, 0,
     PICNIC_SIGNATURE_SIZE_Picnic_L5_1_FS, Picnic_L5_1_FS, TRANSFORM_FS, NULL_FNS},
    {LOWMC_L5_1_OR_NULL, 64, 32, 438, 438, 3, 32, 32, 137, 3, 110, 169, 201,
     PICNIC_SIGNATURE_SIZE_Picnic_L5_1_UR, Picnic_L5_1_UR, TRANSFORM_UR, NULL_FNS}};
static bool instance_initialized[PARAMETER_SET_MAX_INDEX];

static const lowmc_t* lowmc_get_instance(unsigned int idx) {
#if defined(MUL_M4RI)
  if (!lowmc_instances_initialized[idx]) {
    if (lowmc_init(lowmc_instances[idx])) {
      lowmc_instances_initialized[idx] = true;
      return lowmc_instances[idx];
    }
    return NULL;
  }
#endif
  return lowmc_instances[idx];
}

#if defined(MUL_M4RI)
static void clear_lowmc_instance(unsigned int idx) {
  if (lowmc_instances_initialized[idx]) {
    lowmc_clear(lowmc_instances[idx]);
    lowmc_instances_initialized[idx] = false;
  }
}
#endif

static bool create_instance(picnic_instance_t* pp, picnic_params_t param) {
  const lowmc_t* lowmc_instance = NULL;

  switch (param) {
  case Picnic_L1_FS:
  case Picnic_L1_UR:
  case Picnic2_L1_FS:
    lowmc_instance = lowmc_get_instance(0);
    break;

  case Picnic_L3_FS:
  case Picnic_L3_UR:
  case Picnic2_L3_FS:
    lowmc_instance = lowmc_get_instance(1);
    break;

  case Picnic_L5_FS:
  case Picnic_L5_UR:
  case Picnic2_L5_FS:
    lowmc_instance = lowmc_get_instance(2);
    break;

  case Picnic_L1_1_FS:
  case Picnic_L1_1_UR:
    lowmc_instance = lowmc_get_instance(3);
    break;

  case Picnic_L3_1_FS:
  case Picnic_L3_1_UR:
    lowmc_instance = lowmc_get_instance(4);
    break;

  case Picnic_L5_1_FS:
  case Picnic_L5_1_UR:
    lowmc_instance = lowmc_get_instance(5);
    break;

  default:
    return false;
  }

  if (!lowmc_instance) {
    return false;
  }

  pp->impls.lowmc                 = lowmc_get_implementation(pp->lowmc);
  pp->impls.lowmc_store           = lowmc_store_get_implementation(pp->lowmc);
  pp->impls.zkbpp_lowmc           = get_zkbpp_lowmc_implementation(pp->lowmc);
  pp->impls.zkbpp_lowmc_verify    = get_zkbpp_lowmc_verify_implementation(pp->lowmc);
  pp->impls.mzd_share             = get_zkbpp_share_implentation(pp->lowmc);
  pp->impls.lowmc_aux             = lowmc_compute_aux_get_implementation(pp->lowmc);
  pp->impls.lowmc_simulate_online = lowmc_simulate_online_get_implementation(pp->lowmc);

  return true;
}

const picnic_instance_t* picnic_instance_get(picnic_params_t param) {
  if (param <= PARAMETER_SET_INVALID || param >= PARAMETER_SET_MAX_INDEX) {
    return NULL;
  }

  if (!instance_initialized[param]) {
    if (!create_instance(&instances[param], param)) {
      return NULL;
    }
    instance_initialized[param] = true;
  }

  return &instances[param];
}

ATTR_DTOR static void clear_instances(void) {
  for (unsigned int p = PARAMETER_SET_INVALID + 1; p < PARAMETER_SET_MAX_INDEX; ++p) {
    if (instance_initialized[p]) {
      instance_initialized[p] = false;
    }
  }

#if defined(MUL_M4RI)
  for (unsigned int i = 0;
       i < sizeof(lowmc_instances_initialized) / sizeof(lowmc_instances_initialized[0]); ++i) {
    clear_lowmc_instance(i);
  }
#endif
}
