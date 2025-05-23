/*
* Copyright (c) 2018 naehrwert
* Copyright (c) 2019-2022 CTCaer
*
* This program is free software; you can redistribute it and/or modify it
* under the terms and conditions of the GNU General Public License,
* version 2, as published by the Free Software Foundation.
*
* This program is distributed in the hope it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <soc/uart.h>
#include <soc/clock.h>
#include <soc/timer.h>
#include <soc/t210.h>

/* UART A, B, C, D and E. */
static const u16 _uart_base_offsets[5] = { 0, 0x40, 0x200, 0x300, 0x400 };

void uart_init(u32 idx, u32 baud, u32 mode)
{
	uart_t *uart = (uart_t *)(UART_BASE + (u32)_uart_base_offsets[idx]);

	// Make sure no data is being sent.
	if (!(mode & (UART_MCR_CTS_EN | UART_MCR_DTR)))
		uart_wait_xfer(idx, UART_TX_IDLE);

	// Set clock.
	bool clk_type = clock_uart_use_src_div(idx, baud);

	// 2 STOP bits for rates > 1M. (Reduced efficiency but less errors on high baudrates).
	u32 uart_lcr_stop = baud > 1000000 ? UART_LCR_STOP : 0;

	// Misc settings.
	u32 div = clk_type ? ((8 * baud + 408000000) / (16 * baud)) : 1; // DIV_ROUND_CLOSEST.
	uart->UART_IER_DLAB = 0; // Disable interrupts.
	uart->UART_LCR      = UART_LCR_DLAB | UART_LCR_WORD_LENGTH_8; // Enable DLAB & set 8n1 mode.
	uart->UART_THR_DLAB = (u8)div;        // Divisor latch LSB.
	uart->UART_IER_DLAB = (u8)(div >> 8); // Divisor latch MSB.

	// Disable DLAB and set STOP bits setting if applicable.
	uart->UART_LCR = uart_lcr_stop | UART_LCR_WORD_LENGTH_8;
	(void)uart->UART_SPR;

	// Enable fifo.
	uart->UART_IIR_FCR = UART_IIR_FCR_EN_FIFO;
	(void)uart->UART_SPR;
	usleep(20);

	// Disable hardware flow control.
	uart->UART_MCR = 0;
	usleep(96);

	// Clear tx/rx fifos.
	uart->UART_IIR_FCR = UART_IIR_FCR_EN_FIFO | UART_IIR_FCR_TX_CLR | UART_IIR_FCR_RX_CLR;

	// Set hardware flow control.
	uart->UART_MCR = mode;

	// Wait 3 symbols for baudrate change.
	usleep(3 * ((baud + 999999) / baud));
	uart_wait_xfer(idx, UART_TX_IDLE | UART_RX_RDYR);
}

void uart_wait_xfer(u32 idx, u32 which)
{
	uart_t *uart = (uart_t *)(UART_BASE + (u32)_uart_base_offsets[idx]);
	if (UART_TX_IDLE & which)
	{
		while (!(uart->UART_LSR & UART_LSR_TMTY))
			;
	}
	if (UART_RX_RDYR & which)
	{
		while (uart->UART_LSR & UART_LSR_RDR)
			(void)uart->UART_THR_DLAB;
	}
}

void uart_send(u32 idx, const u8 *buf, u32 len)
{
	uart_t *uart = (uart_t *)(UART_BASE + (u32)_uart_base_offsets[idx]);

	for (u32 i = 0; i != len; i++)
	{
		while (!(uart->UART_LSR & UART_LSR_THRE))
			;
		uart->UART_THR_DLAB = buf[i];
	}
}

u32 uart_recv(u32 idx, u8 *buf, u32 len)
{
	uart_t *uart = (uart_t *)(UART_BASE + (u32)_uart_base_offsets[idx]);
	bool manual_mode = uart->UART_MCR & UART_MCR_RTS;
	u32 timeout = get_tmr_us() + 250;
	u32 i;

	if (manual_mode)
		uart->UART_MCR &= ~UART_MCR_RTS;

	for (i = 0; ; i++)
	{
		if (len && len <= i)
			break;

		while (!(uart->UART_LSR & UART_LSR_RDR))
			if (timeout < get_tmr_us())
				goto out;

		buf[i] = uart->UART_THR_DLAB;
		timeout = get_tmr_us() + 250;
	}

out:
	if (manual_mode)
		uart->UART_MCR |= UART_MCR_RTS;

	return i;
}

void uart_invert(u32 idx, bool enable, u32 invert_mask)
{
	uart_t *uart = (uart_t *)(UART_BASE + (u32)_uart_base_offsets[idx]);

	if (enable)
		uart->UART_IRDA_CSR |= invert_mask;
	else
		uart->UART_IRDA_CSR &= ~invert_mask;
	(void)uart->UART_SPR;
}

void uart_set_mode(u32 idx, u32 mode)
{
	uart_t *uart = (uart_t *)(UART_BASE + (u32)_uart_base_offsets[idx]);

	uart->UART_MCR = mode;
	(void)uart->UART_SPR;
}

u32 uart_get_IIR(u32 idx)
{
	uart_t *uart = (uart_t *)(UART_BASE + (u32)_uart_base_offsets[idx]);

	u32 iir = uart->UART_IIR_FCR & UART_IIR_INT_MASK;

	if (iir & UART_IIR_NO_INT)
		return 0;
	else
		return ((iir >> 1) + 1); // Return encoded interrupt.
}

void uart_set_IIR(u32 idx)
{
	uart_t *uart = (uart_t *)(UART_BASE + (u32)_uart_base_offsets[idx]);

	uart->UART_IER_DLAB &= ~UART_IER_DLAB_IE_EORD;
	(void)uart->UART_SPR;
	uart->UART_IER_DLAB |= UART_IER_DLAB_IE_EORD;
	(void)uart->UART_SPR;
}

void uart_empty_fifo(u32 idx, u32 which)
{
	uart_t *uart = (uart_t *)(UART_BASE + (u32)_uart_base_offsets[idx]);

	uart->UART_MCR = 0;
	(void)uart->UART_SPR;
	usleep(96);

	uart->UART_IIR_FCR = UART_IIR_FCR_EN_FIFO | which;
	(void)uart->UART_SPR;
	usleep(18);
	u32 tries = 0;

	if (UART_IIR_FCR_TX_CLR & which)
	{
		while (tries < 10 && !(uart->UART_LSR & UART_LSR_TMTY))
		{
			tries++;
			usleep(100);
		}
		tries = 0;
	}

	if (UART_IIR_FCR_RX_CLR & which)
	{
		while (tries < 10 && (uart->UART_LSR & UART_LSR_RDR))
		{
			tries++;
			usleep(100);
		}
	}
}

#ifdef DEBUG_UART_PORT
#include <stdarg.h>
#include <string.h>

#include <utils/sprintf.h>

void uart_printf(const char *fmt, ...)
{
	va_list ap;

	//! NOTE: Anything more and it will hang. Heap usage is out of the question.
	char text[256];

	va_start(ap, fmt);
	s_vprintf(text, fmt, ap);
	va_end(ap);

	uart_send(DEBUG_UART_PORT, (u8 *)text, strlen(text));
}
#endif
