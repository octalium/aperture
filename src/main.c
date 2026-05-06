#include <stdio.h>

#ifndef APERTURE_VERSION
#define APERTURE_VERSION "0.0.0"
#endif

int main(void)
{
    printf("aperture %s\n", APERTURE_VERSION);
    return 0;
}
