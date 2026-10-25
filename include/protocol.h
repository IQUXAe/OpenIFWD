#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard frame constants */
#define IFWD_SOT                     0x0002  /* Wire sequence [0x02, 0x00] in little-endian */
#define IFWD_EOT                     0x0003  /* Wire sequence [0x03, 0x00] in little-endian */
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
#define IFWD_MSG_CNF_BAUD_CHANGE_EBL 0x0082
#define IFWD_MSG_REQ_CFI_INFO_1      0x0084
#define IFWD_MSG_REQ_CFI_INFO_2      0x0085
#define IFWD_MSG_REQ_SET_PROT_CONF   0x0086
#define IFWD_MSG_RSP_CFI_INFO_2      0x0285
#define IFWD_MSG_RSP_CFI_INFO_1      0x02A0

/* Service, RAM execution, sections and reset */
#define IFWD_MSG_REQ_SET_RAM_ADDR    0x0201
#define IFWD_MSG_REQ_WRITE_TO_RAM    0x0202
#define IFWD_MSG_REQ_EXEC_FROM_RAM   0x0203
#define IFWD_MSG_REQ_SEC_START       0x0204
#define IFWD_MSG_REQ_SEC_END         0x0205
#define IFWD_MSG_RSP_SET_RAM_ADDR    0x0207
#define IFWD_MSG_RSP_SEC_START       0x0207
#define IFWD_MSG_RSP_WRITE_TO_RAM    0x0208
#define IFWD_MSG_RSP_EXEC_FROM_RAM   0x0209
#define IFWD_MSG_RSP_SEC_END         0x020A
#define IFWD_MSG_REQ_BOOTLOADER_VER  0x0207
#define IFWD_MSG_REQ_FORCE_HW_RESET  0x0208

/* Flash engine messages */
#define IFWD_MSG_REQ_FLASH_ID        0x0801
#define IFWD_MSG_REQ_FLASH_SET_ADDR  0x0802
#define IFWD_MSG_REQ_FLASH_READ_BLK  0x0803
#define IFWD_MSG_RSP_FLASH_ID        0x0803
#define IFWD_MSG_RSP_FLASH_READ_BLK  0x0141
#define IFWD_MSG_REQ_FLASH_WRITE_BLK 0x0804
#define IFWD_MSG_RSP_FLASH_SET_ADDR  0x0804
#define IFWD_MSG_REQ_FLASH_ERASE_START 0x0805
#define IFWD_MSG_REQ_FLASH_ERASE_CHECK 0x0806
#define IFWD_MSG_REQ_FLASH_READ_CHK  0x0807

/* High-speed USB buffer sizes (V3/V4 EBL protocol) */
#define EBL_USB_DATA_BLOCK           16384
#define EBL_USB_PACKET_SIZE          16392  /* sizeof(ebl_pkt_hdr_t) (8) + EBL_USB_DATA_BLOCK (16384) */

/* Status codes */
#define IFWD_STATUS_OK               0x00
#define IFWD_STATUS_READY            0x01
#define IFWD_STATUS_ERROR            0x11

#pragma pack(push, 1)

/* Standard 16-bit packet header (7 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  channel;
    uint16_t sot;       /* Wire order: [0x02, 0x00] */
    uint16_t msg_type;  /* IFWD_MSG_* */
    uint16_t length;    /* Length of following payload */
} ifwd_pkt_hdr_t;
_Static_assert(sizeof(ifwd_pkt_hdr_t) == 7, "ifwd_pkt_hdr_t must be exactly 7 bytes");

/* Set Protocol Configuration Request (28 bytes) */
typedef struct __attribute__((packed)) {
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
    uint8_t  reserved1;
    uint8_t  reserved2;
} ifwd_req_prot_conf_t;
_Static_assert(sizeof(ifwd_req_prot_conf_t) == 28, "ifwd_req_prot_conf_t must be exactly 28 bytes");

