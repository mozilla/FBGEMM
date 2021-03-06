/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "fbgemm/FbgemmI8DepthwiseAvx2.h"
#include "fbgemm/Utils.h"

#include <string>
#include <tuple> // for tie

#include "FbgemmI8DepthwiseAvx2-inl.h"

using namespace std;

namespace fbgemm {

template <bool SUM_A = false, bool REMAINDER = false>
static inline ALWAYS_INLINE void inner_prod_3x3_packed_(
    const __m256i* a_v,
    const __m256i* Bp,
    int32_t* C,
    int remainder,
    __m256i* a_sum = nullptr) {
  return inner_prod_packed_<9, SUM_A, REMAINDER>(a_v, Bp, C, remainder, a_sum);
}

template <
    bool SUM_A,
    bool REMAINDER = false,
    bool PER_CHANNEL_QUANTIZATION = false>
static inline ALWAYS_INLINE void inner_prod_3x3_packed_(
    int H,
    int W,
    int K,
    int h_in,
    int w_in,
    const uint8_t* A,
    int32_t A_zero_point,
    const int8_t* Bp,
    const int32_t* B_zero_point,
    int32_t* C,
    int remainder,
    int32_t* row_offsets) {
  __m256i A_zero_point_v = _mm256_set1_epi8(static_cast<uint8_t>(A_zero_point));
  __m256i mask_v = _mm256_setzero_si256();
  if (REMAINDER) {
    mask_v = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(masks[remainder / 4]));
  }

  // The code below can be written as a simple R*S loop but the compiler
  // doesn't unroll so we're manually unrolling it.
  // constexpr int R = 3, S = 3;
  // array<__m256i, R * S> a_v;
  // for (int r = 0; r < R; ++r) {
  //   for (int s = 0; s < S; ++s) {
  //     if (h_in + r >= 0 && h_in + r < H && w_in + s >= 0 && w_in + s < W) {
  //       if (REMAINDER) {
  //         a_v[r * S + s] =
  //             _mm256_maskload_epi32((const int *)(A + (r * W + s) * K),
  //             mask_v);
  //       } else {
  //         a_v[r * S + s] =
  //             _mm256_lddqu_si256((const __m256i *)(A + (r * W + s) * K));
  //       }
  //     } else {
  //       a_v[r * S + s] = A_zero_point_v;
  //     }
  //   }
  // }
  __m256i a_v[9] = {
      A_zero_point_v,
      A_zero_point_v,
      A_zero_point_v,
      A_zero_point_v,
      A_zero_point_v,
      A_zero_point_v,
      A_zero_point_v,
      A_zero_point_v,
      A_zero_point_v,
  };

  if (h_in >= 0 && h_in < H) {
    if (w_in >= 0 && w_in < W) {
      a_v[0] = load_a<REMAINDER>(A + (0 * W + 0) * K, mask_v);
    }
    if (w_in + 1 >= 0 && w_in + 1 < W) {
      a_v[1] = load_a<REMAINDER>(A + (0 * W + 1) * K, mask_v);
    }
    if (w_in + 2 >= 0 && w_in + 2 < W) {
      a_v[2] = load_a<REMAINDER>(A + (0 * W + 2) * K, mask_v);
    }
  }

  if (h_in + 1 >= 0 && h_in + 1 < H) {
    if (w_in >= 0 && w_in < W) {
      a_v[3] = load_a<REMAINDER>(A + (1 * W + 0) * K, mask_v);
    }
    if (w_in + 1 >= 0 && w_in + 1 < W) {
      a_v[4] = load_a<REMAINDER>(A + (1 * W + 1) * K, mask_v);
    }
    if (w_in + 2 >= 0 && w_in + 2 < W) {
      a_v[5] = load_a<REMAINDER>(A + (1 * W + 2) * K, mask_v);
    }
  }

  if (h_in + 2 >= 0 && h_in + 2 < H) {
    if (w_in >= 0 && w_in < W) {
      a_v[6] = load_a<REMAINDER>(A + (2 * W + 0) * K, mask_v);
    }
    if (w_in + 1 >= 0 && w_in + 1 < W) {
      a_v[7] = load_a<REMAINDER>(A + (2 * W + 1) * K, mask_v);
    }
    if (w_in + 2 >= 0 && w_in + 2 < W) {
      a_v[8] = load_a<REMAINDER>(A + (2 * W + 2) * K, mask_v);
    }
  }

