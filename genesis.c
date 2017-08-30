/*
 Copyright 2013-2016 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "genesis.h"
#include "blastem.h"
#include "nor.h"
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <string.h>
#include "render.h"
#include "gst.h"
#include "util.h"
#include "debug.h"
#include "gdb_remote.h"
#define MCLKS_NTSC 53693175
#define MCLKS_PAL  53203395

uint32_t MCLKS_PER_68K;
#define MCLKS_PER_YM  7
#define MCLKS_PER_Z80 15
#define MCLKS_PER_PSG (MCLKS_PER_Z80*16)
#define Z80_INT_PULSE_MCLKS 2573 //measured value is ~171.5 Z80 clocks
#define DEFAULT_SYNC_INTERVAL MCLKS_LINE
#define DEFAULT_LOWPASS_CUTOFF 3390

//TODO: Figure out the exact value for this
#define LINES_NTSC 262
#define LINES_PAL 312

#define MAX_SOUND_CYCLES 100000	

void genesis_serialize(genesis_context *gen, serialize_buffer *buf, uint32_t m68k_pc)
{
	start_section(buf, SECTION_68000);
	m68k_serialize(gen->m68k, m68k_pc, buf);
	end_section(buf);
	
	start_section(buf, SECTION_Z80);
	z80_serialize(gen->z80, buf);
	end_section(buf);
	
	start_section(buf, SECTION_VDP);
	vdp_serialize(gen->vdp, buf);
	end_section(buf);
	
	start_section(buf, SECTION_YM2612);
	ym_serialize(gen->ym, buf);
	end_section(buf);
	
	start_section(buf, SECTION_PSG);
	psg_serialize(gen->psg, buf);
	end_section(buf);
	
	start_section(buf, SECTION_GEN_BUS_ARBITER);
	save_int8(buf, gen->z80->reset);
	save_int8(buf, gen->z80->busreq);
	save_int16(buf, gen->z80->bank_reg);
	end_section(buf);
	
	start_section(buf, SECTION_SEGA_IO_1);
	io_serialize(gen->io.ports, buf);
	end_section(buf);
	
	start_section(buf, SECTION_SEGA_IO_2);
	io_serialize(gen->io.ports + 1, buf);
	end_section(buf);
	
	start_section(buf, SECTION_SEGA_IO_EXT);
	io_serialize(gen->io.ports + 2, buf);
	end_section(buf);
	
	start_section(buf, SECTION_MAIN_RAM);
	save_int8(buf, RAM_WORDS * 2 / 1024);
	save_buffer16(buf, gen->work_ram, RAM_WORDS);
	end_section(buf);
	
	start_section(buf, SECTION_SOUND_RAM);
	save_int8(buf, Z80_RAM_BYTES / 1024);
	save_buffer8(buf, gen->zram, Z80_RAM_BYTES);
	end_section(buf);
	
	cart_serialize(&gen->header, buf);
}

static void ram_deserialize(deserialize_buffer *buf, void *vgen)
{
	genesis_context *gen = vgen;
	uint32_t ram_size = load_int8(buf) * 1024 / 2;
	if (ram_size > RAM_WORDS) {
		fatal_error("State has a RAM size of %d bytes", ram_size * 2);
	}
	load_buffer16(buf, gen->work_ram, ram_size);
	m68k_invalidate_code_range(gen->m68k, 0xE00000, 0x1000000);
}

static void zram_deserialize(deserialize_buffer *buf, void *vgen)
{
	genesis_context *gen = vgen;
	uint32_t ram_size = load_int8(buf) * 1024;
	if (ram_size > Z80_RAM_BYTES) {
		fatal_error("State has a Z80 RAM size of %d bytes", ram_size);
	}
	load_buffer8(buf, gen->zram, ram_size);
	z80_invalidate_code_range(gen->z80, 0, 0x4000);
}

static void update_z80_bank_pointer(genesis_context *gen)
{
	if (gen->z80->bank_reg < 0x100) {
		gen->z80->mem_pointers[1] = get_native_pointer(gen->z80->bank_reg << 15, (void **)gen->m68k->mem_pointers, &gen->m68k->options->gen);
	} else {
		gen->z80->mem_pointers[1] = NULL;
	}
}

static void bus_arbiter_deserialize(deserialize_buffer *buf, void *vgen)
{
	genesis_context *gen = vgen;
	gen->z80->reset = load_int8(buf);
	gen->z80->busreq = load_int8(buf);
	gen->z80->bank_reg = load_int16(buf) & 0x1FF;
}

void genesis_deserialize(deserialize_buffer *buf, genesis_context *gen)
{
	register_section_handler(buf, (section_handler){.fun = m68k_deserialize, .data = gen->m68k}, SECTION_68000);
	register_section_handler(buf, (section_handler){.fun = z80_deserialize, .data = gen->z80}, SECTION_Z80);
	register_section_handler(buf, (section_handler){.fun = vdp_deserialize, .data = gen->vdp}, SECTION_VDP);
	register_section_handler(buf, (section_handler){.fun = ym_deserialize, .data = gen->ym}, SECTION_YM2612);
	register_section_handler(buf, (section_handler){.fun = psg_deserialize, .data = gen->psg}, SECTION_PSG);
	register_section_handler(buf, (section_handler){.fun = bus_arbiter_deserialize, .data = gen}, SECTION_GEN_BUS_ARBITER);
	register_section_handler(buf, (section_handler){.fun = io_deserialize, .data = gen->io.ports}, SECTION_SEGA_IO_1);
	register_section_handler(buf, (section_handler){.fun = io_deserialize, .data = gen->io.ports + 1}, SECTION_SEGA_IO_2);
	register_section_handler(buf, (section_handler){.fun = io_deserialize, .data = gen->io.ports + 2}, SECTION_SEGA_IO_EXT);
	register_section_handler(buf, (section_handler){.fun = ram_deserialize, .data = gen}, SECTION_MAIN_RAM);
	register_section_handler(buf, (section_handler){.fun = zram_deserialize, .data = gen}, SECTION_SOUND_RAM);
	register_section_handler(buf, (section_handler){.fun = cart_deserialize, .data = gen}, SECTION_MAPPER);
	while (buf->cur_pos < buf->size)
	{
		load_section(buf);
	}
	update_z80_bank_pointer(gen);
}

uint16_t read_dma_value(uint32_t address)
{
	genesis_context *genesis = (genesis_context *)current_system;
	//TODO: Figure out what happens when you try to DMA from weird adresses like IO or banked Z80 area
	if ((address >= 0xA00000 && address < 0xB00000) || (address >= 0xC00000 && address <= 0xE00000)) {
		return 0;
	}
	
	//addresses here are word addresses (i.e. bit 0 corresponds to A1), so no need to do multiply by 2
	return read_word(address * 2, (void **)genesis->m68k->mem_pointers, &genesis->m68k->options->gen, genesis->m68k);
}

static uint16_t get_open_bus_value(system_header *system)
{
	genesis_context *genesis = (genesis_context *)system;
	return read_dma_value(genesis->m68k->last_prefetch_address/2);
}

static void adjust_int_cycle(m68k_context * context, vdp_context * v_context)
{
	//static int old_int_cycle = CYCLE_NEVER;
	genesis_context *gen = context->system;
	if (context->sync_cycle - context->current_cycle > gen->max_cycles) {
		context->sync_cycle = context->current_cycle + gen->max_cycles;
	}
	context->int_cycle = CYCLE_NEVER;
	if ((context->status & 0x7) < 6) {
		uint32_t next_vint = vdp_next_vint(v_context);
		if (next_vint != CYCLE_NEVER) {
			context->int_cycle = next_vint;
			context->int_num = 6;
		}
		if ((context->status & 0x7) < 4) {
			uint32_t next_hint = vdp_next_hint(v_context);
			if (next_hint != CYCLE_NEVER) {
				next_hint = next_hint < context->current_cycle ? context->current_cycle : next_hint;
				if (next_hint < context->int_cycle) {
					context->int_cycle = next_hint;
					context->int_num = 4;

				}
			}
		}
	}
	if (context->int_cycle > context->current_cycle && context->int_pending == INT_PENDING_SR_CHANGE) {
		context->int_pending = INT_PENDING_NONE;
	}
	/*if (context->int_cycle != old_int_cycle) {
		printf("int cycle changed to: %d, level: %d @ %d(%d), frame: %d, vcounter: %d, hslot: %d, mask: %d, hint_counter: %d\n", context->int_cycle, context->int_num, v_context->cycles, context->current_cycle, v_context->frame, v_context->vcounter, v_context->hslot, context->status & 0x7, v_context->hint_counter);
		old_int_cycle = context->int_cycle;
	}*/
	
	if (context->status & M68K_STATUS_TRACE || context->trace_pending) {
		context->target_cycle = context->current_cycle;
		return;
	}

	context->target_cycle = context->int_cycle < context->sync_cycle ? context->int_cycle : context->sync_cycle;
	if (context->should_return) {
		context->target_cycle = context->current_cycle;
	} else if (context->target_cycle < context->current_cycle) {
		//Changes to SR can result in an interrupt cycle that's in the past
		//This can cause issues with the implementation of STOP though
		context->target_cycle = context->current_cycle;
	}
	if (context->target_cycle == context->int_cycle) {
		//Currently delays from Z80 access and refresh are applied only when we sync
		//this can cause extra latency when it comes to interrupts
		//to prevent this code forces some extra synchronization in the period immediately before an interrupt
		if ((context->target_cycle - context->current_cycle) > gen->int_latency_prev1) {
			context->target_cycle = context->sync_cycle = context->int_cycle - gen->int_latency_prev1;
		} else if ((context->target_cycle - context->current_cycle) > gen->int_latency_prev2) {
			context->target_cycle = context->sync_cycle = context->int_cycle - gen->int_latency_prev2;
		} else {
			context->target_cycle = context->sync_cycle = context->current_cycle;
		}
		
	}
	/*printf("Cyc: %d, Trgt: %d, Int Cyc: %d, Int: %d, Mask: %X, V: %d, H: %d, HICount: %d, HReg: %d, Line: %d\n",
		context->current_cycle, context->target_cycle, context->int_cycle, context->int_num, (context->status & 0x7),
		v_context->regs[REG_MODE_2] & 0x20, v_context->regs[REG_MODE_1] & 0x10, v_context->hint_counter, v_context->regs[REG_HINT], v_context->cycles / MCLKS_LINE);*/
}

