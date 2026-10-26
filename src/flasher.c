#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "flasher.h"
#include "open_psi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#define CHUNK_WRITE_SIZE 2048

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
            raw_data = fls_get_chunk_payload(psi_fls, chunk, &raw_size);
            if (raw_data && raw_size > 20) {
                uint32_t sub_len = *(const uint32_t *)(raw_data + 8);
                if (sub_len > 0 && sub_len <= raw_size - 20 && sub_len <= 0xFFFF) {
                    psi_data = raw_data + 20;
                    psi_size = (size_t)sub_len;
                }
            }
            if (!psi_data && raw_data && raw_size <= 0xFFFF) {
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
    if (!psi_data && s->config.psi_path && strlen(s->config.psi_path) > 0) {
        if (strstr(s->config.psi_path, ".fls") || strstr(s->config.psi_path, ".FLS")) {
            fls_file_t *pf = fls_open(s->config.psi_path);
            if (pf) {
                const fls_chunk_info_t *chunk = fls_find_chunk(pf, FLS_TAG_RPSI);
                if (!chunk) chunk = fls_find_chunk(pf, FLS_TAG_IMAGE_DATA);
                size_t raw_size = 0;
                const uint8_t *raw_data = NULL;
                if (chunk) {
                    raw_data = fls_get_chunk_payload(pf, chunk, &raw_size);
                    if (raw_data && raw_size > 20) {
                        uint32_t sub_len = *(const uint32_t *)(raw_data + 8);
                        if (sub_len > 0 && sub_len <= raw_size - 20 && sub_len <= 0xFFFF) {
                            allocated_psi = (uint8_t *)malloc(sub_len);
                            if (allocated_psi) {
                                memcpy(allocated_psi, raw_data + 20, sub_len);
                                psi_data = allocated_psi;
                                psi_size = sub_len;
                            }
                        }
                    }
                    if (!psi_data && raw_data && raw_size <= 0xFFFF) {
                        allocated_psi = (uint8_t *)malloc(raw_size);
                        if (allocated_psi) {
                            memcpy(allocated_psi, raw_data, raw_size);
                            psi_data = allocated_psi;
                            psi_size = raw_size;
                        }
                    }
                }
                fls_close(pf);
            }
        }
        if (!psi_data) {
            FILE *raw_psi_f = fopen(s->config.psi_path, "rb");
            if (raw_psi_f) {
                fseek(raw_psi_f, 0, SEEK_END);
                long sz = ftell(raw_psi_f);
                rewind(raw_psi_f);
                if (sz > 0 && sz <= 0xFFFF) {
                    allocated_psi = (uint8_t *)malloc((size_t)sz);
                    if (allocated_psi && fread(allocated_psi, 1, (size_t)sz, raw_psi_f) == (size_t)sz) {
                        psi_data = allocated_psi;
                        psi_size = (size_t)sz;
                    }
                }
                fclose(raw_psi_f);
            }
        }
        if (psi_data) {
            chk8 = 0;
            for (size_t i = 0; i < psi_size; i++) {
                chk8 ^= psi_data[i];
            }
            printf("[*] Loaded custom PSI binary from %s (%zu bytes, XOR-8 0x%02X)...\n",
                   s->config.psi_path, psi_size, chk8);
        }
    }

    if (!psi_data) {
        psi_data = open_psi_binary;
        psi_size = open_psi_size;
        chk8 = open_psi_checksum;
        printf("[*] Using built-in Open Rust PSI for ARM926EJ-S (%zu bytes, XOR-8 0x%02X)...\n", psi_size, chk8);
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

    /* Read Boot ROM ACK */
    uint8_t ack_buf[16] = {0};
    int r = transport_read(s->transport, ack_buf, sizeof(ack_buf), 2000);
    if (r <= 0) {
        r = transport_read(s->transport, ack_buf, sizeof(ack_buf), 2000);
    }

    if (r > 0 && (ack_buf[0] == 0x01 || ack_buf[0] == 0x0B || ack_buf[0] == 0xB1 || ack_buf[0] == 0xC1)) {
        printf("[+] PSI acknowledged by Boot ROM (%d byte(s): 0x%02X", r, ack_buf[0]);
        if (r > 1) printf(" 0x%02X", ack_buf[1]);
        printf(")\n");
        printf("[+] PSI successfully executed by Boot ROM!\n");
    } else {
        fprintf(stderr, "[-] Unexpected Boot ROM ACK (%d bytes, 0x%02X)\n", r, ack_buf[0]);
        if (allocated_psi) free(allocated_psi);
        return -1;
    }

    if (allocated_psi) free(allocated_psi);

    transport_flush(s->transport);
    usleep(250000);
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
    return openifwd_send_recv(s, OPENIFWD_CMD_PING, NULL, 0,
                              OPENIFWD_RSP_PONG, &status_flags, &resp_len, 3000);
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
    uint8_t payload[4];
    memcpy(payload, &block, sizeof(block));
    return openifwd_send_recv(s, OPENIFWD_CMD_ERASE_BLOCK, payload, sizeof(payload),
                              OPENIFWD_RSP_OK, NULL, NULL, 5000);
}

int flasher_open_write_page(flasher_session_t *s, uint32_t block, uint16_t page, const uint8_t *data, const uint8_t *spare) {
    if (!data) return -1;
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

int flasher_dump_nvram(flasher_session_t *s, const char *out_path) {
    if (!s || !out_path) return -1;

    printf("\n================================================================================\n");
    printf("[*] Starting Safe OpenIFWD NVRAM / Calibration Backup\n");
    printf("[*] Target Blocks: 4..11 (1 MB / 512 OneNAND pages)\n");
    printf("[*] Output File:   %s\n", out_path);
    printf("================================================================================\n");

    /* 1. Sync handshake with Boot ROM */
    if (flasher_sync(s, 40) != 0) return -1;

    /* 2. Upload Open PSI into SRAM */
    if (flasher_upload_psi(s, NULL) != 0) return -1;

    /* Settle and ping */
    usleep(250000);
    printf("[*] Connecting to OpenIFWD agent on device...\n");
    if (flasher_open_ping(s) != 0) {
        fprintf(stderr, "[-] Target agent ping failed\n");
        return -1;
    }
    printf("[+] Target agent connected and ready!\n");

    openifwd_hw_info_t hw;
    if (flasher_open_get_hw_info(s, &hw) == 0) {
        printf("[+] Hardware Info: ChipID=0x%02X, CPU=%u MHz, SDRAM=%u MB, Flash=%u MB\n",
               hw.chip_id, hw.cpu_freq_hz / 1000000, hw.sdram_size / (1024*1024),
               (hw.total_blocks * hw.block_size) / (1024*1024));
    }

    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        fprintf(stderr, "[-] Failed to open output file: %s (%s)\n", out_path, strerror(errno));
        return -1;
    }

    const uint32_t start_block = 4;
    const uint32_t end_block = 11;
    const uint16_t pages_per_block = 64;
    size_t total_pages = (end_block - start_block + 1) * pages_per_block;
    size_t dumped_pages = 0;
    uint8_t page_buf[2048];

    for (uint32_t b = start_block; b <= end_block; b++) {
        for (uint16_t p = 0; p < pages_per_block; p++) {
            if (flasher_open_read_page(s, b, p, page_buf, NULL) != 0) {
                fprintf(stderr, "\n[-] Read failed at Block %u, Page %u\n", b, p);
                fclose(fout);
                return -1;
            }
            if (fwrite(page_buf, 1, 2048, fout) != 2048) {
                fprintf(stderr, "\n[-] Write error to output file\n");
                fclose(fout);
                return -1;
            }
            dumped_pages++;
            int pct = (int)((dumped_pages * 100) / total_pages);
            printf("\r[*] NVRAM Backup Progress: %3d%% (%zu / %zu pages, %zu KB)",
                   pct, dumped_pages, total_pages, (dumped_pages * 2048) / 1024);
            fflush(stdout);
        }
    }
    printf("\n[+] NVRAM calibration backup successfully saved to %s (1,048,576 bytes)!\n", out_path);
    fclose(fout);
    return 0;
}

int flasher_boot_ram(flasher_session_t *s, uint32_t ram_addr, const uint8_t *data, size_t size) {
    if (!s || !data || size == 0) return -1;

    /* 1. Sync handshake with Boot ROM */
    if (flasher_sync(s, 40) != 0) return -1;

    /* 2. Upload Open PSI into SRAM */
    if (flasher_upload_psi(s, NULL) != 0) return -1;

    /* Settle and ping */
    usleep(250000);
    printf("[*] Connecting to OpenIFWD agent on device...\n");
    if (flasher_open_ping(s) != 0) {
        fprintf(stderr, "[-] Target agent ping failed\n");
        return -1;
    }
    printf("[+] Target agent connected and ready!\n");

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

static int flash_fls_file(flasher_session_t *s, const char *fls_path, const char *label) {
    if (!fls_path || access(fls_path, R_OK) != 0) return 0;

    printf("[*] Preparing %s: %s\n", label, fls_path);
    fls_file_t *fls = fls_open(fls_path);
    if (!fls) {
        fprintf(stderr, "[-] Failed to parse FLS: %s\n", fls_path);
        return -1;
    }

    const fls_chunk_info_t *chunk = fls_find_chunk(fls, FLS_TAG_IMAGE_DATA);
    if (!chunk) chunk = fls_find_chunk(fls, FLS_TAG_PARTITION_TABLE);

    if (!chunk) {
        fprintf(stderr, "[-] No flashable chunk in %s\n", fls_path);
        fls_close(fls);
        return -1;
    }

    size_t payload_len = 0;
    const uint8_t *payload = fls_get_chunk_payload(fls, chunk, &payload_len);
    if (!payload || payload_len == 0) {
        fprintf(stderr, "[-] Empty payload in %s\n", fls_path);
        fls_close(fls);
        return -1;
    }

    uint32_t target_addr = 0x60000000;
    for (size_t i = 0; i < fls->toc_count; i++) {
        if (fls->toc_entries[i].tag_id == chunk->tag && fls->toc_entries[i].load_address != 0) {
            target_addr = fls->toc_entries[i].load_address;
            break;
        }
    }

    /* Verify NVRAM safety */
    if (!is_flash_range_safe(target_addr, (uint32_t)payload_len)) {
        fprintf(stderr, "[!] CRITICAL: Target address 0x%08X overlaps protected NVRAM! Aborting.\n", target_addr);
        fls_close(fls);
        return -1;
    }

    const uint32_t block_size = 128 * 1024;
    const uint32_t page_size = 2048;
    const uint16_t pages_per_block = 64;

    uint32_t start_block = (target_addr - 0x60000000) / block_size;
    uint32_t num_blocks = (uint32_t)((payload_len + block_size - 1) / block_size);

    printf("[*] Flashing %s: %zu bytes -> Blocks %u..%u\n", label, payload_len, start_block, start_block + num_blocks - 1);

    for (uint32_t b = 0; b < num_blocks; b++) {
        uint32_t blk = start_block + b;
        if (blk >= NVRAM_BLOCK_START && blk <= NVRAM_BLOCK_END) {
            fprintf(stderr, "[!] Attempted to write protected block %u! Skipping.\n", blk);
            continue;
        }

        if (flasher_open_erase_block(s, blk) != 0) {
            fprintf(stderr, "\n[-] Block erase failed: %u\n", blk);
            fls_close(fls);
            return -1;
        }

        for (uint16_t p = 0; p < pages_per_block; p++) {
            size_t offset = (size_t)b * block_size + (size_t)p * page_size;
            if (offset >= payload_len) break;

            uint8_t pbuf[2048];
            memset(pbuf, 0xFF, sizeof(pbuf));
            size_t copy_sz = payload_len - offset;
            if (copy_sz > 2048) copy_sz = 2048;
            memcpy(pbuf, payload + offset, copy_sz);

            if (flasher_open_write_page(s, blk, p, pbuf, NULL) != 0) {
                fprintf(stderr, "\n[-] Page write failed: Block %u, Page %u\n", blk, p);
                fls_close(fls);
                return -1;
            }
        }

        int pct = (int)(((b + 1) * 100) / num_blocks);
        printf("\r[*] Progress: %3d%% (%u / %u blocks)", pct, b + 1, num_blocks);
        fflush(stdout);
    }

    printf("\n[+] %s flashed successfully!\n", label);
    fls_close(fls);
    return 0;
}

int flasher_flash(flasher_session_t *s) {
    if (!s) return -1;

    printf("\n================================================================================\n");
    printf("[*] Starting OpenIFWD Firmware Flashing for Samsung Star 3 (GT-S5222)\n");
    printf("================================================================================\n");

    /* 1. Sync handshake with Boot ROM */
    if (flasher_sync(s, 40) != 0) return -1;

    /* 2. Upload Open PSI into SRAM */
    if (flasher_upload_psi(s, NULL) != 0) return -1;

    /* Settle and ping */
    usleep(250000);
    printf("[*] Connecting to OpenIFWD agent on device...\n");
    if (flasher_open_ping(s) != 0) {
        fprintf(stderr, "[-] Target agent ping failed\n");
        return -1;
    }
    printf("[+] Target agent connected and ready!\n");

    openifwd_hw_info_t hw;
    if (flasher_open_get_hw_info(s, &hw) == 0) {
        printf("[+] Hardware Info: ChipID=0x%02X, CPU=%u MHz, SDRAM=%u MB, Flash=%u MB\n",
               hw.chip_id, hw.cpu_freq_hz / 1000000, hw.sdram_size / (1024*1024),
               (hw.total_blocks * hw.block_size) / (1024*1024));
    }

    if (s->config.bsy_path) {
        if (flash_fls_file(s, s->config.bsy_path, "Baseband System (BSY)") != 0) return -1;
    }
    if (s->config.cds_path) {
        if (flash_fls_file(s, s->config.cds_path, "Customer Data (CDS)") != 0) return -1;
    }
    if (!s->config.skip_csc && s->config.dffs_path) {
        if (flash_fls_file(s, s->config.dffs_path, "Customization / DFFS (CSC)") != 0) return -1;
    }

    printf("[+] Flashing complete! Rebooting device...\n");
    flasher_open_reboot(s);
    return 0;
}
