/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 * Implemented by BoxingBruin.
 */

#include "beam.h"

#include <stdint.h>
#include <string.h>
#include <math.h>

enum {
    // Fractional bits in the codebook weights; stored weights are scaled by 2048.
    kVADPCMBeamFractionBits = 11,

    // Maximum number of competing sequences kept after each sample.
    kVADPCMBeamWidth = 8,

    // Maximum allowed weight for penalizing changes in reconstruction error.
    kVADPCMBeamMaxShape = 16,
};

// One possible encoding of a frame, built one sample at a time.
struct vadpcm_beam_candidate {
    // Order tried, used to break ties between equal scores.
    int order;

    // Total error penalty for the samples processed so far.
    double cost;

    // Running predictions, stored at the codebook's fixed-point scale.
    int64_t accumulator[kVADPCMVectorSampleCount];

    // Audio sample values produced by this sequence.
    int16_t reconstructed[kVADPCMFrameSampleCount];

    // Residual choices made for this sequence.
    int residuals[kVADPCMFrameSampleCount];
};

static int16_t vadpcm_beam_clamp(int64_t sample)
{
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }

    if (sample > INT16_MAX) {
        return INT16_MAX;
    }

    return sample;
}

// Combine a prediction and scaled residual to calculate the sample this choice produces.
static int16_t vadpcm_beam_reconstruct(int64_t accumulator,
                                       int residual,
                                       int scale) {
    // Multiply the residual by the frame's power-of-two scale factor.
    const int64_t correction = (int64_t)residual * (1 << scale);

    // Add the correction to the prediction at matching fixed-point scale,
    // then shift back to audio sample units.
    const int64_t sample = (accumulator + correction * (1 << kVADPCMBeamFractionBits))
                            >> kVADPCMBeamFractionBits;

    return vadpcm_beam_clamp(sample);
}

//Score a sample's reconstruction error and how much that error changed from the previous sample.
static double vadpcm_beam_sample_cost(int16_t reconstructed,
                                      int16_t original,
                                      double previous_error,
                                      double shape) {

    // Measure how far this candidates sample is from the original.
    const double error = (double)reconstructed - original;
    const double change = error - previous_error;

    return error * error + shape * change * change;
}

// Start one sequence using the two history samples and initialize its prediction accumulators.
static void vadpcm_beam_start(struct vadpcm_beam_candidate *candidate,
                              const struct vadpcm_vector *predictor,
                              const int16_t history[2]){
    memset(candidate, 0, sizeof(*candidate));

    for (int i = 0; i < kVADPCMVectorSampleCount; ++i) {
        candidate->accumulator[i] = (int64_t)history[0] * predictor[0].v[i] +
                                     (int64_t)history[1] * predictor[1].v[i];
    }
}

// Add this correction's weighted contributions to the predictions for later samples in the group.
static void vadpcm_beam_propogate(struct vadpcm_beam_candidate *candidate,
                                  const struct vadpcm_vector *predictor,
                                  int position,
                                  int residual,
                                  int scale) {

    // Multiply the residual by the frame's power-of-two scale factor.
    const int64_t correction = (int64_t)residual * (1 << scale);

    // Update predictions for the remaining samples in this group.
    for (int i = position + 1; i < kVADPCMVectorSampleCount; ++i) {
        candidate->accumulator[i] += correction * predictor[1].v[i - position - 1];
    }
}

// Use the last two reconstructed samples and the selected prediction rule
// to prepare predictions for the next eight samples.
static void vadpcm_beam_next_vector(struct vadpcm_beam_candidate *candidate,
                                    const struct vadpcm_vector *predictor) {
    const int16_t older = candidate->reconstructed[kVADPCMVectorSampleCount - 2];
    const int16_t recent = candidate->reconstructed[kVADPCMVectorSampleCount - 1];

    for (int i = 0; i < kVADPCMVectorSampleCount; ++i) {
        candidate->accumulator[i] = (int64_t)older * predictor[0].v[i] +
                                    (int64_t)recent * predictor[1].v[i];
    }
}

