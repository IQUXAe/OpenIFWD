#ifndef FLASHER_H
#define FLASHER_H

#include "fls.h"
#include "protocol.h"
#include "transport.h"

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
} flasher_config_t;

typedef struct {
    transport_t *transport;
    flasher_config_t config;
    uint8_t channel;
    uint8_t chip_id;
    uint32_t target_platform;
    char ebl_version[64];
} flasher_session_t;

flasher_session_t *flasher_create(transport_t *transport, const flasher_config_t *cfg);
void flasher_destroy(flasher_session_t *session);

/* Individual stage functions */
int flasher_sync(flasher_session_t *s, int timeout_sec);
int flasher_upload_psi(flasher_session_t *s, const fls_file_t *psi_fls);
int flasher_upload_ebl(flasher_session_t *s, const fls_file_t *slb_fls);
int flasher_configure_protocol(flasher_session_t *s, const fls_file_t *ref_fls);
int flasher_query_flash_id(flasher_session_t *s);
int flasher_erase_flash(flasher_session_t *s, uint32_t start_addr, uint32_t end_addr);
int flasher_flash_section(flasher_session_t *s, const fls_file_t *fls, const char *name, bool is_nand);
int flasher_flash_image(flasher_session_t *s, const uint8_t *data, size_t size, uint32_t flash_addr);
int flasher_boot_ram(flasher_session_t *s, uint32_t ram_addr, const uint8_t *data, size_t size);
int flasher_verify_checksum(flasher_session_t *s, uint32_t start_addr, uint32_t length, uint16_t expected_chk);
/* NVRAM / EEPROM Protected Range (IMEI, RF Calibration, Board ID) */
#define NVRAM_PROTECT_START          0x60080000
#define NVRAM_PROTECT_END            0x60180000
#define NVRAM_PROTECT_SIZE           0x00100000  /* 1 MB */

int flasher_dump_nvram(flasher_session_t *s, const char *out_path);
int flasher_reboot(flasher_session_t *s);

/* Complete flashing sequence */
int flasher_run(flasher_session_t *s);

#ifdef __cplusplus
}
#endif

#endif /* FLASHER_H */
