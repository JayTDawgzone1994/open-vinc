/* Compile/link only: verifies Tcl's external zlib headers and library. */
#include <zlib.h>

int main(void) {
    return zlibVersion() == 0;
}
