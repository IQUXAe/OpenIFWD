#include "protocol.h"
#include <string.h>
#include <stdio.h>

const char *ifwd_msg_name(uint16_t msg_type) {
    switch (msg_type) {
        case IFWD_MSG_BOOT_TX_LOAD_LEN:    return "boot_tx_load_length";
        case IFWD_MSG_BOOT_TX_LOAD_LEN2:   return "boot_tx_load_length2";
        case IFWD_MSG_BOOT_TX_AT:          return "boot_tx_at";
        case IFWD_MSG_BOOT_TX_EBL_LDR_LEN: return "boot_tx_ebl_loader_length";
        case IFWD_MSG_BOOT_TX_EBL_PAYLOAD: return "boot_tx_ebl_payload_data";
        case IFWD_MSG_BOOT_TX_EBL_CHK:     return "boot_tx_ebl_checksum";
        case IFWD_MSG_BOOT_TX_EBL_LDR_CHK: return "boot_tx_ebl_loader_checksum";
        case IFWD_MSG_BOOT_RX_ACK_BYTE:    return "boot_rx_ack_byte";
        case IFWD_MSG_BOOT_RX_EBL_LDR_RES: return "boot_rx_ebl_loader_checksum_result";
        case IFWD_MSG_BOOT_RX_EBL_VER:     return "boot_rx_ebl_version";
        case IFWD_MSG_BOOT_RX_EBL_CHK_RES: return "boot_rx_ebl_checksum_result";
        case IFWD_MSG_BOOT_RX_PAYLOAD_IND: return "boot_rx_payload_ind";
        case IFWD_MSG_BOOT_RX_EBL_READY:   return "boot_rx_ebl_ready";
        case IFWD_MSG_BOOT_TX_BAUD:        return "boot_tx_baud";
        case IFWD_MSG_REQ_RETRANSMISSION:  return "ReqRetransmission";
        case IFWD_MSG_CNF_BAUD_CHANGE:     return "CnfBaudChange";
        case IFWD_MSG_ERR_WRONG_BAUDRATE:  return "ErrWrongBaudrate";
        case IFWD_MSG_REQ_SET_PROT_CONF:   return "ReqSetProtConf";
        case IFWD_MSG_REQ_SET_RAM_ADDR:    return "ReqSetRamWorkAddress";
        case IFWD_MSG_REQ_WRITE_TO_RAM:    return "ReqWriteToRam";
        case IFWD_MSG_REQ_EXEC_FROM_RAM:   return "ReqExecuteFromRam";
        case IFWD_MSG_REQ_BOOTLOADER_VER:  return "ReqBootloaderVer";
        case IFWD_MSG_REQ_FORCE_HW_RESET:  return "ReqForceHwReset";
        case IFWD_MSG_REQ_FLASH_ID:        return "ReqFlashId";
        case IFWD_MSG_REQ_FLASH_SET_ADDR:  return "ReqFlashSetAddress";
        case IFWD_MSG_REQ_FLASH_READ_BLK:  return "ReqFlashReadBlock";
        case IFWD_MSG_REQ_FLASH_WRITE_BLK: return "ReqFlashWriteBlock";
        case IFWD_MSG_REQ_FLASH_ERASE_START: return "ReqFlashEraseStart";
        case IFWD_MSG_REQ_FLASH_ERASE_CHECK: return "ReqFlashEraseCheck";
        case IFWD_MSG_REQ_FLASH_READ_CHK:  return "ReqFlashReadChecksum";
        default:                           return "UNKNOWN_MSG";
    }
}

uint16_t ifwd_calc_xor16(const uint8_t *data, size_t len) {
    uint16_t xor_sum = 0;
    size_t words = len / 2;
    const uint16_t *p = (const uint16_t *)data;

    for (size_t i = 0; i < words; i++) {
        xor_sum ^= p[i];
    }
    if (len & 1) {
        xor_sum ^= (uint16_t)data[len - 1];
    }
    return xor_sum;
}

uint8_t ifwd_calc_sum8(const uint8_t *data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

uint16_t ifwd_calc_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

size_t ifwd_build_packet(uint8_t channel, uint16_t msg_type,
                         const void *payload, uint16_t payload_len,
                         uint8_t *out_buf, size_t max_buf_len) {
    size_t total_len = sizeof(ifwd_pkt_hdr_t) + payload_len + sizeof(uint16_t);
    if (!out_buf || max_buf_len < total_len) return 0;

    ifwd_pkt_hdr_t *hdr = (ifwd_pkt_hdr_t *)out_buf;
    hdr->channel = channel;
    hdr->sot = IFWD_SOT;
    hdr->msg_type = msg_type;
    hdr->length = payload_len;

    if (payload && payload_len > 0) {
        memcpy(out_buf + sizeof(ifwd_pkt_hdr_t), payload, payload_len);
    }

    /* Compute XOR-16 checksum across Header + Payload */
    size_t body_len = sizeof(ifwd_pkt_hdr_t) + payload_len;
    uint16_t chk = ifwd_calc_xor16(out_buf, body_len);

    /* Append checksum at the end */
    memcpy(out_buf + body_len, &chk, sizeof(uint16_t));

    return total_len;
}

int ifwd_parse_packet(const uint8_t *buf, size_t len,
                      ifwd_pkt_hdr_t *out_hdr, const uint8_t **out_payload) {
    if (!buf || len < sizeof(ifwd_pkt_hdr_t) + sizeof(uint16_t)) {
        return -1;
    }

    const ifwd_pkt_hdr_t *hdr = (const ifwd_pkt_hdr_t *)buf;
    if (hdr->sot != IFWD_SOT) {
        return -2; /* Invalid Start of Transmission */
    }

    size_t expected_total = sizeof(ifwd_pkt_hdr_t) + hdr->length + sizeof(uint16_t);
    if (len < expected_total) {
        return -3; /* Incomplete packet */
    }

    /* Verify checksum */
    size_t body_len = sizeof(ifwd_pkt_hdr_t) + hdr->length;
    uint16_t computed = ifwd_calc_xor16(buf, body_len);
    uint16_t received;
    memcpy(&received, buf + body_len, sizeof(uint16_t));

    if (computed != received) {
        return -4; /* Checksum mismatch */
    }

    if (out_hdr) *out_hdr = *hdr;
    if (out_payload) *out_payload = buf + sizeof(ifwd_pkt_hdr_t);

    return (int)expected_total;
}
