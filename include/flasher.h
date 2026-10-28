#ifndef FLASHER_H
#define FLASHER_H

#include "fls.h"
#include "protocol.h"
#include "transport.h"
#include "open_psi.h"
#include "ram_dumper_embedded.h"
#include "open_flasher_embedded.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *psi_path;     /* Optional custom PSI (default: built-in Open Rust PSI) */
    const char *slb_path;     /* Second Level Bootloader (_slb.fls / Block 2..9) */
    const char *bsy_path;     /* Baseband system code (_BSY.fls) */
    const char *cds_path;     /* Customer Data System (_CDS.fls) */
    const char *dffs_path;    /* Data Flash File System (.dffs / CSC) */
    const char *fls_path;     /* Unified firmware container (.fls) */
    const char *code_path;    /* Raw CODE partition binary (.bin) */
    const char *csc_path;     /* Raw CSC partition binary (.bin) */
    uint32_t baudrate;        /* Baudrate for UART / unused for USB */
    bool erase_all;           /* Erase whole non-protected flash before writing */
    bool force_dangerous_erase; /* Allow erasing/writing protected calibration & metadata blocks (978..1023) */
    bool skip_csc;            /* Skip CSC / DFFS flashing */
    bool no_auto;             /* Disable automatic detection of companion firmware files */
    bool force_auto;          /* Force automatic detection even when explicit targets are specified */
    bool is_usb;              /* Direct USB connection (EP 0x02 OUT / EP 0x83 IN) */
    bool verbose;             /* Verbose logging (hex dumps, packet traces) */
    const char *ram_exec_bin; /* Binary to load into RAM */
    uint32_t ram_exec_addr;   /* RAM execution target address (e.g. 0x60000000) */
} flasher_config_t;

typedef struct {
    transport_t *transport;
    flasher_config_t config;
    uint8_t channel;
    uint8_t chip_id;
    uint32_t target_platform;
    uint16_t openifwd_seq;
    openifwd_hw_info_t hw_info;
} flasher_session_t;

/* NVRAM / EEPROM Protected Range: Blocks 4..11 (0x60080000 - 0x60180000, 1 MB) */
#define NVRAM_BLOCK_START            4U
#define NVRAM_BLOCK_END              11U
#define NVRAM_PROTECTED_START        0x60080000U
#define NVRAM_PROTECTED_END          0x60180000U
#define NVRAM_PROTECTED_SIZE         0x00100000U
#define NVRAM_PROTECT_START          NVRAM_PROTECTED_START
#define NVRAM_PROTECT_END            NVRAM_PROTECTED_END
#define NVRAM_PROTECT_SIZE           NVRAM_PROTECTED_SIZE
#define IFWD_ERR_NVRAM_PROTECTED     0xEE01

/* Samsung GT-S5222 OneNAND Partition Map (XSR Layout) */
#define ONENAND_PAGE_SIZE            2048U
#define ONENAND_PAGES_PER_BLOCK      64U
#define ONENAND_BLOCK_SIZE           (ONENAND_PAGE_SIZE * ONENAND_PAGES_PER_BLOCK) /* 128 KB */

#define ONENAND_PART_BSR_START       0U
#define ONENAND_PART_BSR_BLOCKS      1U

#define ONENAND_PART_BOOTCFG_START   1U
#define ONENAND_PART_BOOTCFG_BLOCKS  1U

#define ONENAND_PART_EBL_START       2U
#define ONENAND_PART_EBL_BLOCKS      8U

#define ONENAND_PART_CODE_START      10U
#define ONENAND_PART_CODE_BLOCKS     322U

#define ONENAND_PART_CSC_START       332U
#define ONENAND_PART_CSC_BLOCKS      8U

#define ONENAND_PART_IFS_START       340U
#define ONENAND_PART_IFS_BLOCKS      638U

#define ONENAND_PART_DYNEEP_START    978U
#define ONENAND_PART_DYNEEP_BLOCKS   8U

#define ONENAND_PART_STATEEP_START   986U
#define ONENAND_PART_STATEEP_BLOCKS  12U

/* OneNAND Hardware Calibration & Metadata Protected Range: Blocks 978..1023 */
#define ONENAND_PROTECTED_BLOCK_START 978U
#define ONENAND_TOTAL_BLOCKS         1024U

/* Magic Tokens for RAM Dumper */
#define MAGIC_CMD_DUMP_BOOTROM       0x42524F4D /* "BROM" */