//#define DO_DEBUG_PRINT
#ifdef DO_DEBUG_PRINT
#define dprintf printf
#define dputs puts
#else
#define dprintf
#define dputs
#endif

static void z80_next_int_pulse(z80_context * z_context)
{
	genesis_context * gen = z_context->system;
	z_context->int_pulse_start = vdp_next_vint_z80(gen->vdp);
	z_context->int_pulse_end = z_context->int_pulse_start + Z80_INT_PULSE_MCLKS;
}

static void sync_z80(z80_context * z_context, uint32_t mclks)
{
#ifndef NO_Z80
	if (z80_enabled) {
		z80_run(z_context, mclks);
	} else
#endif
	{
		z_context->current_cycle = mclks;
	}
}

static void sync_sound(genesis_context * gen, uint32_t target)
{
	//printf("YM | Cycle: %d, bpos: %d, PSG | Cycle: %d, bpos: %d\n", gen->ym->current_cycle, gen->ym->buffer_pos, gen->psg->cycles, gen->psg->buffer_pos * 2);
	while (target > gen->psg->cycles && target - gen->psg->cycles > MAX_SOUND_CYCLES) {
		uint32_t cur_target = gen->psg->cycles + MAX_SOUND_CYCLES;
		//printf("Running PSG to cycle %d\n", cur_target);
		psg_run(gen->psg, cur_target);
		//printf("Running YM-2612 to cycle %d\n", cur_target);
		ym_run(gen->ym, cur_target);
	}
	psg_run(gen->psg, target);
	ym_run(gen->ym, target);

	//printf("Target: %d, YM bufferpos: %d, PSG bufferpos: %d\n", target, gen->ym->buffer_pos, gen->psg->buffer_pos * 2);
}

//TODO: move this inside the system context
static uint32_t last_frame_num;

//My refresh emulation isn't currently good enough and causes more problems than it solves
#define REFRESH_EMULATION
#ifdef REFRESH_EMULATION
#define REFRESH_INTERVAL 128
#define REFRESH_DELAY 2
uint32_t last_sync_cycle;
uint32_t refresh_counter;
#endif

#include <limits.h>
#define ADJUST_BUFFER (8*MCLKS_LINE*313)
#define MAX_NO_ADJUST (UINT_MAX-ADJUST_BUFFER)

