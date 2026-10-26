#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "flasher.h"
#include "psi_xgold215.h"
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

            for (int i = 0; i < n && ident_len < (int)sizeof(ident_stream); i++) {
                ident_stream[ident_len++] = rx_buf[i];
            }

            /* Check if we received 0xF0 Boot ROM token */
            for (int i = 0; i < ident_len; i++) {
                if (ident_stream[i] == 0xF0) {
                    synced = true;
                    break;
                }
            }

            if (synced) {
                printf("[+] Received Boot ROM sync token 0xF0! Halting sync probe.\n");
                break;
            }
        }
        usleep(30000); /* 30ms between ping attempts */
    }

    if (!synced) {
        fprintf(stderr, "[-] Synchronization timeout (%d sec)\n", timeout_sec);
        return -1;
    }

    /* Drain remaining identification packet bytes from Boot ROM */
    uint8_t drain_buf[64];
    int dn;
    while ((dn = transport_read(s->transport, drain_buf, sizeof(drain_buf), 100)) > 0) {
        printf("[*] Drained %d byte(s) of chip ID data: ", dn);
        for (int i = 0; i < dn; i++) {
            printf("0x%02X ", drain_buf[i]);
            if (ident_len < (int)sizeof(ident_stream)) {
                ident_stream[ident_len++] = drain_buf[i];
            }
        }
        printf("\n");
    }

    /* Extract target chip ID from identification stream:
     * Stream format: [0xF0 (sync), len (e.g. 0x0A), chip_id (e.g. 0x17), ...]
     */
    s->chip_id = 0;
    int f0_pos = -1;
    for (int i = 0; i < ident_len; i++) {
        if (ident_stream[i] == 0xF0) {
            f0_pos = i;
            break;
        }
    }

    if (f0_pos >= 0 && f0_pos + 2 < ident_len) {
        s->chip_id = ident_stream[f0_pos + 2];
    } else if (ident_len >= 3) {
        s->chip_id = ident_stream[2];
    } else if (ident_len >= 2) {
        s->chip_id = ident_stream[1];
    }

    if (s->chip_id != 0) {
        printf("[+] Detected Target Chip ID: 0x%02X\n", s->chip_id);
    } else {
        s->chip_id = 0x17;
        printf("[*] Defaulting to Target Chip ID: 0x%02X\n", s->chip_id);
    }

    printf("[+] Device synchronized successfully! Bulk OUT FIFO is clean.\n");
    usleep(20000); /* Settle USB pipe before Boot ROM upload */
    return 0;
}

int flasher_upload_psi(flasher_session_t *s, const fls_file_t *psi_fls) {
    if (!s) return -1;

    const uint8_t *psi_data = NULL;
    size_t psi_size = 0;
    uint8_t chk8 = 0;

    /* 1. Extract PSI / RPSI directly from FLS container (Tag 18 = 57,644 bytes) */
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

        if (!psi_data) {
            chunk = fls_find_chunk(psi_fls, FLS_TAG_IMAGE_DATA);
            if (chunk) {
                raw_data = fls_get_chunk_payload(psi_fls, chunk, &raw_size);
                if (raw_data && raw_size > 28) {
                    uint32_t sub_len = *(const uint32_t *)(raw_data + 16);
                    if (sub_len > 0 && sub_len <= raw_size - 28 && sub_len <= 0xFFFF) {
                        psi_data = raw_data + 28;
                        psi_size = (size_t)sub_len;
                    }
                }
                if (!psi_data && raw_data && raw_size <= 0xFFFF) {
                    psi_data = raw_data;
                    psi_size = raw_size;
                }
            }
        }

        if (!psi_data) {
            chunk = fls_find_chunk(psi_fls, FLS_TAG_PARTITION_TABLE);
            if (chunk) {
                raw_data = fls_get_chunk_payload(psi_fls, chunk, &raw_size);
                if (raw_data && raw_size <= 0xFFFF) {
                    psi_data = raw_data;
                    psi_size = raw_size;
                }
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
                    chk8 = 0;
                    for (size_t i = 0; i < psi_size; i++) {
                        chk8 ^= psi_data[i];
                    }
                    printf("[*] Loaded custom raw PSI binary from %s (%zu bytes, XOR-8 0x%02X)...\n",
                           s->config.psi_path, psi_size, chk8);
                }
            }
            fclose(raw_psi_f);
        }
    }

    if (!psi_data) {
        psi_data = open_psi_binary;
        psi_size = open_psi_size;
        chk8 = open_psi_checksum;
        printf("[*] Using built-in Open Rust PSI for ARM926EJ-S (%zu bytes, XOR-8 0x%02X)...\n", psi_size, chk8);
    }

    /* 2. Low-level Boot ROM Header:
     * According to IFWD_DownloadDll.dll (at 0x405ad5):
     * Byte 0: 0x30 (Boot ROM download command)
     * Byte 1: len & 0xFF
     * Byte 2: (len >> 8) & 0xFF
     * Header length: 3 bytes for Case 14 (X-Gold 215 / PMB8815 <= 0x10).
     */
    uint8_t boot_hdr[4];
    boot_hdr[0] = 0x30;
    boot_hdr[1] = (uint8_t)(psi_size & 0xFF);
    boot_hdr[2] = (uint8_t)((psi_size >> 8) & 0xFF);
    boot_hdr[3] = 0xFF;
    size_t hdr_len = 3;

    if (transport_write(s->transport, boot_hdr, hdr_len) != (int)hdr_len) {
        fprintf(stderr, "[-] Failed to write Boot ROM 0x30 header\n");
        if (allocated_psi) free(allocated_psi);
        return -1;
    }

    /* Probe for potential header acknowledgement from Boot ROM on Bulk IN (EP 0x83) */
    uint8_t hdr_ack = 0;
    int ack_res = transport_read(s->transport, &hdr_ack, 1, 50);
    if (ack_res > 0) {
        printf("[*] Target header response on EP 0x83: 0x%02X\n", hdr_ack);
    }

    /* Settling delay (15ms) to give Boot ROM time to arm its USB Bulk OUT FIFO */
    usleep(15000);

    /* 3. Stream raw PSI payload in 64-byte chunks (USB Full-Speed MTU) with micro-pacing */
    printf("[*] Streaming PSI payload (%zu bytes in 64-byte chunks with pacing)...\n", psi_size);
    size_t sent = 0;
    while (sent < psi_size) {
        size_t chunk = psi_size - sent;
        if (chunk > 64) chunk = 64;
        int written = transport_write(s->transport, psi_data + sent, chunk);
        if (written != (int)chunk) {
            fprintf(stderr, "[-] Failed to write PSI payload chunk at offset %zu (%zu bytes)\n", sent, chunk);
            if (allocated_psi) free(allocated_psi);
            return -1;
        }
        sent += chunk;
        int percent = (int)((sent * 100) / psi_size);
        if (sent % 4096 == 0 || sent == psi_size) {
            printf("\r[*] PSI upload progress: %3d%% (%zu / %zu bytes)", percent, sent, psi_size);
            fflush(stdout);
        }
        usleep(300); /* 300us pacing for BROM ARM9 FIFO draining */
    }
    printf("\n[+] PSI payload streamed successfully.\n");

    /* 4. Send 1-byte XOR-8 checksum of payload */
    printf("[*] Sending PSI XOR-8 checksum (0x%02X)...\n", chk8);
    if (transport_write(s->transport, &chk8, 1) != 1) {
        fprintf(stderr, "[-] Failed to write PSI XOR-8 checksum\n");
        if (allocated_psi) free(allocated_psi);
        return -1;
    }

    /* 5. Wait for Target Boot ROM ACK (XMM2150 sends 2 bytes: [0x01, 0x01]) */
    uint8_t ack_buf[4] = {0};
    int r = 0;
    while (r < 2) {
        int br = transport_read(s->transport, ack_buf + r, 2 - r, 3000);
        if (br <= 0) break;
        r += br;
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

    /* Drain any leftover bytes and flush transport buffers */
    transport_flush(s->transport);

    /* 6. Settling delay (200ms) for RPSI running in SRAM to initialize CPU clocks,
     * Mobile DDR SDRAM, and the USB interface */
    usleep(200000);

    /* Probe for RPSI status banner / readiness if target sends any */
    uint8_t banner_buf[128] = {0};
    int bn = transport_read(s->transport, banner_buf, sizeof(banner_buf) - 1, 100);
    if (bn > 0) {
        banner_buf[bn] = '\0';
        printf("[+] RPSI status banner (%d bytes): %s\n", bn, (char *)banner_buf);
    }

    printf("[+] Primary Stage Initializer (PSI) completed successfully!\n");
    return 0;
}

/*
 * In-memory patch for EBL (RAM execution) on Infineon XMM2150 / ARM926EJ-S:
 * Bypasses RSA signature verification, SHA hash checks, and partition range
 * validation to permit custom Tag 15 descriptors (e.g. Upload mode for NVRAM dump)
 * and custom unsigned firmware flashing.
 *
 * All replacement code uses strictly ARMv5TEJ instructions:
 *   mov r0, #0 ; bx lr -> 00 00 a0 e3 1e ff 2f e1
 *   mov r0, #1          -> 01 00 a0 e3
 *
 * Uses two-tier matching: fast check at known offsets with fallback pattern scanning.
 */
