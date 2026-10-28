#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "flasher.h"
#include "open_psi.h"
#include "romdumper_embedded.h"
#include "ram_dumper_embedded.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <strings.h>
#include <dirent.h>
#include <ctype.h>

#define CHUNK_WRITE_SIZE 2048

typedef struct {
    const char *sha256;
    size_t size;
    const char *description;
} trusted_psi_t;

static const trusted_psi_t k_trusted_psi[] = {
    {
        .sha256 = "652d144ec1466a776dd8c55e34ed4c3faa0fa2adbb52ecbeaab5acad66d1539a",
        .size = 57644,
        .description = "Samsung GT-S5222 Stock PSI (XXMJ1 / XXLAF / XXLC3 / XXLL1)"
    },
    { NULL, 0, NULL }
};

int flasher_verify_psi_sha256(const uint8_t *psi_data, size_t psi_len) {
    if (!psi_data || psi_len == 0) return -1;

    uint8_t hash[32];
    sha256_calc(psi_data, psi_len, hash);
    char hex[65];
    sha256_to_hex(hash, hex);

    for (size_t i = 0; k_trusted_psi[i].sha256 != NULL; i++) {
        if (strcasecmp(hex, k_trusted_psi[i].sha256) == 0 && psi_len == k_trusted_psi[i].size) {
            printf("[+] PSI SHA-256 Verified: %s\n    Identified: %s\n", hex, k_trusted_psi[i].description);
            return 0;
        }
    }

    fprintf(stderr, "[!] Warning: PSI SHA-256 (%s, %zu bytes) not found in trusted list!\n", hex, psi_len);
    return 1;
}

static int check_psi_integrity(flasher_session_t *s, const uint8_t *psi_data, size_t psi_size, uint8_t *allocated_psi) {
    if (!psi_data || psi_size == 0) {
        if (allocated_psi) free(allocated_psi);
        return -1;
    }

    if (s && s->config.skip_psi_sha256) {
        uint8_t hash[32];
        char hex[65];
        sha256_calc(psi_data, psi_size, hash);
        sha256_to_hex(hash, hex);
        printf("[*] Skipping PSI SHA-256 integrity check (hash: %s, --skip-psi-sha256 enabled)\n", hex);
        return 0;
    }

    if (flasher_verify_psi_sha256(psi_data, psi_size) != 0) {
        fprintf(stderr, "[-] ERROR: Untrusted or unknown PSI binary!\n");
        fprintf(stderr, "[-] To proceed anyway with this custom PSI, pass --skip-psi-sha256 (or --skip-psi-check).\n");
        if (allocated_psi) free(allocated_psi);
        return -1;
    }
    return 0;
}

flasher_session_t *flasher_create(transport_t *transport, const flasher_config_t *cfg) {
    if (!transport || !cfg) return NULL;

    flasher_session_t *s = (flasher_session_t *)calloc(1, sizeof(flasher_session_t));
    if (!s) return NULL;

    s->transport = transport;
    s->config = *cfg;
    s->channel = IFWD_DEFAULT_CHANNEL;
    return s;
}

void flasher_destroy(flasher_session_t *session) {
    free(session);
}

int flasher_sync(flasher_session_t *s, int timeout_sec) {
    if (!s) return -1;
    bool is_usb = s->config.is_usb;

    printf("[*] Probing Boot ROM synchronization (Ping-Pong ATAT sync)...\n");
    printf("[*] Please connect phone (hold Volume Down or plug cable without battery, then insert battery).\n");

    if (!is_usb) {
        /* Set DTR/RTS once before sync loop for UART */
        transport_set_control(s->transport, true, true);
    }

    time_t start = time(NULL);
    const uint8_t sync_seq[] = "ATAT";
    uint8_t ident_stream[128];
    int ident_len = 0;
    bool synced = false;

    while (time(NULL) - start < timeout_sec) {
        /* Send strictly ONE ATAT probe (4 bytes) per attempt */
        transport_write(s->transport, sync_seq, 4);

        /* Give Boot ROM time to respond and check Bulk IN */
        uint8_t rx_buf[64];
        int n = transport_read(s->transport, rx_buf, sizeof(rx_buf), 50);
        if (n > 0) {
            printf("[*] Target response: %d byte(s): ", n);
            for (int i = 0; i < n; i++) {
                printf("0x%02X ", rx_buf[i]);
            }
            printf("\n");

            /* Store received data */
            if (ident_len + n < (int)sizeof(ident_stream)) {
                memcpy(ident_stream + ident_len, rx_buf, (size_t)n);
                ident_len += n;
            }

            /* Look for synchronization tokens */
            for (int i = 0; i < n; i++) {
                uint8_t b = rx_buf[i];
                if (b == 0x01 || b == 0x02 || b == 0x0A || b == 0xF0 || b == 0x17) {
                    printf("[+] Received Boot ROM sync token 0x%02X! Halting sync probe.\n", b);
                    synced = true;
                    break;
                }
            }
            if (synced) break;
        }

        /* Brief delay between Ping-Pong probes */
        usleep(30000);
    }

    if (!synced) {
        fprintf(stderr, "[-] Synchronization timed out (%d seconds). Check connections.\n", timeout_sec);
        return -1;
    }

    /* Drain chip ID bytes from Boot ROM */
    uint8_t drain[16];
    int dn = transport_read(s->transport, drain, sizeof(drain), 100);
    if (dn > 0) {
        for (int i = 0; i < dn; i++) {
            printf("[*] Drained 1 byte(s) of chip ID data: 0x%02X\n", drain[i]);
        }
    }

    /* Clean leftover FIFO before PSI upload */
    usleep(20000);
    transport_flush(s->transport);
    printf("[+] Device synchronized successfully!\n");
    return 0;
}

static uint8_t *load_custom_psi(const char *psi_path, size_t *out_size, uint8_t *out_chk) {
    if (!psi_path || strlen(psi_path) == 0) return NULL;

    uint8_t *allocated_psi = NULL;
    size_t psi_size = 0;

    if (strstr(psi_path, ".fls") || strstr(psi_path, ".FLS")) {
        fls_file_t *pf = fls_open(psi_path);
        if (pf) {
            const fls_chunk_info_t *chunk = fls_find_chunk(pf, FLS_TAG_RPSI);
            if (!chunk) chunk = fls_find_chunk(pf, FLS_TAG_IMAGE_DATA);
            size_t raw_size = 0;
            const uint8_t *raw_data = NULL;
            if (chunk) {
                raw_data = fls_get_image_data(pf, chunk, &raw_size);
                if (raw_data && raw_size > 0 && raw_size <= 0xFFFF) {
                    allocated_psi = (uint8_t *)malloc(raw_size);
                    if (allocated_psi) {
                        memcpy(allocated_psi, raw_data, raw_size);
                        psi_size = raw_size;
                    }
                }
            }
            fls_close(pf);
        }
    }

    if (!allocated_psi) {
        FILE *raw_psi_f = fopen(psi_path, "rb");
        if (!raw_psi_f && psi_path[0] != '/') {
            char alt_path[1024];
            snprintf(alt_path, sizeof(alt_path), "../%s", psi_path);
            raw_psi_f = fopen(alt_path, "rb");
            if (raw_psi_f) {
                printf("[*] Note: Resolved relative PSI path to '%s'\n", alt_path);
            }
        }
        if (raw_psi_f) {
            fseek(raw_psi_f, 0, SEEK_END);
            long sz = ftell(raw_psi_f);
            rewind(raw_psi_f);
            if (sz > 0 && sz <= 0xFFFF) {
                allocated_psi = (uint8_t *)malloc((size_t)sz);
                if (allocated_psi) {
                    if (fread(allocated_psi, 1, (size_t)sz, raw_psi_f) == (size_t)sz) {
                        psi_size = (size_t)sz;
                    } else {
                        free(allocated_psi);
                        allocated_psi = NULL;
                    }
                }
            } else {
                fprintf(stderr, "[-] Error: PSI file size %ld is invalid (expected 1..65535 bytes)\n", sz);
            }
            fclose(raw_psi_f);
        } else {
            fprintf(stderr, "[-] Error: Failed to open raw PSI file '%s': %s\n", psi_path, strerror(errno));
        }
    }

    if (allocated_psi && psi_size > 0) {
        uint8_t chk8 = 0;
        for (size_t i = 0; i < psi_size; i++) {
            chk8 ^= allocated_psi[i];
        }
        *out_size = psi_size;
        *out_chk = chk8;
        printf("[*] Loaded custom PSI binary from %s (%zu bytes, XOR-8 0x%02X)...\n",
               psi_path, psi_size, chk8);
        return allocated_psi;
    }

    return NULL;
}

static char *find_psi_in_dir(const char *reference_path) {
    if (!reference_path) return NULL;

    char dir_buf[1024];
    strncpy(dir_buf, reference_path, sizeof(dir_buf) - 1);
    dir_buf[sizeof(dir_buf) - 1] = '\0';

    char *slash = strrchr(dir_buf, '/');
    if (slash) {
        *slash = '\0';
    } else {
        strcpy(dir_buf, ".");
    }

    DIR *d = opendir(dir_buf);
    if (!d) return NULL;

    struct dirent *ent;
    char found_path[1024] = {0};

    while ((ent = readdir(d)) != NULL) {
        char lower_name[256];
        size_t len = strlen(ent->d_name);
        if (len >= sizeof(lower_name)) continue;
        for (size_t i = 0; i <= len; i++) {
            lower_name[i] = (char)tolower((unsigned char)ent->d_name[i]);
        }

        if (strstr(lower_name, "psi") &&
            (strstr(lower_name, ".fls") || strstr(lower_name, ".bin"))) {
            snprintf(found_path, sizeof(found_path), "%s/%s", dir_buf, ent->d_name);
            break;
        }
    }
    closedir(d);

    if (found_path[0]) {
        return strdup(found_path);
    }
    return NULL;
}

static uint8_t *resolve_and_load_psi(flasher_session_t *s, size_t *out_size, uint8_t *out_chk) {
    if (!s || !out_size || !out_chk) return NULL;

    /* 1. Explicitly provided PSI path */
    if (s->config.psi_path && s->config.psi_path[0]) {
        printf("[*] Flasher: Loading user-specified PSI: %s\n", s->config.psi_path);
        return load_custom_psi(s->config.psi_path, out_size, out_chk);
    }

    /* 2. Try auto-detecting in the directory of specified firmware file */
    const char *ref_paths[] = {
        s->config.fls_path,
        s->config.bsy_path,
        s->config.code_path,
        s->config.cds_path,
        s->config.dffs_path,
        s->config.csc_path,
        NULL
    };

    char *detected = NULL;
    for (int i = 0; ref_paths[i] != NULL; i++) {
        if (ref_paths[i] && ref_paths[i][0]) {
            detected = find_psi_in_dir(ref_paths[i]);
            if (detected) break;
        }
    }

    /* Also check current working directory '.' */
    if (!detected) {
        detected = find_psi_in_dir(".");
    }

    if (detected) {
        printf("[*] Auto-detected vendor PSI container in firmware directory: %s\n", detected);
        uint8_t *data = load_custom_psi(detected, out_size, out_chk);
        free(detected);
        if (data) return data;
    }

    /* 3. Not found: Explain clean-room requirement to user */
    fprintf(stderr, "\n[-] ====================================================================\n");
    fprintf(stderr, "[-] ERROR: PSI Stage 1 bootloader not specified!\n");
    fprintf(stderr, "[-] To comply with clean-room open-source licensing and copyright laws,\n");
    fprintf(stderr, "[-] openIFWD does not redistribute proprietary Samsung/Infineon binary blobs.\n");
    fprintf(stderr, "[-] Please specify the vendor PSI container from your official firmware:\n");
    fprintf(stderr, "[-]     --psi <path/to/S5222XXMJ1_psi.fls>  (or raw psi_ram.bin)\n");
    fprintf(stderr, "[-] openIFWD will dynamically unpack the PSI, verify SHA-256 integrity,\n");
    fprintf(stderr, "[-] and substitute EBL with the open-source open_flasher agent.\n");
    fprintf(stderr, "[-] ====================================================================\n\n");
    return NULL;
}

