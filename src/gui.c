#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <gtk/gtk.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include "fls.h"
#include "protocol.h"
#include "transport.h"
#include "flasher.h"

/* Pixman hook to silence KDE Breeze / Wayland bug where invalid negative rectangles
 * are passed down to Cairo when popup menus or comboboxes are opened.
 */
typedef struct {
    int32_t x1, y1, x2, y2;
} pixman_box32_t;

typedef struct {
    pixman_box32_t extents;
    void *data;
} pixman_region32_t;

extern void pixman_region32_init(pixman_region32_t *region);

void pixman_region32_init_rect(pixman_region32_t *region, int x, int y, unsigned int width, unsigned int height) {
    if ((int)width <= 0 || (int)height <= 0 || (x + (int)width <= x) || (y + (int)height <= y)) {
        pixman_region32_init(region);
        return;
    }
    region->extents.x1 = x;
    region->extents.y1 = y;
    region->extents.x2 = x + width;
    region->extents.y2 = y + height;
    region->data = NULL;
}

static void apply_app_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css =
        "menu, .menu, combobox window, combobox menu, window.popup {\n"
        "    margin: 0;\n"
        "    padding: 2px;\n"
        "    box-shadow: none;\n"
        "}\n"
        "combobox > box > button.combo {\n"
        "    margin: 0;\n"
        "}\n"
        "combobox arrow {\n"
        "    margin: 0;\n"
        "}\n";
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 100
    );
    g_object_unref(provider);
}

typedef struct {
    GtkWidget *window;
    GtkWidget *combo_port;
    GtkWidget *combo_baud;

    GtkWidget *check_builtin_psi;
    GtkWidget *entry_psi;
    GtkWidget *btn_browse_psi;
    GtkWidget *entry_slb;
    GtkWidget *entry_bsy;
    GtkWidget *entry_cds;
    GtkWidget *entry_dffs;

    GtkWidget *check_erase;
    GtkWidget *check_ram_only;
    GtkWidget *entry_ram_addr;

    GtkWidget *progress_bar;
    GtkWidget *btn_start;
    GtkWidget *btn_backup_nvram;
    GtkWidget *btn_ram_boot;
    GtkWidget *text_log;
    GtkTextBuffer *log_buffer;

    bool is_running;
    pthread_t worker_thread;
} gui_app_t;

static gui_app_t g_app;

static void log_to_gui(const char *msg) {
    if (!g_app.log_buffer) return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(g_app.log_buffer, &end);
    gtk_text_buffer_insert(g_app.log_buffer, &end, msg, -1);

    GtkTextMark *mark = gtk_text_buffer_get_insert(g_app.log_buffer);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(g_app.text_log), mark, 0.0, TRUE, 0.0, 1.0);
}

typedef struct {
    char *text;
} idle_log_t;

static gboolean idle_append_log(gpointer data) {
    idle_log_t *il = (idle_log_t *)data;
    if (il && il->text) {
        log_to_gui(il->text);
        free(il->text);
        free(il);
    }
    return FALSE;
}

static void gui_log_async(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    idle_log_t *il = (idle_log_t *)malloc(sizeof(idle_log_t));
    if (il) {
        il->text = strdup(buf);
        g_idle_add(idle_append_log, il);
    }
}

typedef struct {
    double fraction;
    char text[64];
} idle_progress_t;

static gboolean idle_update_progress(gpointer data) {
    idle_progress_t *ip = (idle_progress_t *)data;
    if (ip) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_app.progress_bar), ip->fraction);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(g_app.progress_bar), ip->text);
        free(ip);
    }
    return FALSE;
}

static void gui_set_progress_async(double fraction, const char *fmt, ...) {
    idle_progress_t *ip = (idle_progress_t *)malloc(sizeof(idle_progress_t));
    if (ip) {
        ip->fraction = fraction;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(ip->text, sizeof(ip->text), fmt, ap);
        va_end(ap);
        g_idle_add(idle_update_progress, ip);
    }
}

static gboolean idle_finish_op(gpointer data) {
    (void)data;
    g_app.is_running = false;
    gtk_widget_set_sensitive(g_app.btn_start, TRUE);
    gtk_widget_set_sensitive(g_app.btn_backup_nvram, TRUE);
    gtk_widget_set_sensitive(g_app.btn_ram_boot, TRUE);
    return FALSE;
}

static void gui_op_finished_async(void) {
    g_idle_add(idle_finish_op, NULL);
}

static void scan_and_populate_ports(GtkComboBoxText *combo) {
    gtk_combo_box_text_remove_all(combo);
    DIR *dir = opendir("/dev");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strncmp(ent->d_name, "ttyACM", 6) == 0 ||
                strncmp(ent->d_name, "ttyUSB", 6) == 0) {
                char devpath[512];
                snprintf(devpath, sizeof(devpath), "/dev/%s", ent->d_name);
                gtk_combo_box_text_append_text(combo, devpath);
            }
        }
        closedir(dir);
    }
    gtk_combo_box_text_append_text(combo, "usb (direct libusb)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
}

