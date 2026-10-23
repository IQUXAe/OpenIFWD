#include "transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb-1.0/libusb.h>

typedef struct {
    libusb_context *ctx;
    libusb_device_handle *dev_handle;
    uint8_t ep_in;
    uint8_t ep_out;
    int interface_number;
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
        { 0x04e8, 0x6860 }, /* Samsung USB */
        { 0x04e8, 0x6601 }, /* Samsung Download Mode */
        { 0x058b, 0x0015 }, /* Infineon FlashUSB */
        { 0x1519, 0x0020 }, /* Comneon / Infineon modem */
        { 0x1519, 0x0452 }, /* Infineon X-GOLD USB */
        { 0, 0 }
    };

    uint16_t target_vid = 0, target_pid = 0;
    if (device_name && sscanf(device_name, "%hx:%hx", &target_vid, &target_pid) == 2) {
        priv->dev_handle = libusb_open_device_with_vid_pid(priv->ctx, target_vid, target_pid);
    } else {
        /* Auto-probe known devices */
        for (int i = 0; known_devices[i].vid != 0; i++) {
            priv->dev_handle = libusb_open_device_with_vid_pid(priv->ctx, known_devices[i].vid, known_devices[i].pid);
            if (priv->dev_handle) {
                printf("Found Infineon/Samsung USB device: %04x:%04x\n", known_devices[i].vid, known_devices[i].pid);
                break;
            }
        }
    }

    if (!priv->dev_handle) {
        return -1;
    }

    /* Detach kernel driver if active */
    if (libusb_kernel_driver_active(priv->dev_handle, 0) == 1) {
        libusb_detach_kernel_driver(priv->dev_handle, 0);
    }

    if (libusb_claim_interface(priv->dev_handle, 0) < 0) {
        libusb_close(priv->dev_handle);
        priv->dev_handle = NULL;
        return -1;
    }

    /* Default bulk endpoints for Infineon USB bootloader */
    priv->ep_out = 0x01; /* EP 1 OUT */
    priv->ep_in  = 0x82; /* EP 2 IN */
    priv->interface_number = 0;

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
    return 0;
}

static int usb_read(transport_t *t, void *buf, size_t count, int timeout_ms) {
    usb_priv_t *priv = (usb_priv_t *)t->priv;
    if (!priv->dev_handle) return -1;

    int transferred = 0;
    int r = libusb_bulk_transfer(priv->dev_handle, priv->ep_in, (unsigned char *)buf, (int)count, &transferred, timeout_ms);
    if (r == 0 || r == LIBUSB_ERROR_TIMEOUT) {
        return transferred;
    }
    return -1;
}

static int usb_write(transport_t *t, const void *buf, size_t count) {
    usb_priv_t *priv = (usb_priv_t *)t->priv;
    if (!priv->dev_handle) return -1;

    int transferred = 0;
    int r = libusb_bulk_transfer(priv->dev_handle, priv->ep_out, (unsigned char *)buf, (int)count, &transferred, 3000);
    if (r == 0) {
        return transferred;
    }
    return -1;
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
    (void)t;
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