int flasher_upload_psi(flasher_session_t *s, const fls_file_t *psi_fls) {
    if (!s) return -1;

    const uint8_t *psi_data = NULL;
    size_t psi_size = 0;
    uint8_t chk8 = 0;

    /* 1. Extract PSI from FLS container if provided */
    if (psi_fls) {
        const fls_chunk_info_t *chunk = fls_find_chunk(psi_fls, FLS_TAG_RPSI);
        size_t raw_size = 0;
        const uint8_t *raw_data = NULL;

        if (chunk) {
            raw_data = fls_get_image_data(psi_fls, chunk, &raw_size);
            if (raw_data && raw_size > 0 && raw_size <= 0xFFFF) {
                psi_data = raw_data;
                psi_size = raw_size;
            }
        }

        if (psi_data && psi_size > 0 && psi_size <= 0xFFFF) {
            printf("[*] Uploading Primary Stage Initializer from %s (Tag %u: %zu bytes)...\n",
                   psi_fls->filepath, chunk ? chunk->tag : 0, psi_size);
            for (size_t i = 0; i < psi_size; i++) {
                chk8 ^= psi_data[i];
            }
        }
    }

    uint8_t *allocated_psi = NULL;
    if (!psi_data) {
        allocated_psi = resolve_and_load_psi(s, &psi_size, &chk8);
        if (!allocated_psi) {
            return -1;
        }
        psi_data = allocated_psi;
    }

    /* Verify SHA-256 against trusted table */
    if (check_psi_integrity(s, psi_data, psi_size, allocated_psi) != 0) {
        return -1;
    }

    /* Boot ROM Header: Command 0x30, length uint16_le */
    uint8_t boot_hdr[3];
    boot_hdr[0] = 0x30;
    boot_hdr[1] = (uint8_t)(psi_size & 0xFF);
    boot_hdr[2] = (uint8_t)((psi_size >> 8) & 0xFF);

    if (transport_write(s->transport, boot_hdr, 3) != 3) {
        fprintf(stderr, "[-] Failed to write Boot ROM 0x30 header\n");
        if (allocated_psi) free(allocated_psi);
        return -1;
    }

    /* Stream PSI binary */
    printf("[*] Streaming %zu bytes of PSI binary to internal SRAM at 0x00080000...\n", psi_size);
    if (transport_write(s->transport, psi_data, psi_size) != (int)psi_size) {
        fprintf(stderr, "[-] Failed to stream PSI binary\n");
        if (allocated_psi) free(allocated_psi);
        return -1;
    }

    /* Send XOR-8 Checksum */
    printf("[*] Sending XOR-8 checksum: 0x%02X\n", chk8);
    if (transport_write(s->transport, &chk8, 1) != 1) {
        fprintf(stderr, "[-] Failed to send PSI checksum\n");
        if (allocated_psi) free(allocated_psi);
        return -1;
    }

    /* Read Boot ROM ACK (Boot ROM sends 0x01 checksum ACK, then 0x01 execution handover ACK) */
    uint8_t ack_buf[16] = {0};
    int r = transport_read(s->transport, ack_buf, 1, 2000);
    if (r <= 0) {
        r = transport_read(s->transport, ack_buf, 1, 2000);
    }

    if (r > 0 && (ack_buf[0] == 0x01 || ack_buf[0] == 0x0B || ack_buf[0] == 0xB1 || ack_buf[0] == 0xC1)) {
        printf("[+] PSI acknowledged by Boot ROM (0x%02X)\n", ack_buf[0]);

        /* Read 2nd Boot ROM execution ACK matching Frame 82 in trace */
        uint8_t ack2[4] = {0};
        int r2 = transport_read(s->transport, ack2, 1, 500);
        if (r2 > 0) {
            printf("[+] PSI execution handover confirmed by Boot ROM (0x%02X)\n", ack2[0]);
        }
    } else {
        fprintf(stderr, "[-] Unexpected Boot ROM ACK (%d bytes, 0x%02X)\n", r, ack_buf[0]);
        if (allocated_psi) free(allocated_psi);
        return -1;
    }

    if (allocated_psi) free(allocated_psi);

    /* Do NOT call transport_flush() / USBDEVFS_RESETEP.
     * Brief delay for SRAM execution startup matching vendor trace */
    usleep(200000);
    return 0;
}

/* ============================================================================
 * OpenIFWD Wire Protocol Engine
 * ============================================================================ */

static int openifwd_send_recv(flasher_session_t *s,
                              uint8_t cmd,
                              const uint8_t *payload,
                              uint16_t payload_len,
                              uint8_t expected_rsp,
                              void *out_buf,
                              uint16_t *out_len,
                              int timeout_ms) {
    if (!s || !s->transport) return -1;

    uint16_t seq = ++s->openifwd_seq;
    uint8_t tx_buf[4096];
    size_t tx_len = openifwd_build_packet(cmd, seq, payload, payload_len, tx_buf, sizeof(tx_buf));
    if (tx_len == 0) {
        return -2;
    }

    if (transport_write(s->transport, tx_buf, tx_len) != (int)tx_len) {
        return -3;
    }

    /* Wait for response header (11 bytes: OFWD, ver, cmd, seq[2], len[2], chk) */
    uint8_t rx_hdr_buf[11];
    size_t hdr_recvd = 0;
    time_t start = time(NULL);

    while (hdr_recvd < 11) {
        int n = transport_read(s->transport, rx_hdr_buf + hdr_recvd, 11 - hdr_recvd, 50);
        if (n > 0) {
            hdr_recvd += (size_t)n;
        } else if (timeout_ms > 0 && (time(NULL) - start) * 1000 > timeout_ms) {
            return -3; /* Timeout */
        }
    }

    openifwd_pkt_hdr_t rsp_hdr;
    if (openifwd_parse_header(rx_hdr_buf, 11, &rsp_hdr) != 0) {
        return -4; /* Header parse error */
    }

    if (rsp_hdr.cmd != expected_rsp) {
        return (int)rsp_hdr.cmd; /* Return error response code */
    }

    /* Read payload if any */
    if (rsp_hdr.length > 0) {
        uint8_t rx_payload[4096];
        if (rsp_hdr.length > sizeof(rx_payload)) return -5;

        size_t p_recvd = 0;
        while (p_recvd < rsp_hdr.length) {
            int n = transport_read(s->transport, rx_payload + p_recvd, rsp_hdr.length - p_recvd, 50);
            if (n > 0) {
                p_recvd += (size_t)n;
            } else if (timeout_ms > 0 && (time(NULL) - start) * 1000 > timeout_ms) {
                return -3;
            }
        }

        /* Read trailing 2-byte CRC-16 */
        uint8_t crc_buf[2];
        size_t c_recvd = 0;
        while (c_recvd < 2) {
            int n = transport_read(s->transport, crc_buf + c_recvd, 2 - c_recvd, 50);
            if (n > 0) {
                c_recvd += (size_t)n;
            } else if (timeout_ms > 0 && (time(NULL) - start) * 1000 > timeout_ms) {
                return -3;
            }
        }

        uint16_t expected_crc = (uint16_t)crc_buf[0] | ((uint16_t)crc_buf[1] << 8);
        if (openifwd_calc_crc16(rx_payload, rsp_hdr.length) != expected_crc) {
            return -6; /* CRC mismatch */
        }

        if (out_buf && out_len) {
            uint16_t copy_len = (*out_len < rsp_hdr.length) ? *out_len : rsp_hdr.length;
            memcpy(out_buf, rx_payload, copy_len);
            *out_len = copy_len;
        }
    } else {
        if (out_len) *out_len = 0;
    }

    return 0;
}

int flasher_open_ping(flasher_session_t *s) {
    uint32_t status_flags = 0;
    uint16_t resp_len = sizeof(status_flags);

    for (int attempt = 1; attempt <= 5; attempt++) {
        if (s->config.verbose) {
            printf("[*] Probing OpenIFWD agent ping (attempt %d/5)...\n", attempt);
        }
        int r = openifwd_send_recv(s, OPENIFWD_CMD_PING, NULL, 0,
                                   OPENIFWD_RSP_PONG, &status_flags, &resp_len, 500);
        if (r == 0) {
            return 0;
        }
        usleep(40000); /* 40ms between attempts */
    }
    return -1;
}

int flasher_open_get_hw_info(flasher_session_t *s, openifwd_hw_info_t *out_info) {
    if (!out_info) return -1;
    uint16_t resp_len = sizeof(openifwd_hw_info_t);
    int res = openifwd_send_recv(s, OPENIFWD_CMD_GET_HW_INFO, NULL, 0,
                                 OPENIFWD_RSP_HW_INFO, out_info, &resp_len, 3000);
    if (res == 0 && s) {
        s->hw_info = *out_info;
    }
    return res;
}

int flasher_open_erase_block(flasher_session_t *s, uint32_t block) {
    if (block >= ONENAND_PROTECTED_BLOCK_START) {
        if (!s || !s->config.force_dangerous_erase) {
            fprintf(stderr, "[-] ERROR: Block %u is in protected calibration/metadata area (978..1023)!\n", block);
            fprintf(stderr, "[-] Operation aborted to prevent permanent loss of RF calibration & IMEI.\n");
            return -EPERM;
        }
    }
    uint8_t payload[4];
    memcpy(payload, &block, sizeof(block));
    return openifwd_send_recv(s, OPENIFWD_CMD_ERASE_BLOCK, payload, sizeof(payload),
                              OPENIFWD_RSP_OK, NULL, NULL, 5000);
}

int flasher_open_write_page(flasher_session_t *s, uint32_t block, uint16_t page, const uint8_t *data, const uint8_t *spare) {
    if (!data) return -1;
    if (block >= ONENAND_PROTECTED_BLOCK_START) {
        if (!s || !s->config.force_dangerous_erase) {
            fprintf(stderr, "[-] ERROR: Block %u is in protected calibration/metadata area (978..1023)!\n", block);
            fprintf(stderr, "[-] Operation aborted to prevent permanent loss of RF calibration & IMEI.\n");
            return -EPERM;
        }
    }
    uint8_t payload[8 + 2048 + 64];
    uint16_t flags = spare ? 1 : 0;
    memcpy(payload + 0, &block, sizeof(block));
    memcpy(payload + 4, &page, sizeof(page));
    memcpy(payload + 6, &flags, sizeof(flags));
    memcpy(payload + 8, data, 2048);
    uint16_t total_len = 8 + 2048;

    if (spare) {
        memcpy(payload + 8 + 2048, spare, 64);
        total_len += 64;
    }

    return openifwd_send_recv(s, OPENIFWD_CMD_WRITE_PAGE, payload, total_len,
                              OPENIFWD_RSP_OK, NULL, NULL, 5000);
}