static void on_btn_scan_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    scan_and_populate_ports(GTK_COMBO_BOX_TEXT(g_app.combo_port));
    gui_log_async("[*] Ports list refreshed.\n");
}

/* Worker Thread: Flash Firmware / Standard Download */
static void *worker_flash_thread(void *arg) {
    flasher_config_t *cfg = (flasher_config_t *)arg;
    gchar *port = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(g_app.combo_port));

    gui_log_async("====================================================\n");
    gui_log_async("[*] Starting flashing session on port: %s\n", port ? port : "unknown");
    gui_set_progress_async(0.05, "Connecting...");

    cfg->is_usb = (port && (strstr(port, "usb") != NULL || strstr(port, "USB") != NULL));

    transport_t *trans = NULL;
    if (cfg->is_usb) {
        trans = transport_create_usb();
        if (!trans || transport_open(trans, NULL, cfg->baudrate) != 0) {
            gui_log_async("[-] Failed to open direct USB interface\n");
            goto finish;
        }
    } else {
        trans = transport_create_tty();
        if (!trans || transport_open(trans, port, 115200) != 0) {
            gui_log_async("[-] Failed to open port %s\n", port ? port : "");
            goto finish;
        }
    }

    flasher_session_t *s = flasher_create(trans, cfg);
    if (!s) {
        gui_log_async("[-] Failed to initialize flasher session\n");
        goto finish;
    }

    gui_log_async("[*] Waiting for phone connection... (Hold Volume Down or reinsert battery)\n");
    if (flasher_sync(s, 40) != 0) {
        gui_log_async("[-] Device synchronization failed or timed out.\n");
        flasher_destroy(s);
        goto finish;
    }
    gui_set_progress_async(0.20, "Synchronized!");
    gui_log_async("[+] Device synchronized!\n");

    /* Check if RAM only boot */
    gboolean ram_only = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_app.check_ram_only));

    /* Upload PSI */
    if (cfg->use_open_psi || !cfg->psi_path || strlen(cfg->psi_path) == 0) {
        gui_log_async("[*] Uploading built-in Open Rust PSI...\n");
        gui_set_progress_async(0.30, "Uploading Open PSI...");
        if (flasher_upload_psi(s, NULL) != 0) {
            gui_log_async("[-] Built-in PSI upload failed\n");
            flasher_destroy(s);
            goto finish;
        }
    } else {
        gui_log_async("[*] Uploading custom PSI: %s\n", cfg->psi_path);
        gui_set_progress_async(0.30, "Uploading PSI...");
        fls_file_t *psi = fls_open(cfg->psi_path);
        int r = flasher_upload_psi(s, psi);
        if (psi) fls_close(psi);
        if (r != 0) {
            gui_log_async("[-] PSI upload failed: %s\n", cfg->psi_path);
            flasher_destroy(s);
            goto finish;
        }
    }

    if (ram_only) {
        /* RAM Boot mode */
        gui_log_async("[*] Operating in RAM-BOOT mode (no flash will be written)\n");
        if (cfg->bsy_path && strlen(cfg->bsy_path) > 0) {
            uint8_t *payload_buf = NULL;
            size_t payload_len = 0;

            fls_file_t *bsy = fls_open(cfg->bsy_path);
            if (bsy) {
                const fls_chunk_info_t *c = fls_find_chunk(bsy, FLS_TAG_IMAGE_DATA);
                if (c) {
                    size_t sz = 0;
                    const uint8_t *d = fls_get_chunk_payload(bsy, c, &sz);
                    if (d && sz > 0) {
                        payload_buf = (uint8_t *)malloc(sz);
                        if (payload_buf) {
                            memcpy(payload_buf, d, sz);
                            payload_len = sz;
                        }
                    }
                }
                fls_close(bsy);
            }
            if (!payload_buf) {
                FILE *f = fopen(cfg->bsy_path, "rb");
                if (f) {
                    fseek(f, 0, SEEK_END);
                    long sz = ftell(f);
                    rewind(f);
                    if (sz > 0) {
                        payload_buf = (uint8_t *)malloc((size_t)sz);
                        if (payload_buf && fread(payload_buf, 1, (size_t)sz, f) == (size_t)sz) {
                            payload_len = (size_t)sz;
                        } else {
                            free(payload_buf);
                            payload_buf = NULL;
                        }
                    }
                    fclose(f);
                }
            }

            if (payload_buf && payload_len > 0) {
                uint32_t addr = cfg->ram_exec_addr ? cfg->ram_exec_addr : 0x60000000;
                gui_log_async("[*] Uploading payload to SDRAM at 0x%08X (%zu bytes)...\n", addr, payload_len);
                gui_set_progress_async(0.70, "Streaming to SDRAM...");
                if (cfg->use_open_psi || !cfg->slb_path || strlen(cfg->slb_path) == 0) {
                    usleep(250000);
                    flasher_open_ping(s);
                    flasher_boot_ram_open(s, addr, payload_buf, payload_len);
                } else {
                    flasher_boot_ram(s, addr, payload_buf, payload_len);
                }
                free(payload_buf);
            } else {
                gui_log_async("[-] Could not load payload from %s\n", cfg->bsy_path);
            }
        }
        gui_set_progress_async(1.0, "RAM Execution Started!");
        gui_log_async("[+] RAM boot sequence completed!\n");
        flasher_destroy(s);
        goto finish;
    }

    /* Full Flash mode: Upload SLB */
    if (cfg->slb_path && strlen(cfg->slb_path) > 0) {
        gui_log_async("[*] Uploading SLB / EBL: %s\n", cfg->slb_path);
        gui_set_progress_async(0.40, "Injecting SLB/EBL...");
        fls_file_t *slb = fls_open(cfg->slb_path);
        if (slb) {
            flasher_upload_ebl(s, slb);
            fls_close(slb);
        } else {
            gui_log_async("[-] Warning: Failed to open SLB: %s\n", cfg->slb_path);
        }
    }

    gui_set_progress_async(0.50, "Configuring protocol...");
    flasher_configure_protocol(s, NULL);
    flasher_query_flash_id(s);

    if (cfg->erase_all) {
        gui_log_async("[*] Erasing Flash memory...\n");
        gui_set_progress_async(0.60, "Erasing flash...");
        flasher_erase_flash(s, 0x00000000, 0x04000000);
    }

    if (cfg->bsy_path && strlen(cfg->bsy_path) > 0) {
        gui_log_async("[*] Flashing Baseband / Code: %s\n", cfg->bsy_path);
        gui_set_progress_async(0.70, "Flashing BSY...");
        fls_file_t *bsy = fls_open(cfg->bsy_path);
        if (bsy) {
            const fls_chunk_info_t *c = fls_find_chunk(bsy, FLS_TAG_IMAGE_DATA);
            if (c) {
                size_t sz = 0;
                const uint8_t *data = fls_get_chunk_payload(bsy, c, &sz);
                flasher_flash_image(s, data, sz, 0x20000000);
            }
            fls_close(bsy);
        }
    }

    if (cfg->cds_path && strlen(cfg->cds_path) > 0) {
        gui_log_async("[*] Flashing CDS: %s\n", cfg->cds_path);
        gui_set_progress_async(0.85, "Flashing CDS...");
        fls_file_t *cds = fls_open(cfg->cds_path);
        if (cds) {
            const fls_chunk_info_t *c = fls_find_chunk(cds, FLS_TAG_IMAGE_DATA);
            if (c) {
                size_t sz = 0;
                const uint8_t *data = fls_get_chunk_payload(cds, c, &sz);
                flasher_flash_image(s, data, sz, 0x21000000);
            }
            fls_close(cds);
        }
    }

    if (cfg->dffs_path && strlen(cfg->dffs_path) > 0) {
        gui_log_async("[*] Flashing CSC / DFFS: %s\n", cfg->dffs_path);
        gui_set_progress_async(0.95, "Flashing DFFS...");
        FILE *df = fopen(cfg->dffs_path, "rb");
        if (df) {
            fseek(df, 0, SEEK_END);
            long sz = ftell(df);
            rewind(df);
            uint8_t *buf = (uint8_t *)malloc(sz);
            if (buf && fread(buf, 1, sz, df) == (size_t)sz) {
                flasher_flash_image(s, buf, sz, 0x22000000);
            }
            free(buf);
            fclose(df);
        }
    }

    gui_log_async("[*] Rebooting phone...\n");
    flasher_reboot(s);
    gui_set_progress_async(1.0, "Success / Download Complete!");
    gui_log_async("[+] Flashing process completed successfully!\n");
    flasher_destroy(s);

