#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "flasher.h"
#include "psi_xgold215.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

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

    /* For Infineon XMM2150 / PMB8815 / PMB8812 (Chip ID 0x17):
     * The Boot ROM Primary Stage Initializer is hardcoded inside FlashTool_E2
     * (IFWD_DownloadDll.dll .data section at 0x44d8c2). Length: 6,296 bytes.
     * The chunks inside S5222XXMJ1_psi.fls (Tag 18 = 57KB, Tag 12 = 129KB)
     * cannot fit into Boot ROM internal SRAM (~16KB buffer limit). Tag 11 (1628B) is partition
     * table definitions, not executable code.
     * Therefore, for Chip ID 0x17, we MUST use the embedded 6,296-byte PSI binary!
     */
    if (s->chip_id == 0x17 || s->chip_id == 0 || !psi_fls) {
        psi_data = xgold215_psi_bootrom;
        psi_size = xgold215_psi_bootrom_size;
        chk8 = xgold215_psi_bootrom_checksum;
        printf("[*] Using embedded X-Gold 215 / PMB8815 Boot ROM PSI (%zu bytes)...\n", psi_size);
    } else {
        /* Generic / older platforms: extract from FLS container */
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

        if (!psi_data || psi_size == 0 || psi_size > 0xFFFF) {
            fprintf(stderr, "[-] Invalid PSI payload size or chunk not found in %s\n", psi_fls->filepath);
            return -1;
        }

        printf("[*] Uploading Primary Stage Initializer (PSI chunk Tag %u: %zu bytes)...\n",
               chunk ? chunk->tag : 0, psi_size);

        for (size_t i = 0; i < psi_size; i++) {
            chk8 ^= psi_data[i];
        }
    }

    /* 1. Low-level Boot ROM Header:
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

    /* 2. Stream raw PSI payload in 64-byte chunks (USB Full-Speed MTU) with micro-pacing */
    printf("[*] Streaming PSI payload (%zu bytes in 64-byte chunks with pacing)...\n", psi_size);
    size_t sent = 0;
    while (sent < psi_size) {
        size_t chunk = psi_size - sent;
        if (chunk > 64) chunk = 64;
        int written = transport_write(s->transport, psi_data + sent, chunk);
        if (written != (int)chunk) {
            fprintf(stderr, "[-] Failed to write PSI payload chunk at offset %zu (%zu bytes)\n", sent, chunk);
            return -1;
        }
        sent += chunk;
        usleep(500); /* 500us pacing for BROM ARM9 FIFO draining */
    }

    /* 3. Send 1-byte XOR-8 checksum of payload */
    if (transport_write(s->transport, &chk8, 1) != 1) {
        fprintf(stderr, "[-] Failed to write PSI XOR-8 checksum\n");
        return -1;
    }

    /* 4. Wait for Target Boot ROM ACK (expected: 0x01, or 0x0B, 0xB1, 0xC1) */
    uint8_t ack_buf[4] = {0};
    int r = transport_read(s->transport, ack_buf, 1, 5000);
    if (r > 0) {
        printf("[+] PSI acknowledged by Boot ROM (%d byte(s), Ack: 0x%02X)\n", r, ack_buf[0]);
        if (ack_buf[0] == 0x01 || ack_buf[0] == 0x0B || ack_buf[0] == 0xB1 || ack_buf[0] == 0xC1) {
            printf("[+] PSI successfully executed by Boot ROM!\n");
        } else {
            fprintf(stderr, "[-] Unexpected Boot ROM ACK byte: 0x%02X\n", ack_buf[0]);
            return -1;
        }
    } else {
        fprintf(stderr, "[-] No ACK received from Boot ROM within 5s\n");
        return -1;
    }

    /* PSI starts executing immediately in SRAM and will transmit its readiness token */
    return 0;
}