  __m256i a_sum[4];
  inner_prod_3x3_packed_<SUM_A, REMAINDER>(
      a_v, reinterpret_cast<const __m256i*>(Bp), C, remainder, a_sum);
  if (SUM_A) {
    __m256i B_zero_point_v;
    for (int i = 0; i < (REMAINDER ? (remainder / 8) : 4); ++i) {
      if (PER_CHANNEL_QUANTIZATION) {
        B_zero_point_v = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(B_zero_point + i * 8));
      } else {
        B_zero_point_v = _mm256_set1_epi32(B_zero_point[0]);
      }
      _mm256_store_si256(
          reinterpret_cast<__m256i*>(&row_offsets[i * 8]),
          _mm256_mullo_epi32(a_sum[i], B_zero_point_v));
    }
  }
}

template <
    bool FUSE_RELU,
    bool HAS_BIAS,
    bool A_SYMMETRIC,
    bool B_SYMMETRIC,
    typename BIAS_TYPE>
static inline ALWAYS_INLINE void depthwise_3x3_kernel_(
    int H,
    int W,
    int K,
    int h,
    int w,
    int stride_h,
    int stride_w,
    int32_t A_zero_point,
    const uint8_t* A,
    int32_t B_zero_point,
    const int8_t* Bp,
    float C_multiplier,
    int32_t C_zero_point,
    int32_t* C_int32,
    uint8_t* C_uint8,
    int32_t* row_offsets,
    const int32_t* col_offsets,
    const BIAS_TYPE* bias,
    float act_times_w_scale) {
  constexpr int S = 3;
  constexpr int PAD_T = 1, PAD_L = 1, PAD_R = 1;
  int W_OUT = (W + PAD_L + PAD_R - S) / stride_w + 1;
  int h_in = -PAD_T + h * stride_h;
  int w_in = -PAD_L + w * stride_w;

  int k;
  for (k = 0; k < K / 32 * 32; k += 32) {
    inner_prod_3x3_packed_<!B_SYMMETRIC /*SUM_A*/>(
        H,
        W,
        K,
        h_in,
        w_in,
        A + (h_in * W + w_in) * K + k,
        A_zero_point,
        Bp + k * 10,
        &B_zero_point,
        C_int32 + k,
        0,
        B_SYMMETRIC ? nullptr : &row_offsets[k]);
  }
  int remainder = K - k;
  if (remainder) {
    inner_prod_3x3_packed_<!B_SYMMETRIC, true>(
        H,
        W,
        K,
        h_in,
        w_in,
        A + (h_in * W + w_in) * K + k,
        A_zero_point,
        Bp + k * 10,
        &B_zero_point,
        C_int32 + k,
        remainder,
        B_SYMMETRIC ? nullptr : &row_offsets[k]);
  }

  requantize_<
      FUSE_RELU,
      HAS_BIAS,
      false, /*PER_CHAN_QUANT*/
      A_SYMMETRIC,
      B_SYMMETRIC,
      BIAS_TYPE>(
      A_zero_point,
      &C_multiplier,
      C_zero_point,
      C_int32,
      C_uint8 + (h * W_OUT + w) * K,
      K,
      row_offsets,
      col_offsets,
      bias,
      &act_times_w_scale);
}

template <bool FUSE_RELU, bool HAS_BIAS, bool A_SYMMETRIC, typename BIAS_TYPE>
static inline ALWAYS_INLINE void
depthwise_3x3_per_channel_quantization_kernel_(
    int H,
    int W,
    int K,
    int h,
    int w,
    int stride_h,
    int stride_w,
    int32_t A_zero_point,
    const uint8_t* A,
    const int32_t* B_zero_point,
    const int8_t* Bp,
    const float* C_multiplier,
    int32_t C_zero_point,
    int32_t* C_int32,
    uint8_t* C_uint8,
    int32_t* row_offsets,
    const int32_t* col_offsets,
    const BIAS_TYPE* bias,
    const float* act_times_w_scale) {
  constexpr int S = 3;
  constexpr int PAD_T = 1, PAD_L = 1, PAD_R = 1;
  int W_OUT = (W + PAD_L + PAD_R - S) / stride_w + 1;
  int h_in = -PAD_T + h * stride_h;
  int w_in = -PAD_L + w * stride_w;

  int k;
  for (k = 0; k < K / 32 * 32; k += 32) {
    inner_prod_3x3_packed_<
        true, /*SUM_A*/
        false, /*remainder*/
        true /*per-channel*/>(
        H,
        W,
        K,
        h_in,
        w_in,
        A + (h_in * W + w_in) * K + k,
        A_zero_point,
        Bp + k * 10,
        B_zero_point + k,
        C_int32 + k,
        0,
        &row_offsets[k]);
  }
  int remainder = K - k;
  if (remainder) {
    inner_prod_3x3_packed_<
        true, /*SUM_A*/
        true, /*remainder*/
        true /*per-channel*/>(
        H,
        W,
        K,
        h_in,
        w_in,
        A + (h_in * W + w_in) * K + k,
        A_zero_point,
        Bp + k * 10,
        B_zero_point + k,
        C_int32 + k,
        remainder,
        &row_offsets[k]);
  }

  requantize_<
      FUSE_RELU,
      HAS_BIAS,
      true, /*PER_CHAN_QUANT*/
      A_SYMMETRIC,
      false, /*B_SYMM*/
      BIAS_TYPE>(
      A_zero_point,
      C_multiplier,
      C_zero_point,
      C_int32,
      C_uint8 + (h * W_OUT + w) * K,
      K,
      row_offsets,
      col_offsets,
      bias,
      act_times_w_scale);
}

