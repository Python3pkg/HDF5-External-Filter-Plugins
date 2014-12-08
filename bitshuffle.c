#include "bitshuffle.h"
#include "iochain.h"
#include "lz4.h"

#include <stdio.h>
#include <string.h>


#if defined(__AVX2__) && defined (__SSE2__)
#define USEAVX2
#endif

#if defined(__SSE2__)
#define USESSE2
#endif


// Conditional includes for SSE2 and AVX2.
#ifdef USEAVX2
#include <immintrin.h>
#elif defined USESSE2
#include <emmintrin.h>
#endif


// Constants.
#define BSHUF_MIN_RECOMMEND_BLOCK 128
#define BSHUF_BLOCKED_MULT 8    // Block sizes must be multiple of this.
#define BSHUF_TARGET_BLOCK_SIZE_B 8192
// Use fast decompression instead of safe decompression for LZ4.
#define BSHUF_LZ4_DECOMPRESS_FAST


// Macros.
#define CHECK_MULT_EIGHT(n) if (n % 8) return -80;
#define MIN(X,Y) ((X) < (Y) ? (X) : (Y))
#define MAX(X,Y) ((X) > (Y) ? (X) : (Y))
#define CHECK_ERR(count) if (count < 0) { return count; }
#define CHECK_ERR_FREE(count, buf) if (count < 0) { free(buf); return count; }
#define CHECK_ERR_FREE_LZ(count, buf) if (count < 0) {                      \
    free(buf); return count - 1000; }
//#define CHECK_ERR_FREE(count, buf) {}


/* Functions telling what instructions set used at compile time. */

int bshuf_using_SSE2(void) {
#ifdef USESSE2
    return 1;
#else
    return 0;
#endif
}


int bshuf_using_AVX2(void) {
#ifdef USEAVX2
    return 1;
#else
    return 0;
#endif
}




// Function definition for worker functions.
typedef int64_t (*bshufFunDef)(ioc_chain* C_ptr,
        const size_t size, const size_t elem_size);


/* ---- Code that should compile on any machine. ---- */

/* Memory copy with bshuf call signature. For testing and profiling. */
int64_t bshuf_copy(void* in, void* out, const size_t size,
         const size_t elem_size) {

    char* A = (char*) in;
    char* B = (char*) out;

    memcpy(B, A, size * elem_size);
    return size * elem_size;
}


/* Transpose bytes within elements, starting partway through input. */
int64_t bshuf_trans_byte_elem_remainder(void* in, void* out, const size_t size,
         const size_t elem_size, const size_t start) {

    char* A = (char*) in;
    char* B = (char*) out;

    CHECK_MULT_EIGHT(start);

    if (size > start) {
        // ii loop separated into 2 loops so the compiler can unroll
        // the inner one.
        for (size_t ii = start; ii + 7 < size; ii += 8) {
            for (size_t jj = 0; jj < elem_size; jj++) {
                for (size_t kk = 0; kk < 8; kk++) {
                    B[jj * size + ii + kk]
                        = A[ii * elem_size + kk * elem_size + jj];
                }
            }
        }
        for (size_t ii = size - size % 8; ii < size; ii ++) {
            for (size_t jj = 0; jj < elem_size; jj++) {
                B[jj * size + ii] = A[ii * elem_size + jj];
            }
        }
    }
    return size * elem_size;
}


/* Transpose bytes within elements, scalar algorithm. */
int64_t bshuf_trans_byte_elem_scal(void* in, void* out, const size_t size,
         const size_t elem_size) {

    return bshuf_trans_byte_elem_remainder(in, out, size, elem_size, 0);
}


/* Untranspose bytes within elements, starting partway through input. */
int64_t bshuf_untrans_byte_elem_remainder(void* in, void* out,
        const size_t size, const size_t elem_size, const size_t start) {

    char* A = (char*) in;
    char* B = (char*) out;

    CHECK_MULT_EIGHT(start);

    if (size > start) {
        // ii loop separated into 2 loops so the compiler can unroll
        // the inner one.
        for (size_t ii = start; ii + 7 < size; ii += 8) {
            for (size_t jj = 0; jj < elem_size; jj++) {
                for (size_t kk = 0; kk < 8; kk++) {
                    B[ii * elem_size + kk * elem_size + jj] 
                        = A[jj * size + ii + kk];
                }
            }
        }
        for (size_t ii = size - size % 8; ii < size; ii ++) {
            for (size_t jj = 0; jj < elem_size; jj++) {
                 B[ii * elem_size + jj] = A[jj * size + ii];
            }
        }
    }
    return size * elem_size;
}


/* Untranspose bytes within elements, scalar algorithm. */
int64_t bshuf_untrans_byte_elem_scal(void* in, void* out, const size_t size,
         const size_t elem_size) {

    return bshuf_untrans_byte_elem_remainder(in, out, size, elem_size, 0);
}


#define TRANS_BIT_8X8(x, t) {                                               \
        t = (x ^ (x >> 7)) & 0x00AA00AA00AA00AALL;                          \
        x = x ^ t ^ (t << 7);                                               \
        t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCLL;                         \
        x = x ^ t ^ (t << 14);                                              \
        t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0LL;                         \
        x = x ^ t ^ (t << 28);                                              \
    }


/* Transpose bits within bytes. Does not use x86 specific instructions. */
int64_t bshuf_trans_bit_byte_remainder(void* in, void* out, const size_t size,
         const size_t elem_size, const size_t start_byte) {

    uint64_t* A = in;
    uint8_t* B = out;

    uint64_t x, t;

    size_t nbyte = elem_size * size;
    size_t nbyte_bitrow = nbyte / 8;

    CHECK_MULT_EIGHT(nbyte);
    CHECK_MULT_EIGHT(start_byte);

    for (size_t ii = start_byte / 8; ii < nbyte_bitrow; ii ++) {
        x = A[ii];
        TRANS_BIT_8X8(x, t);
        for (int kk = 0; kk < 8; kk ++) {
            B[kk * nbyte_bitrow + ii] = x;
            x = x >> 8;
        }
    }
    return size * elem_size;
}


/* Transpose bits within bytes. Does not use x86 specific instructions. */
int64_t bshuf_trans_bit_byte_scal(void* in, void* out, const size_t size,
         const size_t elem_size) {

    return bshuf_trans_bit_byte_remainder(in, out, size, elem_size, 0);
}


/* Untranspose bits within bytes. Does not use x86 specific instructions. */
int64_t bshuf_untrans_bit_byte_remainder(void* in, void* out,
        const size_t size, const size_t elem_size, const size_t start) {

    uint8_t* A = in;
    uint64_t* B = out;

    CHECK_MULT_EIGHT(size);
    CHECK_MULT_EIGHT(start);

    size_t nbyte_bitrow = size / 8;

    uint64_t x, t;

    for (size_t jj = 0; jj < elem_size; jj ++) {
        for (size_t ii = start / 8; ii < size / 8; ii ++) {
            x = 0;
            for (int kk = 0; kk < 8; kk ++) {
                x = x << 8;
                x += A[jj * size + (7 - kk) * nbyte_bitrow + ii];
            }
            TRANS_BIT_8X8(x, t);
            B[jj * size / 8 + ii] = x;
        }
    }
    return size * elem_size;
}


/* Untranspose bits within bytes. Does not use x86 specific instructions. */
int64_t bshuf_untrans_bit_byte_scal(void* in, void* out, const size_t size,
         const size_t elem_size) {

    return bshuf_untrans_bit_byte_remainder(in, out, size, elem_size, 0);
}


/* Transpose bits within bytes. Does not use x86 specific instructions. */
int64_t bshuf_trans_bit_byte_scal_unrolled(void* in, void* out,
        const size_t size, const size_t elem_size) {

    uint64_t* A = in;
    uint8_t* B = out;

    int64_t count;

    uint64_t x0, x1, x2, x3, t0, t1, t2, t3;

    size_t nbyte = elem_size * size;
    size_t nbyte_bitrow = nbyte / 8;

    for (size_t ii = 0; ii + 3 < nbyte_bitrow; ii += 4) {
        x0 = A[ii + 0];
        x1 = A[ii + 1];
        x2 = A[ii + 2];
        x3 = A[ii + 3];

        TRANS_BIT_8X8(x0, t0);
        TRANS_BIT_8X8(x1, t1);
        TRANS_BIT_8X8(x2, t2);
        TRANS_BIT_8X8(x3, t3);

        // Get back 4% if I unroll this.
        for (int kk = 0; kk < 8; kk ++) {
            B[kk * nbyte_bitrow + ii + 0] = x0;
            x0 = x0 >> 8;
            B[kk * nbyte_bitrow + ii + 1] = x1;
            x1 = x1 >> 8;
            B[kk * nbyte_bitrow + ii + 2] = x2;
            x2 = x2 >> 8;
            B[kk * nbyte_bitrow + ii + 3] = x3;
            x3 = x3 >> 8;
        }
    }
    count = bshuf_trans_bit_byte_remainder(in, out, size, elem_size,
            8 * (nbyte_bitrow - nbyte_bitrow % 4));
    return count;
}


int64_t bshuf_trans_bit_elem_scal(void* in, void* out, const size_t size,
         const size_t elem_size) {

    char* A = (char*) in;
    char* B = (char*) out;

    union {uint64_t x; char b[8];} mx0;
    uint64_t t0;
    size_t nbyte_bitrow = size / 8;

    CHECK_MULT_EIGHT(size);

    for (size_t ii = 0; ii < nbyte_bitrow; ii ++) {
        for (size_t jj = 0; jj < elem_size; jj ++) {
            for (size_t kk = 0; kk < 8; kk ++) {
                mx0.b[kk] = A[(ii + 0) * 8 * elem_size + kk * elem_size + jj];
            }
            TRANS_BIT_8X8(mx0.x, t0);
            for (size_t kk = 0; kk < 8; kk ++) {
                B[jj * size + kk * nbyte_bitrow + ii + 0] = mx0.b[kk];
            }
        }
    }
    return size * elem_size;
}


int64_t bshuf_untrans_bit_elem_scal(void* in, void* out, const size_t size,
         const size_t elem_size) {

    char* A = (char*) in;
    char* B = (char*) out;

    union {uint64_t x; char b[8];} mx0;
    uint64_t t0;
    size_t nbyte_bitrow = size / 8;

    CHECK_MULT_EIGHT(size);

    for (size_t ii = 0; ii < nbyte_bitrow; ii ++) {
        for (size_t jj = 0; jj < elem_size; jj ++) {
            for (size_t kk = 0; kk < 8; kk ++) {
                mx0.b[kk] = A[jj * size + kk * nbyte_bitrow + ii + 0];
            }
            TRANS_BIT_8X8(mx0.x, t0);
            for (size_t kk = 0; kk < 8; kk ++) {
                B[(ii + 0) * 8 * elem_size + kk * elem_size + jj] = mx0.b[kk];
            }
        }
    }
    return size * elem_size;
}


