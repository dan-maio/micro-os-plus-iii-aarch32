/**
 * @file kbd_forward.c
 * @brief PC-side keyboard forwarder for smp_test_int3.
 *
 * Grabs a Linux evdev keyboard device and forwards every EV_KEY event
 * (press / release / repeat) to the Lyra board's OTG1 gadget (2207:001B)
 * as an 8-byte record on bulk EP1-OUT:
 *
 *   [0]='K'  [1]=value (0=up,1=down,2=repeat)  [2..3]=code LE  [4..7]=seq LE
 *
 * Each record lands on the board as a USB OUT-endpoint interrupt.
 *
 * Usage:
 *   sudo ./kbd_forward                      # auto-pick first keyboard
 *   sudo ./kbd_forward /dev/input/event3    # explicit device
 *
 * (root needed for both evdev and the raw USB device, unless udev rules
 *  grant access.)
 *
 * Build: make   (needs libusb-1.0 dev package)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <poll.h>
#include <linux/input.h>
#include <libusb-1.0/libusb.h>

#define USB_VID  0x2207
#define USB_PID  0x001B
#define TIMEOUT_MS 1000

/* The board probes its DWC2 instance for the first implemented OUT
 * endpoint (OTG1 may lack OUT EP1) and reports it in the config
 * descriptor — so discover the bulk-OUT address instead of hardcoding. */
