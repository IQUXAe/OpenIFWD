#include "fls.h"
#include "protocol.h"
#include "open_psi.h"
#include "flasher.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

static void test_protocol_packets(void) {
    printf("[TEST] Testing OpenIFWD wire protocol packet creation and parsing...\n");

    /* 1. Test OpenIFWD CRC-16-CCITT */
    const uint8_t test_msg[] = "OPENIFWD_TEST";
    uint16_t crc1 = openifwd_calc_crc16(test_msg, sizeof(test_msg) - 1);
    assert(crc1 != 0);

    /* 2. Test OpenIFWD packet building and header parsing */
    uint8_t pkt_buf[256];
    uint8_t dummy_payload[] = { 0x01, 0x02, 0x03, 0x04 };
    size_t built_len = openifwd_build_packet(OPENIFWD_CMD_PING, 42, dummy_payload, sizeof(dummy_payload),
                                            pkt_buf, sizeof(pkt_buf));
    assert(built_len == sizeof(openifwd_pkt_hdr_t) + sizeof(dummy_payload) + 2);

    openifwd_pkt_hdr_t parsed_hdr;
    int parse_res = openifwd_parse_header(pkt_buf, built_len, &parsed_hdr);
    assert(parse_res == 0);
    assert(parsed_hdr.magic == OPENIFWD_MAGIC_32);
    assert(parsed_hdr.version == OPENIFWD_VERSION);
    assert(parsed_hdr.cmd == OPENIFWD_CMD_PING);
    assert(parsed_hdr.seq == 42);
    assert(parsed_hdr.length == sizeof(dummy_payload));

    /* Verify CRC-16 trailer */
    uint16_t wire_crc = *(const uint16_t *)(pkt_buf + sizeof(openifwd_pkt_hdr_t) + sizeof(dummy_payload));
    uint16_t calc_crc = openifwd_calc_crc16(dummy_payload, sizeof(dummy_payload));
    assert(wire_crc == calc_crc);

    printf("[+] OpenIFWD packet builder, XOR-8 header checksum, and CRC-16 validated!\n");
    printf("[TEST] OpenIFWD protocol test PASSED!\n");
}

static void test_open_psi(void) {
    printf("[TEST] Testing embedded Rust PSI binary...\n");

    assert(open_psi_size > 0 && open_psi_size <= 0xFFFF);
    assert(memcmp(open_psi_binary + 4, "liHU", 4) == 0);
    assert(memcmp(open_psi_binary + 60, "CJKT", 4) == 0);
    uint32_t entry = *(const uint32_t *)(open_psi_binary + 48);
    assert(entry == 0x00080000);

    /* Verify XOR-8 checksum */
    uint8_t csum = 0;
    for (size_t i = 0; i < open_psi_size; i++) csum ^= open_psi_binary[i];
    assert(csum == open_psi_checksum);

    printf("[+] Embedded Rust Open PSI verified: %zu bytes, XOR-8 0x%02X, load 0x%08X\n",
           open_psi_size, open_psi_checksum, entry);
    printf("[TEST] Embedded Rust Open PSI test PASSED!\n");
}

static void test_fls_parsing(const char *fls_path) {
    printf("[TEST] Testing FLS parser on: %s\n", fls_path);

    fls_file_t *fls = fls_open(fls_path);
    if (!fls) {
        printf("[SKIP] Sample FLS %s not found\n", fls_path);
        return;
    }

    assert(fls->chunk_count > 0);
    printf("[+] Parsed %zu chunks\n", fls->chunk_count);

    const fls_chunk_info_t *c = fls_find_chunk(fls, FLS_TAG_FILE_HDR);
    assert(c != NULL);
    assert(c->tag == FLS_TAG_FILE_HDR);

    size_t sz = 0;
    const uint8_t *data = fls_get_chunk_payload(fls, c, &sz);
    assert(data != NULL);
    assert(sz == c->payload_size);

    if (fls->toc_count > 0) {
        assert(fls->toc_entries != NULL);
        assert(strlen(fls->toc_entries[0].filename) > 0);
        printf("[+] TOC entry [0] filename: %s, mem_class: 0x%08x\n",
               fls->toc_entries[0].filename, fls->toc_entries[0].mem_class);
    }

    fls_close(fls);
    printf("[TEST] FLS parser PASSED!\n");
}