/* Transpose of an array, optimized for small elements. */
#define TRANS_ELEM_TYPE(in, out, lda, ldb, type_t) {                        \
        type_t* A = (type_t*) in;                                           \
        type_t* B = (type_t*) out;                                          \
        for(size_t ii = 0; ii + 7 < lda; ii += 8) {                         \
            for(size_t jj = 0; jj < ldb; jj++) {                            \
                for(size_t kk = 0; kk < 8; kk++) {                          \
                    B[jj*lda + ii + kk] = A[ii*ldb + kk * ldb + jj];        \
                }                                                           \
            }                                                               \
        }                                                                   \
        for(size_t ii = lda - lda % 8; ii < lda; ii ++) {                   \
            for(size_t jj = 0; jj < ldb; jj++) {                            \
                B[jj*lda + ii] = A[ii*ldb + jj];                            \
            }                                                               \
        }                                                                   \
    }


/* General transpose of an array, optimized for large element sizes. */
int64_t bshuf_trans_elem(void* in, void* out, const size_t lda,
        const size_t ldb, const size_t elem_size) {

    char* A = (char*) in;
    char* B = (char*) out;
    for(size_t ii = 0; ii < lda; ii++) {
        for(size_t jj = 0; jj < ldb; jj++) {
            memcpy(&B[(jj*lda + ii) * elem_size],
                   &A[(ii*ldb + jj) * elem_size], elem_size);
        }
    }
    return lda * ldb * elem_size;
}


/* Transpose rows of shuffled bits (size / 8 bytes) within groups of 8. */
int64_t bshuf_trans_bitrow_eight(void* in, void* out, const size_t size,
         const size_t elem_size) {

    size_t nbyte_bitrow = size / 8;

    CHECK_MULT_EIGHT(size);

    return bshuf_trans_elem(in, out, 8, elem_size, nbyte_bitrow);
}


/* For data organized into a row for each bit (8 * elem_size rows), transpose
 * the bytes. */
int64_t bshuf_trans_byte_bitrow(void* in, void* out, const size_t size,
         const size_t elem_size) {
    char* A = (char*) in;
    char* B = (char*) out;

    size_t nbyte_row = size / 8;

    CHECK_MULT_EIGHT(size);

    for (size_t ii = 0; ii < nbyte_row; ii++) {
        for (size_t jj = 0; jj < 8*elem_size; jj++) {
            B[ii * 8 * elem_size + jj] = A[jj * nbyte_row + ii];
        }
    }
    return size * elem_size;
}


/* ---- Code that requires SSE2. x86 architectures post Pentium 4. ---- */

#ifdef USESSE2

/* Transpose bytes within elements using SSE for 16 bit elements. */
int64_t bshuf_trans_byte_elem_SSE_16(void* in, void* out, const size_t size) {

    char* A = (char*) in;
    char* B = (char*) out;
    __m128i a0, b0, a1, b1;

    for (size_t ii=0; ii + 15 < size; ii += 16) {
        a0 = _mm_loadu_si128((__m128i *) &A[2*ii + 0*16]);
        b0 = _mm_loadu_si128((__m128i *) &A[2*ii + 1*16]);

        a1 = _mm_unpacklo_epi8(a0, b0);
        b1 = _mm_unpackhi_epi8(a0, b0);

        a0 = _mm_unpacklo_epi8(a1, b1);
        b0 = _mm_unpackhi_epi8(a1, b1);

        a1 = _mm_unpacklo_epi8(a0, b0);
        b1 = _mm_unpackhi_epi8(a0, b0);

        a0 = _mm_unpacklo_epi8(a1, b1);
        b0 = _mm_unpackhi_epi8(a1, b1);

        _mm_storeu_si128((__m128i *) &B[0*size + ii], a0);
        _mm_storeu_si128((__m128i *) &B[1*size + ii], b0);
    }
    return bshuf_trans_byte_elem_remainder(in, out, size, 2,
            size - size % 16);
}


/* Transpose bytes within elements using SSE for 32 bit elements. */
int64_t bshuf_trans_byte_elem_SSE_32(void* in, void* out, const size_t size) {

    char* A = (char*) in;
    char* B = (char*) out;
    __m128i a0, b0, c0, d0, a1, b1, c1, d1;

    for (size_t ii=0; ii + 15 < size; ii += 16) {
        a0 = _mm_loadu_si128((__m128i *) &A[4*ii + 0*16]);
        b0 = _mm_loadu_si128((__m128i *) &A[4*ii + 1*16]);
        c0 = _mm_loadu_si128((__m128i *) &A[4*ii + 2*16]);
        d0 = _mm_loadu_si128((__m128i *) &A[4*ii + 3*16]);

        a1 = _mm_unpacklo_epi8(a0, b0);
        b1 = _mm_unpackhi_epi8(a0, b0);
        c1 = _mm_unpacklo_epi8(c0, d0);
        d1 = _mm_unpackhi_epi8(c0, d0);

        a0 = _mm_unpacklo_epi8(a1, b1);
        b0 = _mm_unpackhi_epi8(a1, b1);
        c0 = _mm_unpacklo_epi8(c1, d1);
        d0 = _mm_unpackhi_epi8(c1, d1);

        a1 = _mm_unpacklo_epi8(a0, b0);
        b1 = _mm_unpackhi_epi8(a0, b0);
        c1 = _mm_unpacklo_epi8(c0, d0);
        d1 = _mm_unpackhi_epi8(c0, d0);

        a0 = _mm_unpacklo_epi64(a1, c1);
        b0 = _mm_unpackhi_epi64(a1, c1);
        c0 = _mm_unpacklo_epi64(b1, d1);
        d0 = _mm_unpackhi_epi64(b1, d1);

        _mm_storeu_si128((__m128i *) &B[0*size + ii], a0);
        _mm_storeu_si128((__m128i *) &B[1*size + ii], b0);
        _mm_storeu_si128((__m128i *) &B[2*size + ii], c0);
        _mm_storeu_si128((__m128i *) &B[3*size + ii], d0);
    }
    return bshuf_trans_byte_elem_remainder(in, out, size, 4,
            size - size % 16);
}


/* Transpose bytes within elements using SSE for 64 bit elements. */
int64_t bshuf_trans_byte_elem_SSE_64(void* in, void* out, const size_t size) {

    char* A = (char*) in;
    char* B = (char*) out;
    __m128i a0, b0, c0, d0, e0, f0, g0, h0;
    __m128i a1, b1, c1, d1, e1, f1, g1, h1;

    for (size_t ii=0; ii + 15 < size; ii += 16) {
        a0 = _mm_loadu_si128((__m128i *) &A[8*ii + 0*16]);
        b0 = _mm_loadu_si128((__m128i *) &A[8*ii + 1*16]);
        c0 = _mm_loadu_si128((__m128i *) &A[8*ii + 2*16]);
        d0 = _mm_loadu_si128((__m128i *) &A[8*ii + 3*16]);
        e0 = _mm_loadu_si128((__m128i *) &A[8*ii + 4*16]);
        f0 = _mm_loadu_si128((__m128i *) &A[8*ii + 5*16]);
        g0 = _mm_loadu_si128((__m128i *) &A[8*ii + 6*16]);
        h0 = _mm_loadu_si128((__m128i *) &A[8*ii + 7*16]);

        a1 = _mm_unpacklo_epi8(a0, b0);
        b1 = _mm_unpackhi_epi8(a0, b0);
        c1 = _mm_unpacklo_epi8(c0, d0);
        d1 = _mm_unpackhi_epi8(c0, d0);
        e1 = _mm_unpacklo_epi8(e0, f0);
        f1 = _mm_unpackhi_epi8(e0, f0);
        g1 = _mm_unpacklo_epi8(g0, h0);
        h1 = _mm_unpackhi_epi8(g0, h0);

        a0 = _mm_unpacklo_epi8(a1, b1);
        b0 = _mm_unpackhi_epi8(a1, b1);
        c0 = _mm_unpacklo_epi8(c1, d1);
        d0 = _mm_unpackhi_epi8(c1, d1);
        e0 = _mm_unpacklo_epi8(e1, f1);
        f0 = _mm_unpackhi_epi8(e1, f1);
        g0 = _mm_unpacklo_epi8(g1, h1);
        h0 = _mm_unpackhi_epi8(g1, h1);

        a1 = _mm_unpacklo_epi32(a0, c0);
        b1 = _mm_unpackhi_epi32(a0, c0);
        c1 = _mm_unpacklo_epi32(b0, d0);
        d1 = _mm_unpackhi_epi32(b0, d0);
        e1 = _mm_unpacklo_epi32(e0, g0);
        f1 = _mm_unpackhi_epi32(e0, g0);
        g1 = _mm_unpacklo_epi32(f0, h0);
        h1 = _mm_unpackhi_epi32(f0, h0);

        a0 = _mm_unpacklo_epi64(a1, e1);
        b0 = _mm_unpackhi_epi64(a1, e1);
        c0 = _mm_unpacklo_epi64(b1, f1);
        d0 = _mm_unpackhi_epi64(b1, f1);
        e0 = _mm_unpacklo_epi64(c1, g1);
        f0 = _mm_unpackhi_epi64(c1, g1);
        g0 = _mm_unpacklo_epi64(d1, h1);
        h0 = _mm_unpackhi_epi64(d1, h1);

        _mm_storeu_si128((__m128i *) &B[0*size + ii], a0);
        _mm_storeu_si128((__m128i *) &B[1*size + ii], b0);
        _mm_storeu_si128((__m128i *) &B[2*size + ii], c0);
        _mm_storeu_si128((__m128i *) &B[3*size + ii], d0);
        _mm_storeu_si128((__m128i *) &B[4*size + ii], e0);
        _mm_storeu_si128((__m128i *) &B[5*size + ii], f0);
        _mm_storeu_si128((__m128i *) &B[6*size + ii], g0);
        _mm_storeu_si128((__m128i *) &B[7*size + ii], h0);
    }
    return bshuf_trans_byte_elem_remainder(in, out, size, 8,
            size - size % 16);
}


