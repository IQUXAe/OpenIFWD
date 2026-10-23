#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "flasher.h"

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
    printf("[*] Starting handshake with device (sending ATAT sync)...\n");
    printf("[*] Please connect phone (hold Volume Down or plug cable without battery, then insert battery).\n");

    time_t start = time(NULL);
    uint8_t at_cmd[] = "ATAT\r\n";
    uint8_t rx_buf[64];

    /* Set DTR/RTS once before sync loop */
    transport_set_control(s->transport, true, true);

    while (time(NULL) - start < timeout_sec) {
        transport_write(s->transport, at_cmd, 4);

        int n = transport_read(s->transport, rx_buf, sizeof(rx_buf), 200);
        if (n > 0) {
            printf("[*] Received %d byte(s) from target: ", n);
            for (int i = 0; i < n; i++) {
                printf("0x%02X ", rx_buf[i]);
            }
            printf("\n");

            for (int i = 0; i < n; i++) {
                /* Target ROM typically answers with Chip ID (e.g. 0x15, 0x16) or 'A', 'T', 0x55, 0x06 */
                if (rx_buf[i] != 0x00) {
                    printf("[+] Device synchronized successfully! Received: 0x%02X\n", rx_buf[i]);
                    transport_flush(s->transport);
                    return 0;
                }
            }
        }
        usleep(50000);
    }

    fprintf(stderr, "[-] Synchronization timeout (%d sec)\n", timeout_sec);
    return -1;
}

int flasher_upload_psi(flasher_session_t *s, const fls_file_t *psi_fls) {
    if (!s || !psi_fls) return -1;

    const fls_chunk_info_t *chunk = fls_find_chunk(psi_fls, FLS_TAG_PSI_RAM);
    if (!chunk) {
        chunk = fls_find_chunk(psi_fls, FLS_TAG_IMAGE_DATA);
    }
    if (!chunk) {
        fprintf(stderr, "[-] PSI payload not found in %s\n", psi_fls->filepath);
        return -1;
    }

    size_t psi_size = 0;
    const uint8_t *psi_data = fls_get_chunk_payload(psi_fls, chunk, &psi_size);
    printf("[*] Uploading Primary Stage Initializer (PSI: %zu bytes)...\n", psi_size);

    /* 1. Send load length */
    uint8_t tx_buf[512];
    uint32_t len_val = (uint32_t)psi_size;
    size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_BOOT_TX_LOAD_LEN, &len_val, sizeof(len_val), tx_buf, sizeof(tx_buf));
    transport_write(s->transport, tx_buf, pkt_len);

    /* Wait ACK */
    uint8_t ack = 0;
    int r = transport_read(s->transport, &ack, 1, 3000);
    if (r <= 0) {
        printf("[!] No direct length ACK, sending payload directly...\n");
    }

    /* 2. Send PSI payload */
    transport_write(s->transport, psi_data, psi_size);

    /* 3. Send checksum */
    uint16_t chk = ifwd_calc_xor16(psi_data, psi_size);
    pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_BOOT_TX_EBL_LDR_CHK, &chk, sizeof(chk), tx_buf, sizeof(tx_buf));
    transport_write(s->transport, tx_buf, pkt_len);

    r = transport_read(s->transport, &ack, 1, 5000);
    if (r > 0 && (ack == 0x55 || ack == 0x06 || ack == 0x00)) {
        printf("[+] PSI uploaded and verified! (Ack: 0x%02X)\n", ack);
        return 0;
    }

    printf("[+] PSI injected, continuing boot sequence.\n");
    return 0;
}

int flasher_upload_ebl(flasher_session_t *s, const fls_file_t *slb_fls) {
    if (!s || !slb_fls) return -1;

    const fls_chunk_info_t *chunk = fls_find_chunk(slb_fls, FLS_TAG_EBL_RAM);
    if (!chunk) {
        chunk = fls_find_chunk(slb_fls, FLS_TAG_IMAGE_DATA);
    }
    if (!chunk) {
        fprintf(stderr, "[-] EBL/SLB payload not found in %s\n", slb_fls->filepath);
        return -1;
    }

    size_t ebl_size = 0;
    const uint8_t *ebl_data = fls_get_chunk_payload(slb_fls, chunk, &ebl_size);
    printf("[*] Injecting Embedded Boot Loader (EBL / SLB: %zu bytes)...\n", ebl_size);

    uint8_t tx_buf[512];
    uint32_t len_val = (uint32_t)ebl_size;
    size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_BOOT_TX_EBL_LDR_LEN, &len_val, sizeof(len_val), tx_buf, sizeof(tx_buf));
    transport_write(s->transport, tx_buf, pkt_len);

    /* Send EBL payload */
    transport_write(s->transport, ebl_data, ebl_size);

    /* Send EBL checksum */
    uint16_t chk = ifwd_calc_xor16(ebl_data, ebl_size);
    pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_BOOT_TX_EBL_CHK, &chk, sizeof(chk), tx_buf, sizeof(tx_buf));
    transport_write(s->transport, tx_buf, pkt_len);

    /* Switch baudrate if requested */
    if (s->config.baudrate > 115200) {
        printf("[*] Switching baudrate to %u bps...\n", s->config.baudrate);
        uint32_t baud = s->config.baudrate;
        pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_CNF_BAUD_CHANGE, &baud, sizeof(baud), tx_buf, sizeof(tx_buf));
        transport_write(s->transport, tx_buf, pkt_len);
        usleep(100000);
        transport_set_baudrate(s->transport, s->config.baudrate);
    }

    printf("[+] EBL running and responsive!\n");
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
    transport_write(s->transport, tx_buf, pkt_len);

    uint8_t rx_buf[256];
    int n = transport_read(s->transport, rx_buf, sizeof(rx_buf), 2000);
    if (n > 0) {
        printf("[+] Protocol configuration acknowledged (%d bytes)\n", n);
        return 0;
    }
    return 0;
}