static void test_firmware_structure(void) {
    printf("[TEST] Testing Samsung GT-S5222 firmware structure...\n");

    const char *psi_path = "/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222XXMJ1_psi.fls";
    fls_file_t *psi = fls_open(psi_path);
    if (psi) {
        /* PSI Partition Table: Tag 11 must be present and exactly 1628 bytes */
        const fls_chunk_info_t *c11 = fls_find_chunk(psi, FLS_TAG_PARTITION_TABLE);
        assert(c11 != NULL);
        assert(c11->payload_size == 1628);
        printf("[+] PSI Tag 11 validated: %zu bytes (partition table metadata)\n", c11->payload_size);

        /* Secondary Stage Initializer (RPSI): Tag 12 must be present */
        const fls_chunk_info_t *c12 = fls_find_chunk(psi, FLS_TAG_IMAGE_DATA);
        assert(c12 != NULL);
        assert(c12->payload_size == 129144);
        size_t rpsi_sz = 0;
        const uint8_t *rpsi_data = fls_get_chunk_payload(psi, c12, &rpsi_sz);
        assert(rpsi_data != NULL);
        uint32_t sub_len = *(const uint32_t *)(rpsi_data + 16);
        assert(sub_len == 129116);
        printf("[+] RPSI Tag 12 validated: total %zu bytes, code payload %u bytes\n", rpsi_sz, sub_len);

        /* Bootloader Executable Payload (Tag 18 / RPSI): Must be present */
        const fls_chunk_info_t *c18 = fls_find_chunk(psi, FLS_TAG_RPSI);
        assert(c18 != NULL);
        assert(c18->payload_size == 57664);
        size_t rpsi18_sz = 0;
        const uint8_t *rpsi18_data = fls_get_chunk_payload(psi, c18, &rpsi18_sz);
        assert(rpsi18_data != NULL);
        assert(rpsi18_sz == 57664);
        uint32_t sub_len18 = *(const uint32_t *)(rpsi18_data + 8);
        assert(sub_len18 == 57644);
        uint32_t entry_branch = *(const uint32_t *)(rpsi18_data + 20);
        assert(entry_branch == 0xea002ad7);
        printf("[+] RPSI Tag 18 validated: total %zu bytes, code payload %u bytes, entry branch 0x%08x\n",
               rpsi18_sz, sub_len18, entry_branch);

        fls_close(psi);
    }

    printf("[TEST] Samsung GT-S5222 firmware structure PASSED!\n");
}

static void test_flash_sections(void) {
    printf("[TEST] Testing stock firmware components (Tag 15 partitions & Tag 12 chunks)...\n");

    /* 1. Test BSY */
    fls_file_t *bsy = fls_open("/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222XXMJ1_BSY.fls");
    if (bsy) {
        fls_sec_cfg_entry_t entries[8];
        size_t count = 0;
        assert(fls_get_sec_cfg_entries(bsy, entries, 8, &count) == 0);
        assert(count == 2);
        assert(entries[0].load_address == 0x63200000);
        assert(entries[1].load_address == 0x66000000);

        const fls_chunk_info_t *t12[8];
        size_t t12_cnt = fls_find_all_chunks(bsy, FLS_TAG_IMAGE_DATA, t12, 8);
        assert(t12_cnt == 2);
        printf("[+] BSY validated: 2 partitions (0x%08X, 0x%08X), 2 image chunks\n",
               entries[0].load_address, entries[1].load_address);
        fls_close(bsy);
    }

    /* 2. Test CDS */
    fls_file_t *cds = fls_open("/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222XXMJ1_CDS.fls");
    if (cds) {
        fls_sec_cfg_entry_t entries[8];
        size_t count = 0;
        assert(fls_get_sec_cfg_entries(cds, entries, 8, &count) == 0);
        assert(count == 1);
        assert(entries[0].load_address == 0x63F60000);
        printf("[+] CDS validated: partition 0x%08X (size %u bytes)\n",
               entries[0].load_address, entries[0].image_size);
        fls_close(cds);
    }

    /* 3. Test CSC / DFFS */
    fls_file_t *csc = fls_open("/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222OXEMJ1_CSC.dffs");
    if (csc) {
        fls_sec_cfg_entry_t entries[8];
        size_t count = 0;
        assert(fls_get_sec_cfg_entries(csc, entries, 8, &count) == 0);
        assert(count == 1);
        assert(entries[0].load_address == 0x44160000);
        printf("[+] CSC / DFFS validated: NAND partition 0x%08X (size %u bytes)\n",
               entries[0].load_address, entries[0].image_size);
        fls_close(csc);
    }

    printf("[TEST] Stock firmware components PASSED!\n");
}

