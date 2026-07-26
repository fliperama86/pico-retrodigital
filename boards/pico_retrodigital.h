/*
 * SPDX-License-Identifier: BSD-3-Clause
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

#ifndef _BOARDS_PICO_RETRODIGITAL_H
#define _BOARDS_PICO_RETRODIGITAL_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection.
#define PICO_RETRODIGITAL

// The PCB uses the 48-GPIO RP2354B variant.
#define PICO_RP2350A 0

// RP2354B contains a stacked 2 MiB Winbond W25Q16JV flash die. The same
// W25Q080-compatible boot2 sequence is used by Raspberry Pi's W25Q16 boards.
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

    pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (2 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

        pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
