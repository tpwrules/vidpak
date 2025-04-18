#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "fse.h"
#include "fseU16.h"
#include "error_public.h" // from FSE
#include "pack.h"

// temporary data used during pack and unpack operations. contexts are not
// thread-safe!
struct pack_context_t {
    size_t width; // width and height of the packed frame, in pixels
    size_t height;
    size_t bpp; // bits per pixel
    size_t twidth; // width and height of one tile
    size_t theight;
    void* diff; // buffer to store difference between pixels
};

// https://stackoverflow.com/a/3437484
#define max(a,b) \
    ({ __typeof__ (a) _a = (a); \
        __typeof__ (b) _b = (b); \
        _a > _b ? _a : _b; })

#define min(a,b) \
    ({ __typeof__ (a) _a = (a); \
        __typeof__ (b) _b = (b); \
        _a < _b ? _a : _b; })

// encode the delta given a pixel and a prediction
static inline uint16_t delta_encode_12bit(uint16_t pix, uint16_t pred) {
    return (pix-pred) & 0xFFF; // simple difference modulo 12 bits
}

// decode the pixel given a delta and a prediction
static inline uint16_t delta_decode_12bit(uint16_t delta, uint16_t pred) {
    return (delta+pred) & 0xFFF; // simple difference modulo 12 bits
}

// create a pack context to pack (or unpack) frames of the specified size, bits
// per pixel, and tile size. contexts are not thread-safe!
pack_context_t* pack_create_context(int width, int height, int bpp,
        int twidth, int theight) {
    if ((width <= 0) || (height <= 0) || (bpp <= 0)) return NULL;
    if ((twidth <= 0) || (theight <= 0)) return NULL;
    if ((twidth > width) || (theight > height)) return NULL;

    // diff buffer holds difference of one tile while it's processed
    size_t bytes = twidth * theight * ((bpp+7)/8);
    void* diff = malloc(bytes);
    if (!diff) {
        return NULL;
    }
    memset(diff, 0, bytes);

    pack_context_t* ctx = malloc(sizeof(pack_context_t));
    if (!ctx) {
        free(diff);
        return NULL;
    }

    ctx->width = width;
    ctx->height = height;
    ctx->twidth = twidth;
    ctx->theight = theight;
    ctx->bpp = bpp;
    ctx->diff = diff;

    return ctx;
}

// destroy a pack context
void pack_destroy_context(pack_context_t* ctx) {
    if (!ctx) return;
    free(ctx->diff);
    free(ctx);
}

// calculate the maximum possible size of a packed frame
size_t pack_calc_max_packed_size(pack_context_t* ctx) {
    if (!ctx) return 0;

    size_t width = ctx->width;
    size_t height = ctx->height;
    size_t twidth = ctx->twidth;
    size_t theight = ctx->theight;

    // the raw pixel data, assuming it could not be compressed, plus the tile
    // size table
    size_t bytes = (width * height * ((ctx->bpp+7)/8)) +
        (4*((width+twidth-1)/twidth)*((height+theight-1)/theight));

    return bytes;
}

// PACK ROUTINES
// all the routines work by predicting each pixel based on the surrounding
// previous pixels, computing the difference between the prediction and the
// actual pixel, and entropy coding that prediction. exactly how the data is
// stored depends on which predictor and bit depth