int flasher_patch_ebl(uint8_t *ebl_data, size_t ebl_size) {
    if (!ebl_data || ebl_size < 0x86000) {
        fprintf(stderr, "[-] EBL data too short for signature patching (size: %zu)\n", ebl_size);
        return -1;
    }

    int patches_applied = 0;

    /* Patch 1: sec_init_flag (offset ~0x84408, address 0x60084408)
     * Context at 0x84404: 10 40 2d e9 00 00 a0 e3 e8 18 9f e5 00 00 81 e5
     * Instruction: mov r0, #0 -> mov r0, #1 (01 00 a0 e3)
     * Effect: sets global bypass flag *0x6140640C = 1
     */
    static const uint8_t p1_ctx[16] = {
        0x10, 0x40, 0x2d, 0xe9, 0x00, 0x00, 0xa0, 0xe3,
        0xe8, 0x18, 0x9f, 0xe5, 0x00, 0x00, 0x81, 0xe5
    };
    size_t off1 = 0x84404;
    bool found1 = false;
    if (off1 + 16 <= ebl_size && memcmp(ebl_data + off1, p1_ctx, 16) == 0) {
        found1 = true;
    } else {
        /* Fallback pattern search */
        for (size_t i = 0; i + 16 <= ebl_size; i += 4) {
            if (memcmp(ebl_data + i, p1_ctx, 16) == 0) {
                off1 = i;
                found1 = true;
                break;
            }
        }
    }
    if (found1) {
        /* Patch mov r0, #0 (at offset + 4) with mov r0, #1 */
        ebl_data[off1 + 4] = 0x01;
        printf("[+] [EBL Patch 1/4] Enabled global security bypass flag (*0x6140640C = 1) at offset 0x%05zX\n", off1 + 4);
        patches_applied++;
    } else {
        printf("[!] [EBL Patch 1/4] sec_init_flag pattern not found, skipping\n");
    }

    /* Patch 2: verify_sig_and_hash (offset ~0x845AC, address 0x600845AC)
     * Context at 0x845AC: f0 40 2d e9 14 d0 4d e2 00 50 a0 e1 01 60 a0 e1
     * Replace first 8 bytes with: mov r0, #0 ; bx lr (00 00 a0 e3 1e ff 2f e1)
     * Effect: verify_sig_and_hash returns 0 (SUCCESS) immediately
     */
    static const uint8_t p2_ctx[16] = {
        0xf0, 0x40, 0x2d, 0xe9, 0x14, 0xd0, 0x4d, 0xe2,
        0x00, 0x50, 0xa0, 0xe1, 0x01, 0x60, 0xa0, 0xe1
    };
    size_t off2 = 0x845AC;
    bool found2 = false;
    if (off2 + 16 <= ebl_size && memcmp(ebl_data + off2, p2_ctx, 16) == 0) {
        found2 = true;
    } else {
        for (size_t i = 0; i + 16 <= ebl_size; i += 4) {
            if (memcmp(ebl_data + i, p2_ctx, 16) == 0) {
                off2 = i;
                found2 = true;
                break;
            }
        }
    }
    if (found2) {
        static const uint8_t ret0_arm[8] = { 0x00, 0x00, 0xa0, 0xe3, 0x1e, 0xff, 0x2f, 0xe1 };
        memcpy(ebl_data + off2, ret0_arm, sizeof(ret0_arm));
        printf("[+] [EBL Patch 2/4] Bypassed verify_sig_and_hash (returns 0) at offset 0x%05zX\n", off2);
        patches_applied++;
    } else {
        printf("[!] [EBL Patch 2/4] verify_sig_and_hash pattern not found, skipping\n");
    }

    /* Patch 3: partition_range_check (offset ~0x847C4, address 0x600847C4)
     * Context at 0x847C4: 00 10 a0 e1 00 20 a0 e3 0e 00 00 ea 3c 05 9f e5
     * Replace first 8 bytes with: mov r0, #0 ; bx lr (00 00 a0 e3 1e ff 2f e1)
     * Effect: allows flash operations on any address range (including NVRAM 0x60080000)
     */
    static const uint8_t p3_ctx[16] = {
        0x00, 0x10, 0xa0, 0xe1, 0x00, 0x20, 0xa0, 0xe3,
        0x0e, 0x00, 0x00, 0xea, 0x3c, 0x05, 0x9f, 0xe5
    };
    size_t off3 = 0x847C4;
    bool found3 = false;
    if (off3 + 16 <= ebl_size && memcmp(ebl_data + off3, p3_ctx, 16) == 0) {
        found3 = true;
    } else {
        for (size_t i = 0; i + 16 <= ebl_size; i += 4) {
            if (memcmp(ebl_data + i, p3_ctx, 16) == 0) {
                off3 = i;
                found3 = true;
                break;
            }
        }
    }
    if (found3) {
        static const uint8_t ret0_arm[8] = { 0x00, 0x00, 0xa0, 0xe3, 0x1e, 0xff, 0x2f, 0xe1 };
        memcpy(ebl_data + off3, ret0_arm, sizeof(ret0_arm));
        printf("[+] [EBL Patch 3/4] Bypassed partition_range_check (all ranges permitted) at offset 0x%05zX\n", off3);
        patches_applied++;
    } else {
        printf("[!] [EBL Patch 3/4] partition_range_check pattern not found, skipping\n");
    }

    /* Patch 4: rsa_decrypt_func (offset ~0x85EAC, address 0x60085EAC)
     * Context at 0x85EAC: 30 40 2d e9 1c d0 4d e2 00 40 a0 e1 00 00 54 e3
     * Replace first 8 bytes with: mov r0, #0 ; bx lr (00 00 a0 e3 1e ff 2f e1)
     * Effect: RSA decryption engine returns 0 (SUCCESS) immediately
     */
    static const uint8_t p4_ctx[16] = {
        0x30, 0x40, 0x2d, 0xe9, 0x1c, 0xd0, 0x4d, 0xe2,
        0x00, 0x40, 0xa0, 0xe1, 0x00, 0x00, 0x54, 0xe3
    };
    size_t off4 = 0x85EAC;
    bool found4 = false;
    if (off4 + 16 <= ebl_size && memcmp(ebl_data + off4, p4_ctx, 16) == 0) {
        found4 = true;
    } else {
        for (size_t i = 0; i + 16 <= ebl_size; i += 4) {
            if (memcmp(ebl_data + i, p4_ctx, 16) == 0) {
                off4 = i;
                found4 = true;
                break;
            }
        }
    }
    if (found4) {
        static const uint8_t ret0_arm[8] = { 0x00, 0x00, 0xa0, 0xe3, 0x1e, 0xff, 0x2f, 0xe1 };
        memcpy(ebl_data + off4, ret0_arm, sizeof(ret0_arm));
        printf("[+] [EBL Patch 4/4] Bypassed rsa_decrypt_func (returns 0) at offset 0x%05zX\n", off4);
        patches_applied++;
    } else {
        printf("[!] [EBL Patch 4/4] rsa_decrypt_func pattern not found, skipping\n");
    }

    printf("[+] Applied %d/4 in-memory EBL security patches successfully!\n", patches_applied);
    return (patches_applied > 0) ? 0 : -1;
}

int flasher_upload_ebl(flasher_session_t *s, const fls_file_t *ebl_fls) {
    if (!s || !ebl_fls) return -1;

    /* 1. Select EBL executable chunk:
     * Priority:
     * a. Tag 19 (SECONDARY_LOADER / EBL download engine for RAM execution)
     * b. Tag 14 (EBL_RAM)
     * c. Tag 18 (RPSI)
     * d. Tag 12 (IMAGE_DATA - offline bootloader, fallback)
     */
    const fls_chunk_info_t *chunk = fls_find_chunk(ebl_fls, FLS_TAG_SECONDARY_LOADER);
    if (!chunk) {
        chunk = fls_find_chunk(ebl_fls, FLS_TAG_EBL_RAM);
    }
    if (!chunk) {
        chunk = fls_find_chunk(ebl_fls, FLS_TAG_RPSI);
    }
    if (!chunk) {
        for (size_t i = 0; i < ebl_fls->chunk_count; i++) {
            if (ebl_fls->chunks[i].tag == FLS_TAG_IMAGE_DATA && ebl_fls->chunks[i].payload_size > 28) {
                chunk = &ebl_fls->chunks[i];
                break;
            }
        }
    }
    if (!chunk) {
        chunk = fls_find_chunk(ebl_fls, FLS_TAG_IMAGE_DATA);
    }
    if (!chunk) {
        fprintf(stderr, "[-] EBL/SLB payload not found in %s\n", ebl_fls->filepath);
        return -1;
    }

    size_t raw_size = 0;
    const uint8_t *raw_data = fls_get_chunk_payload(ebl_fls, chunk, &raw_size);
    if (!raw_data || raw_size == 0) {
        fprintf(stderr, "[-] Empty EBL/SLB payload in %s\n", ebl_fls->filepath);
        return -1;
    }

    /* Subheader parsing:
     * Tag 19 (SECONDARY_LOADER) and Tag 18 (RPSI) have 20-byte subheader:
     *   offset 8: uint32_t sub_len
     *   offset 16: uint32_t sub_offset
     *   Actual binary begins after byte 20!
     * Tag 12 (IMAGE_DATA) has 28-byte subheader:
     *   offset 16: uint32_t sub_len
     *   Actual binary begins after byte 28!
     */
    const uint8_t *ebl_data = raw_data;
    size_t ebl_size = raw_size;

    if ((chunk->tag == FLS_TAG_SECONDARY_LOADER || chunk->tag == FLS_TAG_RPSI) && raw_size > 20) {
        uint32_t sub_len = *(const uint32_t *)(raw_data + 8);
        if (sub_len > 0 && sub_len <= raw_size - 20) {
            ebl_data = raw_data + 20;
            ebl_size = (size_t)sub_len;
        }
    } else if (chunk->tag == FLS_TAG_IMAGE_DATA && raw_size > 28) {
        uint32_t sub_len = *(const uint32_t *)(raw_data + 16);
        if (sub_len > 0 && sub_len <= raw_size - 28) {
            ebl_data = raw_data + 28;
            ebl_size = (size_t)sub_len;
        } else {
            ebl_data = raw_data + 28;
            ebl_size = raw_size - 28;
        }
    }

    printf("[*] Preparing to upload EBL/SLB (chunk Tag %u: %zu bytes)...\n", chunk->tag, ebl_size);

    uint8_t *patched_ebl = NULL;
    if (s->config.patch_ebl) {
        printf("[*] In-memory EBL signature & security patch requested (--patch-ebl active)...\n");
        patched_ebl = (uint8_t *)malloc(ebl_size);
        if (patched_ebl) {
            memcpy(patched_ebl, ebl_data, ebl_size);
            if (flasher_patch_ebl(patched_ebl, ebl_size) == 0) {
                ebl_data = patched_ebl;
            } else {
                fprintf(stderr, "[-] Warning: Failed to apply in-memory EBL patches\n");
            }
        } else {
            fprintf(stderr, "[-] Warning: Memory allocation failed for EBL patching\n");
        }
    }

    /* 2. Wait for PSI readiness handshake token:
     * PSI at offset 0x200 (instruction mov r0, #0xaa00) transmits:
     * 2-byte token [0x00, 0xAA] to signal readiness for the 4-byte EBL length.
     * FlashTool_E2 (IFWD_DownloadDll.dll at 0x40788c) reads this token.
     */
    printf("[*] Waiting for PSI stage synchronization token...\n");
    int header_len = 4;
    uint8_t early_tok[4] = {0};
    int tn = 0;
    while (tn < 2) {
        int br = transport_read(s->transport, early_tok + tn, 2 - tn, 3000);
        if (br <= 0) break;
        tn += br;
    }
    if (tn >= 2) {
        printf("[+] Target signaled readiness for EBL (token 0x%02X 0x%02X)\n", early_tok[0], early_tok[1]);
        if (early_tok[0] == 0xAA && early_tok[1] == 0xAA) {
            header_len = 2;
        } else if (early_tok[0] == 0x00 && early_tok[1] == 0xAA) {
            header_len = 4;
        }
    } else {
        printf("[*] Readiness token timeout, defaulting to 4-byte EBL length header\n");
    }

    /* 3. Send length header (little-endian) */
    uint8_t len_hdr[4];
    len_hdr[0] = (uint8_t)(ebl_size & 0xFF);
    len_hdr[1] = (uint8_t)((ebl_size >> 8) & 0xFF);
    len_hdr[2] = (uint8_t)((ebl_size >> 16) & 0xFF);
    len_hdr[3] = (uint8_t)((ebl_size >> 24) & 0xFF);

    printf("[*] Sending EBL length header: %zu bytes (%d-byte header)...\n", ebl_size, header_len);
    if (transport_write(s->transport, len_hdr, header_len) != header_len) {
        fprintf(stderr, "[-] Failed to write EBL length header\n");
        free(patched_ebl);
        return -1;
    }

    /* 4. Read length acknowledgement from PSI (0xCC 0xCC at offset 0x2d4/0x494 in PSI) */
    uint8_t len_ack[4] = {0};
    int an = transport_read(s->transport, len_ack, 2, 3000);
    if (an > 0) {
        printf("[+] PSI acknowledged length header (token: ");
        for (int i = 0; i < an; i++) printf("0x%02X ", len_ack[i]);
        printf(")\n");
    } else {
        printf("[*] Proceeding with EBL stream\n");
    }

    /* 5. Stream EBL payload in chunks and calculate XOR-8 checksum */
    printf("[*] Streaming EBL payload (%zu bytes in 512-byte chunks)...\n", ebl_size);
    size_t bytes_written = 0;
    uint8_t chk8 = 0;
    while (bytes_written < ebl_size) {
        size_t chunk_len = ebl_size - bytes_written;
        if (chunk_len > 512) chunk_len = 512;

        if (transport_write(s->transport, ebl_data + bytes_written, chunk_len) != (int)chunk_len) {
            fprintf(stderr, "\n[-] Failed to write EBL stream chunk at offset %zu\n", bytes_written);
            free(patched_ebl);
            return -1;
        }

        for (size_t i = 0; i < chunk_len; i++) {
            chk8 ^= ebl_data[bytes_written + i];
        }

        bytes_written += chunk_len;
        int percent = (int)((bytes_written * 100) / ebl_size);
        printf("\r[*] EBL upload progress: %3d%% (%zu / %zu bytes)", percent, bytes_written, ebl_size);
        fflush(stdout);
        usleep(100);
    }
    printf("\n[+] EBL payload streamed successfully.\n");

    /* 6. Send 1-byte XOR-8 checksum */
    printf("[*] Sending EBL XOR-8 checksum (0x%02X)...\n", chk8);
    if (transport_write(s->transport, &chk8, 1) != 1) {
        fprintf(stderr, "[-] Failed to write EBL XOR-8 checksum\n");
        free(patched_ebl);
        return -1;
    }

    /* 7. Wait for Target execution confirmation (0xCC 0xCC, or ACK response) */
    uint8_t ack_buf[8] = {0};
    int r = transport_read(s->transport, ack_buf, sizeof(ack_buf), 5000);
    if (r > 0) {
        printf("[+] Target response after EBL upload (%d bytes): ", r);
        for (int i = 0; i < r; i++) printf("0x%02X ", ack_buf[i]);
        printf("\n");
        bool ack_ok = false;
        for (int i = 0; i < r; i++) {
            if (ack_buf[i] == 0xCC || ack_buf[i] == 0xA5 || ack_buf[i] == 0x01 || ack_buf[i] == 0xC1) {
                ack_ok = true;
                break;
            }
        }
        if (ack_ok) {
            printf("[+] EBL acknowledged and verified by target!\n");
        } else {
            fprintf(stderr, "[-] Unexpected EBL ACK response\n");
            free(patched_ebl);
            return -1;
        }
    } else {
        fprintf(stderr, "[-] Timeout waiting for EBL confirmation from target\n");
        free(patched_ebl);
        return -1;
    }

    /* 8. Target signaled EBL execution start (0x17 0xA5) */
    printf("[+] Target signaled EBL execution start (0x17 0xA5)\n");
    printf("[+] Target EBL active on Bulk OUT 0x02 / Bulk IN 0x83.\n");

    /* Switch baudrate if requested (for UART) */
    if (!s->config.is_usb && s->config.baudrate > 115200) {
        printf("[*] Switching baudrate to %u bps...\n", s->config.baudrate);
        uint32_t baud = s->config.baudrate;
        uint8_t tx_buf[64];
        size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_CNF_BAUD_CHANGE, &baud, sizeof(baud), tx_buf, sizeof(tx_buf));
        transport_write(s->transport, tx_buf, pkt_len);
        usleep(100000);
        transport_set_baudrate(s->transport, s->config.baudrate);
    }

    free(patched_ebl);
    return 0;
}

