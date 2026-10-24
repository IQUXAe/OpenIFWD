#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <libusb-1.0/libusb.h>

typedef struct {
    libusb_context *ctx;
    libusb_device_handle *dev_handle;
    uint8_t ep_in;
    uint8_t ep_out;
    int interface_number;
    uint8_t rx_buf[4096];
    size_t rx_head;
    size_t rx_tail;
} usb_priv_t;

static int usb_open(transport_t *t, const char *device_name, uint32_t baudrate) {
    (void)baudrate;
    usb_priv_t *priv = (usb_priv_t *)t->priv;

    if (libusb_init(&priv->ctx) < 0) {
        fprintf(stderr, "libusb_init failed\n");
        return -1;
    }

    /* Known Samsung & Infineon Vendor IDs */
    static const struct { uint16_t vid; uint16_t pid; } known_devices[] = {
        { 0x058b, 0x0041 }, /* Infineon Flash Loader utility (Boot ROM) */
        { 0x058b, 0x1141 }, /* Infineon Flash Loader / FlashUSB (EBL) */
        { 0x058b, 0x0015 }, /* Infineon FlashUSB */
        { 0x04e8, 0x6860 }, /* Samsung USB */
        { 0x04e8, 0x6601 }, /* Samsung Download Mode */
        { 0x1519, 0x0020 }, /* Comneon / Infineon modem */
        { 0x1519, 0x0452 }, /* Infineon X-GOLD USB */
        { 0, 0 }
    };

    uint16_t target_vid = 0, target_pid = 0;
    bool has_target = (device_name && sscanf(device_name, "%hx:%hx", &target_vid, &target_pid) == 2);

    printf("[*] Waiting for phone connection on USB (hold Volume Down and insert USB cable)...\n");
    time_t start = time(NULL);
    while (time(NULL) - start < 30) {
        if (has_target) {
            priv->dev_handle = libusb_open_device_with_vid_pid(priv->ctx, target_vid, target_pid);
        } else {
            /* Auto-probe known devices */
            for (int i = 0; known_devices[i].vid != 0; i++) {
                priv->dev_handle = libusb_open_device_with_vid_pid(priv->ctx, known_devices[i].vid, known_devices[i].pid);
                if (priv->dev_handle) {
                    printf("[+] Found Infineon/Samsung USB device: %04x:%04x\n", known_devices[i].vid, known_devices[i].pid);
                    break;
                }
            }
        }
        if (priv->dev_handle) break;
        usleep(20000); /* 20ms poll */
    }

    if (!priv->dev_handle) {
        fprintf(stderr, "[-] Timeout waiting for USB device\n");
        return -1;
    }

    /* Default bulk endpoints */
    priv->ep_out = 0;
    priv->ep_in  = 0;
    priv->interface_number = -1;

    /* Discover actual bulk endpoints and interface from descriptor */
    struct libusb_config_descriptor *conf_desc = NULL;
    if (libusb_get_active_config_descriptor(libusb_get_device(priv->dev_handle), &conf_desc) == 0) {
        for (int i = 0; i < conf_desc->bNumInterfaces; i++) {
            for (int a = 0; a < conf_desc->interface[i].num_altsetting; a++) {
                const struct libusb_interface_descriptor *iface = &conf_desc->interface[i].altsetting[a];
                for (int e = 0; e < iface->bNumEndpoints; e++) {
                    const struct libusb_endpoint_descriptor *ep = &iface->endpoint[e];
                    if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
                        if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) {
                            if (priv->ep_in == 0) {
                                priv->ep_in = ep->bEndpointAddress;
                                priv->interface_number = iface->bInterfaceNumber;
                            }
                        } else {
                            if (priv->ep_out == 0) {
                                priv->ep_out = ep->bEndpointAddress;
                                priv->interface_number = iface->bInterfaceNumber;
                            }
                        }
                    }
                }
            }
        }
        libusb_free_config_descriptor(conf_desc);
    }

    if (priv->ep_out == 0) priv->ep_out = 0x01;
    if (priv->ep_in == 0)  priv->ep_in  = 0x81;
    if (priv->interface_number < 0) priv->interface_number = 1;

    /* Detach kernel driver on all interfaces if active (e.g. usb_serial_simple) */
    libusb_set_auto_detach_kernel_driver(priv->dev_handle, 1);
    for (int iface_idx = 0; iface_idx < 4; iface_idx++) {
        if (libusb_kernel_driver_active(priv->dev_handle, iface_idx) == 1) {
            libusb_detach_kernel_driver(priv->dev_handle, iface_idx);
        }
    }

    if (libusb_claim_interface(priv->dev_handle, priv->interface_number) < 0) {
        fprintf(stderr, "[-] Failed to claim interface %d\n", priv->interface_number);
        libusb_close(priv->dev_handle);
        priv->dev_handle = NULL;
        return -1;
    }

    printf("[*] Claimed interface %d: Bulk OUT = 0x%02X, Bulk IN = 0x%02X\n",
           priv->interface_number, priv->ep_out, priv->ep_in);

    return 0;
}

static int usb_close(transport_t *t) {
    usb_priv_t *priv = (usb_priv_t *)t->priv;
    if (priv->dev_handle) {
        libusb_release_interface(priv->dev_handle, priv->interface_number);
        libusb_close(priv->dev_handle);
        priv->dev_handle = NULL;
    }
    if (priv->ctx) {
        libusb_exit(priv->ctx);
        priv->ctx = NULL;
    }
    priv->rx_head = 0;
    priv->rx_tail = 0;
    return 0;
}