// AVERAGE PREDICTOR
// the first pixel is sent as is, with no prediction. the first row's pixels are
// predicted to be the pixel to their left. the first column's pixels are
// predicted to be the pixel to their top. the rest of the pixels are predicted
// to be the average of the pixels to the left and top.
static size_t pack_12bit_average(size_t width, size_t height, void* diff_,
        const uint16_t* src, uint8_t* dest,
        ssize_t dx, ssize_t dy) {
    // not allowed, but we'd like to let the optimizer know that
    if ((height == 0) || (width == 0)) return 0;

    uint16_t* diff = (uint16_t*)diff_;
    size_t pixels = width * height;
    size_t bytes = 2 * pixels;

    // we slice up the image into four horizontal slices. working on four at a
    // time allows us to better utilize the CPU
    size_t sheight = (height+3)/4; // height of each slice
    // compute the number of slices we actually create, which will be less than
    // the full four if there's only that number of rows in the image
    size_t slices = (height < 4) ? height : 4;

    // compute the height of each slice and then its start
    size_t h0 = (sheight-((height&3)<=0 && (height&3)));
    size_t h1 = (sheight-((height&3)<=1 && (height&3)));
    size_t h2 = (sheight-((height&3)<=2 && (height&3)));
    size_t h3 = (sheight-((height&3)<=3 && (height&3)));
    if ((h0+h1+h2+h3) != height) return 0; // should never happen
    const uint16_t* src0 = (slices > 0) ? &src[dy*(0)] : NULL;
    const uint16_t* src1 = (slices > 1) ? &src[dy*(h0)] : NULL;
    const uint16_t* src2 = (slices > 2) ? &src[dy*(h0+h1)] : NULL;
    const uint16_t* src3 = (slices > 3) ? &src[dy*(h0+h1+h2)] : NULL;

    // store the first pixel as-is
    if (slices > 0) {
        dest[2*0+0] = src0[0] & 0xFF;
        dest[2*0+1] = (src0[0] >> 8) & 0x0F;
    }
    if (slices > 1) {
        dest[2*1+0] = src1[0] & 0xFF;
        dest[2*1+1] = (src1[0] >> 8) & 0x0F;
    }
    if (slices > 2) {
        dest[2*2+0] = src2[0] & 0xFF;
        dest[2*2+1] = (src2[0] >> 8) & 0x0F;
    }
    if (slices > 3) {
        dest[2*3+0] = src3[0] & 0xFF;
        dest[2*3+1] = (src3[0] >> 8) & 0x0F;
    }

    size_t o = slices;
    // prediction of the first row: the pixel's left neighbor
    for (ssize_t x=dx; x!=dx*(ssize_t)width; x+=dx) {
        if (slices > 0) diff[o+0] = delta_encode_12bit(src0[x], src0[x-dx]);
        if (slices > 1) diff[o+1] = delta_encode_12bit(src1[x], src1[x-dx]);
        if (slices > 2) diff[o+2] = delta_encode_12bit(src2[x], src2[x-dx]);
        if (slices > 3) diff[o+3] = delta_encode_12bit(src3[x], src3[x-dx]);
        o += slices;
    }
    // process main rows (we know we have all four slices, otherwise this loop
    // won't start as sheight=0)
    for (size_t y=1; y<sheight; y++) {
        // determine which slices we are processing this loop
        int s0 = (y < (sheight-1)) || ((height & 3) > 0) || ((height & 3) == 0);
        int s1 = (y < (sheight-1)) || ((height & 3) > 1) || ((height & 3) == 0);
        int s2 = (y < (sheight-1)) || ((height & 3) > 2) || ((height & 3) == 0);
        int s3 = (y < (sheight-1)) || ((height & 3) > 3) || ((height & 3) == 0);
        int active = s0+s1+s2+s3;
        
        if (s0) src0 += dy; // move to next row
        if (s1) src1 += dy;
        if (s2) src2 += dy;
        if (s3) src3 += dy;

        // prediction of the first column: the pixel's top neighbor
        if (s0) diff[o+0] = delta_encode_12bit(src0[0], src0[-dy]);
        if (s1) diff[o+1] = delta_encode_12bit(src1[0], src1[-dy]);
        if (s2) diff[o+2] = delta_encode_12bit(src2[0], src2[-dy]);
        if (s3) diff[o+3] = delta_encode_12bit(src3[0], src3[-dy]);
        o += active;

        // prediction of the rest of the pixels: average of left and top
        // neighbors
        for (ssize_t x=dx; x!=dx*(ssize_t)width; x+=dx) {
            uint16_t p0, p1, p2, p3;
            if (s0) p0 = (src0[x-dx]+src0[x-dy])>>1;
            if (s1) p1 = (src1[x-dx]+src1[x-dy])>>1;
            if (s2) p2 = (src2[x-dx]+src2[x-dy])>>1;
            if (s3) p3 = (src3[x-dx]+src3[x-dy])>>1;
            if (s0) diff[o+0] = delta_encode_12bit(src0[x], p0);
            if (s1) diff[o+1] = delta_encode_12bit(src1[x], p1);
            if (s2) diff[o+2] = delta_encode_12bit(src2[x], p2);
            if (s3) diff[o+3] = delta_encode_12bit(src3[x], p3);
            o += active;
        }
    }

    // compress the differences
    size_t sb = 2*slices; // bytes of slice starts (raw data)
    // FSE compressor return values:
    //  -FSE_error_dstSize_tooSmall if insufficient space (we treat as 0)
    //  -error if something went wrong (we bail)
    //  0 if 0 values (pixels-slices == 0)
    //  0 if output is no smaller than input (general case of the above)
    //  1 if 1 value (pixels-slices == 1)
    //  1 if all input values are the same (general case of the above)
    //      could make sense to treat as 0, but 1 is how existing files are
    //  2 < num_bytes < (bytes-sb) otherwise (compression successful)
    //      a return of 2 would confuse the unpacker but it's not possible
    size_t ret = FSE_compressU16(dest+sb, bytes-sb,
        diff+slices, pixels-slices, 4095, 0);
    if (-ret == FSE_error_dstSize_tooSmall) ret = 0; // ran out of space
    if (FSE_isError(ret)) { // something else went wrong, bail out
        return 0;
    } else if (ret == 1) { // all the difference values are the same
        dest[sb] = diff[slices] & 0xFF; // store that value
        dest[sb+1] = diff[slices] >> 8;
        return sb+2;
    } else if (ret == 0) { // compressed differences no smaller than input
        // so just return the input as the result
        for (ssize_t y=0; y!=dy*(ssize_t)height; y+=dy) {
            for (ssize_t x=0; x!=dx*(ssize_t)width; x+=dx) {
                uint16_t p = src[y+x];
                *dest++ = p & 0xFF;
                *dest++ = (p >> 8) & 0x0F;
            }
        }
        // bytes != sb+2, because if so, pixels-slices == 1, so FSE returned 1
        return bytes;
    } else { // bytes of initial pixel of each slice + compressed differences
        return sb+ret; // ret is always > 2 and < (bytes-sb)
    }
}