int flasher_open_read_page(flasher_session_t *s, uint32_t block, uint16_t page, uint8_t *out_data, uint8_t *out_spare) {
    if (!out_data) return -1;
    uint8_t req[8];
    uint16_t flags = out_spare ? 1 : 0;
    memcpy(req + 0, &block, sizeof(block));
    memcpy(req + 4, &page, sizeof(page));
    memcpy(req + 6, &flags, sizeof(flags));

    uint8_t resp_buf[2048 + 64];
    uint16_t resp_len = sizeof(resp_buf);

    int res = openifwd_send_recv(s, OPENIFWD_CMD_READ_PAGE, req, sizeof(req),
                                 OPENIFWD_RSP_PAGE_DATA, resp_buf, &resp_len, 5000);
    if (res != 0) return res;

    if (resp_len >= 2048) {
        memcpy(out_data, resp_buf, 2048);
    }
    if (out_spare && resp_len >= 2048 + 64) {
        memcpy(out_spare, resp_buf + 2048, 64);
    }
    return 0;
}

int flasher_open_mem_write(flasher_session_t *s, uint32_t addr, const uint8_t *data, size_t len) {
    if (!data || len == 0 || len > 2048) return -1;
    uint8_t payload[4 + 2048];
    memcpy(payload, &addr, sizeof(addr));
    memcpy(payload + 4, data, len);

    return openifwd_send_recv(s, OPENIFWD_CMD_MEM_WRITE, payload, (uint16_t)(4 + len),
                              OPENIFWD_RSP_OK, NULL, NULL, 3000);
}

int flasher_open_exec_ram(flasher_session_t *s, uint32_t entry_addr) {
    uint8_t payload[4];
    memcpy(payload, &entry_addr, sizeof(entry_addr));
    return openifwd_send_recv(s, OPENIFWD_CMD_EXEC_RAM, payload, sizeof(payload),
                              OPENIFWD_RSP_OK, NULL, NULL, 3000);
}

int flasher_open_reboot(flasher_session_t *s) {
    return openifwd_send_recv(s, OPENIFWD_CMD_REBOOT, NULL, 0,
                              OPENIFWD_RSP_OK, NULL, NULL, 1000);
}

int flasher_connect_agent(flasher_session_t *s) {
    if (!s || !s->transport) return -1;

    printf("[*] Connecting to OpenIFWD agent on device...\n");

    /* 1. In direct USB mode, wait for Open PSI readiness token (0xCC 0xCC) on Bulk IN (EP 0x83).
     * This inverts the handshake to match vendor psi_ram.bin sequence (0x81bc8),
     * ensuring the device has initialized DWC2 DMA and armed EP 0x02 before host sends OUT. */
    if (s->config.is_usb) {
        printf("[*] Waiting for Open PSI handshake token on EP 0x83...\n");
        uint8_t tok[4] = {0};
        int tn = transport_read(s->transport, tok, 2, 2500);
        if (tn >= 2 && ((tok[0] == 0xCC && tok[1] == 0xCC) || (tok[0] == 0x00 && tok[1] == 0xAA))) {
            printf("[+] Received Open PSI readiness token (0x%02X 0x%02X) on EP 0x83!\n", tok[0], tok[1]);
        } else if (tn > 0) {
            printf("[*] Received %d byte(s) on EP 0x83 (0x%02X), proceeding...\n", tn, tok[0]);
        } else {
            printf("[*] No early token on EP 0x83 (timeout), probing ping...\n");
        }
    } else {
        usleep(30000);
    }

    /* 2. Probe OpenIFWD agent ping */
    if (flasher_open_ping(s) != 0) {
        fprintf(stderr, "[-] Target agent ping failed\n");
        return -1;
    }
    printf("[+] Target agent connected and ready!\n");
    return 0;
}

int flasher_start_ram_dumper(flasher_session_t *s) {
    if (!s || !s->transport) return -1;

    /* 1. Synchronize with Boot ROM */
    if (flasher_sync(s, 40) != 0) return -1;

    usleep(20000);
    transport_flush(s->transport);
    printf("[+] Device synchronized with Boot ROM!\n");

    /* 2. Upload PSI (Stage 1) */
    uint8_t *allocated_psi = NULL;
    const uint8_t *psi_data = NULL;
    size_t psi_size = 0;
    uint8_t psi_chk = 0;

    allocated_psi = resolve_and_load_psi(s, &psi_size, &psi_chk);
    if (!allocated_psi) {
        return -1;
    }
    psi_data = allocated_psi;
    printf("[*] Preparing verified PSI Stage 1 (%zu bytes, XOR-8 0x%02X)...\n", psi_size, psi_chk);

    /* Verify SHA-256 against trusted table */
    if (check_psi_integrity(s, psi_data, psi_size, allocated_psi) != 0) {
        return -1;
    }

    uint8_t boot_hdr[3];
    boot_hdr[0] = 0x30;
    boot_hdr[1] = (uint8_t)(psi_size & 0xFF);
    boot_hdr[2] = (uint8_t)((psi_size >> 8) & 0xFF);

    if (transport_write(s->transport, boot_hdr, 3) != 3) {
        fprintf(stderr, "[-] Failed to write Boot ROM 0x30 header\n");
        if (allocated_psi) free(allocated_psi);
        return -1;
    }

    printf("[*] Streaming %zu bytes of PSI binary to SRAM at 0x00080000...\n", psi_size);
    if (transport_write(s->transport, psi_data, psi_size) != (int)psi_size) {
        fprintf(stderr, "[-] Failed to stream PSI binary\n");
        if (allocated_psi) free(allocated_psi);
        return -1;
    }

    printf("[*] Sending XOR-8 checksum: 0x%02X\n", psi_chk);
    if (transport_write(s->transport, &psi_chk, 1) != 1) {
        fprintf(stderr, "[-] Failed to send PSI checksum\n");
        if (allocated_psi) free(allocated_psi);
        return -1;
    }

    uint8_t ack_buf[16] = {0};
    int r = transport_read(s->transport, ack_buf, 1, 2000);
    if (r <= 0) r = transport_read(s->transport, ack_buf, 1, 2000);

    if (r > 0 && ack_buf[0] == 0x01) {
        printf("[+] PSI acknowledged by Boot ROM (0x01)\n");
        uint8_t ack2[4] = {0};
        int r2 = transport_read(s->transport, ack2, 1, 1000);
        if (r2 > 0 && ack2[0] == 0x01) {
            printf("[+] PSI execution handover confirmed by Boot ROM (0x01)\n");
        }
        usleep(50000);
    } else {
        fprintf(stderr, "[-] Unexpected Boot ROM ACK (%d bytes, 0x%02X)\n", r, ack_buf[0]);
        if (allocated_psi) free(allocated_psi);
        return -1;
    }

    if (allocated_psi) {
        free(allocated_psi);
        allocated_psi = NULL;
    }

    /* 3. Wait for Stock PSI Readiness Token (0x00 0xAA) on EP 0x83 */
    printf("[*] Waiting for Stock PSI readiness token on EP 0x83...\n");
    uint8_t psi_tok[4] = {0};
    int tn = transport_read(s->transport, psi_tok, 2, 3000);
    if (tn == 2 && psi_tok[0] == 0x00 && psi_tok[1] == 0xAA) {
        printf("[+] Stock PSI ready! Received token (0x00 0xAA) - SDRAM & USB active.\n");
    } else {
        fprintf(stderr, "[-] Failed waiting for PSI readiness token (got %d bytes: 0x%02X 0x%02X)\n",
                tn, psi_tok[0], psi_tok[1]);
        return -1;
    }

    /* 4. Send RAM Dumper (Stage 2) Size (4 bytes uint32_le) on EP 0x02 */
    uint32_t dumper_size = (uint32_t)ram_dumper_size;
    uint8_t sz_buf[4];
    sz_buf[0] = (uint8_t)(dumper_size & 0xFF);
    sz_buf[1] = (uint8_t)((dumper_size >> 8) & 0xFF);
    sz_buf[2] = (uint8_t)((dumper_size >> 16) & 0xFF);
    sz_buf[3] = (uint8_t)((dumper_size >> 24) & 0xFF);

    printf("[*] Sending RAM Dumper size (%u bytes) to PSI on EP 0x02...\n", dumper_size);
    if (transport_write(s->transport, sz_buf, 4) != 4) {
        fprintf(stderr, "[-] Failed to write RAM Dumper size\n");
        return -1;
    }

    /* 5. Read Stock PSI ACK (0xCC 0xCC) on EP 0x83 */
    uint8_t sz_ack[4] = {0};
    int an = transport_read(s->transport, sz_ack, 2, 2000);
    if (an == 2 && sz_ack[0] == 0xCC && sz_ack[1] == 0xCC) {
        printf("[+] Stock PSI acknowledged upload request (0xCC 0xCC)\n");
    } else {
        fprintf(stderr, "[-] PSI upload size rejected (got %d bytes: 0x%02X 0x%02X)\n",
                an, sz_ack[0], sz_ack[1]);
        return -1;
    }

    /* 6. Stream RAM Dumper payload + 1-byte checksum in a single transfer to SDRAM at 0x60000000 on EP 0x02 */
    size_t total_xfer = ram_dumper_size + 1;
    uint8_t *xfer_buf = (uint8_t *)malloc(total_xfer);
    if (!xfer_buf) {
        fprintf(stderr, "[-] Out of memory preparing RAM Dumper transfer\n");
        return -1;
    }
    memcpy(xfer_buf, ram_dumper_binary, ram_dumper_size);
    xfer_buf[ram_dumper_size] = ram_dumper_checksum;

    printf("[*] Streaming %zu bytes (payload: %zu B, chk: 0x%02X) to SDRAM at 0x60000000...\n",
           total_xfer, ram_dumper_size, ram_dumper_checksum);
    if (transport_write(s->transport, xfer_buf, total_xfer) != (int)total_xfer) {
        fprintf(stderr, "[-] Failed to stream RAM Dumper payload\n");
        free(xfer_buf);
        return -1;
    }
    free(xfer_buf);

    /* 8. Read PSI Handover ACK on EP 0x83 */
    uint8_t chk_ack[4] = {0};
    int cn = transport_read(s->transport, chk_ack, 2, 2000);
    if (cn >= 1 && (chk_ack[1] == 0xA5 || chk_ack[0] == 0xA5 || chk_ack[0] == 0x01)) {
        printf("[+] Stock PSI verified RAM Dumper (ACK 0x%02X 0x%02X). Handing over execution to 0x60000400!\n",
               chk_ack[0], chk_ack[1]);
    } else {
        printf("[*] PSI ACK response: %d bytes (0x%02X 0x%02X)\n", cn, chk_ack[0], chk_ack[1]);
    }

    /* 9. Read RAM Dumper Hello Beacon ("RDMP") on EP 0x83 */
    printf("[*] Waiting for RAM Dumper agent beacon on EP 0x83...\n");
    uint8_t hello_buf[16] = {0};
    int hn = transport_read(s->transport, hello_buf, 16, 5000);
    if (hn >= 4) {
        uint32_t magic = *(uint32_t *)(hello_buf + 0);
        if (magic == 0x504D4452) { /* "RDMP" */
            uint32_t ver = *(uint32_t *)(hello_buf + 4);
            uint32_t chip = *(uint32_t *)(hello_buf + 8);
            printf("[+] Connected to RAM Dumper Agent! Magic: 'RDMP', Version: %u, ChipID: 0x%04X\n", ver, chip);
            return 0;
        }
    }

    fprintf(stderr, "[-] Did not receive valid RDMP beacon from agent (read %d bytes)\n", hn);
    return -1;
}