// TODO: short-circuit when B_zero_point is 0 or A_zero_point is 0
// This implemntation should be general enough to handle not just 3x3 but other
// filter shapes by parameterizing with R and S but restricting it to just 3x3
// for now.
template <
    bool FUSE_RELU,
    bool HAS_BIAS,
    bool A_SYMMETRIC,
    bool B_SYMMETRIC,
    typename BIAS_TYPE>
static inline ALWAYS_INLINE void depthwise_3x3_pad_1_(
    int N,
    int H,
    int W,
    int K,
    int stride_h,
    int stride_w,
    int32_t A_zero_point,
    const uint8_t* A,
    int32_t B_zero_point,
    const PackedDepthWiseConvMatrix& B,
    float C_multiplier,
    int32_t C_zero_point,
    int32_t* C_int32,
    uint8_t* C_uint8,
    const int32_t* col_offsets,
    const BIAS_TYPE* bias,
    float act_times_w_scale,
    int thread_id,
    int num_threads) {
  assert(K % 8 == 0);
  constexpr int R = 3, S = 3;
  constexpr int PAD_T = 1, PAD_B = 1, PAD_L = 1, PAD_R = 1;
  int H_OUT = (H + PAD_T + PAD_B - R) / stride_h + 1;
  int W_OUT = (W + PAD_L + PAD_R - S) / stride_w + 1;
  const int8_t* Bp = B.PackedMat();

  int32_t* row_offsets = static_cast<int32_t *>(genericAlignedAlloc(((K + 31) / 32 * 32)*sizeof(int32_t), 64));

  int n_begin, n_end;
  int h_begin, h_end, w_begin, w_end;
  if (N >= num_threads) {
    int n_per_thread = (N + num_threads - 1) / num_threads;
    n_begin = std::min(thread_id * n_per_thread, N);
    n_end = std::min(n_begin + n_per_thread, N);
    h_begin = 0;
    h_end = H_OUT;
    w_begin = 0;
    w_end = W_OUT;
  } else {
    int nthreads_per_n = num_threads / N;
    n_begin = std::min(thread_id / nthreads_per_n, N);
    n_end = std::min(n_begin + 1, N);

    int tid_of_n_begin = std::min(n_begin * nthreads_per_n, num_threads);
    int tid_of_n_end = std::min(tid_of_n_begin + nthreads_per_n, num_threads);
    int nthreads_of_n = tid_of_n_end - tid_of_n_begin;
    int tid_within_n = thread_id - tid_of_n_begin;
    assert(tid_within_n >= 0);
    assert(tid_within_n < nthreads_of_n);

    // n is processed by num_threads_h * num_threads_w 2D grid of threads
    int num_threads_h, num_threads_w;
    // num_threads_w <= num_threads_h
    tie(num_threads_w, num_threads_h) = closest_factors_(nthreads_of_n);
    int tid_h = tid_within_n / num_threads_w;
    int tid_w = tid_within_n % num_threads_w;

    int h_per_thread = (H_OUT + num_threads_h - 1) / num_threads_h;
    h_begin = std::min(tid_h * h_per_thread, H_OUT);
    h_end = std::min(h_begin + h_per_thread, H_OUT);

    int w_per_thread = (W_OUT + num_threads_w - 1) / num_threads_w;
    w_begin = std::min(tid_w * w_per_thread, W_OUT);
    w_end = std::min(w_begin + w_per_thread, W_OUT);
  }

  for (int n = n_begin; n < n_end; ++n) {
    const uint8_t* A_base = A + n * H * W * K;
    uint8_t* C_uint8_base = C_uint8 + n * H_OUT * W_OUT * K;

    int h = 0;
    int w = 0;

    if (h_begin == 0) {
      if (w_begin == 0) {
        depthwise_3x3_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            B_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }

      for (w = std::max(1, w_begin); w < std::min(W_OUT - 1, w_end); ++w) {
        depthwise_3x3_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            B_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }

      if (w_end == W_OUT) {
        w = W_OUT - 1;
        depthwise_3x3_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            B_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }
    }

    for (h = std::max(1, h_begin); h < std::min(H - 1, h_end); ++h) {
      if (w_begin == 0) {
        w = 0;
        depthwise_3x3_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            B_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }

      for (w = std::max(1, w_begin); w < std::min(W_OUT - 1, w_end); ++w) {
        depthwise_3x3_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            B_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }

      if (w_end == W_OUT) {
        w = W_OUT - 1;
        depthwise_3x3_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            B_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }
    }

    if (h_end == H_OUT) {
      h = H_OUT - 1;
      w = 0;
      if (w_begin == 0) {
        depthwise_3x3_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            B_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }

      for (w = std::max(1, w_begin); w < std::min(W_OUT - 1, w_end); ++w) {
        depthwise_3x3_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            B_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }

      if (w_end == W_OUT) {
        w = W_OUT - 1;
        depthwise_3x3_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            B_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }
    }
  } // for each n
  genericFree(row_offsets);
};

