/* Auto-generated embedded loaders for openIFWD */
#ifndef RAM_DUMPER_EMBEDDED_H
#define RAM_DUMPER_EMBEDDED_H

#include <stdint.h>
#include <stddef.h>

/* Stock Infineon PMB8815 PSI (Stage 1) */
extern const uint8_t stock_psi_binary[57644];
extern const size_t stock_psi_size;
extern const uint8_t stock_psi_checksum;

/* Standalone Open RAM Dumper (Stage 2 in SDRAM at 0x60000000 / 0x60000400) */
extern const uint8_t ram_dumper_binary[1608];
extern const size_t ram_dumper_size;
extern const uint8_t ram_dumper_checksum;

#endif /* RAM_DUMPER_EMBEDDED_H */