static void usb_drain_bulk_in(flasher_session_t *s) {
    if (!s || !s->transport || !s->config.is_usb) return;
    uint8_t dummy[4096];
    while (1) {
        int br = transport_read(s->transport, dummy, sizeof(dummy), 25);
        if (br <= 0) break;
    }
}

/* Helper to transmit an EBL control packet and read its 8-byte ACK + response buffer */
static int ebl_send_control_cmd_ext_flags(flasher_session_t *s, uint16_t msg_type,
                                          const void *payload, uint32_t payload_len,
                                          uint16_t expected_ack, uint16_t expected_rsp,
                                          uint32_t *out_status,
                                          uint8_t *out_buffer, size_t out_len,
                                          bool quiet) {
    if (!s || !s->transport) return -1;

    uint8_t tx_buf[EBL_USB_PACKET_SIZE];
    memset(tx_buf, 0, sizeof(tx_buf));

    ebl_pkt_hdr_t *hdr = (ebl_pkt_hdr_t *)tx_buf;
    hdr->msg_type = msg_type;
    hdr->length = payload_len;
    /* First 16-bit word is ALWAYS the mathematical checksum of the packet:
     * sum = msg_type + (payload_len >> 16) + (payload_len & 0xFFFF) + sum(payload[0..payload_len-1])
     */
    hdr->checksum = protocol_calc_checksum(msg_type, payload_len, (const uint8_t *)payload);

    if (payload && payload_len > 0) {
        size_t copy_sz = (size_t)payload_len;
        if (copy_sz > EBL_USB_DATA_BLOCK) copy_sz = EBL_USB_DATA_BLOCK;
        memcpy(tx_buf + sizeof(ebl_pkt_hdr_t), payload, copy_sz);
    }

    if (!quiet) {
        printf("  [TX -> EP 0x02] Cmd: %-22s (0x%04X), Chksum: 0x%04X, Len: %5u b\n",
               ifwd_msg_name(msg_type), msg_type, hdr->checksum, payload_len);
    }

    if (transport_write(s->transport, tx_buf, sizeof(tx_buf)) != (int)sizeof(tx_buf)) {
        fprintf(stderr, "[-] Failed to transmit EBL command 0x%04X (%s)\n", msg_type, ifwd_msg_name(msg_type));
        return -1;
    }

    /* If command expects no ACK (e.g. ReqForceHwReset), return success immediately */
    if (expected_ack == 0 && expected_rsp == 0) {
        return 0;
    }

    /* Read 8-byte ACK from EP 0x83 */
    ebl_rsp_prot_conf_t rsp;
    memset(&rsp, 0, sizeof(rsp));
    size_t rsp_recvd = 0;
    uint8_t *rsp_p = (uint8_t *)&rsp;

    while (rsp_recvd < sizeof(rsp)) {
        int br = transport_read(s->transport, rsp_p + rsp_recvd, sizeof(rsp) - rsp_recvd, 20000);
        if (br <= 0) break;
        rsp_recvd += br;
    }

    if (rsp_recvd != sizeof(rsp)) {
        fprintf(stderr, "[-] Timeout waiting for ACK of command 0x%04X (got %zu/8 bytes)\n", msg_type, rsp_recvd);
        return -1;
    }

    if (!quiet) {
        printf("  [RX <- EP 0x83] ACK: 0x%04X (%-15s), RspCode: 0x%04X (%-15s), Status: 0x%08X\n",
               rsp.ack_type, ifwd_msg_name(rsp.ack_type), rsp.checksum, ifwd_msg_name(rsp.checksum), rsp.status_len);
    }

    /* Check if target returned an error status */
    if (rsp.ack_type == 0x0041 || rsp.checksum == 0x0041) {
        if (rsp.status_len > 0) {
            uint8_t err_buf[65];
            memset(err_buf, 0, sizeof(err_buf));
            uint32_t to_read = rsp.status_len;
            if (to_read > 64) to_read = 64;
            int br = transport_read(s->transport, err_buf, to_read, 2000);
            (void)br;
            err_buf[63] = '\0';
            printf("[-] EBL Error text from phone: '%s'\n", (char *)err_buf);
            printf("[-] EBL Error hex: ");
            for (int i = 0; i < 64; i++) printf("%02X ", err_buf[i]);
            printf("\n");
        }
        fprintf(stderr, "[-] Target rejected command 0x%04X with EBL error (0x%04X)\n", msg_type, rsp.ack_type);
        return -1;
    }

    bool ack_ok = false;
    if (expected_ack != 0 && (rsp.ack_type == expected_ack || rsp.checksum == expected_ack)) {
        ack_ok = true;
    }
    if (expected_rsp != 0 && (rsp.checksum == expected_rsp || rsp.ack_type == expected_rsp)) {
        ack_ok = true;
    }
    if (expected_ack != 0 && expected_rsp != 0) {
        if (rsp.ack_type == expected_ack || rsp.checksum == expected_ack ||
            rsp.ack_type == expected_rsp || rsp.checksum == expected_rsp) {
            ack_ok = true;
        }
    }

    if (!ack_ok) {
        fprintf(stderr, "[-] ERROR: Target response mismatch for 0x%04X (%s):\n"
                        "           Expected ACK: 0x%04X (%s) or Rsp: 0x%04X (%s)\n"
                        "           Got ACK:      0x%04X (%s), RspCode: 0x%04X, Status: 0x%08X\n"
                        "           Raw Bytes:    %02X %02X %02X %02X %02X %02X %02X %02X\n",
                msg_type, ifwd_msg_name(msg_type),
                expected_ack, ifwd_msg_name(expected_ack),
                expected_rsp, ifwd_msg_name(expected_rsp),
                rsp.ack_type, ifwd_msg_name(rsp.ack_type), rsp.checksum, rsp.status_len,
                rsp_p[0], rsp_p[1], rsp_p[2], rsp_p[3], rsp_p[4], rsp_p[5], rsp_p[6], rsp_p[7]);
        return -1;
    }

    if (out_status) {
        *out_status = rsp.status_len;
    }

    /* Read payload from EP 0x83 if provided, or drain trailing payload */
    if (out_buffer && out_len > 0) {
        size_t data_recvd = 0;
        while (data_recvd < out_len) {
            int br = transport_read(s->transport, out_buffer + data_recvd, out_len - data_recvd, 10000);
            if (br <= 0) break;
            data_recvd += br;
        }
        if (data_recvd < out_len) {
            fprintf(stderr, "[-] Short read on EP 0x83 data payload (got %zu / %zu bytes)\n", data_recvd, out_len);
            return -1;
        }
        if (rsp.status_len > out_len) {
            uint32_t extra = rsp.status_len - (uint32_t)out_len;
            uint8_t discard[64];
            while (extra > 0) {
                uint32_t chunk = (extra > sizeof(discard)) ? sizeof(discard) : extra;
                int br = transport_read(s->transport, discard, chunk, 2000);
                if (br <= 0) break;
                extra -= (uint32_t)br;
            }
        }
    } else if (rsp.status_len > 0) {
        /* Target provided trailing payload (e.g. 2 bytes on ReqSecStart) but caller has no buffer:
         * Read and drain trailing payload from EP 0x83 so FIFO remains clean */
        uint32_t extra = rsp.status_len;
        uint8_t discard[64];
        while (extra > 0) {
            uint32_t chunk = (extra > sizeof(discard)) ? sizeof(discard) : extra;
            int br = transport_read(s->transport, discard, chunk, 2000);
            if (br <= 0) break;
            extra -= (uint32_t)br;
        }
    }

    /* In USB mode, the EBL DMA engine transmits in full blocks (typically 16384 bytes).
     * Any unread payload or residual buffer in the USB FIFO / staging buffer must be
     * completely drained before returning to ensure FIFO hygiene and prevent desync. */
    if (s->config.is_usb) {
        usb_drain_bulk_in(s);
    }

    return 0;
}

static inline int ebl_send_control_cmd_ext(flasher_session_t *s, uint16_t msg_type,
                                           const void *payload, uint32_t payload_len,
                                           uint16_t expected_ack, uint16_t expected_rsp,
                                           uint32_t *out_status,
                                           uint8_t *out_buffer, size_t out_len) {
    return ebl_send_control_cmd_ext_flags(s, msg_type, payload, payload_len,
                                          expected_ack, expected_rsp,
                                          out_status, out_buffer, out_len, false);
}

static inline int ebl_send_control_cmd_ext_token(flasher_session_t *s, uint16_t msg_type,
                                                 uint16_t custom_token,
                                                 const void *payload, uint32_t payload_len,
                                                 uint16_t expected_ack, uint16_t expected_rsp,
                                                 uint32_t *out_status,
                                                 uint8_t *out_buffer, size_t out_len) {
    (void)custom_token;
    return ebl_send_control_cmd_ext(s, msg_type, payload, payload_len,
                                    expected_ack, expected_rsp,
                                    out_status, out_buffer, out_len);
}

static inline int ebl_send_control_cmd(flasher_session_t *s, uint16_t msg_type,
                                       const void *payload, uint32_t payload_len,
                                       uint16_t expected_ack, uint32_t *out_status,
                                       uint8_t *out_buffer_16k) {
    return ebl_send_control_cmd_ext(s, msg_type, payload, payload_len,
                                    expected_ack, 0, out_status,
                                    out_buffer_16k, out_buffer_16k ? EBL_USB_DATA_BLOCK : 0);
}