finish:
    if (trans) transport_destroy(trans);
    g_free(port);
    if (cfg->psi_path) free((void *)cfg->psi_path);
    if (cfg->slb_path) free((void *)cfg->slb_path);
    if (cfg->bsy_path) free((void *)cfg->bsy_path);
    if (cfg->cds_path) free((void *)cfg->cds_path);
    if (cfg->dffs_path) free((void *)cfg->dffs_path);
    free(cfg);

    gui_op_finished_async();
    return NULL;
}

/* Worker Thread: NVRAM Backup (Blocks 4..11 = 1 MB) */
typedef struct {
    char *port;
    uint32_t baudrate;
    bool is_usb;
    bool use_open_psi;
    char *psi_path;
    char *out_path;
} nvram_worker_args_t;

static void *worker_nvram_thread(void *arg) {
    nvram_worker_args_t *args = (nvram_worker_args_t *)arg;

    gui_log_async("====================================================\n");
    gui_log_async("[*] Starting Safe NVRAM Dump on port: %s\n", args->port ? args->port : "unknown");
    gui_log_async("[*] Destination file: %s\n", args->out_path);
    gui_set_progress_async(0.05, "Connecting...");

    transport_t *trans = NULL;
    if (args->is_usb) {
        trans = transport_create_usb();
        if (!trans || transport_open(trans, NULL, args->baudrate) != 0) {
            gui_log_async("[-] Failed to open direct USB interface\n");
            goto finish;
        }
    } else {
        trans = transport_create_tty();
        if (!trans || transport_open(trans, args->port, 115200) != 0) {
            gui_log_async("[-] Failed to open port %s\n", args->port ? args->port : "");
            goto finish;
        }
    }

    flasher_config_t cfg = {0};
    cfg.is_usb = args->is_usb;
    cfg.baudrate = args->baudrate;
    cfg.use_open_psi = args->use_open_psi;
    cfg.psi_path = args->psi_path;

    flasher_session_t *s = flasher_create(trans, &cfg);
    if (!s) {
        gui_log_async("[-] Failed to initialize flasher session\n");
        goto finish;
    }

    gui_log_async("[*] Waiting for phone connection... (Hold Volume Down or reinsert battery)\n");
    if (flasher_sync(s, 40) != 0) {
        gui_log_async("[-] Device synchronization failed or timed out.\n");
        flasher_destroy(s);
        goto finish;
    }
    gui_set_progress_async(0.20, "Synchronized!");
    gui_log_async("[+] Device synchronized!\n");

    /* Upload PSI */
    gui_set_progress_async(0.35, "Uploading PSI...");
    if (args->use_open_psi || !args->psi_path || strlen(args->psi_path) == 0) {
        gui_log_async("[*] Uploading built-in Open Rust PSI...\n");
        if (flasher_upload_psi(s, NULL) != 0) {
            gui_log_async("[-] Built-in PSI upload failed\n");
            flasher_destroy(s);
            goto finish;
        }
    } else {
        gui_log_async("[*] Uploading custom PSI: %s\n", args->psi_path);
        fls_file_t *psi = fls_open(args->psi_path);
        int r = flasher_upload_psi(s, psi);
        if (psi) fls_close(psi);
        if (r != 0) {
            gui_log_async("[-] Custom PSI upload failed\n");
            flasher_destroy(s);
            goto finish;
        }
    }

    usleep(250000);
    gui_set_progress_async(0.50, "Pinging OpenIFWD agent...");
    if (flasher_open_ping(s) != 0) {
        gui_log_async("[-] OpenIFWD agent ping failed\n");
        flasher_destroy(s);
        goto finish;
    }
    gui_log_async("[+] OpenIFWD agent responded to PING!\n");

    gui_set_progress_async(0.60, "Dumping NVRAM (1MB)...");
    gui_log_async("[*] Reading NVRAM blocks 4..11 (0x60080000 - 0x60180000)...\n");
    if (flasher_dump_nvram_open(s, args->out_path) != 0) {
        gui_log_async("[-] Failed to dump NVRAM.\n");
        gui_set_progress_async(0.0, "NVRAM Dump Failed");
    } else {
        gui_set_progress_async(1.0, "NVRAM Backup Complete!");
        gui_log_async("[+] NVRAM successfully saved to %s (1,048,576 bytes)\n", args->out_path);
    }

    flasher_destroy(s);

finish:
    if (trans) transport_destroy(trans);
    if (args->port) free(args->port);
    if (args->psi_path) free(args->psi_path);
    if (args->out_path) free(args->out_path);
    free(args);

    gui_op_finished_async();
    return NULL;
}

