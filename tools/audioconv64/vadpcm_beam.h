#pragma once

#include "vadpcm/codec/vadpcm.h"

// Refine an existing VADPCM stream in place using beam search. Reuses the
// codebook and compares predictors, scales, and residual sequences for each
// frame. The predictor order is kVADPCMEncodeOrder (2).
//
// Call after successful vadpcm_encode(), using the same encoding parameters,
// codebook, frame count, and original samples. Process a complete channel
// from its beginning; prediction history starts at zero.
//
// dest must contain classic nine-byte frames. Call before dense residual
// packing or Huffman compression. The codebook and stream size are unchanged.
//
// The score penalizes squared reconstruction error and, weighted by shape,
// squared changes in reconstruction error between consecutive samples.
// A shape of zero uses only squared reconstruction error.
//
// Arguments:
//   params: Original encoding parameters; uses predictor_count and the
//           residual bounds. The dither setting is not used.
//   codebook: Input array of params->predictor_count * kVADPCMEncodeOrder
//             vectors from the original encoding
//   frame_count: Number of frames of VADPCM to refine
//   dest: Input/output array of frame_count * kVADPCMFrameByteSize bytes
//   src: Original input array of frame_count * kVADPCMFrameSampleCount
//        int16_t elements, including any padding used during encoding
//   shape: Finite error-change penalty weight in the range [0, 16]
//
// Error codes:
//   kVADPCMErrInvalidParams: Invalid shape or frame count.
vadpcm_error vadpcm_beam_refine(const struct vadpcm_params *params,
                                const struct vadpcm_vector *codebook,
                                size_t frame_count,
                                void *dest,
                                const int16_t *src,
                                double shape);