m68k_context * sync_components(m68k_context * context, uint32_t address)
{
	genesis_context * gen = context->system;
	vdp_context * v_context = gen->vdp;
	z80_context * z_context = gen->z80;
#ifdef REFRESH_EMULATION
	//lame estimation of refresh cycle delay
	refresh_counter += context->current_cycle - last_sync_cycle;
	if (!gen->bus_busy) {
		context->current_cycle += REFRESH_DELAY * MCLKS_PER_68K * (refresh_counter / (MCLKS_PER_68K * REFRESH_INTERVAL));
	}
	refresh_counter = refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
#endif

	uint32_t mclks = context->current_cycle;
	sync_z80(z_context, mclks);
	sync_sound(gen, mclks);
	vdp_run_context(v_context, mclks);
	if (v_context->frame != last_frame_num) {
		//printf("reached frame end %d | MCLK Cycles: %d, Target: %d, VDP cycles: %d, vcounter: %d, hslot: %d\n", last_frame_num, mclks, gen->frame_end, v_context->cycles, v_context->vcounter, v_context->hslot);
		last_frame_num = v_context->frame;

		if(exit_after){
			--exit_after;
			if (!exit_after) {
				exit(0);
			}
		}
		if (context->current_cycle > MAX_NO_ADJUST) {
			uint32_t deduction = mclks - ADJUST_BUFFER;
			vdp_adjust_cycles(v_context, deduction);
			io_adjust_cycles(gen->io.ports, context->current_cycle, deduction);
			io_adjust_cycles(gen->io.ports+1, context->current_cycle, deduction);
			io_adjust_cycles(gen->io.ports+2, context->current_cycle, deduction);
			context->current_cycle -= deduction;
			z80_adjust_cycles(z_context, deduction);
			gen->ym->current_cycle -= deduction;
			gen->psg->cycles -= deduction;
			if (gen->ym->write_cycle != CYCLE_NEVER) {
				gen->ym->write_cycle = gen->ym->write_cycle >= deduction ? gen->ym->write_cycle - deduction : 0;
			}
		}
	}
	gen->frame_end = vdp_cycles_to_frame_end(v_context);
	context->sync_cycle = gen->frame_end;
	//printf("Set sync cycle to: %d @ %d, vcounter: %d, hslot: %d\n", context->sync_cycle, context->current_cycle, v_context->vcounter, v_context->hslot);
	if (context->int_ack) {
		//printf("acknowledging %d @ %d:%d, vcounter: %d, hslot: %d\n", context->int_ack, context->current_cycle, v_context->cycles, v_context->vcounter, v_context->hslot);
		vdp_int_ack(v_context);
		context->int_ack = 0;
	}
	if (!address && (gen->header.enter_debugger || gen->header.save_state)) {
		context->sync_cycle = context->current_cycle + 1;
	}
	adjust_int_cycle(context, v_context);
	if (address) {
		if (gen->header.enter_debugger) {
			gen->header.enter_debugger = 0;
			debugger(context, address);
		}
		if (gen->header.save_state && (z_context->pc || !z_context->native_pc || z_context->reset || !z_context->busreq)) {
			uint8_t slot = gen->header.save_state - 1;
			gen->header.save_state = 0;
			if (z_context->native_pc && !z_context->reset) {
				//advance Z80 core to the start of an instruction
				while (!z_context->pc)
				{
					sync_z80(z_context, z_context->current_cycle + MCLKS_PER_Z80);
				}
			}
			char *save_path;
			if (slot == QUICK_SAVE_SLOT) {
				save_path = save_state_path;
			} else {
				char slotname[] = "slot_0.state";
				slotname[5] = '0' + slot;
				if (!use_native_states) {
					strcpy(slotname + 7, "gst");
				}
				char const *parts[] = {gen->header.save_dir, PATH_SEP, slotname};
				save_path = alloc_concat_m(3, parts);
			}
			if (use_native_states) {
				serialize_buffer state;
				init_serialize(&state);
				genesis_serialize(gen, &state, address);
				save_to_file(&state, save_path);
				free(state.data);
			} else {
				save_gst(gen, save_path, address);
			}
			printf("Saved state to %s\n", save_path);
			if (slot != QUICK_SAVE_SLOT) {
				free(save_path);
			}
		} else if(gen->header.save_state) {
			context->sync_cycle = context->current_cycle + 1;
		}
	}
#ifdef REFRESH_EMULATION
	last_sync_cycle = context->current_cycle;
#endif
	return context;
}

static m68k_context * vdp_port_write(uint32_t vdp_port, m68k_context * context, uint16_t value)
{
	if (vdp_port & 0x2700E0) {
		fatal_error("machine freeze due to write to address %X\n", 0xC00000 | vdp_port);
	}
	vdp_port &= 0x1F;
	//printf("vdp_port write: %X, value: %X, cycle: %d\n", vdp_port, value, context->current_cycle);
#ifdef REFRESH_EMULATION
	//do refresh check here so we can avoid adding a penalty for a refresh that happens during a VDP access
	refresh_counter += context->current_cycle - 4*MCLKS_PER_68K - last_sync_cycle;
	context->current_cycle += REFRESH_DELAY * MCLKS_PER_68K * (refresh_counter / (MCLKS_PER_68K * REFRESH_INTERVAL));
	refresh_counter = refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
	last_sync_cycle = context->current_cycle;
#endif
	sync_components(context, 0);
	genesis_context * gen = context->system;
	vdp_context *v_context = gen->vdp;
	uint32_t before_cycle = v_context->cycles;
	if (vdp_port < 0x10) {
		int blocked;
		if (vdp_port < 4) {
			while (vdp_data_port_write(v_context, value) < 0) {
				while(v_context->flags & FLAG_DMA_RUN) {
					vdp_run_dma_done(v_context, gen->frame_end);
					if (v_context->cycles >= gen->frame_end) {
						uint32_t cycle_diff = v_context->cycles - context->current_cycle;
						uint32_t m68k_cycle_diff = (cycle_diff / MCLKS_PER_68K) * MCLKS_PER_68K;
						if (m68k_cycle_diff < cycle_diff) {
							m68k_cycle_diff += MCLKS_PER_68K;
						}
						context->current_cycle += m68k_cycle_diff;
						gen->bus_busy = 1;
						sync_components(context, 0);
						gen->bus_busy = 0;
					}
				}
				//context->current_cycle = v_context->cycles;
			}
		} else if(vdp_port < 8) {
			vdp_run_context_full(v_context, context->current_cycle);
			before_cycle = v_context->cycles;
			blocked = vdp_control_port_write(v_context, value);
			if (blocked) {
				while (blocked) {
					while(v_context->flags & FLAG_DMA_RUN) {
						vdp_run_dma_done(v_context, gen->frame_end);
						if (v_context->cycles >= gen->frame_end) {
							uint32_t cycle_diff = v_context->cycles - context->current_cycle;
							uint32_t m68k_cycle_diff = (cycle_diff / MCLKS_PER_68K) * MCLKS_PER_68K;
							if (m68k_cycle_diff < cycle_diff) {
								m68k_cycle_diff += MCLKS_PER_68K;
							}
							context->current_cycle += m68k_cycle_diff;
							gen->bus_busy = 1;
							sync_components(context, 0);
							gen->bus_busy = 0;
						}
					}
					
					if (blocked < 0) {
						blocked = vdp_control_port_write(v_context, value);
					} else {
						blocked = 0;
					}
				}
			} else {
				context->sync_cycle = gen->frame_end = vdp_cycles_to_frame_end(v_context);
				//printf("Set sync cycle to: %d @ %d, vcounter: %d, hslot: %d\n", context->sync_cycle, context->current_cycle, v_context->vcounter, v_context->hslot);
				adjust_int_cycle(context, v_context);
			}
		} else {
			fatal_error("Illegal write to HV Counter port %X\n", vdp_port);
		}
		if (v_context->cycles != before_cycle) {
			//printf("68K paused for %d (%d) cycles at cycle %d (%d) for write\n", v_context->cycles - context->current_cycle, v_context->cycles - before_cycle, context->current_cycle, before_cycle);
			uint32_t cycle_diff = v_context->cycles - context->current_cycle;
			uint32_t m68k_cycle_diff = (cycle_diff / MCLKS_PER_68K) * MCLKS_PER_68K;
			if (m68k_cycle_diff < cycle_diff) {
				m68k_cycle_diff += MCLKS_PER_68K;
			}
			context->current_cycle += m68k_cycle_diff;
			//Lock the Z80 out of the bus until the VDP access is complete
			gen->bus_busy = 1;
			sync_z80(gen->z80, v_context->cycles);
			gen->bus_busy = 0;
		}
	} else if (vdp_port < 0x18) {
		psg_write(gen->psg, value);
	} else {
		vdp_test_port_write(gen->vdp, value);
	}
#ifdef REFRESH_EMULATION
	last_sync_cycle -= 4;
	//refresh may have happened while we were waiting on the VDP,
	//so advance refresh_counter but don't add any delays
	if (vdp_port >= 4 && vdp_port < 8 && v_context->cycles != before_cycle) {
		refresh_counter = 0;
	} else {
		refresh_counter += (context->current_cycle - last_sync_cycle);
		refresh_counter = refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
	}
	last_sync_cycle = context->current_cycle;
#endif
	return context;
}