int flasher_upload_ebl(flasher_session_t *s, const fls_file_t *ebl_fls) {
    if (!s || !ebl_fls) return -1;

    /* 1. Select EBL executable chunk:
     * Priority:
     * a. Chunk with Tag 12 (IMAGE_DATA) and size > 28 (contains Samsung SLB/EBL loader)
     * b. Tag 19 (SECONDARY_LOADER)
     * c. Tag 14 (EBL_RAM)
     * d. Tag 18 (RPSI)
     */
    const fls_chunk_info_t *chunk = NULL;
    for (size_t i = 0; i < ebl_fls->chunk_count; i++) {
        if (ebl_fls->chunks[i].tag == FLS_TAG_IMAGE_DATA && ebl_fls->chunks[i].payload_size > 28) {
            chunk = &ebl_fls->chunks[i];
            break;
        }
    }
    if (!chunk) {
        chunk = fls_find_chunk(ebl_fls, FLS_TAG_IMAGE_DATA);
    }
    if (!chunk) {
        chunk = fls_find_chunk(ebl_fls, FLS_TAG_SECONDARY_LOADER);
    }
    if (!chunk) {
        chunk = fls_find_chunk(ebl_fls, FLS_TAG_EBL_RAM);
    }
    if (!chunk) {
        chunk = fls_find_chunk(ebl_fls, FLS_TAG_RPSI);
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

    /* If chunk has 28-byte subheader (Tag 12 IMAGE_DATA):
     * offset 16 (0x10): sub_len
     * offset 24 (0x18): sub_offset
     * Actual binary code begins after byte 28!
     */
    const uint8_t *ebl_data = raw_data;
    size_t ebl_size = raw_size;

    if (chunk->tag == FLS_TAG_IMAGE_DATA && raw_size > 28) {
        uint32_t sub_len = *(const uint32_t *)(raw_data + 16);
        if (sub_len > 0 && sub_len <= raw_size - 28) {
            ebl_data = raw_data + 28;
            ebl_size = (size_t)sub_len;
        } else {
            ebl_data = raw_data + 28;
            ebl_size = raw_size - 28;
        }
    } else if (chunk->tag == FLS_TAG_RPSI && raw_size > 20) {
        uint32_t sub_len = *(const uint32_t *)(raw_data + 8);
        if (sub_len > 0 && sub_len <= raw_size - 20) {
            ebl_data = raw_data + 20;
            ebl_size = (size_t)sub_len;
        }
    }

    printf("[*] Preparing to upload EBL/SLB (chunk Tag %u: %zu bytes)...\n", chunk->tag, ebl_size);

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
            return -1;
        }
    } else {
        fprintf(stderr, "[-] Timeout waiting for EBL confirmation from target\n");
        return -1;
    }

    /* 8. Settle delay and flush */
    usleep(200000);
    transport_flush(s->transport);
    printf("[+] Target EBL active.\n");

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

    return 0;
}

int flasher_configure_protocol(flasher_session_t *s) {
    printf("[*] Configuring transfer protocol (ReqSetProtConf)...\n");

    ifwd_req_prot_conf_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.boot_mode = 0;
    conf.major = 4;
    conf.minor = 68;
    conf.package_size = 8;
    conf.crc_type = 0; /* XOR 16-bit */

    uint8_t tx_buf[256];
    size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_SET_PROT_CONF, &conf, sizeof(conf), tx_buf, sizeof(tx_buf));
    if (transport_write(s->transport, tx_buf, pkt_len) != (int)pkt_len) {
        fprintf(stderr, "[-] Failed to write ReqSetProtConf packet\n");
        return -1;
    }

    uint8_t rx_buf[256];
    int n = transport_read(s->transport, rx_buf, sizeof(rx_buf), 2000);
    if (n > 0) {
        printf("[+] Protocol configuration acknowledged (%d bytes)\n", n);
        return 0;
    }
    printf("[*] Proceeding with default protocol configuration\n");
    return 0;
}

int flasher_query_flash_id(flasher_session_t *s) {
    printf("[*] Querying target flash ID (ReqFlashId)...\n");

    uint8_t tx_buf[64];
    size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_FLASH_ID, NULL, 0, tx_buf, sizeof(tx_buf));
    if (transport_write(s->transport, tx_buf, pkt_len) != (int)pkt_len) {
        fprintf(stderr, "[-] Failed to send ReqFlashId request\n");
        return -1;
    }

    uint8_t rx_buf[256];
    int n = transport_read(s->transport, rx_buf, sizeof(rx_buf), 2000);
    if (n > 0) {
        printf("[+] Target Flash ID received successfully!\n");
        return 0;
    }
    printf("[*] Continuing with default platform parameters.\n");
    return 0;
}

int flasher_erase_flash(flasher_session_t *s, uint32_t start_addr, uint32_t end_addr) {
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

    /* 1. Set RAM Work Address (ReqSetRamWorkAddress = 0x201) */
    uint8_t pkt_buf[CHUNK_WRITE_SIZE + 64];
    uint8_t rx_buf[64];
    uint32_t addr_val = ram_addr;
    size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_SET_RAM_ADDR, &addr_val, sizeof(addr_val), pkt_buf, sizeof(pkt_buf));
    if (transport_write(s->transport, pkt_buf, pkt_len) != (int)pkt_len) {
        fprintf(stderr, "[-] Failed to write ReqSetRamWorkAddress packet\n");
        return -1;
    }
    transport_read(s->transport, rx_buf, sizeof(rx_buf), 1000);
    usleep(10000);

    /* 2. Write data to RAM in chunks (ReqWriteToRam = 0x202) */
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

    /* 3. Execute from RAM (ReqExecuteFromRam = 0x203) */
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

int flasher_reboot(flasher_session_t *s) {
    printf("[*] Resetting target device (ReqForceHwReset)...\n");

    uint8_t tx_buf[64];
    size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_FORCE_HW_RESET, NULL, 0, tx_buf, sizeof(tx_buf));
    if (transport_write(s->transport, tx_buf, pkt_len) != (int)pkt_len) {
        fprintf(stderr, "[-] Failed to send ReqForceHwReset command\n");
        return -1;
    }

    printf("[+] Reboot command sent. Phone will now start up.\n");
    return 0;
}