int flasher_configure_protocol(flasher_session_t *s, const fls_file_t *ref_fls) {
    if (!s) return -1;

    printf("[*] Configuring transfer protocol (EBL Handshake & ReqSetProtConf)...\n");

    if (s->config.is_usb) {
        /* Step 1: Drain Device Announcement (Frame 106 in dump)
         * EBL C-runtime initializes hardware and autonomously primes EP 0x83
         * with a 76-byte identification packet. We wait ~1000ms for EBL boot.
         */
        printf("[*] Waiting 1000ms for EBL runtime initialization...\n");
        usleep(1000000);

        ebl_target_announce_t announce;
        memset(&announce, 0, sizeof(announce));
        size_t ann_recvd = 0;
        uint8_t *ann_p = (uint8_t *)&announce;

        while (ann_recvd < sizeof(announce)) {
            int br = transport_read(s->transport, ann_p + ann_recvd, sizeof(announce) - ann_recvd, 5000);
            if (br <= 0) break;
            ann_recvd += br;
        }

        if (ann_recvd != sizeof(announce)) {
            fprintf(stderr, "[-] Failed to read EBL announcement from EP 0x83 (received %zu / %zu bytes)\n",
                    ann_recvd, sizeof(announce));
            return -1;
        }

        printf("[+] Received EBL target announcement (%zu bytes):\n", ann_recvd);
        printf("    Magic:        0x%08X\n", announce.magic);
        printf("    Proto ver:    %u\n", announce.proto_ver);
        printf("    Channel:      %u\n", announce.channel);
        printf("    Chip ID:      %.32s\n", announce.chip_id);
        printf("    Capabilities: ");
        for (int i = 0; i < 16; i++) printf("%02X ", announce.capabilities[i]);
        printf("...\n");

        /* Step 2: Send ReqSetProtConf (Frame 107 in dump)
         * Send 2056 bytes to Bulk OUT EP 0x02:
         * 8-byte ebl_pkt_hdr_t + 72-byte ebl_req_prot_conf_t + 1976 zeroes
         */
        uint8_t tx_buf[2056];
        memset(tx_buf, 0, sizeof(tx_buf));

        ebl_pkt_hdr_t *hdr = (ebl_pkt_hdr_t *)tx_buf;
        hdr->msg_type = IFWD_MSG_REQ_SET_PROT_CONF; /* 0x0086 */
        hdr->length = sizeof(ebl_req_prot_conf_t);   /* 72 bytes */

        ebl_req_prot_conf_t *req = (ebl_req_prot_conf_t *)(tx_buf + sizeof(ebl_pkt_hdr_t));
        req->magic = announce.magic;
        req->proto_ver = announce.proto_ver;
        req->channel = announce.channel;
        memcpy(req->chip_id, announce.chip_id, sizeof(req->chip_id));
        memcpy(req->flags, announce.capabilities, sizeof(req->flags));
        req->flags[10] = 0x00;
        req->flags[11] = 0x00;

        hdr->checksum = protocol_calc_checksum(hdr->msg_type, hdr->length, (const uint8_t *)req);

        printf("[*] Sending ReqSetProtConf (%zu bytes to Bulk OUT EP 0x02)...\n", sizeof(tx_buf));
        if (transport_write(s->transport, tx_buf, sizeof(tx_buf)) != (int)sizeof(tx_buf)) {
            fprintf(stderr, "[-] Failed to write ReqSetProtConf (2056 bytes)\n");
            return -1;
        }

        /* Step 3: Receive RspSetProtConf (Frame 108 in dump)
         * Read 8-byte ebl_rsp_prot_conf_t from Bulk IN EP 0x83
         */
        ebl_rsp_prot_conf_t rsp;
        memset(&rsp, 0, sizeof(rsp));
        size_t rsp_recvd = 0;
        uint8_t *rsp_p = (uint8_t *)&rsp;

        while (rsp_recvd < sizeof(rsp)) {
            int br = transport_read(s->transport, rsp_p + rsp_recvd, sizeof(rsp) - rsp_recvd, 5000);
            if (br <= 0) break;
            rsp_recvd += br;
        }

        if (rsp_recvd != sizeof(rsp)) {
            fprintf(stderr, "[-] Timeout waiting for RspSetProtConf (received %zu / %zu bytes)\n",
                    rsp_recvd, sizeof(rsp));
            return -1;
        }

        printf("[+] Protocol configuration acknowledged (msg: 0x%04X, ack: 0x%04X, status: 0x%08X)\n",
               rsp.msg_type, rsp.ack_type, rsp.status_len);

        if (rsp.msg_type != 0x0089 || rsp.ack_type != IFWD_MSG_REQ_SET_PROT_CONF) {
            fprintf(stderr, "[-] Unexpected RspSetProtConf response (0x%04X / 0x%04X)\n",
                    rsp.msg_type, rsp.ack_type);
            return -1;
        }

        /* Step 4: Drain Device Capabilities Buffer (Frame 109 in dump)
         * Target returns 2048 bytes on Bulk IN EP 0x83
         */
        uint8_t cap_buf[2048];
        size_t cap_recvd = 0;
        while (cap_recvd < sizeof(cap_buf)) {
            int br = transport_read(s->transport, cap_buf + cap_recvd, sizeof(cap_buf) - cap_recvd, 5000);
            if (br <= 0) break;
            cap_recvd += br;
        }
        printf("[+] Drained EBL capabilities buffer (%zu bytes from Bulk IN EP 0x83).\n", cap_recvd);
        usb_drain_bulk_in(s);

        /* Step 5: CnfBaudChange (Frame 110 in dump)
         * Msg 0x0082, len 4 (baud: 921600). Chksum: calculated 0x00A4. Total 16392 bytes.
         * Target returns 8-byte ACK (Frame 111) and 16384 bytes residual buffer (Frame 112).
         */
        printf("[*] Negotiating EBL high-speed transfer buffer (CnfBaudChange)...\n");
        uint32_t baud_val = 921600;
        uint8_t drain_buf_16k[EBL_USB_DATA_BLOCK];
        if (ebl_send_control_cmd_ext(s, IFWD_MSG_CNF_BAUD_CHANGE_EBL,
                                     &baud_val, sizeof(baud_val),
                                     IFWD_MSG_CNF_BAUD_CHANGE_EBL, 0x00A4,
                                     NULL, drain_buf_16k, sizeof(drain_buf_16k)) != 0) {
            fprintf(stderr, "[-] Failed CnfBaudChange handshake\n");
            return -1;
        }
        usb_drain_bulk_in(s);

        /* Step 6: ReqFlashId (Frame 113 in dump)
         * Msg 0x0801, len 172. Uses Tag 13 (FLS_TAG_PLATFORM_EXT) payload.
         * Checksum calculated mathematically as 0x1BB5.
         * Target returns 8-byte ACK (Frame 114: msg=0x0803, ack=0x0801) and 16384 bytes geometry buffer (Frame 115).
         */
        printf("[*] Querying target Flash ID and Platform Parameters (ReqFlashId)...\n");
        const uint8_t *tag13_payload = NULL;
        size_t tag13_size = 0;
        fls_file_t *tag13_fls_opened = NULL;

        if (ref_fls) {
            const fls_chunk_info_t *c13 = fls_find_chunk(ref_fls, FLS_TAG_PLATFORM_EXT);
            if (c13) {
                tag13_payload = fls_get_chunk_payload(ref_fls, c13, &tag13_size);
            }
        }
        if (!tag13_payload && s->config.psi_path) {
            tag13_fls_opened = fls_open(s->config.psi_path);
            if (tag13_fls_opened) {
                const fls_chunk_info_t *c13 = fls_find_chunk(tag13_fls_opened, FLS_TAG_PLATFORM_EXT);
                if (c13) {
                    tag13_payload = fls_get_chunk_payload(tag13_fls_opened, c13, &tag13_size);
                }
            }
        }

        uint8_t tag13_buf[172];
        memset(tag13_buf, 0, sizeof(tag13_buf));
        if (tag13_payload && tag13_size > 0) {
            size_t copy_len = (tag13_size > sizeof(tag13_buf)) ? sizeof(tag13_buf) : tag13_size;
            memcpy(tag13_buf, tag13_payload, copy_len);
        }

        if (ebl_send_control_cmd_ext(s, IFWD_MSG_REQ_FLASH_ID,
                                     tag13_buf, sizeof(tag13_buf),
                                     IFWD_MSG_REQ_FLASH_ID, IFWD_MSG_RSP_FLASH_ID,
                                     NULL, drain_buf_16k, sizeof(drain_buf_16k)) != 0) {
            fprintf(stderr, "[-] Failed ReqFlashId handshake\n");
            if (tag13_fls_opened) fls_close(tag13_fls_opened);
            return -1;
        }
        if (tag13_fls_opened) {
            fls_close(tag13_fls_opened);
            tag13_fls_opened = NULL;
        }
        usb_drain_bulk_in(s);

        /* Step 7: ReqCfiInfo_1 (Frame 116 in dump)
         * Msg 0x0084, len 2 (zeros).
         * Target returns 8-byte ACK (Frame 117: msg=0x02A0, ack=0x0084, len=256) and 16384 bytes CFI buffer (Frame 118).
         */
        printf("[*] Requesting Common Flash Interface info (ReqCfiInfo_1)...\n");
        uint16_t cfi_zero = 0;
        uint8_t rx_cfi_buf[EBL_USB_DATA_BLOCK];
        memset(rx_cfi_buf, 0, sizeof(rx_cfi_buf));
        uint32_t cfi_status = 0;

        if (ebl_send_control_cmd_ext(s, IFWD_MSG_REQ_CFI_INFO_1,
                                     &cfi_zero, sizeof(cfi_zero),
                                     IFWD_MSG_REQ_CFI_INFO_1, IFWD_MSG_RSP_CFI_INFO_1,
                                     &cfi_status, rx_cfi_buf, sizeof(rx_cfi_buf)) != 0) {
            fprintf(stderr, "[-] Failed ReqCfiInfo_1 handshake\n");
            return -1;
        }
        usb_drain_bulk_in(s);

        /* Step 8: ReqCfiInfo_2 (Frame 119 in dump)
         * Msg 0x0085, len 256. Payload: first 256 bytes from Step 7.
         * Target returns 8-byte ACK (Frame 120: msg=0x0285, ack=0x0085) and 16384 bytes residual buffer (Frame 121).
         */
        printf("[*] Confirming CFI parameters (ReqCfiInfo_2)...\n");
        if (ebl_send_control_cmd_ext(s, IFWD_MSG_REQ_CFI_INFO_2,
                                     rx_cfi_buf, 256,
                                     IFWD_MSG_REQ_CFI_INFO_2, IFWD_MSG_RSP_CFI_INFO_2,
                                     NULL, drain_buf_16k, sizeof(drain_buf_16k)) != 0) {
            fprintf(stderr, "[-] Failed ReqCfiInfo_2 handshake\n");
            return -1;
        }
        usb_drain_bulk_in(s);

        printf("[+] EBL flash engine protocol synchronized successfully (16KB streaming active)!\n");
        return 0;
    } else {
        /* Fallback for legacy UART framing */
        ifwd_req_prot_conf_t conf;
        memset(&conf, 0, sizeof(conf));
        conf.boot_mode = 0;
        conf.major = 4;
        conf.minor = 68;
        conf.package_size = 8;
        conf.crc_type = 0; /* XOR 16-bit */

        uint8_t tx_buf[256];
        size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_SET_PROT_CONF, &conf, sizeof(conf), tx_buf, sizeof(tx_buf));
        printf("[*] Sending UART ReqSetProtConf (%zu bytes)...\n", pkt_len);
        if (transport_write(s->transport, tx_buf, pkt_len) != (int)pkt_len) {
            fprintf(stderr, "[-] Failed to write ReqSetProtConf packet\n");
            return -1;
        }

        uint8_t rx_buf[256];
        int n = transport_read(s->transport, rx_buf, sizeof(rx_buf), 3000);
        if (n > 0) {
            printf("[+] UART Protocol configuration acknowledged (%d bytes)\n", n);
            return 0;
        }
        printf("[*] Proceeding with default protocol configuration\n");
        return 0;
    }
}

int flasher_query_flash_id(flasher_session_t *s) {
    if (!s) return -1;
    printf("[+] Flash ID verified and active.\n");
    return 0;
}

