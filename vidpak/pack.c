#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdio.h>

#include "fse.h"
#include "fseU16.h"
#include "pack.h"

// create a pack context to pack (or unpack) frames of the specified size, bits
// per pixel, and tile size. the frame size must be a multiple of the tile size
// and the tile height must be a multiple of 4! contexts are not thread-safe!
pack_context_t* pack_create_context(int width, int height, int bpp,
        int twidth, int theight) {
    if ((width <= 0) || (height <= 0) || (bpp <= 0)) return NULL;
    if ((twidth <= 0) || (theight <= 0)) return NULL;
    if ((theight & 3) != 0) return NULL;
    if ((width % twidth != 0) || (height % theight != 0)) return NULL;

    size_t bytes = width * height * ((bpp+7)/8);
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

    // the raw pixel data, assuming it could not be compressed
    size_t bytes = ctx->width * ctx->height * ((ctx->bpp+7)/8);
    if (ctx->twidth) { // plus the tile size table, if tiled
        bytes += 4*(ctx->width/ctx->twidth)*(ctx->height/ctx->theight);
    }

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
        size_t dx, size_t dy) {
    uint16_t* diff = (uint16_t*)diff_;
    size_t pixels = width * height;
    size_t bytes = 2 * pixels;

    // we slice up the image into four equally sized horizontal slices. working
    // on four at a time allows us to better utilize the CPU
    size_t sheight = height/4; // height of each slice

    // compute the start of each slice
    const uint16_t* src0 = &src[0*(dy*sheight)];
    const uint16_t* src1 = &src[1*(dy*sheight)];
    const uint16_t* src2 = &src[2*(dy*sheight)];
    const uint16_t* src3 = &src[3*(dy*sheight)];

    // store the first pixel as-is
    dest[2*0+0] = src0[0] & 0xFF;
    dest[2*0+1] = src0[0] >> 8;
    dest[2*1+0] = src1[0] & 0xFF;
    dest[2*1+1] = src1[0] >> 8;
    dest[2*2+0] = src2[0] & 0xFF;
    dest[2*2+1] = src2[0] >> 8;
    dest[2*3+0] = src3[0] & 0xFF;
    dest[2*3+1] = src3[0] >> 8;

    size_t o = 4;
    // prediction of the first row: the pixel's left neighbor
    for (size_t x=dx; x<dx*width; x+=dx) {
        diff[o+0] = (src0[x] - src0[x-dx]) & 0xFFFF;
        diff[o+1] = (src1[x] - src1[x-dx]) & 0xFFFF;
        diff[o+2] = (src2[x] - src2[x-dx]) & 0xFFFF;
        diff[o+3] = (src3[x] - src3[x-dx]) & 0xFFFF;
        o += 4;
    }
    for (size_t y=1; y<sheight; y++) {
        src0 += dy; // move to next row
        src1 += dy;
        src2 += dy;
        src3 += dy;

        // prediction of the first column: the pixel's top neighbor
        diff[o+0] = (src0[0] - src0[-dy]) & 0xFFFF;
        diff[o+1] = (src1[0] - src1[-dy]) & 0xFFFF;
        diff[o+2] = (src2[0] - src2[-dy]) & 0xFFFF;
        diff[o+3] = (src3[0] - src3[-dy]) & 0xFFFF;
        o += 4;

        // prediction of the rest of the pixels: average of left and top
        // neighbors
        for (size_t x=dx; x<dx*width; x+=dx) {
            uint16_t p0 = ((uint32_t)src0[x-dx]+(uint32_t)src0[x-dy])>>1;
            uint16_t p1 = ((uint32_t)src1[x-dx]+(uint32_t)src1[x-dy])>>1;
            uint16_t p2 = ((uint32_t)src2[x-dx]+(uint32_t)src2[x-dy])>>1;
            uint16_t p3 = ((uint32_t)src3[x-dx]+(uint32_t)src3[x-dy])>>1;
            diff[o+0] = (src0[x] - p0) & 0xFFFF;
            diff[o+1] = (src1[x] - p1) & 0xFFFF;
            diff[o+2] = (src2[x] - p2) & 0xFFFF;
            diff[o+3] = (src3[x] - p3) & 0xFFFF;
            o += 4;
        }
    }

    // compress the differences
    size_t ret = FSE_compressU16(dest+8, bytes-8, diff+4, pixels-4, 65535, 0);
    if (FSE_isError(ret)) { // something went wrong, bail out
        printf("FSE said: %lu\n", -ret);
        return 0;
    } else if (ret == 0) { // compressed result is bigger than input
        memcpy((void*)dest, (void*)src, bytes); // so just return the input
        return bytes;
    } else if (ret == 1) { // all the input values are the same
        dest[8] = diff[4] & 0xFF; // store that value
        dest[9] = diff[4] >> 8;
        return 10;
    } else { // 8 bytes of initial pixel of each slice + compressed data
        return 8+ret;
    }
}

