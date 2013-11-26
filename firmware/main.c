/*
 * Author: Minux
 * Author: Xiangfu Liu <xiangfu@openmobilefree.net>
 * Bitcoin:	1CanaaniJzgps8EV6Sfmpb7T8RutpaeyFn
 *
 * This is free and unencumbered software released into the public domain.
 * For details see the UNLICENSE file at the root of the source tree.
 */

#include <stdbool.h>
#include <stdint.h>

#include "minilibc.h"
#include "system_config.h"
#include "defines.h"
#include "io.h"
#include "intr.h"
#include "uart.h"
#include "miner.h"
#include "sha256.h"
#include "alink.h"
#include "twipwm.h"
#include "protocol.h"
#include "crc.h"

#include "hexdump.c"

#define adjust_fan(value)	write_pwm(value)

struct mm_work mm_work;
struct result result;

static uint8_t g_pkg[AVA2_P_COUNT];
static uint8_t g_act[AVA2_P_COUNT];
static int new_stratum;

void delay(unsigned int ms)
{
	unsigned int i;

	while (ms--) {
		for (i = 0; i < CPU_FREQUENCY / 1000 / 5; i++)
			__asm__ __volatile__("nop");
	}
}

static void error(uint8_t n)
{
	volatile uint32_t *gpio = (uint32_t *)GPIO_BASE;
	uint8_t i = 0;

	while (i < 8) {
		delay(1000);
		if (i++ % 2)
			writel(n << 24, gpio);
		else
			writel(0, gpio);
	}
}

static void encode_pkg(uint8_t *p, int type)
{
	uint16_t crc;

	memset(p, 0, AVA2_P_COUNT);

	p[0] = AVA2_H1;
	p[1] = AVA2_H2;
	p[AVA2_P_COUNT - 2] = AVA2_T1;
	p[AVA2_P_COUNT - 1] = AVA2_T2;

	p[2] = type;
	p[3] = 1;
	p[4] = 1;

	switch(type) {
	case AVA2_P_ACKDETECT:
		p[5 + 0] = 'M';
		p[5 + 1] = 'M';
		memcpy(p + 5 + 2, MM_VERSION, 6);
		break;
	}

	crc = crc16(p + 5, AVA2_P_DATA_LEN);
	p[AVA2_P_COUNT - 4] = crc & 0x00ff;
	p[AVA2_P_COUNT - 3] = (crc & 0xff00) >> 8;

	debug32("Send:\n");
	hexdump(p, AVA2_P_COUNT);
}

static void send_pkg(int type)
{
	encode_pkg(g_act, type);
	uart_nwrite((char *)g_act, AVA2_P_COUNT);
}

static int decode_pkg(uint8_t *p, struct mm_work *mw)
{
	unsigned int expected_crc;
	unsigned int actual_crc;
	int idx, cnt;

	uint8_t *data = p + 5;

	idx = p[3];
	cnt = p[4];

	debug32("Receive: %d/%d\n", idx, cnt);
	hexdump(p, AVA2_P_COUNT);

	expected_crc = (p[AVA2_P_COUNT - 3] & 0xff) |
		((p[AVA2_P_COUNT - 4] & 0xff) << 8);

	actual_crc = crc16(data, AVA2_P_DATA_LEN);
	if(expected_crc != actual_crc) {
		debug32("PKG CRC failed (expected %08x, got %08x)\n",
			expected_crc, actual_crc);
		return 1;
	}

	switch (p[2]) {
	case AVA2_P_DETECT:
		send_pkg(AVA2_P_ACK);
		send_pkg(AVA2_P_ACKDETECT);
		new_stratum = 0;
		break;
	case AVA2_P_STATIC:
		memcpy(&mw->coinbase_len, data, 4);
		memcpy(&mw->nonce2_offset, data + 4, 4);
		memcpy(&mw->nonce2_size, data + 8, 4);
		memcpy(&mw->merkle_offset, data + 12, 4);
		memcpy(&mw->nmerkles, data + 16, 4);
		debug32("P_STATIC:  %d, %d, %d, %d, %d\n",
			mw->coinbase_len,
			mw->nonce2_offset,
			mw->nonce2_size,
			mw->merkle_offset,
			mw->nmerkles);
		break;
	case AVA2_P_JOB_ID:
		break;
	case AVA2_P_COINBASE:
		if (idx == 1)
			memset(mw->coinbase, 0, sizeof(mw->coinbase));
		memcpy(mw->coinbase + (idx - 1) * AVA2_P_DATA_LEN, data, AVA2_P_DATA_LEN);
		if (idx == cnt)
			hexdump(mw->coinbase, mw->coinbase_len);
		break;
	case AVA2_P_MERKLES:
		memcpy(mw->merkles[idx - 1], data, AVA2_P_DATA_LEN);
		hexdump(mw->merkles[idx - 1], 32);
		break;
	case AVA2_P_HEADER:
		memcpy(mw->header + (idx - 1) * AVA2_P_DATA_LEN, data, AVA2_P_DATA_LEN);
		if (idx == cnt) {
			hexdump(mw->header, 128);
			new_stratum = 1;
		}
		break;
	default:
		break;
	}

	return 0;
}

static void get_pkg()
{
	static char heada, headv;
	static char tailo, tailn;
	static int start = 0, count = 2;
	char c;

	while (1) {
		if (uart_read_nonblock()) {
			c = uart_read();

			heada = headv;
			headv = c;
			if (heada == AVA2_H1 && headv == AVA2_H2 && !start) {
				g_pkg[0] = heada;
				g_pkg[1] = headv;
				start = 1;
				count = 2;
				continue;
			}

			if (start)
				g_pkg[count++] = c;

			tailo = tailn;
			tailn = c;
			if (tailo == AVA2_T1 && tailn == AVA2_T2) {
				if (count == AVA2_P_COUNT && (!decode_pkg(g_pkg, &mm_work))) {
					;
				} else {
					debug32("E: package broken: %d\n", count);
					send_pkg(AVA2_P_NAK);
				}
				start = 0;
				count = 2;
			}
			if (count >= AVA2_P_COUNT) {
				debug32("E: package broken: %d\n", count);
				send_pkg(AVA2_P_NAK);
			}
		} else
			break;
	}

}

static void submit_result(struct result *r)
{
	hexdump((uint8_t *)(r), 20);
}

static void read_result()
{
	while(!alink_rxbuf_empty()) {
		debug32("Found nonce\n");
		alink_buf_status();
		alink_read_result(&result);
		submit_result(&result);
	}
}


int main(int argv, char **argc) {
	delay(50);		/* Delay 50ms, wait for alink ready */

	struct work work;
	int i;

	irq_setmask(0);
	irq_enable(1);

	uart_init();

	debug32("%s\n", MM_VERSION);

	alink_init(0xff);
	adjust_fan(0x0f);

	new_stratum = 0;
	while (1) {
		get_pkg();
		if (new_stratum)
			break;
	}

	i = 4;
	while (i--) {
		get_pkg();
		if (!new_stratum)
			continue;

		miner_init_work(&mm_work, &work);
		miner_gen_work(&mm_work, &work);
		alink_send_work(&work);
		read_result();
	}

	error(0xf);
	return 0;
}

/* vim: set ts=4 sw=4 fdm=marker : */