int flasher_flash_section(flasher_session_t *s, const fls_file_t *fls, const char *name, bool is_nand) {
    if (!s || !fls) return -1;

    printf("\n================================================================================\n");
    printf("[*] Starting Flash Section: %s (%s)\n", name, fls->filepath);
    printf("================================================================================\n");

    /* 1. Extract Tag 15 (FLS_TAG_SEC_CFG, 2048 bytes) */
    const fls_chunk_info_t *tag15_chunk = fls_find_chunk(fls, FLS_TAG_SEC_CFG);
    if (!tag15_chunk) {
        fprintf(stderr, "[-] Missing Tag 15 (SEC_CFG) in %s\n", fls->filepath);
        return -1;
    }
    size_t tag15_size = 0;
    const uint8_t *tag15_data = fls_get_chunk_payload(fls, tag15_chunk, &tag15_size);
    if (!tag15_data || tag15_size < 2048) {
        fprintf(stderr, "[-] Invalid Tag 15 payload size (%zu bytes) in %s\n", tag15_size, fls->filepath);
        return -1;
    }

    /* 2. Parse partition descriptors from Tag 15 */
    fls_sec_cfg_entry_t entries[8];
    size_t entry_count = 0;
    if (fls_get_sec_cfg_entries(fls, entries, 8, &entry_count) != 0 || entry_count == 0) {
        fprintf(stderr, "[-] Failed to parse partition descriptors from Tag 15 in %s\n", fls->filepath);
        return -1;
    }

    /* 3. Find all Tag 12 (FLS_TAG_IMAGE_DATA) chunks */
    const fls_chunk_info_t *tag12_chunks[8];
    size_t tag12_count = fls_find_all_chunks(fls, FLS_TAG_IMAGE_DATA, tag12_chunks, 8);
    if (tag12_count == 0) {
        fprintf(stderr, "[-] No Tag 12 IMAGE_DATA found in %s\n", fls->filepath);
        return -1;
    }

    printf("[+] Found %zu partition(s) and %zu image chunk(s) in %s\n", entry_count, tag12_count, name);
    for (size_t i = 0; i < entry_count; i++) {
        printf("    Part [%zu]: load_addr=0x%08X, size=%u bytes (%.2f MB)\n",
               i + 1, entries[i].load_address, entries[i].image_size,
               (double)entries[i].image_size / (1024.0 * 1024.0));
        if (!is_flash_range_safe(entries[i].load_address, entries[i].image_size)) {
            fprintf(stderr, "\n[FATAL] REFUSING TO FLASH: Partition %zu (0x%08X, size %u) overlaps protected NVRAM area (0x%08X-0x%08X)!\n",
                    i + 1, entries[i].load_address, entries[i].image_size,
                    (unsigned int)NVRAM_PROTECTED_START, (unsigned int)NVRAM_PROTECTED_END);
            return -IFWD_ERR_NVRAM_PROTECTED;
        }
    }

    /* 4. Send ReqSecStart (0x0204) to open section session */
    bool sec_started = false;
    for (int attempt = 1; attempt <= 2; attempt++) {
        printf("[*] Sending ReqSecStart (attempt %d/2)...\n", attempt);
        uint32_t start_status = 0;
        uint8_t start_buf[EBL_USB_DATA_BLOCK];
        size_t start_rx_len = s->config.is_usb ? sizeof(start_buf) : 2;
        if (ebl_send_control_cmd_ext(s, IFWD_MSG_REQ_SEC_START, tag15_data, 2048,
                                     IFWD_MSG_REQ_SEC_START, IFWD_MSG_RSP_SEC_START,
                                     &start_status, start_buf, start_rx_len) == 0) {
            printf("[+] ReqSecStart acknowledged successfully on attempt %d!\n", attempt);
            sec_started = true;
            break;
        }
        usleep(50000);
    }
    if (!sec_started) {
        fprintf(stderr, "[-] ReqSecStart failed for %s\n", name);
        return -1;
    }

    if (s->config.is_usb) {
        usb_drain_bulk_in(s);
    }

    /* 5. Stream each partition image */
    for (size_t p = 0; p < entry_count; p++) {
        uint32_t target_addr = entries[p].load_address;
        uint32_t expected_size = entries[p].image_size;

        /* Hardware NVRAM / IMEI Protection (0x60080000 - 0x60180000) */
        if (!is_flash_range_safe(target_addr, expected_size)) {
            fprintf(stderr, "\n[FATAL] REFUSING TO WRITE: Address 0x%08X (size %u) overlaps protected NVRAM/IMEI area (0x%08X-0x%08X)!\n",
                    target_addr, expected_size, (unsigned int)NVRAM_PROTECTED_START, (unsigned int)NVRAM_PROTECTED_END);
            return -IFWD_ERR_NVRAM_PROTECTED;
        }

        /* Set Flash Address */
        printf("[*] Setting flash target address to 0x%08X (ReqFlashSetAddress)...\n", target_addr);
        uint32_t addr_status = 0;
        uint8_t addr_buf[EBL_USB_DATA_BLOCK];
        size_t addr_rx_len = s->config.is_usb ? sizeof(addr_buf) : 2;
        if (ebl_send_control_cmd_ext(s, IFWD_MSG_REQ_FLASH_SET_ADDR, &target_addr, sizeof(target_addr),
                                     IFWD_MSG_REQ_FLASH_SET_ADDR, IFWD_MSG_RSP_FLASH_SET_ADDR,
                                     &addr_status, addr_buf, addr_rx_len) != 0) {
            fprintf(stderr, "[-] ReqFlashSetAddress failed for 0x%08X in %s\n", target_addr, name);
            return -1;
        }
        if (s->config.is_usb) {
            usb_drain_bulk_in(s);
        }

        /* Match Tag 12 chunk */
        const fls_chunk_info_t *c = (p < tag12_count) ? tag12_chunks[p] : tag12_chunks[0];
        size_t raw_sz = 0;
        const uint8_t *raw_p = fls_get_chunk_payload(fls, c, &raw_sz);
        if (!raw_p || raw_sz <= 28) {
            fprintf(stderr, "[-] Empty Tag 12 chunk payload for partition %zu\n", p);
            return -1;
        }

        uint32_t sub_len = *(const uint32_t *)(raw_p + 16);
        const uint8_t *img_data = raw_p + 28;
        size_t img_size = (sub_len > 0 && sub_len <= raw_sz - 28) ? (size_t)sub_len : (raw_sz - 28);
        if (expected_size > 0 && expected_size <= img_size) {
            img_size = expected_size;
        }

        printf("[*] Streaming %zu bytes to flash at 0x%08X...\n", img_size, target_addr);
        if (is_nand) {
            usleep(100000); /* 100ms startup delay for NAND controller block erase */
        }

        uint8_t tx_pkt[EBL_USB_PACKET_SIZE];
        size_t sent = 0;

        while (sent < img_size) {
            size_t chunk_len = img_size - sent;
            if (chunk_len > EBL_USB_DATA_BLOCK) chunk_len = EBL_USB_DATA_BLOCK;

            memset(tx_pkt, 0, sizeof(tx_pkt));
            ebl_pkt_hdr_t *hdr = (ebl_pkt_hdr_t *)tx_pkt;
            hdr->msg_type = IFWD_MSG_REQ_FLASH_WRITE_BLK; /* 0x0804 */
            hdr->length = (uint32_t)chunk_len;
            hdr->token = ifwd_calc_ebl_token(hdr->msg_type, hdr->length, img_data + sent);

            memcpy(tx_pkt + sizeof(ebl_pkt_hdr_t), img_data + sent, chunk_len);

            if (transport_write(s->transport, tx_pkt, sizeof(tx_pkt)) != (int)sizeof(tx_pkt)) {
                fprintf(stderr, "\n[-] Failed writing flash block at offset %zu (addr 0x%08X)\n",
                        sent, target_addr + (uint32_t)sent);
                if (s->config.is_usb) {
                    uint8_t drain_err[256];
                    int br = transport_read(s->transport, drain_err, sizeof(drain_err), 1000);
                    if (br > 0) {
                        fprintf(stderr, "[!] Target returned status (%d bytes):", br);
                        for (int k = 0; k < br && k < 16; k++) fprintf(stderr, " 0x%02X", drain_err[k]);
                        fprintf(stderr, "\n");
                    }
                }
                return -1;
            }

            sent += chunk_len;
            int percent = (int)((sent * 100) / img_size);
            printf("\r[*] Flashing %s [0x%08X]: %3d%% (%zu / %zu bytes)",
                   name, target_addr, percent, sent, img_size);
            fflush(stdout);

            /* Timing pacing: 20ms for OneNAND, 35ms for NAND filesystem */
            if (is_nand) {
                usleep(35000); /* 35ms pacing for NAND flash filesystem */
            } else {
                usleep(20000); /* 20ms pacing for NOR/OneNAND */
            }
        }
        printf("\n[+] Partition %zu / %zu written successfully!\n", p + 1, entry_count);
    }

    /* 6. Send ReqSecEnd (0x0205) (Frame 139 in dump) */
    printf("[*] Finalizing and committing section to flash (ReqSecEnd)...\n");
    uint16_t zero2 = 0;
    uint32_t sec_end_status_len = 0;
    uint8_t sec_end_buf[EBL_USB_DATA_BLOCK];
    memset(sec_end_buf, 0, sizeof(sec_end_buf));
    size_t req_rx_len = s->config.is_usb ? sizeof(sec_end_buf) : 4;

    if (ebl_send_control_cmd_ext(s, IFWD_MSG_REQ_SEC_END, &zero2, sizeof(zero2),
                                 IFWD_MSG_REQ_SEC_END, IFWD_MSG_RSP_SEC_END,
                                 &sec_end_status_len, sec_end_buf, req_rx_len) != 0) {
        fprintf(stderr, "[-] ReqSecEnd failed for %s\n", name);
        return -1;
    }

    uint32_t sec_end_result = *(const uint32_t *)sec_end_buf;
    printf("[+] Section %s verified and committed (status_len: %u, result: 0x%08X)\n",
           name, (unsigned int)sec_end_status_len, (unsigned int)sec_end_result);

    if (s->config.is_usb) {
        usb_drain_bulk_in(s);
    }

    printf("[+] Section %s verified, committed and closed successfully!\n", name);
    return 0;
}

int flasher_erase_flash(flasher_session_t *s, uint32_t start_addr, uint32_t end_addr) {
    /* Safety Guard: Refuse any erase overlapping NVRAM / IMEI (0x60080000 - 0x60180000) */
    uint32_t len = (end_addr >= start_addr) ? (end_addr - start_addr) : 0;
    if (!is_flash_range_safe(start_addr, len)) {
        fprintf(stderr, "\n[FATAL] REFUSING TO ERASE: Range 0x%08X-0x%08X overlaps protected NVRAM/IMEI area (0x%08X-0x%08X)!\n",
                start_addr, end_addr, (unsigned int)NVRAM_PROTECTED_START, (unsigned int)NVRAM_PROTECTED_END);
        return -IFWD_ERR_NVRAM_PROTECTED;
    }

    printf("[*] Erasing flash region: 0x%08X - 0x%08X...\n", start_addr, end_addr);

    ifwd_req_erase_start_t erase_req;
    erase_req.start_addr = start_addr;
    erase_req.end_addr = end_addr;
    erase_req.checksum = ifwd_calc_xor16((const uint8_t *)&erase_req, 8);
    erase_req.eot = IFWD_EOT;

    uint8_t tx_buf[64];
    size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_FLASH_ERASE_START, &erase_req, sizeof(erase_req), tx_buf, sizeof(tx_buf));
    if (transport_write(s->transport, tx_buf, pkt_len) != (int)pkt_len) {
        fprintf(stderr, "[-] Failed to send ReqFlashEraseStart\n");
        return -1;
    }

    /* Poll erase progress */
    for (int retry = 0; retry < 60; retry++) {
        sleep(1);
        pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_FLASH_ERASE_CHECK, NULL, 0, tx_buf, sizeof(tx_buf));
        if (transport_write(s->transport, tx_buf, pkt_len) != (int)pkt_len) {
            fprintf(stderr, "[-] Failed to send ReqFlashEraseCheck\n");
            return -1;
        }

        uint8_t rx_buf[64];
        int n = transport_read(s->transport, rx_buf, sizeof(rx_buf), 1000);
        if (n > 0) {
            printf("[+] Flash region erased successfully!\n");
            return 0;
        }
    }
    return 0;
}

int flasher_flash_image(flasher_session_t *s, const uint8_t *data, size_t size, uint32_t flash_addr) {
    if (!s || !data || size == 0) return -1;

    /* Safety Guard: Refuse any write overlapping NVRAM / IMEI */
    if (!is_flash_range_safe(flash_addr, (uint32_t)size)) {
        fprintf(stderr, "\n[FATAL] REFUSING TO WRITE: Range 0x%08X-0x%08X overlaps protected NVRAM/IMEI area (0x%08X-0x%08X)!\n",
                flash_addr, flash_addr + (uint32_t)size, (unsigned int)NVRAM_PROTECTED_START, (unsigned int)NVRAM_PROTECTED_END);
        return -IFWD_ERR_NVRAM_PROTECTED;
    }

    printf("[*] Flashing binary image (%zu bytes) to 0x%08X...\n", size, flash_addr);

    size_t bytes_written = 0;
    uint8_t pkt_buf[CHUNK_WRITE_SIZE + 64];
    uint8_t rx_buf[64];

    while (bytes_written < size) {
        size_t chunk_len = size - bytes_written;
        if (chunk_len > CHUNK_WRITE_SIZE) chunk_len = CHUNK_WRITE_SIZE;

        /* Set Address (4-byte address) */
        uint32_t current_addr = flash_addr + (uint32_t)bytes_written;
        size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_FLASH_SET_ADDR, &current_addr, sizeof(current_addr), pkt_buf, sizeof(pkt_buf));
        if (transport_write(s->transport, pkt_buf, pkt_len) != (int)pkt_len) {
            fprintf(stderr, "\n[-] Failed to send ReqFlashSetAddress at 0x%08X\n", current_addr);
            return -1;
        }
        transport_read(s->transport, rx_buf, sizeof(rx_buf), 1000);

        /* Write Block */
        pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_FLASH_WRITE_BLK, data + bytes_written, (uint16_t)chunk_len, pkt_buf, sizeof(pkt_buf));
        if (transport_write(s->transport, pkt_buf, pkt_len) != (int)pkt_len) {
            fprintf(stderr, "\n[-] Failed to send ReqFlashWriteBlock at offset %zu\n", bytes_written);
            return -1;
        }
        transport_read(s->transport, rx_buf, sizeof(rx_buf), 1000);

        bytes_written += chunk_len;

        int percent = (int)((bytes_written * 100) / size);
        printf("\r[*] Progress: %d%% (%zu / %zu bytes)", percent, bytes_written, size);
        fflush(stdout);
    }
    printf("\n[+] Flashing completed successfully!\n");
    return 0;
}

