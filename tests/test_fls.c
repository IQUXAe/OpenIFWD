#include "fls.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

static void test_protocol_packets(void) {
    printf("[TEST] Testing protocol packet creation and parsing...\n");

    uint8_t buf[256];
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

    fls_close(fls);
    printf("[TEST] FLS parser PASSED!\n");
}

int main(int argc, char **argv) {
    printf("=== Running Unit Tests ===\n");
    test_protocol_packets();

    const char *sample = (argc >= 2) ? argv[1] : "FlashTool_E2_OD/catfish3g.fls";
    test_fls_parsing(sample);

    printf("=== All Tests Passed Successfully! ===\n");
    return 0;
}
