#ifndef PACK_H
#define PACK_H

#include <inttypes.h>

// temporary data used during pack and unpack operations. contexts are not
// thread-safe!
typedef struct pack_context_t pack_context_t;

// create a pack context to pack (or unpack) frames of the specified size, bits
// per pixel, and tile size. contexts are not thread-safe!
pack_context_t* pack_create_context(int width, int height, int bpp,
        int twidth, int theight);
// destroy a pack context
void pack_destroy_context(pack_context_t* ctx);

// calculate the maximum possible size of a packed frame
size_t pack_calc_max_packed_size(pack_context_t* ctx);

// eventually these will select the packer stored in the context

// pack a frame using the specified context. returns the number of bytes
// actually written to the destination buffer, or 0 if failed. its size MUST BE
// at least the size specified by pack_calc_max_packed_size. dx and dy are the
// number of pixels to advance after each pixel in the x and y direction, i.e.
// to pack the whole input array, dx=1 and dy=width. 
size_t pack_with_context(pack_context_t* ctx,
        const uint16_t* src, uint8_t* dest,
        ssize_t dx, ssize_t dy);
// unpack a frame using the specified context. returns 1 if successful or 0 if
// failed. src_size MUST BE exactly the size returned by pack_with_context. dx
// and dy are the number of pixels to advance after each pixel in the x and y
// direction, i.e. to unpack the whole output array, dx=1 and dy=width.
int unpack_with_context(pack_context_t* ctx,
        const uint8_t* src, size_t src_size, uint16_t* dest,
        ssize_t dx, ssize_t dy);

#endif