static m68k_context * vdp_port_write_b(uint32_t vdp_port, m68k_context * context, uint8_t value)
{
	return vdp_port_write(vdp_port, context, vdp_port < 0x10 ? value | value << 8 : ((vdp_port & 1) ? value : 0));
}

static void * z80_vdp_port_write(uint32_t vdp_port, void * vcontext, uint8_t value)
{
	z80_context * context = vcontext;
	genesis_context * gen = context->system;
	vdp_port &= 0xFF;
	if (vdp_port & 0xE0) {
		fatal_error("machine freeze due to write to Z80 address %X\n", 0x7F00 | vdp_port);
	}
	if (vdp_port < 0x10) {
		//These probably won't currently interact well with the 68K accessing the VDP
		if (vdp_port < 4) {
			vdp_run_context(gen->vdp, context->current_cycle);
			vdp_data_port_write(gen->vdp, value << 8 | value);
		} else if (vdp_port < 8) {
			vdp_run_context_full(gen->vdp, context->current_cycle);
			vdp_control_port_write(gen->vdp, value << 8 | value);
		} else {
			fatal_error("Illegal write to HV Counter port %X\n", vdp_port);
		}
	} else if (vdp_port < 0x18) {
		sync_sound(gen, context->current_cycle);
		psg_write(gen->psg, value);
	} else {
		vdp_test_port_write(gen->vdp, value);
	}
	return context;
}

static uint16_t vdp_port_read(uint32_t vdp_port, m68k_context * context)
{
	if (vdp_port & 0x2700E0) {
		fatal_error("machine freeze due to read from address %X\n", 0xC00000 | vdp_port);
	}
	vdp_port &= 0x1F;
	uint16_t value;
#ifdef REFRESH_EMULATION
	//do refresh check here so we can avoid adding a penalty for a refresh that happens during a VDP access
	refresh_counter += context->current_cycle - 4*MCLKS_PER_68K - last_sync_cycle;
	context->current_cycle += REFRESH_DELAY * MCLKS_PER_68K * (refresh_counter / (MCLKS_PER_68K * REFRESH_INTERVAL));
	refresh_counter = refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
	last_sync_cycle = context->current_cycle;
#endif
	sync_components(context, 0);
	genesis_context *gen = context->system;
	vdp_context * v_context = gen->vdp;
	uint32_t before_cycle = v_context->cycles;
	if (vdp_port < 0x10) {
		if (vdp_port < 4) {
			value = vdp_data_port_read(v_context);
		} else if(vdp_port < 8) {
			value = vdp_control_port_read(v_context);
		} else {
			value = vdp_hv_counter_read(v_context);
			//printf("HV Counter: %X at cycle %d\n", value, v_context->cycles);
		}
	} else if (vdp_port < 0x18){
		fatal_error("Illegal read from PSG  port %X\n", vdp_port);
	} else {
		value = vdp_test_port_read(v_context);
	}
	if (v_context->cycles != before_cycle) {
		//printf("68K paused for %d (%d) cycles at cycle %d (%d) for read\n", v_context->cycles - context->current_cycle, v_context->cycles - before_cycle, context->current_cycle, before_cycle);
		context->current_cycle = v_context->cycles;
		//Lock the Z80 out of the bus until the VDP access is complete
		genesis_context *gen = context->system;
		gen->bus_busy = 1;
		sync_z80(gen->z80, v_context->cycles);
		gen->bus_busy = 0;
	}
#ifdef REFRESH_EMULATION
	last_sync_cycle -= 4;
	//refresh may have happened while we were waiting on the VDP,
	//so advance refresh_counter but don't add any delays
	refresh_counter += (context->current_cycle - last_sync_cycle);
	refresh_counter = refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
	last_sync_cycle = context->current_cycle;
#endif
	return value;
}

static uint8_t vdp_port_read_b(uint32_t vdp_port, m68k_context * context)
{
	uint16_t value = vdp_port_read(vdp_port, context);
	if (vdp_port & 1) {
		return value;
	} else {
		return value >> 8;
	}
}

static uint8_t z80_vdp_port_read(uint32_t vdp_port, void * vcontext)
{
	z80_context * context = vcontext;
	if (vdp_port & 0xE0) {
		fatal_error("machine freeze due to read from Z80 address %X\n", 0x7F00 | vdp_port);
	}
	genesis_context * gen = context->system;
	//VDP access goes over the 68K bus like a bank area access
	//typical delay from bus arbitration
	context->current_cycle += 3 * MCLKS_PER_Z80;
	//TODO: add cycle for an access right after a previous one
	//TODO: Below cycle time is an estimate based on the time between 68K !BG goes low and Z80 !MREQ goes high
	//      Needs a new logic analyzer capture to get the actual delay on the 68K side
	gen->m68k->current_cycle += 8 * MCLKS_PER_68K;


	vdp_port &= 0x1F;
	uint16_t ret;
	if (vdp_port < 0x10) {
		//These probably won't currently interact well with the 68K accessing the VDP
		vdp_run_context(gen->vdp, context->current_cycle);
		if (vdp_port < 4) {
			ret = vdp_data_port_read(gen->vdp);
		} else if (vdp_port < 8) {
			ret = vdp_control_port_read(gen->vdp);
		} else {
			ret = vdp_hv_counter_read(gen->vdp);
		}
	} else {
		//TODO: Figure out the correct value today
		ret = 0xFFFF;
	}
	return vdp_port & 1 ? ret : ret >> 8;
}

