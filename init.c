/* Minimal PID 1: the only userspace program in the system. */
#include <stdio.h>
#include <unistd.h>

int main(void) {
    puts("ALXA OS - made by alxa (Linux kernel + custom init)");
    for (;;) pause();   /* PID 1 must never exit */
}
