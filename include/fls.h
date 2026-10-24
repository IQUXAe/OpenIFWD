#ifndef FLS_H
#define FLS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Known FLS chunk tags */
#define FLS_TAG_FILE_HDR          1000  /* File metadata / signature */
#define FLS_TAG_PLATFORM_CFG      3     /* Platform config */
#define FLS_TAG_PARTITION_TABLE   11    /* Flash / RAM partition definitions (1628 bytes) */
#define FLS_TAG_PSI_RAM           11    /* Backward-compatible alias */
#define FLS_TAG_IMAGE_DATA        12    /* Main Firmware / Flash Image */
#define FLS_TAG_PLATFORM_EXT      13    /* Platform extended parameters (172 bytes) */
#define FLS_TAG_EBL_RAM           14    /* Embedded Boot Loader (RAM) */
#define FLS_TAG_SEC_CFG           15    /* Security configuration */
#define FLS_TAG_TOC               16    /* Table of Contents */
#define FLS_TAG_ADDR_MAP          17    /* Address mapping / partition */
#define FLS_TAG_RPSI              18    /* Primary Stage Initializer / Bootloader executable (RAM) */
#define FLS_TAG_NVM               18    /* Backward-compatible alias */
#define FLS_TAG_SECONDARY_LOADER  19    /* Secondary Loader / EBL payload */
#define FLS_TAG_XML_DATA          21    /* Injected XML configuration */
#define FLS_TAG_CERTIFICATE       31    /* Security certificates */

#pragma pack(push, 1)

/* 12-byte TLV header for every FLS chunk */
typedef struct {
    uint32_t tag;
    uint32_t length;  /* Total chunk size including this 12-byte header */
    uint32_t flags;
} fls_chunk_hdr_t;

/* TOC header preceding the TOC entries in Tag 16 (12 bytes) */
typedef struct {
    uint32_t num_items;
    uint32_t reserved;
    uint32_t entries_offset;
} fls_toc_hdr_t;

/* Each TOC entry in Tag 16 is 144 bytes (0x90) */
typedef struct {
    uint32_t tag_id;          /* Associated chunk tag or 0 */
    uint32_t mem_class;       /* Memory class: 1=PSI, 2=SLB/EBL, 4=CODE/Flash */
    uint32_t load_address;    /* Target flash load address */
    uint32_t reserved;        /* Reserved / padding */
    char     filename[128];   /* Name or relative path of component */
} fls_toc_entry_t;

#pragma pack(pop)

/* In-memory representation of a parsed chunk */
typedef struct {
    uint32_t tag;
    uint32_t length;
    uint32_t flags;
    size_t   offset;       /* File offset of the chunk header */
    size_t   payload_off;  /* File offset of the payload (offset + 12) */
    size_t   payload_size; /* Payload length (length - 12) */
} fls_chunk_info_t;

/* Parsed FLS file handle */
typedef struct {
    char *filepath;
    uint8_t *raw_data;
    size_t raw_size;

    fls_chunk_info_t *chunks;
    size_t chunk_count;
    size_t chunk_capacity;

    fls_toc_entry_t *toc_entries;
    size_t toc_count;
} fls_file_t;

/* Functions */
fls_file_t *fls_open(const char *path);
void fls_close(fls_file_t *fls);

const fls_chunk_info_t *fls_find_chunk(const fls_file_t *fls, uint32_t tag);
const uint8_t *fls_get_chunk_payload(const fls_file_t *fls, const fls_chunk_info_t *chunk, size_t *out_size);
const char *fls_tag_name(uint32_t tag);

void fls_print_info(const fls_file_t *fls);
int fls_unpack(const fls_file_t *fls, const char *out_dir);

#ifdef __cplusplus
}
#endif

#endif /* FLS_H */
