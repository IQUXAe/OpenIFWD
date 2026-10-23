#define _POSIX_C_SOURCE 200809L
#include "fls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

const char *fls_tag_name(uint32_t tag) {
    switch (tag) {
        case FLS_TAG_FILE_HDR:     return "FILE_HDR (Meta / Signature)";
        case FLS_TAG_PLATFORM_CFG: return "PLATFORM_CFG (Platform Config)";
        case FLS_TAG_PSI_RAM:      return "PSI_RAM (Primary Stage Initializer)";
        case FLS_TAG_IMAGE_DATA:   return "IMAGE_DATA (Flash Binary Image)";
        case FLS_TAG_EBL_RAM:      return "EBL_RAM (Embedded Bootloader)";
        case FLS_TAG_SEC_CFG:      return "SEC_CFG (Security Config)";
        case FLS_TAG_TOC:          return "TOC (Table of Contents)";
        case FLS_TAG_ADDR_MAP:     return "ADDR_MAP (Memory Map / Partition)";
        case FLS_TAG_NVM:          return "NVM (Non-Volatile Memory)";
        case FLS_TAG_XML_DATA:     return "XML_DATA (Injected XML Config)";
        case FLS_TAG_CERTIFICATE:  return "CERTIFICATE (Security Cert)";
        default:                   return "UNKNOWN";
    }
}

static int ensure_dir(const char *dir) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    size_t len = strlen(tmp);
    if (len == 0) return 0;
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

fls_file_t *fls_open(const char *path) {
    if (!path) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("fopen FLS");
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long fsize = ftell(f);
    if (fsize < (long)sizeof(fls_chunk_hdr_t)) {
        fprintf(stderr, "FLS file too small: %ld bytes\n", fsize);
        fclose(f);
        return NULL;
    }
    rewind(f);

    uint8_t *buffer = (uint8_t *)malloc(fsize);
    if (!buffer) {
        fclose(f);
        return NULL;
    }

    if (fread(buffer, 1, fsize, f) != (size_t)fsize) {
        fprintf(stderr, "Failed to read entire FLS file into memory\n");
        free(buffer);
        fclose(f);
        return NULL;
    }
    fclose(f);

    fls_file_t *fls = (fls_file_t *)calloc(1, sizeof(fls_file_t));
    if (!fls) {
        free(buffer);
        return NULL;
    }

    fls->filepath = strdup(path);
    fls->raw_data = buffer;
    fls->raw_size = (size_t)fsize;
    fls->chunk_capacity = 32;
    fls->chunks = (fls_chunk_info_t *)malloc(fls->chunk_capacity * sizeof(fls_chunk_info_t));

    size_t pos = 0;
    while (pos + sizeof(fls_chunk_hdr_t) <= fls->raw_size) {
        const fls_chunk_hdr_t *hdr = (const fls_chunk_hdr_t *)(fls->raw_data + pos);
        if (hdr->length < sizeof(fls_chunk_hdr_t) || pos + hdr->length > fls->raw_size) {
            /* End of TLV chunks or padding */
            break;
        }

        if (fls->chunk_count >= fls->chunk_capacity) {
            fls->chunk_capacity *= 2;
            fls->chunks = (fls_chunk_info_t *)realloc(fls->chunks, fls->chunk_capacity * sizeof(fls_chunk_info_t));
        }

        fls_chunk_info_t *info = &fls->chunks[fls->chunk_count++];
        info->tag = hdr->tag;
        info->length = hdr->length;
        info->flags = hdr->flags;
        info->offset = pos;
        info->payload_off = pos + sizeof(fls_chunk_hdr_t);
        info->payload_size = hdr->length - sizeof(fls_chunk_hdr_t);

        /* Check if this is the Table of Contents (Tag 16) */
        if (hdr->tag == FLS_TAG_TOC && info->payload_size >= sizeof(fls_toc_hdr_t)) {
            const fls_toc_hdr_t *toc_hdr = (const fls_toc_hdr_t *)(fls->raw_data + info->payload_off);
            size_t items = toc_hdr->num_items;
            size_t available_bytes = info->payload_size - sizeof(fls_toc_hdr_t);
            size_t max_items = available_bytes / sizeof(fls_toc_entry_t);

            if (items > max_items) {
                items = max_items;
            }

            if (items > 0) {
                fls->toc_count = items;
                fls->toc_entries = (fls_toc_entry_t *)calloc(items, sizeof(fls_toc_entry_t));
                const fls_toc_entry_t *src_entries = (const fls_toc_entry_t *)(fls->raw_data + info->payload_off + sizeof(fls_toc_hdr_t));
                memcpy(fls->toc_entries, src_entries, items * sizeof(fls_toc_entry_t));
            }
        }

        pos += hdr->length;
    }

    return fls;
}

