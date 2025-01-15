#define TORCH_ASSERT_ONLY_METHOD_OPERATORS
#include <ATen/core/Tensor.h>

#include <ATen/Dispatch.h>
#include <ATen/Parallel.h>
#include <ATen/cpu/vec/functional.h>
#include <ATen/cpu/vec/vec.h>
#include <ATen/native/CPUBlas.h>
#include <ATen/native/cpu/int_mm_kernel.h>
#include <ATen/native/cpu/utils.h>
#include <c10/util/Unroll.h>
#include <c10/util/irange.h>

#if (defined(_WIN32) || defined(_WIN64))
#define RESTRICT __restrict
#else
#define RESTRICT __restrict__
#endif

#if AT_MKLDNN_ENABLED()
#include <ideep.hpp>
// Add uKernel API versioning to be compatible with different oneDNN versions
// oneDNN 3.6.x updates the ukernel APIs of brgemm and brgemm_pack_B
// brgemm_pack_B is changed to transform and the setting of brgemm beta is
// changed to set_add_C
#if (IDEEP_VERSION_MAJOR == 3 && IDEEP_VERSION_MINOR == 5)
#define ONEDNN_UKERNEL_1
#elif (IDEEP_VERSION_MAJOR >= 3 && IDEEP_VERSION_MINOR >= 6)
#define ONEDNN_UKERNEL_2
#endif
#if (                                                           \
    (defined(ONEDNN_UKERNEL_1) || defined(ONEDNN_UKERNEL_2)) && \
    (defined(__x86_64__) || (defined(_M_X64) && !defined(_M_ARM64EC))))
#define ONEDNN_UKERNEL_ENABLED
#endif
#endif // AT_MKLDNN_ENABLED()
#if defined(ONEDNN_UKERNEL_ENABLED)
#include <oneapi/dnnl/dnnl.hpp>
#include <oneapi/dnnl/dnnl_ukernel.hpp>
#endif // oneDNN BRGEMM