int flasher_dump_memory(flasher_session_t *s, uint32_t addr, uint32_t len, const char *out_path, uint32_t chunk_size) {
    if (!s || !out_path || len == 0) return -1;

    if (chunk_size == 0 || chunk_size > 2048) {
        chunk_size = 2048;
    }

    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        fprintf(stderr, "[-] Failed to open output file: %s (%s)\n", out_path, strerror(errno));
        return -1;
    }

    printf("\n================================================================================\n");
    printf("[*] Starting Hardware Memory Extraction via Standalone RAM Dumper\n");
    printf("[*] Target Range: 0x%08X - 0x%08X (%u KB / %u bytes)\n",
           addr, addr + len, (len + 1023) / 1024, len);
    printf("[*] Chunk Size:   %u bytes\n", chunk_size);
    printf("[*] Output File:  %s\n", out_path);
    printf("================================================================================\n");

    /* Start RAM Dumper if not already running */
    if (flasher_start_ram_dumper(s) != 0) {
        fclose(fout);
        return -1;
    }

    uint32_t offset = 0;
    uint8_t rx_buf[2048];

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    while (offset < len) {
        uint32_t req_len = len - offset;
        if (req_len > chunk_size) req_len = chunk_size;

        /* Build 16-byte command: MAGIC_CMD_READ (0x44414552), addr, req_len, checksum */
        uint32_t cmd_buf[4];
        cmd_buf[0] = 0x44414552; /* "READ" */
        cmd_buf[1] = addr + offset;
        cmd_buf[2] = req_len;
        cmd_buf[3] = cmd_buf[0] ^ cmd_buf[1] ^ cmd_buf[2];

        if (transport_write(s->transport, cmd_buf, 16) != 16) {
            fprintf(stderr, "\n[-] Failed to send READ command at 0x%08X\n", addr + offset);
            fclose(fout);
            return -1;
        }

        /* Read req_len bytes from EP 0x83 */
        uint32_t read_bytes = 0;
        while (read_bytes < req_len) {
            int n = transport_read(s->transport, rx_buf + read_bytes, req_len - read_bytes, 4000);
            if (n <= 0) {
                fprintf(stderr, "\n[-] Timeout reading data at 0x%08X (got %u / %u bytes)\n",
                        addr + offset, read_bytes, req_len);
                fclose(fout);
                return -1;
            }
            read_bytes += (uint32_t)n;
        }

        if (fwrite(rx_buf, 1, req_len, fout) != req_len) {
            fprintf(stderr, "\n[-] Failed to write chunk to output file\n");
            fclose(fout);
            return -1;
        }

        offset += req_len;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start_time.tv_sec) + (now.tv_nsec - start_time.tv_nsec) / 1e9;
        double speed_kb = (elapsed > 0.05) ? ((offset / 1024.0) / elapsed) : 0.0;
        int pct = (int)((offset * 100ULL) / len);
        printf("\r[*] Memory Dump Progress: %3d%% (0x%08X / 0x%08X, %u KB, %.1f KB/s)   ",
               pct, addr + offset, addr + len, offset / 1024, speed_kb);
        fflush(stdout);
    }

    fclose(fout);

    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double total_time = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_nsec - start_time.tv_nsec) / 1e9;
    double avg_speed = (total_time > 0.05) ? ((len / 1024.0) / total_time) : 0.0;
    printf("\n[+] Successfully dumped %u bytes from 0x%08X to %s (%.2f s, %.1f KB/s)!\n\n",
           len, addr, out_path, total_time, avg_speed);

    /* If dumping Boot ROM or High Vectors (0x00000000 or 0xFFFF0000), validate Reset Vector */
    if ((addr == 0x00000000 || addr == 0xFFFF0000) && len >= 4) {
        FILE *vf = fopen(out_path, "rb");
        if (vf) {
            uint32_t reset_vec = 0;
            if (fread(&reset_vec, 1, 4, vf) == 4) {
                printf("[*] Reset Vector at 0x%08X: 0x%08X\n", addr, reset_vec);
                if ((reset_vec & 0xFF000000) == 0xEA000000) {
                    uint32_t target_branch = (reset_vec & 0x00FFFFFF);
                    uint32_t abs_target = (target_branch << 2) + 8 + addr;
                    printf("[+] Valid ARM Branch instruction detected (branches to 0x%08X)!\n", abs_target);
                } else if ((reset_vec & 0xFFFF0000) == 0xE59F0000) {
                    printf("[+] Valid ARM LDR PC instruction detected at 0x%08X!\n", addr);
                }
            }
            fclose(vf);
        }
    }

    return 0;
}

int flasher_dump_nand(flasher_session_t *s, uint32_t start_block, uint32_t block_count, const char *out_path) {
    if (!s || !out_path || block_count == 0) return -1;
    if (start_block + block_count > 1024) {
        fprintf(stderr, "[-] Error: block range %u..%u exceeds 1024 total blocks\n",
                start_block, start_block + block_count - 1);
        return -1;
    }

    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        fprintf(stderr, "[-] Failed to open output file: %s (%s)\n", out_path, strerror(errno));
        return -1;
    }

    uint32_t total_pages = block_count * 64;
    uint32_t total_bytes = total_pages * 2048;

    printf("\n================================================================================\n");
    printf("[*] Starting OneNAND Physical Flash Extraction via Standalone RAM Dumper\n");
    printf("[*] Block Range:  %u - %u (%u blocks, %u pages, %u KB / %u MB)\n",
           start_block, start_block + block_count - 1, block_count, total_pages,
           total_bytes / 1024, total_bytes / (1024 * 1024));
    printf("[*] Output File:  %s\n", out_path);
    printf("================================================================================\n");

    /* Start RAM Dumper if not already running */
    if (flasher_start_ram_dumper(s) != 0) {
        fclose(fout);
        return -1;
    }

    uint8_t page_buf[2048];
    uint32_t pages_done = 0;

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (uint32_t b = 0; b < block_count; b++) {
        uint32_t block = start_block + b;

        for (uint32_t page = 0; page < 64; page++) {
            /* Build 16-byte command: MAGIC_CMD_NAND_READ (0x444E414E), block, page (0..63), checksum */
            uint32_t cmd_buf[4];
            cmd_buf[0] = 0x444E414E; /* "NAND" */
            cmd_buf[1] = block;
            cmd_buf[2] = page; /* Pass page number 0..63 */
            cmd_buf[3] = cmd_buf[0] ^ cmd_buf[1] ^ cmd_buf[2];

            if (transport_write(s->transport, cmd_buf, 16) != 16) {
                fprintf(stderr, "\n[-] Failed to send NAND READ command (block %u, page %u)\n", block, page);
                fclose(fout);
                return -1;
            }

            /* Read 2048 bytes from EP 0x83 */
            uint32_t read_bytes = 0;
            while (read_bytes < 2048) {
                int n = transport_read(s->transport, page_buf + read_bytes, 2048 - read_bytes, 4000);
                if (n <= 0) {
                    fprintf(stderr, "\n[-] Timeout reading NAND page (block %u, page %u, got %u / 2048 bytes)\n",
                            block, page, read_bytes);
                    fclose(fout);
                    return -1;
                }
                read_bytes += (uint32_t)n;
            }

            if (fwrite(page_buf, 1, 2048, fout) != 2048) {
                fprintf(stderr, "\n[-] Failed to write page to output file\n");
                fclose(fout);
                return -1;
            }

            pages_done++;

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - start_time.tv_sec) + (now.tv_nsec - start_time.tv_nsec) / 1e9;
            double speed_kb = (elapsed > 0.05) ? (((pages_done * 2048.0) / 1024.0) / elapsed) : 0.0;
            int pct = (int)((pages_done * 100ULL) / total_pages);
            printf("\r[*] NAND Dump Progress: %3d%% (Block %4u/%4u, Page %2u/64, %u KB, %.1f KB/s)   ",
                   pct, block, start_block + block_count - 1, page, (pages_done * 2048) / 1024, speed_kb);
            fflush(stdout);
        }
    }

    fclose(fout);

    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double total_time = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_nsec - start_time.tv_nsec) / 1e9;
    double avg_speed = (total_time > 0.05) ? ((total_bytes / 1024.0) / total_time) : 0.0;
    printf("\n[+] Successfully dumped %u bytes (%u blocks) to %s (%.2f s, %.1f KB/s)!\n\n",
           total_bytes, block_count, out_path, total_time, avg_speed);

    return 0;
}

int flasher_dump_bootrom(flasher_session_t *s, const char *out_path) {
    if (!s || !out_path) return -1;

    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        fprintf(stderr, "[-] Failed to open output file: %s (%s)\n", out_path, strerror(errno));
        return -1;
    }

    const uint32_t bootrom_base = 0x00400000;
    const uint32_t bootrom_size = 32 * 1024; /* 32 KB (0x8000 bytes) */
    const uint32_t chunk_size = 2048;
    const uint32_t num_chunks = bootrom_size / chunk_size; /* 16 chunks */

    printf("\n================================================================================\n");
    printf("[*] Starting Silicon Boot ROM Extraction via Standalone RAM Dumper\n");
    printf("[*] Target:          Physical Mask ROM 0x00400000 - 0x00407FFF (32 KB)\n");
    printf("[*] Output File:     %s\n", out_path);
    printf("================================================================================\n");

    /* Start RAM Dumper if not already running */
    if (flasher_start_ram_dumper(s) != 0) {
        fclose(fout);
        return -1;
    }

    uint8_t rx_buf[2048];
    uint32_t total_done = 0;

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (uint32_t i = 0; i < num_chunks; i++) {
        uint32_t addr = bootrom_base + i * chunk_size;

        /* Build 16-byte command: MAGIC_CMD_DUMP_BOOTROM (0x42524F4D / "BROM"), addr, length, checksum */
        uint32_t cmd_buf[4];
        cmd_buf[0] = MAGIC_CMD_DUMP_BOOTROM;
        cmd_buf[1] = addr;
        cmd_buf[2] = chunk_size;
        cmd_buf[3] = cmd_buf[0] ^ cmd_buf[1] ^ cmd_buf[2];

        if (transport_write(s->transport, cmd_buf, 16) != 16) {
            fprintf(stderr, "\n[-] Failed to send BROM command at address 0x%08X\n", addr);
            fclose(fout);
            return -1;
        }

        /* Read 2048 bytes from EP 0x83 */
        uint32_t read_bytes = 0;
        while (read_bytes < chunk_size) {
            int n = transport_read(s->transport, rx_buf + read_bytes, chunk_size - read_bytes, 4000);
            if (n <= 0) {
                fprintf(stderr, "\n[-] Timeout reading Boot ROM at address 0x%08X (got %u / %u bytes)\n",
                        addr, read_bytes, chunk_size);
                fclose(fout);
                return -1;
            }
            read_bytes += (uint32_t)n;
        }

        if (fwrite(rx_buf, 1, chunk_size, fout) != chunk_size) {
            fprintf(stderr, "\n[-] Failed to write Boot ROM chunk to output file\n");
            fclose(fout);
            return -1;
        }

        total_done += chunk_size;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start_time.tv_sec) + (now.tv_nsec - start_time.tv_nsec) / 1e9;
        double speed_kb = (elapsed > 0.05) ? ((total_done / 1024.0) / elapsed) : 0.0;
        int pct = (int)((total_done * 100ULL) / bootrom_size);
        printf("\r[*] Boot ROM Dump Progress: %3d%% (0x%08X / 0x%08X, %u KB, %.1f KB/s)   ",
               pct, addr + chunk_size, bootrom_base + bootrom_size, total_done / 1024, speed_kb);
        fflush(stdout);
    }

    fclose(fout);

    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double total_time = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_nsec - start_time.tv_nsec) / 1e9;
    double avg_speed = (total_time > 0.05) ? ((bootrom_size / 1024.0) / total_time) : 0.0;
    printf("\n[+] Successfully dumped %u bytes Silicon Boot ROM to %s (%.2f s, %.1f KB/s)!\n",
           bootrom_size, out_path, total_time, avg_speed);

    /* Validate Reset Vector */
    FILE *vf = fopen(out_path, "rb");
    if (vf) {
        uint32_t reset_vec = 0;
        if (fread(&reset_vec, 1, 4, vf) == 4) {
            printf("[*] Silicon Boot ROM Reset Vector (at 0x00400000): 0x%08X\n", reset_vec);
            if ((reset_vec & 0xFF000000) == 0xEA000000) {
                uint32_t target_branch = (reset_vec & 0x00FFFFFF);
                uint32_t abs_target = (target_branch << 2) + 8;
                printf("[+] Valid ARM Branch instruction detected (entry point 0x%08X)!\n", abs_target);
            } else if ((reset_vec & 0xFFFF0000) == 0xE59F0000) {
                uint32_t lit_offset = (reset_vec & 0x0FFF);
                fseek(vf, lit_offset + 8, SEEK_SET);
                uint32_t handler_addr = 0;
                if (fread(&handler_addr, 1, 4, vf) == 4) {
                    printf("[+] Valid ARM LDR PC instruction detected (Reset Entry Handler: 0x%08X)!\n", handler_addr);
                } else {
                    printf("[+] Valid ARM LDR PC instruction detected at 0x00400000!\n");
                }
            }
        }
        fclose(vf);
    }
    printf("\n");

    return 0;
}

