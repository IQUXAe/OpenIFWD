#ifndef PSI_XGOLD215_H
#define PSI_XGOLD215_H

#include <stdint.h>
#include <stddef.h>

/* Primary Stage Initializer for Infineon XMM2150 / PMB8815 / X-Gold 215 / PMB8812 (Chip ID 0x17).
 * Extracted from official FlashTool_E2 (IFWD_DownloadDll.dll at offset 0x471d2 / VA 0x44d8c2).
 * Length: exactly 6,296 bytes. Precalculated XOR-8 Checksum: 0x4C. Entry Point: 0x00080044.
 */
extern const uint8_t xgold215_psi_bootrom[6296];
extern const size_t xgold215_psi_bootrom_size;
extern const uint8_t xgold215_psi_bootrom_checksum;

#endif /* PSI_XGOLD215_H */
