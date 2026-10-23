#define _POSIX_C_SOURCE 200809L

#include "fls.h"
#include "protocol.h"
#include "transport.h"
#include "flasher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

static void print_usage(const char *prog) {
    printf("====================================================================\n");
    printf(" OpenIFWD - Open Source Infineon Firmware Downloader & RAM Boot Tool\n");
    printf("====================================================================\n");
    printf("Usage: %s <command> [options]\n\n", prog);
    printf("Commands:\n");
    printf("  info     <file.fls>               Inspect FLS header, chunks and TOC\n");
    printf("  unpack   <file.fls> [out_dir]     Extract binary chunks from FLS container\n");
    printf("  scan                              Scan for connected phone TTY ports\n");
    printf("  ram-boot -p <port> --bin <file>   Load & execute binary directly in RAM\n");
    printf("  flash    [options]                Flash Samsung Star 3 (GT-S5222)\n\n");
    printf("Flash Options:\n");
    printf("  -p, --port <device>     Serial port (e.g. /dev/ttyACM0, /dev/ttyUSB0, or 'usb')\n");
    printf("  --psi <file.fls>        Primary Stage Initializer (_psi.fls)\n");
    printf("  --slb <file.fls>        Secondary Loader Binary / EBL (_slb.fls)\n");
    printf("  --bsy <file.fls>        Baseband system code (_BSY.fls)\n");
    printf("  --cds <file.fls>        Customer Data System (_CDS.fls)\n");
    printf("  --dffs <file.dffs>      Data Flash File System (.dffs)\n");
    printf("  -b, --baud <rate>       Baud rate (default: 921600)\n");
    printf("  --erase-all             Erase whole flash before writing\n");
    printf("  -h, --help              Show this help menu\n\n");
    printf("Examples:\n");
    printf("  %s info catfish3g.fls\n", prog);
    printf("  %s unpack catfish3g.fls ./extracted\n", prog);
    printf("  %s scan\n", prog);
    printf("  %s flash -p /dev/ttyACM0 --psi S5222_psi.fls --slb S5222_slb.fls --bsy S5222_BSY.fls\n", prog);
    printf("====================================================================\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "info") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: missing FLS file path.\nUsage: %s info <file.fls>\n", argv[0]);
            return 1;
        }
        fls_file_t *fls = fls_open(argv[2]);
        if (!fls) {
            fprintf(stderr, "Failed to open FLS file: %s\n", argv[2]);
            return 1;
        }
        fls_print_info(fls);
        fls_close(fls);
        return 0;
    }

    if (strcmp(cmd, "unpack") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: missing FLS file path.\nUsage: %s unpack <file.fls> [out_dir]\n", argv[0]);
            return 1;
        }
        const char *out_dir = (argc >= 4) ? argv[3] : "./unpacked";
        fls_file_t *fls = fls_open(argv[2]);
        if (!fls) {
            fprintf(stderr, "Failed to open FLS file: %s\n", argv[2]);
            return 1;
        }
        int ret = fls_unpack(fls, out_dir);
        fls_close(fls);
        return ret;
    }

    if (strcmp(cmd, "scan") == 0) {
        transport_scan_ports();
        return 0;
    }

    if (strcmp(cmd, "ram-boot") == 0) {
        const char *port = NULL;
        const char *psi_path = NULL;
        const char *bin_path = NULL;
        uint32_t ram_addr = 0x20000000; /* Default SDRAM address for X-Gold */

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = argv[++i];
            else if (strcmp(argv[i], "--psi") == 0 && i + 1 < argc) psi_path = argv[++i];
            else if (strcmp(argv[i], "--bin") == 0 && i + 1 < argc) bin_path = argv[++i];
            else if (strcmp(argv[i], "--addr") == 0 && i + 1 < argc) ram_addr = (uint32_t)strtoul(argv[++i], NULL, 0);
        }

        if (!port || !bin_path) {
            fprintf(stderr, "[-] Error: -p <port> and --bin <file.bin> are required for ram-boot.\n");
            fprintf(stderr, "    Usage: %s ram-boot -p /dev/ttyACM0 [--psi psi.fls] --bin payload.bin [--addr 0x20000000]\n", argv[0]);
            return 1;
        }

        FILE *bf = fopen(bin_path, "rb");
        if (!bf) {
            fprintf(stderr, "[-] Failed to open binary payload: %s\n", bin_path);
            return 1;
        }
        fseek(bf, 0, SEEK_END);
        long bsz = ftell(bf);
        rewind(bf);
        uint8_t *bdata = (uint8_t *)malloc(bsz);
        if (!bdata || fread(bdata, 1, bsz, bf) != (size_t)bsz) {
            fprintf(stderr, "[-] Failed to read binary payload\n");
            fclose(bf);
            free(bdata);
            return 1;
        }
        fclose(bf);

        transport_t *trans = transport_create_tty();
        if (!trans || transport_open(trans, port, 115200) != 0) {
            fprintf(stderr, "[-] Failed to open port %s\n", port);
            if (trans) transport_destroy(trans);
            free(bdata);
            return 1;
        }

        flasher_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.psi_path = psi_path;
        cfg.baudrate = 921600;

        flasher_session_t *s = flasher_create(trans, &cfg);
        if (flasher_sync(s, 45) != 0) {
            flasher_destroy(s);
            transport_destroy(trans);
            free(bdata);
            return 1;
        }

        if (psi_path) {
            fls_file_t *psi = fls_open(psi_path);
            if (psi) {
                flasher_upload_psi(s, psi);
                fls_close(psi);
            }
        }

        flasher_configure_protocol(s);
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
            {"bsy",       required_argument, 0, 1003},
            {"cds",       required_argument, 0, 1004},
            {"dffs",      required_argument, 0, 1005},
            {"erase-all", no_argument,       0, 1006},
            {"help",      no_argument,       0, 'h'},
            {0, 0, 0, 0}
        };

        optind = 2; /* Skip "flash" command */
        int opt, opt_idx = 0;
        while ((opt = getopt_long(argc, argv, "p:b:h", long_options, &opt_idx)) != -1) {
            switch (opt) {
                case 'p': port = optarg; break;
                case 'b': cfg.baudrate = (uint32_t)atoi(optarg); break;
                case 1001: cfg.psi_path = optarg; break;
                case 1002: cfg.slb_path = optarg; break;
                case 1003: cfg.bsy_path = optarg; break;
                case 1004: cfg.cds_path = optarg; break;
                case 1005: cfg.dffs_path = optarg; break;
                case 1006: cfg.erase_all = true; break;
                case 'h': print_usage(argv[0]); return 0;
                default: return 1;
            }
        }

        if (!port) {
            fprintf(stderr, "[-] Error: Serial port is required (-p /dev/ttyACM0 or -p usb)\n");
            return 1;
        }

        transport_t *trans = NULL;
        if (strcmp(port, "usb") == 0) {
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

        flasher_session_t *session = flasher_create(trans, &cfg);
        int res = flasher_run(session);
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