static int unpack_12bit_average(size_t width, size_t height, void* diff_,
        const uint8_t* src, size_t src_size, uint16_t* dest,
        size_t dx, size_t dy) {
    uint16_t* diff = (uint16_t*)diff_;
    size_t pixels = width * height;
    size_t bytes = 2 * pixels;

    // uncompress the differences
    if (src_size == 0) { // invalid pointer was passed in
        return 0;
    } else if (src_size == bytes) { // input is not compressed
        memcpy((void*)dest, (void*)src, bytes);
        return 1;
    } else if (src_size == 10) { // all the input values were the same
        uint16_t v = ((uint16_t)src[9] << 8) | src[8]; // get the value
        for (size_t i=4; i<pixels; i++) { // and fill the buffer with it
            diff[i] = v;
        }
    } else { // data is actually compressed
        size_t ret = FSE_decompressU16(diff+4, pixels-4, src+8, src_size-8);
        if (FSE_isError(ret)) {
            return 0;
        }
    }

    // we slice up the image into four equally sized horizontal slices. working
    // on four at a time allows us to better utilize the CPU
    size_t sheight = height/4; // height of each slice

    // compute the start of each slice
    uint16_t* dest0 = &dest[0*(dy*sheight)];
    uint16_t* dest1 = &dest[1*(dy*sheight)];
    uint16_t* dest2 = &dest[2*(dy*sheight)];
    uint16_t* dest3 = &dest[3*(dy*sheight)];

    // recover the first pixel value
    // unlike during compression, we keep the left pixel in a variable instead
    // of re-reading it from the array for speed
    uint16_t l0 = ((uint16_t)src[2*0+1] << 8) | src[2*0+0];
    uint16_t l1 = ((uint16_t)src[2*1+1] << 8) | src[2*1+0];
    uint16_t l2 = ((uint16_t)src[2*2+1] << 8) | src[2*2+0];
    uint16_t l3 = ((uint16_t)src[2*3+1] << 8) | src[2*3+0];
    dest0[0] = l0; dest1[0] = l1; dest2[0] = l2; dest3[0] = l3;

    size_t i = 4;
    // prediction of the first row: the pixel's left neighbor
    for (size_t x=dx; x<dx*width; x+=dx) {
        l0 = (diff[i+0] + l0) & 0xFFFF;
        l1 = (diff[i+1] + l1) & 0xFFFF;
        l2 = (diff[i+2] + l2) & 0xFFFF;
        l3 = (diff[i+3] + l3) & 0xFFFF;
        dest0[x] = l0; dest1[x] = l1; dest2[x] = l2; dest3[x] = l3;
        i += 4;
    }
    for (size_t y=1; y<sheight; y++) {
        dest0 += dy; // move to next row
        dest1 += dy;
        dest2 += dy;
        dest3 += dy;

        // prediction of the first column: the pixel's top neighbor
        l0 = (diff[i+0] + dest0[-dy]) & 0xFFFF;
        l1 = (diff[i+1] + dest1[-dy]) & 0xFFFF;
        l2 = (diff[i+2] + dest2[-dy]) & 0xFFFF;
        l3 = (diff[i+3] + dest3[-dy]) & 0xFFFF;
        dest0[0] = l0; dest1[0] = l1; dest2[0] = l2; dest3[0] = l3;
        i += 4;

        // prediction of the rest of the pixels: average of left and top
        // neighbors
        for (size_t x=dx; x<dx*width; x+=dx) {
            uint16_t p0 = ((uint32_t)l0+(uint32_t)dest0[x-dy])>>1;
            uint16_t p1 = ((uint32_t)l1+(uint32_t)dest1[x-dy])>>1;
            uint16_t p2 = ((uint32_t)l2+(uint32_t)dest2[x-dy])>>1;
            uint16_t p3 = ((uint32_t)l3+(uint32_t)dest3[x-dy])>>1;
            l0 = (diff[i+0] + p0) & 0xFFFF;
            l1 = (diff[i+1] + p1) & 0xFFFF;
            l2 = (diff[i+2] + p2) & 0xFFFF;
            l3 = (diff[i+3] + p3) & 0xFFFF;
            dest0[x] = l0; dest1[x] = l1; dest2[x] = l2; dest3[x] = l3;
            i += 4;
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
        size_t dx, size_t dy) {
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
    size_t dest_pos = 4*(width/twidth)*(height/theight);
    size_t tile = 0;
    for (size_t ty=0; ty<height; ty+=theight) {
        for (size_t tx=0; tx<width; tx+=twidth) {
            uint32_t size = (uint32_t)pack_12bit_average(twidth, theight, diff,
                &src[(ty*dy)+(tx*dx)],
                &dest[dest_pos],
                dx, dy);
            if (size == 0) return 0;
            memcpy(&dest[4*tile], &size, sizeof(uint32_t));
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
        size_t dx, size_t dy) {
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
    size_t src_pos = 4*(width/twidth)*(height/theight);
    if (src_pos > src_size) return 0;
    size_t tile = 0;
    for (size_t ty=0; ty<height; ty+=theight) {
        for (size_t tx=0; tx<width; tx+=twidth) {
            uint32_t size;
            memcpy(&size, &src[4*tile], sizeof(uint32_t));
            if (size > (src_size - src_pos)) return 0;
            int success = unpack_12bit_average(twidth, theight, diff,
                &src[src_pos], size,
                &dest[(ty*dy)+(tx*dx)],
                dx, dy);
            if (success != 1) return 0;
            src_pos += size;
            tile++;
        }
    }
    return 1;
}
