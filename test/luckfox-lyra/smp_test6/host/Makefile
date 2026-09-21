# smp_test6 host-side test apps (native, libusb-1.0, C++23)
CXX      ?= g++
CXXFLAGS := -std=c++20 -O2 -Wall $(shell pkg-config --cflags libusb-1.0)
LDLIBS   := $(shell pkg-config --libs libusb-1.0)
OUT      := output

APPS := usb_test_host usb_recv_host
TARGETS := $(addprefix $(OUT)/,$(APPS))

all: $(TARGETS)

$(OUT):
	mkdir -p $@

$(OUT)/%: %.cpp libusb_wrapper.hpp | $(OUT)
	$(CXX) $(CXXFLAGS) $< $(LDLIBS) -o $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
