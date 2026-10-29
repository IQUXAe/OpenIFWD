#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "fls.h"
#include "protocol.h"
#include "transport.h"
#include "flasher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <dirent.h>
#include <errno.h>

static void auto_detect_firmware_files(flasher_config_t *cfg, char *auto_slb, char *auto_bsy, char *auto_cds, char *auto_dffs, size_t buf_sz) {
    if (!cfg || !cfg->psi_path) return;

    char dir[512];
    snprintf(dir, sizeof(dir), "%s", cfg->psi_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
    } else {
        snprintf(dir, sizeof(dir), ".");
    }

    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (len < 4) continue;

        /* Match SLB */
        if (!cfg->slb_path && (!cfg->fls_path || !strstr(cfg->fls_path, name)) && (strstr(name, "slb") || strstr(name, "SLB")) && strcmp(name + len - 4, ".fls") == 0) {
            if (snprintf(auto_slb, buf_sz, "%s/%s", dir, name) < (int)buf_sz && access(auto_slb, R_OK) == 0) {
                cfg->slb_path = auto_slb;
                printf("[*] Auto-detected Second Level Bootloader (SLB): %s\n", cfg->slb_path);
            }
        }

        /* Match BSY */
        if (!cfg->bsy_path && (!cfg->fls_path || !strstr(cfg->fls_path, name)) && (strstr(name, "bsy") || strstr(name, "BSY")) && strcmp(name + len - 4, ".fls") == 0) {
            if (snprintf(auto_bsy, buf_sz, "%s/%s", dir, name) < (int)buf_sz && access(auto_bsy, R_OK) == 0) {
                cfg->bsy_path = auto_bsy;
                printf("[*] Auto-detected Baseband System (BSY): %s\n", cfg->bsy_path);
            }
        }

        /* Match CDS */
        if (!cfg->cds_path && (!cfg->fls_path || !strstr(cfg->fls_path, name)) && (strstr(name, "cds") || strstr(name, "CDS")) && strcmp(name + len - 4, ".fls") == 0) {
            if (snprintf(auto_cds, buf_sz, "%s/%s", dir, name) < (int)buf_sz && access(auto_cds, R_OK) == 0) {
                cfg->cds_path = auto_cds;
                printf("[*] Auto-detected Customer Data (CDS): %s\n", cfg->cds_path);
            }
        }

        /* Match CSC / DFFS */
        if (!cfg->skip_csc && !cfg->dffs_path && (!cfg->fls_path || !strstr(cfg->fls_path, name)) && (strstr(name, "csc") || strstr(name, "CSC") || (len >= 5 && strcmp(name + len - 5, ".dffs") == 0))) {
            if (snprintf(auto_dffs, buf_sz, "%s/%s", dir, name) < (int)buf_sz && access(auto_dffs, R_OK) == 0) {
                cfg->dffs_path = auto_dffs;
                printf("[*] Auto-detected Customization / DFFS (CSC): %s\n", cfg->dffs_path);
            }
        }
    }
    closedir(d);
}