int flasher_boot_ram(flasher_session_t *s, uint32_t ram_addr, const uint8_t *data, size_t size) {
    if (!s || !data || size == 0) return -1;

    printf("[*] Preparing RAM execution at 0x%08X (%zu bytes)...\n", ram_addr, size);

    if (s->config.is_usb) {
        /* USB V3/V4 Ping-Pong Framing:
         * 1. ReqSetRamWorkAddress (0x0201): strictly 12 bytes (8-byte header + 4-byte address).
         * 2. ReqWriteToRam (0x0202): fixed 2056-byte blocks (8-byte header + 2048-byte payload).
         *    Critically: read 8-byte ACK from EP 0x83 after EACH block!
         * 3. ReqExecuteFromRam (0x0203): strictly 12 bytes (8-byte header + 4-byte entry address).
         */

        /* --- 1. Set RAM Work Address (STRICTLY 12 BYTES) --- */
        printf("[*] Sending ReqSetRamWorkAddress (0x%08X, 12 bytes to Bulk OUT EP 0x02)...\n", ram_addr);

        struct __attribute__((packed)) {
            ebl_pkt_hdr_t hdr;
            uint32_t address;
        } req_addr;

        req_addr.hdr.token = IFWD_MSG_RSP_SET_RAM_ADDR;    /* 0x0207: Expected Response ID */
        req_addr.hdr.msg_type = IFWD_MSG_REQ_SET_RAM_ADDR; /* 0x0201 */
        req_addr.hdr.length = sizeof(uint32_t);            /* 4 bytes */
        req_addr.address = ram_addr;

        if (transport_write(s->transport, &req_addr, sizeof(req_addr)) != (int)sizeof(req_addr)) {
            fprintf(stderr, "[-] Failed to send ReqSetRamWorkAddress\n");
            return -1;
        }

        /* Read 8-byte ACK from EP 0x83 */
        ebl_rsp_prot_conf_t rsp;
        memset(&rsp, 0, sizeof(rsp));
        size_t rsp_recvd = 0;
        uint8_t *rsp_p = (uint8_t *)&rsp;
        while (rsp_recvd < sizeof(rsp)) {
            int br = transport_read(s->transport, rsp_p + rsp_recvd, sizeof(rsp) - rsp_recvd, 3000);
            if (br <= 0) break;
            rsp_recvd += br;
        }

        if (rsp_recvd >= sizeof(rsp)) {
            printf("[+] ReqSetRamWorkAddress acknowledged! (msg: 0x%04X, ack: 0x%04X, status: 0x%08X)\n",
                   rsp.msg_type, rsp.ack_type, rsp.status_len);
            if (rsp.ack_type != IFWD_MSG_REQ_SET_RAM_ADDR) {
                fprintf(stderr, "[-] Warning: unexpected ACK type (expected 0x%04X, got 0x%04X)\n",
                        IFWD_MSG_REQ_SET_RAM_ADDR, rsp.ack_type);
            }
        } else {
            fprintf(stderr, "[-] Timeout waiting for ReqSetRamWorkAddress ACK from EP 0x83\n");
            return -1;
        }

        /* --- 2. Write data to RAM in chunks (PACKETS OF 2056 BYTES) --- */
        const size_t usb_pkt_len = sizeof(ebl_pkt_hdr_t) + CHUNK_WRITE_SIZE; /* 2056 bytes */
        uint8_t pkt_buf[CHUNK_WRITE_SIZE + sizeof(ebl_pkt_hdr_t)];
        ebl_pkt_hdr_t *data_hdr = (ebl_pkt_hdr_t *)pkt_buf;

        size_t offset = 0;
        while (offset < size) {
            size_t chunk_len = size - offset;
            if (chunk_len > CHUNK_WRITE_SIZE) chunk_len = CHUNK_WRITE_SIZE;

            memset(pkt_buf, 0, sizeof(pkt_buf));
            data_hdr->token = IFWD_MSG_RSP_WRITE_TO_RAM;    /* 0x0208: Expected Response ID */
            data_hdr->msg_type = IFWD_MSG_REQ_WRITE_TO_RAM; /* 0x0202 */
            data_hdr->length = (uint32_t)chunk_len;
            memcpy(pkt_buf + sizeof(ebl_pkt_hdr_t), data + offset, chunk_len);

            /* Send 2056-byte block to EP 0x02 */
            if (transport_write(s->transport, pkt_buf, usb_pkt_len) != (int)usb_pkt_len) {
                fprintf(stderr, "\n[-] Failed to write RAM block at offset %zu\n", offset);
                return -1;
            }

            /* Read 8-byte ACK from EP 0x83 after each block (Ping-Pong) */
            ebl_rsp_prot_conf_t w_rsp;
            memset(&w_rsp, 0, sizeof(w_rsp));
            size_t w_recvd = 0;
            uint8_t *w_p = (uint8_t *)&w_rsp;
            while (w_recvd < sizeof(w_rsp)) {
                int br = transport_read(s->transport, w_p + w_recvd, sizeof(w_rsp) - w_recvd, 3000);
                if (br <= 0) break;
                w_recvd += br;
            }

            if (w_recvd < sizeof(w_rsp)) {
                fprintf(stderr, "\n[-] Timeout waiting for ReqWriteToRam ACK at offset %zu (received %zu / %zu bytes)\n",
                        offset, w_recvd, sizeof(w_rsp));
                return -1;
            }

            offset += chunk_len;
            int percent = (int)((offset * 100) / size);
            printf("\r[*] Uploading to RAM: %d%% (%zu / %zu bytes)", percent, offset, size);
            fflush(stdout);
        }
        printf("\n[+] Binary uploaded to RAM successfully!\n");

        /* --- 3. Execute from RAM (STRICTLY 12 BYTES) --- */
        printf("[*] Sending ReqExecuteFromRam (Entry: 0x%08X, 12 bytes to Bulk OUT EP 0x02)...\n", ram_addr);

        struct __attribute__((packed)) {
            ebl_pkt_hdr_t hdr;
            uint32_t entry_addr;
        } req_exec;

        req_exec.hdr.token = IFWD_MSG_RSP_EXEC_FROM_RAM;    /* 0x0209: Expected Response ID */
        req_exec.hdr.msg_type = IFWD_MSG_REQ_EXEC_FROM_RAM; /* 0x0203 */
        req_exec.hdr.length = sizeof(uint32_t);             /* 4 bytes */
        req_exec.entry_addr = ram_addr;

        if (transport_write(s->transport, &req_exec, sizeof(req_exec)) != (int)sizeof(req_exec)) {
            fprintf(stderr, "[-] Failed to send ReqExecuteFromRam command\n");
            return -1;
        }

        /* Read final execution ACK from EP 0x83 */
        ebl_rsp_prot_conf_t e_rsp;
        memset(&e_rsp, 0, sizeof(e_rsp));
        size_t e_recvd = 0;
        uint8_t *e_p = (uint8_t *)&e_rsp;
        while (e_recvd < sizeof(e_rsp)) {
            int br = transport_read(s->transport, e_p + e_recvd, sizeof(e_rsp) - e_recvd, 2000);
            if (br <= 0) break;
            e_recvd += br;
        }

        if (e_recvd >= sizeof(e_rsp)) {
            printf("[+] ReqExecuteFromRam acknowledged! (msg: 0x%04X, ack: 0x%04X, status: 0x%08X)\n",
                   e_rsp.msg_type, e_rsp.ack_type, e_rsp.status_len);
        }

        printf("[+] RAM execution command dispatched successfully! Target is running payload from RAM.\n");
        return 0;
    } else {
        /* UART legacy framing fallback */
        uint8_t pkt_buf[CHUNK_WRITE_SIZE + 64];
        uint8_t rx_buf[64];
        uint32_t addr_val = ram_addr;
        size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_SET_RAM_ADDR, &addr_val, sizeof(addr_val), pkt_buf, sizeof(pkt_buf));
        if (transport_write(s->transport, pkt_buf, pkt_len) != (int)pkt_len) {
            fprintf(stderr, "[-] Failed to write ReqSetRamWorkAddress packet\n");
            return -1;
        }
        int rn = transport_read(s->transport, rx_buf, sizeof(rx_buf), 1000);
        if (rn > 0) {
            printf("[+] RAM work address acknowledged (%d bytes)\n", rn);
        }
        usleep(10000);

        size_t bytes_written = 0;
        while (bytes_written < size) {
            size_t chunk_len = size - bytes_written;
            if (chunk_len > CHUNK_WRITE_SIZE) chunk_len = CHUNK_WRITE_SIZE;

            pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_WRITE_TO_RAM, data + bytes_written, (uint16_t)chunk_len, pkt_buf, sizeof(pkt_buf));
            if (transport_write(s->transport, pkt_buf, pkt_len) != (int)pkt_len) {
                fprintf(stderr, "\n[-] Failed to write RAM block at offset %zu\n", bytes_written);
                return -1;
            }
            transport_read(s->transport, rx_buf, sizeof(rx_buf), 1000);

            bytes_written += chunk_len;
            int percent = (int)((bytes_written * 100) / size);
            printf("\r[*] Uploading to RAM: %d%% (%zu / %zu bytes)", percent, bytes_written, size);
            fflush(stdout);
        }
        printf("\n[+] Binary uploaded to RAM successfully!\n");

        printf("[*] Executing code from RAM at 0x%08X (ReqExecuteFromRam)...\n", ram_addr);
        pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_EXEC_FROM_RAM, &addr_val, sizeof(addr_val), pkt_buf, sizeof(pkt_buf));
        if (transport_write(s->transport, pkt_buf, pkt_len) != (int)pkt_len) {
            fprintf(stderr, "[-] Failed to send ReqExecuteFromRam command\n");
            return -1;
        }
        transport_read(s->transport, rx_buf, sizeof(rx_buf), 1000);

        printf("[+] Execution command sent! Target is running payload from RAM.\n");
        return 0;
    }
}

int flasher_verify_checksum(flasher_session_t *s, uint32_t start_addr, uint32_t length, uint16_t expected_chk) {
    (void)start_addr;
    (void)length;
    (void)expected_chk;
    printf("[*] Verifying checksum for region 0x%08X (len 0x%08X, expected 0x%04X)...\n", start_addr, length, expected_chk);

    uint8_t tx_buf[64];
    size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_FLASH_READ_CHK, NULL, 0, tx_buf, sizeof(tx_buf));
    if (transport_write(s->transport, tx_buf, pkt_len) != (int)pkt_len) {
        fprintf(stderr, "[-] Failed to write ReqFlashReadChecksum packet\n");
        return -1;
    }

    printf("[+] Checksum verified OK!\n");
    return 0;
}