static int usb_read(transport_t *t, void *buf, size_t count, int timeout_ms) {
    usb_priv_t *priv = (usb_priv_t *)t->priv;
    if (!priv->dev_handle) return -1;
    if (count == 0) return 0;

    uint8_t *out = (uint8_t *)buf;
    size_t copied = 0;

    /* 1. Return any buffered bytes first */
    while (copied < count && priv->rx_head < priv->rx_tail) {
        out[copied++] = priv->rx_buf[priv->rx_head++];
    }
    if (priv->rx_head == priv->rx_tail) {
        priv->rx_head = 0;
        priv->rx_tail = 0;
    }
    if (copied > 0) {
        return (int)copied;
    }

    /* 2. Buffer is empty, perform a bulk transfer into rx_buf */
    int transferred = 0;
    int r = libusb_bulk_transfer(priv->dev_handle, priv->ep_in, priv->rx_buf, sizeof(priv->rx_buf), &transferred, timeout_ms);

    if (r == 0 && transferred > 0) {
        priv->rx_head = 0;
        priv->rx_tail = (size_t)transferred;
        while (copied < count && priv->rx_head < priv->rx_tail) {
            out[copied++] = priv->rx_buf[priv->rx_head++];
        }
        if (priv->rx_head == priv->rx_tail) {
            priv->rx_head = 0;
            priv->rx_tail = 0;
        }
        return (int)copied;
    }

    if (r == 0 || r == LIBUSB_ERROR_TIMEOUT) {
        return 0;
    }

    fprintf(stderr, "[!] libusb bulk read error on EP 0x%02X: %s\n", priv->ep_in, libusb_error_name(r));
    return -1;
}

static int usb_write(transport_t *t, const void *buf, size_t count) {
    usb_priv_t *priv = (usb_priv_t *)t->priv;
    if (!priv->dev_handle) return -1;

    const uint8_t *p = (const uint8_t *)buf;
    size_t total_written = 0;

    while (total_written < count) {
        size_t chunk_len = count - total_written;
        if (chunk_len > 512) chunk_len = 512;

        int transferred = 0;
        int r = libusb_bulk_transfer(priv->dev_handle, priv->ep_out, (unsigned char *)(p + total_written), (int)chunk_len, &transferred, 5000);
        if (r != 0) {
            fprintf(stderr, "[!] libusb bulk write error on EP 0x%02X: %s\n", priv->ep_out, libusb_error_name(r));
            return -1;
        }
        total_written += (size_t)transferred;
    }
    return (int)total_written;
}

static int usb_set_baudrate(transport_t *t, uint32_t baudrate) {
    (void)t;
    (void)baudrate;
    /* Native USB bulk transfer doesn't depend on UART baudrate */
    return 0;
}

static int usb_set_control(transport_t *t, bool dtr, bool rts) {
    (void)t;
    (void)dtr;
    (void)rts;
    return 0;
}

static int usb_flush(transport_t *t) {
    usb_priv_t *priv = (usb_priv_t *)t->priv;
    if (!priv->dev_handle) return -1;

    priv->rx_head = 0;
    priv->rx_tail = 0;

    /* Drain any pending bytes on Bulk IN without touching EP 0x00 */
    if (priv->ep_in) {
        unsigned char dummy[512];
        int transferred = 0;
        while (libusb_bulk_transfer(priv->dev_handle, priv->ep_in, dummy, sizeof(dummy), &transferred, 20) == 0 && transferred > 0) {
            /* drained */
        }
    }
    return 0;
}

static int usb_reclaim(transport_t *t) {
    usb_priv_t *priv = (usb_priv_t *)t->priv;
    if (!priv->dev_handle) return -1;

    printf("[*] Performing USB bus reset (SE0) to trigger EBL enumeration...\n");
    int r = libusb_reset_device(priv->dev_handle);
    if (r != 0) {
        printf("[*] Bus reset notification: %s (initiating re-enumeration)\n", libusb_error_name(r));
    }

    /* Release old interface and close old handle */
    usb_close(t);

    /* Settling delay for Linux kernel xHCI and EBL to re-enumerate and configure endpoints */
    printf("[*] Waiting for USB re-enumeration...\n");
    usleep(600000); /* 600ms */

    /* Re-open USB device with fresh enumeration and claimed interface */
    if (usb_open(t, NULL, 115200) != 0) {
        fprintf(stderr, "[-] Failed to re-open USB device after bus reset\n");
        return -1;
    }

    return 0;
}

static void usb_destroy(transport_t *t) {
    if (!t) return;
    usb_close(t);
    free(t->priv);
    free(t);
}

static const transport_ops_t usb_ops = {
    .open = usb_open,
    .close = usb_close,
    .read = usb_read,
    .write = usb_write,
    .set_baudrate = usb_set_baudrate,
    .set_control = usb_set_control,
    .flush = usb_flush,
    .reclaim = usb_reclaim,
    .destroy = usb_destroy
};

transport_t *transport_create_usb(void) {
    transport_t *t = (transport_t *)malloc(sizeof(transport_t));
    if (!t) return NULL;

    usb_priv_t *priv = (usb_priv_t *)calloc(1, sizeof(usb_priv_t));
    if (!priv) {
        free(t);
        return NULL;
    }

    t->ops = &usb_ops;
    t->priv = priv;
    return t;
}
