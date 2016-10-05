#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include "m68k_core.h"
#include "jaguar.h"
#include "util.h"

//BIOS Area Memory map
// 10 00 00 - 10 04 00 : Video mode/ Memory control registers
// 10 04 00 - 10 08 00 : CLUT
// 10 08 00 - 10 10 00 : Line buffer A
// 10 10 00 - 10 18 00 : Line buffer B
// 10 18 00 - 10 20 00 : Write Line buffer
// 10 21 00 - 10 30 00 : GPU/blitter registers
// 10 30 00 - 10 40 00 : GPU Local RAM (mirrored every 1K?)
// 11 00 00 - 11 00 40 : Timer/Clock registers
// 11 40 00 - 11 40 04 : Joystick Interface
// 11 A1 00 - 11 A1 52 : DSP/DAC/I2S Registers
// 11 B0 00 - 11 D0 00 : DSP Local RAM (8KB)
// 11 D0 00 - 11 E0 00 : Wave table ROM

int headless = 1;


void rom0_write_16(uint32_t address, jaguar_context *system, uint16_t value)
{
	//TODO: Use write_latch and write_pending to turn two 16-bit writes into a 32-bit one
	//Documentation heavily suggests that writes to most registers should be 32-bits wide
	if (address < 0x100000 || address >= 0x120000) {
		//Boot ROM
		fprintf(stderr, "Invalid write to Boot ROM - %X:%X\n", address, value);
		return;
	}
	if (address < 0x103000) {
		if (address < 0x101000) {
			if (address < 0x100400) {
				//Video mode / Memory control registers
				fprintf(stderr, "Unhanelde write to video mode/memory control registers - %X:%X", address, value);
			} else if (address < 0x100800) {
				//CLUT
				address = address >> 1 & 255;
				system->clut[address] = value;
			} else {
				//Line buffer A
				address = address >> 1 & 0x3FF;
				if (address < LINEBUFFER_WORDS) {
					system->line_buffer_a[address] = value;
				}
			}
		} else if (address < 0x101800) {
			//Line buffer B
			address = address >> 1 & 0x3FF;
			if (address < LINEBUFFER_WORDS) {
				system->line_buffer_b[address] = value;
			}
		} else if (address < 0x102100) {
			//Write Line Buffer
			address = address >> 1 & 0x3FF;
			if (address < LINEBUFFER_WORDS) {
				system->write_line_buffer[address] = value;
			}
		} else {
			//GPU/Blitter registers
			fprintf(stderr, "Unhandled write to GPU/Blitter registers %X: %X\n", address, value);
		}
	} else if (address < 0x11A100) {
		if (address < 0x110000) {
			//GPU Local RAM
			uint32_t offset = address >> 2 & (GPU_RAM_BYTES / sizeof(uint32_t) - 1);
			uint32_t value32 = value;
			if (address & 2) {
				system->gpu_local[offset] &= 0xFFFF0000;
			} else {
				system->gpu_local[offset] &= 0x0000FFFF;
				value32 = value32 << 16;
			}
			system->gpu_local[offset] |= value32;
		} else if (address < 0x114000) {
			//timer clock registers
			fprintf(stderr, "Unhanelde write to timer/clock registers - %X:%X", address, value);
		} else {
			//joystick interface
			fprintf(stderr, "Unhanelde write to joystick interface - %X:%X", address, value);
		}
	} else if (address < 0x11B000) {
		//DSP/DAC/I2S Registers
		fprintf(stderr, "Unhanelde write to DSP/DAC/I2S registers - %X:%X", address, value);
	} else if (address < 0x11D000) {
		//DSP local RAM
		uint32_t offset = address >> 2 & (DSP_RAM_BYTES / sizeof(uint32_t) - 1);
		uint32_t value32 = value;
		if (address & 2) {
			system->dsp_local[offset] &= 0xFFFF0000;
		} else {
			system->dsp_local[offset] &= 0x0000FFFF;
			value32 = value32 << 16;
		}
		system->gpu_local[offset] |= value32;
	} else {
		//Wave table ROM
		fprintf(stderr, "Invalid write to wave table ROM - %X:%X\n", address, value);
	}
}

