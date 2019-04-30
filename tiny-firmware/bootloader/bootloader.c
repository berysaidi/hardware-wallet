/*
 * This file is part of the Skycoin project, https://skycoin.net/
 *
 * Copyright (C) 2014 Pavol Rusnak <stick@satoshilabs.com>
 * Copyright (C) 2018-2019 Skycoin Project
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/cm3/scb.h>

#include "bootloader.h"
#include "buttons.h"
#include "setup.h"
#include "usb.h"
#include "oled.h"
#include "util.h"
#include "signatures.h"
#include "layout.h"
#include "serialno.h"
#include "rng.h"


void layoutFirmwareHash(const uint8_t *hash)
{
	char str[4][17];
	for (int i = 0; i < 4; i++) {
		data2hex(hash + i * 8, 8, str[i]);
	}
	layoutDialog(&bmp_icon_question, "Abort", "Continue", "Compare fingerprints", str[0], str[1], str[2], str[3], NULL, NULL);
}

#if SIGNATURE_DEBUG
void layout32bits(const uint8_t *buffer, const char* message)
{
	char str[4][17];
	for (int i = 0; i < 4; i++) {
		data2hex(buffer + i * 8, 8, str[i]);
	}
	layoutDialog(&bmp_icon_question, "Abort", "Continue", message, str[0], str[1], str[2], str[3], NULL, NULL);
	do {
		delay(100000);
		buttonUpdate();
	} while (!button.YesUp && !button.NoUp);
}
#endif

void show_halt(void)
{
	layoutDialog(&bmp_icon_error, NULL, NULL, NULL, "Unofficial firmware", "aborted.", NULL, "Unplug your Skywallet", "contact our support.", NULL);
	shutdown();
}

void show_unofficial_warning(const uint8_t *hash)
{
	layoutDialog(&bmp_icon_warning, "Abort", "I'll take the risk", NULL, "WARNING!", NULL, "Unofficial firmware", "detected.", NULL, NULL);

	do {
		delay(100000);
		buttonUpdate();
	} while (!button.YesUp && !button.NoUp);

	if (button.NoUp) {
		show_halt(); // no button was pressed -> halt
	}

	layoutFirmwareHash(hash);

	do {
		delay(100000);
		buttonUpdate();
	} while (!button.YesUp && !button.NoUp);

	if (button.NoUp) {
		show_halt(); // no button was pressed -> halt
	}

	// everything is OK, user pressed 2x Continue -> continue program
}

void __attribute__((noreturn)) load_app(int signed_firmware)
{
	(void)signed_firmware;

	// zero out SRAM
	memset_reg(_ram_start, _ram_end, 0);

	load_vector_table((const vector_table_t *) FLASH_APP_START);
}

bool firmware_present(void)
{
#ifndef APPVER
	if (memcmp((const void *)FLASH_META_MAGIC, "SKY1", 4)) { // magic does not match
		return false;
	}
	if (*((const uint32_t *)FLASH_META_CODELEN) < 4096) { // firmware reports smaller size than 4kB
		return false;
	}
	if (*((const uint32_t *)FLASH_META_CODELEN) > FLASH_TOTAL_SIZE - (FLASH_APP_START - FLASH_ORIGIN)) { // firmware reports bigger size than flash size
		return false;
	}
#endif
	return true;
}

static inline bool bit_status_in_byte(uint8_t data, uint8_t bit_pos) {
	return (data & (uint8_t)(1 << bit_pos)) != 0;
}

static inline void set_bit_in_byte(uint8_t *data, bool val, uint8_t bit_pos) {
	uint8_t mask = (uint8_t)(1 << bit_pos);
	if (val) {
		(*data) |= mask;
	} else {
		(*data) &= (mask ^ 255);
	}
}

static inline void reverse_byte(uint8_t *data) {
	uint8_t tmp_data = *data;
	for (uint8_t i = 0; i < 8; ++i) {
		set_bit_in_byte(data, bit_status_in_byte(tmp_data, i), 7 - i);
	}
}

static inline BITMAP inver_bitmap(BITMAP bm, uint8_t *data) {
	size_t data_len = (bm.width * bm.height) / 8;
	memcpy(data, bm.data, data_len);
	size_t data_mid_len = data_len/2;
	for (size_t i = 0; i < data_mid_len; ++i) {
		reverse_byte(&data[i]);
		reverse_byte(&data[data_len - 1 - i]);
		// NOTE: swap bytes in `i` with `data_len - 1 - i`
		uint8_t byte = 0;
		memcpy(&byte, &data[i], sizeof(byte));
		memcpy(&data[i], &data[data_len - 1 - i], sizeof(byte));
		memcpy(&data[data_len - 1 - i], &byte, sizeof(byte));
	}
	if (data_len % 2) {
		reverse_byte(&data[data_mid_len + 1]);
	}
	BITMAP inverted = {
		.width = bm.width,
		.height = bm.height,
		.data = data
	};
	return inverted;
}

extern uint8_t rdp_level;
void bootloader_loop(void)
{
	oledClear();
	if (rdp_level != 2) {
		// NOTE 48*64 is the size of the bmp_logo64 buffer.
		uint8_t bmp_logo64_data_inverted[48*64] = {0};
		BITMAP bmp_logo64_inverted =
				inver_bitmap(bmp_logo64, bmp_logo64_data_inverted);
		oledDrawBitmap(0, 0, &bmp_logo64_inverted);
	} else {
		oledDrawBitmap(0, 0, &bmp_logo64);
	}
	if (firmware_present()) {
		oledDrawString(52, 0, "SKYCOIN", FONT_STANDARD);
		// NOTE(): *2 due to the hex formad and +1 because of the trailing NUL char
		static char serial[SERIAL_NUMBER_SIZE*2 + 1];
		fill_serialno_fixed(serial);
		oledDrawString(52, 20, "Serial No.", FONT_STANDARD);
		oledDrawString(52, 40, serial + 12, FONT_STANDARD); // second part of serial
		serial[12] = 0;
		oledDrawString(52, 30, serial, FONT_STANDARD);      // first part of serial
		oledDrawStringRight(OLED_WIDTH - 1, OLED_HEIGHT - 8, "Loader " VERSTR(VERSION_MAJOR) "." VERSTR(VERSION_MINOR) "." VERSTR(VERSION_PATCH), FONT_STANDARD);
	} else {
		oledDrawString(52, 10, "Welcome!", FONT_STANDARD);
		oledDrawString(52, 30, "Please visit", FONT_STANDARD);
		oledDrawString(52, 50, "skycoin.net", FONT_STANDARD);
	}
	oledRefresh();

	usbLoop(firmware_present());
}

int main(void)
{
#ifndef APPVER
	setup();
#endif
	__stack_chk_guard = random32(); // this supports compiler provided unpredictable stack protection checks
#ifndef APPVER
	set_up_rdp_level();
	memory_protect();
	oledInit();
#endif

#ifndef APPVER
	// at least one button is unpressed
	uint16_t state = gpio_port_read(BTN_PORT);
	int unpressed = ((state & BTN_PIN_YES) == BTN_PIN_YES || (state & BTN_PIN_NO) == BTN_PIN_NO);

	if (firmware_present() && unpressed) {

		oledClear();
		oledDrawBitmap(0, 0, &bmp_logo64);
		oledRefresh();

		uint8_t hash[32];
		int signed_firmware = signatures_ok(hash);
		if (SIG_OK != signed_firmware) {
			show_unofficial_warning(hash);
		}

		delay(100000);

		load_app(signed_firmware);
	}
#endif

	bootloader_loop();

	return 0;
}
