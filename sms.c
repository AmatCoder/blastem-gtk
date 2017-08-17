#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include "sms.h"
#include "blastem.h"
#include "render.h"
#include "util.h"
#include "debug.h"

static void *memory_io_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	if (location & 1) {
		uint8_t fuzzy_ctrl_0 = sms->io.ports[0].control, fuzzy_ctrl_1 = sms->io.ports[1].control;
		io_control_write(sms->io.ports, (~value) << 5 & 0x60, z80->current_cycle);
		fuzzy_ctrl_0 |= sms->io.ports[0].control;
		io_control_write(sms->io.ports+1, (~value) << 3 & 0x60, z80->current_cycle);
		fuzzy_ctrl_1 |= sms->io.ports[1].control;
		if (
			(fuzzy_ctrl_0 & 0x40 & (sms->io.ports[0].output ^ (value << 1)) & (value << 1))
			|| (fuzzy_ctrl_0 & 0x40 & (sms->io.ports[1].output ^ (value >> 1)) & (value >> 1))
		) {
			//TH is an output and it went from 0 -> 1
			vdp_run_context(sms->vdp, z80->current_cycle);
			vdp_latch_hv(sms->vdp);
		}
		io_data_write(sms->io.ports, value << 1, z80->current_cycle);
		io_data_write(sms->io.ports + 1, value >> 1, z80->current_cycle);
	} else {
		//TODO: memory control write
	}
	return vcontext;
}

static uint8_t hv_read(uint32_t location, void *vcontext)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	vdp_run_context(sms->vdp, z80->current_cycle);
	uint16_t hv = vdp_hv_counter_read(sms->vdp);
	if (location & 1) {
		return hv;
	} else {
		return hv >> 8;
	}
}

static void *sms_psg_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	psg_run(sms->psg, z80->current_cycle);
	psg_write(sms->psg, value);
	return vcontext;
}

static void update_interrupts(sms_context *sms)
{
	uint32_t vint = vdp_next_vint(sms->vdp);
	uint32_t hint = vdp_next_hint(sms->vdp);
	sms->z80->int_pulse_start = vint < hint ? vint : hint;
}

static uint8_t vdp_read(uint32_t location, void *vcontext)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	vdp_run_context(sms->vdp, z80->current_cycle);
	if (location & 1) {
		uint8_t ret = vdp_control_port_read(sms->vdp);
		sms->vdp->flags2 &= ~(FLAG2_VINT_PENDING|FLAG2_HINT_PENDING);
		update_interrupts(sms);
		return ret;
	} else {
		return vdp_data_port_read_pbc(sms->vdp);
	}
}

static void *vdp_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	if (location & 1) {
		vdp_run_context_full(sms->vdp, z80->current_cycle);
		vdp_control_port_write_pbc(sms->vdp, value);
		update_interrupts(sms);
	} else {
		vdp_run_context(sms->vdp, z80->current_cycle);
		vdp_data_port_write_pbc(sms->vdp, value);
	}
	return vcontext;
}

static uint8_t io_read(uint32_t location, void *vcontext)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	if (location == 0xC0 || location == 0xDC) {
		uint8_t port_a = io_data_read(sms->io.ports, z80->current_cycle);
		uint8_t port_b = io_data_read(sms->io.ports+1, z80->current_cycle);
		return (port_a & 0x3F) | (port_b << 6);
	}
	if (location == 0xC1 || location == 0xDD) {
		uint8_t port_a = io_data_read(sms->io.ports, z80->current_cycle);
		uint8_t port_b = io_data_read(sms->io.ports+1, z80->current_cycle);
		return (port_a & 0x40) | (port_b >> 2 & 0xF) | (port_b << 1 & 0x80) | 0x10;
	}
	return 0xFF;
}