/* Transpose bytes within elements using best SSE algorithm available. */
int64_t bshuf_trans_byte_elem_SSE(void* in, void* out, const size_t size,
         const size_t elem_size) {

    int64_t count;

    // Trivial cases: power of 2 bytes.
    switch (elem_size) {
        case 1:
            count = bshuf_copy(in, out, size, elem_size);
            return count;
        case 2:
            count = bshuf_trans_byte_elem_SSE_16(in, out, size);
            return count;
        case 4:
            count = bshuf_trans_byte_elem_SSE_32(in, out, size);
            return count;
        case 8:
            count = bshuf_trans_byte_elem_SSE_64(in, out, size);
            return count;
    }

    // Worst case: odd number of bytes. Turns out that this is faster for
    // (odd * 2) byte elements as well (hense % 4).
    if (elem_size % 4) {
        count = bshuf_trans_byte_elem_scal(in, out, size, elem_size);
        return count;
    }

    // Multiple of power of 2: transpose hierarchically.
    {
        size_t nchunk_elem;
        void* tmp_buf = malloc(size * elem_size);
        if (tmp_buf == NULL) return -1;

        if ((elem_size % 8) == 0) {
            nchunk_elem = elem_size / 8;
            TRANS_ELEM_TYPE(in, out, size, nchunk_elem, int64_t);
            count = bshuf_trans_byte_elem_SSE_64(out, tmp_buf,
                    size * nchunk_elem);
            bshuf_trans_elem(tmp_buf, out, 8, nchunk_elem, size);
        } else if ((elem_size % 4) == 0) {
            nchunk_elem = elem_size / 4;
            TRANS_ELEM_TYPE(in, out, size, nchunk_elem, int32_t);
            count = bshuf_trans_byte_elem_SSE_32(out, tmp_buf,
                    size * nchunk_elem);
            bshuf_trans_elem(tmp_buf, out, 4, nchunk_elem, size);
        } else {
            // Not used since scalar algorithm is faster.
            nchunk_elem = elem_size / 2;
            TRANS_ELEM_TYPE(in, out, size, nchunk_elem, int16_t);
            count = bshuf_trans_byte_elem_SSE_16(out, tmp_buf,
                    size * nchunk_elem);
            bshuf_trans_elem(tmp_buf, out, 2, nchunk_elem, size);
        }

        free(tmp_buf);
        return count;
    }
}


/* Transpose bits within bytes using SSE. */
int64_t bshuf_trans_bit_byte_SSE(void* in, void* out, const size_t size,
         const size_t elem_size) {

    char* A = (char*) in;
    char* B = (char*) out;
    uint16_t* Bui;

    int64_t count;

    size_t nbyte = elem_size * size;

    CHECK_MULT_EIGHT(nbyte);

    __m128i xmm;
    int bt;

    for (size_t ii = 0; ii + 15 < nbyte; ii += 16) {
        xmm = _mm_loadu_si128((__m128i *) &A[ii]);
        for (size_t kk = 0; kk < 8; kk++) {
            bt = _mm_movemask_epi8(xmm);
            xmm = _mm_slli_epi16(xmm, 1);
            Bui = (uint16_t*) &B[((7 - kk) * nbyte + ii) / 8];
            *Bui = bt;
        }
    }
    count = bshuf_trans_bit_byte_remainder(in, out, size, elem_size,
            nbyte - nbyte % 16);
    return count;
}


/* Tranpose bits within elements. */
int64_t bshuf_trans_bit_elem_SSE(void* in, void* out, const size_t size,
         const size_t elem_size) {

    int64_t count;

    CHECK_MULT_EIGHT(size);

    void* tmp_buf = malloc(size * elem_size);
    if (tmp_buf == NULL) return -1;

    count = bshuf_trans_byte_elem_SSE(in, out, size, elem_size);
    CHECK_ERR_FREE(count, tmp_buf);
    count = bshuf_trans_bit_byte_SSE(out, tmp_buf, size, elem_size);
    CHECK_ERR_FREE(count, tmp_buf);
    count = bshuf_trans_bitrow_eight(tmp_buf, out, size, elem_size);

    free(tmp_buf);

    return count;
}


/* For data organized into a row for each bit (8 * elem_size rows), transpose
 * the bytes. */
int64_t bshuf_trans_byte_bitrow_SSE(void* in, void* out, const size_t size,
         const size_t elem_size) {

    char* A = (char*) in;
    char* B = (char*) out;

    CHECK_MULT_EIGHT(size);

    size_t nrows = 8 * elem_size;
    size_t nbyte_row = size / 8;

    __m128i a0, b0, c0, d0, e0, f0, g0, h0;
    __m128i a1, b1, c1, d1, e1, f1, g1, h1;
    __m128 *as, *bs, *cs, *ds, *es, *fs, *gs, *hs;

    for (size_t ii = 0; ii + 7 < nrows; ii += 8) {
        for (size_t jj = 0; jj + 15 < nbyte_row; jj += 16) {
            a0 = _mm_loadu_si128((__m128i *) &A[(ii + 0)*nbyte_row + jj]);
            b0 = _mm_loadu_si128((__m128i *) &A[(ii + 1)*nbyte_row + jj]);
            c0 = _mm_loadu_si128((__m128i *) &A[(ii + 2)*nbyte_row + jj]);
            d0 = _mm_loadu_si128((__m128i *) &A[(ii + 3)*nbyte_row + jj]);
            e0 = _mm_loadu_si128((__m128i *) &A[(ii + 4)*nbyte_row + jj]);
            f0 = _mm_loadu_si128((__m128i *) &A[(ii + 5)*nbyte_row + jj]);
            g0 = _mm_loadu_si128((__m128i *) &A[(ii + 6)*nbyte_row + jj]);
            h0 = _mm_loadu_si128((__m128i *) &A[(ii + 7)*nbyte_row + jj]);


            a1 = _mm_unpacklo_epi8(a0, b0);
            b1 = _mm_unpacklo_epi8(c0, d0);
            c1 = _mm_unpacklo_epi8(e0, f0);
            d1 = _mm_unpacklo_epi8(g0, h0);
            e1 = _mm_unpackhi_epi8(a0, b0);
            f1 = _mm_unpackhi_epi8(c0, d0);
            g1 = _mm_unpackhi_epi8(e0, f0);
            h1 = _mm_unpackhi_epi8(g0, h0);


            a0 = _mm_unpacklo_epi16(a1, b1);
            b0 = _mm_unpacklo_epi16(c1, d1);
            c0 = _mm_unpackhi_epi16(a1, b1);
            d0 = _mm_unpackhi_epi16(c1, d1);

            e0 = _mm_unpacklo_epi16(e1, f1);
            f0 = _mm_unpacklo_epi16(g1, h1);
            g0 = _mm_unpackhi_epi16(e1, f1);
            h0 = _mm_unpackhi_epi16(g1, h1);


            a1 = _mm_unpacklo_epi32(a0, b0);
            b1 = _mm_unpackhi_epi32(a0, b0);

            c1 = _mm_unpacklo_epi32(c0, d0);
            d1 = _mm_unpackhi_epi32(c0, d0);

            e1 = _mm_unpacklo_epi32(e0, f0);
            f1 = _mm_unpackhi_epi32(e0, f0);

            g1 = _mm_unpacklo_epi32(g0, h0);
            h1 = _mm_unpackhi_epi32(g0, h0);

            // We don't have a storeh instruction for integers, so interpret
            // as a float. Have a storel (_mm_storel_epi64).
            as = (__m128 *) &a1;
            bs = (__m128 *) &b1;
            cs = (__m128 *) &c1;
            ds = (__m128 *) &d1;
            es = (__m128 *) &e1;
            fs = (__m128 *) &f1;
            gs = (__m128 *) &g1;
            hs = (__m128 *) &h1;

            _mm_storel_pi((__m64 *) &B[(jj + 0) * nrows + ii], *as);
            _mm_storel_pi((__m64 *) &B[(jj + 2) * nrows + ii], *bs);
            _mm_storel_pi((__m64 *) &B[(jj + 4) * nrows + ii], *cs);
            _mm_storel_pi((__m64 *) &B[(jj + 6) * nrows + ii], *ds);
            _mm_storel_pi((__m64 *) &B[(jj + 8) * nrows + ii], *es);
            _mm_storel_pi((__m64 *) &B[(jj + 10) * nrows + ii], *fs);
            _mm_storel_pi((__m64 *) &B[(jj + 12) * nrows + ii], *gs);
            _mm_storel_pi((__m64 *) &B[(jj + 14) * nrows + ii], *hs);

            _mm_storeh_pi((__m64 *) &B[(jj + 1) * nrows + ii], *as);
            _mm_storeh_pi((__m64 *) &B[(jj + 3) * nrows + ii], *bs);
            _mm_storeh_pi((__m64 *) &B[(jj + 5) * nrows + ii], *cs);
            _mm_storeh_pi((__m64 *) &B[(jj + 7) * nrows + ii], *ds);
            _mm_storeh_pi((__m64 *) &B[(jj + 9) * nrows + ii], *es);
            _mm_storeh_pi((__m64 *) &B[(jj + 11) * nrows + ii], *fs);
            _mm_storeh_pi((__m64 *) &B[(jj + 13) * nrows + ii], *gs);
            _mm_storeh_pi((__m64 *) &B[(jj + 15) * nrows + ii], *hs);
        }
        for (size_t jj = nbyte_row - nbyte_row % 16; jj < nbyte_row; jj ++) {
            B[jj * nrows + ii + 0] = A[(ii + 0)*nbyte_row + jj];
            B[jj * nrows + ii + 1] = A[(ii + 1)*nbyte_row + jj];
            B[jj * nrows + ii + 2] = A[(ii + 2)*nbyte_row + jj];
            B[jj * nrows + ii + 3] = A[(ii + 3)*nbyte_row + jj];
            B[jj * nrows + ii + 4] = A[(ii + 4)*nbyte_row + jj];
            B[jj * nrows + ii + 5] = A[(ii + 5)*nbyte_row + jj];
            B[jj * nrows + ii + 6] = A[(ii + 6)*nbyte_row + jj];
            B[jj * nrows + ii + 7] = A[(ii + 7)*nbyte_row + jj];
        }
    }
    return size * elem_size;
}