/* Worker Thread: RAM Boot (Load directly to SDRAM and execute) */
typedef struct {
    char *port;
    uint32_t baudrate;
    bool is_usb;
    bool use_open_psi;
    char *psi_path;
    char *payload_path;
    uint32_t target_addr;
} ramboot_worker_args_t;

static void *worker_ramboot_thread(void *arg) {
    ramboot_worker_args_t *args = (ramboot_worker_args_t *)arg;

    gui_log_async("====================================================\n");
    gui_log_async("[*] Starting RAM Boot session on port: %s\n", args->port ? args->port : "unknown");
    gui_log_async("[*] Payload: %s, Target Address: 0x%08X\n", args->payload_path, args->target_addr);
    gui_set_progress_async(0.05, "Connecting...");

    transport_t *trans = NULL;
    uint8_t *payload_buf = NULL;
    size_t payload_len = 0;

    fls_file_t *fls = fls_open(args->payload_path);
    if (fls) {
        const fls_chunk_info_t *c = fls_find_chunk(fls, FLS_TAG_IMAGE_DATA);
        if (c) {
            size_t sz = 0;
            const uint8_t *d = fls_get_chunk_payload(fls, c, &sz);
            if (d && sz > 0) {
                payload_buf = (uint8_t *)malloc(sz);
                if (payload_buf) {
                    memcpy(payload_buf, d, sz);
                    payload_len = sz;
                }
            }
        }
        fls_close(fls);
    }

    if (!payload_buf) {
        FILE *f = fopen(args->payload_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            rewind(f);
            if (sz > 0) {
                payload_buf = (uint8_t *)malloc((size_t)sz);
                if (payload_buf && fread(payload_buf, 1, (size_t)sz, f) == (size_t)sz) {
                    payload_len = (size_t)sz;
                } else {
                    free(payload_buf);
                    payload_buf = NULL;
                }
            }
            fclose(f);
        }
    }

    if (!payload_buf || payload_len == 0) {
        gui_log_async("[-] Failed to read binary payload from %s\n", args->payload_path);
        gui_set_progress_async(0.0, "Payload Error");
        goto finish;
    }

    if (args->is_usb) {
        trans = transport_create_usb();
        if (!trans || transport_open(trans, NULL, args->baudrate) != 0) {
            gui_log_async("[-] Failed to open direct USB interface\n");
            goto finish;
        }
    } else {
        trans = transport_create_tty();
        if (!trans || transport_open(trans, args->port, 115200) != 0) {
            gui_log_async("[-] Failed to open port %s\n", args->port ? args->port : "");
            goto finish;
        }
    }

    flasher_config_t cfg = {0};
    cfg.is_usb = args->is_usb;
    cfg.baudrate = args->baudrate;
    cfg.use_open_psi = args->use_open_psi;
    cfg.psi_path = args->psi_path;

    flasher_session_t *s = flasher_create(trans, &cfg);
    if (!s) {
        gui_log_async("[-] Failed to initialize flasher session\n");
        goto finish;
    }

    gui_log_async("[*] Waiting for phone connection... (Hold Volume Down or reinsert battery)\n");
    if (flasher_sync(s, 40) != 0) {
        gui_log_async("[-] Device synchronization failed or timed out.\n");
        flasher_destroy(s);
        goto finish;
    }
    gui_set_progress_async(0.20, "Synchronized!");
    gui_log_async("[+] Device synchronized!\n");

    /* Upload PSI */
    gui_set_progress_async(0.35, "Uploading PSI...");
    if (args->use_open_psi || !args->psi_path || strlen(args->psi_path) == 0) {
        gui_log_async("[*] Uploading built-in Open Rust PSI...\n");
        if (flasher_upload_psi(s, NULL) != 0) {
            gui_log_async("[-] PSI upload failed\n");
            flasher_destroy(s);
            goto finish;
        }
    } else {
        gui_log_async("[*] Uploading custom PSI: %s\n", args->psi_path);
        fls_file_t *psi = fls_open(args->psi_path);
        int r = flasher_upload_psi(s, psi);
        if (psi) fls_close(psi);
        if (r != 0) {
            gui_log_async("[-] Custom PSI upload failed\n");
            flasher_destroy(s);
            goto finish;
        }
    }

    usleep(250000);
    gui_set_progress_async(0.50, "Pinging OpenIFWD agent...");
    if (flasher_open_ping(s) != 0) {
        gui_log_async("[-] OpenIFWD agent ping failed\n");
        flasher_destroy(s);
        goto finish;
    }
    gui_log_async("[+] OpenIFWD agent responded to PING!\n");

    gui_set_progress_async(0.65, "Streaming to SDRAM...");
    gui_log_async("[*] Streaming payload (%zu bytes) to SDRAM at 0x%08X...\n", payload_len, args->target_addr);
    if (flasher_boot_ram_open(s, args->target_addr, payload_buf, payload_len) != 0) {
        gui_log_async("[-] RAM boot execution failed.\n");
        gui_set_progress_async(0.0, "RAM Boot Failed");
    } else {
        gui_set_progress_async(1.0, "Code Executing in SDRAM!");
        gui_log_async("[+] Code successfully loaded into SDRAM and executed at 0x%08X!\n", args->target_addr);
    }

    flasher_destroy(s);

finish:
    if (payload_buf) free(payload_buf);
    if (trans) transport_destroy(trans);
    if (args->port) free(args->port);
    if (args->psi_path) free(args->psi_path);
    if (args->payload_path) free(args->payload_path);
    free(args);

    gui_op_finished_async();
    return NULL;
}

