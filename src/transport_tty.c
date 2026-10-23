#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <dirent.h>

typedef struct {
    int fd;
    char port_name[256];
    uint32_t baudrate;
    struct termios orig_tio;
} tty_priv_t;

static speed_t baud_to_speed(uint32_t baud) {
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
#ifdef B460800
        case 460800: return B460800;
#endif
#ifdef B921600
        case 921600: return B921600;
#endif
        default:     return B115200;
    }
}

static int tty_apply_settings(int fd, uint32_t baud) {
    struct termios tio;
    memset(&tio, 0, sizeof(tio));

    if (tcgetattr(fd, &tio) != 0) {
        perror("tcgetattr");
        return -1;
    }

    /* Set raw mode */
    cfmakeraw(&tio);

    /* 8N1 */
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;

    /* Disable hardware flow control */
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cflag |= (CLOCAL | CREAD);

    /* Set non-blocking with 0.1s minimum timer */
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1;

    speed_t spd = baud_to_speed(baud);
    cfsetispeed(&tio, spd);
    cfsetospeed(&tio, spd);

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        perror("tcsetattr");
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return 0;
}

static int tty_open(transport_t *t, const char *device_name, uint32_t baudrate) {
    tty_priv_t *priv = (tty_priv_t *)t->priv;
    if (priv->fd >= 0) {
        close(priv->fd);
        priv->fd = -1;
    }

    int fd = -1;
    for (int retry = 0; retry < 300; retry++) {
        fd = open(device_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd >= 0) break;
        if (retry == 0) {
            printf("[*] Waiting for %s to become available (connect phone)...\n", device_name);
        }
        usleep(100000); /* 100ms */
    }
    if (fd < 0) {
        return -1;
    }

    /* Save original termios */
    tcgetattr(fd, &priv->orig_tio);

    if (tty_apply_settings(fd, baudrate) != 0) {
        close(fd);
        return -1;
    }

    priv->fd = fd;
    priv->baudrate = baudrate;
    strncpy(priv->port_name, device_name, sizeof(priv->port_name) - 1);
    return 0;
}

static int tty_close(transport_t *t) {
    tty_priv_t *priv = (tty_priv_t *)t->priv;
    if (priv->fd >= 0) {
        tcsetattr(priv->fd, TCSANOW, &priv->orig_tio);
        close(priv->fd);
        priv->fd = -1;
    }
    return 0;
}

static int tty_read(transport_t *t, void *buf, size_t count, int timeout_ms) {
    tty_priv_t *priv = (tty_priv_t *)t->priv;
    if (priv->fd < 0 || !buf || count == 0) return -1;

    uint8_t *p = (uint8_t *)buf;
    size_t total_read = 0;

    while (total_read < count) {
        struct pollfd pfd = { .fd = priv->fd, .events = POLLIN };
        int ret = poll(&pfd, 1, timeout_ms);

        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;
        } else if (ret == 0) {
            /* Timeout */
            break;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return -1;
        }

        if (pfd.revents & POLLIN) {
            ssize_t n = read(priv->fd, p + total_read, count - total_read);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                return -1;
            }
            if (n == 0) break;
            total_read += (size_t)n;
        }
    }
    return (int)total_read;
}

static int tty_write(transport_t *t, const void *buf, size_t count) {
    tty_priv_t *priv = (tty_priv_t *)t->priv;
    if (priv->fd < 0 || !buf || count == 0) return -1;

    const uint8_t *p = (const uint8_t *)buf;
    size_t total_written = 0;

    while (total_written < count) {
        ssize_t n = write(priv->fd, p + total_written, count - total_written);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                usleep(1000);
                continue;
            }
            return -1;
        }
        total_written += (size_t)n;
    }
    return (int)total_written;
}

static int tty_set_baudrate(transport_t *t, uint32_t baudrate) {
    tty_priv_t *priv = (tty_priv_t *)t->priv;
    if (priv->fd < 0) return -1;
    if (tty_apply_settings(priv->fd, baudrate) == 0) {
        priv->baudrate = baudrate;
        return 0;
    }
    return -1;
}

static int tty_set_control(transport_t *t, bool dtr, bool rts) {
    tty_priv_t *priv = (tty_priv_t *)t->priv;
    if (priv->fd < 0) return -1;

    int status = 0;
    ioctl(priv->fd, TIOCMGET, &status);

    if (dtr) status |= TIOCM_DTR;
    else     status &= ~TIOCM_DTR;

    if (rts) status |= TIOCM_RTS;
    else     status &= ~TIOCM_RTS;

    return ioctl(priv->fd, TIOCMSET, &status);
}

static int tty_flush(transport_t *t) {
    tty_priv_t *priv = (tty_priv_t *)t->priv;
    if (priv->fd < 0) return -1;
    return tcflush(priv->fd, TCIOFLUSH);
}

static void tty_destroy(transport_t *t) {
    if (!t) return;
    tty_close(t);
    free(t->priv);
    free(t);
}

static const transport_ops_t tty_ops = {
    .open = tty_open,
    .close = tty_close,
    .read = tty_read,
    .write = tty_write,
    .set_baudrate = tty_set_baudrate,
    .set_control = tty_set_control,
    .flush = tty_flush,
    .destroy = tty_destroy
};

transport_t *transport_create_tty(void) {
    transport_t *t = (transport_t *)malloc(sizeof(transport_t));
    if (!t) return NULL;

    tty_priv_t *priv = (tty_priv_t *)calloc(1, sizeof(tty_priv_t));
    if (!priv) {
        free(t);
        return NULL;
    }
    priv->fd = -1;

    t->ops = &tty_ops;
    t->priv = priv;
    return t;
}

int transport_scan_ports(void) {
    DIR *dir = opendir("/dev");
    if (!dir) return -1;

    printf("Available communication ports under /dev:\n");
    int count = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "ttyACM", 6) == 0 ||
            strncmp(ent->d_name, "ttyUSB", 6) == 0) {
            printf("  -> /dev/%s\n", ent->d_name);
            count++;
        }
    }
    closedir(dir);

    if (count == 0) {
        printf("  (No ttyACM* or ttyUSB* devices currently detected)\n");
    }
    return count;
}
