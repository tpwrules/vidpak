#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>

#include "pack.h"

#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// expand image from color 8 bits to grayscale 16 bits with the three color
// channels beside each other
void expand_image(uint8_t* in, uint16_t* out, int ix, int iy) {
    for (int y=0; y<iy; y++) {
        for (int x=0; x<ix; x++) {
            // split to planes for better compression,
            // horizonally so we don't break the (iy % 4) == 0 rule
            out[x+0*ix] = in[x+0];
            out[x+1*ix] = in[x+1];
            out[x+2*ix] = in[x+2];
        }
        in += ix;
        out += (3*ix);
    }
}

// compute the size of the file at the given path
size_t get_file_size(char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fclose(fp);

    return (size_t)size;
}

// get the current time in float seconds from a monotonic clock
double get_time() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);

    return (double)t.tv_sec + ((double)t.tv_nsec/1e9);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "args: %s path/to/image\n", argv[0]);
        return 1;
    }

    printf("loading input image...\n");
    int ix, iy, ic;
    size_t image_file_size = get_file_size(argv[1]);
    uint8_t* data = stbi_load(argv[1], &ix, &iy, &ic, 3);
    if (!data) {
        fprintf(stderr, "load failed\n");
        return 1;
    }
    size_t raw_data_size = ix*iy*3;

    // expand image to 2 bytes per pixel (the only supported format) with the
    // three color channels horizontal next to each other to avoid problems
    // with image dimensions.
    uint16_t* expanded_image = malloc(ix*iy*3*sizeof(uint16_t));
    if (!expanded_image) {
        fprintf(stderr, "out of memory for expanded image\n");
        stbi_image_free(data);
        return 1;
    }
    expand_image(data, expanded_image, ix, iy);

    // create pack context as 12 bits (the only supported depth)
    // and with each color channel in its own tile for better compression.
    // changing tile size might improve compression further.
    // the tile height needs to be divisible by 4 or context creation will fail.
    pack_context_t* ctx = pack_create_context(ix*3, iy, 12, ix, iy);
    if (!ctx) {
        fprintf(stderr, "failed to create pack context (image dims are probably not compatible)\n");
        free(expanded_image);
        stbi_image_free(data);
        return 1;
    }

    // allocate memory for the packed image. the function
    // `pack_calc_max_packed_size` computes the maximum size required in the
    // worst case scenario.
    printf("packing image...\n");
    uint8_t* packed_data = malloc(pack_calc_max_packed_size(ctx));
    if (!packed_data) {
        fprintf(stderr, "out of memory for packed data\n");
        pack_destroy_context(ctx);
        free(expanded_image);
        stbi_image_free(data);
        return 1;
    }

    // pack the expanded image data. the last two parameters specify the array
    // strides. the exact returned size must be communicated to the unpacking
    // function somehow for the unpacking to complete successfully.
    double pack_start_time = get_time();
    size_t packed_data_size = pack_with_context(ctx,
        expanded_image, packed_data, 1, ix*3);
    double pack_duration = get_time() - pack_start_time;

    if (!packed_data_size) {
        fprintf(stderr, "pack failed\n");
        free(packed_data);
        pack_destroy_context(ctx);
        free(expanded_image);
        stbi_image_free(data);
        return 1;
    }

    printf("unpacking image...\n");
    uint16_t* unpacked_image = (uint16_t*)malloc(ix*iy*3*sizeof(uint16_t));
    if (!unpacked_image) {
        fprintf(stderr, "out of memory for unpacked image\n");
        free(packed_data);
        pack_destroy_context(ctx);
        free(expanded_image);
        stbi_image_free(data);
        return 1;
    }

    // unpack the image data again. note that we have to pass in the exact size
    // of the packed data to unpack it successfully.
    double unpack_start_time = get_time();
    int success = unpack_with_context(ctx, packed_data, packed_data_size,
        unpacked_image, 1, ix*3);
    double unpack_duration = get_time() - unpack_start_time;
    if (!success) {
        fprintf(stderr, "unpack failed\n");
        free(unpacked_image);
        free(packed_data);
        pack_destroy_context(ctx);
        free(expanded_image);
        stbi_image_free(data);
        return 1;
    }

    // verify that the unpacked image matches the packed image.
    if (memcmp(expanded_image, unpacked_image, ix*iy*3*sizeof(uint16_t))) {
        printf("pack was not lossless!!!!\n");
    } else {
        printf("yay: pack was lossless!\n");
    }

    printf("\nstats:\n");
    printf("input file size: %lu\n", image_file_size);
    printf("raw data size: %lu\n", raw_data_size);
    printf("packed data size: %lu\n", packed_data_size);
    printf("packed size relative to input size: %.2f%%\n",
        100*(float)packed_data_size/(float)image_file_size);
    printf("packed size relative to raw size: %.2f%%\n",
        100*(float)packed_data_size/(float)raw_data_size);
    printf("pack time: %.2fms\n", pack_duration*1e3);
    printf("unpack time: %.2fms\n", unpack_duration*1e3);

    free(unpacked_image);
    free(packed_data);
    pack_destroy_context(ctx);
    free(expanded_image);
    stbi_image_free(data);
    return 0;
}