uint16_t rom0_read_16(uint32_t address, jaguar_context *system)
{
	if (address < 0x100000 || address >= 0x120000) {
		//Boot ROM
		address = address >> 1 & ((system->bios_size >> 1) - 1);
		return system->bios[address];
	}
	if (address < 0x103000) {
		if (address < 0x101000) {
			if (address < 0x100400) {
				//Video mode / Memory control registers
				fprintf(stderr, "Unhandled read from video mode/memory control registers - %X", address);
			} else if (address < 0x100800) {
				//CLUT
				address = address >> 1 & 255;
				return system->clut[address];
			} else {
				//Line buffer A
				address = address >> 1 & 0x3FF;
				if (address < LINEBUFFER_WORDS) {
					return system->line_buffer_a[address];
				}
			}
		} else if (address < 0x101800) {
			//Line buffer B
			address = address >> 1 & 0x3FF;
			if (address < LINEBUFFER_WORDS) {
				return system->line_buffer_b[address];
			}
		} else if (address < 0x102100) {
			//Write Line Buffer
			address = address >> 1 & 0x3FF;
			if (address < LINEBUFFER_WORDS) {
				return system->write_line_buffer[address];
			}
		} else {
			//GPU/Blitter registers
			fprintf(stderr, "Unhandled read from GPU/Blitter registers %X\n", address);
		}
	} else if (address < 0x11A100) {
		if (address < 0x110000) {
			//GPU Local RAM
			uint32_t offset = address >> 2 & (GPU_RAM_BYTES / sizeof(uint32_t) - 1);
			if (address & 2) {
				return system->gpu_local[offset];
			} else {
				return system->gpu_local[offset] >> 16;
			}
		} else if (address < 0x114000) {
			//timer clock registers
			fprintf(stderr, "Unhandled read from timer/clock registers - %X", address);
		} else {
			//joystick interface
			fprintf(stderr, "Unhandled read from joystick interface - %X", address);
		}
	} else if (address < 0x11B000) {
		//DSP/DAC/I2S Registers
		fprintf(stderr, "Unhandled read from DSP/DAC/I2S registers - %X", address);
	} else if (address < 0x11D000) {
		//DSP local RAM
		uint32_t offset = address >> 2 & (DSP_RAM_BYTES / sizeof(uint32_t) - 1);
		if (address & 2) {
				return system->dsp_local[offset];
			} else {
				return system->dsp_local[offset] >> 16;
			}
	} else {
		//Wave table ROM
		fprintf(stderr, "Unhandled read from wave table ROM - %X\n", address);
	}
	return 0xFFFF;
}


void *rom0_write_m68k(uint32_t address, void *context, uint16_t value)
{
	rom0_write_16(address, ((m68k_context *)context)->system, value);
	return context;
}

uint16_t rom0_read_m68k(uint32_t address, void *context)
{
	return rom0_read_16(address, ((m68k_context *)context)->system);
}

void *rom0_write_m68k_b(uint32_t address, void *context, uint8_t value)
{
	//seems unlikely these areas support byte access
	uint16_t value16 = value;
	value16 |= value16 << 8;
	rom0_write_16(address, ((m68k_context *)context)->system, value16);
	return context;
}

uint8_t rom0_read_m68k_b(uint32_t address, void *context)
{
	uint16_t value = rom0_read_16(address, ((m68k_context *)context)->system);
	if (address & 1) {
		return value;
	}
	return value >> 8;
}

m68k_context * sync_components(m68k_context * context, uint32_t address)
{
	if (context->current_cycle > 0x10000000) {
		context->current_cycle -= 0x10000000;
	}
	return context;
}

jaguar_context *init_jaguar(uint16_t *bios, uint32_t bios_size, uint16_t *cart, uint32_t cart_size)
{
	jaguar_context *system = calloc(1, sizeof(jaguar_context));
	system->bios = bios;
	system->bios_size = bios_size;
	system->cart = cart;
	system->cart_size = cart_size;

	memmap_chunk jag_m68k_map[8];
	for (uint32_t start = 0, index=0; index < 8; index++, start += 0x200000)
	{
		jag_m68k_map[index].start = start;
		jag_m68k_map[index].end = start + 0x200000;
		jag_m68k_map[index].mask = index ? 0x1FFFFF : 0xFFFFFF;
		jag_m68k_map[index].aux_mask = bios_size - 1;
		jag_m68k_map[index].ptr_index = index;
		jag_m68k_map[index].flags = MMAP_READ | MMAP_WRITE | MMAP_PTR_IDX | MMAP_FUNC_NULL | MMAP_AUX_BUFF;
		jag_m68k_map[index].buffer = bios;
		jag_m68k_map[index].read_16 = rom0_read_m68k;
		jag_m68k_map[index].read_8 = rom0_read_m68k_b;
		jag_m68k_map[index].write_16 = rom0_write_m68k;
		jag_m68k_map[index].write_8 = rom0_write_m68k_b;
	}
	m68k_options *opts = malloc(sizeof(m68k_options));
	init_m68k_opts(opts, jag_m68k_map, 8, 2);
	system->m68k = init_68k_context(opts);
	system->m68k->system = system;
	return system;
}

//modified copy of the version in blastem.c
uint16_t *load_rom(char * filename, uint32_t *size)
{
	FILE * f = fopen(filename, "rb");
	if (!f) {
		return 0;
	}
	fseek(f, 0, SEEK_END);
	long filesize = ftell(f);
	fseek(f, 0, SEEK_SET);
	
	*size = nearest_pow2(filesize);
	uint16_t *cart = malloc(*size);
	if (filesize != fread(cart, 1, filesize, f)) {
		fatal_error("Error reading from %s\n", filename);
	}
	while (filesize < *size)
	{
		cart[filesize / 2] = 0xFFFF;
		filesize += 2;
	}
	fclose(f);
	return cart;
}

//temporary main function until I clean up blastem.c
int main(int argc, char **argv)
{
	if (argc < 3) {
		fputs("Usage: blastjag BIOS ROM\n", stderr);
		return 1;
	}
	uint32_t bios_size;
	uint16_t *bios = load_rom(argv[1], &bios_size);
	if (!bios_size) {
		fatal_error("Failed to read BIOS from %s\n", argv[1]);
	}
	uint32_t cart_size;
	uint16_t *cart = load_rom(argv[2], &cart_size);
	if (!bios_size) {
		fatal_error("Failed to read cart from %s\n", argv[2]);
	}
	jaguar_context *system = init_jaguar(bios, bios_size, cart, cart_size);
	m68k_reset(system->m68k);
	return 0;
}