int flasher_dump_nvram(flasher_session_t *s, const char *out_path) {
    return flasher_dump_memory(s, 0x60080000, 0x00100000, out_path, 2048);
}

int flasher_boot_ram(flasher_session_t *s, uint32_t ram_addr, const uint8_t *data, size_t size) {
    if (!s || !data || size == 0) return -1;

    /* 1. Sync handshake with Boot ROM */
    if (flasher_sync(s, 40) != 0) return -1;

    /* 2. Upload Open PSI into SRAM */
    if (flasher_upload_psi(s, NULL) != 0) return -1;

    /* 3. Connect to Open PSI agent */
    if (flasher_connect_agent(s) != 0) return -1;

    printf("[*] OpenIFWD RAM Boot: streaming %zu bytes to 0x%08X...\n", size, (unsigned int)ram_addr);
    const size_t chunk_size = 2048;
    size_t sent = 0;

    while (sent < size) {
        size_t chunk = size - sent;
        if (chunk > chunk_size) chunk = chunk_size;

        if (flasher_open_mem_write(s, ram_addr + (uint32_t)sent, data + sent, chunk) != 0) {
            fprintf(stderr, "\n[-] Memory write failed at offset %zu\n", sent);
            return -1;
        }

        sent += chunk;
        int pct = (int)((sent * 100) / size);
        printf("\r[*] Uploading to RAM: %3d%% (%zu / %zu bytes)", pct, sent, size);
        fflush(stdout);
    }
    printf("\n[+] Payload successfully loaded into SDRAM!\n");
    printf("[*] Executing code at 0x%08X in Supervisor mode...\n", (unsigned int)ram_addr);
    return flasher_open_exec_ram(s, ram_addr);
}



/* ============================================================================
 * Open Flasher (OFLS / RESP) Native Protocol Implementation
 * ============================================================================ */

static uint32_t ofls_crc32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320U;
            else crc >>= 1;
        }
    }
    return ~crc;
}

static int ofls_send_cmd(flasher_session_t *s, uint32_t cmd_id, uint32_t addr, uint32_t length, ofls_resp_t *out_resp, int timeout_ms) {
    if (!s || !s->transport) return -1;

    ofls_cmd_t cmd;
    cmd.magic  = MAGIC_OFLS_CMD;
    cmd.cmd    = cmd_id;
    cmd.addr   = addr;
    cmd.length = length;

    if (transport_write(s->transport, &cmd, sizeof(cmd)) != (int)sizeof(cmd)) {
        fprintf(stderr, "[-] Failed to write OFLS command 0x%02X on EP 0x02\n", cmd_id);
        return -1;
    }

    ofls_resp_t resp;
    int r = transport_read(s->transport, &resp, sizeof(resp), timeout_ms);
    if (r != (int)sizeof(resp)) {
        fprintf(stderr, "[-] Incomplete OFLS response (got %d bytes, expected %zu)\n", r, sizeof(resp));
        return -2;
    }

    if (resp.magic != MAGIC_OFLS_RESP) {
        fprintf(stderr, "[-] Invalid OFLS response magic: 0x%08X (expected 0x%08X)\n", resp.magic, MAGIC_OFLS_RESP);
        return -3;
    }

    if (out_resp) {
        *out_resp = resp;
    }

    return 0;
}

int flasher_start_open_flasher(flasher_session_t *s) {
    if (!s || !s->transport) return -1;

    /* 1. Quick probe: check if Open Flasher is already running on the device */
    ofls_cmd_t probe_cmd = {MAGIC_OFLS_CMD, OFLS_CMD_PING, 0, 0};
    if (transport_write(s->transport, &probe_cmd, sizeof(probe_cmd)) == (int)sizeof(probe_cmd)) {
        ofls_resp_t probe_resp;
        int pr = transport_read(s->transport, &probe_resp, sizeof(probe_resp), 200);
        if (pr == (int)sizeof(probe_resp) && probe_resp.magic == MAGIC_OFLS_RESP && probe_resp.status == 0) {
            printf("[+] Open Flasher agent is already active and responding on EP 0x83!\n");
            return 0;
        }
    }

    /* 2. Synchronize with Boot ROM */
    if (flasher_sync(s, 40) != 0) return -1;

    usleep(20000);
    transport_flush(s->transport);
    printf("[+] Device synchronized with Boot ROM!\n");

    /* 3. Upload PSI (Stage 1) */
    uint8_t *allocated_psi = NULL;
    const uint8_t *psi_data = NULL;
    size_t psi_size = 0;
    uint8_t psi_chk = 0;

    allocated_psi = resolve_and_load_psi(s, &psi_size, &psi_chk);
    if (!allocated_psi) {
        return -1;
    }
    psi_data = allocated_psi;
    printf("[*] Preparing verified PSI Stage 1 (%zu bytes, XOR-8 0x%02X)...\n", psi_size, psi_chk);

    /* Verify SHA-256 against trusted table */
    if (check_psi_integrity(s, psi_data, psi_size, allocated_psi) != 0) {
        return -1;
    }

    uint8_t boot_hdr[3];
    boot_hdr[0] = 0x30;
    boot_hdr[1] = (uint8_t)(psi_size & 0xFF);
    boot_hdr[2] = (uint8_t)((psi_size >> 8) & 0xFF);

    if (transport_write(s->transport, boot_hdr, 3) != 3) {
        fprintf(stderr, "[-] Failed to write Boot ROM 0x30 header\n");
        if (allocated_psi) free(allocated_psi);
        return -1;
    }

    printf("[*] Streaming %zu bytes of PSI binary to SRAM at 0x00080000...\n", psi_size);
    if (transport_write(s->transport, psi_data, psi_size) != (int)psi_size) {
        fprintf(stderr, "[-] Failed to stream PSI binary\n");
        if (allocated_psi) free(allocated_psi);
        return -1;
    }

    printf("[*] Sending XOR-8 checksum: 0x%02X\n", psi_chk);
    if (transport_write(s->transport, &psi_chk, 1) != 1) {
        fprintf(stderr, "[-] Failed to send PSI checksum\n");
        if (allocated_psi) free(allocated_psi);
        return -1;
    }

    uint8_t ack_buf[16] = {0};
    int r = transport_read(s->transport, ack_buf, 1, 2000);
    if (r <= 0) r = transport_read(s->transport, ack_buf, 1, 2000);

    if (r > 0 && ack_buf[0] == 0x01) {
        printf("[+] PSI acknowledged by Boot ROM (0x01)\n");
        uint8_t ack2[4] = {0};
        int r2 = transport_read(s->transport, ack2, 1, 1000);
        if (r2 > 0 && ack2[0] == 0x01) {
            printf("[+] PSI execution handover confirmed by Boot ROM (0x01)\n");
        }
        usleep(50000);
    } else {
        fprintf(stderr, "[-] Unexpected Boot ROM ACK (%d bytes, 0x%02X)\n", r, ack_buf[0]);
        if (allocated_psi) free(allocated_psi);
        return -1;
    }

    if (allocated_psi) {
        free(allocated_psi);
        allocated_psi = NULL;
    }

    /* 4. Wait for Stock PSI Readiness Token (0x00 0xAA) on EP 0x83 */
    printf("[*] Waiting for Stock PSI readiness token on EP 0x83...\n");
    uint8_t psi_tok[4] = {0};
    int tn = transport_read(s->transport, psi_tok, 2, 3000);
    if (tn == 2 && psi_tok[0] == 0x00 && psi_tok[1] == 0xAA) {
        printf("[+] Stock PSI ready! Received token (0x00 0xAA) - SDRAM & USB active.\n");
    } else {
        fprintf(stderr, "[-] Failed waiting for PSI readiness token (got %d bytes: 0x%02X 0x%02X)\n",
                tn, psi_tok[0], psi_tok[1]);
        return -1;
    }

    /* 5. Send Open Flasher (Stage 2) Size (4 bytes uint32_le) on EP 0x02 */
    uint32_t flasher_size = (uint32_t)open_flasher_size;
    uint8_t sz_buf[4];
    sz_buf[0] = (uint8_t)(flasher_size & 0xFF);
    sz_buf[1] = (uint8_t)((flasher_size >> 8) & 0xFF);
    sz_buf[2] = (uint8_t)((flasher_size >> 16) & 0xFF);
    sz_buf[3] = (uint8_t)((flasher_size >> 24) & 0xFF);

    printf("[*] Sending Open Flasher size (%u bytes) to PSI on EP 0x02...\n", flasher_size);
    if (transport_write(s->transport, sz_buf, 4) != 4) {
        fprintf(stderr, "[-] Failed to write Open Flasher size\n");
        return -1;
    }

    /* 6. Read Stock PSI ACK (0xCC 0xCC) on EP 0x83 */
    uint8_t sz_ack[4] = {0};
    int an = transport_read(s->transport, sz_ack, 2, 2000);
    if (an == 2 && sz_ack[0] == 0xCC && sz_ack[1] == 0xCC) {
        printf("[+] Stock PSI acknowledged upload request (0xCC 0xCC)\n");
    } else {
        fprintf(stderr, "[-] PSI upload size rejected (got %d bytes: 0x%02X 0x%02X)\n",
                an, sz_ack[0], sz_ack[1]);
        return -1;
    }

    /* 7. Stream Open Flasher payload + 1-byte checksum to SDRAM at 0x60000000 */
    size_t total_xfer = open_flasher_size + 1;
    uint8_t *xfer_buf = (uint8_t *)malloc(total_xfer);
    if (!xfer_buf) {
        fprintf(stderr, "[-] Out of memory preparing Open Flasher transfer\n");
        return -1;
    }
    memcpy(xfer_buf, open_flasher_binary, open_flasher_size);
    xfer_buf[open_flasher_size] = open_flasher_checksum;

    printf("[*] Streaming %zu bytes (payload: %zu B, chk: 0x%02X) to SDRAM at 0x60000000...\n",
           total_xfer, open_flasher_size, open_flasher_checksum);
    if (transport_write(s->transport, xfer_buf, total_xfer) != (int)total_xfer) {
        fprintf(stderr, "[-] Failed to stream Open Flasher payload\n");
        free(xfer_buf);
        return -1;
    }
    free(xfer_buf);

    /* 8. Read PSI Handover ACK on EP 0x83 */
    uint8_t chk_ack[4] = {0};
    int cn = transport_read(s->transport, chk_ack, 2, 2000);
    if (cn >= 1 && (chk_ack[1] == 0xA5 || chk_ack[0] == 0xA5 || chk_ack[0] == 0x01)) {
        printf("[+] Stock PSI verified Open Flasher (ACK 0x%02X 0x%02X). Handing over execution to 0x60000400!\n",
               chk_ack[0], chk_ack[1]);
    } else {
        printf("[*] PSI ACK response: %d bytes (0x%02X 0x%02X)\n", cn, chk_ack[0], chk_ack[1]);
    }

    /* 9. Read Open Flasher Startup Beacon ("RESP" / "PONG") on EP 0x83 */
    printf("[*] Waiting for Open Flasher startup beacon on EP 0x83...\n");
    ofls_resp_t beacon;
    int bn = transport_read(s->transport, &beacon, sizeof(beacon), 5000);
    if (bn == (int)sizeof(beacon) && beacon.magic == MAGIC_OFLS_RESP && beacon.crc32 == MAGIC_OFLS_PONG) {
        printf("[+] Connected to Open Flasher Agent! Protocol version: %u\n", beacon.bytes_done);
        return 0;
    }

    fprintf(stderr, "[-] Did not receive valid OFLS beacon from agent (read %d bytes)\n", bn);
    return -1;
}

