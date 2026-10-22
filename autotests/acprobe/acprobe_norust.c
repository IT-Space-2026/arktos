/*
 * acprobe_norust.c — the no-rust leg of the acprobe autotest.
 *
 * The --bridge harness dispatches real envelopes through the
 * rustcore static library; with CONFIG+=no-rust there is no library
 * to link, so this stub reports the skip and exits green — the same
 * contract the ARKTOS_RUSTCORE-gated Qt tests use for their
 * compiled-out legs.
 */

#include <stdio.h>

int main(void)
{
    puts("acprobe --bridge: skipped (built without rustcore)");
    return 0;
}