// Add one residual choice to a candidate, record its reconstructed sample, score it, and update later predictions.
static void vadpcm_beam_extend(struct vadpcm_beam_candidate *candidate,
                               const struct vadpcm_vector *predictor,
                               const int16_t original[kVADPCMFrameSampleCount],
                               int sample_index,
                               int residual,
                               int scale,
                               double previous_frame_error,
                               double shape) {

    const int position = sample_index % kVADPCMVectorSampleCount;

    // Penalize sudden changes in reconstruction error.
    // Get the previous sample's error so we can compare it with this sample's.
    const double previous_error = sample_index == 0
                                  ? previous_frame_error
                                  : (double)candidate->reconstructed[sample_index -1] -
                                  original[sample_index -1];

    const int16_t reconstructed = vadpcm_beam_reconstruct(candidate->accumulator[position],
                                                          residual,
                                                          scale);

    // Store the residual and its reconstructed sample as part of this sequence.
    candidate->residuals[sample_index] = residual;
    candidate->reconstructed[sample_index] = reconstructed;

    // Add this sample's error penalty to the sequence's total score.
    candidate->cost += vadpcm_beam_sample_cost(reconstructed,
                                        original[sample_index],
                                               previous_error,
                                               shape);

    // Apply this correction to later predictions in the group using the predictor's weights.
    vadpcm_beam_propogate(candidate, predictor, position, residual, scale);
}

// Compare two candidates by score.
static int vadpcm_beam_worse(const struct vadpcm_beam_candidate *a,
                             const struct vadpcm_beam_candidate *b) {

    return a->cost > b->cost || (a->cost == b->cost && a->order > b->order);
}

// Move an index down the heap until worse candidates are above better ones again.
static void vadpcm_beam_sift(const struct vadpcm_beam_candidate *candidates,
                              int *heap,
                              int count,
                              int at) {
    const int value = heap[at];

    // Continue while this position has at least one child entry to compare.
    while (at * 2 + 1 < count) {
        int child = at * 2 + 1;

        if (child + 1 < count &&
            vadpcm_beam_worse(&candidates[heap[child + 1]],&candidates[heap[child]])) {
            child++;
        }

        if (!vadpcm_beam_worse(
            &candidates[heap[child]],
            &candidates[value])) {
            break;
        }

        heap[at] = heap[child];
        at = child;
    }

    heap[at] = value;
}

// Keep a candidate if there is room, or replace the worst candidate if this one is better.
static void vadpcm_beam_keep(struct vadpcm_beam_candidate *candidates,
                             int *heap,
                             int *count,
                             const struct vadpcm_beam_candidate *candidate) {

    // If there is an empty slot, keep this candidate without discarding.
    if (*count < kVADPCMBeamWidth) {
        const int slot = *count;
        int at = (*count)++;

        candidates[slot] = *candidate;

        // Move the new candidate's index toward heap[0] while it is worse than the entry above it.
        while (at > 0 && vadpcm_beam_worse(candidate, &candidates[heap[(at - 1) / 2]])) {
            heap[at] = heap[(at - 1) / 2];
            at = (at - 1) / 2;
        }

        heap[at] = slot;
    }else {
        // Discard the new candidate unless it beats the worst one.
        if (!vadpcm_beam_worse(&candidates[heap[0]],candidate)) {
            return;
        }

        // Replace the worst candidate with this better one.
        candidates[heap[0]] = *candidate;
        // Move the replacement's index away from heap[0] if there are worse candidates.
        vadpcm_beam_sift(candidates, heap, *count, 0);
    }
}

// Copy the kept candidates into current, ordered from best to worst.
static void vadpcm_beam_order(struct vadpcm_beam_candidate *current,
                              const struct vadpcm_beam_candidate *next,
                              int *heap,
                              int count) {
    for (int n = count; n > 0; n--) {
        current[n - 1] = next[heap[0]];
        heap[0] = heap[n - 1];

        if (n > 1) {
            vadpcm_beam_sift(next, heap, n -1, 0);
        }
    }
}