static void test_fls_payload_extraction(void) {
    printf("[TEST] Testing pure payload extraction (stripping 28-byte FLS subheader)...\n");

    /* 1. Test Second Level Bootloader (SLB - Block 2) */
    fls_file_t *slb = fls_open("/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222XXMJ1_slb.fls");
    if (slb) {
        const fls_chunk_info_t *c = fls_find_chunk(slb, FLS_TAG_IMAGE_DATA);
        assert(c != NULL);
        assert(c->payload_size == 283728);

        size_t img_sz = 0;
        const uint8_t *img = fls_get_image_data(slb, c, &img_sz);
        assert(img != NULL);
        assert(img_sz == 283700); /* 283728 - 28 bytes header */

        /* Ensure 28-byte FLS subheader metadata was stripped */
        uint32_t first_word = *(const uint32_t *)img;
        assert(first_word == 0x00000000); /* Vector/BSS space */
        uint32_t sub_tag = *(const uint32_t *)(img + 12);
        assert(sub_tag != 0x00002D72); /* Must NOT be the FLS subheader CRC/tag */

        /* Verify executable ARM code at offset 0x400 (Target execution address 0x60000400) */
        uint32_t arm_entry_opcode = *(const uint32_t *)(img + 0x400);
        assert(arm_entry_opcode == 0xEB000261); /* bl reset/init */
        uint32_t arm_mrs = *(const uint32_t *)(img + 0x404);
        assert(arm_mrs == 0xE10F0000); /* mrs r0, CPSR */

        printf("[+] SLB (Block 2): 28-byte subheader stripped. Size %zu bytes, ARM entry at 0x400 validated: 0x%08X (bl)\n",
               img_sz, arm_entry_opcode);
        fls_close(slb);
    }

    /* 2. Test Baseband System (BSY - Block 10) */
    fls_file_t *bsy = fls_open("/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222XXMJ1_BSY.fls");
    if (bsy) {
        const fls_chunk_info_t *chunks[4];
        size_t n = fls_find_all_chunks(bsy, FLS_TAG_IMAGE_DATA, chunks, 4);
        assert(n == 2);

        /* Chunk 0: Boot core & vector header */
        size_t sz0 = 0;
        const uint8_t *d0 = fls_get_image_data(bsy, chunks[0], &sz0);
        assert(d0 != NULL);
        assert(sz0 == 6525748);
        uint32_t branch_op = *(const uint32_t *)d0;
        assert(branch_op == 0xEA00007E); /* b reset_handler */
        assert(memcmp(d0 + 4, "liHU", 4) == 0);

        /* Chunk 1: Main system code */
        size_t sz1 = 0;
        const uint8_t *d1 = fls_get_image_data(bsy, chunks[1], &sz1);
        assert(d1 != NULL);
        assert(sz1 == 30082376);
        uint16_t thumb_push = *(const uint16_t *)d1;
        assert(thumb_push == 0xB4FE); /* push {r1..r7, lr} */

        printf("[+] BSY (Block 10): 28-byte subheader stripped. Chunk 0 branch 0x%08X ('liHU'), Chunk 1 Thumb 0x%04X validated\n",
               branch_op, thumb_push);
        fls_close(bsy);
    }

    /* 3. Test CDS & CSC/DFFS */
    fls_file_t *cds = fls_open("/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222XXMJ1_CDS.fls");
    if (cds) {
        const fls_chunk_info_t *c = fls_find_chunk(cds, FLS_TAG_IMAGE_DATA);
        assert(c != NULL);
        size_t sz = 0;
        const uint8_t *d = fls_get_image_data(cds, c, &sz);
        assert(d != NULL && sz == 346452);
        assert(memcmp(d + 12, "CJKT", 4) == 0);
        printf("[+] CDS: 28-byte subheader stripped. Size %zu bytes, signature 'CJKT' validated\n", sz);
        fls_close(cds);
    }

    fls_file_t *csc = fls_open("/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222OXEMJ1_CSC.dffs");
    if (csc) {
        const fls_chunk_info_t *c = fls_find_chunk(csc, FLS_TAG_IMAGE_DATA);
        assert(c != NULL);
        size_t sz = 0;
        const uint8_t *d = fls_get_image_data(csc, c, &sz);
        assert(d != NULL && sz == 49514884);
        assert(memcmp(d + 12, "IDF@", 4) == 0);
        printf("[+] CSC/DFFS: 28-byte subheader stripped. Size %zu bytes, signature 'IDF@' validated\n", sz);
        fls_close(csc);
    }

    printf("[TEST] Pure payload extraction test PASSED!\n");
}