static void update_mem_map(uint32_t location, sms_context *sms, uint8_t value)
{
	z80_context *z80 = sms->z80;
	void *old_value;
	if (location) {
		uint32_t idx = location - 1;
		old_value = z80->mem_pointers[idx];
		z80->mem_pointers[idx] = sms->rom + (value << 14 & (sms->rom_size-1));
		if (old_value != z80->mem_pointers[idx]) {
			//invalidate any code we translated for the relevant bank
			z80_invalidate_code_range(z80, idx ? idx * 0x4000 : 0x400, idx * 0x4000 + 0x4000);
		}
	} else {
		old_value = z80->mem_pointers[2];
		if (value & 8) {
			//cartridge RAM is enabled
			z80->mem_pointers[2] = sms->cart_ram + (value & 4 ? (SMS_CART_RAM_SIZE/2) : 0);
		} else {
			//cartridge RAM is disabled
			z80->mem_pointers[2] = sms->rom + (sms->bank_regs[3] << 14 & (sms->rom_size-1));
		}
		if (old_value != z80->mem_pointers[2]) {
			//invalidate any code we translated for the relevant bank
			z80_invalidate_code_range(z80, 0x8000, 0xC000);
		}
	}
}

static void *mapper_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	location &= 3;
	sms->ram[0x1FFC + location] = value;
	sms->bank_regs[location] = value;
	update_mem_map(location, sms, value);
	return vcontext;
}

static void *cart_ram_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	if (sms->bank_regs[0] & 8) {
		//cartridge RAM is enabled
		location &= 0x3FFF;
		z80->mem_pointers[2][location] = value;
		z80_handle_code_write(0x8000 + location, z80);
	}
	return vcontext;
}

uint8_t debug_commands(system_header *system, char *input_buf)
{
	sms_context *sms = (sms_context *)system;
	switch(input_buf[0])
	{
	case 'v':
		if (input_buf[1] == 'r') {
			vdp_print_reg_explain(sms->vdp);
		} else if (input_buf[1] == 's') {
			vdp_print_sprite_table(sms->vdp);
		} else {
			return 0;
		}
		break;
	}
	return 1;
}

static memmap_chunk io_map[] = {
	{0x00, 0x40, 0xFF, 0, 0, 0, NULL, NULL, NULL, NULL,     memory_io_write},
	{0x40, 0x80, 0xFF, 0, 0, 0, NULL, NULL, NULL, hv_read,  sms_psg_write},
	{0x80, 0xC0, 0xFF, 0, 0, 0, NULL, NULL, NULL, vdp_read, vdp_write},
	{0xC0, 0x100,0xFF, 0, 0, 0, NULL, NULL, NULL, io_read,  NULL}
};

static void set_speed_percent(system_header * system, uint32_t percent)
{
	sms_context *context = (sms_context *)system;
	uint32_t old_clock = context->master_clock;
	context->master_clock = ((uint64_t)context->normal_clock * (uint64_t)percent) / 100;

	psg_adjust_master_clock(context->psg, context->master_clock);
}

void sms_serialize(sms_context *sms, serialize_buffer *buf)
{
	start_section(buf, SECTION_Z80);
	z80_serialize(sms->z80, buf);
	end_section(buf);
	
	start_section(buf, SECTION_VDP);
	vdp_serialize(sms->vdp, buf);
	end_section(buf);
	
	start_section(buf, SECTION_PSG);
	psg_serialize(sms->psg, buf);
	end_section(buf);
	
	start_section(buf, SECTION_SEGA_IO_1);
	io_serialize(sms->io.ports, buf);
	end_section(buf);
	
	start_section(buf, SECTION_SEGA_IO_2);
	io_serialize(sms->io.ports + 1, buf);
	end_section(buf);
	
	start_section(buf, SECTION_MAIN_RAM);
	save_int8(buf, sizeof(sms->ram) / 1024);
	save_buffer8(buf, sms->ram, sizeof(sms->ram));
	end_section(buf);
	
	start_section(buf, SECTION_MAPPER);
	save_int8(buf, 1);//mapper type, 1 for Sega mapper
	save_buffer8(buf, sms->bank_regs, sizeof(sms->bank_regs));
	end_section(buf);
	
	start_section(buf, SECTION_CART_RAM);
	save_int8(buf, SMS_CART_RAM_SIZE / 1024);
	save_buffer8(buf, sms->cart_ram, SMS_CART_RAM_SIZE);
	end_section(buf);
}

static void ram_deserialize(deserialize_buffer *buf, void *vsms)
{
	sms_context *sms = vsms;
	uint32_t ram_size = load_int8(buf) * 1024;
	if (ram_size > sizeof(sms->ram)) {
		fatal_error("State has a RAM size of %d bytes", ram_size);
	}
	load_buffer8(buf, sms->ram, ram_size);
}

