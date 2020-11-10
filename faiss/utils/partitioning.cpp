/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <faiss/utils/partitioning.h>

#include <cmath>
#include <cassert>

#include <faiss/impl/FaissAssert.h>

#include <faiss/utils/AlignedTable.h>

#include <faiss/utils/ordered_key_value.h>

#include <faiss/utils/simdlib.h>

namespace faiss {


/******************************************************************
 * Internal routines
 ******************************************************************/


namespace partitioning {

template<typename T>
T median3(T a, T b, T c) {
    if (a > b) {
        std::swap(a, b);
    }
    if (c > b) {
        return b;
    }
    if (c > a) {
        return c;
    }
    return a;
}


template<class C>
typename C::T sample_threshold_median3(
    const typename C::T * vals, int n,
    typename C::T thresh_inf, typename C::T thresh_sup
) {
    using T = typename C::T;
    size_t big_prime = 6700417;
    T val3[3];
    int vi = 0;

    for (size_t i = 0; i < n; i++) {
        T v = vals[(i * big_prime) % n];
        // thresh_inf < v < thresh_sup (for CMax)
        if (C::cmp(v, thresh_inf) && C::cmp(thresh_sup, v)) {
            val3[vi++] = v;
            if (vi == 3) {
                break;
            }
        }
    }

    if (vi == 3) {
        return median3(val3[0], val3[1], val3[2]);
    } else if (vi != 0) {
        return val3[0];
    } else {
        return thresh_inf;
        //   FAISS_THROW_MSG("too few values to compute a median");
    }
}

template<class C>
void count_lt_and_eq(
    const typename C::T * vals, size_t n, typename C::T thresh,
    size_t & n_lt, size_t & n_eq
) {
    n_lt = n_eq = 0;

    for(size_t i = 0; i < n; i++) {
        typename C::T v = *vals++;
        if(C::cmp(thresh, v)) {
            n_lt++;
        } else if(v == thresh) {
            n_eq++;
        }
    }
}


template<class C>
size_t compress_array(
    typename C::T *vals, typename C::TI * ids,
    size_t n, typename C::T thresh, size_t n_eq
) {
    size_t wp = 0;
    for(size_t i = 0; i < n; i++) {
        if (C::cmp(thresh, vals[i])) {
            vals[wp] = vals[i];
            ids[wp] = ids[i];
            wp++;
        } else if (n_eq > 0 && vals[i] == thresh) {
            vals[wp] = vals[i];
            ids[wp] = ids[i];
            wp++;
            n_eq--;
        }
    }
    assert(n_eq == 0);
    return wp;
}


#define IFV if(false)

template<class C>
typename C::T partition_fuzzy_median3(
    typename C::T *vals, typename C::TI * ids, size_t n,
    size_t q_min, size_t q_max, size_t * q_out)
{

    if (q_min == 0) {
        if (q_out) {
            *q_out = C::Crev::neutral();
        }
        return 0;
    }
    if (q_max >= n) {
        if (q_out) {
            *q_out = q_max;
        }
        return C::neutral();
    }

    using T = typename C::T;

    // here we use bissection with a median of 3 to find the threshold and
    // compress the arrays afterwards. So it's a n*log(n) algoirithm rather than
    // qselect's O(n) but it avoids shuffling around the array.

    FAISS_THROW_IF_NOT(n >= 3);

    T thresh_inf = C::Crev::neutral();
    T thresh_sup = C::neutral();
    T thresh = median3(vals[0], vals[n / 2], vals[n - 1]);

    size_t n_eq = 0, n_lt = 0;
    size_t q = 0;

    for(int it = 0; it < 200; it++) {
        count_lt_and_eq<C>(vals, n, thresh, n_lt, n_eq);

        IFV  printf("   thresh=%g [%g %g] n_lt=%ld n_eq=%ld, q=%ld:%ld/%ld\n",
            float(thresh), float(thresh_inf), float(thresh_sup),
            n_lt, n_eq, q_min, q_max, n);

        if (n_lt <= q_min) {
            if (n_lt + n_eq >= q_min) {
                q = q_min;
                break;
            } else {
                thresh_inf = thresh;
            }
        } else if (n_lt <= q_max) {
            q = n_lt;
            break;
        } else {
            thresh_sup = thresh;
        }

        // FIXME avoid a second pass over the array to sample the threshold
        IFV  printf("     sample thresh in [%g %g]\n", float(thresh_inf), float(thresh_sup));
        T new_thresh = sample_threshold_median3<C>(vals, n, thresh_inf, thresh_sup);
        if (new_thresh == thresh_inf) {
            // then there is nothing between thresh_inf and thresh_sup
            break;
        }
        thresh = new_thresh;
    }

    int64_t n_eq_1 = q - n_lt;

    IFV printf("shrink: thresh=%g n_eq_1=%ld\n", float(thresh), n_eq_1);

    if (n_eq_1 < 0) { // happens when > q elements are at lower bound
        q = q_min;
        thresh = C::Crev::nextafter(thresh);
        n_eq_1 = q;
    } else {
        assert(n_eq_1 <= n_eq);
    }

    int wp = compress_array<C>(vals, ids, n, thresh, n_eq_1);

    assert(wp == q);
    if (q_out) {
        *q_out = q;
    }

    return thresh;
}


} // namespace partitioning



/******************************************************************
 * SIMD routines when vals is an aligned array of uint16_t
 ******************************************************************/

#ifdef __AVX2__

namespace simd_partitioning {



void find_minimax(
        const uint16_t * vals, size_t n,
        uint16_t & smin, uint16_t & smax
) {

    simd16uint16 vmin(0xffff), vmax(0);
    for (size_t i = 0; i + 15 < n; i += 16) {
        simd16uint16 v(vals + i);
        vmin.accu_min(v);
        vmax.accu_max(v);
    }

    uint16_t tab32[32] __attribute__ ((aligned (32)));
    vmin.store(tab32);
    vmax.store(tab32 + 16);

    smin = tab32[0], smax = tab32[16];

    for(int i = 1; i < 16; i++) {
        smin = std::min(smin, tab32[i]);
        smax = std::max(smax, tab32[i + 16]);
    }

    // missing values
    for(size_t i = (n & ~15); i < n; i++) {
        smin = std::min(smin, vals[i]);
        smax = std::max(smax, vals[i]);
    }

}


// max func differentiates between CMin and CMax (keep lowest or largest)
template<class C>
simd16uint16 max_func(simd16uint16 v, simd16uint16 thr16) {
    constexpr bool is_max = C::is_max;
    if (is_max) {
        return simd16uint16(_mm256_max_epu16(v.i, thr16.i));
    } else {
        return simd16uint16(_mm256_min_epu16(v.i, thr16.i));
    }
}

template<class C>
void count_lt_and_eq(
    const uint16_t * vals, int n, uint16_t thresh,
    size_t & n_lt, size_t & n_eq
) {
    n_lt = n_eq = 0;
    simd16uint16 thr16(thresh);

    size_t n1 = n / 16;

    for (size_t i = 0; i < n1; i++) {
        simd16uint16 v(vals);
        vals += 16;
        simd16uint16 eqmask = (v == thr16);
        simd16uint16 max2 = max_func<C>(v, thr16);
        simd16uint16 gemask = (v == max2);
        uint32_t bits = _mm256_movemask_epi8(
            _mm256_packs_epi16(eqmask.i, gemask.i)
        );
        int i_eq = __builtin_popcount(bits & 0x00ff00ff);
        int i_ge = __builtin_popcount(bits) - i_eq;
        n_eq += i_eq;
        n_lt += 16 - i_ge;
    }

    for(size_t i = n1 * 16; i < n; i++) {
        uint16_t v = *vals++;
        if(C::cmp(thresh, v)) {
            n_lt++;
        } else if(v == thresh) {
            n_eq++;
        }
    }
}



/* compress separated values and ids table, keeping all values < thresh and at
 * most n_eq equal values */
template<class C>
int simd_compress_array(
    uint16_t *vals, typename C::TI * ids, size_t n, uint16_t thresh, int n_eq
) {
    simd16uint16 thr16(thresh);
    simd16uint16 mixmask(0xff00);

    int wp = 0;
    size_t i0;

    // loop while there are eqs to collect
    for (i0 = 0; i0 + 15 < n && n_eq > 0; i0 += 16) {
        simd16uint16 v(vals + i0);
        simd16uint16 max2 = max_func<C>(v, thr16);
        simd16uint16 gemask = (v == max2);
        simd16uint16 eqmask = (v == thr16);
        uint32_t bits = _mm256_movemask_epi8(
            _mm256_blendv_epi8(eqmask.i, gemask.i, mixmask.i)
        );
        bits ^= 0xAAAAAAAA;
        // bit 2*i     : eq
        // bit 2*i + 1 : lt

        while(bits) {
            int j = __builtin_ctz(bits) & (~1);
            bool is_eq = (bits >> j) & 1;
            bool is_lt = (bits >> j) & 2;
            bits &= ~(3 << j);
            j >>= 1;

            if (is_lt) {
                vals[wp] = vals[i0 + j];
                ids[wp] = ids[i0 + j];
                wp++;
            } else if(is_eq && n_eq > 0) {
                vals[wp] = vals[i0 + j];
                ids[wp] = ids[i0 + j];
                wp++;
                n_eq--;
            }
        }
    }

    // handle remaining, only striclty lt ones.
    for (; i0 + 15 < n; i0 += 16) {
        simd16uint16 v(vals + i0);
        simd16uint16 max2 = max_func<C>(v, thr16);
        simd16uint16 gemask = (v == max2);
        uint32_t bits = ~_mm256_movemask_epi8(gemask.i);

        while(bits) {
            int j = __builtin_ctz(bits);
            bits &= ~(3 << j);
            j >>= 1;

            vals[wp] = vals[i0 + j];
            ids[wp] = ids[i0 + j];
            wp++;
        }
    }

    for(int i = (n & ~15); i < n; i++) {
        if (C::cmp(thresh, vals[i])) {
            vals[wp] = vals[i];
            ids[wp] = ids[i];
            wp++;
        } else if (vals[i] == thresh && n_eq > 0) {
            vals[wp] = vals[i];
            ids[wp] = ids[i];
            wp++;
            n_eq--;
        }
    }
    assert(n_eq == 0);
    return wp;
}




static uint64_t get_cy () {
#ifdef  MICRO_BENCHMARK
    uint32_t high, low;
    asm volatile("rdtsc \n\t"
                 : "=a" (low),
                   "=d" (high));
    return ((uint64_t)high << 32) | (low);
#else
    return 0;
#endif
}

#define IFV if(false)

template<class C>
uint16_t simd_partition_fuzzy_with_bounds(
    uint16_t *vals, typename C::TI * ids, size_t n,
    size_t q_min, size_t q_max, size_t * q_out,
    uint16_t s0i, uint16_t s1i)
{

    if (q_min == 0) {
        if (q_out) {
            *q_out = 0;
        }
        return 0;
    }
    if (q_max >= n) {
        if (q_out) {
            *q_out = q_max;
        }
        return 0xffff;
    }
    if (s0i == s1i) {
        if (q_out) {
            *q_out = q_min;
        }
        return s0i;
    }
    uint64_t t0 = get_cy();

    // lower bound inclusive, upper exclusive
    size_t s0 = s0i, s1 = s1i + 1;

    IFV printf("bounds: %ld %ld\n", s0, s1 - 1);

    int thresh;
    size_t n_eq = 0, n_lt = 0;
    size_t q = 0;

    for(int it = 0; it < 200; it++) {
        // while(s0 + 1 < s1) {
        thresh = (s0 + s1) / 2;
        count_lt_and_eq<C>(vals, n, thresh, n_lt, n_eq);

        IFV  printf("   [%ld %ld] thresh=%d n_lt=%ld n_eq=%ld, q=%ld:%ld/%ld\n",
            s0, s1, thresh, n_lt, n_eq, q_min, q_max, n);
        if (n_lt <= q_min) {
            if (n_lt + n_eq >= q_min) {
                q = q_min;
                break;
            } else {
                if (C::is_max) {
                    s0 = thresh;
                } else {
                    s1 = thresh;
                }
            }
        } else if (n_lt <= q_max) {
            q = n_lt;
            break;
        } else {
            if (C::is_max) {
                s1 = thresh;
            } else {
                s0 = thresh;
            }
        }

    }

    uint64_t t1 = get_cy();

    // number of equal values to keep
    int64_t n_eq_1 = q - n_lt;

    IFV printf("shrink: thresh=%d q=%ld n_eq_1=%ld\n", thresh, q, n_eq_1);
    if (n_eq_1 < 0) { // happens when > q elements are at lower bound
        assert(s0 + 1 == s1);
        q = q_min;
        if (C::is_max) {
            thresh--;
        } else {
            thresh++;
        }
        n_eq_1 = q;
        IFV printf("  override: thresh=%d n_eq_1=%ld\n", thresh, n_eq_1);
    } else {
        assert(n_eq_1 <= n_eq);
    }

    size_t wp = simd_compress_array<C>(vals, ids, n, thresh, n_eq_1);

    IFV printf("wp=%ld\n", wp);
    assert(wp == q);
    if (q_out) {
        *q_out = q;
    }

    return thresh;
}


template<class C>
uint16_t simd_partition_fuzzy(
    uint16_t *vals, typename C::TI * ids, size_t n,
    size_t q_min, size_t q_max, size_t * q_out
) {

    assert(is_aligned_pointer(vals, 32));

    uint64_t t0 = get_cy();
    uint16_t s0i, s1i;
    find_minimax(vals, n, s0i, s1i);
    // QSelect_stats.t0 += get_cy() - t0;

    return simd_partition_fuzzy_with_bounds<C>(
        vals, ids, n, q_min, q_max, q_out, s0i, s1i);
}



template<class C>
uint16_t simd_partition(uint16_t *vals, typename C::TI * ids, size_t n, size_t q) {

    assert(is_aligned_pointer(vals));

    if (q == 0) {
        return 0;
    }
    if (q >= n) {
        return 0xffff;
    }

    uint16_t s0i, s1i;
    find_minimax(vals, n, s0i, s1i);

    return simd_partition_fuzzy_with_bounds<C>(
        vals, ids, n, q, q, nullptr, s0i, s1i);
}

template<class C>
uint16_t simd_partition_with_bounds(
    uint16_t *vals, typename C::TI * ids, size_t n, size_t q,
    uint16_t s0i, uint16_t s1i)
{
    return simd_partition_fuzzy_with_bounds<C>(
        vals, ids, n, q, q, nullptr, s0i, s1i);
}

} // namespace simd_partitioning

#endif

/******************************************************************
 * Driver routine
 ******************************************************************/


template<class C>
typename C::T partition_fuzzy(
    typename C::T *vals, typename C::TI * ids, size_t n,
    size_t q_min, size_t q_max, size_t * q_out)
{
#ifdef __AVX2__
    constexpr bool is_uint16 = std::is_same<typename C::T, uint16_t>::value;
    if (is_uint16 && is_aligned_pointer(vals)) {
        return simd_partitioning::simd_partition_fuzzy<C>(
            (uint16_t*)vals, ids, n, q_min, q_max, q_out);
    }
#endif
    return partitioning::partition_fuzzy_median3<C>(
        vals, ids, n, q_min, q_max, q_out);
}


// explicit template instanciations

template float partition_fuzzy<CMin<float, int64_t>> (
    float *vals, int64_t * ids, size_t n,
    size_t q_min, size_t q_max, size_t * q_out);

template float partition_fuzzy<CMax<float, int64_t>> (
    float *vals, int64_t * ids, size_t n,
    size_t q_min, size_t q_max, size_t * q_out);

template uint16_t partition_fuzzy<CMin<uint16_t, int64_t>> (
    uint16_t *vals, int64_t * ids, size_t n,
    size_t q_min, size_t q_max, size_t * q_out);

template uint16_t partition_fuzzy<CMax<uint16_t, int64_t>> (
    uint16_t *vals, int64_t * ids, size_t n,
    size_t q_min, size_t q_max, size_t * q_out);

template uint16_t partition_fuzzy<CMin<uint16_t, int>> (
    uint16_t *vals, int * ids, size_t n,
    size_t q_min, size_t q_max, size_t * q_out);

template uint16_t partition_fuzzy<CMax<uint16_t, int>> (
    uint16_t *vals, int * ids, size_t n,
    size_t q_min, size_t q_max, size_t * q_out);



/******************************************************************
 * Histogram subroutines
 ******************************************************************/



namespace  {

/************************************************************
 * 8 bins
 ************************************************************/

simd32uint8 accu4to8(simd16uint16 a4) {
    simd16uint16 mask4(0x0f0f);

    simd16uint16 a8_0 = a4 & mask4;
    simd16uint16 a8_1 = (a4 >> 4) & mask4;

    return simd32uint8(_mm256_hadd_epi16(a8_0.i, a8_1.i));
}


simd16uint16 accu8to16(simd32uint8 a8) {
    simd16uint16 mask8(0x00ff);

    simd16uint16 a8_0 = simd16uint16(a8) & mask8;
    simd16uint16 a8_1 = (simd16uint16(a8) >> 8) & mask8;

    return simd16uint16(_mm256_hadd_epi16(a8_0.i, a8_1.i));
}


static const simd32uint8 shifts(_mm256_setr_epi8(
    1, 16, 0, 0,  4, 64, 0, 0,
    0, 0, 1, 16,  0, 0, 4, 64,
    1, 16, 0, 0,  4, 64, 0, 0,
    0, 0, 1, 16,  0, 0, 4, 64
));

// 2-bit accumulator: we can add only up to 3 elements
// on output we return 2*4-bit results
// preproc returns either an index in 0..7 or a value with the high bit set
// that yeilds a 0 when used in the table look-up
template<int N, class Preproc>
void compute_accu2(
        const uint16_t * & data,
        Preproc & pp,
        simd16uint16 & a4lo, simd16uint16 & a4hi
) {
    simd16uint16 mask2(0x3333);
    simd16uint16 a2((uint16_t)0); // 2-bit accu
    for (int j = 0; j < N; j ++) {
        simd16uint16 v(data);
        data += 16;
        v = pp(v);
        simd16uint16 idx = v | (v << 8) | simd16uint16(0x800);
        a2 += simd16uint16(shifts.lookup_2_lanes(simd32uint8(idx)));
    }
    a4lo += a2 & mask2;
    a4hi += (a2 >> 2) & mask2;
}


template<class Preproc>
simd16uint16 histogram_8(
        const uint16_t * data, Preproc pp,
        size_t n_in) {

    assert (n_in % 16 == 0);
    int n = n_in / 16;

    simd32uint8 a8lo(0);
    simd32uint8 a8hi(0);

    for(int i0 = 0; i0 < n; i0 += 15) {
        simd16uint16 a4lo(0);  // 4-bit accus
        simd16uint16 a4hi(0);

        int i1 = std::min(i0 + 15, n);
        int i;
        for(i = i0; i + 2 < i1; i += 3) {
            compute_accu2<3>(data, pp, a4lo, a4hi); // adds 3 max
        }
        switch (i1 - i) {
        case 2:
            compute_accu2<2>(data, pp, a4lo, a4hi);
            break;
        case 1:
            compute_accu2<1>(data, pp, a4lo, a4hi);
            break;
        }

        a8lo += accu4to8(a4lo);
        a8hi += accu4to8(a4hi);
    }

    // move to 16-bit accu
    simd16uint16 a16lo = accu8to16(a8lo);
    simd16uint16 a16hi = accu8to16(a8hi);

    simd16uint16 a16 = simd16uint16(_mm256_hadd_epi16(a16lo.i, a16hi.i));

    // the 2 lanes must still be combined
    return a16;
}


/************************************************************
 * 16 bins
 ************************************************************/



static const simd32uint8 shifts2(_mm256_setr_epi8(
    1, 2, 4, 8, 16, 32, 64, (char)128,
    1, 2, 4, 8, 16, 32, 64, (char)128,
    1, 2, 4, 8, 16, 32, 64, (char)128,
    1, 2, 4, 8, 16, 32, 64, (char)128
));


simd32uint8 shiftr_16(simd32uint8 x, int n)
{
    return simd32uint8(simd16uint16(x) >> n);
}


inline simd32uint8 combine_2x2(simd32uint8 a, simd32uint8 b) {

    __m256i a1b0 = _mm256_permute2f128_si256(a.i, b.i, 0x21);
    __m256i a0b1 = _mm256_blend_epi32(a.i, b.i, 0xF0);

    return simd32uint8(a1b0) + simd32uint8(a0b1);
}


// 2-bit accumulator: we can add only up to 3 elements
// on output we return 2*4-bit results
template<int N, class Preproc>
void compute_accu2_16(
        const uint16_t * & data, Preproc pp,
        simd32uint8 & a4_0, simd32uint8 & a4_1,
        simd32uint8 & a4_2, simd32uint8 & a4_3
) {
    simd32uint8 mask1(0x55);
    simd32uint8 a2_0; // 2-bit accu
    simd32uint8 a2_1; // 2-bit accu
    a2_0.clear(); a2_1.clear();

    for (int j = 0; j < N; j ++) {
        simd16uint16 v(data);
        data += 16;
        v = pp(v);

        simd16uint16 idx = v | (v << 8);
        simd32uint8 a1 = shifts2.lookup_2_lanes(simd32uint8(idx));

        if (pp.do_clip()) {
            simd16uint16 lt16 = (v >> 4) == simd16uint16(0);
            a1 = a1 & lt16;
        }

        simd16uint16 lt8 = (v >> 3) == simd16uint16(0);
        lt8.i = _mm256_xor_si256(lt8.i, _mm256_set1_epi16(0xff00));

        a1 = a1 & lt8;

        a2_0 += a1 & mask1;
        a2_1 += shiftr_16(a1, 1) & mask1;
    }
    simd32uint8 mask2(0x33);

    a4_0 += a2_0 & mask2;
    a4_1 += a2_1 & mask2;
    a4_2 += shiftr_16(a2_0, 2) & mask2;
    a4_3 += shiftr_16(a2_1, 2) & mask2;

}


simd32uint8 accu4to8_2(simd32uint8 a4_0, simd32uint8 a4_1) {
    simd32uint8 mask4(0x0f);

    simd32uint8 a8_0 = combine_2x2(
        a4_0 & mask4,
        shiftr_16(a4_0, 4) & mask4
    );

    simd32uint8 a8_1 = combine_2x2(
        a4_1 & mask4,
        shiftr_16(a4_1, 4) & mask4
    );

    return simd32uint8(_mm256_hadd_epi16(a8_0.i, a8_1.i));
}



template<class Preproc>
simd16uint16 histogram_16(const uint16_t * data, Preproc pp, size_t n_in) {

    assert (n_in % 16 == 0);
    int n = n_in / 16;

    simd32uint8 a8lo((uint8_t)0);
    simd32uint8 a8hi((uint8_t)0);

    for(int i0 = 0; i0 < n; i0 += 7) {
        simd32uint8 a4_0(0); // 0, 4, 8, 12
        simd32uint8 a4_1(0); // 1, 5, 9, 13
        simd32uint8 a4_2(0); // 2, 6, 10, 14
        simd32uint8 a4_3(0); // 3, 7, 11, 15

        int i1 = std::min(i0 + 7, n);
        int i;
        for(i = i0; i + 2 < i1; i += 3) {
            compute_accu2_16<3>(data, pp, a4_0, a4_1, a4_2, a4_3);
        }
        switch (i1 - i) {
        case 2:
            compute_accu2_16<2>(data, pp, a4_0, a4_1, a4_2, a4_3);
            break;
        case 1:
            compute_accu2_16<1>(data, pp, a4_0, a4_1, a4_2, a4_3);
            break;
        }

        a8lo += accu4to8_2(a4_0, a4_1);
        a8hi += accu4to8_2(a4_2, a4_3);
    }

    // move to 16-bit accu
    simd16uint16 a16lo = accu8to16(a8lo);
    simd16uint16 a16hi = accu8to16(a8hi);

    simd16uint16 a16 = simd16uint16(_mm256_hadd_epi16(a16lo.i, a16hi.i));

    __m256i perm32 = _mm256_setr_epi32(
        0, 2, 4, 6, 1, 3, 5, 7
    );
    a16.i = _mm256_permutevar8x32_epi32(a16.i, perm32);

    return a16;
}

struct PreprocNOP {
    simd16uint16 operator () (simd16uint16 x)  {
        return x;
    }
    static bool do_clip() {
        return false;
    }
};


template<int shift>
struct PreprocMinShift {
    simd16uint16 min16;