//TODO: Move this inside the system context
static uint32_t zram_counter = 0;

static m68k_context * io_write(uint32_t location, m68k_context * context, uint8_t value)
{
	genesis_context * gen = context->system;
	if (location < 0x10000) {
		//Access to Z80 memory incurs a one 68K cycle wait state
		context->current_cycle += MCLKS_PER_68K;
		if (!z80_enabled || z80_get_busack(gen->z80, context->current_cycle)) {
			location &= 0x7FFF;
			if (location < 0x4000) {
				gen->zram[location & 0x1FFF] = value;
#ifndef NO_Z80
				z80_handle_code_write(location & 0x1FFF, gen->z80);
#endif
			} else if (location < 0x6000) {
				sync_sound(gen, context->current_cycle);
				if (location & 1) {
					ym_data_write(gen->ym, value);
				} else if(location & 2) {
					ym_address_write_part2(gen->ym, value);
				} else {
					ym_address_write_part1(gen->ym, value);
				}
			} else if (location == 0x6000) {
				gen->z80->bank_reg = (gen->z80->bank_reg >> 1 | value << 8) & 0x1FF;
				if (gen->z80->bank_reg < 0x80) {
					gen->z80->mem_pointers[1] = (gen->z80->bank_reg << 15) + ((char *)gen->z80->mem_pointers[2]);
				} else {
					gen->z80->mem_pointers[1] = NULL;
				}
			} else {
				fatal_error("68K write to unhandled Z80 address %X\n", location);
			}
		}
	} else {
		location &= 0x1FFF;
		if (location < 0x100) {
			switch(location/2)
			{
			case 0x1:
				io_data_write(gen->io.ports, value, context->current_cycle);
				break;
			case 0x2:
				io_data_write(gen->io.ports+1, value, context->current_cycle);
				break;
			case 0x3:
				io_data_write(gen->io.ports+2, value, context->current_cycle);
				break;
			case 0x4:
				io_control_write(gen->io.ports, value, context->current_cycle);
				break;
			case 0x5:
				io_control_write(gen->io.ports+1, value, context->current_cycle);
				break;
			case 0x6:
				io_control_write(gen->io.ports+2, value, context->current_cycle);
				break;
			case 0x7:
				gen->io.ports[0].serial_out = value;
				break;
			case 0x8:
			case 0xB:
			case 0xE:
				//serial input port is not writeable
				break;
			case 0x9:
				gen->io.ports[0].serial_ctrl = value;
				break;
			case 0xA:
				gen->io.ports[1].serial_out = value;
				break;
			case 0xC:
				gen->io.ports[1].serial_ctrl = value;
				break;
			case 0xD:
				gen->io.ports[2].serial_out = value;
				break;
			case 0xF:
				gen->io.ports[2].serial_ctrl = value;
				break;
			}
		} else {
			if (location == 0x1100) {
				if (value & 1) {
					dputs("bus requesting Z80");
					if (z80_enabled) {
						z80_assert_busreq(gen->z80, context->current_cycle);
					} else {
						gen->z80->busack = 1;
					}
				} else {
					if (gen->z80->busreq) {
						dputs("releasing z80 bus");
						#ifdef DO_DEBUG_PRINT
						char fname[20];
						sprintf(fname, "zram-%d", zram_counter++);
						FILE * f = fopen(fname, "wb");
						fwrite(z80_ram, 1, sizeof(z80_ram), f);
						fclose(f);
						#endif
					}
					if (z80_enabled) {
						z80_clear_busreq(gen->z80, context->current_cycle);
					} else {
						gen->z80->busack = 0;
					}
				}
			} else if (location == 0x1200) {
				sync_z80(gen->z80, context->current_cycle);
				if (value & 1) {
					if (z80_enabled) {
						z80_clear_reset(gen->z80, context->current_cycle);
					} else {
						gen->z80->reset = 0;
					}
				} else {
					if (z80_enabled) {
						z80_assert_reset(gen->z80, context->current_cycle);
					} else {
						gen->z80->reset = 1;
					}
					ym_reset(gen->ym);
				}
			}
		}
	}
	return context;
}

static m68k_context * io_write_w(uint32_t location, m68k_context * context, uint16_t value)
{
	if (location < 0x10000 || (location & 0x1FFF) >= 0x100) {
		return io_write(location, context, value >> 8);
	} else {
		return io_write(location, context, value);
	}
}

#define FOREIGN 0x80
#define HZ50 0x40
#define USA FOREIGN
#define JAP 0x00
#define EUR (HZ50|FOREIGN)
#define NO_DISK 0x20

static uint8_t io_read(uint32_t location, m68k_context * context)
{
	uint8_t value;
	genesis_context *gen = context->system;
	if (location < 0x10000) {
		//Access to Z80 memory incurs a one 68K cycle wait state
		context->current_cycle += MCLKS_PER_68K;
		if (!z80_enabled || z80_get_busack(gen->z80, context->current_cycle)) {
			location &= 0x7FFF;
			if (location < 0x4000) {
				value = gen->zram[location & 0x1FFF];
			} else if (location < 0x6000) {
				sync_sound(gen, context->current_cycle);
				value = ym_read_status(gen->ym);
			} else {
				value = 0xFF;
			}
		} else {
			value = 0xFF;
		}
	} else {
		location &= 0x1FFF;
		if (location < 0x100) {
			switch(location/2)
			{
			case 0x0:
				//version bits should be 0 for now since we're not emulating TMSS
				value = gen->version_reg;
				break;
			case 0x1:
				value = io_data_read(gen->io.ports, context->current_cycle);
				break;
			case 0x2:
				value = io_data_read(gen->io.ports+1, context->current_cycle);
				break;
			case 0x3:
				value = io_data_read(gen->io.ports+2, context->current_cycle);
				break;
			case 0x4:
				value = gen->io.ports[0].control;
				break;
			case 0x5:
				value = gen->io.ports[1].control;
				break;
			case 0x6:
				value = gen->io.ports[2].control;
				break;
			case 0x7:
				value = gen->io.ports[0].serial_out;
				break;
			case 0x8:
				value = gen->io.ports[0].serial_in;
				break;
			case 0x9:
				value = gen->io.ports[0].serial_ctrl;
				break;
			case 0xA:
				value = gen->io.ports[1].serial_out;
				break;
			case 0xB:
				value = gen->io.ports[1].serial_in;
				break;
			case 0xC:
				value = gen->io.ports[1].serial_ctrl;
				break;
			case 0xD:
				value = gen->io.ports[2].serial_out;
				break;
			case 0xE:
				value = gen->io.ports[2].serial_in;
				break;
			case 0xF:
				value = gen->io.ports[2].serial_ctrl;
				break;
			default:
				value = 0xFF;
			}
		} else {
			if (location == 0x1100) {
				value = z80_enabled ? !z80_get_busack(gen->z80, context->current_cycle) : !gen->z80->busack;
				value |= (get_open_bus_value(&gen->header) >> 8) & 0xFE;
				dprintf("Byte read of BUSREQ returned %d @ %d (reset: %d)\n", value, context->current_cycle, gen->z80->reset);
			} else if (location == 0x1200) {
				value = !gen->z80->reset;
			} else {
				value = 0xFF;
				printf("Byte read of unknown IO location: %X\n", location);
			}
		}
	}
	return value;
}