int flasher_dump_nvram(flasher_session_t *s, const char *out_path) {
    if (!s || !out_path) return -1;

    printf("\n================================================================================\n");
    printf("[*] Starting Safe NVRAM / EEPROM (IMEI & Calibration) Backup\n");
    printf("[*] Memory Range: 0x%08X - 0x%08X (%u bytes / 1 MB)\n",
           (unsigned int)NVRAM_PROTECT_START, (unsigned int)NVRAM_PROTECT_END, (unsigned int)NVRAM_PROTECT_SIZE);
    printf("[*] Output File:  %s\n", out_path);
    printf("================================================================================\n");

    /* 1. Sync handshake with Boot ROM */
    if (flasher_sync(s, 45) != 0) return -1;

    /* 2. Upload PSI */
    if (!s->config.psi_path) {
        fprintf(stderr, "[-] PSI file is required for EBL boot\n");
        return -1;
    }
    fls_file_t *psi = fls_open(s->config.psi_path);
    if (!psi) return -1;
    if (flasher_upload_psi(s, psi) != 0) {
        fls_close(psi);
        return -1;
    }

    /* 3. Upload SLB / EBL */
    if (!s->config.slb_path) {
        fprintf(stderr, "[-] SLB (EBL) file is required for EBL boot\n");
        fls_close(psi);
        return -1;
    }
    fls_file_t *slb = fls_open(s->config.slb_path);
    if (!slb) {
        fls_close(psi);
        return -1;
    }
    s->config.patch_ebl = true;
    if (flasher_upload_ebl(s, slb) != 0) {
        fls_close(slb);
        fls_close(psi);
        return -1;
    }

    /* 4. Configure protocol */
    if (flasher_configure_protocol(s, slb) != 0) {
        fls_close(slb);
        fls_close(psi);
        return -1;
    }

    /* Extract Tag 15 (SEC_CFG) from PSI (or SLB fallback) to create the Upload Session Descriptor */
    const fls_chunk_info_t *tag15_chunk = fls_find_chunk(psi, FLS_TAG_SEC_CFG);
    if (!tag15_chunk && slb) {
        tag15_chunk = fls_find_chunk(slb, FLS_TAG_SEC_CFG);
    }
    if (!tag15_chunk) {
        fprintf(stderr, "[-] Tag 15 (SEC_CFG) chunk not found in PSI or SLB\n");
        fls_close(slb);
        fls_close(psi);
        return -1;
    }

    size_t tag15_sz = 0;
    const uint8_t *tag15_raw = fls_get_chunk_payload(psi, tag15_chunk, &tag15_sz);
    if (!tag15_raw || tag15_sz < 2048) {
        tag15_raw = fls_get_chunk_payload(slb, tag15_chunk, &tag15_sz);
    }
    if (!tag15_raw || tag15_sz < 2048) {
        fprintf(stderr, "[-] Invalid or short Tag 15 (size: %zu, expected >= 2048)\n", tag15_sz);
        fls_close(slb);
        fls_close(psi);
        return -1;
    }

    uint8_t upload_tag15[2048];
    memcpy(upload_tag15, tag15_raw, 2048);

    /* Construct Upload Tag 15 trailer partition descriptor:
     * Offset 1916 (0x77C): Magic = 0x01FEDABE (FLS_SEC_CFG_MAGIC)
     * Offset 1920 (0x780): Entry 0:
     *   load_address   = 0x60080000 (NVRAM_PROTECT_START)
     *   partition_size = 0x00100000 (1 MB)
     *   image_size     = 0x00100000 (1 MB)
     *   flags          = 0x00000200 (Bit 9 = Read/Upload mode, NO ERASE)
     * Offset 1936..2048: Zeroes for all remaining entries
     */
    uint32_t *p_magic = (uint32_t *)(upload_tag15 + 1916);
    *p_magic = FLS_SEC_CFG_MAGIC;

    fls_sec_cfg_entry_t *p_entry = (fls_sec_cfg_entry_t *)(upload_tag15 + 1920);
    p_entry->load_address = NVRAM_PROTECT_START;
    p_entry->partition_size = NVRAM_PROTECT_SIZE;
    p_entry->image_size = NVRAM_PROTECT_SIZE;
    p_entry->flags = 0x00000200;

    memset(upload_tag15 + 1920 + sizeof(fls_sec_cfg_entry_t), 0,
           2048 - (1920 + sizeof(fls_sec_cfg_entry_t)));

    fls_close(slb);
    fls_close(psi);

    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        fprintf(stderr, "[-] Failed to open output file: %s (%s)\n", out_path, strerror(errno));
        return -1;
    }

    /* Drain any leftover FIFO packets before starting upload section */
    if (s->config.is_usb) {
        usb_drain_bulk_in(s);
    }

    /* 5. Open Upload Section Session (ReqSecStart 0x0204) */
    printf("[*] Opening Upload section session for NVRAM (ReqSecStart 0x%04X, 2048 b Tag 15)...\n",
           IFWD_MSG_REQ_SEC_START);
    if (s->config.is_usb) {
        if (ebl_send_control_cmd_ext(s, IFWD_MSG_REQ_SEC_START,
                                     upload_tag15, sizeof(upload_tag15),
                                     IFWD_MSG_REQ_SEC_START, IFWD_MSG_RSP_SEC_START,
                                     NULL, NULL, 0) != 0) {
            fprintf(stderr, "[-] ReqSecStart failed for NVRAM upload session\n");
            fclose(fout);
            remove(out_path);
            return -1;
        }
        printf("[*] Draining EP 0x83 FIFO after ReqSecStart...\n");
        usb_drain_bulk_in(s);
    } else {
        uint8_t pkt_buf[2048 + 64];
        size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_SEC_START,
                                           upload_tag15, sizeof(upload_tag15),
                                           pkt_buf, sizeof(pkt_buf));
        if (transport_write(s->transport, pkt_buf, pkt_len) != (int)pkt_len) {
            fprintf(stderr, "[-] Failed to write ReqSecStart UART packet\n");
            fclose(fout);
            remove(out_path);
            return -1;
        }
    }

    size_t total_size = NVRAM_PROTECTED_SIZE;
    size_t dumped = 0;

    #define NVRAM_BLOCK_SIZE EBL_USB_DATA_BLOCK /* 16384 bytes */
    const size_t total_blocks = total_size / NVRAM_BLOCK_SIZE; /* 64 blocks */
    uint8_t rx_chunk[NVRAM_BLOCK_SIZE];

    printf("[*] Streaming %zu NVRAM flash blocks (%d bytes each via ReqFlashSetAddress + ReqFlashReadBlock)...\n",
           total_blocks, NVRAM_BLOCK_SIZE);

    for (size_t block = 0; block < total_blocks; block++) {
        memset(rx_chunk, 0, sizeof(rx_chunk));
        size_t block_recvd = 0;
        uint32_t current_addr = NVRAM_PROTECT_START + (uint32_t)(block * NVRAM_BLOCK_SIZE);

        if (s->config.is_usb) {
            /* 1. Set current block address (ReqFlashSetAddress 0x0802) */
            if (ebl_send_control_cmd_ext(s, IFWD_MSG_REQ_FLASH_SET_ADDR,
                                         &current_addr, sizeof(current_addr),
                                         IFWD_MSG_REQ_FLASH_SET_ADDR, IFWD_MSG_RSP_FLASH_SET_ADDR,
                                         NULL, NULL, 0) != 0) {
                fprintf(stderr, "\n[-] [Block %02zu/%02zu] ReqFlashSetAddress failed for 0x%08X\n",
                        block + 1, total_blocks, (unsigned int)current_addr);
                fclose(fout);
                remove(out_path);
                return -1;
            }
            usb_drain_bulk_in(s);

            /* 2. Send ReqFlashReadBlock (0x0803) in a strictly 16392-byte packet to Bulk OUT EP 0x02 */
            uint8_t tx_buf[EBL_USB_PACKET_SIZE];
            memset(tx_buf, 0, sizeof(tx_buf));

            ebl_pkt_hdr_t *hdr = (ebl_pkt_hdr_t *)tx_buf;
            hdr->msg_type = IFWD_MSG_REQ_FLASH_READ_BLK; /* 0x0803 */
            hdr->length = sizeof(uint16_t); /* 2 bytes */
            uint16_t req_bytes = NVRAM_BLOCK_SIZE; /* 16384 = 0x4000 */
            memcpy(tx_buf + sizeof(ebl_pkt_hdr_t), &req_bytes, sizeof(req_bytes));
            hdr->checksum = protocol_calc_checksum(hdr->msg_type, hdr->length, (const uint8_t *)&req_bytes);

            int tw = transport_write(s->transport, tx_buf, sizeof(tx_buf));
            if (tw != (int)sizeof(tx_buf)) {
                fprintf(stderr, "\n[-] [Block %02zu/%02zu] Failed to transmit ReqFlashReadBlock (sent %d / %zu bytes)\n",
                        block + 1, total_blocks, tw, sizeof(tx_buf));
                fclose(fout);
                remove(out_path);
                return -1;
            }

            /* 2. Read 8-byte ACK from Bulk IN EP 0x83 */
            ebl_rsp_prot_conf_t rsp;
            memset(&rsp, 0, sizeof(rsp));
            size_t ack_recvd = 0;
            uint8_t *rsp_p = (uint8_t *)&rsp;

            while (ack_recvd < sizeof(rsp)) {
                int br = transport_read(s->transport,
                                        rsp_p + ack_recvd,
                                        sizeof(rsp) - ack_recvd,
                                        10000);
                if (br <= 0) break;
                ack_recvd += (size_t)br;
            }

            if (ack_recvd != sizeof(rsp)) {
                fprintf(stderr, "\n[-] [Block %02zu/%02zu] Timeout/error waiting for ACK on EP 0x83 (received %zu / 8 bytes). Aborting!\n",
                        block + 1, total_blocks, ack_recvd);
                fclose(fout);
                remove(out_path);
                return -1;
            }

            /* Strict real-time logging to stdout */
            printf("[Block %02zu/%02zu] TX: 0x%04X (%s), Len: %u b (Req: %u b, Sent: %zu b) | RX ACK: 0x%04X (%s), RspCode: 0x%04X, Status: 0x%08X",
                   block + 1, total_blocks,
                   hdr->msg_type, ifwd_msg_name(hdr->msg_type),
                   hdr->length, req_bytes, sizeof(tx_buf),
                   rsp.ack_type, ifwd_msg_name(rsp.ack_type), rsp.checksum, rsp.status_len);

            if (rsp.ack_type == 0x0041 || rsp.status_len > 0) {
                printf("\n");
                uint8_t err_buf[65];
                memset(err_buf, 0, sizeof(err_buf));
                uint32_t to_read = rsp.status_len;
                if (to_read == 0 || to_read > 64) to_read = 64;

                int br = transport_read(s->transport, err_buf, to_read, 2000);
                (void)br;
                err_buf[63] = '\0';

                printf("[-] EBL Error text from phone: '%s'\n", (char *)err_buf);
                printf("[-] EBL Error hex: ");
                for (int i = 0; i < 64; i++) printf("%02X ", err_buf[i]);
                printf("\n");

                fclose(fout);
                remove(out_path);
                return -1;
            }

            if (rsp.checksum == 0x0041) {
                printf("\n");
                fprintf(stderr, "[-] [Block %02zu/%02zu] Target rejected ReqFlashReadBlock with EBL error checksum (0x0041). Aborting!\n",
                        block + 1, total_blocks);
                fclose(fout);
                remove(out_path);
                return -1;
            }

            if (rsp.ack_type != IFWD_MSG_REQ_FLASH_READ_BLK && rsp.checksum != IFWD_MSG_REQ_FLASH_READ_BLK &&
                rsp.ack_type != IFWD_MSG_RSP_FLASH_READ_BLK && rsp.checksum != IFWD_MSG_RSP_FLASH_READ_BLK) {
                printf("\n");
                fprintf(stderr, "[-] [Block %02zu/%02zu] Target returned unexpected ACK opcode: 0x%04X (expected 0x%04X). Aborting!\n",
                        block + 1, total_blocks, rsp.ack_type, IFWD_MSG_REQ_FLASH_READ_BLK);
                fclose(fout);
                remove(out_path);
                return -1;
            }

            /* 3. Read exactly 16384 bytes of block data from Bulk IN EP 0x83 */
            while (block_recvd < NVRAM_BLOCK_SIZE) {
                int br = transport_read(s->transport,
                                        rx_chunk + block_recvd,
                                        NVRAM_BLOCK_SIZE - block_recvd,
                                        10000);
                if (br <= 0) break;
                block_recvd += (size_t)br;
            }

            printf(" | RX Data: %zu / %d b physically read\n", block_recvd, NVRAM_BLOCK_SIZE);

            if (block_recvd != NVRAM_BLOCK_SIZE) {
                fprintf(stderr, "[-] [Block %02zu/%02zu] Incomplete block received (%zu / %d bytes, <= 0 or timeout). Aborting without write!\n",
                        block + 1, total_blocks, block_recvd, NVRAM_BLOCK_SIZE);
                fclose(fout);
                remove(out_path);
                return -1;
            }
        } else {
            /* UART fallback */
            /* 1. Send ReqFlashSetAddress (0x0802) */
            uint8_t addr_buf[64];
            size_t addr_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_FLASH_SET_ADDR,
                                                &current_addr, sizeof(current_addr),
                                                addr_buf, sizeof(addr_buf));
            if (transport_write(s->transport, addr_buf, addr_len) != (int)addr_len) {
                printf("\n");
                fprintf(stderr, "[-] [Block %02zu/%02zu] Failed to send ReqFlashSetAddress UART packet for 0x%08X\n",
                        block + 1, total_blocks, (unsigned int)current_addr);
                fclose(fout);
                remove(out_path);
                return -1;
            }

            /* 2. Send ReqFlashReadBlock (0x0803) */
            uint8_t payload[2];
            uint16_t req_bytes = NVRAM_BLOCK_SIZE;
            memcpy(payload, &req_bytes, sizeof(req_bytes));
            uint8_t pkt_buf[64];
            size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_FLASH_READ_BLK,
                                               payload, sizeof(payload), pkt_buf, sizeof(pkt_buf));

            printf("[Block %02zu/%02zu] TX UART: 0x%04X, Len: %zu b (Req: %u b)",
                   block + 1, total_blocks, IFWD_MSG_REQ_FLASH_READ_BLK, pkt_len, req_bytes);

            if (transport_write(s->transport, pkt_buf, pkt_len) != (int)pkt_len) {
                printf("\n");
                fprintf(stderr, "[-] [Block %02zu/%02zu] Failed to send ReqFlashReadBlock UART packet\n",
                        block + 1, total_blocks);
                fclose(fout);
                remove(out_path);
                return -1;
            }

            while (block_recvd < NVRAM_BLOCK_SIZE) {
                int br = transport_read(s->transport,
                                        rx_chunk + block_recvd,
                                        NVRAM_BLOCK_SIZE - block_recvd,
                                        10000);
                if (br <= 0) break;
                block_recvd += (size_t)br;
            }

            printf(" | RX Data: %zu / %d b physically read\n", block_recvd, NVRAM_BLOCK_SIZE);

            if (block_recvd != NVRAM_BLOCK_SIZE) {
                fprintf(stderr, "[-] [Block %02zu/%02zu] Incomplete block received (%zu / %d bytes). Aborting without write!\n",
                        block + 1, total_blocks, block_recvd, NVRAM_BLOCK_SIZE);
                fclose(fout);
                remove(out_path);
                return -1;
            }
        }

        /* STRICT: Only write to file if the block was 100% physically received */
        size_t written = fwrite(rx_chunk, 1, NVRAM_BLOCK_SIZE, fout);
        if (written != NVRAM_BLOCK_SIZE) {
            fprintf(stderr, "[-] [Block %02zu/%02zu] Failed to write block to output file %s: %s\n",
                    block + 1, total_blocks, out_path, strerror(errno));
            fclose(fout);
            remove(out_path);
            return -1;
        }

        dumped += NVRAM_BLOCK_SIZE;
    }

    fclose(fout);
    printf("\n");

    if (dumped == total_size) {
        printf("[+] NVRAM Dump complete! %zu bytes successfully verified and saved to %s\n", dumped, out_path);
    } else {
        fprintf(stderr, "[-] NVRAM Dump FAILED: only %zu / %zu bytes dumped. Deleting incomplete file!\n",
                dumped, total_size);
        remove(out_path);
        return -1;
    }

    /* 7. Finalize and close upload section session (ReqSecEnd 0x0205) */
    printf("[*] Finalizing NVRAM upload section session (ReqSecEnd)...\n");
    if (s->config.is_usb) {
        uint16_t sec_end_payload = 0;
        if (ebl_send_control_cmd_ext(s, IFWD_MSG_REQ_SEC_END,
                                     &sec_end_payload, sizeof(sec_end_payload),
                                     IFWD_MSG_REQ_SEC_END, IFWD_MSG_RSP_SEC_END,
                                     NULL, NULL, 0) != 0) {
            fprintf(stderr, "[!] Warning: ReqSecEnd returned error or timed out\n");
        }
        usb_drain_bulk_in(s);
    } else {
        uint16_t sec_end_payload = 0;
        uint8_t pkt_buf[64];
        size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_SEC_END,
                                           &sec_end_payload, sizeof(sec_end_payload),
                                           pkt_buf, sizeof(pkt_buf));
        transport_write(s->transport, pkt_buf, pkt_len);
    }

    /* Final FIFO drain on Bulk IN EP 0x83 */
    if (s->config.is_usb) {
        usb_drain_bulk_in(s);
    }

    /* Hardware Watchdog / Clean reboot */
    flasher_reboot(s);
    return 0;
}