/* Shuffle bits within the bytes of eight element blocks. */
int64_t bshuf_shuffle_bit_eightelem_SSE(void* in, void* out, const size_t size,
         const size_t elem_size) {

    CHECK_MULT_EIGHT(size);

    // With a bit of care, this could be written such that such that it is
    // in_buf = out_buf safe.
    char* A = (char*) in;
    char* B = (char*) out;
    uint16_t* Bui = (uint16_t*) out;

    size_t nbyte = elem_size * size;

    __m128i xmm;
    int bt;
    uint64_t x, t;

    if (elem_size % 2) {
        for (size_t ii = 0; ii + 8 * elem_size - 1 < nbyte;
                ii += 8 * elem_size) {
            for (size_t jj = 0; jj < 8 * elem_size; jj += 8) {
                x = *((uint64_t*) &A[ii + jj]);
                TRANS_BIT_8X8(x, t);
                for (size_t kk = 0; kk < 8; kk++) {
                    *((uint8_t*) &B[ii + jj / 8 + kk * elem_size]) = x;
                    x = x >> 8;
                }
            }
        }
    } else {
        for (size_t ii = 0; ii + 8 * elem_size - 1 < nbyte;
                ii += 8 * elem_size) {
            for (size_t jj = 0; jj + 15 < 8 * elem_size; jj += 16) {
                xmm = _mm_loadu_si128((__m128i *) &A[ii + jj]);
                for (size_t kk = 0; kk < 8; kk++) {
                    bt = _mm_movemask_epi8(xmm);
                    xmm = _mm_slli_epi16(xmm, 1);
                    size_t ind = (ii + jj / 8 + (7 - kk) * elem_size);
                    Bui[ind / 2] = bt;
                }
            }
        }
    }
    return size * elem_size;
}


int64_t bshuf_untrans_bit_elem_SSE(void* in, void* out, const size_t size,
         const size_t elem_size) {

    int64_t count;

    CHECK_MULT_EIGHT(size);

    void* tmp_buf = malloc(size * elem_size);
    if (tmp_buf == NULL) return -1;

    count = bshuf_trans_byte_bitrow_SSE(in, tmp_buf, size, elem_size);
    CHECK_ERR_FREE(count, tmp_buf);
    count =  bshuf_shuffle_bit_eightelem_SSE(tmp_buf, out, size, elem_size);

    free(tmp_buf);

    return count;
}

#else // #ifdef USESSE2


int64_t bshuf_untrans_bit_elem_SSE(void* in, void* out, const size_t size,
         const size_t elem_size) {
    return -11;
}


int64_t bshuf_trans_bit_elem_SSE(void* in, void* out, const size_t size,
         const size_t elem_size) {
    return -11;
}


int64_t bshuf_trans_byte_bitrow_SSE(void* in, void* out, const size_t size,
         const size_t elem_size) {
    return -11;
}


int64_t bshuf_trans_bit_byte_SSE(void* in, void* out, const size_t size,
         const size_t elem_size) {
    return -11;
}


int64_t bshuf_trans_byte_elem_SSE(void* in, void* out, const size_t size,
         const size_t elem_size) {
    return -11;
}


int64_t bshuf_trans_byte_elem_SSE_64(void* in, void* out, const size_t size) {
    return -11;
}


int64_t bshuf_trans_byte_elem_SSE_32(void* in, void* out, const size_t size) {
    return -11;
}


int64_t bshuf_trans_byte_elem_SSE_16(void* in, void* out, const size_t size) {
    return -11;
}


int64_t bshuf_shuffle_bit_eightelem_SSE(void* in, void* out, const size_t size,
         const size_t elem_size) {
    return -11;
}


#endif // #ifdef USESSE2


/* ---- Code that requires AVX2. Intel Haswell (2013) and later. ---- */

#ifdef USEAVX2

/* Transpose bits within bytes using AVX. */
int64_t bshuf_trans_bit_byte_AVX_unrolled(void* in, void* out,
        const size_t size, const size_t elem_size) {

    char* A = (char*) in;
    char* B = (char*) out;
    int32_t* Bui;

    int64_t count;

    size_t nbyte = elem_size * size;
    size_t kk;

    __m256i ymm0, ymm1, ymm2, ymm3, ymm4, ymm5, ymm6, ymm7;
    int32_t bt0, bt1, bt2, bt3, bt4, bt5, bt6, bt7;

    // Turns out that doubly unrolling this loop (unrolling 2 loops of 8) 
    // gives a speed up roughly 70% for some problem sizes.  The compiler will
    // not automatically doubly unroll a loop, but will optimize the
    // order of operations within one long section.
    for (size_t ii = 0; ii + 32 * 8 - 1 < nbyte; ii += 32 * 8) {
        ymm0 = _mm256_loadu_si256((__m256i *) &A[ii + 0*32]);
        ymm1 = _mm256_loadu_si256((__m256i *) &A[ii + 1*32]);
        ymm2 = _mm256_loadu_si256((__m256i *) &A[ii + 2*32]);
        ymm3 = _mm256_loadu_si256((__m256i *) &A[ii + 3*32]);
        ymm4 = _mm256_loadu_si256((__m256i *) &A[ii + 4*32]);
        ymm5 = _mm256_loadu_si256((__m256i *) &A[ii + 5*32]);
        ymm6 = _mm256_loadu_si256((__m256i *) &A[ii + 6*32]);
        ymm7 = _mm256_loadu_si256((__m256i *) &A[ii + 7*32]);

        kk = 0;
        Bui = (int32_t*) &B[((7 - kk) * nbyte + ii) / 8];
        bt0 = _mm256_movemask_epi8(ymm0);
        bt1 = _mm256_movemask_epi8(ymm1);
        bt2 = _mm256_movemask_epi8(ymm2);
        bt3 = _mm256_movemask_epi8(ymm3);
        bt4 = _mm256_movemask_epi8(ymm4);
        bt5 = _mm256_movemask_epi8(ymm5);
        bt6 = _mm256_movemask_epi8(ymm6);
        bt7 = _mm256_movemask_epi8(ymm7);
        ymm0 = _mm256_slli_epi16(ymm0, 1);
        ymm1 = _mm256_slli_epi16(ymm1, 1);
        ymm2 = _mm256_slli_epi16(ymm2, 1);
        ymm3 = _mm256_slli_epi16(ymm3, 1);
        ymm4 = _mm256_slli_epi16(ymm4, 1);
        ymm5 = _mm256_slli_epi16(ymm5, 1);
        ymm6 = _mm256_slli_epi16(ymm6, 1);
        ymm7 = _mm256_slli_epi16(ymm7, 1);
        Bui[0] = bt0;
        Bui[1] = bt1;
        Bui[2] = bt2;
        Bui[3] = bt3;
        Bui[4] = bt4;
        Bui[5] = bt5;
        Bui[6] = bt6;
        Bui[7] = bt7;

        kk = 1;
        Bui = (int32_t*) &B[((7 - kk) * nbyte + ii) / 8];
        bt0 = _mm256_movemask_epi8(ymm0);
        bt1 = _mm256_movemask_epi8(ymm1);
        bt2 = _mm256_movemask_epi8(ymm2);
        bt3 = _mm256_movemask_epi8(ymm3);
        bt4 = _mm256_movemask_epi8(ymm4);
        bt5 = _mm256_movemask_epi8(ymm5);
        bt6 = _mm256_movemask_epi8(ymm6);
        bt7 = _mm256_movemask_epi8(ymm7);
        ymm0 = _mm256_slli_epi16(ymm0, 1);
        ymm1 = _mm256_slli_epi16(ymm1, 1);
        ymm2 = _mm256_slli_epi16(ymm2, 1);
        ymm3 = _mm256_slli_epi16(ymm3, 1);
        ymm4 = _mm256_slli_epi16(ymm4, 1);
        ymm5 = _mm256_slli_epi16(ymm5, 1);
        ymm6 = _mm256_slli_epi16(ymm6, 1);
        ymm7 = _mm256_slli_epi16(ymm7, 1);
        Bui[0] = bt0;
        Bui[1] = bt1;
        Bui[2] = bt2;
        Bui[3] = bt3;
        Bui[4] = bt4;
        Bui[5] = bt5;
        Bui[6] = bt6;
        Bui[7] = bt7;

        kk = 2;
        Bui = (int32_t*) &B[((7 - kk) * nbyte + ii) / 8];
        bt0 = _mm256_movemask_epi8(ymm0);
        bt1 = _mm256_movemask_epi8(ymm1);
        bt2 = _mm256_movemask_epi8(ymm2);
        bt3 = _mm256_movemask_epi8(ymm3);
        bt4 = _mm256_movemask_epi8(ymm4);
        bt5 = _mm256_movemask_epi8(ymm5);
        bt6 = _mm256_movemask_epi8(ymm6);
        bt7 = _mm256_movemask_epi8(ymm7);
        ymm0 = _mm256_slli_epi16(ymm0, 1);
        ymm1 = _mm256_slli_epi16(ymm1, 1);
        ymm2 = _mm256_slli_epi16(ymm2, 1);
        ymm3 = _mm256_slli_epi16(ymm3, 1);
        ymm4 = _mm256_slli_epi16(ymm4, 1);
        ymm5 = _mm256_slli_epi16(ymm5, 1);
        ymm6 = _mm256_slli_epi16(ymm6, 1);
        ymm7 = _mm256_slli_epi16(ymm7, 1);
        Bui[0] = bt0;
        Bui[1] = bt1;
        Bui[2] = bt2;
        Bui[3] = bt3;
        Bui[4] = bt4;
        Bui[5] = bt5;
        Bui[6] = bt6;
        Bui[7] = bt7;

        kk = 3;
        Bui = (int32_t*) &B[((7 - kk) * nbyte + ii) / 8];
        bt0 = _mm256_movemask_epi8(ymm0);
        bt1 = _mm256_movemask_epi8(ymm1);
        bt2 = _mm256_movemask_epi8(ymm2);
        bt3 = _mm256_movemask_epi8(ymm3);
        bt4 = _mm256_movemask_epi8(ymm4);
        bt5 = _mm256_movemask_epi8(ymm5);
        bt6 = _mm256_movemask_epi8(ymm6);
        bt7 = _mm256_movemask_epi8(ymm7);
        ymm0 = _mm256_slli_epi16(ymm0, 1);
        ymm1 = _mm256_slli_epi16(ymm1, 1);
        ymm2 = _mm256_slli_epi16(ymm2, 1);
        ymm3 = _mm256_slli_epi16(ymm3, 1);
        ymm4 = _mm256_slli_epi16(ymm4, 1);
        ymm5 = _mm256_slli_epi16(ymm5, 1);
        ymm6 = _mm256_slli_epi16(ymm6, 1);
        ymm7 = _mm256_slli_epi16(ymm7, 1);
        Bui[0] = bt0;
        Bui[1] = bt1;
        Bui[2] = bt2;
        Bui[3] = bt3;
        Bui[4] = bt4;
        Bui[5] = bt5;
        Bui[6] = bt6;
        Bui[7] = bt7;

        kk = 4;
        Bui = (int32_t*) &B[((7 - kk) * nbyte + ii) / 8];
        bt0 = _mm256_movemask_epi8(ymm0);
        bt1 = _mm256_movemask_epi8(ymm1);
        bt2 = _mm256_movemask_epi8(ymm2);
        bt3 = _mm256_movemask_epi8(ymm3);
        bt4 = _mm256_movemask_epi8(ymm4);
        bt5 = _mm256_movemask_epi8(ymm5);
        bt6 = _mm256_movemask_epi8(ymm6);
        bt7 = _mm256_movemask_epi8(ymm7);
        ymm0 = _mm256_slli_epi16(ymm0, 1);
        ymm1 = _mm256_slli_epi16(ymm1, 1);
        ymm2 = _mm256_slli_epi16(ymm2, 1);
        ymm3 = _mm256_slli_epi16(ymm3, 1);
        ymm4 = _mm256_slli_epi16(ymm4, 1);
        ymm5 = _mm256_slli_epi16(ymm5, 1);
        ymm6 = _mm256_slli_epi16(ymm6, 1);
        ymm7 = _mm256_slli_epi16(ymm7, 1);
        Bui[0] = bt0;
        Bui[1] = bt1;
        Bui[2] = bt2;
        Bui[3] = bt3;
        Bui[4] = bt4;
        Bui[5] = bt5;
        Bui[6] = bt6;
        Bui[7] = bt7;

        kk = 5;
        Bui = (int32_t*) &B[((7 - kk) * nbyte + ii) / 8];
        bt0 = _mm256_movemask_epi8(ymm0);
        bt1 = _mm256_movemask_epi8(ymm1);
        bt2 = _mm256_movemask_epi8(ymm2);
        bt3 = _mm256_movemask_epi8(ymm3);
        bt4 = _mm256_movemask_epi8(ymm4);
        bt5 = _mm256_movemask_epi8(ymm5);
        bt6 = _mm256_movemask_epi8(ymm6);
        bt7 = _mm256_movemask_epi8(ymm7);
        ymm0 = _mm256_slli_epi16(ymm0, 1);
        ymm1 = _mm256_slli_epi16(ymm1, 1);
        ymm2 = _mm256_slli_epi16(ymm2, 1);
        ymm3 = _mm256_slli_epi16(ymm3, 1);
        ymm4 = _mm256_slli_epi16(ymm4, 1);
        ymm5 = _mm256_slli_epi16(ymm5, 1);
        ymm6 = _mm256_slli_epi16(ymm6, 1);
        ymm7 = _mm256_slli_epi16(ymm7, 1);
        Bui[0] = bt0;
        Bui[1] = bt1;
        Bui[2] = bt2;
        Bui[3] = bt3;
        Bui[4] = bt4;
        Bui[5] = bt5;
        Bui[6] = bt6;
        Bui[7] = bt7;

        kk = 6;
        Bui = (int32_t*) &B[((7 - kk) * nbyte + ii) / 8];
        bt0 = _mm256_movemask_epi8(ymm0);
        bt1 = _mm256_movemask_epi8(ymm1);
        bt2 = _mm256_movemask_epi8(ymm2);
        bt3 = _mm256_movemask_epi8(ymm3);
        bt4 = _mm256_movemask_epi8(ymm4);
        bt5 = _mm256_movemask_epi8(ymm5);
        bt6 = _mm256_movemask_epi8(ymm6);
        bt7 = _mm256_movemask_epi8(ymm7);
        ymm0 = _mm256_slli_epi16(ymm0, 1);
        ymm1 = _mm256_slli_epi16(ymm1, 1);
        ymm2 = _mm256_slli_epi16(ymm2, 1);
        ymm3 = _mm256_slli_epi16(ymm3, 1);
        ymm4 = _mm256_slli_epi16(ymm4, 1);
        ymm5 = _mm256_slli_epi16(ymm5, 1);
        ymm6 = _mm256_slli_epi16(ymm6, 1);
        ymm7 = _mm256_slli_epi16(ymm7, 1);
        Bui[0] = bt0;
        Bui[1] = bt1;
        Bui[2] = bt2;
        Bui[3] = bt3;
        Bui[4] = bt4;
        Bui[5] = bt5;
        Bui[6] = bt6;
        Bui[7] = bt7;

        kk = 7;
        Bui = (int32_t*) &B[((7 - kk) * nbyte + ii) / 8];
        bt0 = _mm256_movemask_epi8(ymm0);
        bt1 = _mm256_movemask_epi8(ymm1);
        bt2 = _mm256_movemask_epi8(ymm2);
        bt3 = _mm256_movemask_epi8(ymm3);
        bt4 = _mm256_movemask_epi8(ymm4);
        bt5 = _mm256_movemask_epi8(ymm5);
        bt6 = _mm256_movemask_epi8(ymm6);
        bt7 = _mm256_movemask_epi8(ymm7);
        ymm0 = _mm256_slli_epi16(ymm0, 1);
        ymm1 = _mm256_slli_epi16(ymm1, 1);
        ymm2 = _mm256_slli_epi16(ymm2, 1);
        ymm3 = _mm256_slli_epi16(ymm3, 1);
        ymm4 = _mm256_slli_epi16(ymm4, 1);
        ymm5 = _mm256_slli_epi16(ymm5, 1);
        ymm6 = _mm256_slli_epi16(ymm6, 1);
        ymm7 = _mm256_slli_epi16(ymm7, 1);
        Bui[0] = bt0;
        Bui[1] = bt1;
        Bui[2] = bt2;
        Bui[3] = bt3;
        Bui[4] = bt4;
        Bui[5] = bt5;
        Bui[6] = bt6;
        Bui[7] = bt7;
    }
    count = bshuf_trans_bit_byte_remainder(in, out, size, elem_size,
            nbyte - nbyte % (32 * 8));
    return count;
}