static uint16_t io_read_w(uint32_t location, m68k_context * context)
{
	genesis_context *gen = context->system;
	uint16_t value = io_read(location, context);
	if (location < 0x10000 || (location & 0x1FFF) < 0x100) {
		value = value | (value << 8);
	} else {
		value <<= 8;
		value |= get_open_bus_value(&gen->header) & 0xFF;
	}
	return value;
}

static void * z80_write_ym(uint32_t location, void * vcontext, uint8_t value)
{
	z80_context * context = vcontext;
	genesis_context * gen = context->system;
	sync_sound(gen, context->current_cycle);
	if (location & 1) {
		ym_data_write(gen->ym, value);
	} else if (location & 2) {
		ym_address_write_part2(gen->ym, value);
	} else {
		ym_address_write_part1(gen->ym, value);
	}
	return context;
}

static uint8_t z80_read_ym(uint32_t location, void * vcontext)
{
	z80_context * context = vcontext;
	genesis_context * gen = context->system;
	sync_sound(gen, context->current_cycle);
	return ym_read_status(gen->ym);
}

static uint8_t z80_read_bank(uint32_t location, void * vcontext)
{
	z80_context * context = vcontext;
	genesis_context *gen = context->system;
	if (gen->bus_busy) {
		context->current_cycle = context->sync_cycle;
	}
	//typical delay from bus arbitration
	context->current_cycle += 3 * MCLKS_PER_Z80;
	//TODO: add cycle for an access right after a previous one
	//TODO: Below cycle time is an estimate based on the time between 68K !BG goes low and Z80 !MREQ goes high
	//      Needs a new logic analyzer capture to get the actual delay on the 68K side
	gen->m68k->current_cycle += 8 * MCLKS_PER_68K;

	location &= 0x7FFF;
	if (context->mem_pointers[1]) {
		return context->mem_pointers[1][location ^ 1];
	}
	uint32_t address = context->bank_reg << 15 | location;
	if (address >= 0xC00000 && address < 0xE00000) {
		return z80_vdp_port_read(location & 0xFF, context);
	} else {
		fprintf(stderr, "Unhandled read by Z80 from address %X through banked memory area (%X)\n", address, context->bank_reg << 15);
	}
	return 0;
}

static void *z80_write_bank(uint32_t location, void * vcontext, uint8_t value)
{
	z80_context * context = vcontext;
	genesis_context *gen = context->system;
	if (gen->bus_busy) {
		context->current_cycle = context->sync_cycle;
	}
	//typical delay from bus arbitration
	context->current_cycle += 3 * MCLKS_PER_Z80;
	//TODO: add cycle for an access right after a previous one
	//TODO: Below cycle time is an estimate based on the time between 68K !BG goes low and Z80 !MREQ goes high
	//      Needs a new logic analyzer capture to get the actual delay on the 68K side
	gen->m68k->current_cycle += 8 * MCLKS_PER_68K;

	location &= 0x7FFF;
	uint32_t address = context->bank_reg << 15 | location;
	if (address >= 0xE00000) {
		address &= 0xFFFF;
		((uint8_t *)gen->work_ram)[address ^ 1] = value;
	} else if (address >= 0xC00000) {
		z80_vdp_port_write(location & 0xFF, context, value);
	} else {
		fprintf(stderr, "Unhandled write by Z80 to address %X through banked memory area\n", address);
	}
	return context;
}

static void *z80_write_bank_reg(uint32_t location, void * vcontext, uint8_t value)
{
	z80_context * context = vcontext;

	context->bank_reg = (context->bank_reg >> 1 | value << 8) & 0x1FF;
	update_z80_bank_pointer(context->system);

	return context;
}

static void set_speed_percent(system_header * system, uint32_t percent)
{
	genesis_context *context = (genesis_context *)system;
	uint32_t old_clock = context->master_clock;
	context->master_clock = ((uint64_t)context->normal_clock * (uint64_t)percent) / 100;
	while (context->ym->current_cycle != context->psg->cycles) {
		sync_sound(context, context->psg->cycles + MCLKS_PER_PSG);
	}
	ym_adjust_master_clock(context->ym, context->master_clock);
	psg_adjust_master_clock(context->psg, context->master_clock);
}

void set_region(genesis_context *gen, rom_info *info, uint8_t region)
{
	if (!region) {
		char * def_region = tern_find_path_default(config, "system\0default_region\0", (tern_val){.ptrval = "U"}, TVAL_PTR).ptrval;
		if (!info->regions || (info->regions & translate_region_char(toupper(*def_region)))) {
			region = translate_region_char(toupper(*def_region));
		} else {
			region = info->regions;
		}
	}
	if (region & REGION_E) {
		gen->version_reg = NO_DISK | EUR;
	} else if (region & REGION_J) {
		gen->version_reg = NO_DISK | JAP;
	} else {
		gen->version_reg = NO_DISK | USA;
	}
	
	if (region & HZ50) {
		gen->normal_clock = MCLKS_PAL;
	} else {
		gen->normal_clock = MCLKS_NTSC;
	}
	gen->master_clock = gen->normal_clock;
}

static void handle_reset_requests(genesis_context *gen)
{
	while (gen->reset_requested)
	{
		gen->reset_requested = 0;
		z80_assert_reset(gen->z80, gen->m68k->current_cycle);
		z80_clear_busreq(gen->z80, gen->m68k->current_cycle);
		ym_reset(gen->ym);
		//Is there any sort of VDP reset?
		m68k_reset(gen->m68k);
	}
	vdp_release_framebuffer(gen->vdp);
}