int flasher_ofls_ping(flasher_session_t *s) {
    if (flasher_start_open_flasher(s) != 0) return -1;

    printf("[*] Probing Open Flasher PING...\n");
    ofls_resp_t resp;
    int r = ofls_send_cmd(s, OFLS_CMD_PING, 0, 0, &resp, 2000);
    if (r == 0 && resp.status == 0 && resp.crc32 == MAGIC_OFLS_PONG) {
        printf("[+] PONG received! Protocol version: %u\n", resp.bytes_done);
        return 0;
    }

    fprintf(stderr, "[-] OFLS Ping failed (err: %d, status: 0x%08X)\n", r, resp.status);
    return -1;
}

int flasher_ofls_id(flasher_session_t *s, uint16_t *out_mfr, uint16_t *out_dev) {
    if (flasher_start_open_flasher(s) != 0) return -1;

    printf("[*] Querying OneNAND Flash ID...\n");
    ofls_resp_t resp;
    int r = ofls_send_cmd(s, OFLS_CMD_FLASH_ID, 0, 0, &resp, 2000);
    if (r == 0 && resp.status == 0) {
        uint16_t mfr = (uint16_t)((resp.bytes_done >> 16) & 0xFFFF);
        uint16_t dev = (uint16_t)(resp.bytes_done & 0xFFFF);
        if (out_mfr) *out_mfr = mfr;
        if (out_dev) *out_dev = dev;

        printf("[+] Samsung OneNAND Flash Detected!\n");
        printf("    Manufacturer ID: 0x%04X (Samsung: %s)\n", mfr, (mfr == 0x00EC) ? "YES" : "NO");
        printf("    Device ID:       0x%04X\n", dev);
        if (dev == 0x0055) {
            printf("    Chip Model:      Samsung KFM1G16Q2C (1Gb / 128MB OneNAND)\n");
        }
        return 0;
    }

    fprintf(stderr, "[-] OFLS Flash ID query failed (err: %d, status: 0x%08X)\n", r, resp.status);
    return -1;
}

int flasher_ofls_erase(flasher_session_t *s, uint32_t block, uint32_t count) {
    if (count == 0) count = 1;

    /* NVRAM & Metadata Protection Check */
    if (block >= ONENAND_PROTECTED_BLOCK_START || (block + count) > ONENAND_PROTECTED_BLOCK_START) {
        if (!s || !s->config.force_dangerous_erase) {
            fprintf(stderr, "[-] ERROR: Block range %u..%u targets protected calibration/metadata area (978..1023)!\n",
                    block, block + count - 1);
            fprintf(stderr, "[-] Operation aborted to prevent permanent loss of RF calibration & IMEI.\n");
            return -EPERM;
        }
    }

    if (flasher_start_open_flasher(s) != 0) return -1;

    printf("[*] Erasing %u OneNAND block(s) starting from block %u...\n", count, block);
    ofls_resp_t resp;
    int r = ofls_send_cmd(s, OFLS_CMD_BLOCK_ERASE, block, count, &resp, 10000);
    if (r == 0 && resp.status == 0) {
        printf("[+] Successfully erased %u block(s)!\n", resp.bytes_done);
        return 0;
    }

    if (resp.status == IFWD_ERR_NVRAM_PROTECTED) {
        fprintf(stderr, "[-] Agent rejected erase: Block is in protected NVRAM calibration area!\n");
        return -EPERM;
    }

    uint16_t ctrl_stat = (uint16_t)((resp.bytes_done >> 16) & 0xFFFF);
    uint16_t wp_stat   = (uint16_t)(resp.bytes_done & 0xFFFF);
    uint16_t int_stat  = (uint16_t)(resp.crc32 & 0xFFFF);

    fprintf(stderr, "\n[-] ====================================================================\n");
    fprintf(stderr, "[-] OFLS Block Erase FAILED (status code 0x%08X)\n", resp.status);
    fprintf(stderr, "[-] OneNAND Hardware Diagnostics:\n");
    fprintf(stderr, "[-]   CTRL_STATUS (0x1E480): 0x%04X\n", ctrl_stat);
    if (ctrl_stat & 0x0400) fprintf(stderr, "[-]     -> Bit 10: Erase/Program Hardware Error\n");
    if (ctrl_stat & 0x0100) fprintf(stderr, "[-]     -> Bit  8: Locked Block Access Violation\n");
    if (ctrl_stat & 0x8000) fprintf(stderr, "[-]     -> Bit 15: Operation still Ongoing\n");
    fprintf(stderr, "[-]   WP_STATUS   (0x1E49C): 0x%04X\n", wp_stat);
    fprintf(stderr, "[-]     -> Bit 2 (US)  = %u (%s)\n", (wp_stat >> 2) & 1, (wp_stat & 4) ? "Unlocked" : "NOT Unlocked");
    fprintf(stderr, "[-]     -> Bit 1 (LS)  = %u (%s)\n", (wp_stat >> 1) & 1, (wp_stat & 2) ? "LOCKED" : "Not Locked");
    fprintf(stderr, "[-]     -> Bit 0 (LTS) = %u (%s)\n", wp_stat & 1, (wp_stat & 1) ? "LOCKED-TIGHT (Hardware WP/Reset Required)" : "Not Locked-Tight");
    fprintf(stderr, "[-]   INT_STATUS  (0x1E482): 0x%04X\n", int_stat);
    fprintf(stderr, "[-]     -> Bit 15 (INT_MASTER) = %u\n", (int_stat >> 15) & 1);
    fprintf(stderr, "[-]     -> Bit  5 (INT_ERASE)  = %u\n", (int_stat >> 5) & 1);
    fprintf(stderr, "[-] ====================================================================\n\n");
    return -1;
}

int flasher_ofls_write_page(flasher_session_t *s, uint32_t block, uint32_t page, const uint8_t *data, size_t len) {
    if (!data || len == 0) return -1;

    /* NVRAM & Metadata Protection Check */
    if (block >= ONENAND_PROTECTED_BLOCK_START) {
        if (!s || !s->config.force_dangerous_erase) {
            fprintf(stderr, "[-] ERROR: Block %u is in protected calibration/metadata area (978..1023)!\n", block);
            fprintf(stderr, "[-] Operation aborted to prevent permanent loss of RF calibration & IMEI.\n");
            return -EPERM;
        }
    }

    if (flasher_start_open_flasher(s) != 0) return -1;

    uint8_t page_buf[2048];
    if (len >= 2048) {
        memcpy(page_buf, data, 2048);
    } else {
        memcpy(page_buf, data, len);
        memset(page_buf + len, 0xFF, 2048 - len);
    }

    uint32_t expected_crc = ofls_crc32(page_buf, 2048);

    /* 1. Send CMD_PAGE_WRITE */
    ofls_cmd_t cmd = {MAGIC_OFLS_CMD, OFLS_CMD_PAGE_WRITE, block, page};
    if (transport_write(s->transport, &cmd, sizeof(cmd)) != (int)sizeof(cmd)) {
        fprintf(stderr, "[-] Failed to write OFLS Page Write command\n");
        return -1;
    }

    /* 2. Stream 2048-byte page payload */
    if (transport_write(s->transport, page_buf, 2048) != 2048) {
        fprintf(stderr, "[-] Failed to stream 2048-byte page data\n");
        return -1;
    }

    /* 3. Read response */
    ofls_resp_t resp;
    int r = transport_read(s->transport, &resp, sizeof(resp), 5000);
    if (r != (int)sizeof(resp) || resp.magic != MAGIC_OFLS_RESP) {
        fprintf(stderr, "[-] Failed reading OFLS Page Write response\n");
        return -2;
    }

    if (resp.status == IFWD_ERR_NVRAM_PROTECTED) {
        fprintf(stderr, "[-] Agent rejected write: Block %u is in protected NVRAM calibration area!\n", block);
        return -EPERM;
    }

    if (resp.status == 0 && resp.crc32 == expected_crc) {
        return 0;
    }

    fprintf(stderr, "[-] OFLS Page Write failed: status=0x%08X, CRC: 0x%08X (expected: 0x%08X)\n",
            resp.status, resp.crc32, expected_crc);
    return -3;
}

int flasher_ofls_read_page(flasher_session_t *s, uint32_t block, uint32_t page, uint8_t *out_data, size_t len) {
    if (!out_data || len == 0) return -1;

    if (flasher_start_open_flasher(s) != 0) return -1;

    ofls_cmd_t cmd = {MAGIC_OFLS_CMD, OFLS_CMD_PAGE_READ, block, page};
    if (transport_write(s->transport, &cmd, sizeof(cmd)) != (int)sizeof(cmd)) {
        fprintf(stderr, "[-] Failed to write OFLS Page Read command\n");
        return -1;
    }

    /* 1. Read response header */
    ofls_resp_t resp;
    int r = transport_read(s->transport, &resp, sizeof(resp), 5000);
    if (r != (int)sizeof(resp) || resp.magic != MAGIC_OFLS_RESP) {
        fprintf(stderr, "[-] Failed reading OFLS Page Read response header\n");
        return -2;
    }

    if (resp.status != 0 && resp.status != 5) { /* 5 indicates ECC warning */
        fprintf(stderr, "[-] OFLS Page Read reported error: status=0x%08X\n", resp.status);
        return -3;
    }

    /* 2. Read 2048-byte page payload */
    uint8_t page_buf[2048];
    int pr = transport_read(s->transport, page_buf, 2048, 5000);
    if (pr != 2048) {
        fprintf(stderr, "[-] Incomplete page payload received (got %d bytes, expected 2048)\n", pr);
        return -4;
    }

    uint32_t calc_crc = ofls_crc32(page_buf, 2048);
    if (calc_crc != resp.crc32) {
        fprintf(stderr, "[!] Warning: CRC32 mismatch on page read (agent: 0x%08X, host: 0x%08X)\n",
                resp.crc32, calc_crc);
    }

    size_t copy_sz = (len < 2048) ? len : 2048;
    memcpy(out_data, page_buf, copy_sz);

    if (resp.status == 5) {
        printf("[!] Notice: ECC warning flagged by OneNAND controller for Block %u, Page %u\n", block, page);
    }

    return 0;
}