template <bool FUSE_RELU, bool HAS_BIAS, bool A_SYMMETRIC, typename BIAS_TYPE>
static inline ALWAYS_INLINE void
depthwise_3x3_per_channel_quantization_pad_1_(
    int N,
    int H,
    int W,
    int K,
    int stride_h,
    int stride_w,
    int32_t A_zero_point,
    const uint8_t* A,
    const int32_t* B_zero_point,
    const PackedDepthWiseConvMatrix& B,
    const float* C_multiplier,
    int32_t C_zero_point,
    int32_t* C_int32,
    uint8_t* C_uint8,
    const int32_t* col_offsets,
    const BIAS_TYPE* bias,
    const float* act_times_w_scale,
    int thread_id,
    int num_threads) {
  assert(K % 8 == 0);
  constexpr int R = 3, S = 3;
  constexpr int PAD_T = 1, PAD_B = 1, PAD_L = 1, PAD_R = 1;
  int H_OUT = (H + PAD_T + PAD_B - R) / stride_h + 1;
  int W_OUT = (W + PAD_L + PAD_R - S) / stride_w + 1;
  const int8_t* Bp = B.PackedMat();

  int32_t* row_offsets = static_cast<int32_t*>(genericAlignedAlloc(((K + 31) / 32 * 32)*sizeof(int32_t), 64)); // __attribute__((aligned(64)));

  int n_begin, n_end;
  int h_begin, h_end, w_begin, w_end;
  if (N >= num_threads) {
    int n_per_thread = (N + num_threads - 1) / num_threads;
    n_begin = std::min(thread_id * n_per_thread, N);
    n_end = std::min(n_begin + n_per_thread, N);
    h_begin = 0;
    h_end = H_OUT;
    w_begin = 0;
    w_end = W_OUT;
  } else {
    int nthreads_per_n = num_threads / N;
    n_begin = std::min(thread_id / nthreads_per_n, N);
    n_end = std::min(n_begin + 1, N);

    int tid_of_n_begin = std::min(n_begin * nthreads_per_n, num_threads);
    int tid_of_n_end = std::min(tid_of_n_begin + nthreads_per_n, num_threads);
    int nthreads_of_n = tid_of_n_end - tid_of_n_begin;
    int tid_within_n = thread_id - tid_of_n_begin;
    assert(tid_within_n >= 0);
    assert(tid_within_n < nthreads_of_n);

    // n is processed by num_threads_h * num_threads_w 2D grid of threads
    int num_threads_h, num_threads_w;
    // num_threads_w <= num_threads_h
    tie(num_threads_w, num_threads_h) = closest_factors_(nthreads_of_n);
    int tid_h = tid_within_n / num_threads_w;
    int tid_w = tid_within_n % num_threads_w;

    int h_per_thread = (H_OUT + num_threads_h - 1) / num_threads_h;
    h_begin = std::min(tid_h * h_per_thread, H_OUT);
    h_end = std::min(h_begin + h_per_thread, H_OUT);

    int w_per_thread = (W_OUT + num_threads_w - 1) / num_threads_w;
    w_begin = std::min(tid_w * w_per_thread, W_OUT);
    w_end = std::min(w_begin + w_per_thread, W_OUT);
  }

  for (int n = n_begin; n < n_end; ++n) {
    const uint8_t* A_base = A + n * H * W * K;
    uint8_t* C_uint8_base = C_uint8 + n * H_OUT * W_OUT * K;

    int h = 0;
    int w = 0;

    if (h_begin == 0) {
      if (w_begin == 0) {
        depthwise_3x3_per_channel_quantization_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }

      for (w = std::max(1, w_begin); w < std::min(W_OUT - 1, w_end); ++w) {
        depthwise_3x3_per_channel_quantization_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }

      if (w_end == W_OUT) {
        w = W_OUT - 1;
        depthwise_3x3_per_channel_quantization_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }
    }

    for (h = std::max(1, h_begin); h < std::min(H - 1, h_end); ++h) {
      if (w_begin == 0) {
        w = 0;
        depthwise_3x3_per_channel_quantization_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }

      for (w = std::max(1, w_begin); w < std::min(W_OUT - 1, w_end); ++w) {
        depthwise_3x3_per_channel_quantization_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }

      if (w_end == W_OUT) {
        w = W_OUT - 1;
        depthwise_3x3_per_channel_quantization_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }
    }

    if (h_end == H_OUT) {
      h = H_OUT - 1;
      w = 0;
      if (w_begin == 0) {
        depthwise_3x3_per_channel_quantization_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }

      for (w = std::max(1, w_begin); w < std::min(W_OUT - 1, w_end); ++w) {
        depthwise_3x3_per_channel_quantization_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }

      if (w_end == W_OUT) {
        w = W_OUT - 1;
        depthwise_3x3_per_channel_quantization_kernel_<
            FUSE_RELU,
            HAS_BIAS,
            A_SYMMETRIC,
            BIAS_TYPE>(
            H,
            W,
            K,
            h,
            w,
            stride_h,
            stride_w,
            A_zero_point,
            A_base,
            B_zero_point,
            Bp,
            C_multiplier,
            C_zero_point,
            C_int32,
            C_uint8_base,
            row_offsets,
            col_offsets,
            bias,
            act_times_w_scale);
      }
    }
  } // for each n
};

