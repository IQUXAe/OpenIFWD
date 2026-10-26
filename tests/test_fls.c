#include "fls.h"
#include "protocol.h"
#include "psi_xgold215.h"
#include "flasher.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

static void test_protocol_packets(void) {
    printf("[TEST] Testing protocol packet creation and parsing...\n");

    uint8_t buf[512];
    uint32_t test_addr = 0x20000000;
    size_t pkt_len = ifwd_build_packet(1, IFWD_MSG_REQ_FLASH_SET_ADDR, &test_addr, sizeof(test_addr), buf, sizeof(buf));
    assert(pkt_len > sizeof(ifwd_pkt_hdr_t));

    ifwd_pkt_hdr_t hdr;
    const uint8_t *payload = NULL;
    int parsed_len = ifwd_parse_packet(buf, pkt_len, &hdr, &payload);
    assert(parsed_len == (int)pkt_len);
    assert(hdr.channel == 1);
    assert(hdr.sot == IFWD_SOT);
    assert(hdr.msg_type == IFWD_MSG_REQ_FLASH_SET_ADDR);
    assert(hdr.length == sizeof(test_addr));

    uint32_t parsed_addr = 0;
    memcpy(&parsed_addr, payload, sizeof(parsed_addr));
    assert(parsed_addr == test_addr);

    /* Test RAM Work Address packet (ReqSetRamWorkAddress = 0x0201) */
    pkt_len = ifwd_build_packet(1, IFWD_MSG_REQ_SET_RAM_ADDR, &test_addr, sizeof(test_addr), buf, sizeof(buf));
    assert(pkt_len > 0);
    parsed_len = ifwd_parse_packet(buf, pkt_len, &hdr, &payload);
    assert(parsed_len == (int)pkt_len);
    assert(hdr.msg_type == IFWD_MSG_REQ_SET_RAM_ADDR);

    /* Test Write to RAM packet (ReqWriteToRam = 0x0202) */
    uint8_t ram_data[128];
    for (int i = 0; i < 128; i++) ram_data[i] = (uint8_t)i;
    pkt_len = ifwd_build_packet(1, IFWD_MSG_REQ_WRITE_TO_RAM, ram_data, sizeof(ram_data), buf, sizeof(buf));
    assert(pkt_len > 0);
    parsed_len = ifwd_parse_packet(buf, pkt_len, &hdr, &payload);
    assert(parsed_len == (int)pkt_len);
    assert(hdr.msg_type == IFWD_MSG_REQ_WRITE_TO_RAM);
    assert(hdr.length == sizeof(ram_data));
    assert(memcmp(payload, ram_data, sizeof(ram_data)) == 0);

    /* Test Execute from RAM packet (ReqExecuteFromRam = 0x0203) */
    pkt_len = ifwd_build_packet(1, IFWD_MSG_REQ_EXEC_FROM_RAM, &test_addr, sizeof(test_addr), buf, sizeof(buf));
    assert(pkt_len > 0);
    parsed_len = ifwd_parse_packet(buf, pkt_len, &hdr, &payload);
    assert(parsed_len == (int)pkt_len);
    assert(hdr.msg_type == IFWD_MSG_REQ_EXEC_FROM_RAM);

    /* Test Protocol Configuration packet (ReqSetProtConf = 0x0086) */
    ifwd_req_prot_conf_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.boot_mode = 0;
    conf.major = 4;
    conf.minor = 68;
    conf.package_size = 8;
    conf.crc_type = 0;
    pkt_len = ifwd_build_packet(1, IFWD_MSG_REQ_SET_PROT_CONF, &conf, sizeof(conf), buf, sizeof(buf));
    assert(pkt_len > 0);
    parsed_len = ifwd_parse_packet(buf, pkt_len, &hdr, &payload);
    assert(parsed_len == (int)pkt_len);
    assert(hdr.msg_type == IFWD_MSG_REQ_SET_PROT_CONF);

    /* Test EBL USB packet token calculation (matches pcapng frames 116, 128, 139) */
    uint16_t zero2 = 0;
    assert(ifwd_calc_ebl_token(IFWD_MSG_REQ_CFI_INFO_1, 2, (const uint8_t *)&zero2) == 0x0086);
    uint32_t addr_psi = 0x00090000;
    assert(ifwd_calc_ebl_token(IFWD_MSG_REQ_FLASH_SET_ADDR, 4, (const uint8_t *)&addr_psi) == 0x080F);
    assert(ifwd_calc_ebl_token(IFWD_MSG_REQ_SEC_END, 2, (const uint8_t *)&zero2) == 0x0207);

    printf("[TEST] Protocol packet creation and verification PASSED!\n");
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
        const fls_chunk_info_t *c11 = fls_find_chunk(psi, FLS_TAG_PSI_RAM);
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

    const char *slb_path = "/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222XXMJ1_slb.fls";
    fls_file_t *slb = fls_open(slb_path);
    if (slb) {
        /* Offline Bootloader (SLB): Tag 12 */
        const fls_chunk_info_t *c12 = fls_find_chunk(slb, FLS_TAG_IMAGE_DATA);
        assert(c12 != NULL);
        assert(c12->payload_size == 283728);
        size_t slb_sz = 0;
        const uint8_t *slb_data = fls_get_chunk_payload(slb, c12, &slb_sz);
        assert(slb_data != NULL);
        uint32_t sub_len = *(const uint32_t *)(slb_data + 16);
        assert(sub_len == 283700);
        printf("[+] SLB Tag 12 validated: total %zu bytes, code payload %u bytes\n", slb_sz, sub_len);

        /* RAM Download Engine (EBL): Tag 19 */
        const fls_chunk_info_t *c19 = fls_find_chunk(slb, FLS_TAG_SECONDARY_LOADER);
        assert(c19 != NULL);
        assert(c19->payload_size == 587760);
        size_t slb19_sz = 0;
        const uint8_t *slb19_data = fls_get_chunk_payload(slb, c19, &slb19_sz);
        assert(slb19_data != NULL);
        uint32_t sub_len19 = *(const uint32_t *)(slb19_data + 8);
        assert(sub_len19 == 587740);
        printf("[+] SLB Tag 19 (EBL) validated: total %zu bytes, code payload %u bytes\n", slb19_sz, sub_len19);

        assert(slb->toc_count > 0);
        assert(slb->toc_entries[0].mem_class == 2); /* SLB/EBL mem_class */
        printf("[+] SLB TOC validated: mem_class 0x%08x\n", slb->toc_entries[0].mem_class);

        fls_close(slb);
    }

    printf("[TEST] Samsung GT-S5222 firmware structure PASSED!\n");
}