static int find_bulk_out(libusb_device_handle *dev, uint8_t *ep_out)
{
    struct libusb_config_descriptor *cfg;
    if (libusb_get_active_config_descriptor(libusb_get_device(dev), &cfg) != 0)
        return -1;
    int found = -1;
    const struct libusb_interface_descriptor *alt =
        &cfg->interface[0].altsetting[0];
    for (int i = 0; i < alt->bNumEndpoints; ++i) {
        const struct libusb_endpoint_descriptor *e = &alt->endpoint[i];
        if ((e->bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_BULK &&
            (e->bEndpointAddress & 0x80) == 0) {
            *ep_out = e->bEndpointAddress;
            found = 0;
            break;
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

static volatile int g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

/* Pick the first /dev/input/event* that advertises EV_KEY with letters.
 * Prefer /dev/input/by-id/<*-event-kbd> — gaming keyboards expose several
 * event nodes (macro/consumer interfaces); the by-id "-event-kbd" symlink
 * is the canonical main keyboard interface. */
static int auto_open_keyboard(char *path_out, size_t path_len)
{
    DIR *byid = opendir("/dev/input/by-id");
    if (byid) {
        struct dirent *e;
        while ((e = readdir(byid)) != NULL) {
            size_t n = strlen(e->d_name);
            /* main interface only: "...-event-kbd" but NOT "...-ifNN-event-kbd"
             * (secondary interfaces of gaming keyboards don't emit normal keys) */
            if (n > 10 && strcmp(e->d_name + n - 10, "-event-kbd") == 0 &&
                strstr(e->d_name, "-if") == NULL) {
                char path[512];
                snprintf(path, sizeof path, "/dev/input/by-id/%s", e->d_name);
                int fd = open(path, O_RDONLY);
                if (fd >= 0) {
                    char name[128] = "?";
                    ioctl(fd, EVIOCGNAME(sizeof name), name);
                    printf("Using keyboard: %s (%s)\n", path, name);
                    snprintf(path_out, path_len, "%s", path);
                    closedir(byid);
                    return fd;
                }
            }
        }
        closedir(byid);
    }

    DIR *d = opendir("/dev/input");
    if (!d)
        return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        char path[280];
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        unsigned long evbits = 0;
        unsigned long keybits[(KEY_MAX + 1) / (8 * sizeof(long)) + 1];
        memset(keybits, 0, sizeof keybits);
        ioctl(fd, EVIOCGBIT(0, sizeof evbits), &evbits);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits);
        /* must support EV_KEY and have KEY_A (bit 30) — i.e. a keyboard */
        if ((evbits & (1ul << EV_KEY)) &&
            (keybits[KEY_A / (8 * sizeof(long))] &
             (1ul << (KEY_A % (8 * sizeof(long)))))) {
            char name[128] = "?";
            ioctl(fd, EVIOCGNAME(sizeof name), name);
            printf("Using keyboard: %s (%s)\n", path, name);
            snprintf(path_out, path_len, "%s", path);
            closedir(d);
            return fd;
        }
        close(fd);
    }
    closedir(d);
    return -1;
}

int main(int argc, char *argv[])
{
    char kbd_path[512] = "";
    int kfd;

    if (argc > 1) {
        snprintf(kbd_path, sizeof kbd_path, "%s", argv[1]);
        kfd = open(kbd_path, O_RDONLY);
        if (kfd < 0) {
            perror(kbd_path);
            return 1;
        }
    } else {
        kfd = auto_open_keyboard(kbd_path, sizeof kbd_path);
        if (kfd < 0) {
            fprintf(stderr, "No keyboard found in /dev/input (need root?)\n");
            return 1;
        }
        /* reopen blocking */
        close(kfd);
        kfd = open(kbd_path, O_RDONLY);
        if (kfd < 0) {
            perror(kbd_path);
            return 1;
        }
    }

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) {
        fprintf(stderr, "libusb_init failed\n");
        return 1;
    }
    libusb_device_handle *dev =
        libusb_open_device_with_vid_pid(ctx, USB_VID, USB_PID);
    if (!dev) {
        fprintf(stderr, "Board not found (%04x:%04x). Is USB1 connected "
                        "and the firmware running?\n", USB_VID, USB_PID);
        return 1;
    }
    libusb_set_auto_detach_kernel_driver(dev, 1);
    if (libusb_claim_interface(dev, 0) != 0) {
        fprintf(stderr, "claim_interface failed\n");
        return 1;
    }
    uint8_t ep_out = 0x01;
    if (find_bulk_out(dev, &ep_out) != 0) {
        fprintf(stderr, "No bulk-OUT endpoint in the device descriptor?\n");
        return 1;
    }
    printf("Connected to board %04x:%04x (bulk-OUT ep 0x%02x) — "
           "forwarding keys (Ctrl-C to stop)\n", USB_VID, USB_PID, ep_out);

    /* sigaction WITHOUT SA_RESTART: Ctrl-C must interrupt the blocking
     * evdev read() (plain signal() sets SA_RESTART and the read resumes). */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);

    uint32_t seq = 0;
    struct input_event ev;
    while (!g_stop) {
        /* poll with timeout so Ctrl-C is honored even if this event node
         * never produces anything (wrong interface of a multi-node kbd) */
        struct pollfd pfd = { .fd = kfd, .events = POLLIN };
        int pr = poll(&pfd, 1, 500);
        if (pr < 0)
            break;                     /* EINTR from Ctrl-C */
        if (pr == 0)
            continue;                  /* timeout — recheck g_stop */
        ssize_t n = read(kfd, &ev, sizeof ev);
        if (n != (ssize_t)sizeof ev)
            break;
        if (ev.type != EV_KEY)
            continue;

        uint8_t rec[8];
        rec[0] = 'K';
        rec[1] = (uint8_t)ev.value;             /* 0=up 1=down 2=repeat */
        rec[2] = (uint8_t)(ev.code & 0xFF);
        rec[3] = (uint8_t)(ev.code >> 8);
        rec[4] = (uint8_t)(seq & 0xFF);
        rec[5] = (uint8_t)((seq >> 8) & 0xFF);
        rec[6] = (uint8_t)((seq >> 16) & 0xFF);
        rec[7] = (uint8_t)((seq >> 24) & 0xFF);
        seq++;

        int sent = 0;
        int rc = libusb_bulk_transfer(dev, ep_out, rec, sizeof rec,
                                      &sent, TIMEOUT_MS);
        if (rc != 0) {
            fprintf(stderr, "bulk_transfer: %s\n", libusb_error_name(rc));
            break;
        }
        printf("key %-4u %s  (seq %u)\r\n", ev.code,
               ev.value == 0 ? "up  " : (ev.value == 1 ? "DOWN" : "rep "),
               seq);
    }

    printf("\nStopping.\n");
    libusb_release_interface(dev, 0);
    libusb_close(dev);
    libusb_exit(ctx);
    close(kfd);
    return 0;
}
