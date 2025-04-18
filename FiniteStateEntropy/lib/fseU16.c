/* ******************************************************************
   FSEU16 : Finite State Entropy coder for 16-bits input
   Copyright (C) 2013-2016, Yann Collet.

   BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following disclaimer
   in the documentation and/or other materials provided with the
   distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   You can contact the author at :
   - Source repository : https://github.com/Cyan4973/FiniteStateEntropy
   - Public forum : https://groups.google.com/forum/#!forum/lz4c
****************************************************************** */

/* *************************************************************
*  Tuning parameters
*****************************************************************/
/* MEMORY_USAGE :
*  Memory usage formula : N->2^N Bytes (examples : 10 -> 1KB; 12 -> 4KB ; 16 -> 64KB; 20 -> 1MB; etc.)
*  Increasing memory usage improves compression ratio
*  Reduced memory usage can improve speed, due to cache effect
*  Recommended max value is 14, for 16KB, which nicely fits into Intel x86 L1 cache */
#ifndef FSEU16_MAX_MEMORY_USAGE
#  define FSEU16_MAX_MEMORY_USAGE 15
#endif
#ifndef FSEU16_DEFAULT_MEMORY_USAGE
#  define FSEU16_DEFAULT_MEMORY_USAGE 14
#endif

/* **************************************************************
*  Includes
*****************************************************************/
#include <assert.h>
#include "fseU16.h"
#define FSEU16_SYMBOLVALUE_ABSOLUTEMAX 4095
#if (FSEU16_MAX_SYMBOL_VALUE > FSEU16_SYMBOLVALUE_ABSOLUTEMAX)
#  error "FSEU16_MAX_SYMBOL_VALUE is too large !"
#endif

/* **************************************************************
*  Compiler specifics
*****************************************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  pragma warning(disable : 4214)        /* disable: C4214: non-int bitfields */
#endif

#if defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif

#if defined (__clang__)
#  pragma clang diagnostic ignored "-Wunused-function"
#endif


/* **************************************************************
*  Local type
****************************************************************/
typedef struct {
    unsigned short newState;
    unsigned short nbBits : 4;
    unsigned short symbol : 12;
} FSE_decode_tU16;    /* Note : the size of this struct must be 4 */

static_assert(sizeof(FSE_decode_tU16) == 4, "size of FSE_decode_tU16 must be 4!");

/* *******************************************************************
*  Include type-specific functions from fse.c (C template emulation)
*********************************************************************/
#define FSE_COMMONDEFS_ONLY

#ifdef FSE_MAX_MEMORY_USAGE
#  undef FSE_MAX_MEMORY_USAGE
#endif
#ifdef FSE_DEFAULT_MEMORY_USAGE
#  undef FSE_DEFAULT_MEMORY_USAGE
#endif
#define FSE_MAX_MEMORY_USAGE FSEU16_MAX_MEMORY_USAGE
#define FSE_DEFAULT_MEMORY_USAGE FSEU16_DEFAULT_MEMORY_USAGE

#define FSE_FUNCTION_TYPE U16
#define FSE_FUNCTION_EXTENSION U16

#define FSE_count_generic FSE_count_genericU16
#define FSE_buildCTable   FSE_buildCTableU16
#define FSE_buildCTable_wksp FSE_buildCTable_wksp_U16

#define FSE_DECODE_TYPE   FSE_decode_tU16
#define FSE_createDTable  FSE_createDTableU16
#define FSE_freeDTable    FSE_freeDTableU16
#define FSE_buildDTable   FSE_buildDTableU16

#include "fse_compress.c"   /* FSE_countU16, FSE_buildCTableU16 */
#include "fse_decompress.c"   /* FSE_buildDTableU16 */


/*! FSE_countU16() :
    This function counts U16 values stored in `src`,
    and push the histogram into `count`.
   @return : count of most common element
   *maxSymbolValuePtr : will be updated with value of highest symbol.
*/
size_t FSE_countU16(unsigned* count, unsigned* maxSymbolValuePtr,
                    const U16* src, size_t srcSize)
{
    const U16* ip16 = (const U16*)src;
    const U16* const end = src + srcSize;
    unsigned maxSymbolValue = *maxSymbolValuePtr;

    memset(count, 0, (maxSymbolValue+1)*sizeof(*count));
    if (srcSize==0) { *maxSymbolValuePtr = 0; return 0; }

    while (ip16<end) {
        if (*ip16 > maxSymbolValue)
            return ERROR(maxSymbolValue_tooSmall);
        count[*ip16++]++;
    }

    while (!count[maxSymbolValue]) maxSymbolValue--;
    *maxSymbolValuePtr = maxSymbolValue;

    {   U32 s, max=0;
        for (s=0; s<=maxSymbolValue; s++)
            if (count[s] > max) max = count[s];
        return (size_t)max;
    }
}