void fls_close(fls_file_t *fls) {
    if (!fls) return;
    free(fls->filepath);
    free(fls->raw_data);
    free(fls->chunks);
    free(fls->toc_entries);
    free(fls);
}

const fls_chunk_info_t *fls_find_chunk(const fls_file_t *fls, uint32_t tag) {
    if (!fls) return NULL;
    for (size_t i = 0; i < fls->chunk_count; i++) {
        if (fls->chunks[i].tag == tag) {
            return &fls->chunks[i];
        }
    }
    return NULL;
}

const uint8_t *fls_get_chunk_payload(const fls_file_t *fls, const fls_chunk_info_t *chunk, size_t *out_size) {
    if (!fls || !chunk) return NULL;
    if (chunk->payload_off + chunk->payload_size > fls->raw_size) return NULL;
    if (out_size) *out_size = chunk->payload_size;
    return fls->raw_data + chunk->payload_off;
}

void fls_print_info(const fls_file_t *fls) {
    if (!fls) return;

    printf("================================================================================\n");
    printf("FLS Container: %s\n", fls->filepath);
    printf("Total Size:    %zu bytes (%.2f MB)\n", fls->raw_size, (double)fls->raw_size / (1024.0 * 1024.0));
    printf("Found Chunks:  %zu\n", fls->chunk_count);
    printf("--------------------------------------------------------------------------------\n");
    printf("%-5s | %-6s | %-12s | %-10s | %s\n", "Idx", "Tag", "Offset", "Size", "Type Description");
    printf("------+--------+--------------+------------+------------------------------------\n");

    for (size_t i = 0; i < fls->chunk_count; i++) {
        const fls_chunk_info_t *c = &fls->chunks[i];
        printf("[%02zu]  | %-6u | 0x%08zx   | %-10zu | %s\n",
               i, c->tag, c->offset, c->payload_size, fls_tag_name(c->tag));
    }

    if (fls->toc_count > 0) {
        printf("\n--- Table of Contents (TOC: %zu items) ---\n", fls->toc_count);
        for (size_t t = 0; t < fls->toc_count; t++) {
            const fls_toc_entry_t *entry = &fls->toc_entries[t];
            printf("  Item [%zu]:\n", t + 1);
            printf("    Filename:   %s\n", entry->filename[0] ? entry->filename : "<unnamed>");
            printf("    Tag ID:     %u\n", entry->tag_id);
            printf("    Mem Class:  0x%08x\n", entry->mem_class);
            printf("    Load Addr:  0x%08x\n", entry->load_address);
            printf("    Length:     0x%08x (%u bytes)\n", entry->length, entry->length);
            printf("    Checksum:   0x%08x\n", entry->checksum);
        }
    }
    printf("================================================================================\n");
}

int fls_unpack(const fls_file_t *fls, const char *out_dir) {
    if (!fls || !out_dir) return -1;
    if (ensure_dir(out_dir) != 0) {
        fprintf(stderr, "Failed to create directory: %s\n", out_dir);
        return -1;
    }

    char path[1024];
    for (size_t i = 0; i < fls->chunk_count; i++) {
        const fls_chunk_info_t *c = &fls->chunks[i];
        snprintf(path, sizeof(path), "%s/chunk_%02zu_tag%u.bin", out_dir, i, c->tag);

        FILE *f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "Failed to write %s: %s\n", path, strerror(errno));
            continue;
        }

        size_t written = fwrite(fls->raw_data + c->payload_off, 1, c->payload_size, f);
        fclose(f);
        printf("Extracted: %s (%zu bytes)\n", path, written);
    }
    return 0;
}