// Dispatch A_SYMMETRIC and B_SYMMETRIC
template <bool FUSE_RELU, bool HAS_BIAS, typename BIAS_TYPE>
static void depthwise_3x3_pad_1_(
    int N,
    int H,
    int W,
    int K,
    int stride_h,
    int stride_w,
    int32_t A_zero_point,
    const uint8_t* A,
    int32_t B_zero_point,
    const PackedDepthWiseConvMatrix& B,
    float C_multiplier,
    int32_t C_zero_point,
    uint8_t* C,
    const int32_t* col_offsets,
    const BIAS_TYPE* bias,
    float act_times_w_scale,
    int thread_id,
    int num_threads) {
  int32_t* C_int32_temp = new int32_t[(K + 31) / 32 * 32];
  if (A_zero_point == 0 || col_offsets == nullptr) {
    if (B_zero_point == 0) {
      depthwise_3x3_pad_1_<
          FUSE_RELU,
          HAS_BIAS,
          true /*A_symmetric*/,
          true /*B_symmetric*/,
          BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          B,
          C_multiplier,
          C_zero_point,
          C_int32_temp,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else {
      depthwise_3x3_pad_1_<
          FUSE_RELU,
          HAS_BIAS,
          true /*A_symmetric*/,
          false /*B_symmetric*/,
          BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          B,
          C_multiplier,
          C_zero_point,
          C_int32_temp,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    }
  } else {
    if (B_zero_point == 0) {
      depthwise_3x3_pad_1_<
          FUSE_RELU,
          HAS_BIAS,
          false /*A_symmetric*/,
          true /*B_symmetric*/,
          BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          B,
          C_multiplier,
          C_zero_point,
          C_int32_temp,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else {
      depthwise_3x3_pad_1_<
          FUSE_RELU,
          HAS_BIAS,
          false /*A_symmetric*/,
          false /*B_symmetric*/,
          BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          B,
          C_multiplier,
          C_zero_point,
          C_int32_temp,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    }
  }
  delete[] C_int32_temp;
}

// Dispatch HAS_BIAS
template <bool FUSE_RELU, typename BIAS_TYPE>
static void depthwise_3x3_pad_1_(
    int N,
    int H,
    int W,
    int K,
    int stride_h,
    int stride_w,
    int32_t A_zero_point,
    const uint8_t* A,
    int32_t B_zero_point,
    const PackedDepthWiseConvMatrix& B,
    float C_multiplier,
    int32_t C_zero_point,
    uint8_t* C,
    const int32_t* col_offsets,
    const BIAS_TYPE* bias,
    float act_times_w_scale,
    int thread_id,
    int num_threads) {
  if (bias) {
    depthwise_3x3_pad_1_<FUSE_RELU, true /*HAS_BIAS*/, BIAS_TYPE>(
        N,
        H,
        W,
        K,
        stride_h,
        stride_w,
        A_zero_point,
        A,
        B_zero_point,
        B,
        C_multiplier,
        C_zero_point,
        C,
        col_offsets,
        bias,
        act_times_w_scale,
        thread_id,
        num_threads);
  } else {
    depthwise_3x3_pad_1_<FUSE_RELU, false /*HAS_BIAS*/, BIAS_TYPE>(
        N,
        H,
        W,
        K,
        stride_h,
        stride_w,
        A_zero_point,
        A,
        B_zero_point,
        B,
        C_multiplier,
        C_zero_point,
        C,
        col_offsets,
        bias,
        act_times_w_scale,
        thread_id,
        num_threads);
  }
}

// Dispatch input shape and FUSE_RELU
// assumption: W > 3 and H > 3
template <typename BIAS_TYPE>
void depthwise_3x3_pad_1(
    int N,
    int H,
    int W,
    int K,
    int stride_h,
    int stride_w,
    int32_t A_zero_point,
    const uint8_t* A,
    int32_t B_zero_point,
    const PackedDepthWiseConvMatrix& B,
    float C_multiplier,
    int32_t C_zero_point,
    uint8_t* C,
    const int32_t* col_offsets,
    const BIAS_TYPE* bias,
    bool fuse_relu,
    float act_times_w_scale,
    int thread_id,
    int num_threads) {
  if (B.GetKernelProduct() != 3 * 3) {
    string msg =
        "[FBGEMM_CONV_ERROR] Packed weight is expected to have kernel_prod " +
        to_string(3 * 3) + " but has " + to_string(B.GetKernelProduct());
    throw logic_error(msg);
  }
  if (stride_h == 0 || stride_w == 0 || num_threads == 0) {
    assert(0 && "stride_h == 0 || stride_w == 0 || num_threads == 0");
    return;
  }
  if (N == 0) {
    // In C2, batch size 0 is allowed, so we should just early return.
    return;
  }
  if (fuse_relu) {
    if (7 == H && 7 == W && 1 == stride_h && 1 == stride_w) {
      depthwise_3x3_pad_1_<true /* FUSE_RELU */, BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          B,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else if (14 == H && 14 == W && 2 == stride_h && 2 == stride_w) {
      depthwise_3x3_pad_1_<true /* FUSE_RELU */, BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          B,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else if (1 == stride_h && 1 == stride_w) {
      depthwise_3x3_pad_1_<true /* FUSE_RELU */, BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          B,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else if (2 == stride_h && 2 == stride_w) {
      depthwise_3x3_pad_1_<true /* FUSE_RELU */, BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          B,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else {
      depthwise_3x3_pad_1_<true /* FUSE_RELU */, BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          B,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    }
  } else {
    if (7 == H && 7 == W && 1 == stride_h && 1 == stride_w) {
      depthwise_3x3_pad_1_<false /* FUSE_RELU */, BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          B,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else if (14 == H && 14 == W && 2 == stride_h && 2 == stride_w) {
      depthwise_3x3_pad_1_<false /* FUSE_RELU */, BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          B,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else if (1 == stride_h && 1 == stride_w) {
      depthwise_3x3_pad_1_<false /* FUSE_RELU */, BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          B,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else if (2 == stride_h && 2 == stride_w) {
      depthwise_3x3_pad_1_<false /* FUSE_RELU */, BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          B,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else {
      depthwise_3x3_pad_1_<false /* FUSE_RELU */, BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          B,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    }
  }
}

// Dispatch A_SYMMETRIC
template <bool FUSE_RELU, bool HAS_BIAS, typename BIAS_TYPE>
static void depthwise_3x3_per_channel_quantization_pad_1_(
    int N,
    int H,
    int W,
    int K,
    int stride_h,
    int stride_w,
    int32_t A_zero_point,
    const uint8_t* A,
    const int32_t* B_zero_point,
    const PackedDepthWiseConvMatrix& Bp,
    const float* C_multiplier,
    int32_t C_zero_point,
    uint8_t* C,
    const int32_t* col_offsets,
    const BIAS_TYPE* bias,
    const float* act_times_w_scale,
    int thread_id,
    int num_threads) {
  int32_t* C_int32_temp = new int32_t[(K + 31) / 32 * 32];
  if (A_zero_point == 0 || col_offsets == nullptr) {
    depthwise_3x3_per_channel_quantization_pad_1_<
        FUSE_RELU,
        HAS_BIAS,
        true /*A_SYMM*/,
        BIAS_TYPE>(
        N,
        H,
        W,
        K,
        stride_h,
        stride_w,
        A_zero_point,
        A,
        B_zero_point,
        Bp,
        C_multiplier,
        C_zero_point,
        C_int32_temp,
        C,
        col_offsets,
        bias,
        act_times_w_scale,
        thread_id,
        num_threads);
  } else {
    depthwise_3x3_per_channel_quantization_pad_1_<
        FUSE_RELU,
        HAS_BIAS,
        false /*A_SYMM*/,
        BIAS_TYPE>(
        N,
        H,
        W,
        K,
        stride_h,
        stride_w,
        A_zero_point,
        A,
        B_zero_point,
        Bp,
        C_multiplier,
        C_zero_point,
        C_int32_temp,
        C,
        col_offsets,
        bias,
        act_times_w_scale,
        thread_id,
        num_threads);
  }
  delete[] C_int32_temp;
}

// Dispatch HAS_BIAS
template <bool FUSE_RELU, typename BIAS_TYPE>
static void depthwise_3x3_per_channel_quantization_pad_1_(
    int N,
    int H,
    int W,
    int K,
    int stride_h,
    int stride_w,
    int32_t A_zero_point,
    const uint8_t* A,
    const int32_t* B_zero_point,
    const PackedDepthWiseConvMatrix& Bp,
    const float* C_multiplier,
    int32_t C_zero_point,
    uint8_t* C,
    const int32_t* col_offsets,
    const BIAS_TYPE* bias,
    const float* act_times_w_scale,
    int thread_id,
    int num_threads) {
  if (bias) {
    depthwise_3x3_per_channel_quantization_pad_1_<
        FUSE_RELU,
        true /* HAS_BIAS */,
        BIAS_TYPE>(
        N,
        H,
        W,
        K,
        stride_h,
        stride_w,
        A_zero_point,
        A,
        B_zero_point,
        Bp,
        C_multiplier,
        C_zero_point,
        C,
        col_offsets,
        bias,
        act_times_w_scale,
        thread_id,
        num_threads);
  } else {
    depthwise_3x3_per_channel_quantization_pad_1_<
        FUSE_RELU,
        false /* HAS_BIAS */,
        BIAS_TYPE>(
        N,
        H,
        W,
        K,
        stride_h,
        stride_w,
        A_zero_point,
        A,
        B_zero_point,
        Bp,
        C_multiplier,
        C_zero_point,
        C,
        col_offsets,
        bias,
        act_times_w_scale,
        thread_id,
        num_threads);
  }
}

// Dispatch input shape and FUSE_RELU
template <typename BIAS_TYPE>
void depthwise_3x3_per_channel_quantization_pad_1(
    int N,
    int H,
    int W,
    int K,
    int stride_h,
    int stride_w,
    int32_t A_zero_point,
    const uint8_t* A,
    const int32_t* B_zero_point,
    const PackedDepthWiseConvMatrix& Bp,
    const float* C_multiplier,
    int32_t C_zero_point,
    uint8_t* C,
    const int32_t* col_offsets,
    const BIAS_TYPE* bias,
    bool fuse_relu,
    const float* act_times_w_scale,
    int thread_id,
    int num_threads) {
  if (Bp.GetKernelProduct() != 3 * 3) {
    string msg =
        "[FBGEMM_CONV_ERROR] Packed weight is expected to have kernel_prod " +
        to_string(3 * 3) + " but has " + to_string(Bp.GetKernelProduct());
    throw logic_error(msg);
  }
  if (stride_h == 0 || stride_w == 0 || num_threads == 0) {
    assert(0 && "stride_h == 0 || stride_w == 0 || num_threads == 0");
    return;
  }
  if (N == 0) {
    // In C2, batch size 0 is allowed, so we should just early return.
    return;
  }
  if (fuse_relu) {
    if (7 == H && 7 == W && 1 == stride_h && 1 == stride_w) {
      depthwise_3x3_per_channel_quantization_pad_1_<
          true /* FUSE_RELU */,
          BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          Bp,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else if (14 == H && 14 == W && 2 == stride_h && 2 == stride_w) {
      depthwise_3x3_per_channel_quantization_pad_1_<
          true /* FUSE_RELU */,
          BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          Bp,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else if (1 == stride_h && 1 == stride_w) {
      depthwise_3x3_per_channel_quantization_pad_1_<
          true /* FUSE_RELU */,
          BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          Bp,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else if (2 == stride_h && 2 == stride_w) {
      depthwise_3x3_per_channel_quantization_pad_1_<
          true /* FUSE_RELU */,
          BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          Bp,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else {
      depthwise_3x3_per_channel_quantization_pad_1_<
          true /* FUSE_RELU */,
          BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          Bp,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    }
  } else {
    if (7 == H && 7 == W && 1 == stride_h && 1 == stride_w) {
      depthwise_3x3_per_channel_quantization_pad_1_<
          false /* FUSE_RELU */,
          BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          Bp,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else if (14 == H && 14 == W && 2 == stride_h && 2 == stride_w) {
      depthwise_3x3_per_channel_quantization_pad_1_<
          false /* FUSE_RELU */,
          BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          Bp,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else if (1 == stride_h && 1 == stride_w) {
      depthwise_3x3_per_channel_quantization_pad_1_<
          false /* FUSE_RELU */,
          BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          Bp,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else if (2 == stride_h && 2 == stride_w) {
      depthwise_3x3_per_channel_quantization_pad_1_<
          false /* FUSE_RELU */,
          BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          Bp,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    } else {
      depthwise_3x3_per_channel_quantization_pad_1_<
          false /* FUSE_RELU */,
          BIAS_TYPE>(
          N,
          H,
          W,
          K,
          stride_h,
          stride_w,
          A_zero_point,
          A,
          B_zero_point,
          Bp,
          C_multiplier,
          C_zero_point,
          C,
          col_offsets,
          bias,
          act_times_w_scale,
          thread_id,
          num_threads);
    }
  }
}

// To be removed
void depthwise_3x3_pad_1(
    int N,
    int H,
    int W,
    int K,
    int stride_h,
    int stride_w,
    int32_t A_zero_point,
    const uint8_t* A,
    int32_t B_zero_point,
    const PackedDepthWiseConvMatrix& B,
    float C_multiplier,
    int32_t C_zero_point,
    uint8_t* C,
    const int32_t* col_offsets,
    const int32_t* bias,
    bool fuse_relu,
    int thread_id,
    int num_threads) {
  depthwise_3x3_pad_1<std::int32_t>(
      N,
      H,
      W,
      K,
      stride_h,
      stride_w,
      A_zero_point,
      A,
      B_zero_point,
      B,
      C_multiplier,
      C_zero_point,
      C,
      col_offsets,
      bias,
      fuse_relu,
      1.0f,
      thread_id,
      num_threads);
}

// To be removed
void depthwise_3x3_per_channel_quantization_pad_1(
    int N,
    int H,
    int W,
    int K,
    int stride_h,
    int stride_w,
    int32_t A_zero_point,
    const uint8_t* A,
    const int32_t* B_zero_point,
    const PackedDepthWiseConvMatrix& Bp,
    const float* C_multiplier,
    int32_t C_zero_point,
    uint8_t* C,
    const int32_t* col_offsets,
    const int32_t* bias,
    bool fuse_relu,
    int thread_id,
    int num_threads) {
  depthwise_3x3_per_channel_quantization_pad_1<std::int32_t>(
      N,
      H,
      W,
      K,
      stride_h,
      stride_w,
      A_zero_point,
      A,
      B_zero_point,
      Bp,
      C_multiplier,
      C_zero_point,
      C,
      col_offsets,
      bias,
      fuse_relu,
      nullptr,
      thread_id,
      num_threads);
}

template void depthwise_3x3_pad_1(
    int N,
    int H,
    int W,
    int K,
    int stride_h,
    int stride_w,
    int32_t A_zero_point,
    const uint8_t* A,
    int32_t B_zero_point,
    const PackedDepthWiseConvMatrix& B,
    float C_multiplier,
    int32_t C_zero_point,
    uint8_t* C,
    const int32_t* col_offsets,
    const int32_t* bias,
    bool fuse_relu,
    float act_times_w_scale,
    int thread_id,
    int num_threads);

template void depthwise_3x3_pad_1(
    int N,
    int H,
    int W,
    int K,
    int stride_h,
    int stride_w,
    int32_t A_zero_point,
    const uint8_t* A,
    int32_t B_zero_point,
    const PackedDepthWiseConvMatrix& B,
    float C_multiplier,
    int32_t C_zero_point,
    uint8_t* C,
    const int32_t* col_offsets,
    const float* bias,
    bool fuse_relu,
    float act_times_w_scale,
    int thread_id,
    int num_threads);

template void depthwise_3x3_per_channel_quantization_pad_1(
    int N,
    int H,
    int W,
    int K,
    int stride_h,
    int stride_w,
    int32_t A_zero_point,
    const uint8_t* A,
    const int32_t* B_zero_point,
    const PackedDepthWiseConvMatrix& Bp,
    const float* C_multiplier,
    int32_t C_zero_point,
    uint8_t* C,
    const int32_t* col_offsets,
    const int32_t* bias,
    bool fuse_relu,
    const float* act_times_w_scale,
    int thread_id,
    int num_threads);

template void depthwise_3x3_per_channel_quantization_pad_1(
    int N,
    int H,
    int W,
    int K,
    int stride_h,
    int stride_w,
    int32_t A_zero_point,
    const uint8_t* A,
    const int32_t* B_zero_point,
    const PackedDepthWiseConvMatrix& Bp,
    const float* C_multiplier,
    int32_t C_zero_point,
    uint8_t* C,
    const int32_t* col_offsets,
    const float* bias,
    bool fuse_relu,
    const float* act_times_w_scale,
    int thread_id,
    int num_threads);

} // namespace fbgemm