/* EBL 8-byte Packet Header (V3/V4 framing) */
typedef struct __attribute__((packed)) {
    uint16_t token;      /* e.g. 0x0576 for ReqSetProtConf */
    uint16_t msg_type;   /* e.g. 0x0086 (ReqSetProtConf) */
    uint32_t length;     /* 0x00000048 (72 bytes body length) */
} ebl_pkt_hdr_t;
_Static_assert(sizeof(ebl_pkt_hdr_t) == 8, "ebl_pkt_hdr_t must be 8 bytes");

/* EBL 76-byte Target Identification / Announcement (Frame 128) */
typedef struct __attribute__((packed)) {
    uint32_t magic;      /* 0x000000BB */
    uint32_t proto_ver;  /* 0x00000003 */
    uint32_t channel;    /* 0x00000001 */
    char     chip_id[32];/* "2150_OD_KL1_3.1" (null-padded) */
    uint8_t  capabilities[32]; /* 32 bytes capability flags */
} ebl_target_announce_t;
_Static_assert(sizeof(ebl_target_announce_t) == 76, "ebl_target_announce_t must be 76 bytes");

/* EBL 72-byte ReqSetProtConf Payload (Frame 129) */
typedef struct __attribute__((packed)) {
    uint32_t magic;      /* 0x000000BB */
    uint32_t proto_ver;  /* 0x00000003 */
    uint32_t channel;    /* 0x00000001 */
    char     chip_id[32];/* "2150_OD_KL1_3.1" */
    uint8_t  flags[28];  /* 28 bytes configuration flags */
} ebl_req_prot_conf_t;
_Static_assert(sizeof(ebl_req_prot_conf_t) == 72, "ebl_req_prot_conf_t must be 72 bytes");

/* EBL 8-byte RspSetProtConf Status (Frame 130) */
typedef struct __attribute__((packed)) {
    uint16_t msg_type;   /* 0x0089 (RspSetProtConf) */
    uint16_t ack_type;   /* 0x0086 (Ack for ReqSetProtConf) */
    uint32_t status_len; /* 0x00000002 */
} ebl_rsp_prot_conf_t;
_Static_assert(sizeof(ebl_rsp_prot_conf_t) == 8, "ebl_rsp_prot_conf_t must be 8 bytes");

/* Request Flash Set Address */
typedef struct __attribute__((packed)) {
    uint32_t address;
    uint16_t checksum;
    uint16_t end;
} ifwd_req_set_address_t;

/* Request Flash Erase Start */
typedef struct __attribute__((packed)) {
    uint32_t start_addr;
    uint32_t end_addr;
    uint16_t checksum;
    uint16_t eot;
} ifwd_req_erase_start_t;

/* Erase Check Response */
typedef struct __attribute__((packed)) {
    uint8_t  status;
    uint32_t erase_addr;
} ifwd_rsp_erase_check_t;

/* Tag 15 (FLS_TAG_SEC_CFG) partition trailer descriptor (Frame 122 / 142 / 172) */
#define FLS_SEC_CFG_MAGIC            0x01FEDABE

typedef struct __attribute__((packed)) {
    uint32_t load_address;    /* Flash load address (e.g. 0x00090000, 0x60000000, 0x63200000) */
    uint32_t partition_size;  /* Partition size in flash */
    uint32_t image_size;      /* Actual valid image byte size */
    uint32_t flags;           /* Reserved / flags */
} fls_sec_cfg_entry_t;
_Static_assert(sizeof(fls_sec_cfg_entry_t) == 16, "fls_sec_cfg_entry_t must be 16 bytes");

#pragma pack(pop)

/* Checksum and CRC functions */
uint16_t ifwd_calc_xor16(const uint8_t *data, size_t len);
uint16_t ifwd_calc_crc16(const uint8_t *data, size_t len);
uint8_t  ifwd_calc_sum8(const uint8_t *data, size_t len);
uint16_t ifwd_calc_ebl_token(uint16_t msg_type, uint32_t length, const uint8_t *data);

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