static int unpack_12bit_average(size_t width, size_t height, void* diff_,
        const uint8_t* src, size_t src_size, uint16_t* dest,
        ssize_t dx, ssize_t dy) {
    // not allowed, but we'd like to let the optimizer know that
    if ((height == 0) || (width == 0)) return 0;

    uint16_t* diff = (uint16_t*)diff_;
    size_t pixels = width * height;
    size_t bytes = 2 * pixels;

    // we slice up the image into four horizontal slices. working on four at a
    // time allows us to better utilize the CPU
    size_t sheight = (height+3)/4; // height of each slice
    // compute the number of slices we actually create, which will be less than
    // the full four if there's only that number of rows in the image
    size_t slices = (height < 4) ? height : 4;

    // uncompress the differences
    size_t sb = 2*slices; // bytes of slice starts (raw data)
    if (src_size == 0) { // invalid pointer was passed in
        return 0;
    } else if (src_size == sb+2) { // all the difference values were the same
        // this is the case if sb+2 == bytes, see packer for details
        uint16_t v = ((uint16_t)src[sb+1] << 8) | src[sb]; // get the value
        for (size_t i=slices; i<pixels; i++) { // and fill the buffer with it
            diff[i] = v;
        }
    } else if (src_size == bytes) { // input is not compressed and is the result
        for (ssize_t y=0; y!=dy*(ssize_t)height; y+=dy) {
            for (ssize_t x=0; x!=dx*(ssize_t)width; x+=dx) {
                uint16_t l = *src++;
                uint16_t h = *src++;
                dest[y+x] = (h << 8) | l;
            }
        }
        return 1;
    } else { // differences are actually compressed
        size_t ret = FSE_decompressU16(diff+slices, pixels-slices,
            src+sb, src_size-sb);
        if (FSE_isError(ret)) {
            return 0;
        }
    }

    // compute the height of each slice and then its start
    size_t h0 = (sheight-((height&3)<=0 && (height&3)));
    size_t h1 = (sheight-((height&3)<=1 && (height&3)));
    size_t h2 = (sheight-((height&3)<=2 && (height&3)));
    size_t h3 = (sheight-((height&3)<=3 && (height&3)));
    if ((h0+h1+h2+h3) != height) return 0; // should never happen
    uint16_t* dest0 = (slices > 0) ? &dest[dy*(0)] : NULL;
    uint16_t* dest1 = (slices > 1) ? &dest[dy*(h0)] : NULL;
    uint16_t* dest2 = (slices > 2) ? &dest[dy*(h0+h1)] : NULL;
    uint16_t* dest3 = (slices > 3) ? &dest[dy*(h0+h1+h2)] : NULL;

    // recover the first pixel value
    // unlike during compression, we keep the left pixel in a variable instead
    // of re-reading it from the array for speed
    uint16_t l0 = (slices > 0) ? ((uint16_t)src[2*0+1] << 8) | src[2*0+0] : 0;
    uint16_t l1 = (slices > 1) ? ((uint16_t)src[2*1+1] << 8) | src[2*1+0] : 0;
    uint16_t l2 = (slices > 2) ? ((uint16_t)src[2*2+1] << 8) | src[2*2+0] : 0;
    uint16_t l3 = (slices > 3) ? ((uint16_t)src[2*3+1] << 8) | src[2*3+0] : 0;
    if (slices > 0) dest0[0] = l0;
    if (slices > 1) dest1[0] = l1;
    if (slices > 2) dest2[0] = l2;
    if (slices > 3) dest3[0] = l3;

    size_t i = slices;
    // prediction of the first row: the pixel's left neighbor
    for (ssize_t x=dx; x!=dx*(ssize_t)width; x+=dx) {
        if (slices > 0) l0 = delta_decode_12bit(diff[i+0], l0);
        if (slices > 1) l1 = delta_decode_12bit(diff[i+1], l1);
        if (slices > 2) l2 = delta_decode_12bit(diff[i+2], l2);
        if (slices > 3) l3 = delta_decode_12bit(diff[i+3], l3);
        if (slices > 0) dest0[x] = l0;
        if (slices > 1) dest1[x] = l1;
        if (slices > 2) dest2[x] = l2;
        if (slices > 3) dest3[x] = l3;
        i += slices;
    }
    // process main rows (we know we have all four slices, otherwise this loop
    // won't start as sheight=0)
    for (size_t y=1; y<sheight; y++) {
        // determine which slices we are processing this loop
        int s0 = (y < (sheight-1)) || ((height & 3) > 0) || ((height & 3) == 0);
        int s1 = (y < (sheight-1)) || ((height & 3) > 1) || ((height & 3) == 0);
        int s2 = (y < (sheight-1)) || ((height & 3) > 2) || ((height & 3) == 0);
        int s3 = (y < (sheight-1)) || ((height & 3) > 3) || ((height & 3) == 0);
        int active = s0+s1+s2+s3;

        if (s0) dest0 += dy; // move to next row
        if (s1) dest1 += dy;
        if (s2) dest2 += dy;
        if (s3) dest3 += dy;

        // prediction of the first column: the pixel's top neighbor
        if (s0) l0 = delta_decode_12bit(diff[i+0], dest0[-dy]);
        if (s1) l1 = delta_decode_12bit(diff[i+1], dest1[-dy]);
        if (s2) l2 = delta_decode_12bit(diff[i+2], dest2[-dy]);
        if (s3) l3 = delta_decode_12bit(diff[i+3], dest3[-dy]);
        if (s0) dest0[0] = l0;
        if (s1) dest1[0] = l1;
        if (s2) dest2[0] = l2;
        if (s3) dest3[0] = l3;
        i += active;

        // prediction of the rest of the pixels: average of left and top
        // neighbors
        for (ssize_t x=dx; x!=dx*(ssize_t)width; x+=dx) {
            uint16_t p0, p1, p2, p3;
            if (s0) p0 = (l0+dest0[x-dy])>>1;
            if (s1) p1 = (l1+dest1[x-dy])>>1;
            if (s2) p2 = (l2+dest2[x-dy])>>1;
            if (s3) p3 = (l3+dest3[x-dy])>>1;
            if (s0) l0 = delta_decode_12bit(diff[i+0], p0);
            if (s1) l1 = delta_decode_12bit(diff[i+1], p1);
            if (s2) l2 = delta_decode_12bit(diff[i+2], p2);
            if (s3) l3 = delta_decode_12bit(diff[i+3], p3);
            if (s0) dest0[x] = l0;
            if (s1) dest1[x] = l1;
            if (s2) dest2[x] = l2;
            if (s3) dest3[x] = l3;
            i += active;
        }
    }

    return 1;
}

