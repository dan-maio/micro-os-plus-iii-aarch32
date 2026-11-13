/* arch/cc.h - compiler/platform glue for lwIP (AArch32, GCC, newlib). */
#ifndef RPIZ2W_LWIP_ARCH_CC_H
#define RPIZ2W_LWIP_ARCH_CC_H

#define BYTE_ORDER LITTLE_ENDIAN

/* lwIP typedefs ssize_t as 'int' unless SSIZE_MAX is defined, which clashes
 * with the toolchain's <stdio.h> ('long'). Defining it makes lwIP take the
 * <unistd.h> branch and use the libc type. On this port long is 32 bits. */
#ifndef SSIZE_MAX
#define SSIZE_MAX 0x7fffffffL
#endif

void kprintf(const char *format, ...) __attribute__((format(printf, 1, 2)));
unsigned int net_random(void);

#define LWIP_PLATFORM_DIAG(x) do { kprintf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) \
    do { kprintf("[NET] lwIP assertion \"%s\" at %s:%d\n", x, __FILE__, __LINE__); } while (0)
#define LWIP_RAND() net_random()

#endif