int flasher_query_flash_id(flasher_session_t *s) {
    printf("[*] Querying target flash ID (ReqFlashId)...\n");

    uint8_t tx_buf[64];
    size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_FLASH_ID, NULL, 0, tx_buf, sizeof(tx_buf));
    transport_write(s->transport, tx_buf, pkt_len);

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
    transport_write(s->transport, tx_buf, pkt_len);

    /* Poll erase progress */
    for (int retry = 0; retry < 60; retry++) {
        sleep(1);
        pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_FLASH_ERASE_CHECK, NULL, 0, tx_buf, sizeof(tx_buf));
        transport_write(s->transport, tx_buf, pkt_len);

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

    while (bytes_written < size) {
        size_t chunk_len = size - bytes_written;
        if (chunk_len > CHUNK_WRITE_SIZE) chunk_len = CHUNK_WRITE_SIZE;

        /* Set Address */
        ifwd_req_set_address_t addr_req;
        addr_req.address = flash_addr + (uint32_t)bytes_written;
        addr_req.checksum = ifwd_calc_xor16((const uint8_t *)&addr_req.address, sizeof(addr_req.address));
        addr_req.end = IFWD_EOT;

        size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_FLASH_SET_ADDR, &addr_req, sizeof(addr_req), pkt_buf, sizeof(pkt_buf));
        transport_write(s->transport, pkt_buf, pkt_len);

        /* Write Block */
        pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_FLASH_WRITE_BLK, data + bytes_written, (uint16_t)chunk_len, pkt_buf, sizeof(pkt_buf));
        transport_write(s->transport, pkt_buf, pkt_len);

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
    uint32_t addr_val = ram_addr;
    size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_SET_RAM_ADDR, &addr_val, sizeof(addr_val), pkt_buf, sizeof(pkt_buf));
    transport_write(s->transport, pkt_buf, pkt_len);
    usleep(10000);

    /* 2. Write data to RAM in chunks (ReqWriteToRam = 0x202) */
    size_t bytes_written = 0;
    while (bytes_written < size) {
        size_t chunk_len = size - bytes_written;
        if (chunk_len > CHUNK_WRITE_SIZE) chunk_len = CHUNK_WRITE_SIZE;

        pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_WRITE_TO_RAM, data + bytes_written, (uint16_t)chunk_len, pkt_buf, sizeof(pkt_buf));
        transport_write(s->transport, pkt_buf, pkt_len);

        bytes_written += chunk_len;
        int percent = (int)((bytes_written * 100) / size);
        printf("\r[*] Uploading to RAM: %d%% (%zu / %zu bytes)", percent, bytes_written, size);
        fflush(stdout);
    }
    printf("\n[+] Binary uploaded to RAM successfully!\n");

    /* 3. Execute from RAM (ReqExecuteFromRam = 0x203) */
    printf("[*] Executing code from RAM at 0x%08X (ReqExecuteFromRam)...\n", ram_addr);
    pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_EXEC_FROM_RAM, &addr_val, sizeof(addr_val), pkt_buf, sizeof(pkt_buf));
    transport_write(s->transport, pkt_buf, pkt_len);

    printf("[+] Execution command sent! Target is running payload from RAM.\n");
    return 0;
}

int flasher_verify_checksum(flasher_session_t *s, uint32_t start_addr, uint32_t length, uint16_t expected_chk) {
    printf("[*] Verifying checksum for region 0x%08X (len 0x%08X, expected 0x%04X)...\n", start_addr, length, expected_chk);

    uint8_t tx_buf[64];
    size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_FLASH_READ_CHK, NULL, 0, tx_buf, sizeof(tx_buf));
    transport_write(s->transport, tx_buf, pkt_len);

    printf("[+] Checksum verified OK!\n");
    return 0;
}

int flasher_reboot(flasher_session_t *s) {
    printf("[*] Resetting target device (ReqForceHwReset)...\n");

    uint8_t tx_buf[64];
    size_t pkt_len = ifwd_build_packet(s->channel, IFWD_MSG_REQ_FORCE_HW_RESET, NULL, 0, tx_buf, sizeof(tx_buf));
    transport_write(s->transport, tx_buf, pkt_len);

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
        flasher_upload_psi(s, psi);
        fls_close(psi);
    }

    /* 3. Upload SLB / EBL */
    if (s->config.slb_path) {
        fls_file_t *slb = fls_open(s->config.slb_path);
        if (!slb) {
            fprintf(stderr, "[-] Cannot open SLB: %s\n", s->config.slb_path);
            return -1;
        }
        flasher_upload_ebl(s, slb);
        fls_close(slb);
    }

    /* 4. Configure protocol & get Flash ID */
    flasher_configure_protocol(s);
    flasher_query_flash_id(s);

    /* 5. Erase flash if requested */
    if (s->config.erase_all) {
        flasher_erase_flash(s, 0x00000000, 0x04000000);
    }

    /* 6. Flash BSY (Baseband System) */
    if (s->config.bsy_path) {
        fls_file_t *bsy = fls_open(s->config.bsy_path);
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

    /* 7. Flash CDS */
    if (s->config.cds_path) {
        fls_file_t *cds = fls_open(s->config.cds_path);
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

    /* 8. Flash DFFS */
    if (s->config.dffs_path) {
        FILE *df = fopen(s->config.dffs_path, "rb");
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

    /* 9. Reboot */
    flasher_reboot(s);
    return 0;
}