// pack a frame using the specified context. returns the number of bytes
// actually written to the destination buffer, or 0 if failed. its size MUST BE
// at least the size specified by pack_calc_max_packed_size. dx and dy are the
// number of pixels to advance after each pixel in the x and y direction, i.e.
// to pack the whole input array, dx=1 and dy=width.
size_t pack_with_context(pack_context_t* ctx,
        const uint16_t* src, uint8_t* dest,
        ssize_t dx, ssize_t dy) {
    if ((!ctx) || (!src) || (!dest)) return 0;
    if (ctx->bpp != 12) return 0;
    if ((dx == 0) || (dy == 0)) return 0;

    size_t width = ctx->width;
    size_t height = ctx->height;
    size_t twidth = ctx->twidth;
    size_t theight = ctx->theight;
    void* diff = ctx->diff;

    // pack each tile individually. we start with a table of the size of
    // each tile in bytes so we know where each tile's data is
    size_t dest_pos = 4*((width+twidth-1)/twidth)*((height+theight-1)/theight);
    size_t tile = 0;
    for (size_t ty=0; ty<height; ty+=theight) {
        for (size_t tx=0; tx<width; tx+=twidth) {
            uint32_t size = (uint32_t)pack_12bit_average(
                min(twidth, width-tx), min(theight, height-ty), diff,
                &src[(ty*dy)+(tx*dx)], &dest[dest_pos],
                dx, dy);
            if (size == 0) return 0;
            dest[4*tile+0] = size & 0xFF;
            dest[4*tile+1] = (size >> 8) & 0xFF;
            dest[4*tile+2] = (size >> 16) & 0xFF;
            dest[4*tile+3] = (size >> 24) & 0xFF;
            dest_pos += size;
            tile++;
        }
    }
    return dest_pos;
}