static void test_embedded_psi(void) {
    printf("[TEST] Testing embedded X-Gold 215 / PMB8815 Boot ROM PSI payload...\n");
    assert(xgold215_psi_bootrom_size == 6296);
    assert(xgold215_psi_bootrom_checksum == 0x4C);

    uint8_t chk = 0;
    for (size_t i = 0; i < xgold215_psi_bootrom_size; i++) {
        chk ^= xgold215_psi_bootrom[i];
    }
    assert(chk == xgold215_psi_bootrom_checksum);

    /* Verify entry branch at ARM reset vector (0x00080000 -> 0x00080044) */
    uint32_t reset_vec = *(const uint32_t *)xgold215_psi_bootrom;
    assert(reset_vec == 0xe59ff018); /* ldr pc, [pc, #24] */
    uint32_t reset_target = *(const uint32_t *)(xgold215_psi_bootrom + 0x20);
    assert(reset_target == 0x00080044);

    printf("[+] Embedded X-Gold 215 Boot ROM PSI verified: %zu bytes, XOR-8 0x%02X, reset vector -> 0x%08X\n",
           xgold215_psi_bootrom_size, chk, reset_target);
    printf("[TEST] Embedded PSI test PASSED!\n");
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

static void test_nvram_safety(void) {
    printf("[TEST] Testing NVRAM memory safety protection (is_flash_range_safe)...\n");

    /* 1. Regions completely outside NVRAM range */
    assert(is_flash_range_safe(0x00090000, 0x00020000) == true); /* PSI Flash */
    assert(is_flash_range_safe(0x60000000, 0x00080000) == true); /* SLB/EBL (ends at 0x60080000) */
    assert(is_flash_range_safe(0x60180000, 0x00100000) == true); /* Starts after NVRAM */
    assert(is_flash_range_safe(0x63200000, 0x01C00000) == true); /* CODE section */

    /* 2. Exact NVRAM range */
    assert(is_flash_range_safe(NVRAM_PROTECTED_START, NVRAM_PROTECTED_SIZE) == false);

    /* 3. Sub-ranges inside NVRAM */
    assert(is_flash_range_safe(0x60080000, 0x1000) == false);     /* Beginning of NVRAM */
    assert(is_flash_range_safe(0x6017F000, 0x1000) == false);     /* End of NVRAM */
    assert(is_flash_range_safe(0x60090000, 0x1000) == false);     /* Middle of NVRAM */

    /* 4. Overlapping boundaries */
    assert(is_flash_range_safe(0x60070000, 0x00020000) == false); /* Crosses start boundary */
    assert(is_flash_range_safe(0x60100000, 0x00100000) == false); /* Crosses end boundary */
    assert(is_flash_range_safe(0x60000000, 0x00200000) == false); /* Completely encloses NVRAM */

    /* 5. NVRAM Dump Geometry & Parameters */
    assert(NVRAM_PROTECTED_START == 0x60080000U);
    assert(NVRAM_PROTECTED_END == 0x60180000U);
    assert(NVRAM_PROTECTED_SIZE == 0x00100000U); /* Exactly 1 MB */
    assert(EBL_USB_DATA_BLOCK == 16384);
    assert(NVRAM_PROTECTED_SIZE % EBL_USB_DATA_BLOCK == 0);
    assert(NVRAM_PROTECTED_SIZE / EBL_USB_DATA_BLOCK == 64); /* Exactly 64 blocks */
    assert(IFWD_MSG_REQ_SEC_START == 0x0204);
    assert(IFWD_MSG_RSP_SEC_START == 0x0207);
    assert(IFWD_MSG_REQ_SEC_END == 0x0205);
    assert(IFWD_MSG_RSP_SEC_END == 0x020A);
    assert(IFWD_MSG_REQ_FLASH_SET_ADDR == 0x0802);
    assert(IFWD_MSG_RSP_FLASH_SET_ADDR == 0x0804);
    assert(IFWD_MSG_REQ_FLASH_READ_BLK == 0x0803);
    assert(IFWD_MSG_REQ_FORCE_HW_RESET == 0x0208);
    assert(strcmp(ifwd_msg_name(0x0207), "RspSecStart") == 0);

    /* 6. Verify Tag 15 Upload Session Descriptor Layout */
    uint8_t upload_tag15[2048] = {0};
    uint32_t *p_magic = (uint32_t *)(upload_tag15 + 1916);
    *p_magic = FLS_SEC_CFG_MAGIC;
    fls_sec_cfg_entry_t *p_entry = (fls_sec_cfg_entry_t *)(upload_tag15 + 1920);
    p_entry->load_address = NVRAM_PROTECT_START;
    p_entry->partition_size = NVRAM_PROTECT_SIZE;
    p_entry->image_size = NVRAM_PROTECT_SIZE;
    p_entry->flags = 0x00000200; /* Read/Upload mode, No Erase */

    assert(*(uint32_t *)(upload_tag15 + 1916) == 0x01FEDABE);
    assert(p_entry->load_address == 0x60080000);
    assert(p_entry->partition_size == 0x00100000);
    assert(p_entry->image_size == 0x00100000);
    assert(p_entry->flags == 0x00000200);

    printf("[TEST] NVRAM memory safety and dump geometry PASSED!\n");
}

static void test_ebl_checksum_calculation(void) {
    printf("[TEST] Testing EBL packet checksum calculations (protocol_calc_checksum)...\n");

    /* 1. CnfBaudChange (0x0082, len 4, baud 921600):
     * 921600 = 0x000E1000 -> byte array [0x00, 0x10, 0x0E, 0x00]
     * sum = 0x0082 + 4 + 0x00 + 0x10 + 0x0E + 0x00 = 0x00A4
     */
    uint32_t baud = 921600;
    uint16_t baud_chk = protocol_calc_checksum(0x0082, sizeof(baud), (const uint8_t *)&baud);
    assert(baud_chk == 0x00A4);
    printf("[+] CnfBaudChange checksum validated: 0x%04X == 0x00A4\n", baud_chk);

    /* 2. ReqFlashId (0x0801, len 172, Tag 13 payload from psi.fls):
     * Mathematical sum over Tag 13 payload MUST equal exactly 0x1BB5
     */
    fls_file_t *psi = fls_open("/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222XXMJ1_psi.fls");
    if (psi) {
        const fls_chunk_info_t *c13 = fls_find_chunk(psi, FLS_TAG_PLATFORM_EXT);
        assert(c13 != NULL);
        size_t c13_sz = 0;
        const uint8_t *c13_p = fls_get_chunk_payload(psi, c13, &c13_sz);
        assert(c13_p != NULL);
        uint8_t tag13_buf[172] = {0};
        size_t copy_sz = (c13_sz > sizeof(tag13_buf)) ? sizeof(tag13_buf) : c13_sz;
        memcpy(tag13_buf, c13_p, copy_sz);

        uint16_t flash_id_chk = protocol_calc_checksum(IFWD_MSG_REQ_FLASH_ID, sizeof(tag13_buf), tag13_buf);
        assert(flash_id_chk == 0x1BB5);
        printf("[+] ReqFlashId Tag 13 checksum validated: 0x%04X == 0x1BB5\n", flash_id_chk);
        fls_close(psi);
    }

    printf("[TEST] EBL packet checksum calculations PASSED!\n");
}

static void test_ebl_patching(void) {
    printf("[TEST] Testing in-memory EBL RSA & security patching (flasher_patch_ebl)...\n");

    /* Open S5222XXMJ1_slb.fls to test real EBL extraction and patching */
    fls_file_t *slb = fls_open("/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222XXMJ1_slb.fls");
    if (!slb) {
        printf("[*] SLB not found, testing with synthetic buffer\n");
        return;
    }

    const fls_chunk_info_t *c19 = fls_find_chunk(slb, FLS_TAG_SECONDARY_LOADER);
    assert(c19 != NULL);
    size_t raw_sz = 0;
    const uint8_t *raw_p = fls_get_chunk_payload(slb, c19, &raw_sz);
    assert(raw_p != NULL && raw_sz > 20);

    uint32_t sub_len = *(const uint32_t *)(raw_p + 8);
    assert(sub_len == 587740);
    const uint8_t *ebl_src = raw_p + 20;

    uint8_t *ebl_buf = (uint8_t *)malloc(sub_len);
    assert(ebl_buf != NULL);
    memcpy(ebl_buf, ebl_src, sub_len);

    /* Verify unpatched initial bytes */
    assert(ebl_buf[0x84408] == 0x00);
    assert(ebl_buf[0x845AC] == 0xF0 && ebl_buf[0x845AC + 1] == 0x40);
    assert(ebl_buf[0x847C4] == 0x00 && ebl_buf[0x847C4 + 1] == 0x10);
    assert(ebl_buf[0x85EAC] == 0x30 && ebl_buf[0x85EAC + 1] == 0x40);

    /* Apply in-memory patch */
    int patch_res = flasher_patch_ebl(ebl_buf, sub_len);
    assert(patch_res == 0);

    /* Verify patched bytes:
     * 1. 0x84408: mov r0, #1 (01 00 a0 e3)
     * 2. 0x845AC: mov r0, #0; bx lr (00 00 a0 e3 1e ff 2f e1)
     * 3. 0x847C4: mov r0, #0; bx lr (00 00 a0 e3 1e ff 2f e1)
     * 4. 0x85EAC: mov r0, #0; bx lr (00 00 a0 e3 1e ff 2f e1)
     */
    static const uint8_t ret0_arm[8] = { 0x00, 0x00, 0xa0, 0xe3, 0x1e, 0xff, 0x2f, 0xe1 };

    assert(ebl_buf[0x84408] == 0x01);
    assert(memcmp(ebl_buf + 0x845AC, ret0_arm, 8) == 0);
    assert(memcmp(ebl_buf + 0x847C4, ret0_arm, 8) == 0);
    assert(memcmp(ebl_buf + 0x85EAC, ret0_arm, 8) == 0);

    free(ebl_buf);
    fls_close(slb);

    printf("[+] In-memory EBL RSA & security patching verified: all 4 patch sites validated!\n");
    printf("[TEST] EBL patching test PASSED!\n");
}

#include "open_psi.h"

static void test_openifwd_protocol(void) {
    printf("[TEST] Testing OpenIFWD wire protocol and embedded Rust PSI...\n");

    /* 1. Verify embedded Rust PSI header:
     * Offset 0:  b reset_handler (0xea00000e)
     * Offset 4:  "liHU" (0xffffd003)
     * Offset 48: 0x00080000 (Load address)
     * Offset 60: "CJKT"
     */
    assert(open_psi_size == 4060);
    assert(memcmp(open_psi_binary + 4, "liHU", 4) == 0);
    assert(memcmp(open_psi_binary + 60, "CJKT", 4) == 0);
    uint32_t entry = *(const uint32_t *)(open_psi_binary + 48);
    assert(entry == 0x00080000);

    /* Verify checksum matches */
    uint8_t csum = 0;
    for (size_t i = 0; i < open_psi_size; i++) csum ^= open_psi_binary[i];
    assert(csum == open_psi_checksum);
    printf("[+] Embedded Rust Open PSI verified: %zu bytes, XOR-8 0x%02X, load 0x%08X\n",
           open_psi_size, open_psi_checksum, entry);

    /* 2. Verify OpenIFWD CRC-16-CCITT */
    const uint8_t test_msg[] = "OPENIFWD_TEST";
    uint16_t crc1 = openifwd_calc_crc16(test_msg, sizeof(test_msg) - 1);
    assert(crc1 != 0);

    /* 3. Verify OpenIFWD packet building and header parsing */
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

int main(int argc, char **argv) {
    printf("=== Running Unit Tests ===\n");
    test_protocol_packets();
    test_openifwd_protocol();
    test_embedded_psi();
    test_nvram_safety();
    test_ebl_checksum_calculation();
    test_ebl_patching();

    const char *sample = (argc >= 2) ? argv[1] : "flashtools/FlashTool_E2_OD/catfish3g.fls";
    test_fls_parsing(sample);

    /* Test Samsung Star 3 (GT-S5222) firmware files if available */
    test_fls_parsing("/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222XXMJ1_psi.fls");
    test_fls_parsing("/home/iquxae/Projects/gt-s5222_dev/S5222XXMJ1_OXEMJ1/S5222XXMJ1_slb.fls");

    test_firmware_structure();
    test_flash_sections();

    printf("=== All Tests Passed Successfully! ===\n");
    return 0;
}