static void on_btn_start_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    if (g_app.is_running) return;

    flasher_config_t *cfg = (flasher_config_t *)calloc(1, sizeof(flasher_config_t));
    cfg->use_open_psi = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_app.check_builtin_psi));
    const char *psi_txt = gtk_entry_get_text(GTK_ENTRY(g_app.entry_psi));
    if (!cfg->use_open_psi && psi_txt && strlen(psi_txt) > 0) {
        cfg->psi_path = strdup(psi_txt);
    }
    cfg->slb_path = strdup(gtk_entry_get_text(GTK_ENTRY(g_app.entry_slb)));
    cfg->bsy_path = strdup(gtk_entry_get_text(GTK_ENTRY(g_app.entry_bsy)));
    cfg->cds_path = strdup(gtk_entry_get_text(GTK_ENTRY(g_app.entry_cds)));
    cfg->dffs_path = strdup(gtk_entry_get_text(GTK_ENTRY(g_app.entry_dffs)));
    cfg->erase_all = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_app.check_erase));

    const char *addr_txt = gtk_entry_get_text(GTK_ENTRY(g_app.entry_ram_addr));
    cfg->ram_exec_addr = (addr_txt && strlen(addr_txt) > 0) ? (uint32_t)strtoul(addr_txt, NULL, 0) : 0x60000000;

    gchar *baud_str = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(g_app.combo_baud));
    cfg->baudrate = baud_str ? (uint32_t)atoi(baud_str) : 921600;
    g_free(baud_str);

    g_app.is_running = true;
    gtk_widget_set_sensitive(g_app.btn_start, FALSE);
    gtk_widget_set_sensitive(g_app.btn_backup_nvram, FALSE);
    gtk_widget_set_sensitive(g_app.btn_ram_boot, FALSE);

    pthread_create(&g_app.worker_thread, NULL, worker_flash_thread, cfg);
    pthread_detach(g_app.worker_thread);
}