/* Transpose bits within bytes using AVX. Less optimized version. */
int64_t bshuf_trans_bit_byte_AVX(void* in, void* out, const size_t size,
         const size_t elem_size) {

    char* A = (char*) in;
    char* B = (char*) out;
    uint32_t* Bui;

    size_t nbyte = elem_size * size;

    int64_t count;

    __m256i ymm;
    int bt;

    for (size_t ii = 0; ii + 31 < nbyte; ii += 32) {
        ymm = _mm256_loadu_si256((__m256i *) &A[ii]);
        for (size_t kk = 0; kk < 8; kk++) {
            bt = _mm256_movemask_epi8(ymm);
            ymm = _mm256_slli_epi16(ymm, 1);
            Bui = (uint32_t*) &B[((7 - kk) * nbyte + ii) / 8];
            *Bui = bt;
        }
    }
    count = bshuf_trans_bit_byte_remainder(in, out, size, elem_size,
            nbyte - nbyte % 32);
    return count;
}


/* Transpose bits within elements. */
int64_t bshuf_trans_bit_elem_AVX(void* in, void* out, const size_t size,
         const size_t elem_size) {

    int64_t count;

    CHECK_MULT_EIGHT(size);

    void* tmp_buf = malloc(size * elem_size);
    if (tmp_buf == NULL) return -1;

    count = bshuf_trans_byte_elem_SSE(in, out, size, elem_size);
    CHECK_ERR_FREE(count, tmp_buf);
    count = bshuf_trans_bit_byte_AVX(out, tmp_buf, size, elem_size);
    CHECK_ERR_FREE(count, tmp_buf);
    count = bshuf_trans_bitrow_eight(tmp_buf, out, size, elem_size);

    free(tmp_buf);

    return count;
}