int flasher_ofls_reset(flasher_session_t *s) {
    printf("[*] Sending OFLS RESET to agent...\n");
    ofls_resp_t resp;
    ofls_send_cmd(s, OFLS_CMD_RESET, 0, 0, &resp, 1000);
    printf("[+] Reset command sent. Phone rebooting...\n");
    return 0;
}

int flasher_flash_binary_to_partition(flasher_session_t *s, const char *bin_path, uint32_t start_block, uint32_t max_blocks, const char *label) {
    if (!s || !bin_path || access(bin_path, R_OK) != 0) {
        fprintf(stderr, "[-] Cannot access %s file: %s (%s)\n", label, bin_path ? bin_path : "<null>", strerror(errno));
        return -1;
    }

    FILE *f = fopen(bin_path, "rb");
    if (!f) {
        fprintf(stderr, "[-] Failed to open %s: %s\n", bin_path, strerror(errno));
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_sz = ftell(f);
    rewind(f);

    if (file_sz <= 0) {
        fprintf(stderr, "[-] %s file %s is empty\n", label, bin_path);
        fclose(f);
        return -1;
    }

    uint32_t blocks_needed = (uint32_t)((file_sz + ONENAND_BLOCK_SIZE - 1) / ONENAND_BLOCK_SIZE);
    if (blocks_needed > max_blocks) {
        fprintf(stderr, "[-] ERROR: File %s (%ld bytes, %u blocks) exceeds partition capacity (%u blocks)!\n",
                bin_path, file_sz, blocks_needed, max_blocks);
        fclose(f);
        return -1;
    }

    /* Check NVRAM safety */
    if (start_block >= ONENAND_PROTECTED_BLOCK_START || (start_block + blocks_needed) > ONENAND_PROTECTED_BLOCK_START) {
        if (!s->config.force_dangerous_erase) {
            fprintf(stderr, "[-] ERROR: Partition write targets protected NVRAM calibration area (978..1023)!\n");
            fclose(f);
            return -EPERM;
        }
    }

    printf("\n[*] Flashing Partition [%s] from: %s\n", label, bin_path);
    printf("    Target Range: Blocks %u .. %u (%u blocks, %ld bytes)\n",
           start_block, start_block + blocks_needed - 1, blocks_needed, file_sz);

    /* 1. Erase target partition blocks */
    printf("[*] Erasing %u block(s) for partition [%s]...\n", blocks_needed, label);
    if (flasher_ofls_erase(s, start_block, blocks_needed) != 0) {
        fprintf(stderr, "[-] Failed to erase blocks for [%s]\n", label);
        fclose(f);
        return -1;
    }

    /* 2. Program pages */
    uint8_t page_buf[ONENAND_PAGE_SIZE];
    uint32_t total_pages = (uint32_t)((file_sz + ONENAND_PAGE_SIZE - 1) / ONENAND_PAGE_SIZE);
    uint32_t pages_done = 0;

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (uint32_t b = 0; b < blocks_needed; b++) {
        uint32_t blk = start_block + b;
        for (uint32_t p = 0; p < ONENAND_PAGES_PER_BLOCK; p++) {
            if (pages_done >= total_pages) break;

            memset(page_buf, 0xFF, sizeof(page_buf));
            size_t nread = fread(page_buf, 1, sizeof(page_buf), f);
            if (nread == 0 && ferror(f)) {
                fprintf(stderr, "\n[-] Read error reading %s at block %u, page %u\n", bin_path, blk, p);
                fclose(f);
                return -1;
            }

            if (flasher_ofls_write_page(s, blk, p, page_buf, sizeof(page_buf)) != 0) {
                fprintf(stderr, "\n[-] Write failed at Block %u, Page %u\n", blk, p);
                fclose(f);
                return -1;
            }

            pages_done++;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - start_time.tv_sec) + (now.tv_nsec - start_time.tv_nsec) / 1e9;
            double speed_kb = (elapsed > 0.05) ? (((size_t)pages_done * 2) / elapsed) : 0.0;
            int pct = (int)((pages_done * 100ULL) / total_pages);
            printf("\r[*] [%s] Progress: %3d%% (%u / %u pages, Block %u Page %u, %.1f KB/s)   ",
                   label, pct, pages_done, total_pages, blk, p, speed_kb);
            fflush(stdout);
        }
    }

    fclose(f);
    printf("\n[+] Partition [%s] flashed successfully! (%u pages written)\n", label, pages_done);
    return 0;
}

int flasher_init_sec_cfg(flasher_session_t *s, const char *fls_path) {
    if (!s || !fls_path || access(fls_path, R_OK) != 0) {
        return -1;
    }

    fls_file_t *fls = fls_open(fls_path);
    if (!fls) {
        return -1;
    }

    size_t sec_len = 0;
    const uint8_t *sec_data = fls_get_sec_cfg(fls, &sec_len);
    if (!sec_data || sec_len != 2048) {
        fls_close(fls);
        return 0;
    }

    /* Copy raw 2048-byte Tag 15 SEC_CFG into session context */
    memcpy(s->sec_cfg, sec_data, 2048);
    s->has_sec_cfg = true;

    printf("\n[*] Initializing Security Configuration (Tag 15) from: %s\n", fls_path);
    printf("    SEC_CFG Size: 2048 bytes loaded into session\n");

    /* Validate magic at offset 1916 (0x77C) */
    uint32_t magic = *(const uint32_t *)(sec_data + 1916);
    if (magic == FLS_SEC_CFG_MAGIC) {
        printf("[+] Security Signature: 0x%08X (FLS_SEC_CFG_MAGIC Verified)\n", magic);
    } else {
        printf("[!] Security Signature: 0x%08X (Expected 0x%08X)\n", magic, FLS_SEC_CFG_MAGIC);
    }
    printf("[+] Target Security Profile: 2150_OD_KL1_3.1 (Secured)\n");

    /* Parse partition entries */
    fls_sec_cfg_entry_t entries[8];
    size_t count = 0;
    if (fls_get_sec_cfg_entries(fls, entries, 8, &count) == 0 && count > 0) {
        printf("[+] Security Partition Descriptors (%zu found):\n", count);
        for (size_t e = 0; e < count; e++) {
            printf("    [%zu] Load Addr: 0x%08X, Max Size: %6u KB, Image Size: %6u KB\n",
                   e + 1, entries[e].load_address,
                   entries[e].partition_size / 1024,
                   entries[e].image_size / 1024);
        }
    }

    fls_close(fls);
    return 0;
}

int flasher_flash_slb(flasher_session_t *s, const char *slb_path) {
    if (!s || !slb_path || access(slb_path, R_OK) != 0) {
        fprintf(stderr, "[-] Cannot access SLB container: %s (%s)\n", slb_path ? slb_path : "<null>", strerror(errno));
        return -1;
    }

    /* Initialize SEC_CFG from SLB container if available */
    flasher_init_sec_cfg(s, slb_path);

    printf("\n================================================================================\n");
    printf("[*] Flashing Second Level Bootloader (SLB): %s\n", slb_path);
    printf("================================================================================\n");

    fls_file_t *fls = fls_open(slb_path);
    if (!fls) {
        fprintf(stderr, "[-] Failed to open SLB container: %s\n", slb_path);
        return -1;
    }

    const fls_chunk_info_t *c = fls_find_chunk(fls, FLS_TAG_IMAGE_DATA);
    if (!c) {
        fprintf(stderr, "[-] No IMAGE_DATA chunk (Tag 12) found in SLB container %s\n", slb_path);
        fls_close(fls);
        return -1;
    }

    size_t payload_len = 0;
    const uint8_t *payload = fls_get_image_data(fls, c, &payload_len);
    if (!payload || payload_len == 0) {
        fprintf(stderr, "[-] Failed to extract SLB payload from %s\n", slb_path);
        fls_close(fls);
        return -1;
    }

    /* 1. Calculate blocks needed: ceil(payload_len / ONENAND_BLOCK_SIZE) */
    uint32_t blocks_needed = (uint32_t)((payload_len + ONENAND_BLOCK_SIZE - 1) / ONENAND_BLOCK_SIZE);
    uint32_t start_block = ONENAND_PART_EBL_START; /* Block 2 */

    if (blocks_needed > ONENAND_PART_EBL_BLOCKS) {
        fprintf(stderr, "[-] Error: SLB size (%zu bytes, %u blocks) exceeds partition capacity (%u blocks)\n",
                payload_len, blocks_needed, ONENAND_PART_EBL_BLOCKS);
        fls_close(fls);
        return -1;
    }

    printf("[*] SLB Payload: %zu bytes (%u blocks needed: Blocks %u .. %u)\n",
           payload_len, blocks_needed, start_block, start_block + blocks_needed - 1);

    /* 2. Erase target blocks (for SLB: Blocks 2, 3, 4) */
    printf("[*] Erasing %u block(s) for SLB (Blocks %u .. %u)...\n",
           blocks_needed, start_block, start_block + blocks_needed - 1);
    if (flasher_ofls_erase(s, start_block, blocks_needed) != 0) {
        fprintf(stderr, "[-] Failed to erase SLB blocks (%u .. %u)\n",
                start_block, start_block + blocks_needed - 1);
        fls_close(fls);
        return -1;
    }

    /* 3. Program pages with block increment every 64 pages (128 KB) */
    uint32_t total_pages = (uint32_t)((payload_len + ONENAND_PAGE_SIZE - 1) / ONENAND_PAGE_SIZE);
    uint8_t page_buf[ONENAND_PAGE_SIZE];

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (uint32_t pages_done = 0; pages_done < total_pages; pages_done++) {
        uint32_t blk = start_block + (pages_done / ONENAND_PAGES_PER_BLOCK);
        uint32_t page = pages_done % ONENAND_PAGES_PER_BLOCK;

        size_t offset = (size_t)pages_done * ONENAND_PAGE_SIZE;
        memset(page_buf, 0xFF, sizeof(page_buf));

        size_t copy_sz = payload_len - offset;
        if (copy_sz > ONENAND_PAGE_SIZE) copy_sz = ONENAND_PAGE_SIZE;
        memcpy(page_buf, payload + offset, copy_sz);

        if (flasher_ofls_write_page(s, blk, page, page_buf, sizeof(page_buf)) != 0) {
            fprintf(stderr, "\n[-] SLB write failed at Block %u, Page %u (page %u / %u)\n",
                    blk, page, pages_done + 1, total_pages);
            fls_close(fls);
            return -1;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start_time.tv_sec) + (now.tv_nsec - start_time.tv_nsec) / 1e9;
        double speed_kb = (elapsed > 0.05) ? (((size_t)(pages_done + 1) * 2) / elapsed) : 0.0;
        int pct = (int)(((pages_done + 1) * 100ULL) / total_pages);
        printf("\r[*] [SLB] Progress: %3d%% (%u / %u pages, Block %u Page %u, %.1f KB/s)   ",
               pct, pages_done + 1, total_pages, blk, page, speed_kb);
        fflush(stdout);
    }

    printf("\n[+] Second Level Bootloader (SLB) successfully written! (%u pages across %u blocks)\n",
           total_pages, blocks_needed);

    fls_close(fls);
    return 0;
}