static void on_btn_backup_nvram_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    if (g_app.is_running) return;

    GtkWidget *dialog = gtk_file_chooser_dialog_new("Save NVRAM Backup (IMEI & RF Calibration)",
                                                    GTK_WINDOW(g_app.window),
                                                    GTK_FILE_CHOOSER_ACTION_SAVE,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Save", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "s5222_nvram_backup.bin");

    char *save_path = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        save_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    }
    gtk_widget_destroy(dialog);

    if (!save_path) return;

    gchar *port = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(g_app.combo_port));
    gchar *baud_str = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(g_app.combo_baud));

    nvram_worker_args_t *args = (nvram_worker_args_t *)calloc(1, sizeof(nvram_worker_args_t));
    args->port = port ? strdup(port) : strdup("usb");
    args->baudrate = baud_str ? (uint32_t)atoi(baud_str) : 921600;
    args->is_usb = (strstr(args->port, "usb") != NULL || strstr(args->port, "USB") != NULL);
    args->use_open_psi = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_app.check_builtin_psi));
    const char *psi_txt = gtk_entry_get_text(GTK_ENTRY(g_app.entry_psi));
    if (!args->use_open_psi && psi_txt && strlen(psi_txt) > 0) {
        args->psi_path = strdup(psi_txt);
    }
    args->out_path = save_path;

    g_free(port);
    g_free(baud_str);

    g_app.is_running = true;
    gtk_widget_set_sensitive(g_app.btn_start, FALSE);
    gtk_widget_set_sensitive(g_app.btn_backup_nvram, FALSE);
    gtk_widget_set_sensitive(g_app.btn_ram_boot, FALSE);

    pthread_create(&g_app.worker_thread, NULL, worker_nvram_thread, args);
    pthread_detach(g_app.worker_thread);
}

static void on_btn_ramboot_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    if (g_app.is_running) return;

    const char *payload_path = gtk_entry_get_text(GTK_ENTRY(g_app.entry_bsy));
    if (!payload_path || strlen(payload_path) == 0) {
        gui_log_async("[-] Please specify a binary or BSY payload file in the Payload/BSY field!\n");
        return;
    }

    const char *addr_txt = gtk_entry_get_text(GTK_ENTRY(g_app.entry_ram_addr));
    uint32_t target_addr = 0x60000000;
    if (addr_txt && strlen(addr_txt) > 0) {
        target_addr = (uint32_t)strtoul(addr_txt, NULL, 0);
    }

    gchar *port = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(g_app.combo_port));
    gchar *baud_str = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(g_app.combo_baud));

    ramboot_worker_args_t *args = (ramboot_worker_args_t *)calloc(1, sizeof(ramboot_worker_args_t));
    args->port = port ? strdup(port) : strdup("usb");
    args->baudrate = baud_str ? (uint32_t)atoi(baud_str) : 921600;
    args->is_usb = (strstr(args->port, "usb") != NULL || strstr(args->port, "USB") != NULL);
    args->use_open_psi = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_app.check_builtin_psi));
    const char *psi_txt = gtk_entry_get_text(GTK_ENTRY(g_app.entry_psi));
    if (!args->use_open_psi && psi_txt && strlen(psi_txt) > 0) {
        args->psi_path = strdup(psi_txt);
    }
    args->payload_path = strdup(payload_path);
    args->target_addr = target_addr;

    g_free(port);
    g_free(baud_str);

    g_app.is_running = true;
    gtk_widget_set_sensitive(g_app.btn_start, FALSE);
    gtk_widget_set_sensitive(g_app.btn_backup_nvram, FALSE);
    gtk_widget_set_sensitive(g_app.btn_ram_boot, FALSE);

    pthread_create(&g_app.worker_thread, NULL, worker_ramboot_thread, args);
    pthread_detach(g_app.worker_thread);
}

typedef struct {
    GtkWidget *entry;
    GtkWindow *parent;
    const char *title;
} browse_ctx_t;