static void cart_ram_deserialize(deserialize_buffer *buf, void *vsms)
{
	sms_context *sms = vsms;
	uint32_t ram_size = load_int8(buf) * 1024;
	if (ram_size > SMS_CART_RAM_SIZE) {
		fatal_error("State has a cart RAM size of %d bytes", ram_size);
	}
	load_buffer8(buf, sms->cart_ram, ram_size);
}

static void mapper_deserialize(deserialize_buffer *buf, void *vsms)
{
	sms_context *sms = vsms;
	uint8_t mapper_type = load_int8(buf);
	if (mapper_type != 1) {
		warning("State contains an unrecognized mapper type %d, it may be from a newer version of BlastEm\n", mapper_type);
		return;
	}
	for (int i = 0; i < sizeof(sms->bank_regs); i++)
	{
		sms->bank_regs[i] = load_int8(buf);
		update_mem_map(i, sms, sms->bank_regs[i]);
	}
}

void sms_deserialize(deserialize_buffer *buf, sms_context *sms)
{
	register_section_handler(buf, (section_handler){.fun = z80_deserialize, .data = sms->z80}, SECTION_Z80);
	register_section_handler(buf, (section_handler){.fun = vdp_deserialize, .data = sms->vdp}, SECTION_VDP);
	register_section_handler(buf, (section_handler){.fun = psg_deserialize, .data = sms->psg}, SECTION_PSG);
	register_section_handler(buf, (section_handler){.fun = io_deserialize, .data = sms->io.ports}, SECTION_SEGA_IO_1);
	register_section_handler(buf, (section_handler){.fun = io_deserialize, .data = sms->io.ports + 1}, SECTION_SEGA_IO_2);
	register_section_handler(buf, (section_handler){.fun = ram_deserialize, .data = sms}, SECTION_MAIN_RAM);
	register_section_handler(buf, (section_handler){.fun = mapper_deserialize, .data = sms}, SECTION_MAPPER);
	register_section_handler(buf, (section_handler){.fun = cart_ram_deserialize, .data = sms}, SECTION_CART_RAM);
	//TODO: cart RAM
	while (buf->cur_pos < buf->size)
	{
		load_section(buf);
	}
	z80_invalidate_code_range(sms->z80, 0xC000, 0x10000);
	if (sms->bank_regs[0] & 8) {
		//cart RAM is enabled, invalidate the region in case there is any code there
		z80_invalidate_code_range(sms->z80, 0x8000, 0xC000);
	}
}

static void save_state(sms_context *sms, uint8_t slot)
{
	char *save_path;
	if (slot == QUICK_SAVE_SLOT) {
		save_path = save_state_path;
	} else {
		char slotname[] = "slot_0.state";
		slotname[5] = '0' + slot;
		char const *parts[] = {sms->header.save_dir, PATH_SEP, slotname};
		save_path = alloc_concat_m(3, parts);
	}
	serialize_buffer state;
	init_serialize(&state);
	sms_serialize(sms, &state);
	save_to_file(&state, save_path);
	printf("Saved state to %s\n", save_path);
	free(state.data);
}

static uint8_t load_state_path(sms_context *sms, char *path)
{
	deserialize_buffer state;
	uint8_t ret;
	if ((ret = load_from_file(&state, path))) {
		sms_deserialize(&state, sms);
		free(state.data);
		printf("Loaded %s\n", path);
	}
	return ret;
}

static uint8_t load_state(system_header *system, uint8_t slot)
{
	sms_context *sms = (sms_context *)system;
	char numslotname[] = "slot_0.state";
	char *slotname;
	if (slot == QUICK_SAVE_SLOT) {
		slotname = "quicksave.state";
	} else {
		numslotname[5] = '0' + slot;
		slotname = numslotname;
	}
	char const *parts[] = {sms->header.save_dir, PATH_SEP, slotname};
	char *statepath = alloc_concat_m(3, parts);
	uint8_t ret = load_state_path(sms, statepath);
	free(statepath);
	return ret;
}