namespace at::native {

namespace {

template <typename T>
void int8pack_mm_kernel_(
    const Tensor& C,
    const Tensor& A,
    const Tensor& B,
    const Tensor& scales);

#if defined(CPU_CAPABILITY_AVX512) && !defined(_MSC_VER)

static inline void transpose_16x16_fp32(__m512 a[16]) {
  __m512 t[16];
  c10::ForcedUnroll<8>{}([&](auto i) {
    t[i] = _mm512_unpacklo_ps(a[2 * i], a[2 * i + 1]);
    t[i + 8] = _mm512_unpackhi_ps(a[2 * i], a[2 * i + 1]);
  });
  c10::ForcedUnroll<8>{}([&](auto i) {
    a[i] = (__m512)_mm512_unpacklo_pd((__m512d)t[i * 2], (__m512d)t[i * 2 + 1]);
    a[i + 8] =
        (__m512)_mm512_unpackhi_pd((__m512d)t[i * 2], (__m512d)t[i * 2 + 1]);
  });
  c10::ForcedUnroll<8>{}([&](auto i) {
    t[2 * i] = _mm512_shuffle_f32x4(a[2 * i], a[2 * i + 1], 0x44);
    t[2 * i + 1] = _mm512_shuffle_f32x4(a[2 * i], a[2 * i + 1], 0xee);
  });
  a[0] = _mm512_shuffle_f32x4(t[0], t[2], 0x88);
  a[4] = _mm512_shuffle_f32x4(t[0], t[2], 0xdd);
  a[8] = _mm512_shuffle_f32x4(t[1], t[3], 0x88);
  a[12] = _mm512_shuffle_f32x4(t[1], t[3], 0xdd);
  a[2] = _mm512_shuffle_f32x4(t[4], t[6], 0x88);
  a[6] = _mm512_shuffle_f32x4(t[4], t[6], 0xdd);
  a[10] = _mm512_shuffle_f32x4(t[5], t[7], 0x88);
  a[14] = _mm512_shuffle_f32x4(t[5], t[7], 0xdd);
  a[1] = _mm512_shuffle_f32x4(t[8], t[10], 0x88);
  a[5] = _mm512_shuffle_f32x4(t[8], t[10], 0xdd);
  a[9] = _mm512_shuffle_f32x4(t[9], t[11], 0x88);
  a[13] = _mm512_shuffle_f32x4(t[11], t[11], 0xdd);
  a[3] = _mm512_shuffle_f32x4(t[12], t[14], 0x88);
  a[7] = _mm512_shuffle_f32x4(t[12], t[14], 0xdd);
  a[11] = _mm512_shuffle_f32x4(t[13], t[15], 0x88);
  a[15] = _mm512_shuffle_f32x4(t[13], t[15], 0xdd);
}

template <int NUM>
void dequant_and_unpack(
    const int8_t* B,
    BFloat16* B_unpack,
    const BFloat16* scales,
    const int K,
    const int ldb_unpack);

template <>
void dequant_and_unpack<1>(
    const int8_t* B,
    BFloat16* B_unpack,
    const BFloat16* scales,
    const int K,
    const int ldb_unpack) {
  for (int k = 0; k < K; k++) {
    int8_t b8 = B[k];
    B_unpack[k * ldb_unpack] = static_cast<BFloat16>(b8) * scales[0];
  }
}

template <>
void dequant_and_unpack<16>(
    const int8_t* B,
    BFloat16* B_unpack,
    const BFloat16* scales,
    const int K,
    const int ldb_unpack) {
  const int ldb = K;
  __m512 scale[16];
  c10::ForcedUnroll<16>{}([&](auto i) {
    float ss = static_cast<float>(scales[i]);
    scale[i] = _mm512_set1_ps(ss);
  });
  for (int k = 0; k < K; k += 16) {
    int kk = std::min(k, K - 16);
    __m512 vb[16];
    c10::ForcedUnroll<16>{}([&](auto i) {
      __m128i b8 = _mm_load_si128((__m128i*)(B + kk + i * ldb));
      __m512i b32 = _mm512_cvtepi8_epi32(b8);
      vb[i] = _mm512_cvtepi32_ps(b32);
      vb[i] = _mm512_mul_ps(vb[i], scale[i]);
    });
    transpose_16x16_fp32(vb);
    c10::ForcedUnroll<16>{}([&](auto i) {
      _mm256_storeu_epi16(
          (void*)(B_unpack + (kk + i) * ldb_unpack), vec::cvtfp32_bf16(vb[i]));
    });
  }
}

template <>
void int8pack_mm_kernel_<BFloat16>(
    const Tensor& C,
    const Tensor& A,
    const Tensor& B,
    const Tensor& scales) {
  const BFloat16* A_data = A.const_data_ptr<BFloat16>();
  const int8_t* B_data = B.const_data_ptr<int8_t>();
  BFloat16* C_data = C.data_ptr<BFloat16>();
  const BFloat16* S_data = scales.const_data_ptr<BFloat16>();

  int M = A.size(0);
  int N = B.size(0);
  int K = A.size(1);
  int lda = K;
  int ldb = K;
  int ldc = N;
  BFloat16* B_unpack = (BFloat16*)malloc(N * K * sizeof(BFloat16));
  int thread_num = get_num_threads();
  int n = std::max(16, N / thread_num + (bool)(N % thread_num));
  int L2_cache = 2 * 1024 * 1024;

  // auto brg = dnnl::ukernel::brgemm(
  //     M,
  //     16,
  //     K,
  //     1,
  //     lda,
  //     N,
  //     ldc,
  //     dnnl::memory::data_type::bf16,
  //     dnnl::memory::data_type::bf16,
  //     dnnl::memory::data_type::bf16,
  //     1,
  //     0);
  // brg.generate();

  at::parallel_for(0, N / n + (bool)(N % n), 0, [&](int begin, int end) {
    // brg.set_hw_context();
    int local_begin = begin * n;
    int local_n = std::min(n, N - local_begin);
    const int8_t* B_local = B_data + local_begin * ldb;
    BFloat16* B_unpack_local = B_unpack + local_begin;
    const BFloat16* scales_local = S_data + local_begin;
    // std::vector<uint8_t> scratchpad(brg.get_scratchpad_size());
    if (local_n >= 16 && K >= 16) {
      int local_k = (L2_cache - M * local_n * sizeof(BFloat16)) /
          (M + local_n) / sizeof(BFloat16) / 16 * 16;
      local_k = std::min(K, local_k);
      for (int i = 0; i < local_n; i += 16) {
        int ii = std::min(i, local_n - 16);
        dequant_and_unpack<16>(
            B_local + ii * ldb, B_unpack_local + ii, scales_local + ii, K, N);
      }
      // for (int i = 0; i < local_n; i += 16) {
      //   int ii = std::min(i, local_n - 16);
      //   printf("ii:%d\n", ii);
      //   brg.execute(
      //       A_data,
      //       B_unpack_local + ii,
      //       {{0, 0}},
      //       C_data + ii,
      //       scratchpad.data());
      // }
      // printf("%d brgemm done\n", begin);
    } else {
      for (int i = 0; i < local_n; i++)
        dequant_and_unpack<1>(
            B_local + i * ldb, B_unpack_local + i, scales_local + i, K, N);
    }
    // cpublas::brgemm(
    //     M,
    //     local_n,
    //     K,
    //     lda,
    //     N,
    //     ldc,
    //     false,
    //     A_data,
    //     B_unpack_local,
    //     C_data + local_begin);
    // cpublas::brgemm_release();
    // dnnl::ukernel::brgemm::release_hw_context();
  });
  dnnl::engine& eng = ideep::engine::cpu_engine();
  dnnl::stream& engine_stream = ideep::stream::default_stream();
  static bool cache = false;
  static dnnl::matmul::primitive_desc matmul_pd;
  static dnnl::matmul matmul_forward;
  static dnnl::memory A_m, B_m, C_m;
  if (!cache) {
    auto a_desc = dnnl ::memory::desc(
        {M, K}, dnnl::memory::data_type::bf16, dnnl::memory::format_tag::ab);
    auto b_desc = dnnl ::memory::desc(
        {K, N}, dnnl::memory::data_type::bf16, dnnl::memory::format_tag::ab);
    auto c_desc = dnnl ::memory::desc(
        {M, N}, dnnl::memory::data_type::bf16, dnnl::memory::format_tag::ab);
    dnnl::primitive_attr pattr;
    pattr.set_scratchpad_mode(dnnl::scratchpad_mode::user);
    matmul_pd =
        dnnl::matmul::primitive_desc(eng, a_desc, b_desc, c_desc, pattr);
    A_m = dnnl::memory(a_desc, eng);
    B_m = dnnl::memory(b_desc, eng);
    C_m = dnnl::memory(c_desc, eng);
    matmul_forward = dnnl::matmul(matmul_pd);
    cache = true;
  }
  dnnl::memory::desc scratchpad_md = matmul_pd.scratchpad_desc();
  std::vector<uint8_t> scratchpad_data(matmul_pd.scratchpad_desc().get_size());
  dnnl::memory scratchpad_memory(scratchpad_md, eng, scratchpad_data.data());
  A_m.set_data_handle((void*)A_data);
  B_m.set_data_handle((void*)B_unpack);
  C_m.set_data_handle((void*)C_data);
  std::unordered_map<int, dnnl::memory> args;
  args.insert({DNNL_ARG_SCRATCHPAD, scratchpad_memory});
  args.insert({DNNL_ARG_SRC, A_m});
  args.insert({DNNL_ARG_WEIGHTS, B_m});
  args.insert({DNNL_ARG_DST, C_m});
  matmul_forward.execute(engine_stream, args);
  engine_stream.wait();
  free(B_unpack);
}

// A block : {BLOCK_M, BLOCK_K}, lda = K
// B block : {BLOCK_K, BLOCK_N}, ldb = K
// C block : {BLOCK_M, BLOCK_N}, ldc = N
//
// scales block: {BLOCK_N}
//
template <int BLOCK_M, int BLOCK_N>
inline void tinygemm_kernel(
    const BFloat16* RESTRICT A,
    const int8_t* RESTRICT B,
    const BFloat16* RESTRICT scales,
    BFloat16* RESTRICT C,
    int lda,
    int ldb,
    int ldc,
    int K) {
  constexpr int ROWS = BLOCK_M;
  constexpr int COLS = BLOCK_N;

  const int PREFETCH_SIZE_K = 16 * 4;

  __m512 va;
  __m512 vb[COLS];
  __m512 vc[ROWS * COLS];
  __m512 scale[COLS];

  auto load_scale = [&](int i) {
    float ss = static_cast<float>(scales[i]);
    scale[i] = _mm512_set1_ps(ss);
  };
  c10::ForcedUnroll<COLS>{}(load_scale);

  auto loadc = [&](auto i) { vc[i] = _mm512_setzero_ps(); };
  c10::ForcedUnroll<ROWS * COLS>{}(loadc);

  auto compute = [&](auto i, int k) {
    constexpr int row = i / COLS;
    constexpr int col = i % COLS;

    if constexpr (col == 0) {
      __m256i a16 = _mm256_load_si256((__m256i*)(A + row * lda + k));
      if (k + PREFETCH_SIZE_K < K) {
        _mm_prefetch(A + row * lda + k + PREFETCH_SIZE_K, _MM_HINT_T0);
      }
      vec::cvtbf16_fp32(a16, va);
    }

    if constexpr (row == 0) {
      __m128i b8 = _mm_load_si128((__m128i*)(B + col * ldb + k));
      if (k + PREFETCH_SIZE_K < K) {
        _mm_prefetch(B + col * ldb + k + PREFETCH_SIZE_K, _MM_HINT_T0);
      }
      __m512i b32 = _mm512_cvtepi8_epi32(b8);
      vb[col] = _mm512_cvtepi32_ps(b32);
      vb[col] = _mm512_mul_ps(vb[col], scale[col]);
    }

    constexpr int idx = row * COLS + col;
    vc[idx] = _mm512_fmadd_ps(va, vb[col], vc[idx]);
  };

  for (int k = 0; k < K; k += 16) {
    c10::ForcedUnroll<ROWS * COLS>{}(compute, k);
  }

  auto storec = [&](auto i) {
    constexpr int row = i / COLS;
    constexpr int col = i % COLS;
    C[row * ldc + col] = static_cast<BFloat16>(_mm512_reduce_add_ps(vc[i]));
  };
  c10::ForcedUnroll<ROWS * COLS>{}(storec);
}

#elif defined(CPU_CAPABILITY_AVX2) && !defined(_MSC_VER)

static inline float _mm256_reduce_add_ps(__m256& v) {
  __m256 v1 = _mm256_permute2f128_ps(v, v, 0x1);
  v = _mm256_add_ps(v, v1);
  v1 = _mm256_shuffle_ps(v, v, 0x4E);
  v = _mm256_add_ps(v, v1);
  v1 = _mm256_shuffle_ps(v, v, 0xB1);
  v = _mm256_add_ps(v, v1);
  return _mm256_cvtss_f32(v);
}

template <int BLOCK_M, int BLOCK_N>
inline void tinygemm_kernel(
    const BFloat16* RESTRICT A,
    const int8_t* RESTRICT B,
    const BFloat16* RESTRICT scales,
    BFloat16* RESTRICT C,
    int lda,
    int ldb,
    int ldc,
    int K) {

  constexpr int ROWS = BLOCK_M;
  constexpr int COLS = BLOCK_N;

  const int PREFETCH_SIZE_K = 16 * 4;

  __m256 va;
  __m256 vb[COLS];
  __m256 vc[ROWS * COLS];
  __m256 scale[COLS];

  auto load_scale = [&](int i) {
    float ss = static_cast<float>(scales[i]);
    scale[i] = _mm256_set1_ps(ss);
  };
  c10::ForcedUnroll<COLS>{}(load_scale);

  auto loadc = [&](auto i) {
    vc[i] = _mm256_setzero_ps();
  };
  c10::ForcedUnroll<ROWS * COLS>{}(loadc);

  auto compute = [&](auto i, int k) {
    constexpr int row = i / COLS;
    constexpr int col = i % COLS;

    if constexpr (col == 0) {
      __m128i a16 = _mm_load_si128((__m128i*)(A + row * lda + k));
      if (k + PREFETCH_SIZE_K < K) {
        _mm_prefetch(A + row * lda + k + PREFETCH_SIZE_K, _MM_HINT_T0);
      }
      vec::cvtbf16_fp32(a16, va);
    }

    if constexpr (row == 0) {
       __m128i b8 = _mm_loadu_si64((__m128i*)(B + col * ldb + k));
       if (k + PREFETCH_SIZE_K < K) {
         _mm_prefetch(B + col * ldb + k + PREFETCH_SIZE_K, _MM_HINT_T0);
       }
       __m256i b32 = _mm256_cvtepi8_epi32(b8);
       vb[col] = _mm256_cvtepi32_ps(b32);
       vb[col] = _mm256_mul_ps(vb[col], scale[col]);
     }

     constexpr int idx = row * COLS + col;
     vc[idx] = _mm256_fmadd_ps(va, vb[col], vc[idx]);
  };

  for (int k = 0; k < K; k += 8) {
    c10::ForcedUnroll<ROWS * COLS>{}(compute, k);
  }

  auto storec = [&](auto i) {
    constexpr int row = i / COLS;
    constexpr int col = i % COLS;
    C[row * ldc + col] = static_cast<BFloat16>(_mm256_reduce_add_ps(vc[i]));
  };
  c10::ForcedUnroll<ROWS * COLS>{}(storec);
}

#endif

#if !defined(C10_MOBILE) && defined(__aarch64__)
#include <arm_neon.h>

inline float reduce(float32x4_t x) {
        auto sum = vpaddq_f32(x, x);
        return vgetq_lane_f32(vpaddq_f32(sum, sum), 0);
}

inline float32x4x2_t load_as_float32x4x2(const Half* ptr) {
  float16x8_t f16_val = vld1q_f16(reinterpret_cast<const float16_t *>(ptr));
  auto val_low = vcvt_f32_f16(vget_low_f16(f16_val));
  auto val_high = vcvt_f32_f16(vget_high_f16(f16_val));
  return {val_low, val_high};
}

inline float32x4_t load_as_float32x4(const Half* ptr) {
    return vcvt_f32_f16(vld1_f16(reinterpret_cast<const float16_t *>(ptr)));
}

inline float32x4x2_t load_as_float32x4x2(const BFloat16* ptr) {
  int32x4_t shift = vdupq_n_s32(16);
  uint16x8_t u16_val = vld1q_u16(reinterpret_cast<const uint16_t *>(ptr));
  uint32x4_t int_low = vmovl_u16(vget_low_u16(u16_val));
  uint32x4_t int_high = vmovl_u16(vget_high_u16(u16_val));
  return {vreinterpretq_f32_u32(vshlq_u32(int_low, shift)), vreinterpretq_f32_u32(vshlq_u32(int_high, shift))};
}

inline float32x4_t load_as_float32x4(const BFloat16* ptr) {
  int32x4_t shift = vdupq_n_s32(16);
  uint32x4_t as_int = vmovl_u16(vld1_u16(reinterpret_cast<const uint16_t *>(ptr)));
  return vreinterpretq_f32_u32(vshlq_u32(as_int, shift));
}

inline float32x4_t load_as_float32x4(const float* ptr) {
  return vld1q_f32(ptr);
}

inline float32x4x2_t load_as_float32x4x2(const float* ptr) {
  return {vld1q_f32(ptr), vld1q_f32(ptr + 4)};
}

template <int BLOCK_M, int BLOCK_N, typename T>
inline void tinygemm_kernel_(
    const T* RESTRICT A,
    const int8_t* RESTRICT B,
    const T* RESTRICT scales,
    T* RESTRICT C,
    int lda,
    int ldb,
    int ldc,
    int K) {

  for (const auto m : c10::irange(BLOCK_M)) {
    float32x4_t c_val[BLOCK_N];
    c10::ForcedUnroll<BLOCK_N>{}([&](auto i) {
        c_val[i] = vdupq_n_f32(0.0);
    });
    for (int k = 0; k < K; k += 8) {
      auto a_val = load_as_float32x4x2(A + m * lda + k);
      c10::ForcedUnroll<BLOCK_N>{}([&](auto i) {
        int16x8_t b_val = vmovl_s8(vld1_s8(B + i * ldb + k));
        auto b_val_low = vcvtq_f32_s32(vmovl_s16(vget_low_s16(b_val)));
        auto b_val_high = vcvtq_f32_s32(vmovl_s16(vget_high_s16(b_val)));
        c_val[i] = vfmaq_f32(c_val[i], a_val.val[1], b_val_high);
        c_val[i] = vfmaq_f32(c_val[i], a_val.val[0], b_val_low);
      });
    }

#if __OPTIMIZE__
    float32x4_t scale_val = load_as_float32x4(scales);
    c10::ForcedUnroll<BLOCK_N>{}([&](auto i) {
      C[m * ldc + i] = reduce(c_val[i]) * vgetq_lane_f32(scale_val, i);
    });
#else
    // Workaround GCCs inability to infer lane index at compile time
    // See https://github.com/pytorch/pytorch/issues/126283
    c10::ForcedUnroll<BLOCK_N>{}([&](auto i) {
      C[m * ldc + i] = reduce(c_val[i]) * float(scales[i]);
    });
#endif
  }
}

template <int BLOCK_M, int BLOCK_N>
inline void tinygemm_kernel(
    const Half* RESTRICT A,
    const int8_t* RESTRICT B,
    const Half* RESTRICT scales,
    Half* RESTRICT C,
    int lda,
    int ldb,
    int ldc,
    int K) {
  tinygemm_kernel_<BLOCK_M, BLOCK_N>(A, B, scales, C, lda, ldb, ldc, K);
}

template <int BLOCK_M, int BLOCK_N>
inline void tinygemm_kernel(
    const BFloat16* RESTRICT A,
    const int8_t* RESTRICT B,
    const BFloat16* RESTRICT scales,
    BFloat16* RESTRICT C,
    int lda,
    int ldb,
    int ldc,
    int K) {
  tinygemm_kernel_<BLOCK_M, BLOCK_N>(A, B, scales, C, lda, ldb, ldc, K);
}

template <int BLOCK_M, int BLOCK_N>
inline void tinygemm_kernel(
    const float* RESTRICT A,
    const int8_t* RESTRICT B,
    const float* RESTRICT scales,
    float* RESTRICT C,
    int lda,
    int ldb,
    int ldc,
    int K) {
  tinygemm_kernel_<BLOCK_M, BLOCK_N>(A, B, scales, C, lda, ldb, ldc, K);
}
#endif

// non-vectorized version
template <int BLOCK_M, int BLOCK_N, typename T>
inline void tinygemm_kernel(
    const T* RESTRICT A,
    const int8_t* RESTRICT B,
    const T* RESTRICT scales,
    T* RESTRICT C,
    int lda,
    int ldb,
    int ldc,
    int K) {

  for (const auto m : c10::irange(BLOCK_M)) {
    for (const auto n : c10::irange(BLOCK_N)) {
      float c_val = 0;
      float scale_val = static_cast<float>(scales[n]);
      for (const auto k : c10::irange(K)) {
        float a_val = static_cast<float>(A[m * lda + k]);
        float b_val = static_cast<float>(B[n * ldb + k]);
        c_val += a_val * (b_val * scale_val);
      }
      C[m * ldc + n] = c_val;
    }
  }
}

#define LAUNCH_TINYGEMM_KERNEL(MB_SIZE, NB_SIZE)                 \
  tinygemm_kernel<MB_SIZE, NB_SIZE>(                             \
      A_ptr, B_ptr, S_ptr, C_ptr,                                \
      K, K, N, K);

#define LAUNCH_TINYGEMM_NB_SIZE(MB_SIZE)                         \
  switch (nb_size) {                                             \
    case 1:                                                      \
      LAUNCH_TINYGEMM_KERNEL(MB_SIZE, 1);                        \
      break;                                                     \
    case 2:                                                      \
      LAUNCH_TINYGEMM_KERNEL(MB_SIZE, 2);                        \
      break;                                                     \
    case 3:                                                      \
      LAUNCH_TINYGEMM_KERNEL(MB_SIZE, 3);                        \
      break;                                                     \
    case 4:                                                      \
      LAUNCH_TINYGEMM_KERNEL(MB_SIZE, 4);                        \
      break;                                                     \
    default:                                                     \
      TORCH_CHECK(false, "Unsupported n block size: ", nb_size); \
      break;                                                     \
  }

template<typename T>
void int8pack_mm_kernel_(
    const Tensor& C,
    const Tensor& A,
    const Tensor& B,
    const Tensor& scales) {

  const auto* A_data = A.const_data_ptr<T>();
  const auto* B_data = B.const_data_ptr<int8_t>();
  auto* C_data = C.data_ptr<T>();
  const auto* S_data = scales.const_data_ptr<T>();

  int M = A.size(0);
  int N = B.size(0);
  int K = A.size(1);

  constexpr int BLOCK_M = 4;
  constexpr int BLOCK_N = 4;

  const int MB = (M + BLOCK_M - 1) / BLOCK_M;
  const int NB = (N + BLOCK_N - 1) / BLOCK_N;

  at::parallel_for(0, MB * NB, 0, [&](int begin, int end) {
    int mb{0}, nb{0};
    data_index_init(begin, mb, MB, nb, NB);

    for (const auto i : c10::irange(begin, end)) {
      (void)i;

      int mb_start = mb * BLOCK_M;
      int mb_size = std::min(BLOCK_M, M - mb_start);
      int nb_start = nb * BLOCK_N;
      int nb_size = std::min(BLOCK_N, N - nb_start);

      const auto* A_ptr = A_data + mb_start * K;
      const auto* B_ptr = B_data + nb_start * K;
      const auto* S_ptr = S_data + nb_start;
      auto* C_ptr = C_data + mb_start * N + nb_start;

      switch (mb_size) {
        case 1:
          LAUNCH_TINYGEMM_NB_SIZE(1);
          break;
        case 2:
          LAUNCH_TINYGEMM_NB_SIZE(2);
          break;
        case 3:
          LAUNCH_TINYGEMM_NB_SIZE(3);
          break;
        case 4:
          LAUNCH_TINYGEMM_NB_SIZE(4);
          break;
        default:
          TORCH_CHECK(false, "Unsupported m block size: ", mb_size);
      }

      // move to the next index
      data_index_step(mb, MB, nb, NB);
    }
  });
}

void int8pack_mm_kernel(
    const Tensor& C,
    const Tensor& A,
    const Tensor& B,
    const Tensor& scales) {
  if (C.dtype() == kHalf) {
    int8pack_mm_kernel_<Half>(C, A, B, scales);
  } else if (C.dtype() == kBFloat16) {
    int8pack_mm_kernel_<BFloat16>(C, A, B, scales);
  } else {
    int8pack_mm_kernel_<float>(C, A, B, scales);
  }
}

} // anonymous namespace

ALSO_REGISTER_AVX512_DISPATCH(int8pack_mm_stub, &int8pack_mm_kernel)

} // at::native
