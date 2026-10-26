#ifndef FLASHER_H
#define FLASHER_H

#include "fls.h"
#include "protocol.h"
#include "transport.h"
#include "open_psi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *psi_path;
    const char *slb_path;
    const char *bsy_path;
    const char *cds_path;
    const char *dffs_path;
    uint32_t baudrate;
    bool erase_all;
    bool skip_csc;
    bool is_usb;
    bool patch_ebl;
    bool use_open_psi;        /* Use built-in open Rust PSI by default */
    const char *ram_exec_bin; /* Binary to load into RAM */
    uint32_t ram_exec_addr;   /* RAM execution target address (e.g. 0x60000000) */
} flasher_config_t;

typedef struct {
    transport_t *transport;
    flasher_config_t config;
    uint8_t channel;
    uint8_t chip_id;
    uint32_t target_platform;
    char ebl_version[64];
    uint16_t openifwd_seq;
    openifwd_hw_info_t hw_info;
} flasher_session_t;

flasher_session_t *flasher_create(transport_t *transport, const flasher_config_t *cfg);
void flasher_destroy(flasher_session_t *session);

/* Individual stage functions */
int flasher_sync(flasher_session_t *s, int timeout_sec);
int flasher_upload_psi(flasher_session_t *s, const fls_file_t *psi_fls);
int flasher_patch_ebl(uint8_t *ebl_data, size_t ebl_size);
int flasher_upload_ebl(flasher_session_t *s, const fls_file_t *slb_fls);
int flasher_configure_protocol(flasher_session_t *s, const fls_file_t *ref_fls);
int flasher_query_flash_id(flasher_session_t *s);
int flasher_erase_flash(flasher_session_t *s, uint32_t start_addr, uint32_t end_addr);
int flasher_flash_section(flasher_session_t *s, const fls_file_t *fls, const char *name, bool is_nand);
int flasher_flash_image(flasher_session_t *s, const uint8_t *data, size_t size, uint32_t flash_addr);
int flasher_boot_ram(flasher_session_t *s, uint32_t ram_addr, const uint8_t *data, size_t size);
int flasher_verify_checksum(flasher_session_t *s, uint32_t start_addr, uint32_t length, uint16_t expected_chk);

/* OpenIFWD Standard Protocol Engine (Native Open PSI) */
int flasher_open_ping(flasher_session_t *s);
int flasher_open_get_hw_info(flasher_session_t *s, openifwd_hw_info_t *out_info);
int flasher_open_erase_block(flasher_session_t *s, uint32_t block);
int flasher_open_write_page(flasher_session_t *s, uint32_t block, uint16_t page, const uint8_t *data, const uint8_t *spare);
int flasher_open_read_page(flasher_session_t *s, uint32_t block, uint16_t page, uint8_t *out_data, uint8_t *out_spare);
int flasher_open_mem_write(flasher_session_t *s, uint32_t addr, const uint8_t *data, size_t len);
int flasher_open_exec_ram(flasher_session_t *s, uint32_t entry_addr);
int flasher_open_reboot(flasher_session_t *s);
int flasher_dump_nvram_open(flasher_session_t *s, const char *out_path);
int flasher_boot_ram_open(flasher_session_t *s, uint32_t ram_addr, const uint8_t *data, size_t size);
/* NVRAM / EEPROM Protected Range (IMEI, RF Calibration, Board ID) */
#define NVRAM_PROTECTED_START        0x60080000U
#define NVRAM_PROTECTED_END          0x60180000U
#define NVRAM_PROTECTED_SIZE         0x00100000U  /* 1 MB */

#define NVRAM_PROTECT_START          NVRAM_PROTECTED_START
#define NVRAM_PROTECT_END            NVRAM_PROTECTED_END
#define NVRAM_PROTECT_SIZE           NVRAM_PROTECTED_SIZE
#define IFWD_ERR_NVRAM_PROTECTED     0xEE01

static inline bool is_flash_range_safe(uint32_t addr, uint32_t length) {
    uint32_t end = addr + length;
    if ((addr >= NVRAM_PROTECTED_START && addr < NVRAM_PROTECTED_END) ||
        (end > NVRAM_PROTECTED_START && end <= NVRAM_PROTECTED_END) ||
        (addr < NVRAM_PROTECTED_START && end > NVRAM_PROTECTED_END)) {
        return false;
    }
    return true;
}

int flasher_dump_nvram(flasher_session_t *s, const char *out_path);
int flasher_reboot(flasher_session_t *s);

/* Complete flashing sequence */
int flasher_run(flasher_session_t *s);

#ifdef __cplusplus
}
#endif

#endif /* FLASHER_H */