static void run_sms(system_header *system)
{
	render_disable_ym();
	sms_context *sms = (sms_context *)system;
	uint32_t target_cycle = sms->z80->current_cycle + 3420*16;
	//TODO: PAL support
	render_set_video_standard(VID_NTSC);
	while (!sms->should_return)
	{
		if (system->enter_debugger && sms->z80->pc) {
			system->enter_debugger = 0;
			zdebugger(sms->z80, sms->z80->pc);
		}
		if (sms->z80->nmi_start == CYCLE_NEVER) {
			uint32_t nmi = vdp_next_nmi(sms->vdp);
			if (nmi != CYCLE_NEVER) {
				z80_assert_nmi(sms->z80, nmi);
			}
		}
		z80_run(sms->z80, target_cycle);
		if (sms->z80->reset) {
			z80_clear_reset(sms->z80, sms->z80->current_cycle + 128*15);
		}
		target_cycle = sms->z80->current_cycle;
		vdp_run_context(sms->vdp, target_cycle);
		psg_run(sms->psg, target_cycle);
		
		if (system->save_state) {
			while (!sms->z80->pc) {
				//advance Z80 to an instruction boundary
				z80_run(sms->z80, sms->z80->current_cycle + 1);
			}
			save_state(sms, system->save_state - 1);
			system->save_state = 0;
		}
		
		target_cycle += 3420*16;
		if (target_cycle > 0x10000000) {
			uint32_t adjust = sms->z80->current_cycle - 3420*262*2;
			io_adjust_cycles(sms->io.ports, sms->z80->current_cycle, adjust);
			io_adjust_cycles(sms->io.ports+1, sms->z80->current_cycle, adjust);
			z80_adjust_cycles(sms->z80, adjust);
			vdp_adjust_cycles(sms->vdp, adjust);
			sms->psg->cycles -= adjust;
			target_cycle -= adjust;
		}
	}
	vdp_release_framebuffer(sms->vdp);
	sms->should_return = 0;
	render_enable_ym();
}

static void resume_sms(system_header *system)
{
	sms_context *sms = (sms_context *)system;
	vdp_reacquire_framebuffer(sms->vdp);
	run_sms(system);
}

static void start_sms(system_header *system, char *statefile)
{
	sms_context *sms = (sms_context *)system;
	set_keybindings(&sms->io);
	
	z80_assert_reset(sms->z80, 0);
	z80_clear_reset(sms->z80, 128*15);
	
	if (statefile) {
		load_state_path(sms, statefile);
	}
	
	if (system->enter_debugger) {
		system->enter_debugger = 0;
		zinsert_breakpoint(sms->z80, sms->z80->pc, (uint8_t *)zdebugger);
	}
	
	run_sms(system);
}

static void soft_reset(system_header *system)
{
	sms_context *sms = (sms_context *)system;
	z80_assert_reset(sms->z80, sms->z80->current_cycle);
	sms->z80->target_cycle = sms->z80->sync_cycle = sms->z80->current_cycle;
}

static void free_sms(system_header *system)
{
	sms_context *sms = (sms_context *)system;
	vdp_free(sms->vdp);
	z80_options_free(sms->z80->options);
	free(sms->z80);
	psg_free(sms->psg);
	free(sms);
}

static uint16_t get_open_bus_value(system_header *system)
{
	return 0xFFFF;
}

static void request_exit(system_header *system)
{
	sms_context *sms = (sms_context *)system;
	sms->should_return = 1;
}

static void inc_debug_mode(system_header *system)
{
	sms_context *sms = (sms_context *)system;
	sms->vdp->debug++;
	if (sms->vdp->debug == 7) {
		sms->vdp->debug = 0;
	}
}

static void inc_debug_pal(system_header *system)
{
	sms_context *sms = (sms_context *)system;
	sms->vdp->debug_pal++;
	if (sms->vdp->debug_pal == 4) {
		sms->vdp->debug_pal = 0;
	}
}

static void load_save(system_header *system)
{
	//TODO: Implement me
}

static void persist_save(system_header *system)
{
	//TODO: Implement me
}

