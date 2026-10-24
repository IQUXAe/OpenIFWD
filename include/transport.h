#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct transport_s transport_t;

typedef struct {
    int (*open)(transport_t *t, const char *device_name, uint32_t baudrate);
    int (*close)(transport_t *t);
    int (*read)(transport_t *t, void *buf, size_t count, int timeout_ms);
    int (*write)(transport_t *t, const void *buf, size_t count);
    int (*set_baudrate)(transport_t *t, uint32_t baudrate);
    int (*set_control)(transport_t *t, bool dtr, bool rts);
    int (*flush)(transport_t *t);
    int (*reclaim)(transport_t *t);
    int (*switch_interface)(transport_t *t, int iface, uint8_t ep_out, uint8_t ep_in);
    void (*destroy)(transport_t *t);
} transport_ops_t;

struct transport_s {
    const transport_ops_t *ops;
    void *priv;
};

/* Constructors */
transport_t *transport_create_tty(void);
transport_t *transport_create_usb(void);

/* Convenience wrappers */
static inline int transport_open(transport_t *t, const char *dev, uint32_t baud) {
    return t && t->ops && t->ops->open ? t->ops->open(t, dev, baud) : -1;
}

static inline int transport_close(transport_t *t) {
    return t && t->ops && t->ops->close ? t->ops->close(t) : -1;
}

static inline int transport_read(transport_t *t, void *buf, size_t count, int timeout_ms) {
    return t && t->ops && t->ops->read ? t->ops->read(t, buf, count, timeout_ms) : -1;
}

static inline int transport_write(transport_t *t, const void *buf, size_t count) {
    return t && t->ops && t->ops->write ? t->ops->write(t, buf, count) : -1;
}

static inline int transport_set_baudrate(transport_t *t, uint32_t baud) {
    return t && t->ops && t->ops->set_baudrate ? t->ops->set_baudrate(t, baud) : -1;
}

static inline int transport_set_control(transport_t *t, bool dtr, bool rts) {
    return t && t->ops && t->ops->set_control ? t->ops->set_control(t, dtr, rts) : -1;
}

static inline int transport_flush(transport_t *t) {
    return t && t->ops && t->ops->flush ? t->ops->flush(t) : -1;
}

static inline int transport_reclaim(transport_t *t) {
    return t && t->ops && t->ops->reclaim ? t->ops->reclaim(t) : 0;
}

static inline int transport_switch_interface(transport_t *t, int iface, uint8_t ep_out, uint8_t ep_in) {
    return t && t->ops && t->ops->switch_interface ? t->ops->switch_interface(t, iface, ep_out, ep_in) : 0;
}

static inline void transport_destroy(transport_t *t) {
    if (t && t->ops && t->ops->destroy) {
        t->ops->destroy(t);
    }
}

/* System port & USB device discovery */
int transport_scan_ports(void);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_H */
