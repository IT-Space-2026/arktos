/* WK01 link/run probe — NOT in any build system; compile via ./build-probe.sh.
 *
 * Trivial: call webkit_get_{major,minor,micro}_version() and print the
 * triple. Proves three things at once against the WK00 ~/wpe prefix:
 *   1. headers resolve        (pkg-config --cflags wpe-webkit-2.0)
 *   2. the lib links          (sid glibc dynamic-linker + explicit
 *                              sid libc.so.6/libm.so.6 for GLIBC_2.4x
 *                              versioned refs — see ACQUIRED.md)
 *   3. runtime loading works  (sid ld.so interp + rpath wpe+glibc +
 *                              LD_PRELOAD g_sort_array shim)
 */
#include <stdio.h>
#include <wpe/webkit.h>

int main(void)
{
    printf("WPE WebKit %u.%u.%u\n",
           webkit_get_major_version(),
           webkit_get_minor_version(),
           webkit_get_micro_version());
    return 0;
}