static void print_usage(const char *prog) {
    printf("====================================================================\n");
    printf(" OpenIFWD - Open Source Infineon Firmware Downloader & RAM Boot Tool\n");
    printf("====================================================================\n");
    printf("Usage: %s <command> [options]\n\n", prog);
    printf("Commands:\n");
    printf("  info     <file.fls>               Inspect FLS header, chunks and TOC\n");
    printf("  unpack   <file.fls> [out_dir]     Extract binary chunks from FLS container\n");
    printf("  scan                              Scan for connected phone TTY ports\n");
    printf("  dump-bootrom -p <port> [-o <file>] Extract 32KB Boot ROM (PMB8815 physical mask ROM 0x00400000)\n");
    printf("  dump-nvram -p <port> [-o <file>]  Safely backup 1MB NVRAM/EEPROM (IMEI & RF calibrations)\n");
    printf("  dump-nand  -p <port> -b <block> -c <count> -o <file> Dump raw OneNAND physical flash blocks\n");
    printf("  dump-memory -p <port> -a <hex> -l <len> -o <file> [-c <chunk>] Read arbitrary memory range\n");
    printf("  dump-mem   -p <port> -a <hex> -l <len> [-o <file>] Alias for dump-memory\n");
    printf("  ram-boot   -p <port> --bin <file> [--addr 0x60000000] Load & execute binary directly in SDRAM\n");
    printf("  flash    -p <port> [options]      Flash Samsung Star 3 (GT-S5222) firmware\n");
    printf("  flasher-boot -p <port>            Bootstrap & start Open Flasher agent in SDRAM\n");
    printf("  flasher-ping -p <port>            Ping Open Flasher agent\n");
    printf("  flasher-id   -p <port>            Query OneNAND Manufacturer and Device ID\n");
    printf("  flasher-read -p <port> -b <block> --page <page> [-o <file>] [-x] [--lines N] [--all]\n");
    printf("                                    Read 2048-byte OneNAND page (hexdump or save to file)\n");
    printf("  flasher-erase -p <port> -b <block> [-c <count>] Erase OneNAND block(s)\n");
    printf("  flasher-write -p <port> -b <block> --page <page> -i <file> Write 2048-byte OneNAND page\n");
    printf("  flasher-reset -p <port>           Reboot phone via Open Flasher agent\n\n");
    printf("Options:\n");
    printf("  -p, --port <device>     Serial port (/dev/ttyACM0, /dev/ttyUSB0, or 'usb')\n");
    printf("  -o, --out, --output <file> Output file for dumps / page read\n");
    printf("  -x, --hex               Display hex dump of read page in console (hexdump -C format)\n");
    printf("  -l, --lines <num>       Number of 16-byte hex dump lines to display (default: 8 = 128 bytes)\n");
    printf("  -a, --all, --full       Display full 2048-byte page hex dump (all 128 lines)\n");
    printf("  -i, --in, --input <file> Input file for page write\n");
    printf("  -b, --block, --start-block <num> Starting block for NAND operations (0..1023)\n");
    printf("  -c, --count, --block-count <num> Number of blocks to dump / erase (default: 1)\n");
    printf("  -P, --page <num>        Page index within block (0..63)\n");
    printf("  -a, --addr, --address <hex> Base address for memory dump (e.g. 0xFFFF0000, 0x60080000)\n");
    printf("  -l, --len, --length <len>   Length in bytes for memory dump (hex e.g. 0x10000 or decimal)\n");
    printf("  -c, --chunk, --chunk-size <size> Chunk size in bytes (default: 2048, max: 2048)\n");
    printf("  --fls <file.fls>        Firmware container (.fls) to flash\n");
    printf("  --code <file.bin>       Raw binary for CODE partition (Part 3, Block 10..)\n");
    printf("  --csc <file.bin>        Raw binary for CSC partition (Part 4, Block 332..)\n");
    printf("  --bin <file.bin>        Raw binary payload for ram-boot\n");
    printf("  --addr <hex>            Target SDRAM execution address (default: 0x60000000)\n");
    printf("  --psi <file.fls/bin>    Vendor PSI Stage 1 container (.fls) or raw binary (auto-detected if omitted)\n");
    printf("  --flash-psi [file.fls]  Flash PSI primary bootloader into OneNAND Block 0 (BSR)\n");
    printf("  --bsr [file.fls]        Alias for --flash-psi\n");
    printf("  --no-flash-psi          Skip writing PSI into Block 0 during full flash\n");
    printf("  --skip-psi-sha256       Skip SHA-256 integrity verification of PSI binary\n");
    printf("  --skip-psi-check        Alias for --skip-psi-sha256\n");
    printf("  --slb <file.fls>        Second Level Bootloader (_slb.fls, Block 2..9)\n");
    printf("  --bsy <file.fls>        Baseband system code (_BSY.fls)\n");
    printf("  --cds <file.fls>        Customer Data System (_CDS.fls)\n");
    printf("  --dffs <file.dffs>      Data Flash File System (.dffs)\n");
    printf("  --skip-csc              Skip CSC/DFFS flash (boot target OS directly)\n");
    printf("  --no-auto               Disable automatic detection of companion firmware files\n");
    printf("  --force-auto            Force auto-detection even when specific target files are provided\n");
    printf("  -b, --baud <rate>       Baud rate (default: 921600)\n");
    printf("  --erase-all             Erase whole non-protected flash before writing\n");
    printf("  --force-dangerous-erase Allow erasing/writing protected calibration & metadata blocks (978..1023)\n");
    printf("  -v, --verbose           Enable verbose logging (hex dumps and USB packet traces)\n");
    printf("  -h, --help              Show this help menu\n\n");
    printf("Examples:\n");
    printf("  %s flash -p usb --psi S5222XXMJ1_psi.fls --fls S5222XXMJ1_BSY.fls\n", prog);
    printf("  %s flash -p usb --fls S5222XXMJ1_OXEMJ1/S5222XXMJ1_BSY.fls\n", prog);
    printf("  %s flash -p usb --code code.bin --csc csc.bin\n", prog);
    printf("  %s flasher-boot -p usb\n", prog);
    printf("  %s flasher-id -p usb\n", prog);
    printf("  %s flasher-read -p usb -b 2 --page 0 --hex\n", prog);
    printf("  %s flasher-read -p usb -b 0 --page 0 -o block0_page0.bin\n", prog);
    printf("  %s flasher-erase -p usb -b 10 -c 1\n", prog);
    printf("  %s flasher-write -p usb -b 0 -i block0_dump.bin\n", prog);
    printf("  %s flasher-write -p usb -b 10 --page 0 -i custom_payload.bin\n", prog);
    printf("  %s dump-bootrom -p usb -o bootrom_pmb8815_32k.bin -v\n", prog);
    printf("  %s dump-nvram -p usb -o s5222_nvram_backup.bin -v\n", prog);
    printf("  %s dump-nand -p usb -b 0 -c 1 -o nand_block0_boot.bin -v\n", prog);
    printf("  %s dump-nand -p usb -b 4 -c 8 -o nand_nvram_backup.bin -v\n", prog);
    printf("  %s dump-memory -p usb -a 0xFFFF0000 -l 0x10000 -o bootrom_high.bin -v\n", prog);
    printf("  %s dump-memory -p usb -a 0x60080000 -l 0x100000 -o nvram_backup.bin -v\n", prog);
    printf("  %s ram-boot -p usb --bin my_os.bin --addr 0x60000000\n", prog);
    printf("====================================================================\n");
}

