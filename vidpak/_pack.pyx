"""Actual packing routines, implemented in C."""

from cpython cimport array
from libc.stdint cimport uint8_t, uint16_t, uint32_t

import numpy as np

cdef extern from "pack.h":
    ctypedef struct pack_context_t:
        pass

    pack_context_t* pack_create_context(int width, int height, int bpp,
            int twidth, int theight)
    void pack_destroy_context(pack_context_t* ctx)
    size_t pack_calc_max_packed_size(pack_context_t* ctx)

    size_t pack_with_context(pack_context_t* ctx,
            const uint16_t* src, uint8_t* dest,
            size_t dx, size_t dy) nogil
    int unpack_with_context(pack_context_t* ctx,
            const uint8_t* src, size_t src_size, uint16_t* dest,
            size_t dx, size_t dy) nogil

cdef class PackContext:
    cdef pack_context_t* _ctx
    cdef readonly ssize_t max_packed_size # signed types to avoid warnings
    cdef readonly ssize_t _ctx_w
    cdef readonly ssize_t _ctx_h

    def __cinit__(self, int width, int height, int bpp, int twidth, int theight):
        if width <= 0 or height <= 0:
            raise ValueError(
                "width {} and height {} must be positive".format(width, height))
        if twidth <= 0 or theight <= 0:
            raise ValueError("tile width {} and height {} must be "
                "positive".format(twidth, theight))
        if twidth > width or theight > height:
            raise ValueError(f"tile dimensions {twidth}x{theight} can't exceed "
                f"frame dimensions {width}x{height}")
        if bpp != 12:
            raise ValueError("BPP {} is not supported".format(bpp))

        self._ctx = pack_create_context(width, height, bpp, twidth, theight)
        if not self._ctx: raise MemoryError("failed to create pack context")
        self.max_packed_size = pack_calc_max_packed_size(self._ctx)
        self._ctx_w = width
        self._ctx_h = height

    def pack(self, src, dest=None):
        cdef const uint16_t[:, :] src_arr = src
        if (src_arr.strides[0] & 1) or (src_arr.strides[1] & 1):
            raise ValueError("input strides can't be odd")
        cdef ssize_t dx = src_arr.strides[1]//2
        cdef ssize_t dy = src_arr.strides[0]//2
        if src_arr.shape[1] != self._ctx_w or src_arr.shape[0] != self._ctx_h:
            raise ValueError("source dimensions don't match context dimensions")

        cdef uint8_t[::1] dest_arr
        if dest is None:
            arr = array.clone(array.array('B', []), self.max_packed_size, False)
            dest_arr = arr
        else:
            if len(dest) < self.max_packed_size:
                raise ValueError("destination buffer is not large enough")
            dest_arr = dest

        cdef const uint16_t* src_ptr = &src_arr[0, 0]
        cdef uint8_t* dest_ptr = &dest_arr[0]
        with nogil:
            compressed_size = pack_with_context(self._ctx,
                src_ptr, dest_ptr, dx, dy)
        if compressed_size == 0: raise Exception("compression failed")

        if dest is None:
            array.resize(arr, compressed_size)
            return arr, compressed_size
        else:
            return dest, compressed_size

    def unpack(self, src, dest=None):
        cdef const uint8_t[::1] src_arr = src

        if dest is None:
            dest = np.empty((self._ctx_h, self._ctx_w), dtype=np.uint16)
        cdef uint16_t[:, :] dest_arr = dest
        if (dest_arr.strides[0] & 1) or (dest_arr.strides[1] & 1):
            raise ValueError("output strides can't be odd")
        cdef ssize_t dx = dest_arr.strides[1]//2
        cdef ssize_t dy = dest_arr.strides[0]//2
        if dest_arr.shape[1] != self._ctx_w or dest_arr.shape[0] != self._ctx_h:
            raise ValueError("dest dimensions don't match context dimensions")

        cdef const uint8_t* src_ptr = &src_arr[0]
        cdef uint16_t* dest_ptr = &dest_arr[0, 0]
        cdef size_t src_len = len(src_arr)
        with nogil:
            success = unpack_with_context(self._ctx,
                src_ptr, src_len, dest_ptr, dx, dy)
        if success != 1: raise Exception("decompression failed")

        return dest

    def __dealloc__(self):
        pack_destroy_context(self._ctx)