/* *******************************************************
*  U16 Compression functions
*********************************************************/
size_t FSE_compressU16_usingCTable (void* dst, size_t maxDstSize,
                              const U16*  src, size_t srcSize,
                              const FSE_CTable* ct)
{
    const U16* const istart = (const U16*) src;
    const U16* const iend = istart + srcSize;
    const U16* ip=iend;

    BIT_CStream_t bitC;
    FSE_CState_t CState1, CState2;

    /* init */
    if (srcSize <= 2) return 0;
    { size_t const initError = BIT_initCStream(&bitC, dst, maxDstSize);
      if (FSE_isError(initError)) return 0; /* not enough space available to write a bitstream */ }

    if (srcSize & 1) {
        FSE_initCState2(&CState1, ct, *--ip);
        FSE_initCState2(&CState2, ct, *--ip);
        FSE_encodeSymbol(&bitC, &CState1, *--ip);
        BIT_flushBits(&bitC);
    } else {
        FSE_initCState2(&CState2, ct, *--ip);
        FSE_initCState2(&CState1, ct, *--ip);
    }

    /* join to mod 4 */
    srcSize -= 2;
    if ((sizeof(bitC.bitContainer)*8 > FSE_MAX_TABLELOG*4+7 ) && (srcSize & 2)) {  /* test bit 2 */
        FSE_encodeSymbol(&bitC, &CState2, *--ip);
        FSE_encodeSymbol(&bitC, &CState1, *--ip);
        BIT_flushBits(&bitC);
    }

    /* 2 or 4 encoding per loop */
    while ( ip>istart ) {

        FSE_encodeSymbol(&bitC, &CState2, *--ip);

        if (sizeof(bitC.bitContainer)*8 < FSE_MAX_TABLELOG*2+7 )   /* this test must be static */
            BIT_flushBits(&bitC);

        FSE_encodeSymbol(&bitC, &CState1, *--ip);

        if (sizeof(bitC.bitContainer)*8 > FSE_MAX_TABLELOG*4+7 ) {  /* this test must be static */
            FSE_encodeSymbol(&bitC, &CState2, *--ip);
            FSE_encodeSymbol(&bitC, &CState1, *--ip);
        }

        BIT_flushBits(&bitC);
    }

    FSE_flushCState(&bitC, &CState2);
    FSE_flushCState(&bitC, &CState1);
    return BIT_closeCStream(&bitC);
}


size_t FSE_compressU16(void* dst, size_t maxDstSize,
       const unsigned short* src, size_t srcSize,
       unsigned maxSymbolValue, unsigned tableLog)
{
    const U16* const istart = src;
    const U16* ip = istart;

    BYTE* const ostart = (BYTE*) dst;
    BYTE* const omax = ostart + maxDstSize;
    BYTE* op = ostart;

    U32   counting[FSE_MAX_SYMBOL_VALUE+1] = {0};
    S16   norm[FSE_MAX_SYMBOL_VALUE+1];

    /* checks */
    if (srcSize <= 1) return srcSize;
    if (!maxSymbolValue) maxSymbolValue = FSE_MAX_SYMBOL_VALUE;
    if (!tableLog) tableLog = FSE_DEFAULT_TABLELOG;
    if (maxSymbolValue > FSE_MAX_SYMBOL_VALUE) return ERROR(maxSymbolValue_tooLarge);
    if (tableLog > FSE_MAX_TABLELOG) return ERROR(tableLog_tooLarge);

    /* Scan for stats */
    {   size_t const maxCount = FSE_countU16 (counting, &maxSymbolValue, ip, srcSize);
        if (FSE_isError(maxCount)) return maxCount;
        if (maxCount == srcSize) return 1;   /* src contains one constant element x srcSize times. Use RLE compression. */
    }
    /* Normalize */
    tableLog = FSE_optimalTableLog(tableLog, srcSize, maxSymbolValue);
    {   size_t const errorCode = FSE_normalizeCount (norm, tableLog, counting, srcSize, maxSymbolValue);
        if (FSE_isError(errorCode)) return errorCode;
    }
    /* Write table description header */
    {   size_t const NSize = FSE_writeNCount (op, omax-op, norm, maxSymbolValue, tableLog);
        if (FSE_isError(NSize)) return NSize;
        op += NSize;
    }
    /* Compress */
    {   FSE_CTable CTable[FSE_CTABLE_SIZE_U32(FSE_MAX_TABLELOG, FSE_MAX_SYMBOL_VALUE)];
        size_t const errorCode = FSE_buildCTableU16 (CTable, norm, maxSymbolValue, tableLog);
        if (FSE_isError(errorCode)) return errorCode;
        size_t csize = FSE_compressU16_usingCTable (op, omax - op, ip, srcSize, CTable);
        if (csize == 0) return ERROR(dstSize_tooSmall); // overflow detected
        op += csize;
    }

    /* check compressibility */
    if ( (size_t)(op-ostart) >= (size_t)(srcSize-1)*(sizeof(U16)) )
        return 0;   /* no compression */

    return op-ostart;
}