int64_t bshuf_untrans_bit_byte_AVX(void* in, void* out, const size_t size,
         const size_t elem_size) {

    CHECK_MULT_EIGHT(size);

    char* in_b = (char*) in;
    char* out_b = (char*) out;
    //uint32_t* out_ui32 = (uint32_t*) out;

    size_t brow = size / 8;    // Bytes in a "bitrow".
    size_t start_ind;

    __m256i ymm0, ymm1, ymm2, ymm3, ymm4, ymm5, ymm6, ymm7, ymm8, ymm9, ymm10,
            ymm11, ymm12, ymm13, ymm14, ymm15;

    int i0, i1, i2, i3, i4, i5, i6, i7;

    // Loop over bytes within elements.
    for (size_t ii = 0; ii < elem_size; ii += 1) {
        // Loop over elements in groups of 256 (packed into 32 bytes).
        for (size_t jj = 0; jj + 31 < size / 8; jj += 32) {
            // Load.
            start_ind = ii * size + jj;
            ymm0 = _mm256_loadu_si256((__m256i *) &in_b[start_ind + 0 * brow]);
            ymm1 = _mm256_loadu_si256((__m256i *) &in_b[start_ind + 1 * brow]);
            ymm2 = _mm256_loadu_si256((__m256i *) &in_b[start_ind + 2 * brow]);
            ymm3 = _mm256_loadu_si256((__m256i *) &in_b[start_ind + 3 * brow]);
            ymm4 = _mm256_loadu_si256((__m256i *) &in_b[start_ind + 4 * brow]);
            ymm5 = _mm256_loadu_si256((__m256i *) &in_b[start_ind + 5 * brow]);
            ymm6 = _mm256_loadu_si256((__m256i *) &in_b[start_ind + 6 * brow]);
            ymm7 = _mm256_loadu_si256((__m256i *) &in_b[start_ind + 7 * brow]);

            // Lace 8 rows together for grouping into bytes.
            ymm8 = _mm256_unpacklo_epi8(ymm0, ymm1);
            ymm9 = _mm256_unpacklo_epi8(ymm2, ymm3);
            ymm10 = _mm256_unpacklo_epi8(ymm4, ymm5);
            ymm11 = _mm256_unpacklo_epi8(ymm6, ymm7);
            ymm12 = _mm256_unpackhi_epi8(ymm0, ymm1);
            ymm13 = _mm256_unpackhi_epi8(ymm2, ymm3);
            ymm14 = _mm256_unpackhi_epi8(ymm4, ymm5);
            ymm15 = _mm256_unpackhi_epi8(ymm6, ymm7);

            ymm0 = _mm256_unpacklo_epi16(ymm8, ymm9);
            ymm1 = _mm256_unpacklo_epi16(ymm10, ymm11);
            ymm2 = _mm256_unpackhi_epi16(ymm8, ymm9);
            ymm3 = _mm256_unpackhi_epi16(ymm10, ymm11);

            ymm4 = _mm256_unpacklo_epi16(ymm12, ymm13);
            ymm5 = _mm256_unpacklo_epi16(ymm14, ymm15);
            ymm6 = _mm256_unpackhi_epi16(ymm12, ymm13);
            ymm7 = _mm256_unpackhi_epi16(ymm14, ymm15);

            ymm8 = _mm256_unpacklo_epi32(ymm0, ymm1);
            ymm9 = _mm256_unpackhi_epi32(ymm0, ymm1);

            ymm10 = _mm256_unpacklo_epi32(ymm2, ymm3);
            ymm11 = _mm256_unpackhi_epi32(ymm2, ymm3);

            ymm12 = _mm256_unpacklo_epi32(ymm4, ymm5);
            ymm13 = _mm256_unpackhi_epi32(ymm4, ymm5);

            ymm14 = _mm256_unpacklo_epi32(ymm6, ymm7);
            ymm15 = _mm256_unpackhi_epi32(ymm6, ymm7);

            // Bit transpose.
            i0 = _mm256_movemask_epi8(ymm8);
            i1 = _mm256_movemask_epi8(ymm9);
            i2 = _mm256_movemask_epi8(ymm10);
            i3 = _mm256_movemask_epi8(ymm11);
            i4 = _mm256_movemask_epi8(ymm12);
            i5 = _mm256_movemask_epi8(ymm13);
            i6 = _mm256_movemask_epi8(ymm14);
            i7 = _mm256_movemask_epi8(ymm15);
            ymm8 = _mm256_slli_epi16(ymm8, 1);
            ymm9 = _mm256_slli_epi16(ymm9, 1);
            ymm10 = _mm256_slli_epi16(ymm10, 1);
            ymm11 = _mm256_slli_epi16(ymm11, 1);
            ymm12 = _mm256_slli_epi16(ymm12, 1);
            ymm13 = _mm256_slli_epi16(ymm13, 1);
            ymm14 = _mm256_slli_epi16(ymm14, 1);
            ymm15 = _mm256_slli_epi16(ymm15, 1);
            // Weird order here is because the 256 bit unpack instructions act
            // as two 128 bit instructions instead of on the whole register.
            ymm7 = _mm256_set_epi32(i7, i5, i3, i1, i6, i4, i2, i0);

            i0 = _mm256_movemask_epi8(ymm8);
            i1 = _mm256_movemask_epi8(ymm9);
            i2 = _mm256_movemask_epi8(ymm10);
            i3 = _mm256_movemask_epi8(ymm11);
            i4 = _mm256_movemask_epi8(ymm12);
            i5 = _mm256_movemask_epi8(ymm13);
            i6 = _mm256_movemask_epi8(ymm14);
            i7 = _mm256_movemask_epi8(ymm15);
            ymm8 = _mm256_slli_epi16(ymm8, 1);
            ymm9 = _mm256_slli_epi16(ymm9, 1);
            ymm10 = _mm256_slli_epi16(ymm10, 1);
            ymm11 = _mm256_slli_epi16(ymm11, 1);
            ymm12 = _mm256_slli_epi16(ymm12, 1);
            ymm13 = _mm256_slli_epi16(ymm13, 1);
            ymm14 = _mm256_slli_epi16(ymm14, 1);
            ymm15 = _mm256_slli_epi16(ymm15, 1);
            ymm6 = _mm256_set_epi32(i7, i5, i3, i1, i6, i4, i2, i0);

            i0 = _mm256_movemask_epi8(ymm8);
            i1 = _mm256_movemask_epi8(ymm9);
            i2 = _mm256_movemask_epi8(ymm10);
            i3 = _mm256_movemask_epi8(ymm11);
            i4 = _mm256_movemask_epi8(ymm12);
            i5 = _mm256_movemask_epi8(ymm13);
            i6 = _mm256_movemask_epi8(ymm14);
            i7 = _mm256_movemask_epi8(ymm15);
            ymm8 = _mm256_slli_epi16(ymm8, 1);
            ymm9 = _mm256_slli_epi16(ymm9, 1);
            ymm10 = _mm256_slli_epi16(ymm10, 1);
            ymm11 = _mm256_slli_epi16(ymm11, 1);
            ymm12 = _mm256_slli_epi16(ymm12, 1);
            ymm13 = _mm256_slli_epi16(ymm13, 1);
            ymm14 = _mm256_slli_epi16(ymm14, 1);
            ymm15 = _mm256_slli_epi16(ymm15, 1);
            ymm5 = _mm256_set_epi32(i7, i5, i3, i1, i6, i4, i2, i0);

            i0 = _mm256_movemask_epi8(ymm8);
            i1 = _mm256_movemask_epi8(ymm9);
            i2 = _mm256_movemask_epi8(ymm10);
            i3 = _mm256_movemask_epi8(ymm11);
            i4 = _mm256_movemask_epi8(ymm12);
            i5 = _mm256_movemask_epi8(ymm13);
            i6 = _mm256_movemask_epi8(ymm14);
            i7 = _mm256_movemask_epi8(ymm15);
            ymm8 = _mm256_slli_epi16(ymm8, 1);
            ymm9 = _mm256_slli_epi16(ymm9, 1);
            ymm10 = _mm256_slli_epi16(ymm10, 1);
            ymm11 = _mm256_slli_epi16(ymm11, 1);
            ymm12 = _mm256_slli_epi16(ymm12, 1);
            ymm13 = _mm256_slli_epi16(ymm13, 1);
            ymm14 = _mm256_slli_epi16(ymm14, 1);
            ymm15 = _mm256_slli_epi16(ymm15, 1);
            ymm4 = _mm256_set_epi32(i7, i5, i3, i1, i6, i4, i2, i0);

            i0 = _mm256_movemask_epi8(ymm8);
            i1 = _mm256_movemask_epi8(ymm9);
            i2 = _mm256_movemask_epi8(ymm10);
            i3 = _mm256_movemask_epi8(ymm11);
            i4 = _mm256_movemask_epi8(ymm12);
            i5 = _mm256_movemask_epi8(ymm13);
            i6 = _mm256_movemask_epi8(ymm14);
            i7 = _mm256_movemask_epi8(ymm15);
            ymm8 = _mm256_slli_epi16(ymm8, 1);
            ymm9 = _mm256_slli_epi16(ymm9, 1);
            ymm10 = _mm256_slli_epi16(ymm10, 1);
            ymm11 = _mm256_slli_epi16(ymm11, 1);
            ymm12 = _mm256_slli_epi16(ymm12, 1);
            ymm13 = _mm256_slli_epi16(ymm13, 1);
            ymm14 = _mm256_slli_epi16(ymm14, 1);
            ymm15 = _mm256_slli_epi16(ymm15, 1);
            ymm3 = _mm256_set_epi32(i7, i5, i3, i1, i6, i4, i2, i0);

            i0 = _mm256_movemask_epi8(ymm8);
            i1 = _mm256_movemask_epi8(ymm9);
            i2 = _mm256_movemask_epi8(ymm10);
            i3 = _mm256_movemask_epi8(ymm11);
            i4 = _mm256_movemask_epi8(ymm12);
            i5 = _mm256_movemask_epi8(ymm13);
            i6 = _mm256_movemask_epi8(ymm14);
            i7 = _mm256_movemask_epi8(ymm15);
            ymm8 = _mm256_slli_epi16(ymm8, 1);
            ymm9 = _mm256_slli_epi16(ymm9, 1);
            ymm10 = _mm256_slli_epi16(ymm10, 1);
            ymm11 = _mm256_slli_epi16(ymm11, 1);
            ymm12 = _mm256_slli_epi16(ymm12, 1);
            ymm13 = _mm256_slli_epi16(ymm13, 1);
            ymm14 = _mm256_slli_epi16(ymm14, 1);
            ymm15 = _mm256_slli_epi16(ymm15, 1);
            ymm2 = _mm256_set_epi32(i7, i5, i3, i1, i6, i4, i2, i0);

            i0 = _mm256_movemask_epi8(ymm8);
            i1 = _mm256_movemask_epi8(ymm9);
            i2 = _mm256_movemask_epi8(ymm10);
            i3 = _mm256_movemask_epi8(ymm11);
            i4 = _mm256_movemask_epi8(ymm12);
            i5 = _mm256_movemask_epi8(ymm13);
            i6 = _mm256_movemask_epi8(ymm14);
            i7 = _mm256_movemask_epi8(ymm15);
            ymm8 = _mm256_slli_epi16(ymm8, 1);
            ymm9 = _mm256_slli_epi16(ymm9, 1);
            ymm10 = _mm256_slli_epi16(ymm10, 1);
            ymm11 = _mm256_slli_epi16(ymm11, 1);
            ymm12 = _mm256_slli_epi16(ymm12, 1);
            ymm13 = _mm256_slli_epi16(ymm13, 1);
            ymm14 = _mm256_slli_epi16(ymm14, 1);
            ymm15 = _mm256_slli_epi16(ymm15, 1);
            ymm1 = _mm256_set_epi32(i7, i5, i3, i1, i6, i4, i2, i0);

            i0 = _mm256_movemask_epi8(ymm8);
            i1 = _mm256_movemask_epi8(ymm9);
            i2 = _mm256_movemask_epi8(ymm10);
            i3 = _mm256_movemask_epi8(ymm11);
            i4 = _mm256_movemask_epi8(ymm12);
            i5 = _mm256_movemask_epi8(ymm13);
            i6 = _mm256_movemask_epi8(ymm14);
            i7 = _mm256_movemask_epi8(ymm15);
            ymm0 = _mm256_set_epi32(i7, i5, i3, i1, i6, i4, i2, i0);

            // Lace bytes together in proper order.
            ymm8 = _mm256_unpacklo_epi8(ymm0, ymm1);
            ymm9 = _mm256_unpacklo_epi8(ymm2, ymm3);
            ymm10 = _mm256_unpacklo_epi8(ymm4, ymm5);
            ymm11 = _mm256_unpacklo_epi8(ymm6, ymm7);
            ymm12 = _mm256_unpackhi_epi8(ymm0, ymm1);
            ymm13 = _mm256_unpackhi_epi8(ymm2, ymm3);
            ymm14 = _mm256_unpackhi_epi8(ymm4, ymm5);
            ymm15 = _mm256_unpackhi_epi8(ymm6, ymm7);

            ymm0 = _mm256_unpacklo_epi16(ymm8, ymm9);
            ymm1 = _mm256_unpacklo_epi16(ymm10, ymm11);
            ymm2 = _mm256_unpackhi_epi16(ymm8, ymm9);
            ymm3 = _mm256_unpackhi_epi16(ymm10, ymm11);

            ymm4 = _mm256_unpacklo_epi16(ymm12, ymm13);
            ymm5 = _mm256_unpacklo_epi16(ymm14, ymm15);
            ymm6 = _mm256_unpackhi_epi16(ymm12, ymm13);
            ymm7 = _mm256_unpackhi_epi16(ymm14, ymm15);

            ymm8 = _mm256_unpacklo_epi32(ymm0, ymm1);
            ymm9 = _mm256_unpackhi_epi32(ymm0, ymm1);

            ymm10 = _mm256_unpacklo_epi32(ymm2, ymm3);
            ymm11 = _mm256_unpackhi_epi32(ymm2, ymm3);

            ymm12 = _mm256_unpacklo_epi32(ymm4, ymm5);
            ymm13 = _mm256_unpackhi_epi32(ymm4, ymm5);

            ymm14 = _mm256_unpacklo_epi32(ymm6, ymm7);
            ymm15 = _mm256_unpackhi_epi32(ymm6, ymm7);

            // Store.
            start_ind = ii * size + jj * 8;
            _mm256_storeu_si256((__m256i *) &out_b[start_ind + 0 * 32], ymm8);
            _mm256_storeu_si256((__m256i *) &out_b[start_ind + 1 * 32], ymm10);
            _mm256_storeu_si256((__m256i *) &out_b[start_ind + 2 * 32], ymm12);
            _mm256_storeu_si256((__m256i *) &out_b[start_ind + 3 * 32], ymm14);
            _mm256_storeu_si256((__m256i *) &out_b[start_ind + 4 * 32], ymm9);
            _mm256_storeu_si256((__m256i *) &out_b[start_ind + 5 * 32], ymm11);
            _mm256_storeu_si256((__m256i *) &out_b[start_ind + 6 * 32], ymm13);
            _mm256_storeu_si256((__m256i *) &out_b[start_ind + 7 * 32], ymm15);

        }
    }

    int64_t count = bshuf_untrans_bit_byte_remainder(in, out, size, elem_size,
            size - size % (32 * 8));

    return count;
}