// Compare residual sequences for one frame using a fixed predictor and scale, keeping several alternatives at each sample.
static int vadpcm_beam_search(struct vadpcm_beam_candidate *best,
                              const struct vadpcm_vector *predictor,
                              const int16_t original[kVADPCMFrameSampleCount],
                              const int16_t history[2],
                              int scale,
                              int min_residual,
                              int max_residual,
                              double previous_frame_error,
                              double shape,
                              double best_cost) {
    // Sequences kept after the previous sample.
    struct vadpcm_beam_candidate current[kVADPCMBeamWidth];
    // Best sequences after adding a residual for the current sample.
    struct vadpcm_beam_candidate next[kVADPCMBeamWidth];

    // Number of possible residual values.
    const int choices = max_residual - min_residual + 1;

    // Start one sequence using the previous frame's final two samples.
    vadpcm_beam_start(&current[0], predictor, history);
    int count = 1;

    for (int sample = 0; sample < kVADPCMFrameSampleCount; sample++) {
        // Track candidate positions so we can replace the worst one.
        int heap[kVADPCMBeamWidth];
        int next_count = 0;

        // Extend each sequence.
        for (int parent = 0; parent < count; parent++) {
            // Preserve existing sequence while trying different continuations.
            struct vadpcm_beam_candidate base = current[parent];

            if (sample == kVADPCMVectorSampleCount) {
                vadpcm_beam_next_vector(&base, predictor);
            }

            // Try each possible residual.
            for (int residual = min_residual; residual <= max_residual; residual++) {
                struct vadpcm_beam_candidate candidate = base;

                // Default to earlier candidate when two candidates have equal cost.
                candidate.order = parent * choices + residual - min_residual;

                vadpcm_beam_extend(&candidate, predictor, original, sample, residual,
                                   scale, previous_frame_error, shape);

                // Keep this candidate if there is room, or if it beats the worst one kept.
                vadpcm_beam_keep(next, heap, &next_count, &candidate);
            }
        }

        // Order from best to worst, current[0] being best.
        vadpcm_beam_order(current, next, heap, next_count);
        count = next_count;

        // Skip if we can not beat the best frame we have found so far.
        if (current[0].cost >= best_cost) {
            return 0;
        }
    }

    *best = current[0];
    return 1;
}

// Refine each encoded frame by comparing predictors, scales, and residual sequences, then write the winning encoding back.
vadpcm_error vadpcm_beam_refine(const struct vadpcm_params *params,
                                const struct vadpcm_vector *codebook,
                                size_t frame_count,
                                void *dest,
                                const int16_t *src,
                                double shape) {
    if (!isfinite(shape) || shape < 0.0 || shape > kVADPCMBeamMaxShape) {
        return kVADPCMErrInvalidParams;
    }

    if (frame_count == 0) {
        return kVADPCMErrNone;
    }

    if (frame_count > SIZE_MAX / (kVADPCMFrameSampleCount * sizeof(*src))) {
        return kVADPCMErrInvalidParams;
    }

    uint8_t *frames = dest;
    int16_t history[2] = {0,0};
    double previous_frame_error = 0.0;

    for (size_t frame = 0; frame < frame_count; frame++) {
        // This frame's encoded bytes and its original audio samples.
        uint8_t *encoded = frames + frame * kVADPCMFrameByteSize;
        const int16_t *original = src + frame * kVADPCMFrameSampleCount;

        // Use the original encoder's scale as the starting point.
        const int initial_scale = encoded[0] >> 4;

        struct vadpcm_beam_candidate best = {.cost = INFINITY};
        int best_predictor = 0;
        int best_scale = initial_scale;

        // Compare every predictor at scales within two steps of the starting point.
        for (int predictor = 0; predictor < params->predictor_count; predictor++) {
            for (int scale = initial_scale - 2; scale <= initial_scale + 2; scale++) {
                if (scale < 0 || scale > 15) {
                    continue;
                }

                struct vadpcm_beam_candidate candidate;

                if (vadpcm_beam_search(&candidate, codebook + predictor * kVADPCMEncodeOrder,
                                       original, history, scale,
                                       params->min_residual, params->max_residual,
                                       previous_frame_error, shape, best.cost)) {
                    best = candidate;
                    best_predictor = predictor;
                    best_scale = scale;
                }
            }
        }

        // Store the winning scale and predictor.
        encoded[0] = (uint8_t)((best_scale << 4) | best_predictor);

        // Pack each pair of residuals.
        for (int i = 0; i < kVADPCMFrameSampleCount / 2; i++) {
            encoded[1 + i] = (uint8_t)((((unsigned)best.residuals[2 * i] & 15u) << 4) |
                                        ((unsigned)best.residuals[2 * i + 1] & 15u));
        }

        // Use this frame's final two reconstructed samples as the next frame's history.
        history[0] = best.reconstructed[kVADPCMFrameSampleCount - 2];
        history[1] = best.reconstructed[kVADPCMFrameSampleCount - 1];

        // Carry the final sample's error forward for scoring.
        previous_frame_error = (double)history[1] - original[kVADPCMFrameSampleCount - 1];
    }
    return kVADPCMErrNone;
}