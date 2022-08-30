#include <stdio.h>
#include <stdlib.h>

#include "pack.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "args: %s path/to/image\n", argv[0]);
        return 1;
    }
}