int flasher_flash_fls_container(flasher_session_t *s, const char *fls_path) {
    if (!s || !fls_path || access(fls_path, R_OK) != 0) {
        fprintf(stderr, "[-] Cannot access FLS container: %s (%s)\n", fls_path ? fls_path : "<null>", strerror(errno));
        return -1;
    }

    /* If this is an SLB container, delegate directly to dedicated multi-block SLB flasher */
    if (strstr(fls_path, "slb") || strstr(fls_path, "SLB")) {
        return flasher_flash_slb(s, fls_path);
    }

    /* Initialize SEC_CFG from container if available */
    flasher_init_sec_cfg(s, fls_path);

    printf("\n[*] Processing FLS container: %s\n", fls_path);
    fls_file_t *fls = fls_open(fls_path);
    if (!fls) {
        fprintf(stderr, "[*] Note: %s is not an FLS container; attempting direct raw partition flash to CODE\n", fls_path);
        return flasher_flash_binary_to_partition(s, fls_path, ONENAND_PART_CODE_START, ONENAND_PART_CODE_BLOCKS, "CODE");
    }

    /* Determine partition mapping based on file name or TOC */
    uint32_t start_block = ONENAND_PART_CODE_START;
    uint32_t max_blocks  = ONENAND_PART_CODE_BLOCKS;
    const char *label    = "CODE/BSY";

    if (strstr(fls_path, "dffs") || strstr(fls_path, "DFFS") || strstr(fls_path, "ifs") || strstr(fls_path, "IFS")) {
        start_block = ONENAND_PART_IFS_START;
        max_blocks  = ONENAND_PART_IFS_BLOCKS;
        label       = "IFS/DFFS";
    } else if (strstr(fls_path, "cds") || strstr(fls_path, "CDS") || strstr(fls_path, "csc") || strstr(fls_path, "CSC")) {
        start_block = ONENAND_PART_CSC_START;
        max_blocks  = ONENAND_PART_CSC_BLOCKS;
        label       = "CSC/CDS";
    }

    const fls_chunk_info_t *chunks[32];
    size_t n_chunks = fls_find_all_chunks(fls, FLS_TAG_IMAGE_DATA, chunks, 32);

    if (n_chunks == 0) {
        const fls_chunk_info_t *c = fls_find_chunk(fls, FLS_TAG_PARTITION_TABLE);
        if (c) {
            chunks[0] = c;
            n_chunks = 1;
        }
    }

    if (n_chunks == 0) {
        fprintf(stderr, "[-] No flashable IMAGE_DATA or payload chunks found in %s\n", fls_path);
        fls_close(fls);
        return -1;
    }

    printf("[*] Found %zu flashable image chunk(s) in %s (Targeting [%s] starting at Block %u)\n",
           n_chunks, fls_path, label, start_block);

    uint32_t cur_block = start_block;
    uint32_t total_written_blocks = 0;

    for (size_t i = 0; i < n_chunks; i++) {
        size_t payload_len = 0;
        const uint8_t *payload = fls_get_image_data(fls, chunks[i], &payload_len);
        if (!payload || payload_len == 0) continue;

        uint32_t blocks_needed = (uint32_t)((payload_len + ONENAND_BLOCK_SIZE - 1) / ONENAND_BLOCK_SIZE);
        if (total_written_blocks + blocks_needed > max_blocks) {
            fprintf(stderr, "[-] Warning: Chunk %zu (%zu bytes, %u blocks) exceeds partition remaining space (%u blocks)\n",
                    i + 1, payload_len, blocks_needed, max_blocks - total_written_blocks);
            blocks_needed = max_blocks - total_written_blocks;
            if (blocks_needed == 0) break;
        }

        /* Check NVRAM safety */
        if (cur_block >= ONENAND_PROTECTED_BLOCK_START || (cur_block + blocks_needed) > ONENAND_PROTECTED_BLOCK_START) {
            if (!s->config.force_dangerous_erase) {
                fprintf(stderr, "[-] ERROR: Chunk %zu write targets protected NVRAM calibration area (978..1023)!\n", i + 1);
                fls_close(fls);
                return -EPERM;
            }
        }

        printf("[*] Erasing %u block(s) for Chunk %zu (Blocks %u .. %u)...\n",
               blocks_needed, i + 1, cur_block, cur_block + blocks_needed - 1);
        if (flasher_ofls_erase(s, cur_block, blocks_needed) != 0) {
            fprintf(stderr, "[-] Erase failed for Chunk %zu\n", i + 1);
            fls_close(fls);
            return -1;
        }

        uint32_t total_pages = (uint32_t)((payload_len + ONENAND_PAGE_SIZE - 1) / ONENAND_PAGE_SIZE);
        uint8_t page_buf[ONENAND_PAGE_SIZE];

        struct timespec start_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        for (uint32_t pages_done = 0; pages_done < total_pages; pages_done++) {
            uint32_t blk = cur_block + (pages_done / ONENAND_PAGES_PER_BLOCK);
            uint32_t p = pages_done % ONENAND_PAGES_PER_BLOCK;

            size_t offset = (size_t)pages_done * ONENAND_PAGE_SIZE;
            memset(page_buf, 0xFF, sizeof(page_buf));

            size_t copy_sz = payload_len - offset;
            if (copy_sz > ONENAND_PAGE_SIZE) copy_sz = ONENAND_PAGE_SIZE;
            memcpy(page_buf, payload + offset, copy_sz);

            if (flasher_ofls_write_page(s, blk, p, page_buf, sizeof(page_buf)) != 0) {
                fprintf(stderr, "\n[-] Write failed at Chunk %zu, Block %u, Page %u\n", i + 1, blk, p);
                fls_close(fls);
                return -1;
            }

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - start_time.tv_sec) + (now.tv_nsec - start_time.tv_nsec) / 1e9;
            double speed_kb = (elapsed > 0.05) ? (((size_t)(pages_done + 1) * 2) / elapsed) : 0.0;
            int pct = (int)(((pages_done + 1) * 100ULL) / total_pages);
            printf("\r[*] Chunk %zu/%zu Progress: %3d%% (%u / %u pages, Block %u Page %u, %.1f KB/s)   ",
                   i + 1, n_chunks, pct, pages_done + 1, total_pages, blk, p, speed_kb);
            fflush(stdout);
        }
        printf("\n[+] Chunk %zu flashed successfully!\n", i + 1);

        cur_block += blocks_needed;
        total_written_blocks += blocks_needed;
    }

    fls_close(fls);
    printf("[+] %s flashed successfully (%u blocks total)!\n", label, total_written_blocks);
    return 0;
}

int flasher_flash(flasher_session_t *s) {
    if (!s) return -1;

    printf("\n================================================================================\n");
    printf("[*] Starting OpenIFWD Firmware Flasher for Samsung GT-S5222 (PMB8815)\n");
    printf("================================================================================\n");

    /* 1. Synchronize with Boot ROM, upload PSI with SHA-256 validation, start open_flasher agent */
    if (flasher_start_open_flasher(s) != 0) {
        fprintf(stderr, "[-] Failed to bootstrap Open Flasher agent on device\n");
        return -1;
    }

    /* 2. Query Flash ID to verify Samsung OneNAND connection and model */
    uint16_t mfr = 0, dev = 0;
    if (flasher_ofls_id(s, &mfr, &dev) != 0 || mfr != 0x00EC) {
        fprintf(stderr, "[-] Target flash is not a valid Samsung OneNAND (MFR: 0x%04X, DEV: 0x%04X)\n", mfr, dev);
        return -1;
    }

    /* 3. Initialize Security Configuration (Tag 15, 2048 bytes) from primary firmware container */
    const char *primary_fls = s->config.slb_path ? s->config.slb_path :
                             (s->config.fls_path ? s->config.fls_path :
                             (s->config.bsy_path ? s->config.bsy_path : s->config.psi_path));
    if (primary_fls) {
        flasher_init_sec_cfg(s, primary_fls);
    }

    /* 4. Execute requested flash operations */
    int flashed_count = 0;

    /* A. Dedicated SLB section FLS (--slb) -> Blocks 2..4 (Second Level Bootloader flashed FIRST) */
    if (s->config.slb_path) {
        if (flasher_flash_slb(s, s->config.slb_path) != 0) {
            return -1;
        }
        flashed_count++;
    }

    /* B. Dedicated CODE partition binary (--code) */
    if (s->config.code_path) {
        if (flasher_flash_binary_to_partition(s, s->config.code_path,
                                              ONENAND_PART_CODE_START,
                                              ONENAND_PART_CODE_BLOCKS, "CODE") != 0) {
            return -1;
        }
        flashed_count++;
    }

    /* C. Dedicated CSC partition binary (--csc) */
    if (s->config.csc_path) {
        if (flasher_flash_binary_to_partition(s, s->config.csc_path,
                                              ONENAND_PART_CSC_START,
                                              ONENAND_PART_CSC_BLOCKS, "CSC") != 0) {
            return -1;
        }
        flashed_count++;
    }

    /* D. Unified or Section FLS container (--fls) */
    if (s->config.fls_path && (!s->config.slb_path || strcmp(s->config.fls_path, s->config.slb_path) != 0)) {
        if (flasher_flash_fls_container(s, s->config.fls_path) != 0) {
            return -1;
        }
        flashed_count++;
    }

    /* E. BSY section FLS (--bsy) */
    if (s->config.bsy_path && (!s->config.fls_path || strcmp(s->config.bsy_path, s->config.fls_path) != 0)) {
        if (flasher_flash_fls_container(s, s->config.bsy_path) != 0) {
            return -1;
        }
        flashed_count++;
    }

    /* F. CDS section FLS (--cds) */
    if (s->config.cds_path && (!s->config.fls_path || strcmp(s->config.cds_path, s->config.fls_path) != 0)) {
        if (flasher_flash_fls_container(s, s->config.cds_path) != 0) {
            return -1;
        }
        flashed_count++;
    }

    /* G. DFFS / CSC section (--dffs) */
    if (!s->config.skip_csc && s->config.dffs_path && (!s->config.fls_path || strcmp(s->config.dffs_path, s->config.fls_path) != 0)) {
        if (flasher_flash_fls_container(s, s->config.dffs_path) != 0) {
            return -1;
        }
        flashed_count++;
    }

    if (flashed_count == 0) {
        fprintf(stderr, "[-] No firmware files specified to flash!\n");
        fprintf(stderr, "    Usage: ./openifwd flash -p usb --fls <firmware.fls>\n");
        fprintf(stderr, "    Or:    ./openifwd flash -p usb --code <code.bin> --csc <csc.bin>\n");
        return -1;
    }

    printf("\n================================================================================\n");
    printf("[+] Flashing complete! (%d component(s) successfully written)\n", flashed_count);
    printf("[*] Resetting and rebooting device...\n");
    printf("================================================================================\n");
    flasher_ofls_reset(s);

    return 0;
}

void flasher_hexdump(const uint8_t *data, size_t len, size_t max_lines) {
    if (!data || len == 0) return;
    size_t lines = 0;
    for (size_t i = 0; i < len; i += 16) {
        if (max_lines > 0 && lines >= max_lines) {
            printf("... (%zu bytes omitted, use -a / --all / --full or --lines %zu to display full page)\n",
                   len - i, (len + 15) / 16);
            break;
        }
        printf("%08zx  ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < len) {
                printf("%02x ", data[i + j]);
            } else {
                printf("   ");
            }
            if (j == 7) {
                printf(" ");
            }
        }
        printf(" |");
        for (size_t j = 0; j < 16 && (i + j) < len; j++) {
            uint8_t c = data[i + j];
            printf("%c", (c >= 32 && c <= 126) ? c : '.');
        }
        printf("|\n");
        lines++;
    }
}
