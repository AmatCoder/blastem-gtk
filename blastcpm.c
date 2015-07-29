#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <sys/select.h>

#include "z80_to_x86.h"
#include "util.h"

uint8_t ram[64 * 1024];

#define START_OFF 0x100
#define OS_START 0xE400
#define OS_RESET 0xE403
int headless = 1;

void z80_next_int_pulse(z80_context * context)
{
	context->int_pulse_start = context->int_pulse_end = CYCLE_NEVER;
}

void render_errorbox(char *title, char *message)
{
}

void render_infobox(char *title, char *message)
{
}

void *console_write(uint32_t address, void *context, uint8_t value)
{
	putchar(value);
	return context;
}

uint8_t console_read(uint32_t address, void *context)
{
	return getchar();
}

void *console_flush_write(uint32_t address, void *context, uint8_t value)
{
	fflush(stdout);
	return context;
}

uint8_t console_status_read(uint32_t address, void *context)
{
	fd_set read_fds;
	FD_ZERO(&read_fds);
	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	FD_SET(fileno(stdin), &read_fds);
	return select(fileno(stdin)+1, &read_fds, NULL, NULL, &timeout) > 0; 
}

void *exit_write(uint32_t address, void *context, uint8_t value)
{
	exit(0);
	return context;
}

const memmap_chunk z80_map[] = {
	{ 0x0000, 0x10000,  0xFFFF, 0, MMAP_READ | MMAP_WRITE | MMAP_CODE, ram, NULL, NULL, NULL, NULL},
};

const memmap_chunk io_map[] = {
	{ 0x0, 0x1, 0xFFFF, 0, 0, NULL, NULL, NULL, console_read, console_write},
	{ 0x1, 0x2, 0xFFFF, 0, 0, NULL, NULL, NULL, console_status_read, console_flush_write},
	{ 0x2, 0x3, 0xFFFF, 0, 0, NULL, NULL, NULL, NULL, exit_write},
};

int main(int argc, char **argv)
{
	FILE *f = fopen("fake_cpm.bin", "rb");
	long fsize = file_size(f);
	if (fsize > sizeof(ram) - OS_START) {
		fsize = sizeof(ram) - OS_START;
	}
	if (fread(ram + OS_START, 1, fsize, f) != fsize) {
		fprintf(stderr, "Error reading from fake_cpm.bin\n");
		exit(1);
	}
	f = fopen(argv[1], "rb");
	fsize = file_size(f);
	if (fsize > OS_START - START_OFF) {
		fsize = OS_START - START_OFF;
	}
	if (fread(ram + START_OFF, 1, fsize, f) != fsize) {
		fprintf(stderr, "Error reading from file %s\n", argv[1]);
		exit(1);
	}
	fclose(f);
	ram[0] = 0xC3;
	ram[1] = OS_RESET & 0xFF;
	ram[2] = OS_RESET >> 8;
	ram[5] = 0xC3;
	ram[6] = OS_START & 0xFF;
	ram[7] = OS_START >> 8;
	
	z80_options opts;
	z80_context context;
	init_z80_opts(&opts, z80_map, 1, io_map, 3, 1);
	init_z80_context(&context, &opts);
	for(;;)
	{
		z80_run(&context, 1000000);
		context.current_cycle = 0;
	}
	return 0;
}