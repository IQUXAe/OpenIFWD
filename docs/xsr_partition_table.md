# Samsung GT-S5222 (Star 3 Duos) OneNAND Flash Layout (XSR Architecture)

## Overview
- **Flash Architecture:** Samsung OneNAND (KFM1G16Q2C / MCP)
- **Total Capacity:** 128 MB (1,024 physical erase blocks of 128 KB each)
- **Block Size:** 128 KB (64 pages × 2,048 bytes per page)
- **Partition Table Location:** Block 1022, byte offset `0x400` (physical flash address `0x07FC0400`)
- **Partition Magic:** `XSRPART\0` (`0x58 0x53 0x52 0x50 0x41 0x52 0x54 0x00`)
- **Format Version:** `0x00001000` (XSR v1.0)
- **Total Partitions:** 8

---

## Partition Table (Decoded from Block 1022 `XSRPART`)

| Part # | Type | Flags | Start Block | Block Count | Size (KB) | Size (MB) | Physical Address Range | Description & Contents |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| **0** | `0x00` | `0x01` | **0** | 1 | 128 KB | 0.125 MB | `0x00000000` – `0x0001FFFF` | **BSR / IPL**: Boot Sector Recovery / Initial Program Loader (Reset vector & HW init). |
| **1** | `0x20` | `0x01` | **1** | 1 | 128 KB | 0.125 MB | `0x00020000` – `0x0003FFFF` | **Boot Config**: Second-stage boot configuration, headers, and security hashes. |
| **2** | `0x10` | `0x01` | **2** | 8 | 1,024 KB | 1.000 MB | `0x00040000` – `0x0013FFFF` | **EBL / Loaders**: Emergency Boot Loader and Baseband DSP patches. |
| **3** | `0x01` | `0x01` | **10** | 322 | 41,216 KB | 40.250 MB | `0x00140000` – `0x0297FFFF` | **CODE**: Main firmware image (Nucleus RTOS kernel, Baseband stack, TouchWiz MMI). |
| **4** | `0x15` | `0x01` | **332** | 8 | 1,024 KB | 1.000 MB | `0x02980000` – `0x02A7FFFF` | **CSC**: Customer Specific Customization (regional carrier configs, APNs, CSC profiles). |
| **5** | `0x23` | `0x01` | **340** | 638 | 81,664 KB | 79.750 MB | `0x02A80000` – `0x07A3FFFF` | **IFS / RFS**: Internal / Robust File System (`XSR2` magic). Media, user data, contacts, SQLite DBs. |
| **6** | `0x24` | `0x01` | **978** | 8 | 1,024 KB | 1.000 MB | `0x07A40000` – `0x07B3FFFF` | **DYN_EEP**: Dynamic EEPROM (NVRAM runtime config, network counters, call timers). |
| **7** | `0x25` | `0x01` | **986** | 12 | 1,536 KB | 1.500 MB | `0x07B40000` – `0x07CBFFFF` | **STATIC_EEP**: Factory calibration data, RF PA calibration tables, Product Code (`GT-S5222UWASER`), IMEI slots. |

---

## Reserved & Hardware Maintenance Regions

| Block Range | Count | Size | Physical Address Range | Description |
| :---: | :---: | :---: | :---: | :--- |
| **998 – 1021** | 24 | 3,072 KB (3.0 MB) | `0x07CC0000` – `0x07F7FFFF` | **Wear-Leveling Pool & Security Patches**: XSR replacement block reserve, SIM-Lock tokens, and `ULOCKPCH` (Block 1000). |
| **1022 – 1023** | 2 | 256 KB (0.25 MB) | `0x07F80000` – `0x07FFFFFF` | **XSR Metadata & BBT**: Partition table `XSRPART` (Block 1022 @ `+0x400`), block lock data `LOCKPCHD`, and factory Bad Block Table (BBT). |

---

## Raw Binary Entry Reference (16 bytes per partition)
Format: `uint32_t type`, `uint32_t flags`, `uint32_t start_block`, `uint32_t block_count`
- Part 0: `00 00 00 00  01 00 00 00  00 00 00 00  01 00 00 00` (Blk 0, cnt 1)
- Part 1: `20 00 00 00  01 00 00 00  01 00 00 00  01 00 00 00` (Blk 1, cnt 1)
- Part 2: `10 00 00 00  01 00 00 00  02 00 00 00  08 00 00 00` (Blk 2, cnt 8)
- Part 3: `01 00 00 00  01 00 00 00  0a 00 00 00  42 01 00 00` (Blk 10, cnt 322)
- Part 4: `15 00 00 00  01 00 00 00  4c 01 00 00  08 00 00 00` (Blk 332, cnt 8)
- Part 5: `23 00 00 00  01 00 00 00  54 01 00 00  7e 02 00 00` (Blk 340, cnt 638)
- Part 6: `24 00 00 00  01 00 00 00  d2 03 00 00  08 00 00 00` (Blk 978, cnt 8)
- Part 7: `25 00 00 00  01 00 00 00  da 03 00 00  0c 00 00 00` (Blk 986, cnt 12)