sms_context *alloc_configure_sms(system_media *media, uint32_t opts, uint8_t force_region, rom_info *info_out)
{
	memset(info_out, 0, sizeof(*info_out));
	sms_context *sms = calloc(1, sizeof(sms_context));
	uint32_t rom_size = nearest_pow2(media->size);
	memmap_chunk memory_map[6];
	if (media->size > 0xC000)  {
		info_out->map_chunks = 6;
		uint8_t *ram_reg_overlap = sms->ram + sizeof(sms->ram) - 4;
		memory_map[0] = (memmap_chunk){0x0000, 0x0400,  0xFFFF,             0, 0, MMAP_READ,                        media->buffer, NULL, NULL, NULL, NULL};
		memory_map[1] = (memmap_chunk){0x0400, 0x4000,  0xFFFF,             0, 0, MMAP_READ|MMAP_PTR_IDX|MMAP_CODE, NULL,     NULL, NULL, NULL, NULL};
		memory_map[2] = (memmap_chunk){0x4000, 0x8000,  0x3FFF,             0, 1, MMAP_READ|MMAP_PTR_IDX|MMAP_CODE, NULL,     NULL, NULL, NULL, NULL};
		memory_map[3] = (memmap_chunk){0x8000, 0xC000,  0x3FFF,             0, 2, MMAP_READ|MMAP_PTR_IDX|MMAP_CODE, NULL,     NULL, NULL, NULL, cart_ram_write};
		memory_map[4] = (memmap_chunk){0xC000, 0xFFFC,  sizeof(sms->ram)-1, 0, 0, MMAP_READ|MMAP_WRITE|MMAP_CODE,   sms->ram, NULL, NULL, NULL, NULL};
		memory_map[5] = (memmap_chunk){0xFFFC, 0x10000, 0x0003,             0, 0, MMAP_READ,                        ram_reg_overlap, NULL, NULL, NULL, mapper_write};
	} else {
		info_out->map_chunks = 2;
		memory_map[0] = (memmap_chunk){0x0000, 0xC000,  rom_size-1,         0, 0, MMAP_READ,                      media->buffer,  NULL, NULL, NULL, NULL};
		memory_map[1] = (memmap_chunk){0xC000, 0x10000, sizeof(sms->ram)-1, 0, 0, MMAP_READ|MMAP_WRITE|MMAP_CODE, sms->ram, NULL, NULL, NULL, NULL};
	};
	info_out->map = malloc(sizeof(memmap_chunk) * info_out->map_chunks);
	memcpy(info_out->map, memory_map, sizeof(memmap_chunk) * info_out->map_chunks);
	z80_options *zopts = malloc(sizeof(z80_options));
	init_z80_opts(zopts, info_out->map, info_out->map_chunks, io_map, 4, 15, 0xFF);
	sms->z80 = init_z80_context(zopts);
	sms->z80->system = sms;
	sms->z80->options->gen.debug_cmd_handler = debug_commands;
	
	sms->rom = media->buffer;
	sms->rom_size = rom_size;
	if (info_out->map_chunks > 2) {
		sms->z80->mem_pointers[0] = sms->rom;
		sms->z80->mem_pointers[1] = sms->rom + 0x4000;
		sms->z80->mem_pointers[2] = sms->rom + 0x8000;
		sms->bank_regs[1] = 0;
		sms->bank_regs[2] = 0x4000 >> 14;
		sms->bank_regs[3] = 0x8000 >> 14;
	}
	
	char * lowpass_cutoff_str = tern_find_path(config, "audio\0lowpass_cutoff\0", TVAL_PTR).ptrval;
	uint32_t lowpass_cutoff = lowpass_cutoff_str ? atoi(lowpass_cutoff_str) : 3390;
	
	//TODO: Detect region and pick master clock based off of that
	sms->normal_clock = sms->master_clock = 53693175;
	
	sms->psg = malloc(sizeof(psg_context));
	psg_init(sms->psg, render_sample_rate(), sms->master_clock, 15*16, render_audio_buffer(), lowpass_cutoff);
	
	sms->vdp = malloc(sizeof(vdp_context));
	init_vdp_context(sms->vdp, 0);
	sms->vdp->system = &sms->header;
	
	info_out->save_type = SAVE_NONE;
	info_out->name = strdup(media->name);
	
	setup_io_devices(config, info_out, &sms->io);
	
	sms->header.set_speed_percent = set_speed_percent;
	sms->header.start_context = start_sms;
	sms->header.resume_context = resume_sms;
	sms->header.load_save = load_save;
	sms->header.persist_save = persist_save;
	sms->header.load_state = load_state;
	sms->header.free_context = free_sms;
	sms->header.get_open_bus_value = get_open_bus_value;
	sms->header.request_exit = request_exit;
	sms->header.soft_reset = soft_reset;
	sms->header.inc_debug_mode = inc_debug_mode;
	sms->header.inc_debug_pal = inc_debug_pal;
	sms->header.type = SYSTEM_SMS;
	
	return sms;
}