static void on_browse_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    browse_ctx_t *ctx = (browse_ctx_t *)user_data;

    GtkWidget *dialog = gtk_file_chooser_dialog_new(ctx->title,
                                                    ctx->parent,
                                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Open", GTK_RESPONSE_ACCEPT,
                                                    NULL);

    const char *cur = gtk_entry_get_text(GTK_ENTRY(ctx->entry));
    if (cur && strlen(cur) > 0) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), cur);
    }

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            gtk_entry_set_text(GTK_ENTRY(ctx->entry), filename);
            g_free(filename);
        }
    }
    gtk_widget_destroy(dialog);
}

static void free_browse_ctx(gpointer data, GClosure *closure) {
    (void)closure;
    free(data);
}

static void add_grid_file_row(GtkGrid *grid, int row, const char *label_text, const char *placeholder,
                              GtkWidget **out_entry, GtkWidget **out_btn, GtkWindow *parent) {
    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);

    GtkWidget *entry = gtk_entry_new();
    gtk_widget_set_hexpand(entry, TRUE);
    if (placeholder) {
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    }

    GtkWidget *btn = gtk_button_new_with_label("Browse...");
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);

    browse_ctx_t *ctx = (browse_ctx_t *)malloc(sizeof(browse_ctx_t));
    ctx->entry = entry;
    ctx->parent = parent;
    ctx->title = label_text;
    g_signal_connect_data(btn, "clicked", G_CALLBACK(on_browse_clicked), ctx, free_browse_ctx, 0);

    gtk_grid_attach(grid, lbl, 0, row, 1, 1);
    gtk_grid_attach(grid, entry, 1, row, 1, 1);
    gtk_grid_attach(grid, btn, 2, row, 1, 1);

    if (out_entry) *out_entry = entry;
    if (out_btn) *out_btn = btn;
}