static flasher_session_t *init_session(const char *port, flasher_config_t *cfg, transport_t **out_trans) {
    if (!port) {
        fprintf(stderr, "[-] Error: Device port is required (-p usb or -p /dev/ttyACM0)\n");
        return NULL;
    }
    cfg->is_usb = (strcmp(port, "usb") == 0);
    transport_t *trans = NULL;
    if (cfg->is_usb) {
        trans = transport_create_usb();
        if (!trans || transport_open(trans, NULL, cfg->baudrate) != 0) {
            fprintf(stderr, "[-] Failed to open direct USB interface\n");
            if (trans) transport_destroy(trans);
            return NULL;
        }
    } else {
        trans = transport_create_tty();
        if (!trans || transport_open(trans, port, 115200) != 0) {
            fprintf(stderr, "[-] Failed to open port %s\n", port);
            if (trans) transport_destroy(trans);
            return NULL;
        }
    }
    if (cfg->verbose) {
        transport_set_verbose(trans, true);
    }
    *out_trans = trans;
    return flasher_create(trans, cfg);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "info") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s info <file.fls>\n", argv[0]);
            return 1;
        }
        fls_file_t *fls = fls_open(argv[2]);
        if (!fls) return 1;
        fls_print_info(fls);
        fls_close(fls);
        return 0;
    }

    if (strcmp(cmd, "unpack") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s unpack <file.fls> [out_dir]\n", argv[0]);
            return 1;
        }
        const char *out_dir = (argc >= 4) ? argv[3] : ".";
        fls_file_t *fls = fls_open(argv[2]);
        if (!fls) return 1;
        int res = fls_unpack(fls, out_dir);
        fls_close(fls);
        return (res == 0) ? 0 : 1;
    }

    if (strcmp(cmd, "scan") == 0) {
        printf("[*] Scanning for phone TTY ports...\n");
        transport_scan_ports();
        return 0;
    }

    if (strcmp(cmd, "ram-boot") == 0) {
        const char *port = NULL;
        const char *bin_path = NULL;
        uint32_t ram_addr = 0x60000000;
        flasher_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.baudrate = 921600;

        static struct option long_options[] = {
            {"port",    required_argument, 0, 'p'},
            {"bin",     required_argument, 0, 1001},
            {"addr",    required_argument, 0, 1002},
            {"psi",     required_argument, 0, 1003},
            {"verbose", no_argument,       0, 'v'},
            {"help",    no_argument,       0, 'h'},
            {0, 0, 0, 0}
        };

        optind = 2;
        int opt, opt_idx = 0;
        while ((opt = getopt_long(argc, argv, "p:vh", long_options, &opt_idx)) != -1) {
            switch (opt) {
                case 'p': port = optarg; break;
                case 1001: bin_path = optarg; break;
                case 1002: ram_addr = (uint32_t)strtoul(optarg, NULL, 0); break;
                case 1003: cfg.psi_path = optarg; break;
                case 'v': cfg.verbose = true; break;
                case 'h': print_usage(argv[0]); return 0;
                default: return 1;
            }
        }

        if (!port || !bin_path) {
            fprintf(stderr, "[-] Error: -p <port> and --bin <file.bin> are required for ram-boot\n");
            return 1;
        }

        FILE *bf = fopen(bin_path, "rb");
        if (!bf) {
            fprintf(stderr, "[-] Error: Failed to open binary: %s (%s)\n", bin_path, strerror(errno));
            return 1;
        }
        fseek(bf, 0, SEEK_END);
        long bsz = ftell(bf);
        rewind(bf);
        if (bsz <= 0) {
            fprintf(stderr, "[-] Error: Empty binary payload\n");
            fclose(bf);
            return 1;
        }

        uint8_t *bdata = (uint8_t *)malloc((size_t)bsz);
        if (!bdata || fread(bdata, 1, (size_t)bsz, bf) != (size_t)bsz) {
            fprintf(stderr, "[-] Error: Failed to read binary payload\n");
            fclose(bf);
            if (bdata) free(bdata);
            return 1;
        }
        fclose(bf);

        cfg.is_usb = (strcmp(port, "usb") == 0);

        transport_t *trans = NULL;
        if (cfg.is_usb) {
            trans = transport_create_usb();
            if (!trans || transport_open(trans, NULL, cfg.baudrate) != 0) {
                fprintf(stderr, "[-] Failed to open direct USB interface\n");
                if (trans) transport_destroy(trans);
                free(bdata);
                return 1;
            }
        } else {
            trans = transport_create_tty();
            if (!trans || transport_open(trans, port, 115200) != 0) {
                fprintf(stderr, "[-] Failed to open port %s\n", port);
                if (trans) transport_destroy(trans);
                free(bdata);
                return 1;
            }
        }

        if (cfg.verbose) {
            transport_set_verbose(trans, true);
        }

        flasher_session_t *s = flasher_create(trans, &cfg);
        int res = flasher_boot_ram(s, ram_addr, bdata, (size_t)bsz);

        free(bdata);
        flasher_destroy(s);
        transport_destroy(trans);
        return res;
    }

    if (strcmp(cmd, "flash") == 0) {
        flasher_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.baudrate = 921600;

        const char *port = NULL;

        static struct option long_options[] = {
            {"port",      required_argument, 0, 'p'},
            {"baud",      required_argument, 0, 'b'},
            {"psi",       required_argument, 0, 1001},
            {"slb",       required_argument, 0, 1002},
            {"fls",       required_argument, 0, 1010},
            {"code",      required_argument, 0, 1011},
            {"csc",       required_argument, 0, 1012},
            {"bsy",       required_argument, 0, 1003},
            {"cds",       required_argument, 0, 1004},
            {"dffs",      required_argument, 0, 1005},
            {"erase-all", no_argument,       0, 1006},
            {"force-dangerous-erase", no_argument, 0, 1008},
            {"skip-csc",  no_argument,       0, 1007},
            {"no-auto",   no_argument,       0, 1013},
            {"no-autodetect", no_argument,   0, 1013},
            {"auto",      no_argument,       0, 1014},
            {"force-auto", no_argument,      0, 1014},
            {"flash-psi", optional_argument, 0, 1025},
            {"bsr",       optional_argument, 0, 1025},
            {"skip-flash-psi", no_argument,   0, 1026},
            {"no-flash-psi",   no_argument,   0, 1026},
            {"skip-psi-sha256", no_argument, 0, 1020},
            {"skip-psi-check",  no_argument, 0, 1020},
            {"skip-psi-hash",   no_argument, 0, 1020},
            {"no-psi-check",    no_argument, 0, 1020},
            {"verbose",   no_argument,       0, 'v'},
            {"help",      no_argument,       0, 'h'},
            {0, 0, 0, 0}
        };

        optind = 2;
        int opt, opt_idx = 0;
        while ((opt = getopt_long(argc, argv, "p:b:vh", long_options, &opt_idx)) != -1) {
            switch (opt) {
                case 'p': port = optarg; break;
                case 'b': cfg.baudrate = (uint32_t)atoi(optarg); break;
                case 1001: cfg.psi_path = optarg; break;
                case 1002: cfg.slb_path = optarg; break;
                case 1010: cfg.fls_path = optarg; break;
                case 1011: cfg.code_path = optarg; break;
                case 1012: cfg.csc_path = optarg; break;
                case 1003: cfg.bsy_path = optarg; break;
                case 1004: cfg.cds_path = optarg; break;
                case 1005: cfg.dffs_path = optarg; break;
                case 1006: cfg.erase_all = true; break;
                case 1007: cfg.skip_csc = true; break;
                case 1008: cfg.force_dangerous_erase = true; break;
                case 1013: cfg.no_auto = true; break;
                case 1014: cfg.force_auto = true; break;
                case 1025:
                    cfg.flash_psi = true;
                    if (optarg) cfg.flash_psi_path = optarg;
                    break;
                case 1026:
                    cfg.no_flash_psi = true;
                    break;
                case 1020: cfg.skip_psi_sha256 = true; break;
                case 'v': cfg.verbose = true; break;
                case 'h': print_usage(argv[0]); return 0;
                default: return 1;
            }
        }

        if (!port) {
            fprintf(stderr, "[-] Error: Serial port is required (-p /dev/ttyACM0 or -p usb)\n");
            return 1;
        }

        char auto_slb[1024] = {0};
        char auto_bsy[1024] = {0};
        char auto_cds[1024] = {0};
        char auto_dffs[1024] = {0};

        /* Disable auto-detection if user explicitly passed target firmware files,
         * or if --no-auto was requested, unless explicitly overridden by --force-auto */
        bool has_explicit_targets = (cfg.fls_path != NULL || cfg.slb_path != NULL ||
                                     cfg.bsy_path != NULL || cfg.code_path != NULL ||
                                     cfg.csc_path != NULL || cfg.cds_path != NULL ||
                                     cfg.dffs_path != NULL || cfg.flash_psi ||
                                     cfg.flash_psi_path != NULL);

        if (!cfg.no_auto && (!has_explicit_targets || cfg.force_auto)) {
            auto_detect_firmware_files(&cfg, auto_slb, auto_bsy, auto_cds, auto_dffs, sizeof(auto_bsy));
        } else if (has_explicit_targets && !cfg.force_auto) {
            printf("[*] Specific firmware target(s) specified; auto-detection of companion files disabled.\n");
        }

        cfg.is_usb = (strcmp(port, "usb") == 0);

        transport_t *trans = NULL;
        if (cfg.is_usb) {
            trans = transport_create_usb();
            if (!trans || transport_open(trans, NULL, cfg.baudrate) != 0) {
                fprintf(stderr, "[-] Failed to open direct USB interface\n");
                if (trans) transport_destroy(trans);
                return 1;
            }
        } else {
            trans = transport_create_tty();
            if (!trans || transport_open(trans, port, 115200) != 0) {
                fprintf(stderr, "[-] Failed to open port %s\n", port);
                if (trans) transport_destroy(trans);
                return 1;
            }
        }

        if (cfg.verbose) {
            transport_set_verbose(trans, true);
        }

        flasher_session_t *session = flasher_create(trans, &cfg);
        int res = flasher_flash(session);
        flasher_destroy(session);
        transport_destroy(trans);
        return res;
    }

    if (strcmp(cmd, "dump-bootrom") == 0) {
        const char *port = NULL;
        const char *out_path = "bootrom_pmb8815_32k.bin";
        flasher_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.baudrate = 921600;

        static struct option long_options[] = {
            {"port",      required_argument, 0, 'p'},
            {"out",       required_argument, 0, 'o'},
            {"psi",       required_argument, 0, 1001},
            {"skip-psi-sha256", no_argument, 0, 1020},
            {"skip-psi-check",  no_argument, 0, 1020},
            {"skip-psi-hash",   no_argument, 0, 1020},
            {"no-psi-check",    no_argument, 0, 1020},
            {"verbose",   no_argument,       0, 'v'},
            {"help",      no_argument,       0, 'h'},
            {0, 0, 0, 0}
        };

        optind = 2;
        int opt, opt_idx = 0;
        while ((opt = getopt_long(argc, argv, "p:o:vh", long_options, &opt_idx)) != -1) {
            switch (opt) {
                case 'p': port = optarg; break;
                case 'o': out_path = optarg; break;
                case 1001: cfg.psi_path = optarg; break;
                case 1020: cfg.skip_psi_sha256 = true; break;
                case 'v': cfg.verbose = true; break;
                case 'h': print_usage(argv[0]); return 0;
                default: return 1;
            }
        }

        if (!port) {
            fprintf(stderr, "[-] Error: Port is required (-p usb or -p /dev/ttyACM0)\n");
            return 1;
        }

        cfg.is_usb = (strcmp(port, "usb") == 0);

        transport_t *trans = NULL;
        if (cfg.is_usb) {
            trans = transport_create_usb();
            if (!trans || transport_open(trans, NULL, cfg.baudrate) != 0) {
                fprintf(stderr, "[-] Failed to open direct USB interface\n");
                if (trans) transport_destroy(trans);
                return 1;
            }
        } else {
            trans = transport_create_tty();
            if (!trans || transport_open(trans, port, 115200) != 0) {
                fprintf(stderr, "[-] Failed to open port %s\n", port);
                if (trans) transport_destroy(trans);
                return 1;
            }
        }

        if (cfg.verbose) {
            transport_set_verbose(trans, true);
        }

        flasher_session_t *session = flasher_create(trans, &cfg);
        int res = flasher_dump_bootrom(session, out_path);
        flasher_destroy(session);
        transport_destroy(trans);
        return res;
    }

    if (strcmp(cmd, "dump-nvram") == 0) {
        const char *port = NULL;
        const char *out_path = "s5222_nvram_backup.bin";
        flasher_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.baudrate = 921600;

        static struct option long_options[] = {
            {"port",      required_argument, 0, 'p'},
            {"psi",       required_argument, 0, 1001},
            {"skip-psi-sha256", no_argument, 0, 1020},
            {"skip-psi-check",  no_argument, 0, 1020},
            {"skip-psi-hash",   no_argument, 0, 1020},
            {"no-psi-check",    no_argument, 0, 1020},
            {"out",       required_argument, 0, 'o'},
            {"verbose",   no_argument,       0, 'v'},
            {"help",      no_argument,       0, 'h'},
            {0, 0, 0, 0}
        };

        optind = 2;
        int opt, opt_idx = 0;
        while ((opt = getopt_long(argc, argv, "p:o:vh", long_options, &opt_idx)) != -1) {
            switch (opt) {
                case 'p': port = optarg; break;
                case 'o': out_path = optarg; break;
                case 1001: cfg.psi_path = optarg; break;
                case 1020: cfg.skip_psi_sha256 = true; break;
                case 'v': cfg.verbose = true; break;
                case 'h': print_usage(argv[0]); return 0;
                default: return 1;
            }
        }

        if (!port) {
            fprintf(stderr, "[-] Error: Port is required (-p usb or -p /dev/ttyACM0)\n");
            return 1;
        }

        cfg.is_usb = (strcmp(port, "usb") == 0);

        transport_t *trans = NULL;
        if (cfg.is_usb) {
            trans = transport_create_usb();
            if (!trans || transport_open(trans, NULL, cfg.baudrate) != 0) {
                fprintf(stderr, "[-] Failed to open direct USB interface\n");
                if (trans) transport_destroy(trans);
                return 1;
            }
        } else {
            trans = transport_create_tty();
            if (!trans || transport_open(trans, port, 115200) != 0) {
                fprintf(stderr, "[-] Failed to open port %s\n", port);
                if (trans) transport_destroy(trans);
                return 1;
            }
        }

        if (cfg.verbose) {
            transport_set_verbose(trans, true);
        }

        flasher_session_t *session = flasher_create(trans, &cfg);
        int res = flasher_dump_nvram(session, out_path);
        flasher_destroy(session);
        transport_destroy(trans);
        return res;
    }

    if (strcmp(cmd, "dump-memory") == 0 || strcmp(cmd, "dump-mem") == 0) {
        const char *port = NULL;
        const char *out_path = "mem_dump.bin";
        uint32_t addr = 0x00000000;
        uint32_t len = 0x10000;
        uint32_t chunk_size = 2048;
        bool addr_set = false;
        bool len_set = false;
        flasher_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.baudrate = 921600;

        static struct option long_options[] = {
            {"port",       required_argument, 0, 'p'},
            {"addr",       required_argument, 0, 'a'},
            {"address",    required_argument, 0, 'a'},
            {"len",        required_argument, 0, 'l'},
            {"length",     required_argument, 0, 'l'},
            {"out",        required_argument, 0, 'o'},
            {"output",     required_argument, 0, 'o'},
            {"chunk",      required_argument, 0, 'c'},
            {"chunk-size", required_argument, 0, 'c'},
            {"psi",        required_argument, 0, 1001},
            {"skip-psi-sha256", no_argument, 0, 1020},
            {"skip-psi-check",  no_argument, 0, 1020},
            {"skip-psi-hash",   no_argument, 0, 1020},
            {"no-psi-check",    no_argument, 0, 1020},
            {"verbose",    no_argument,       0, 'v'},
            {"help",       no_argument,       0, 'h'},
            {0, 0, 0, 0}
        };

        optind = 2;
        int opt, opt_idx = 0;
        while ((opt = getopt_long(argc, argv, "p:a:l:o:c:vh", long_options, &opt_idx)) != -1) {
            switch (opt) {
                case 'p': port = optarg; break;
                case 'a': addr = (uint32_t)strtoul(optarg, NULL, 0); addr_set = true; break;
                case 'l': len = (uint32_t)strtoul(optarg, NULL, 0); len_set = true; break;
                case 'o': out_path = optarg; break;
                case 'c': chunk_size = (uint32_t)strtoul(optarg, NULL, 0); break;
                case 1001: cfg.psi_path = optarg; break;
                case 1020: cfg.skip_psi_sha256 = true; break;
                case 'v': cfg.verbose = true; break;
                case 'h': print_usage(argv[0]); return 0;
                default: return 1;
            }
        }

        if (!port) {
            fprintf(stderr, "[-] Error: Port is required (-p usb or -p /dev/ttyACM0)\n");
            return 1;
        }

        if (!addr_set) {
            fprintf(stderr, "[-] Error: Address is required (-a <HEX_ADDR>, e.g. -a 0xFFFF0000)\n");
            return 1;
        }

        if (!len_set || len == 0) {
            fprintf(stderr, "[-] Error: Length is required (-l <HEX_OR_DEC_LEN>, e.g. -l 0x10000 or -l 65536)\n");
            return 1;
        }

        if (chunk_size == 0) {
            chunk_size = 2048;
        }
        if (chunk_size > 2048) {
            printf("[*] Capping chunk size to 2048 bytes (maximum supported by USB DMA buffer)\n");
            chunk_size = 2048;
        }

        cfg.is_usb = (strcmp(port, "usb") == 0);

        if (cfg.psi_path) {
            printf("[*] CLI dump-memory: Configured custom PSI path: %s\n", cfg.psi_path);
        }

        transport_t *trans = NULL;
        if (cfg.is_usb) {
            trans = transport_create_usb();
            if (!trans || transport_open(trans, NULL, cfg.baudrate) != 0) {
                fprintf(stderr, "[-] Failed to open direct USB interface\n");
                if (trans) transport_destroy(trans);
                return 1;
            }
        } else {
            trans = transport_create_tty();
            if (!trans || transport_open(trans, port, 115200) != 0) {
                fprintf(stderr, "[-] Failed to open port %s\n", port);
                if (trans) transport_destroy(trans);
                return 1;
            }
        }

        if (cfg.verbose) {
            transport_set_verbose(trans, true);
        }

        flasher_session_t *session = flasher_create(trans, &cfg);
        int res = flasher_dump_memory(session, addr, len, out_path, chunk_size);
        flasher_destroy(session);
        transport_destroy(trans);
        return res;
    }

    if (strcmp(cmd, "dump-nand") == 0) {
        const char *port = NULL;
        const char *out_path = "nand_dump.bin";
        uint32_t start_block = 0;
        uint32_t block_count = 1;
        bool block_set = false;
        flasher_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.baudrate = 921600;

        static struct option long_options[] = {
            {"port",        required_argument, 0, 'p'},
            {"block",       required_argument, 0, 'b'},
            {"start-block", required_argument, 0, 'b'},
            {"count",       required_argument, 0, 'c'},
            {"block-count", required_argument, 0, 'c'},
            {"out",         required_argument, 0, 'o'},
            {"output",      required_argument, 0, 'o'},
            {"psi",         required_argument, 0, 1001},
            {"skip-psi-sha256", no_argument, 0, 1020},
            {"skip-psi-check",  no_argument, 0, 1020},
            {"skip-psi-hash",   no_argument, 0, 1020},
            {"no-psi-check",    no_argument, 0, 1020},
            {"verbose",     no_argument,       0, 'v'},
            {"help",        no_argument,       0, 'h'},
            {0, 0, 0, 0}
        };

        optind = 2;
        int opt, opt_idx = 0;
        while ((opt = getopt_long(argc, argv, "p:b:c:o:vh", long_options, &opt_idx)) != -1) {
            switch (opt) {
                case 'p': port = optarg; break;
                case 'b': start_block = (uint32_t)strtoul(optarg, NULL, 0); block_set = true; break;
                case 'c': block_count = (uint32_t)strtoul(optarg, NULL, 0); break;
                case 'o': out_path = optarg; break;
                case 1001: cfg.psi_path = optarg; break;
                case 1020: cfg.skip_psi_sha256 = true; break;
                case 'v': cfg.verbose = true; break;
                case 'h': print_usage(argv[0]); return 0;
                default: return 1;
            }
        }

        if (!port) {
            fprintf(stderr, "[-] Error: Port is required (-p usb or -p /dev/ttyACM0)\n");
            return 1;
        }

        if (!block_set) {
            fprintf(stderr, "[-] Error: Start block is required (-b <START_BLOCK>, e.g. -b 0)\n");
            return 1;
        }

        if (block_count == 0) {
            fprintf(stderr, "[-] Error: Block count must be greater than 0 (-c <COUNT>)\n");
            return 1;
        }

        if (start_block + block_count > 1024) {
            fprintf(stderr, "[-] Error: Start block (%u) + count (%u) exceeds 1024 total blocks (128MB OneNAND)\n",
                    start_block, block_count);
            return 1;
        }

        cfg.is_usb = (strcmp(port, "usb") == 0);

        transport_t *trans = NULL;
        if (cfg.is_usb) {
            trans = transport_create_usb();
            if (!trans || transport_open(trans, NULL, cfg.baudrate) != 0) {
                fprintf(stderr, "[-] Failed to open direct USB interface\n");
                if (trans) transport_destroy(trans);
                return 1;
            }
        } else {
            trans = transport_create_tty();
            if (!trans || transport_open(trans, port, 115200) != 0) {
                fprintf(stderr, "[-] Failed to open port %s\n", port);
                if (trans) transport_destroy(trans);
                return 1;
            }
        }

        if (cfg.verbose) {
            transport_set_verbose(trans, true);
        }

        flasher_session_t *session = flasher_create(trans, &cfg);
        int res = flasher_dump_nand(session, start_block, block_count, out_path);
        flasher_destroy(session);
        transport_destroy(trans);
        return res;
    }

    if (strcmp(cmd, "flasher-boot") == 0) {
        const char *port = NULL;
        flasher_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.baudrate = 921600;

        static struct option long_options[] = {
            {"port",      required_argument, 0, 'p'},
            {"psi",       required_argument, 0, 1001},
            {"skip-psi-sha256", no_argument, 0, 1020},
            {"skip-psi-check",  no_argument, 0, 1020},
            {"skip-psi-hash",   no_argument, 0, 1020},
            {"no-psi-check",    no_argument, 0, 1020},
            {"verbose",   no_argument,       0, 'v'},
            {"help",      no_argument,       0, 'h'},
            {0, 0, 0, 0}
        };

        optind = 2;
        int opt, opt_idx = 0;
        while ((opt = getopt_long(argc, argv, "p:vh", long_options, &opt_idx)) != -1) {
            switch (opt) {
                case 'p': port = optarg; break;
                case 1001: cfg.psi_path = optarg; break;
                case 1020: cfg.skip_psi_sha256 = true; break;
                case 'v': cfg.verbose = true; break;
                case 'h': print_usage(argv[0]); return 0;
                default: return 1;
            }
        }

        transport_t *trans = NULL;
        flasher_session_t *session = init_session(port, &cfg, &trans);
        if (!session) return 1;

        int res = flasher_start_open_flasher(session);
        flasher_destroy(session);
        transport_destroy(trans);
        return res;
    }

    if (strcmp(cmd, "flasher-ping") == 0) {
        const char *port = NULL;
        flasher_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.baudrate = 921600;

        static struct option long_options[] = {
            {"port",      required_argument, 0, 'p'},
            {"psi",       required_argument, 0, 1001},
            {"skip-psi-sha256", no_argument, 0, 1020},
            {"skip-psi-check",  no_argument, 0, 1020},
            {"skip-psi-hash",   no_argument, 0, 1020},
            {"no-psi-check",    no_argument, 0, 1020},
            {"verbose",   no_argument,       0, 'v'},
            {"help",      no_argument,       0, 'h'},
            {0, 0, 0, 0}
        };

        optind = 2;
        int opt, opt_idx = 0;
        while ((opt = getopt_long(argc, argv, "p:vh", long_options, &opt_idx)) != -1) {
            switch (opt) {
                case 'p': port = optarg; break;
                case 1001: cfg.psi_path = optarg; break;
                case 1020: cfg.skip_psi_sha256 = true; break;
                case 'v': cfg.verbose = true; break;
                case 'h': print_usage(argv[0]); return 0;
                default: return 1;
            }
        }

        transport_t *trans = NULL;
        flasher_session_t *session = init_session(port, &cfg, &trans);
        if (!session) return 1;

        int res = flasher_ofls_ping(session);
        flasher_destroy(session);
        transport_destroy(trans);
        return res;
    }

    if (strcmp(cmd, "flasher-id") == 0) {
        const char *port = NULL;
        flasher_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.baudrate = 921600;

        static struct option long_options[] = {
            {"port",      required_argument, 0, 'p'},
            {"psi",       required_argument, 0, 1001},
            {"skip-psi-sha256", no_argument, 0, 1020},
            {"skip-psi-check",  no_argument, 0, 1020},
            {"skip-psi-hash",   no_argument, 0, 1020},
            {"no-psi-check",    no_argument, 0, 1020},
            {"verbose",   no_argument,       0, 'v'},
            {"help",      no_argument,       0, 'h'},
            {0, 0, 0, 0}
        };

        optind = 2;
        int opt, opt_idx = 0;
        while ((opt = getopt_long(argc, argv, "p:vh", long_options, &opt_idx)) != -1) {
            switch (opt) {
                case 'p': port = optarg; break;
                case 1001: cfg.psi_path = optarg; break;
                case 1020: cfg.skip_psi_sha256 = true; break;
                case 'v': cfg.verbose = true; break;
                case 'h': print_usage(argv[0]); return 0;
                default: return 1;
            }
        }

        transport_t *trans = NULL;
        flasher_session_t *session = init_session(port, &cfg, &trans);
        if (!session) return 1;

        uint16_t mfr = 0, dev = 0;
        int res = flasher_ofls_id(session, &mfr, &dev);
        flasher_destroy(session);
        transport_destroy(trans);
        return res;
    }

    if (strcmp(cmd, "flasher-read") == 0) {
        const char *port = NULL;
        uint32_t block = 0;
        uint32_t page = 0;
        const char *out_path = NULL;
        bool hex_mode = false;
        bool full_page = false;
        size_t max_lines = 8; /* Default: 8 lines = 128 bytes */
        flasher_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.baudrate = 921600;

        static struct option long_options[] = {
            {"port",      required_argument, 0, 'd'},
            {"block",     required_argument, 0, 'b'},
            {"page",      required_argument, 0, 'P'},
            {"out",       required_argument, 0, 'o'},
            {"hex",       no_argument,       0, 'x'},
            {"lines",     required_argument, 0, 'l'},
            {"all",       no_argument,       0, 'a'},
            {"full",      no_argument,       0, 'a'},
            {"psi",       required_argument, 0, 1001},
            {"skip-psi-sha256", no_argument, 0, 1020},
            {"skip-psi-check",  no_argument, 0, 1020},
            {"skip-psi-hash",   no_argument, 0, 1020},
            {"no-psi-check",    no_argument, 0, 1020},
            {"verbose",   no_argument,       0, 'v'},
            {"help",      no_argument,       0, 'h'},
            {0, 0, 0, 0}
        };

        optind = 2;
        int opt, opt_idx = 0;
        while ((opt = getopt_long(argc, argv, "d:b:P:p:o:l:n:xavh", long_options, &opt_idx)) != -1) {
            switch (opt) {
                case 'd': port = optarg; break;
                case 'b': block = (uint32_t)strtoul(optarg, NULL, 0); break;
                case 'P': page = (uint32_t)strtoul(optarg, NULL, 0); break;
                case 'p':
                    if (optarg[0] >= '0' && optarg[0] <= '9') {
                        page = (uint32_t)strtoul(optarg, NULL, 0);
                    } else {
                        port = optarg;
                    }
                    break;
                case 'o': out_path = optarg; break;
                case 'x': hex_mode = true; break;
                case 'l':
                case 'n':
                    max_lines = (size_t)strtoul(optarg, NULL, 0);
                    hex_mode = true;
                    break;
                case 'a':
                    full_page = true;
                    hex_mode = true;
                    max_lines = 0;
                    break;
                case 1001: cfg.psi_path = optarg; break;
                case 1020: cfg.skip_psi_sha256 = true; break;
                case 'v': cfg.verbose = true; break;
                case 'h': print_usage(argv[0]); return 0;
                default: return 1;
            }
        }

        /* If no output file was specified, default to hex dump mode in console */
        if (!out_path) {
            hex_mode = true;
        }

        transport_t *trans = NULL;
        flasher_session_t *session = init_session(port, &cfg, &trans);
        if (!session) return 1;

        uint8_t page_buf[2048];
        int res = flasher_ofls_read_page(session, block, page, page_buf, 2048);
        if (res == 0) {
            if (out_path && !hex_mode) {
                FILE *f = fopen(out_path, "wb");
                if (f) {
                    fwrite(page_buf, 1, 2048, f);
                    fclose(f);
                    printf("[+] Saved 2048 bytes of Block %u, Page %u to %s\n", block, page, out_path);
                } else {
                    fprintf(stderr, "[-] Failed to open output file: %s (%s)\n", out_path, strerror(errno));
                    res = 1;
                }
            } else {
                printf("[*] OneNAND Block %u, Page %u (2048 bytes):\n", block, page);
                flasher_hexdump(page_buf, 2048, full_page ? 0 : max_lines);
            }
        }

        flasher_destroy(session);
        transport_destroy(trans);
        return res;
    }

    if (strcmp(cmd, "flasher-erase") == 0) {
        const char *port = NULL;
        uint32_t block = 0;
        uint32_t count = 1;
        flasher_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.baudrate = 921600;

        static struct option long_options[] = {
            {"port",      required_argument, 0, 'p'},
            {"block",     required_argument, 0, 'b'},
            {"count",     required_argument, 0, 'c'},
            {"force-dangerous-erase", no_argument, 0, 1002},
            {"psi",       required_argument, 0, 1001},
            {"skip-psi-sha256", no_argument, 0, 1020},
            {"skip-psi-check",  no_argument, 0, 1020},
            {"skip-psi-hash",   no_argument, 0, 1020},
            {"no-psi-check",    no_argument, 0, 1020},
            {"verbose",   no_argument,       0, 'v'},
            {"help",      no_argument,       0, 'h'},
            {0, 0, 0, 0}
        };

        optind = 2;
        int opt, opt_idx = 0;
        while ((opt = getopt_long(argc, argv, "p:b:c:vh", long_options, &opt_idx)) != -1) {
            switch (opt) {
                case 'p': port = optarg; break;
                case 'b': block = (uint32_t)strtoul(optarg, NULL, 0); break;
                case 'c': count = (uint32_t)strtoul(optarg, NULL, 0); break;
                case 1001: cfg.psi_path = optarg; break;
                case 1002: cfg.force_dangerous_erase = true; break;
                case 1020: cfg.skip_psi_sha256 = true; break;
                case 'v': cfg.verbose = true; break;
                case 'h': print_usage(argv[0]); return 0;
                default: return 1;
            }
        }

        transport_t *trans = NULL;
        flasher_session_t *session = init_session(port, &cfg, &trans);
        if (!session) return 1;

        int res = flasher_ofls_erase(session, block, count);
        flasher_destroy(session);
        transport_destroy(trans);
        return res;
    }

    if (strcmp(cmd, "flasher-write") == 0) {
        const char *port = NULL;
        uint32_t block = 0;
        uint32_t page = 0;
        bool has_page = false;
        const char *in_path = NULL;
        flasher_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.baudrate = 921600;

        static struct option long_options[] = {
            {"port",      required_argument, 0, 'd'},
            {"block",     required_argument, 0, 'b'},
            {"page",      required_argument, 0, 'P'},
            {"input",     required_argument, 0, 'i'},
            {"in",        required_argument, 0, 'i'},
            {"force-dangerous-erase", no_argument, 0, 1002},
            {"psi",       required_argument, 0, 1001},
            {"skip-psi-sha256", no_argument, 0, 1020},
            {"skip-psi-check",  no_argument, 0, 1020},
            {"skip-psi-hash",   no_argument, 0, 1020},
            {"no-psi-check",    no_argument, 0, 1020},
            {"verbose",   no_argument,       0, 'v'},
            {"help",      no_argument,       0, 'h'},
            {0, 0, 0, 0}
        };

        optind = 2;
        int opt, opt_idx = 0;
        while ((opt = getopt_long(argc, argv, "d:b:P:p:i:vh", long_options, &opt_idx)) != -1) {
            switch (opt) {
                case 'd': port = optarg; break;
                case 'b': block = (uint32_t)strtoul(optarg, NULL, 0); break;
                case 'P':
                    page = (uint32_t)strtoul(optarg, NULL, 0);
                    has_page = true;
                    break;
                case 'p':
                    if (optarg[0] >= '0' && optarg[0] <= '9') {
                        page = (uint32_t)strtoul(optarg, NULL, 0);
                        has_page = true;
                    } else {
                        port = optarg;
                    }
                    break;
                case 'i': in_path = optarg; break;
                case 1001: cfg.psi_path = optarg; break;
                case 1002: cfg.force_dangerous_erase = true; break;
                case 1020: cfg.skip_psi_sha256 = true; break;
                case 'v': cfg.verbose = true; break;
                case 'h': print_usage(argv[0]); return 0;
                default: return 1;
            }
        }

        if (!in_path) {
            fprintf(stderr, "[-] Error: Input file path is required (-i <file>)\n");
            return 1;
        }

        FILE *f = fopen(in_path, "rb");
        if (!f) {
            fprintf(stderr, "[-] Failed to open input file: %s (%s)\n", in_path, strerror(errno));
            return 1;
        }

        fseek(f, 0, SEEK_END);
        long file_sz = ftell(f);
        rewind(f);

        if (file_sz <= 0) {
            fprintf(stderr, "[-] Error: Input file %s is empty\n", in_path);
            fclose(f);
            return 1;
        }

        transport_t *trans = NULL;
        flasher_session_t *session = init_session(port, &cfg, &trans);
        if (!session) {
            fclose(f);
            return 1;
        }

        int res = 0;
        if (!has_page || file_sz > (long)ONENAND_PAGE_SIZE) {
            /* Streaming / multi-block mode */
            uint32_t blocks = (uint32_t)((file_sz + ONENAND_BLOCK_SIZE - 1) / ONENAND_BLOCK_SIZE);
            uint32_t total_pages = (uint32_t)((file_sz + ONENAND_PAGE_SIZE - 1) / ONENAND_PAGE_SIZE);

            printf("[*] Multi-page write: %ld bytes -> %u block(s), %u page(s) starting from Block %u\n",
                   file_sz, blocks, total_pages, block);

            /* Erase target blocks first */
            printf("[*] Erasing %u block(s) starting from Block %u...\n", blocks, block);
            res = flasher_ofls_erase(session, block, blocks);
            if (res != 0) {
                fprintf(stderr, "[-] Failed to erase block range %u..%u\n", block, block + blocks - 1);
                fclose(f);
                flasher_destroy(session);
                transport_destroy(trans);
                return res;
            }

            uint8_t page_buf[ONENAND_PAGE_SIZE];
            for (uint32_t p = 0; p < total_pages; p++) {
                uint32_t curr_blk = block + (p / ONENAND_PAGES_PER_BLOCK);
                uint32_t curr_page = p % ONENAND_PAGES_PER_BLOCK;

                memset(page_buf, 0xFF, sizeof(page_buf));
                size_t n = fread(page_buf, 1, sizeof(page_buf), f);
                if (n == 0 && ferror(f)) {
                    fprintf(stderr, "\n[-] File read error at page %u\n", p);
                    res = -1;
                    break;
                }

                res = flasher_ofls_write_page(session, curr_blk, curr_page, page_buf, sizeof(page_buf));
                if (res != 0) {
                    fprintf(stderr, "\n[-] Failed to write Block %u, Page %u (status %d)\n", curr_blk, curr_page, res);
                    break;
                }

                int pct = (int)(((p + 1) * 100ULL) / total_pages);
                printf("\r[*] Progress: %3d%% (%u / %u pages written, Block %u Page %u)   ",
                       pct, p + 1, total_pages, curr_blk, curr_page);
                fflush(stdout);
            }
            if (res == 0) {
                printf("\n[+] Successfully programmed %ld bytes (%u pages) across %u block(s) starting at Block %u!\n",
                       file_sz, total_pages, blocks, block);
            }
        } else {
            /* Single page targeted write mode */
            uint8_t page_buf[ONENAND_PAGE_SIZE];
            memset(page_buf, 0xFF, sizeof(page_buf));
            size_t n = fread(page_buf, 1, sizeof(page_buf), f);
            (void)n;

            res = flasher_ofls_write_page(session, block, page, page_buf, sizeof(page_buf));
            if (res == 0) {
                printf("[+] Successfully programmed 2048 bytes to Block %u, Page %u!\n", block, page);
            } else {
                fprintf(stderr, "[-] Failed to write Block %u, Page %u (status %d)\n", block, page, res);
            }
        }

        fclose(f);
        flasher_destroy(session);
        transport_destroy(trans);
        return res;
    }

    if (strcmp(cmd, "flasher-reset") == 0) {
        const char *port = NULL;
        flasher_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.baudrate = 921600;

        static struct option long_options[] = {
            {"port",      required_argument, 0, 'p'},
            {"verbose",   no_argument,       0, 'v'},
            {"help",      no_argument,       0, 'h'},
            {0, 0, 0, 0}
        };

        optind = 2;
        int opt, opt_idx = 0;
        while ((opt = getopt_long(argc, argv, "p:vh", long_options, &opt_idx)) != -1) {
            switch (opt) {
                case 'p': port = optarg; break;
                case 'v': cfg.verbose = true; break;
                case 'h': print_usage(argv[0]); return 0;
                default: return 1;
            }
        }

        transport_t *trans = NULL;
        flasher_session_t *session = init_session(port, &cfg, &trans);
        if (!session) return 1;

        int res = flasher_ofls_reset(session);
        flasher_destroy(session);
        transport_destroy(trans);
        return res;
    }

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    print_usage(argv[0]);
    return 1;
}