/* For data organized into a row for each bit (8 * elem_size rows), transpose
 * the bytes. */
int64_t bshuf_trans_byte_bitrow_AVX(void* in, void* out, const size_t size,
         const size_t elem_size) {

    char* A = (char*) in;
    char* B = (char*) out;

    CHECK_MULT_EIGHT(size);

    size_t nrows = 8 * elem_size;
    size_t nbyte_row = size / 8;

    if (elem_size % 4) return bshuf_trans_byte_bitrow_SSE(in, out, size,
            elem_size);

    __m256i ymm_0[8];
    __m256i ymm_1[8];
    __m256i ymm_storeage[8][4];

    for (size_t jj = 0; jj + 31 < nbyte_row; jj += 32) {
        for (size_t ii = 0; ii + 3 < elem_size; ii += 4) {
            for (size_t hh = 0; hh < 4; hh ++) {

                for (size_t kk = 0; kk < 8; kk ++){
                    ymm_0[kk] = _mm256_loadu_si256((__m256i *) &A[
                            (ii * 8 + hh * 8 + kk) * nbyte_row + jj]);
                }

                for (size_t kk = 0; kk < 4; kk ++){
                    ymm_1[kk] = _mm256_unpacklo_epi8(ymm_0[kk * 2],
                            ymm_0[kk * 2 + 1]);
                    ymm_1[kk + 4] = _mm256_unpackhi_epi8(ymm_0[kk * 2],
                            ymm_0[kk * 2 + 1]);
                }

                for (size_t kk = 0; kk < 2; kk ++){
                    for (size_t mm = 0; mm < 2; mm ++){
                        ymm_0[kk * 4 + mm] = _mm256_unpacklo_epi16(
                                ymm_1[kk * 4 + mm * 2],
                                ymm_1[kk * 4 + mm * 2 + 1]);
                        ymm_0[kk * 4 + mm + 2] = _mm256_unpackhi_epi16(
                                ymm_1[kk * 4 + mm * 2],
                                ymm_1[kk * 4 + mm * 2 + 1]);
                    }
                }

                for (size_t kk = 0; kk < 4; kk ++){
                    ymm_1[kk * 2] = _mm256_unpacklo_epi32(ymm_0[kk * 2],
                            ymm_0[kk * 2 + 1]);
                    ymm_1[kk * 2 + 1] = _mm256_unpackhi_epi32(ymm_0[kk * 2],
                            ymm_0[kk * 2 + 1]);
                }

                for (size_t kk = 0; kk < 8; kk ++){
                    ymm_storeage[kk][hh] = ymm_1[kk];
                }
            }

            for (size_t mm = 0; mm < 8; mm ++) {

                for (size_t kk = 0; kk < 4; kk ++){
                    ymm_0[kk] = ymm_storeage[mm][kk];
                }

                ymm_1[0] = _mm256_unpacklo_epi64(ymm_0[0], ymm_0[1]);
                ymm_1[1] = _mm256_unpacklo_epi64(ymm_0[2], ymm_0[3]);
                ymm_1[2] = _mm256_unpackhi_epi64(ymm_0[0], ymm_0[1]);
                ymm_1[3] = _mm256_unpackhi_epi64(ymm_0[2], ymm_0[3]);

                ymm_0[0] = _mm256_permute2x128_si256(ymm_1[0], ymm_1[1], 32);
                ymm_0[1] = _mm256_permute2x128_si256(ymm_1[2], ymm_1[3], 32);
                ymm_0[2] = _mm256_permute2x128_si256(ymm_1[0], ymm_1[1], 49);
                ymm_0[3] = _mm256_permute2x128_si256(ymm_1[2], ymm_1[3], 49);

                _mm256_storeu_si256((__m256i *) &B[
                        (jj + mm * 2 + 0 * 16) * nrows + ii * 8], ymm_0[0]);
                _mm256_storeu_si256((__m256i *) &B[
                        (jj + mm * 2 + 0 * 16 + 1) * nrows + ii * 8], ymm_0[1]);
                _mm256_storeu_si256((__m256i *) &B[
                        (jj + mm * 2 + 1 * 16) * nrows + ii * 8], ymm_0[2]);
                _mm256_storeu_si256((__m256i *) &B[
                        (jj + mm * 2 + 1 * 16 + 1) * nrows + ii * 8], ymm_0[3]);
            }
        }
    }
    for (size_t ii = 0; ii < nrows; ii ++ ) {
        for (size_t jj = nbyte_row - nbyte_row % 32; jj < nbyte_row; jj ++) {
            B[jj * nrows + ii] = A[ii * nbyte_row + jj];
        }
    }
    return size * elem_size;
}


/* Shuffle bits within the bytes of eight element blocks. */
int64_t bshuf_shuffle_bit_eightelem_AVX(void* in, void* out, const size_t size,
         const size_t elem_size) {

    CHECK_MULT_EIGHT(size);

    // With a bit of care, this could be written such that such that it is
    // in_buf = out_buf safe.
    char* A = (char*) in;
    char* B = (char*) out;

    size_t nbyte = elem_size * size;

    __m256i ymm;
    int32_t bt;

    if (elem_size % 4) {
        return bshuf_shuffle_bit_eightelem_SSE(in, out, size, elem_size);
    } else {
        for (size_t jj = 0; jj + 31 < 8 * elem_size; jj += 32) {
            for (size_t ii = 0; ii + 8 * elem_size - 1 < nbyte;
                    ii += 8 * elem_size) {
                ymm = _mm256_loadu_si256((__m256i *) &A[ii + jj]);
                for (size_t kk = 0; kk < 8; kk++) {
                    bt = _mm256_movemask_epi8(ymm);
                    ymm = _mm256_slli_epi16(ymm, 1);
                    size_t ind = (ii + jj / 8 + (7 - kk) * elem_size);
                    * (int32_t *) &B[ind] = bt;
                }
            }
        }
    }
    return size * elem_size;
}


int64_t bshuf_untrans_bit_elem_AVX(void* in, void* out, const size_t size,
         const size_t elem_size) {

    int64_t count;

    CHECK_MULT_EIGHT(size);

    void* tmp_buf = malloc(size * elem_size);
    if (tmp_buf == NULL) return -1;

    count = bshuf_trans_byte_bitrow_AVX(in, tmp_buf, size, elem_size);
    CHECK_ERR_FREE(count, tmp_buf);
    count =  bshuf_shuffle_bit_eightelem_AVX(tmp_buf, out, size, elem_size);

    free(tmp_buf);
    return count;
}

#else // #ifdef USEAVX2


int64_t bshuf_trans_bit_byte_AVX_unrolled(void* in, void* out,
        const size_t size, const size_t elem_size) {
    return -12;
}


int64_t bshuf_trans_bit_byte_AVX(void* in, void* out, const size_t size,
         const size_t elem_size) {
    return -12;
}


int64_t bshuf_trans_bit_elem_AVX(void* in, void* out, const size_t size,
         const size_t elem_size) {
    return -12;
}

int64_t bshuf_untrans_bit_byte_AVX(void* in, void* out, const size_t size,
         const size_t elem_size) {
    return -12;
}


int64_t bshuf_trans_byte_bitrow_AVX(void* in, void* out, const size_t size,
         const size_t elem_size) {
    return -12;
}


int64_t bshuf_shuffle_bit_eightelem_AVX(void* in, void* out, const size_t size,
         const size_t elem_size) {
    return -12;
}


int64_t bshuf_untrans_bit_elem_AVX(void* in, void* out, const size_t size,
         const size_t elem_size) {
    return -12;
}


#endif // #ifdef USEAVX2


/* ---- Public functions ---- */

int64_t bshuf_trans_bit_elem(void* in, void* out, const size_t size, 
        const size_t elem_size) {

    int64_t count;
#ifdef USEAVX2
    count = bshuf_trans_bit_elem_AVX(in, out, size, elem_size);
#elif defined(USESSE2)
    count = bshuf_trans_bit_elem_SSE(in, out, size, elem_size);
#else
    count = bshuf_trans_bit_elem_scal(in, out, size, elem_size);
#endif
    return count;
}