static void on_check_builtin_psi_toggled(GtkToggleButton *tb, gpointer user_data) {
    (void)user_data;
    gboolean builtin = gtk_toggle_button_get_active(tb);
    gtk_widget_set_sensitive(g_app.entry_psi, !builtin);
    gtk_widget_set_sensitive(g_app.btn_browse_psi, !builtin);
    if (builtin) {
        gtk_entry_set_placeholder_text(GTK_ENTRY(g_app.entry_psi), "(Using embedded Open Rust PSI)");
        gtk_entry_set_text(GTK_ENTRY(g_app.entry_psi), "");
    } else {
        gtk_entry_set_placeholder_text(GTK_ENTRY(g_app.entry_psi), "Select custom PSI (*_psi.fls or *.bin)...");
    }
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    apply_app_css();

    g_app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_app.window), "OpenIFWD - Open Flasher & RAM Boot Tool");
    gtk_window_set_default_size(GTK_WINDOW(g_app.window), 820, 680);
    gtk_container_set_border_width(GTK_CONTAINER(g_app.window), 12);
    g_signal_connect(g_app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(g_app.window), main_box);

    /* Connection Settings Frame */
    GtkWidget *frame_conn = gtk_frame_new(" Port & Connection ");
    GtkWidget *grid_conn = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid_conn), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid_conn), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid_conn), 10);
    gtk_container_add(GTK_CONTAINER(frame_conn), grid_conn);

    GtkWidget *lbl_port = gtk_label_new("Port:");
    gtk_widget_set_halign(lbl_port, GTK_ALIGN_START);
    g_app.combo_port = gtk_combo_box_text_new();
    gtk_combo_box_set_wrap_width(GTK_COMBO_BOX(g_app.combo_port), 1);
    gtk_widget_set_hexpand(g_app.combo_port, TRUE);

    GtkWidget *btn_scan = gtk_button_new_with_label("Refresh Ports");
    g_signal_connect(btn_scan, "clicked", G_CALLBACK(on_btn_scan_clicked), NULL);

    GtkWidget *lbl_baud = gtk_label_new("Baudrate:");
    gtk_widget_set_halign(lbl_baud, GTK_ALIGN_START);
    g_app.combo_baud = gtk_combo_box_text_new();
    gtk_combo_box_set_wrap_width(GTK_COMBO_BOX(g_app.combo_baud), 1);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app.combo_baud), "921600");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app.combo_baud), "460800");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app.combo_baud), "115200");
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_app.combo_baud), 0);

    gtk_grid_attach(GTK_GRID(grid_conn), lbl_port, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_conn), g_app.combo_port, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_conn), btn_scan, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_conn), lbl_baud, 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_conn), g_app.combo_baud, 4, 0, 1, 1);

    gtk_box_pack_start(GTK_BOX(main_box), frame_conn, FALSE, FALSE, 0);

    /* Firmware Files Frame */
    GtkWidget *frame_files = gtk_frame_new(" Firmware & Initializer (GT-S5222 / XMM2150) ");
    GtkWidget *grid_files = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid_files), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid_files), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid_files), 10);
    gtk_container_add(GTK_CONTAINER(frame_files), grid_files);

    GtkWindow *win = GTK_WINDOW(g_app.window);

    /* Checkbox for built-in Open Rust PSI */
    g_app.check_builtin_psi = gtk_check_button_new_with_label("Use Built-in Open Rust PSI (Default, replaces stock PSI/EBL)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_app.check_builtin_psi), TRUE);
    g_signal_connect(g_app.check_builtin_psi, "toggled", G_CALLBACK(on_check_builtin_psi_toggled), NULL);
    gtk_grid_attach(GTK_GRID(grid_files), g_app.check_builtin_psi, 0, 0, 3, 1);

    add_grid_file_row(GTK_GRID(grid_files), 1, "1. Custom PSI (*.bin/fls):", "(Using embedded Open Rust PSI)",
                      &g_app.entry_psi, &g_app.btn_browse_psi, win);
    gtk_widget_set_sensitive(g_app.entry_psi, FALSE);
    gtk_widget_set_sensitive(g_app.btn_browse_psi, FALSE);

    add_grid_file_row(GTK_GRID(grid_files), 2, "2. SLB (*_slb.fls):", "Optional if using stock EBL...", &g_app.entry_slb, NULL, win);
    add_grid_file_row(GTK_GRID(grid_files), 3, "3. BSY / Payload (*_BSY.fls or *.bin):", "Select baseband code or RAM binary...", &g_app.entry_bsy, NULL, win);
    add_grid_file_row(GTK_GRID(grid_files), 4, "4. CDS (*_CDS.fls):", "Select *_CDS.fls file...", &g_app.entry_cds, NULL, win);
    add_grid_file_row(GTK_GRID(grid_files), 5, "5. CSC (*.dffs):",    "Select *.dffs or CSC file...", &g_app.entry_dffs, NULL, win);

    gtk_box_pack_start(GTK_BOX(main_box), frame_files, FALSE, FALSE, 0);

    /* Options Frame */
    GtkWidget *frame_opts = gtk_frame_new(" Options ");
    GtkWidget *grid_opts = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid_opts), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid_opts), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid_opts), 8);
    gtk_container_add(GTK_CONTAINER(frame_opts), grid_opts);

    g_app.check_erase = gtk_check_button_new_with_label("Erase whole flash before download");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_app.check_erase), TRUE);

    g_app.check_ram_only = gtk_check_button_new_with_label("RAM Boot only (Don't flash OneNAND)");

    GtkWidget *lbl_ram_addr = gtk_label_new("Target SDRAM Address:");
    gtk_widget_set_halign(lbl_ram_addr, GTK_ALIGN_START);
    g_app.entry_ram_addr = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_app.entry_ram_addr), "0x60000000");
    gtk_entry_set_width_chars(GTK_ENTRY(g_app.entry_ram_addr), 12);

    gtk_grid_attach(GTK_GRID(grid_opts), g_app.check_erase, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_opts), g_app.check_ram_only, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_opts), lbl_ram_addr, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_opts), g_app.entry_ram_addr, 3, 0, 1, 1);

    gtk_box_pack_start(GTK_BOX(main_box), frame_opts, FALSE, FALSE, 0);

    /* Actions and Progress */
    GtkWidget *action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    g_app.btn_start = gtk_button_new_with_label(" Start Download ");
    g_signal_connect(g_app.btn_start, "clicked", G_CALLBACK(on_btn_start_clicked), NULL);

    g_app.btn_backup_nvram = gtk_button_new_with_label(" Backup NVRAM ");
    g_signal_connect(g_app.btn_backup_nvram, "clicked", G_CALLBACK(on_btn_backup_nvram_clicked), NULL);

    g_app.btn_ram_boot = gtk_button_new_with_label(" Boot RAM (Exec) ");
    g_signal_connect(g_app.btn_ram_boot, "clicked", G_CALLBACK(on_btn_ramboot_clicked), NULL);

    g_app.progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(g_app.progress_bar), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(g_app.progress_bar), "Ready");

    gtk_box_pack_start(GTK_BOX(action_box), g_app.btn_start, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(action_box), g_app.btn_backup_nvram, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(action_box), g_app.btn_ram_boot, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(action_box), g_app.progress_bar, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(main_box), action_box, FALSE, FALSE, 0);

    /* Console Log Output */
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 180);

    g_app.text_log = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_app.text_log), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(g_app.text_log), FALSE);
    g_app.log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_app.text_log));

    gtk_container_add(GTK_CONTAINER(scroll), g_app.text_log);
    gtk_box_pack_start(GTK_BOX(main_box), scroll, TRUE, TRUE, 0);

    /* Initialize available ports */
    scan_and_populate_ports(GTK_COMBO_BOX_TEXT(g_app.combo_port));

    log_to_gui("[*] OpenIFWD GUI initialized.\n");
    log_to_gui("[*] Default: Built-in Open Rust PSI enabled (NVRAM write protected, RAM boot supported).\n");
    log_to_gui("[*] Select your firmware or binary payload files to proceed.\n");

    gtk_widget_show_all(g_app.window);
    gtk_main();

    return 0;
}