/* *******************************************************
*  U16 Decompression functions
*********************************************************/

MEM_STATIC U16 FSE_decodeSymbolU16(FSE_DState_t* DStatePtr, BIT_DStream_t* bitD)
{
    FSE_decode_tU16 const DInfo = ((const FSE_decode_tU16*)(DStatePtr->table))[DStatePtr->state];
    U32 const nbBits = DInfo.nbBits;
    U16 const symbol = DInfo.symbol;
    size_t const lowBits = BIT_readBits(bitD, nbBits);

    DStatePtr->state = DInfo.newState + lowBits;
    return symbol;
}


size_t FSE_decompressU16_usingDTable (U16* dst, size_t maxDstSize,
                               const void* cSrc, size_t cSrcSize,
                               const FSE_DTable* dt)
{
    U16* const ostart = (U16*) dst;
    U16* op = ostart;
    U16* const omax = op + maxDstSize;
    U16* const olimit = omax-3;

    BIT_DStream_t bitD;
    FSE_DState_t state1;
    FSE_DState_t state2;

    /* Init */
    CHECK_F(BIT_initDStream(&bitD, cSrc, cSrcSize));

    FSE_initDState(&state1, &bitD, dt);
    FSE_initDState(&state2, &bitD, dt);

    /* 4 symbols per loop */
    for ( ; (BIT_reloadDStream(&bitD)==BIT_DStream_unfinished) & (op<olimit) ; op+=4) {
        op[0] = FSE_decodeSymbolU16(&state1, &bitD);

        if (FSE_MAX_TABLELOG*2+7 > sizeof(bitD.bitContainer)*8)    /* This test must be static */
            BIT_reloadDStream(&bitD);

        op[1] = FSE_decodeSymbolU16(&state2, &bitD);

        if (FSE_MAX_TABLELOG*4+7 > sizeof(bitD.bitContainer)*8)    /* This test must be static */
            { if (BIT_reloadDStream(&bitD) > BIT_DStream_unfinished) { op+=2; break; } }

        op[2] = FSE_decodeSymbolU16(&state1, &bitD);

        if (FSE_MAX_TABLELOG*2+7 > sizeof(bitD.bitContainer)*8)    /* This test must be static */
            BIT_reloadDStream(&bitD);

        op[3] = FSE_decodeSymbolU16(&state2, &bitD);
    }

    /* tail */
    /* note : BIT_reloadDStream(&bitD) >= FSE_DStream_partiallyFilled; Ends at exactly BIT_DStream_completed */
    while (1) {
        if (op>(omax-2)) return ERROR(dstSize_tooSmall);
        *op++ = FSE_decodeSymbolU16(&state1, &bitD);
        if (BIT_reloadDStream(&bitD)==BIT_DStream_overflow) {
            *op++ = FSE_decodeSymbolU16(&state2, &bitD);
            break;
        }

        if (op>(omax-2)) return ERROR(dstSize_tooSmall);
        *op++ = FSE_decodeSymbolU16(&state2, &bitD);
        if (BIT_reloadDStream(&bitD)==BIT_DStream_overflow) {
            *op++ = FSE_decodeSymbolU16(&state1, &bitD);
            break;
    }   }

    return op-ostart;
}


typedef FSE_DTable DTable_max_t[FSE_DTABLE_SIZE_U32(FSE_MAX_TABLELOG)];

size_t FSE_decompressU16(U16* dst, size_t maxDstSize,
                  const void* cSrc, size_t cSrcSize)
{
    const BYTE* const istart = (const BYTE*) cSrc;
    const BYTE* ip = istart;
    short NCount[FSE_MAX_SYMBOL_VALUE+1];
    DTable_max_t dt;
    unsigned maxSymbolValue = FSE_MAX_SYMBOL_VALUE;
    unsigned tableLog;

    /* Sanity check */
    if (cSrcSize<2) return ERROR(srcSize_wrong);   /* specific corner cases (uncompressed & rle) */

    /* normal FSE decoding mode */
    {   size_t const NSize = FSE_readNCount (NCount, &maxSymbolValue, &tableLog, istart, cSrcSize);
        if (FSE_isError(NSize)) return NSize;
        ip += NSize;
        cSrcSize -= NSize;
    }
    {   size_t const errorCode = FSE_buildDTableU16 (dt, NCount, maxSymbolValue, tableLog);
        if (FSE_isError(errorCode)) return errorCode;
    }
    return FSE_decompressU16_usingDTable (dst, maxDstSize, ip, cSrcSize, dt);
}
