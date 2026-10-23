#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard frame constants */
#define IFWD_SOT                     0x0200  /* Start of Transmission marker */
#define IFWD_EOT                     0x0300  /* End of Transmission marker */
#define IFWD_DEFAULT_CHANNEL         1

/* Boot sequence opcodes */
#define IFWD_MSG_BOOT_TX_LOAD_LEN    0x9000
#define IFWD_MSG_BOOT_TX_LOAD_LEN2   0x9001
#define IFWD_MSG_BOOT_TX_AT          0x9002  /* "ATAT" boot sequence */
#define IFWD_MSG_BOOT_TX_EBL_LDR_LEN 0x9003
#define IFWD_MSG_BOOT_TX_EBL_PAYLOAD 0x9004
#define IFWD_MSG_BOOT_TX_EBL_CHK     0x9005
#define IFWD_MSG_BOOT_TX_EBL_LDR_CHK 0x9006
#define IFWD_MSG_BOOT_RX_ACK_BYTE    0x9007
#define IFWD_MSG_BOOT_RX_EBL_LDR_RES 0x9008
#define IFWD_MSG_BOOT_RX_EBL_VER     0x9009
#define IFWD_MSG_BOOT_RX_EBL_CHK_RES 0x900A
#define IFWD_MSG_BOOT_RX_PAYLOAD_IND 0x900B
#define IFWD_MSG_BOOT_RX_EBL_READY   0x900C
#define IFWD_MSG_BOOT_TX_BAUD        0x900D

/* Main protocol messages */
#define IFWD_MSG_REQ_RETRANSMISSION  0x0080
#define IFWD_MSG_CNF_BAUD_CHANGE     0x0081
#define IFWD_MSG_ERR_WRONG_BAUDRATE  0x0082
#define IFWD_MSG_REQ_SET_PROT_CONF   0x0086

/* Service, RAM execution and reset */
#define IFWD_MSG_REQ_SET_RAM_ADDR    0x0201
#define IFWD_MSG_REQ_WRITE_TO_RAM    0x0202
#define IFWD_MSG_REQ_EXEC_FROM_RAM   0x0203
#define IFWD_MSG_REQ_BOOTLOADER_VER  0x0207
#define IFWD_MSG_REQ_FORCE_HW_RESET  0x0208

/* Flash engine messages */
#define IFWD_MSG_REQ_FLASH_ID        0x0801
#define IFWD_MSG_REQ_FLASH_SET_ADDR  0x0802
#define IFWD_MSG_REQ_FLASH_READ_BLK  0x0803
#define IFWD_MSG_REQ_FLASH_WRITE_BLK 0x0804
#define IFWD_MSG_REQ_FLASH_ERASE_START 0x0805
#define IFWD_MSG_REQ_FLASH_ERASE_CHECK 0x0806
#define IFWD_MSG_REQ_FLASH_READ_CHK  0x0807

/* Status codes */
#define IFWD_STATUS_OK               0x00
#define IFWD_STATUS_READY            0x01
#define IFWD_STATUS_ERROR            0x11

#pragma pack(push, 1)

/* Standard 16-bit packet header (7 bytes) */
typedef struct {
    uint8_t  channel;
    uint16_t sot;       /* Big-endian or Little-endian depending on stream: 0x0200 */
    uint16_t msg_type;  /* IFWD_MSG_* */
    uint16_t length;    /* Length of following payload */
} ifwd_pkt_hdr_t;

/* Set Protocol Configuration Request */
typedef struct {
    uint32_t boot_mode;
    uint32_t major;
    uint32_t minor;
    uint8_t  name;
    uint8_t  protocol_version;
    uint8_t  package_size;
    uint8_t  crc_method;
    uint8_t  crc_type;
    uint8_t  buffer_count;
    uint8_t  compression;
    uint8_t  hw_struct_size;
    uint8_t  erase_sector_verify;
    uint8_t  flash_debug;
    uint8_t  protocol_crc;
    uint8_t  skip_erase;
    uint8_t  skip_wr_pack_crc;
    uint8_t  boot_cfg_val;
    uint8_t  reserved;
    uint16_t expected_memory_class;
    uint8_t  filler;
} ifwd_req_prot_conf_t;

/* Request Flash Set Address */
typedef struct {
    uint32_t address;
    uint16_t checksum;
    uint16_t end;
} ifwd_req_set_address_t;

/* Request Flash Erase Start */
typedef struct {
    uint32_t start_addr;
    uint32_t end_addr;
    uint16_t checksum;
    uint16_t eot;
} ifwd_req_erase_start_t;

/* Erase Check Response */
typedef struct {
    uint8_t  status;
    uint32_t erase_addr;
} ifwd_rsp_erase_check_t;

#pragma pack(pop)

/* Checksum and CRC functions */
uint16_t ifwd_calc_xor16(const uint8_t *data, size_t len);
uint16_t ifwd_calc_crc16(const uint8_t *data, size_t len);
uint8_t  ifwd_calc_sum8(const uint8_t *data, size_t len);

/* Packet building & parsing */
size_t ifwd_build_packet(uint8_t channel, uint16_t msg_type,
                         const void *payload, uint16_t payload_len,
                         uint8_t *out_buf, size_t max_buf_len);

int ifwd_parse_packet(const uint8_t *buf, size_t len,
                      ifwd_pkt_hdr_t *out_hdr, const uint8_t **out_payload);

const char *ifwd_msg_name(uint16_t msg_type);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_H */