static inline bool is_flash_range_safe(uint32_t addr, uint32_t length) {
    uint32_t end = addr + length;
    if ((addr >= NVRAM_PROTECTED_START && addr < NVRAM_PROTECTED_END) ||
        (end > NVRAM_PROTECTED_START && end <= NVRAM_PROTECTED_END) ||
        (addr < NVRAM_PROTECTED_START && end > NVRAM_PROTECTED_END)) {
        return false;
    }
    return true;
}

flasher_session_t *flasher_create(transport_t *transport, const flasher_config_t *cfg);
void flasher_destroy(flasher_session_t *session);

/* Boot ROM Synchronization & PSI Upload */
int flasher_sync(flasher_session_t *s, int timeout_sec);
int flasher_upload_psi(flasher_session_t *s, const fls_file_t *psi_fls);

/* OpenIFWD Binary Protocol API */
int flasher_open_ping(flasher_session_t *s);
int flasher_open_get_hw_info(flasher_session_t *s, openifwd_hw_info_t *out_info);
int flasher_open_erase_block(flasher_session_t *s, uint32_t block);
int flasher_open_write_page(flasher_session_t *s, uint32_t block, uint16_t page, const uint8_t *data, const uint8_t *spare);
int flasher_open_read_page(flasher_session_t *s, uint32_t block, uint16_t page, uint8_t *out_data, uint8_t *out_spare);
int flasher_open_mem_write(flasher_session_t *s, uint32_t addr, const uint8_t *data, size_t len);
int flasher_open_exec_ram(flasher_session_t *s, uint32_t entry_addr);
int flasher_open_reboot(flasher_session_t *s);

/* High-Level User Operations */
int flasher_start_ram_dumper(flasher_session_t *s);
int flasher_dump_memory(flasher_session_t *s, uint32_t addr, uint32_t len, const char *out_path, uint32_t chunk_size);
int flasher_dump_nand(flasher_session_t *s, uint32_t start_block, uint32_t block_count, const char *out_path);
int flasher_dump_bootrom(flasher_session_t *s, const char *out_path);
int flasher_dump_nvram(flasher_session_t *s, const char *out_path);
int flasher_boot_ram(flasher_session_t *s, uint32_t ram_addr, const uint8_t *data, size_t size);
int flasher_flash(flasher_session_t *s);
int flasher_verify_psi_sha256(const uint8_t *psi_data, size_t psi_len);
int flasher_flash_binary_to_partition(flasher_session_t *s, const char *bin_path, uint32_t start_block, uint32_t max_blocks, const char *label);
int flasher_flash_fls_container(flasher_session_t *s, const char *fls_path);

/* Open Flasher (OFLS / RESP) Protocol API */
#define MAGIC_OFLS_CMD               0x534C464FU /* 'OFLS' */
#define MAGIC_OFLS_RESP              0x50534552U /* 'RESP' */
#define MAGIC_OFLS_PONG              0x474E4F50U /* 'PONG' */

#define OFLS_CMD_PING                0x01U
#define OFLS_CMD_FLASH_ID            0x02U
#define OFLS_CMD_BLOCK_ERASE         0x03U
#define OFLS_CMD_PAGE_WRITE          0x04U
#define OFLS_CMD_PAGE_READ           0x05U
#define OFLS_CMD_RESET               0x06U

typedef struct __attribute__((packed)) {
    uint32_t magic;    /* 'OFLS' (0x534C464F) */
    uint32_t cmd;      /* CMD ID */
    uint32_t addr;     /* Block index or target address */
    uint32_t length;   /* Page index, block count, or payload len */
} ofls_cmd_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;    /* 'RESP' (0x50534552) */
    uint32_t status;   /* 0 = OK, !0 = Error code (0xEE01 = NVRAM protected) */
    uint32_t bytes_done;
    uint32_t crc32;
} ofls_resp_t;

int flasher_start_open_flasher(flasher_session_t *s);
int flasher_ofls_ping(flasher_session_t *s);
int flasher_ofls_id(flasher_session_t *s, uint16_t *out_mfr, uint16_t *out_dev);
int flasher_ofls_erase(flasher_session_t *s, uint32_t block, uint32_t count);
int flasher_ofls_write_page(flasher_session_t *s, uint32_t block, uint32_t page, const uint8_t *data, size_t len);
int flasher_ofls_read_page(flasher_session_t *s, uint32_t block, uint32_t page, uint8_t *out_data, size_t len);
int flasher_ofls_reset(flasher_session_t *s);

#ifdef __cplusplus
}
#endif

#endif /* FLASHER_H */