    PreprocMinShift(uint16_t min) {
        min16.set1(min);
    }

    simd16uint16 operator () (simd16uint16 x)  {
        x = x - min16;
        x.i = _mm256_srai_epi16(x.i, shift); // signed
        return x;
    }

    static bool do_clip() {
        return true;
    }


};

/* unbounded versions of the functions */

void simd_histogram_8_unbounded(
    const uint16_t *data, int n,
    int *hist)
{
    PreprocNOP pp;
    simd16uint16 a16 = histogram_8(data, pp, (n & ~15));

    uint16_t a16_tab[16] __attribute__ ((aligned (32)));
    a16.store(a16_tab);

    for(int i = 0; i < 8; i++) {
        hist[i] = a16_tab[i] + a16_tab[i + 8];
    }

    for(int i = (n & ~15); i < n; i++) {
        hist[data[i]]++;
    }

}


void simd_histogram_16_unbounded(
    const uint16_t *data, int n,
    int *hist)
{

    simd16uint16 a16 = histogram_16(data, PreprocNOP(), (n & ~15));

    uint16_t a16_tab[16] __attribute__ ((aligned (32)));
    a16.store(a16_tab);

    for(int i = 0; i < 16; i++) {
        hist[i] = a16_tab[i];
    }

    for(int i = (n & ~15); i < n; i++) {
        hist[data[i]]++;
    }

}



} // anonymous namespace

/************************************************************
 * Driver routines
 ************************************************************/

void simd_histogram_8(
    const uint16_t *data, int n,
    uint16_t min, int shift,
    int *hist)
{
    if (shift < 0) {
        simd_histogram_8_unbounded(data, n, hist);
        return;
    }

    simd16uint16 a16;

#define DISPATCH(s)  \
     case s: \
        a16 = histogram_8(data, PreprocMinShift<s>(min), (n & ~15)); \
        break

    switch(shift) {
        DISPATCH(0);
        DISPATCH(1);
        DISPATCH(2);
        DISPATCH(3);
        DISPATCH(4);
        DISPATCH(5);
        DISPATCH(6);
        DISPATCH(7);
        DISPATCH(8);
    default:
        FAISS_THROW_FMT("dispatch for shift=%d not instantiated", shift);
    }
#undef DISPATCH

    uint16_t a16_tab[16] __attribute__ ((aligned (32)));
    a16.store(a16_tab);

    for(int i = 0; i < 8; i++) {
        hist[i] = a16_tab[i] + a16_tab[i + 8];
    }

    // complete with remaining bins
    for(int i = (n & ~15); i < n; i++) {
        uint16_t v = (data[i] - min);
        int16_t vs = v;
        vs >>= shift;  // need signed shift
        v = vs;
        if (v < 8)  hist[v]++;
    }

}



void simd_histogram_16(
    const uint16_t *data, int n,
    uint16_t min, int shift,
    int *hist)
{
    if (shift < 0) {
        simd_histogram_16_unbounded(data, n, hist);
        return;
    }

    simd16uint16 a16;

#define DISPATCH(s)  \
     case s: \
        a16 = histogram_16(data, PreprocMinShift<s>(min), (n & ~15)); \
        break

    switch(shift) {
        DISPATCH(0);
        DISPATCH(1);
        DISPATCH(2);
        DISPATCH(3);
        DISPATCH(4);
        DISPATCH(5);
        DISPATCH(6);
        DISPATCH(7);
        DISPATCH(8);
    default:
        FAISS_THROW_FMT("dispatch for shift=%d not instantiated", shift);
    }
#undef DISPATCH

    uint16_t a16_tab[16] __attribute__ ((aligned (32)));
    a16.store(a16_tab);

    for(int i = 0; i < 16; i++) {
        hist[i] = a16_tab[i];
    }

    for(int i = (n & ~15); i < n; i++) {
        uint16_t v = (data[i] - min);
        int16_t vs = v;
        vs >>= shift;  // need signed shift
        v = vs;
        if (v < 16)  hist[v]++;
    }

}

// this code does not compile properly with GCC 7.4.0
// FIXME make a scalar version


} // namespace faiss