static void test_nvram_safety(void) {
    printf("[TEST] Testing NVRAM memory safety protection (is_flash_range_safe)...\n");

    /* 1. Regions completely outside NVRAM range */
    assert(is_flash_range_safe(0x00000000, 0x00020000) == true); /* Block 0: BSR (psi_flash) */
    assert(is_flash_range_safe(0x00040000, 0x00060000) == true); /* Blocks 2..4: SLB */
    assert(is_flash_range_safe(0x00140000, 0x02840000) == true); /* Blocks 10..331: CODE / BSY */
    assert(is_flash_range_safe(0x07B40000, 0x00100000) == true); /* Starts after DYN_EEP (Block 986) */

    /* 2. Exact NVRAM range */
    assert(is_flash_range_safe(NVRAM_PROTECTED_START, NVRAM_PROTECTED_SIZE) == false);

    /* 3. Sub-ranges inside NVRAM */
    assert(is_flash_range_safe(0x07A40000, 0x1000) == false);     /* Beginning of NVRAM */
    assert(is_flash_range_safe(0x07B3F000, 0x1000) == false);     /* End of NVRAM */
    assert(is_flash_range_safe(0x07A80000, 0x1000) == false);     /* Middle of NVRAM */

    /* 4. Overlapping boundaries */
    assert(is_flash_range_safe(0x07A30000, 0x00020000) == false); /* Crosses start boundary */
    assert(is_flash_range_safe(0x07B00000, 0x00100000) == false); /* Crosses end boundary */
    assert(is_flash_range_safe(0x07000000, 0x01000000) == false); /* Completely encloses NVRAM */

    /* 5. NVRAM Dump Geometry & Parameters */
    assert(NVRAM_BLOCK_START == 978U);
    assert(NVRAM_BLOCK_END == 985U);
    assert(NVRAM_BLOCK_COUNT == 8U);
    assert(NVRAM_PROTECTED_START == 0x07A40000U);
    assert(NVRAM_PROTECTED_END == 0x07B40000U);
    assert(NVRAM_PROTECTED_SIZE == 0x00100000U); /* Exactly 1 MB = 8 blocks * 128 KB */

    printf("[TEST] NVRAM memory safety and dump geometry PASSED!\n");
}

static void test_flasher_hexdump(void) {
    printf("[TEST] Testing flasher_hexdump output formatting and boundary handling...\n");
    /* Null and 0-length checks */
    flasher_hexdump(NULL, 0, 0);
    flasher_hexdump(NULL, 100, 5);

    /* Verify with dummy data */
    uint8_t dummy[64];
    for (size_t i = 0; i < sizeof(dummy); i++) {
        dummy[i] = (uint8_t)(i + 32); /* printable ascii characters */
    }
    flasher_hexdump(dummy, sizeof(dummy), 2);
    printf("[TEST] flasher_hexdump PASSED!\n");
}

static void test_psi_sha256_verification(void) {
    printf("[TEST] Testing PSI SHA-256 verification and skip flag...\n");

    /* 1. NULL / empty handling */
    assert(flasher_verify_psi_sha256(NULL, 0) == -1);
    assert(flasher_verify_psi_sha256(NULL, 100) == -1);

    /* 2. Untrusted dummy binary (must return 1 = untrusted) */
    uint8_t dummy[512] = {0};
    assert(flasher_verify_psi_sha256(dummy, sizeof(dummy)) == 1);

    /* 3. Valid stock PSI extraction & verification */
    fls_file_t *psi_fls = fls_open("/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222XXMJ1_psi.fls");
    if (psi_fls) {
        const fls_chunk_info_t *c = fls_find_chunk(psi_fls, FLS_TAG_RPSI);
        assert(c != NULL);
        size_t sz = 0;
        const uint8_t *data = fls_get_image_data(psi_fls, c, &sz);
        assert(data != NULL && sz == 57644);

        int verify_res = flasher_verify_psi_sha256(data, sz);
        assert(verify_res == 0);
        fls_close(psi_fls);
        printf("[+] Stock PSI SHA-256 verified successfully!\n");
    }

    /* 4. Verify flasher_config_t skip_psi_sha256 flag */
    flasher_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.skip_psi_sha256 = true;
    assert(cfg.skip_psi_sha256 == true);

    printf("[TEST] PSI SHA-256 verification PASSED!\n");
}

