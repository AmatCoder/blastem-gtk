/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef GENESIS_H_
#define GENESIS_H_

#include <stdint.h>
#include "system.h"
#include "m68k_core.h"
#include "z80_to_x86.h"
#include "ym2612.h"
#include "vdp.h"
#include "psg.h"
#include "io.h"
#include "romdb.h"
#include "arena.h"
#include "i2c.h"

typedef struct genesis_context genesis_context;

struct genesis_context {
	system_header   header;
	m68k_context    *m68k;
	z80_context     *z80;
	vdp_context     *vdp;
	ym2612_context  *ym;
	psg_context     *psg;
	uint16_t        *cart;
	uint16_t        *lock_on;
	uint16_t        *work_ram;
	uint8_t         *zram;
	void            *extra;
	uint8_t         *save_storage;
	void            *mapper_temp;
	eeprom_map      *eeprom_map;
	uint32_t        num_eeprom;
	uint32_t        save_size;
	uint32_t        save_ram_mask;
	uint32_t        master_clock; //Current master clock value
	uint32_t        normal_clock; //Normal master clock (used to restore master clock after turbo mode)
	uint32_t        frame_end;
	uint32_t        max_cycles;
	uint32_t        int_latency_prev1;
	uint32_t        int_latency_prev2;
	uint8_t         bank_regs[8];
	uint16_t        mapper_start_index;
	uint8_t         mapper_type;
	uint8_t         save_type;
	sega_io         io;
	uint8_t         version_reg;
	uint8_t         bus_busy;
	uint8_t         reset_requested;
	eeprom_state    eeprom;
	nor_state       nor;
};

#define RAM_WORDS 32 * 1024
#define Z80_RAM_BYTES 8 * 1024

uint16_t read_dma_value(uint32_t address);
m68k_context * sync_components(m68k_context *context, uint32_t address);
genesis_context *alloc_config_genesis(void *rom, uint32_t rom_size, void *lock_on, uint32_t lock_on_size, uint32_t system_opts, uint8_t force_region, rom_info *info_out);
void genesis_serialize(genesis_context *gen, serialize_buffer *buf, uint32_t m68k_pc);
void genesis_deserialize(deserialize_buffer *buf, genesis_context *gen);

#endif //GENESIS_H_