#include "m68k_internal.h" //needed for get_native_address_trans, should be eliminated once handling of PC is cleaned up
static uint8_t load_state(system_header *system, uint8_t slot)
{
	genesis_context *gen = (genesis_context *)system;
	char numslotname[] = "slot_0.state";
	char *slotname;
	if (slot == QUICK_SAVE_SLOT) {
		slotname = "quicksave.state";
	} else {
		numslotname[5] = '0' + slot;
		slotname = numslotname;
	}
	char const *parts[] = {gen->header.save_dir, PATH_SEP, slotname};
	char *statepath = alloc_concat_m(3, parts);
	deserialize_buffer state;
	uint32_t pc = 0;
	uint8_t ret;
	if (load_from_file(&state, statepath)) {
		genesis_deserialize(&state, gen);
		free(state.data);
		//HACK
		pc = gen->m68k->last_prefetch_address;
		ret = 1;
	} else {
		strcpy(statepath + strlen(statepath)-strlen("state"), "gst");
		pc = load_gst(gen, statepath);
		ret = pc != 0;
	}
	if (ret) {
		gen->m68k->resume_pc = get_native_address_trans(gen->m68k, pc);
	}
	free(statepath);
	return ret;
}

static void start_genesis(system_header *system, char *statefile)
{
	genesis_context *gen = (genesis_context *)system;
	set_keybindings(&gen->io);
	render_set_video_standard((gen->version_reg & HZ50) ? VID_PAL : VID_NTSC);
	if (statefile) {
		//first try loading as a native format savestate
		deserialize_buffer state;
		uint32_t pc;
		if (load_from_file(&state, statefile)) {
			genesis_deserialize(&state, gen);
			free(state.data);
			//HACK
			pc = gen->m68k->last_prefetch_address;
		} else {
			pc = load_gst(gen, statefile);
			if (!pc) {
				fatal_error("Failed to load save state %s\n", statefile);
			}
		}
		printf("Loaded %s\n", statefile);
		if (gen->header.enter_debugger) {
			gen->header.enter_debugger = 0;
			insert_breakpoint(gen->m68k, pc, gen->header.debugger_type == DEBUGGER_NATIVE ? debugger : gdb_debug_enter);
		}
		adjust_int_cycle(gen->m68k, gen->vdp);
		start_68k_context(gen->m68k, pc);
	} else {
		if (gen->header.enter_debugger) {
			gen->header.enter_debugger = 0;
			uint32_t address = gen->cart[2] << 16 | gen->cart[3];
			insert_breakpoint(gen->m68k, address, gen->header.debugger_type == DEBUGGER_NATIVE ? debugger : gdb_debug_enter);
		}
		m68k_reset(gen->m68k);
	}
	handle_reset_requests(gen);
	return;
}

static void resume_genesis(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	map_all_bindings(&gen->io);
	render_set_video_standard((gen->version_reg & HZ50) ? VID_PAL : VID_NTSC);
	vdp_reacquire_framebuffer(gen->vdp);
	resume_68k(gen->m68k);
	handle_reset_requests(gen);
}

static void inc_debug_mode(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	gen->vdp->debug++;
	if (gen->vdp->debug == 7) {
		gen->vdp->debug = 0;
	}
}

static void inc_debug_pal(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	gen->vdp->debug_pal++;
	if (gen->vdp->debug_pal == 4) {
		gen->vdp->debug_pal = 0;
	}
}

static void request_exit(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	gen->m68k->should_return = 1;
}

static void persist_save(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	if (gen->save_type == SAVE_NONE) {
		return;
	}
	FILE * f = fopen(save_filename, "wb");
	if (!f) {
		fprintf(stderr, "Failed to open %s file %s for writing\n", save_type_name(gen->save_type), save_filename);
		return;
	}
	fwrite(gen->save_storage, 1, gen->save_size, f);
	fclose(f);
	printf("Saved %s to %s\n", save_type_name(gen->save_type), save_filename);
}

static void load_save(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	FILE * f = fopen(save_filename, "rb");
	if (f) {
		uint32_t read = fread(gen->save_storage, 1, gen->save_size, f);
		fclose(f);
		if (read > 0) {
			printf("Loaded %s from %s\n", save_type_name(gen->save_type), save_filename);
		}
	}
}

static void soft_reset(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	gen->m68k->should_return = 1;
	gen->reset_requested = 1;
}

static void free_genesis(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	vdp_free(gen->vdp);
	m68k_options_free(gen->m68k->options);
	free(gen->cart);
	free(gen->m68k);
	free(gen->work_ram);
	z80_options_free(gen->z80->options);
	free(gen->z80);
	free(gen->zram);
	ym_free(gen->ym);
	psg_free(gen->psg);
	free(gen->save_storage);
	free(gen->header.save_dir);
	free(gen->lock_on);
	free(gen);
}