// unpack a frame using the specified context. returns 1 if successful or 0 if
// failed. src_size MUST BE exactly the size returned by pack_with_context. dx
// and dy are the number of pixels to advance after each pixel in the x and y
// direction, i.e. to unpack the whole output array, dx=1 and dy=width.
int unpack_with_context(pack_context_t* ctx,
        const uint8_t* src, size_t src_size, uint16_t* dest,
        ssize_t dx, ssize_t dy) {
    if ((!ctx) || (!src) || (!dest)) return 0;
    if (ctx->bpp != 12) return 0;
    if ((src_size == 0) || (dx == 0) || (dy == 0)) return 0;

    size_t width = ctx->width;
    size_t height = ctx->height;
    size_t twidth = ctx->twidth;
    size_t theight = ctx->theight;
    void* diff = ctx->diff;

    // unpack each tile individually. we start with a table of the size of
    // each tile in bytes so we know where each tile's data is
    size_t src_pos = 4*((width+twidth-1)/twidth)*((height+theight-1)/theight);
    if (src_pos > src_size) return 0;
    size_t tile = 0;
    for (size_t ty=0; ty<height; ty+=theight) {
        for (size_t tx=0; tx<width; tx+=twidth) {
            uint32_t size = src[4*tile+0] | (src[4*tile+1]<<8)
                | (src[4*tile+2]<<16) | (src[4*tile+3]<<24);
            if (size > (src_size - src_pos)) return 0;
            int success = unpack_12bit_average(
                min(twidth, width-tx), min(theight, height-ty), diff,
                &src[src_pos], size, &dest[(ty*dy)+(tx*dx)],
                dx, dy);
            if (success != 1) return 0;
            src_pos += size;
            tile++;
        }
    }
    return 1;
}