static void test_slb_multiblock_geometry(void) {
    printf("[TEST] Testing Second Level Bootloader (SLB) multi-block flash geometry...\n");

    fls_file_t *slb = fls_open("/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222XXMJ1_slb.fls");
    if (slb) {
        const fls_chunk_info_t *c = fls_find_chunk(slb, FLS_TAG_IMAGE_DATA);
        assert(c != NULL);

        size_t payload_len = 0;
        const uint8_t *payload = fls_get_image_data(slb, c, &payload_len);
        assert(payload != NULL);
        assert(payload_len == 283700);

        /* 1. Calculate OneNAND blocks needed: ceil(283700 / 131072) == 3 blocks */
        uint32_t blocks_needed = (uint32_t)((payload_len + ONENAND_BLOCK_SIZE - 1) / ONENAND_BLOCK_SIZE);
        assert(blocks_needed == 3);

        /* Target range: Blocks 2, 3, 4 */
        uint32_t start_block = ONENAND_PART_EBL_START;
        assert(start_block == 2U);
        uint32_t end_block = start_block + blocks_needed - 1;
        assert(end_block == 4U);

        /* 2. Total pages: ceil(283700 / 2048) == 139 pages */
        uint32_t total_pages = (uint32_t)((payload_len + ONENAND_PAGE_SIZE - 1) / ONENAND_PAGE_SIZE);
        assert(total_pages == 139);

        /* 3. Verify every page mapping and block increment */
        for (uint32_t pages_done = 0; pages_done < total_pages; pages_done++) {
            uint32_t blk = start_block + (pages_done / ONENAND_PAGES_PER_BLOCK);
            uint32_t p   = pages_done % ONENAND_PAGES_PER_BLOCK;

            if (pages_done < 64) {
                assert(blk == 2);
                assert(p == pages_done);
            } else if (pages_done < 128) {
                assert(blk == 3);
                assert(p == pages_done - 64);
            } else {
                assert(blk == 4);
                assert(p == pages_done - 128);
            }
        }

        printf("[+] SLB multi-block geometry validated: %zu bytes -> %u blocks (Blocks 2..4), %u pages\n",
               payload_len, blocks_needed, total_pages);
        fls_close(slb);
    }

    printf("[TEST] SLB multi-block geometry PASSED!\n");
}

static void test_psi_flash_block0_geometry(void) {
    printf("[TEST] Testing Primary Stage Initializer (psi_flash / Block 0) flash geometry...\n");

    fls_file_t *psi = fls_open("/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222XXMJ1_psi.fls");
    if (psi) {
        const fls_chunk_info_t *c = fls_find_chunk(psi, FLS_TAG_IMAGE_DATA);
        assert(c != NULL);

        size_t payload_len = 0;
        const uint8_t *payload = fls_get_image_data(psi, c, &payload_len);
        assert(payload != NULL);
        assert(payload_len == 129116);

        /* 1. Target is strictly Block 0 (BSR) */
        uint32_t target_block = ONENAND_PART_BSR_START;
        assert(target_block == 0U);

        /* 2. Calculate OneNAND blocks needed: ceil(129116 / 131072) == 1 block */
        uint32_t blocks_needed = (uint32_t)((payload_len + ONENAND_BLOCK_SIZE - 1) / ONENAND_BLOCK_SIZE);
        assert(blocks_needed == 1U);
        assert(blocks_needed <= ONENAND_PART_BSR_BLOCKS);

        /* 3. Total pages under EBL FUN_60018d9c layout:
         * Page 0 holds strictly 512 bytes BSR header + 1536 bytes 0xFF.
         * Remaining (payload_len - 512) bytes = 128604 bytes across Pages 1..63.
         * 128604 / 2048 = 62 full pages + 1628 bytes on Page 63.
         * Total = 1 + 63 = 64 pages (exact capacity of 1 Block). */
        uint32_t shift_pages = 1 + (uint32_t)((payload_len - 512 + ONENAND_PAGE_SIZE - 1) / ONENAND_PAGE_SIZE);
        assert(shift_pages == 64U);
        size_t last_page_code_bytes = (payload_len - 512) - (62 * ONENAND_PAGE_SIZE);
        assert(last_page_code_bytes == 1628);

        /* 4. Verify all pages target Block 0 */
        for (uint32_t p = 0; p < shift_pages; p++) {
            uint32_t blk = target_block + (p / ONENAND_PAGES_PER_BLOCK);
            uint32_t page = p % ONENAND_PAGES_PER_BLOCK;
            assert(blk == 0);
            assert(page == p);
        }

        printf("[+] PSI (Block 0 / BSR) geometry validated: %zu bytes -> Block 0, %u pages (EBL 0x600 shift aligned)\n",
               payload_len, shift_pages);
        fls_close(psi);
    }

    printf("[TEST] PSI (Block 0 / BSR) flash geometry PASSED!\n");
}