int64_t bshuf_untrans_bit_elem(void* in, void* out, const size_t size, 
        const size_t elem_size) {

    int64_t count;
#ifdef USEAVX2
    count = bshuf_untrans_bit_elem_AVX(in, out, size, elem_size);
#elif defined(USESSE2)
    count = bshuf_untrans_bit_elem_SSE(in, out, size, elem_size);
#else
    count = bshuf_untrans_bit_elem_scal(in, out, size, elem_size);
#endif
    return count;
}


size_t bshuf_default_block_size(const size_t elem_size) {
    // This function needs to be absolutely stable between versions.
    // Otherwise encoded data will not be decodable.

    size_t block_size = BSHUF_TARGET_BLOCK_SIZE_B / elem_size;
    // Ensure it is a required multiple.
    block_size = (block_size / BSHUF_BLOCKED_MULT) * BSHUF_BLOCKED_MULT;
    return MAX(block_size, BSHUF_MIN_RECOMMEND_BLOCK);
}


int64_t bshuf_blocked_wrap_fun(bshufFunDef fun, void* in, void* out,
        const size_t size, const size_t elem_size, size_t block_size) {

    ioc_chain C;
    ioc_init(&C, in, out);

    int64_t err = 0, count, cum_count = 0;
    size_t last_block_size;

    if (block_size == 0) {
        block_size = bshuf_default_block_size(elem_size);
    }
    if (block_size < 0 || block_size % BSHUF_BLOCKED_MULT) return -81;

    #pragma omp parallel for private(count) reduction(+ : cum_count)
    for (size_t ii = 0; ii < size / block_size; ii ++) {
        count = fun(&C, block_size, elem_size);
        if (count < 0) err = count;
        cum_count += count;
    }

    last_block_size = size % block_size;
    last_block_size = last_block_size - last_block_size % BSHUF_BLOCKED_MULT;
    if (last_block_size) {
        count = fun(&C, last_block_size, elem_size);
        if (count < 0) err = count;
        cum_count += count;
    }

    if (err < 0) return err;

    size_t leftover_bytes = size % BSHUF_BLOCKED_MULT * elem_size;
    size_t this_iter;
    char *last_in = (char *) ioc_get_in(&C, &this_iter);
    ioc_set_next_in(&C, &this_iter, (void *) (last_in + leftover_bytes));
    char *last_out = (char *) ioc_get_out(&C, &this_iter);
    ioc_set_next_out(&C, &this_iter, (void *) (last_out + leftover_bytes));

    memcpy(last_out, last_in, leftover_bytes);

    ioc_destroy(&C);

    return cum_count + leftover_bytes;
}


int64_t bshuf_bitshuffle_block(ioc_chain *C_ptr,
        const size_t size, const size_t elem_size) {

    size_t this_iter;
    void *in = ioc_get_in(C_ptr, &this_iter);
    ioc_set_next_in(C_ptr, &this_iter, (void*) ((char*) in + size * elem_size));
    void *out = ioc_get_out(C_ptr, &this_iter);
    ioc_set_next_out(C_ptr, &this_iter, (void *) ((char *) out + size * elem_size));

    int64_t count = bshuf_trans_bit_elem(in, out, size, elem_size);
    return count;
}


int64_t bshuf_bitunshuffle_block(ioc_chain* C_ptr,
        const size_t size, const size_t elem_size) {


    size_t this_iter;
    void *in = ioc_get_in(C_ptr, &this_iter);
    ioc_set_next_in(C_ptr, &this_iter, (void*) ((char*) in + size * elem_size));
    void *out = ioc_get_out(C_ptr, &this_iter);
    ioc_set_next_out(C_ptr, &this_iter, (void *) ((char *) out + size * elem_size));

    int64_t count = bshuf_untrans_bit_elem(in, out, size, elem_size);
    return count;
}


int64_t bshuf_bitshuffle(void* in, void* out, const size_t size,
        const size_t elem_size, size_t block_size) {

    return bshuf_blocked_wrap_fun(&bshuf_bitshuffle_block, in, out, size,
            elem_size, block_size);
}


int64_t bshuf_bitunshuffle(void* in, void* out, const size_t size,
        const size_t elem_size, size_t block_size) {

    return bshuf_blocked_wrap_fun(&bshuf_bitunshuffle_block, in, out, size,
            elem_size, block_size);
}


/* ---- Utilities ---- */

/* Write a 64 bit unsigned integer to a buffer in big endian order. */
void bshuf_write_uint64_BE(void* buf, uint64_t num) {
    uint8_t* b = buf;
    uint64_t pow28 = 1 << 8;
    for (int ii = 7; ii >= 0; ii--) {
        b[ii] = num % pow28;
        num = num / pow28;
    }
}


/* Read a 64 bit unsigned integer from a buffer big endian order. */
uint64_t bshuf_read_uint64_BE(void* buf) {
    uint8_t* b = buf;
    uint64_t num = 0, pow28 = 1 << 8, cp = 1;
    for (int ii = 7; ii >= 0; ii--) {
        num += b[ii] * cp;
        cp *= pow28;
    }
    return num;
}


/* Write a 32 bit unsigned integer to a buffer in big endian order. */
void bshuf_write_uint32_BE(void* buf, uint32_t num) {
    uint8_t* b = buf;
    uint32_t pow28 = 1 << 8;
    for (int ii = 3; ii >= 0; ii--) {
        b[ii] = num % pow28;
        num = num / pow28;
    }
}


/* Read a 32 bit unsigned integer from a buffer big endian order. */
uint32_t bshuf_read_uint32_BE(void* buf) {
    uint8_t* b = buf;
    uint32_t num = 0, pow28 = 1 << 8, cp = 1;
    for (int ii = 3; ii >= 0; ii--) {
        num += b[ii] * cp;
        cp *= pow28;
    }
    return num;
}


size_t bshuf_compress_lz4_bound(const size_t size,
        const size_t elem_size, size_t block_size) {

    size_t bound, leftover;

    if (block_size == 0) {
        block_size = bshuf_default_block_size(elem_size);
    }
    if (block_size < 0 || block_size % BSHUF_BLOCKED_MULT) return -81;

    // Note that each block gets a 4 byte header.
    // Size of full blocks.
    bound = (LZ4_compressBound(block_size * elem_size) + 4) * (size / block_size);
    // Size of partial blocks, if any.
    leftover = ((size % block_size) / BSHUF_BLOCKED_MULT) * BSHUF_BLOCKED_MULT;
    if (leftover) bound += LZ4_compressBound(leftover * elem_size) + 4;
    // Size of uncompressed data not fitting into any blocks.
    bound += (size % BSHUF_BLOCKED_MULT) * elem_size;
    return bound;
}


int64_t bshuf_compress_lz4_block(ioc_chain *C_ptr,
        const size_t size, const size_t elem_size) {

    int64_t nbytes, count;

    void* tmp_buf_bshuf = malloc(size * elem_size);
    if (tmp_buf_bshuf == NULL) return -1;

    void* tmp_buf_lz4 = malloc(LZ4_compressBound(size * elem_size));
    if (tmp_buf_lz4 == NULL){
        free(tmp_buf_bshuf);
        return -1;
    }

    size_t this_iter;

    void *in = ioc_get_in(C_ptr, &this_iter);
    ioc_set_next_in(C_ptr, &this_iter, (void*) ((char*) in + size * elem_size));

    count = bshuf_trans_bit_elem(in, tmp_buf_bshuf, size, elem_size);
    if (count < 0) {
        free(tmp_buf_lz4);
        free(tmp_buf_bshuf);
        return count;
    }
    nbytes = LZ4_compress(tmp_buf_bshuf, tmp_buf_lz4, size * elem_size);
    free(tmp_buf_bshuf);
    CHECK_ERR_FREE_LZ(nbytes, tmp_buf_lz4);

    void *out = ioc_get_out(C_ptr, &this_iter);
    ioc_set_next_out(C_ptr, &this_iter, (void *) ((char *) out + nbytes + 4));

    bshuf_write_uint32_BE(out, nbytes);
    memcpy((char *) out + 4, tmp_buf_lz4, nbytes);

    free(tmp_buf_lz4);

    return nbytes + 4;
}



int64_t bshuf_decompress_lz4_block(ioc_chain *C_ptr,
        const size_t size, const size_t elem_size) {

    int64_t nbytes, count;

    size_t this_iter;
    void *in = ioc_get_in(C_ptr, &this_iter);
    int32_t nbytes_from_header = bshuf_read_uint32_BE(in);
    ioc_set_next_in(C_ptr, &this_iter, (void*) ((char*) in + nbytes_from_header + 4));

    void *out = ioc_get_out(C_ptr, &this_iter);
    ioc_set_next_out(C_ptr, &this_iter, (void *) ((char *) out + size * elem_size));

    void* tmp_buf = malloc(size * elem_size);
    if (tmp_buf == NULL) return -1;

#ifdef BSHUF_LZ4_DECOMPRESS_FAST
    nbytes = LZ4_decompress_fast((char*) in + 4, tmp_buf, size * elem_size);
    CHECK_ERR_FREE_LZ(nbytes, tmp_buf);
    if (nbytes != nbytes_from_header) {
        free(tmp_buf);
        return -91;
    }
#else
    nbytes = LZ4_decompress_safe((char*) in + 4, tmp_buf, nbytes_from_header,
                                 size * elem_size);
    CHECK_ERR_FREE_LZ(nbytes, tmp_buf);
    if (nbytes != size * elem_size) {
        free(tmp_buf);
        return -91;
    }
    nbytes = nbytes_from_header;
#endif
    count = bshuf_untrans_bit_elem(tmp_buf, out, size, elem_size);
    CHECK_ERR_FREE(count, tmp_buf);
    nbytes += 4;

    free(tmp_buf);
    return nbytes;
}


int64_t bshuf_compress_lz4(void* in, void* out, const size_t size,
        const size_t elem_size, size_t block_size) {
    return bshuf_blocked_wrap_fun(&bshuf_compress_lz4_block, in, out, size,
            elem_size, block_size);
}


int64_t bshuf_decompress_lz4(void* in, void* out, const size_t size,
        const size_t elem_size, size_t block_size) {
    return bshuf_blocked_wrap_fun(&bshuf_decompress_lz4_block, in, out, size,
            elem_size, block_size);
}


#undef TRANS_BIT_8X8
#undef TRANS_ELEM_TYPE
#undef MIN
#undef MAX
#undef CHECK_MULT_EIGHT
#undef CHECK_ERR
#undef CHECK_ERR_FREE
#undef CHECK_ERR_FREE_LZ

#undef USESSE2
#undef USEAVX2
