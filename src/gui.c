#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <gtk/gtk.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    GtkWidget *entry_psi;
    GtkWidget *entry_slb;
    GtkWidget *entry_bsy;
    GtkWidget *entry_cds;
    GtkWidget *entry_dffs;

    GtkWidget *check_erase;
    GtkWidget *check_ram_only;
    GtkWidget *progress_bar;
    GtkWidget *btn_start;
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
    if (cfg->psi_path && strlen(cfg->psi_path) > 0) {
        gui_log_async("[*] Uploading PSI: %s\n", cfg->psi_path);
        gui_set_progress_async(0.30, "Uploading PSI...");
        fls_file_t *psi = fls_open(cfg->psi_path);
        if (psi) {
            flasher_upload_psi(s, psi);
            fls_close(psi);
        } else {
            gui_log_async("[-] Warning: Failed to open PSI: %s\n", cfg->psi_path);
        }
    }

    if (ram_only) {
        /* RAM Boot mode */
        gui_log_async("[*] Operating in RAM-BOOT mode (no flash will be written)\n");
        if (cfg->bsy_path && strlen(cfg->bsy_path) > 0) {
            fls_file_t *bsy = fls_open(cfg->bsy_path);
            if (bsy) {
                const fls_chunk_info_t *c = fls_find_chunk(bsy, FLS_TAG_IMAGE_DATA);
                if (c) {
                    size_t sz = 0;
                    const uint8_t *data = fls_get_chunk_payload(bsy, c, &sz);
                    gui_log_async("[*] Uploading payload to SDRAM at 0x20000000 (%zu bytes)...\n", sz);
                    flasher_boot_ram(s, 0x20000000, data, sz);
                }
                fls_close(bsy);
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
    flasher_configure_protocol(s);
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
    free(cfg);

    g_app.is_running = false;
    gtk_widget_set_sensitive(g_app.btn_start, TRUE);
    return NULL;
}

static void on_btn_start_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    if (g_app.is_running) return;

    flasher_config_t *cfg = (flasher_config_t *)calloc(1, sizeof(flasher_config_t));
    cfg->psi_path = strdup(gtk_entry_get_text(GTK_ENTRY(g_app.entry_psi)));
    cfg->slb_path = strdup(gtk_entry_get_text(GTK_ENTRY(g_app.entry_slb)));
    cfg->bsy_path = strdup(gtk_entry_get_text(GTK_ENTRY(g_app.entry_bsy)));
    cfg->cds_path = strdup(gtk_entry_get_text(GTK_ENTRY(g_app.entry_cds)));
    cfg->dffs_path = strdup(gtk_entry_get_text(GTK_ENTRY(g_app.entry_dffs)));
    cfg->erase_all = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_app.check_erase));

    gchar *baud_str = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(g_app.combo_baud));
    cfg->baudrate = baud_str ? (uint32_t)atoi(baud_str) : 921600;
    g_free(baud_str);

    g_app.is_running = true;
    gtk_widget_set_sensitive(g_app.btn_start, FALSE);

    pthread_create(&g_app.worker_thread, NULL, worker_flash_thread, cfg);
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

static void add_grid_file_row(GtkGrid *grid, int row, const char *label_text, const char *placeholder, GtkWidget **out_entry, GtkWindow *parent) {
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

    *out_entry = entry;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    apply_app_css();

    g_app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_app.window), "OpenIFWD - Infineon Firmware Downloader & Flasher");
    gtk_window_set_default_size(GTK_WINDOW(g_app.window), 780, 640);
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

    /* Firmware Files Frame (Using GtkGrid for proper geometry) */
    GtkWidget *frame_files = gtk_frame_new(" Firmware Files (GT-S5222) ");
    GtkWidget *grid_files = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid_files), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid_files), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid_files), 10);
    gtk_container_add(GTK_CONTAINER(frame_files), grid_files);

    GtkWindow *win = GTK_WINDOW(g_app.window);
    add_grid_file_row(GTK_GRID(grid_files), 0, "1. PSI (*_psi.fls):", "Select *_psi.fls file...", &g_app.entry_psi, win);
    add_grid_file_row(GTK_GRID(grid_files), 1, "2. SLB (*_slb.fls):", "Select *_slb.fls file...", &g_app.entry_slb, win);
    add_grid_file_row(GTK_GRID(grid_files), 2, "3. BSY (*_BSY.fls):", "Select *_BSY.fls file...", &g_app.entry_bsy, win);
    add_grid_file_row(GTK_GRID(grid_files), 3, "4. CDS (*_CDS.fls):", "Select *_CDS.fls file...", &g_app.entry_cds, win);
    add_grid_file_row(GTK_GRID(grid_files), 4, "5. CSC (*.dffs):",    "Select *.dffs or CSC file...", &g_app.entry_dffs, win);

    gtk_box_pack_start(GTK_BOX(main_box), frame_files, FALSE, FALSE, 0);

    /* Options Frame */
    GtkWidget *frame_opts = gtk_frame_new(" Options ");
    GtkWidget *box_opts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_container_set_border_width(GTK_CONTAINER(box_opts), 8);
    gtk_container_add(GTK_CONTAINER(frame_opts), box_opts);

    g_app.check_erase = gtk_check_button_new_with_label("Erase whole flash before download");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_app.check_erase), TRUE);

    g_app.check_ram_only = gtk_check_button_new_with_label("RAM Boot only (Execute in SDRAM, don't write flash)");

    gtk_box_pack_start(GTK_BOX(box_opts), g_app.check_erase, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box_opts), g_app.check_ram_only, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(main_box), frame_opts, FALSE, FALSE, 0);

    /* Actions and Progress */
    GtkWidget *action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    g_app.btn_start = gtk_button_new_with_label(" Start Download ");
    g_signal_connect(g_app.btn_start, "clicked", G_CALLBACK(on_btn_start_clicked), NULL);

    g_app.progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(g_app.progress_bar), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(g_app.progress_bar), "Ready");

    gtk_box_pack_start(GTK_BOX(action_box), g_app.btn_start, FALSE, FALSE, 0);
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
    log_to_gui("[*] Select your firmware files via Browse... buttons or enter paths manually.\n");

    gtk_widget_show_all(g_app.window);
    gtk_main();

    return 0;
}