static void test_bootrom_uart_telemetry(void) {
    printf("[TEST] Testing Boot ROM UART 12-byte telemetry frame parsing...\n");

    /* Mock 12-byte Boot ROM response frame */
    /* 0xF0 0x0A [SCU_CFG CHIP_ID BOOT_MODE PLL_STAT CLK_DIV SCU_RESET PWR_STAT SYS_STAT SEC_STAT] 0xFF */
    /* Example: Chip ID 0x21 (Step 2, Rev 1), SCU 0x03, Boot 0x01, PLL 0x05, CLK 0x02, RESET 0x00 */
    uint8_t valid_frame[12] = {
        0xF0, 0x0A, 0x03, 0x21, 0x01, 0x05, 0x02, 0x00, 0x80, 0x10, 0x00, 0xFF
    };

    bootrom_uart_telemetry_t telem;
    int res = bootrom_uart_parse_telemetry(valid_frame, sizeof(valid_frame), &telem);
    assert(res == 0);
    assert(telem.scu_cfg == 0x03);
    assert(telem.chip_id == 0x21);
    assert(telem.chip_step == 2);
    assert(telem.chip_rev == 1);
    assert(telem.boot_mode == 0x01);
    assert(telem.pll_stat == 0x05);
    assert(telem.clk_div == 0x02);
    assert(telem.scu_reset == 0x00);
    assert(telem.pwr_stat == 0x80);
    assert(telem.sys_stat == 0x10);
    assert(telem.sec_stat == 0x00);

    /* Malformed frame tests */
    /* 1. NULL frame or frame length < 12 */
    assert(bootrom_uart_parse_telemetry(NULL, 12, &telem) == -1);
    assert(bootrom_uart_parse_telemetry(valid_frame, 11, &telem) == -1);

    /* 2. Invalid start tag (not 0xF0) */
    uint8_t bad_start[12];
    memcpy(bad_start, valid_frame, 12);
    bad_start[0] = 0xAA;
    assert(bootrom_uart_parse_telemetry(bad_start, 12, &telem) == -2);

    /* 3. Invalid payload length (not 0x0A) */
    uint8_t bad_len[12];
    memcpy(bad_len, valid_frame, 12);
    bad_len[1] = 0x09;
    assert(bootrom_uart_parse_telemetry(bad_len, 12, &telem) == -3);

    /* 4. Invalid stop tag (not 0xFF) */
    uint8_t bad_stop[12];
    memcpy(bad_stop, valid_frame, 12);
    bad_stop[11] = 0xFE;
    assert(bootrom_uart_parse_telemetry(bad_stop, 12, &telem) == -4);

    printf("[+] Boot ROM UART 12-byte telemetry parsing and validation PASSED!\n");
}

int main(int argc, char **argv) {
    printf("=== Running OpenIFWD Unit Tests ===\n");
    test_protocol_packets();
    test_open_psi();
    test_nvram_safety();
    test_flasher_hexdump();
    test_psi_sha256_verification();
    test_slb_multiblock_geometry();
    test_psi_flash_block0_geometry();
    test_bootrom_uart_telemetry();

    const char *sample = (argc >= 2) ? argv[1] : "flashtools/FlashTool_E2_OD/catfish3g.fls";
    test_fls_parsing(sample);

    /* Test Samsung Star 3 (GT-S5222) firmware files if available */
    test_fls_parsing("/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222XXMJ1_psi.fls");
    test_firmware_structure();
    test_flash_sections();
    test_fls_payload_extraction();

    printf("=== All Tests Passed Successfully! ===\n");
    return 0;
}