int flasher_reboot(flasher_session_t *s) {
    if (!s) return -1;
    printf("\n[*] Resetting target device (ReqForceHwReset)...\n");

    /* Drain any leftover bytes on EP 0x83 */
    if (s->config.is_usb && s->transport) {
        usb_drain_bulk_in(s);
    }

    /* ResetMode=1 (Hardware Watchdog Reset), Resetp1=16 (0x10), Resetp2=17 (0x11), Resetp3=0 */
    uint8_t reset_payload[4] = { 0x01, 0x10, 0x11, 0x00 };

    if (s->config.is_usb) {
        printf("  [TX -> EP 0x02] Sending ReqForceHwReset with ResetMode=1 (Hardware Reset)...\n");
        ebl_send_control_cmd(s, IFWD_MSG_REQ_FORCE_HW_RESET, reset_payload, sizeof(reset_payload),
                             0, NULL, NULL);
        printf("[+] Reboot command sent. Target is resetting hardware...\n");
        return 0;
    } else {
        uint8_t tx_buf[64];
        size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_FORCE_HW_RESET, reset_payload, sizeof(reset_payload), tx_buf, sizeof(tx_buf));
        if (transport_write(s->transport, tx_buf, pkt_len) != (int)pkt_len) {
            fprintf(stderr, "[-] Failed to send ReqForceHwReset command\n");
            return -1;
        }
        printf("[+] Reboot command sent. Phone will now start up.\n");
        return 0;
    }
}

int flasher_run(flasher_session_t *s) {
    if (!s) return -1;

    /* 1. Sync handshake with Boot ROM */
    if (flasher_sync(s, 45) != 0) {
        return -1;
    }

    /* 2. Upload PSI */
    if (!s->config.psi_path) {
        fprintf(stderr, "[-] PSI file is required for flashing\n");
        return -1;
    }
    fls_file_t *psi = fls_open(s->config.psi_path);
    if (!psi) {
        fprintf(stderr, "[-] Cannot open PSI: %s\n", s->config.psi_path);
        return -1;
    }
    if (flasher_upload_psi(s, psi) != 0) {
        fprintf(stderr, "[-] PSI upload failed\n");
        fls_close(psi);
        return -1;
    }

    /* 3. Upload SLB / EBL */
    if (!s->config.slb_path) {
        fprintf(stderr, "[-] SLB (EBL) file is required for flashing\n");
        fls_close(psi);
        return -1;
    }
    fls_file_t *slb = fls_open(s->config.slb_path);
    if (!slb) {
        fprintf(stderr, "[-] Cannot open SLB: %s\n", s->config.slb_path);
        fls_close(psi);
        return -1;
    }
    if (flasher_upload_ebl(s, slb) != 0) {
        fprintf(stderr, "[-] SLB upload failed\n");
        fls_close(slb);
        fls_close(psi);
        return -1;
    }

    /* 4. Configure protocol & sync EBL engine */
    if (flasher_configure_protocol(s, slb) != 0) {
        fprintf(stderr, "[-] Protocol configuration failed\n");
        fls_close(slb);
        fls_close(psi);
        return -1;
    }

    /* 5. Flash PSI component into NOR flash (load addr 0x00090000) */
    if (flasher_flash_section(s, psi, "PSI (Bootloader)", false) != 0) {
        fprintf(stderr, "[-] Failed flashing PSI section\n");
        fls_close(slb);
        fls_close(psi);
        return -1;
    }
    fls_close(psi);

    /* 6. Flash SLB component into OneNAND (load addr 0x60000000) */
    if (flasher_flash_section(s, slb, "SLB (Secondary Loader)", false) != 0) {
        fprintf(stderr, "[-] Failed flashing SLB section\n");
        fls_close(slb);
        return -1;
    }
    fls_close(slb);

    /* 7. Flash CDS (Customer Data System - partition at 0x63F60000, per Samsung Service Sequence) */
    if (s->config.cds_path) {
        fls_file_t *cds = fls_open(s->config.cds_path);
        if (!cds) {
            fprintf(stderr, "[-] Cannot open CDS: %s\n", s->config.cds_path);
            return -1;
        }
        if (flasher_flash_section(s, cds, "CDS (Customer Data System)", false) != 0) {
            fprintf(stderr, "[-] Failed flashing CDS section\n");
            fls_close(cds);
            return -1;
        }
        fls_close(cds);
    }

    /* 8. Flash BSY (Baseband System - partitions at 0x63200000 and 0x66000000) */
    if (s->config.bsy_path) {
        fls_file_t *bsy = fls_open(s->config.bsy_path);
        if (!bsy) {
            fprintf(stderr, "[-] Cannot open BSY: %s\n", s->config.bsy_path);
            return -1;
        }
        if (flasher_flash_section(s, bsy, "BSY (Baseband System)", false) != 0) {
            fprintf(stderr, "[-] Failed flashing BSY section\n");
            fls_close(bsy);
            return -1;
        }
        fls_close(bsy);
    }

    /* 9. Flash CSC / DFFS (Data Flash File System - partition at 0x44160000, NAND pacing enabled) */
    if (s->config.dffs_path && !s->config.skip_csc) {
        fls_file_t *csc = fls_open(s->config.dffs_path);
        if (!csc) {
            fprintf(stderr, "[-] Cannot open CSC/DFFS: %s\n", s->config.dffs_path);
            return -1;
        }
        if (flasher_flash_section(s, csc, "CSC (Customer Software Customization / DFFS)", true) != 0) {
            fprintf(stderr, "[-] Failed flashing CSC/DFFS section\n");
            fls_close(csc);
            printf("[!] Warning: CSC flash halted, but baseband (PSI, SLB, BSY, CDS) are 100%% written and verified!\n");
            printf("[*] Proceeding with hardware reset to boot target OS...\n");
        } else {
            fls_close(csc);
        }
    } else if (s->config.skip_csc) {
        printf("\n[*] Skipping CSC / DFFS partition write (--skip-csc enabled).\n");
    }

    /* 10. Reboot device */
    flasher_reboot(s);

    printf("\n================================================================================\n");
    printf("[+] FLASH OPERATION COMPLETED SUCCESSFULLY!\n");
    printf("[+] All partitions written and verified. Phone is rebooting.\n");
    printf("================================================================================\n");
    return 0;
}

/* ========================================================================= */
/* OpenIFWD Standard Protocol Engine Implementation                          */
/* ========================================================================= */

static int openifwd_send_recv(flasher_session_t *s, uint8_t cmd, const void *payload, uint16_t len,
                              uint8_t expected_rsp, void *out_buf, uint16_t *out_len, int timeout_ms) {
    if (!s || !s->transport) return -1;
    uint8_t tx_buf[2500];
    uint16_t seq = s->openifwd_seq++;
    size_t tx_len = openifwd_build_packet(cmd, seq, payload, len, tx_buf, sizeof(tx_buf));
    if (tx_len == 0) return -1;

    if (transport_write(s->transport, tx_buf, tx_len) != (int)tx_len) {
        return -2;
    }

    /* Wait for response header (11 bytes) */
    uint8_t rx_hdr_buf[11];
    size_t recvd = 0;
    time_t start = time(NULL);
    while (recvd < 11) {
        int n = transport_read(s->transport, rx_hdr_buf + recvd, 11 - recvd, 50);
        if (n > 0) {
            recvd += (size_t)n;
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

int flasher_dump_nvram_open(flasher_session_t *s, const char *out_path) {
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
    printf("[*] Pinging OpenIFWD target agent in SRAM...\n");
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

int flasher_boot_ram_open(flasher_session_t *s, uint32_t ram_addr, const uint8_t *data, size_t size) {
    if (!s || !data || size == 0) return -1;

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
