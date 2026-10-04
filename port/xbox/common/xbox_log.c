/*
XBOX_LOG.C

Log lines to COM1 and the screen (xbox_log.h).

The SuperIO chip (an SMSC LPC47M157 on the debug kits; xemu's
-device lpc47m157) is configured through ports 0x2E/0x2F: the enter key
0x55, the logical device number at index 0x07 (4 is serial port 1), its base
address at 0x60/0x61, activation at 0x30, and the exit key 0xAA. xemu's
comes up with both serial ports inactive, so they are configured here at
the usual COM1 base, 0x3F8, as a 16550: 115200 baud, 8 data bits, no parity,
one stop bit, FIFOs on.

Whether the port is there is found from the 16550's scratch register, which
reads back what was written: a retail console, without the chip, reads back
0xFF, and its line status is never polled forever (MAXIMUM_TRANSMIT_POLLS).
*/

#include "xbox_log.h"

#include <hal/debug.h>
#include <stdarg.h>
#include <stdio.h>

#define SUPERIO_CONFIG_PORT 0x2E
#define SUPERIO_DATA_PORT 0x2F
#define SUPERIO_ENTER_KEY 0x55
#define SUPERIO_EXIT_KEY 0xAA
#define SUPERIO_DEVICE_SELECT 0x07
#define SUPERIO_DEVICE_SERIAL_1 0x04
#define SUPERIO_ACTIVATE 0x30
#define SUPERIO_BASE_HIGH 0x60
#define SUPERIO_BASE_LOW 0x61

#define COM1 0x3F8
#define UART_DATA 0         /* divisor low with DLAB */
#define UART_INTERRUPTS 1   /* divisor high with DLAB */
#define UART_FIFO 2
#define UART_LINE_CONTROL 3
#define UART_MODEM_CONTROL 4
#define UART_LINE_STATUS 5
#define UART_SCRATCH 7
#define LINE_STATUS_TRANSMIT_EMPTY 0x20
#define MAXIMUM_TRANSMIT_POLLS 100000

static int serial_present;
static int screen_output;

static inline void port_write(unsigned short port, unsigned char value)
{
	__asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned char port_read(unsigned short port)
{
	unsigned char value;
	__asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static void superio_write(unsigned char index, unsigned char value)
{
	port_write(SUPERIO_CONFIG_PORT, index);
	port_write(SUPERIO_DATA_PORT, value);
}

static void serial_write_character(char character)
{
	int polls;

	for (polls = 0; polls < MAXIMUM_TRANSMIT_POLLS; polls++)
	{
		if (port_read(COM1 + UART_LINE_STATUS) & LINE_STATUS_TRANSMIT_EMPTY)
			break;
	}
	port_write(COM1 + UART_DATA, (unsigned char)character);
}

int xbox_log_initialize(int to_screen)
{
	screen_output = to_screen;

	port_write(SUPERIO_CONFIG_PORT, SUPERIO_ENTER_KEY);
	superio_write(SUPERIO_DEVICE_SELECT, SUPERIO_DEVICE_SERIAL_1);
	superio_write(SUPERIO_BASE_HIGH, COM1 >> 8);
	superio_write(SUPERIO_BASE_LOW, COM1 & 0xFF);
	superio_write(SUPERIO_ACTIVATE, 1);
	port_write(SUPERIO_CONFIG_PORT, SUPERIO_EXIT_KEY);

	port_write(COM1 + UART_SCRATCH, 0x5A);
	serial_present = port_read(COM1 + UART_SCRATCH) == 0x5A;
	if (serial_present)
	{
		port_write(COM1 + UART_INTERRUPTS, 0);
		port_write(COM1 + UART_LINE_CONTROL, 0x80); /* DLAB */
		port_write(COM1 + UART_DATA, 1);            /* 115200 baud */
		port_write(COM1 + UART_INTERRUPTS, 0);
		port_write(COM1 + UART_LINE_CONTROL, 0x03); /* 8N1 */
		port_write(COM1 + UART_FIFO, 0xC7);
		port_write(COM1 + UART_MODEM_CONTROL, 0x03);
	}
	return serial_present;
}

void xbox_log_to_screen(int to_screen)
{
	screen_output = to_screen;
}

void xbox_log(const char *format, ...)
{
	char line[512];
	va_list arguments;
	const char *character;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);

	if (serial_present)
	{
		for (character = line; *character; character++)
		{
			if (*character == '\n')
				serial_write_character('\r');
			serial_write_character(*character);
		}
		serial_write_character('\r');
		serial_write_character('\n');
	}
	if (screen_output)
	{
		debugPrint("%s\n", line);
	}
}

void xbox_log_done(void)
{
	xbox_log("%s", XBOX_LOG_DONE_MARKER);
}
