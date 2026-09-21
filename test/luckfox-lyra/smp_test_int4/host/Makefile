# Host-side keyboard forwarder for smp_test_int3 (native Linux, libusb-1.0)

CC      ?= gcc
CFLAGS  += -O2 -Wall -Wextra
LDLIBS  += -lusb-1.0

kbd_forward: kbd_forward.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f kbd_forward

.PHONY: clean