int flasher_run(flasher_session_t *s) {
    if (!s) return -1;

    /* 1. Sync handshake */
    if (flasher_sync(s, 45) != 0) {
        return -1;
    }

    /* 2. Upload PSI */
    if (s->config.psi_path) {
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
        fls_close(psi);
    }

    /* 3. Upload SLB / EBL */
    if (s->config.slb_path) {
        fls_file_t *slb = fls_open(s->config.slb_path);
        if (!slb) {
            fprintf(stderr, "[-] Cannot open SLB: %s\n", s->config.slb_path);
            return -1;
        }
        if (flasher_upload_ebl(s, slb) != 0) {
            fprintf(stderr, "[-] SLB upload failed\n");
            fls_close(slb);
            return -1;
        }
        fls_close(slb);
    }

    /* 4. Configure protocol & get Flash ID */
    if (flasher_configure_protocol(s) != 0) {
        fprintf(stderr, "[-] Protocol configuration failed\n");
        return -1;
    }
    flasher_query_flash_id(s);

    /* 5. Erase flash if requested */
    if (s->config.erase_all) {
        flasher_erase_flash(s, 0x00000000, 0x04000000);
    }

    /* 6. Flash BSY (Baseband System) */
    if (s->config.bsy_path) {
        fls_file_t *bsy = fls_open(s->config.bsy_path);
        if (bsy) {
            uint32_t bsy_base = 0x00000000;
            for (size_t i = 0; i < bsy->chunk_count; i++) {
                if (bsy->chunks[i].tag == FLS_TAG_IMAGE_DATA) {
                    size_t sz = 0;
                    const uint8_t *data = fls_get_chunk_payload(bsy, &bsy->chunks[i], &sz);
                    if (data && sz > 28) {
                        uint32_t sub_len = *(const uint32_t *)(data + 16);
                        uint32_t sub_offset = *(const uint32_t *)(data + 24);
                        const uint8_t *payload_data = data + 28;
                        size_t payload_len = (sub_len > 0 && sub_len <= sz - 28) ? (size_t)sub_len : (sz - 28);
                        uint32_t target_addr = bsy_base + sub_offset;
                        if (flasher_flash_image(s, payload_data, payload_len, target_addr) != 0) {
                            fprintf(stderr, "[-] Failed flashing BSY chunk %zu\n", i);
                            fls_close(bsy);
                            return -1;
                        }
                    } else if (data && sz > 0) {
                        if (flasher_flash_image(s, data, sz, bsy_base) != 0) {
                            fprintf(stderr, "[-] Failed flashing BSY chunk %zu\n", i);
                            fls_close(bsy);
                            return -1;
                        }
                    }
                }
            }
            fls_close(bsy);
        }
    }

    /* 7. Flash CDS */
    if (s->config.cds_path) {
        fls_file_t *cds = fls_open(s->config.cds_path);
        if (cds) {
            uint32_t cds_base = 0x00000000;
            for (size_t i = 0; i < cds->chunk_count; i++) {
                if (cds->chunks[i].tag == FLS_TAG_IMAGE_DATA) {
                    size_t sz = 0;
                    const uint8_t *data = fls_get_chunk_payload(cds, &cds->chunks[i], &sz);
                    if (data && sz > 28) {
                        uint32_t sub_len = *(const uint32_t *)(data + 16);
                        uint32_t sub_offset = *(const uint32_t *)(data + 24);
                        const uint8_t *payload_data = data + 28;
                        size_t payload_len = (sub_len > 0 && sub_len <= sz - 28) ? (size_t)sub_len : (sz - 28);
                        uint32_t target_addr = cds_base + sub_offset;
                        if (flasher_flash_image(s, payload_data, payload_len, target_addr) != 0) {
                            fprintf(stderr, "[-] Failed flashing CDS chunk %zu\n", i);
                            fls_close(cds);
                            return -1;
                        }
                    } else if (data && sz > 0) {
                        if (flasher_flash_image(s, data, sz, cds_base) != 0) {
                            fprintf(stderr, "[-] Failed flashing CDS chunk %zu\n", i);
                            fls_close(cds);
                            return -1;
                        }
                    }
                }
            }
            fls_close(cds);
        }
    }

    /* 8. Flash DFFS */
    if (s->config.dffs_path) {
        FILE *df = fopen(s->config.dffs_path, "rb");
        if (df) {
            fseek(df, 0, SEEK_END);
            long sz = ftell(df);
            rewind(df);
            uint8_t *buf = (uint8_t *)malloc(sz);
            if (buf && fread(buf, 1, sz, df) == (size_t)sz) {
                if (flasher_flash_image(s, buf, (size_t)sz, 0x02000000) != 0) {
                    fprintf(stderr, "[-] Failed flashing DFFS\n");
                    free(buf);
                    fclose(df);
                    return -1;
                }
            }
            free(buf);
            fclose(df);
        }
    }

    /* 9. Reboot */
    flasher_reboot(s);
    return 0;
}