genesis_context *alloc_init_genesis(rom_info *rom, void *main_rom, void *lock_on, uint32_t system_opts, uint8_t force_region)
{
	static memmap_chunk z80_map[] = {
		{ 0x0000, 0x4000,  0x1FFF, 0, 0, MMAP_READ | MMAP_WRITE | MMAP_CODE, NULL, NULL, NULL, NULL,              NULL },
		{ 0x8000, 0x10000, 0x7FFF, 0, 0, 0,                                  NULL, NULL, NULL, z80_read_bank,     z80_write_bank},
		{ 0x4000, 0x6000,  0x0003, 0, 0, 0,                                  NULL, NULL, NULL, z80_read_ym,       z80_write_ym},
		{ 0x6000, 0x6100,  0xFFFF, 0, 0, 0,                                  NULL, NULL, NULL, NULL,              z80_write_bank_reg},
		{ 0x7F00, 0x8000,  0x00FF, 0, 0, 0,                                  NULL, NULL, NULL, z80_vdp_port_read, z80_vdp_port_write}
	};
	genesis_context *gen = calloc(1, sizeof(genesis_context));
	gen->header.set_speed_percent = set_speed_percent;
	gen->header.start_context = start_genesis;
	gen->header.resume_context = resume_genesis;
	gen->header.load_save = load_save;
	gen->header.persist_save = persist_save;
	gen->header.load_state = load_state;
	gen->header.soft_reset = soft_reset;
	gen->header.free_context = free_genesis;
	gen->header.get_open_bus_value = get_open_bus_value;
	gen->header.request_exit = request_exit;
	gen->header.inc_debug_mode = inc_debug_mode;
	gen->header.inc_debug_pal = inc_debug_pal;
	gen->header.type = SYSTEM_GENESIS;
	set_region(gen, rom, force_region);

	gen->vdp = malloc(sizeof(vdp_context));
	init_vdp_context(gen->vdp, gen->version_reg & 0x40);
	gen->vdp->system = &gen->header;
	gen->frame_end = vdp_cycles_to_frame_end(gen->vdp);
	char * config_cycles = tern_find_path(config, "clocks\0max_cycles\0", TVAL_PTR).ptrval;
	gen->max_cycles = config_cycles ? atoi(config_cycles) : DEFAULT_SYNC_INTERVAL;
	gen->int_latency_prev1 = MCLKS_PER_68K * 32;
	gen->int_latency_prev2 = MCLKS_PER_68K * 16;

	char * lowpass_cutoff_str = tern_find_path(config, "audio\0lowpass_cutoff\0", TVAL_PTR).ptrval;
	uint32_t lowpass_cutoff = lowpass_cutoff_str ? atoi(lowpass_cutoff_str) : DEFAULT_LOWPASS_CUTOFF;
	
	gen->ym = malloc(sizeof(ym2612_context));
	ym_init(gen->ym, render_sample_rate(), gen->master_clock, MCLKS_PER_YM, render_audio_buffer(), system_opts, lowpass_cutoff);

	gen->psg = malloc(sizeof(psg_context));
	psg_init(gen->psg, render_sample_rate(), gen->master_clock, MCLKS_PER_PSG, render_audio_buffer(), lowpass_cutoff);

	gen->zram = calloc(1, Z80_RAM_BYTES);
	z80_map[0].buffer = gen->zram = calloc(1, Z80_RAM_BYTES);
#ifndef NO_Z80
	z80_options *z_opts = malloc(sizeof(z80_options));
	init_z80_opts(z_opts, z80_map, 5, NULL, 0, MCLKS_PER_Z80, 0xFFFF);
	gen->z80 = init_z80_context(z_opts);
	gen->z80->next_int_pulse = z80_next_int_pulse;
	z80_assert_reset(gen->z80, 0);
#else
	gen->z80 = calloc(1, sizeof(z80_context));
#endif

	gen->z80->system = gen;
	gen->z80->mem_pointers[0] = gen->zram;
	gen->z80->mem_pointers[1] = gen->z80->mem_pointers[2] = (uint8_t *)main_rom;

	gen->cart = main_rom;
	gen->lock_on = lock_on;
	gen->work_ram = calloc(2, RAM_WORDS);
	if (!strcmp("random", tern_find_path_default(config, "system\0ram_init\0", (tern_val){.ptrval = "zero"}, TVAL_PTR).ptrval))
	{
		srand(time(NULL));
		for (int i = 0; i < RAM_WORDS; i++)
		{
			gen->work_ram[i] = rand();
		}
		for (int i = 0; i < Z80_RAM_BYTES; i++)
		{
			gen->zram[i] = rand();
		}
		for (int i = 0; i < VRAM_SIZE; i++)
		{
			gen->vdp->vdpmem[i] = rand();
		}
		for (int i = 0; i < SAT_CACHE_SIZE; i++)
		{
			gen->vdp->sat_cache[i] = rand();
		}
		for (int i = 0; i < CRAM_SIZE; i++)
		{
			write_cram_internal(gen->vdp, i, rand());
		}
		for (int i = 0; i < VSRAM_SIZE; i++)
		{
			gen->vdp->vsram[i] = rand();
		}
	}
	setup_io_devices(config, rom, &gen->io);

	gen->mapper_type = rom->mapper_type;
	gen->save_type = rom->save_type;
	if (gen->save_type != SAVE_NONE) {
		gen->save_ram_mask = rom->save_mask;
		gen->save_size = rom->save_size;
		gen->save_storage = rom->save_buffer;
		gen->eeprom_map = rom->eeprom_map;
		gen->num_eeprom = rom->num_eeprom;
		if (gen->save_type == SAVE_I2C) {
			eeprom_init(&gen->eeprom, gen->save_storage, gen->save_size);
		} else if (gen->save_type == SAVE_NOR) {
			nor_flash_init(&gen->nor, gen->save_storage, gen->save_size, rom->save_page_size, rom->save_product_id, rom->save_bus);
		}
	} else {
		gen->save_storage = NULL;
	}
	
	//This must happen before we generate memory access functions in init_m68k_opts
	for (int i = 0; i < rom->map_chunks; i++)
	{
		if (rom->map[i].start == 0xE00000) {
			rom->map[i].buffer = gen->work_ram;
			break;
		}
	}

	m68k_options *opts = malloc(sizeof(m68k_options));
	init_m68k_opts(opts, rom->map, rom->map_chunks, MCLKS_PER_68K);
	//TODO: make this configurable
	opts->gen.flags |= M68K_OPT_BROKEN_READ_MODIFY;
	gen->m68k = init_68k_context(opts, NULL);
	gen->m68k->system = gen;
	opts->address_log = (system_opts & OPT_ADDRESS_LOG) ? fopen("address.log", "w") : NULL;
	
	//This must happen after the 68K context has been allocated
	for (int i = 0; i < rom->map_chunks; i++)
	{
		if (rom->map[i].flags & MMAP_PTR_IDX) {
			gen->m68k->mem_pointers[rom->map[i].ptr_index] = rom->map[i].buffer;
		}
	}
	
	if (gen->mapper_type == MAPPER_SEGA) {
		//initialize bank registers
		for (int i = 1; i < sizeof(gen->bank_regs); i++)
		{
			gen->bank_regs[i] = i;
		}
	}

	return gen;
}

genesis_context *alloc_config_genesis(void *rom, uint32_t rom_size, void *lock_on, uint32_t lock_on_size, uint32_t ym_opts, uint8_t force_region, rom_info *info_out)
{
	static memmap_chunk base_map[] = {
		{0xE00000, 0x1000000, 0xFFFF,   0, 0, MMAP_READ | MMAP_WRITE | MMAP_CODE, NULL,
		           NULL,          NULL,         NULL,            NULL},
		{0xC00000, 0xE00000,  0x1FFFFF, 0, 0, 0,                                  NULL,
		           (read_16_fun)vdp_port_read,  (write_16_fun)vdp_port_write,
		           (read_8_fun)vdp_port_read_b, (write_8_fun)vdp_port_write_b},
		{0xA00000, 0xA12000,  0x1FFFF,  0, 0, 0,                                  NULL,
		           (read_16_fun)io_read_w,      (write_16_fun)io_write_w,
		           (read_8_fun)io_read,         (write_8_fun)io_write}
	};
	static tern_node *rom_db;
	if (!rom_db) {
		rom_db = load_rom_db();
	}
	*info_out = configure_rom(rom_db, rom, rom_size, lock_on, lock_on_size, base_map, sizeof(base_map)/sizeof(base_map[0]));
	rom = info_out->rom;
	rom_size = info_out->rom_size;
#ifndef BLASTEM_BIG_ENDIAN
	byteswap_rom(rom_size, rom);
	if (lock_on) {
		byteswap_rom(lock_on_size, lock_on);
	}
#endif
	char *m68k_divider = tern_find_path(config, "clocks\0m68k_divider\0", TVAL_PTR).ptrval;
	if (!m68k_divider) {
		m68k_divider = "7";
	}
	MCLKS_PER_68K = atoi(m68k_divider);
	if (!MCLKS_PER_68K) {
		MCLKS_PER_68K = 7;
	}
	return alloc_init_genesis(info_out, rom, lock_on, ym_opts, force_region);
}
