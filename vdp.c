/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "vdp.h"
#include "blastem.h"
#include "genesis.h"
#include <stdlib.h>
#include <string.h>
#include "render.h"
#include "util.h"

#define NTSC_INACTIVE_START 224
#define PAL_INACTIVE_START 240
#define MODE4_INACTIVE_START 192
#define BUF_BIT_PRIORITY 0x40
#define MAP_BIT_PRIORITY 0x8000
#define MAP_BIT_H_FLIP 0x800
#define MAP_BIT_V_FLIP 0x1000

#define SCROLL_BUFFER_SIZE 32
#define SCROLL_BUFFER_MASK (SCROLL_BUFFER_SIZE-1)
#define SCROLL_BUFFER_DRAW (SCROLL_BUFFER_SIZE/2)

#define MCLKS_SLOT_H40  16
#define MCLKS_SLOT_H32  20
#define VINT_SLOT_H40  0 //21 slots before HSYNC, 16 during, 10 after
#define VINT_SLOT_H32  0  //old value was 23, but recent tests suggest the actual value is close to the H40 one
#define VINT_SLOT_MODE4 4
#define HSYNC_SLOT_H40  230
#define HSYNC_END_H40  (HSYNC_SLOT_H40+17)
#define HBLANK_START_H40 178 //should be 179 according to Nemesis, but 178 seems to fit slightly better with my test ROM results
#define HBLANK_END_H40  0 //should be 5.5 according to Nemesis, but 0 seems to fit better with my test ROM results
#define HBLANK_START_H32 233 //should be 147 according to Nemesis which is very different from my test ROM result
#define HBLANK_END_H32 0 //should be 5 according to Nemesis, but 0 seems to fit better with my test ROM results
#define LINE_CHANGE_H40 165
#define LINE_CHANGE_H32 133
#define LINE_CHANGE_MODE4 249
#define VBLANK_START_H40 (LINE_CHANGE_H40+2)
#define VBLANK_START_H32 (LINE_CHANGE_H32+2)
#define FIFO_LATENCY    3

#define BORDER_TOP_V24     27
#define BORDER_TOP_V28     11
#define BORDER_TOP_V24_PAL 54
#define BORDER_TOP_V28_PAL 38
#define BORDER_TOP_V30_PAL 30

#define BORDER_BOT_V24     24
#define BORDER_BOT_V28     8
#define BORDER_BOT_V24_PAL 48
#define BORDER_BOT_V28_PAL 32
#define BORDER_BOT_V30_PAL 24

#define INVALID_LINE 0x200

enum {
	INACTIVE = 0,
	PREPARING, //used for line 0x1FF
	ACTIVE
};

static int32_t color_map[1 << 12];
static uint16_t mode4_address_map[0x4000];
static uint32_t planar_to_chunky[256];
static uint8_t levels[] = {0, 27, 49, 71, 87, 103, 119, 130, 146, 157, 174, 190, 206, 228, 255};

static uint8_t debug_base[][3] = {
	{127, 127, 127}, //BG
	{0, 0, 127},     //A
	{127, 0, 0},     //Window
	{0, 127, 0},     //B
	{127, 0, 127}    //Sprites
};

static void update_video_params(vdp_context *context)
{
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		if (context->regs[REG_MODE_2] & BIT_PAL) {
			if (context->flags2 & FLAG2_REGION_PAL) {
				context->inactive_start = PAL_INACTIVE_START;
				context->border_top = BORDER_TOP_V30_PAL;
				context->border_bot = BORDER_BOT_V30_PAL;
			} else {
				//the behavior here is rather weird and needs more investigation
				context->inactive_start = 0xF0;
				context->border_top = 1;
				context->border_bot = 3;
			}
		} else {
			context->inactive_start = NTSC_INACTIVE_START;
			if (context->flags2 & FLAG2_REGION_PAL) {
				context->border_top = BORDER_TOP_V28_PAL;
				context->border_bot = BORDER_BOT_V28_PAL;
			} else {
				context->border_top = BORDER_TOP_V28;
				context->border_bot = BORDER_TOP_V28;
			}
		}
		if (context->regs[REG_MODE_4] & BIT_H40) {
			context->max_sprites_frame = MAX_SPRITES_FRAME;
			context->max_sprites_line = MAX_SPRITES_LINE;
		} else {
			context->max_sprites_frame = MAX_SPRITES_FRAME_H32;
			context->max_sprites_line = MAX_SPRITES_LINE_H32;
		}
		if (context->state == INACTIVE) {
			//Undo forced INACTIVE state due to neither Mode 4 nor Mode 5 being active
			if (context->vcounter < context->inactive_start) {
				context->state = ACTIVE;
			} else if (context->vcounter == 0x1FF) {
				context->state = PREPARING;
			}
		}
	} else {
		context->inactive_start = MODE4_INACTIVE_START;
		if (context->flags2 & FLAG2_REGION_PAL) {
			context->border_top = BORDER_TOP_V24_PAL;
			context->border_bot = BORDER_BOT_V24_PAL;
		} else {
			context->border_top = BORDER_TOP_V24;
			context->border_bot = BORDER_BOT_V24;
		}
		if (!(context->regs[REG_MODE_1] & BIT_MODE_4)){
			context->state = INACTIVE;
		} else if (context->state == INACTIVE) {
			//Undo forced INACTIVE state due to neither Mode 4 nor Mode 5 being active
			if (context->vcounter < context->inactive_start) {
				context->state = ACTIVE;
			}
			else if (context->vcounter == 0x1FF) {
				context->state = PREPARING;
			}
		}
	}
}

static uint8_t color_map_init_done;

void init_vdp_context(vdp_context * context, uint8_t region_pal)
{
	memset(context, 0, sizeof(*context));
	context->vdpmem = malloc(VRAM_SIZE);
	memset(context->vdpmem, 0, VRAM_SIZE);
	/*
	*/
	if (headless) {
		context->output = malloc(LINEBUF_SIZE * sizeof(uint32_t));
		context->output_pitch = 0;
	} else {
		context->cur_buffer = FRAMEBUFFER_ODD;
		context->fb = render_get_framebuffer(FRAMEBUFFER_ODD, &context->output_pitch);
	}
	context->linebuf = malloc(LINEBUF_SIZE + SCROLL_BUFFER_SIZE*2);
	memset(context->linebuf, 0, LINEBUF_SIZE + SCROLL_BUFFER_SIZE*2);
	context->tmp_buf_a = context->linebuf + LINEBUF_SIZE;
	context->tmp_buf_b = context->tmp_buf_a + SCROLL_BUFFER_SIZE;
	context->sprite_draws = MAX_DRAWS;
	context->fifo_write = 0;
	context->fifo_read = -1;
	context->regs[REG_HINT] = context->hint_counter = 0xFF;

	if (!color_map_init_done) {
		uint8_t b,g,r;
		for (uint16_t color = 0; color < (1 << 12); color++) {
			if (color & FBUF_SHADOW) {
				b = levels[(color >> 9) & 0x7];
				g = levels[(color >> 5) & 0x7];
				r = levels[(color >> 1) & 0x7];
			} else if(color & FBUF_HILIGHT) {
				b = levels[((color >> 9) & 0x7) + 7];
				g = levels[((color >> 5) & 0x7) + 7];
				r = levels[((color >> 1) & 0x7) + 7];
			} else if(color & FBUF_MODE4) {
				b = levels[(color >> 4 & 0xC) | (color >> 6 & 0x2)];
				g = levels[(color >> 2 & 0x8) | (color >> 1 & 0x4) | (color >> 4 & 0x2)];
				r = levels[(color << 1 & 0xC) | (color >> 1 & 0x2)];
			} else {
				b = levels[(color >> 8) & 0xE];
				g = levels[(color >> 4) & 0xE];
				r = levels[color & 0xE];
			}
			color_map[color] = render_map_color(r, g, b);
		}
		for (uint16_t mode4_addr = 0; mode4_addr < 0x4000; mode4_addr++)
		{
			uint16_t mode5_addr = mode4_addr & 0x3DFD;
			mode5_addr |= mode4_addr << 8 & 0x200;
			mode5_addr |= mode4_addr >> 8 & 2;
			mode4_address_map[mode4_addr] = mode5_addr;
		}
		for (uint32_t planar = 0; planar < 256; planar++)
		{
			uint32_t chunky = 0;
			for (int bit = 7; bit >= 0; bit--)
			{
				chunky = chunky << 4;
				chunky |= planar >> bit & 1;
			}
			planar_to_chunky[planar] = chunky;
		}
		color_map_init_done = 1;
	}
	for (uint8_t color = 0; color < (1 << (3 + 1 + 1 + 1)); color++)
	{
		uint8_t src = color & DBG_SRC_MASK;
		if (src > DBG_SRC_S) {
			context->debugcolors[color] = 0;
		} else {
			uint8_t r,g,b;
			b = debug_base[src][0];
			g = debug_base[src][1];
			r = debug_base[src][2];
			if (color & DBG_PRIORITY)
			{
				if (b) {
					b += 48;
				}
				if (g) {
					g += 48;
				}
				if (r) {
					r += 48;
				}
			}
			if (color & DBG_SHADOW) {
				b /= 2;
				g /= 2;
				r /=2 ;
			}
			if (color & DBG_HILIGHT) {
				if (b) {
					b += 72;
				}
				if (g) {
					g += 72;
				}
				if (r) {
					r += 72;
				}
			}
			context->debugcolors[color] = render_map_color(r, g, b);
		}
	}
	if (region_pal) {
		context->flags2 |= FLAG2_REGION_PAL;
	}
	update_video_params(context);
	if (!headless) {
		context->output = (uint32_t *)(((char *)context->fb) + context->output_pitch * context->border_top);
	}
}

void vdp_free(vdp_context *context)
{
	free(context->vdpmem);
	free(context->linebuf);
	free(context);
}

static int is_refresh(vdp_context * context, uint32_t slot)
{
	if (context->regs[REG_MODE_4] & BIT_H40) {
		return slot == 250 || slot == 26 || slot == 59 || slot == 90 || slot == 122 || slot == 154;
	} else {
		//TODO: Figure out which slots are refresh when display is off in 32-cell mode
		//These numbers are guesses based on H40 numbers
		return slot == 243 || slot == 19 || slot == 51 || slot == 83 || slot == 115;
		//The numbers below are the refresh slots during active display
		//return (slot == 29 || slot == 61 || slot == 93 || slot == 125);
	}
}

static void increment_address(vdp_context *context)
{
	context->address += context->regs[REG_AUTOINC];
	if (!(context->regs[REG_MODE_2] & BIT_MODE_5)) {
		context->address++;
	}
}

static void render_sprite_cells(vdp_context * context)
{
	sprite_draw * d = context->sprite_draw_list + context->cur_slot;
	context->serial_address = d->address;
	if (context->cur_slot >= context->sprite_draws) {

		uint16_t dir;
		int16_t x;
		if (d->h_flip) {
			x = d->x_pos + 7;
			dir = -1;
		} else {
			x = d->x_pos;
			dir = 1;
		}
		//printf("Draw Slot %d of %d, Rendering sprite cell from %X to x: %d\n", context->cur_slot, context->sprite_draws, d->address, x);
		context->cur_slot--;
		for (uint16_t address = d->address; address != ((d->address+4) & 0xFFFF); address++) {
			if (x >= 0 && x < 320) {
				if (!(context->linebuf[x] & 0xF)) {
					context->linebuf[x] = (context->vdpmem[address] >> 4) | d->pal_priority;
				} else if (context->vdpmem[address] >> 4) {
					context->flags2 |= FLAG2_SPRITE_COLLIDE;
				}
			}
			x += dir;
			if (x >= 0 && x < 320) {
				if (!(context->linebuf[x] & 0xF)) {
					context->linebuf[x] = (context->vdpmem[address] & 0xF)  | d->pal_priority;
				} else if (context->vdpmem[address] & 0xF) {
					context->flags2 |= FLAG2_SPRITE_COLLIDE;
				}
			}
			x += dir;
		}
	} else {
		context->cur_slot--;
	}
}

static void fetch_sprite_cells_mode4(vdp_context * context)
{
	if (context->sprite_index >= context->sprite_draws) {
		sprite_draw * d = context->sprite_draw_list + context->sprite_index;
		uint32_t address = mode4_address_map[d->address & 0x3FFF];
		context->fetch_tmp[0] = context->vdpmem[address];
		context->fetch_tmp[1] = context->vdpmem[address + 1];
	}
}

static void render_sprite_cells_mode4(vdp_context * context)
{
	if (context->sprite_index >= context->sprite_draws) {
		sprite_draw * d = context->sprite_draw_list + context->sprite_index;
		uint32_t pixels = planar_to_chunky[context->fetch_tmp[0]] << 1;
		pixels |= planar_to_chunky[context->fetch_tmp[1]];
		uint32_t address = mode4_address_map[(d->address + 2) & 0x3FFF];
		pixels |= planar_to_chunky[context->vdpmem[address]] << 3;
		pixels |= planar_to_chunky[context->vdpmem[address + 1]] << 2;
		int x = d->x_pos & 0xFF;
		for (int i = 28; i >= 0; i -= 4, x++)
		{
			if (context->linebuf[x] && (pixels >> i & 0xF)) {
				if (
					((context->regs[REG_MODE_1] & BIT_SPRITE_8PX) && x > 8)
					|| ((!(context->regs[REG_MODE_1] & BIT_SPRITE_8PX)) && x < 256)
				) {
					context->flags2 |= FLAG2_SPRITE_COLLIDE;
				}
			} else {
				context->linebuf[x] = pixels >> i & 0xF;
			}
		}
		context->sprite_index--;
	}
}

static uint32_t mode5_sat_address(vdp_context *context)
{
	uint32_t addr = context->regs[REG_SAT] << 9;
	if (!(context->regs[REG_MODE_2] & BIT_128K_VRAM)) {
		addr &= 0xFFFF;
	}
	if (context->regs[REG_MODE_4] & BIT_H40) {
		addr &= 0x1FC00;
	}
	return addr;
}

void vdp_print_sprite_table(vdp_context * context)
{
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		uint16_t sat_address = mode5_sat_address(context);
		uint16_t current_index = 0;
		uint8_t count = 0;
		do {
			uint16_t address = current_index * 8 + sat_address;
			uint16_t cache_address = current_index * 4;
			uint8_t height = ((context->sat_cache[cache_address+2] & 0x3) + 1) * 8;
			uint8_t width = (((context->sat_cache[cache_address+2]  >> 2) & 0x3) + 1) * 8;
			int16_t y = ((context->sat_cache[cache_address] & 0x3) << 8 | context->sat_cache[cache_address+1]) & 0x1FF;
			int16_t x = ((context->vdpmem[address+ 6] & 0x3) << 8 | context->vdpmem[address + 7]) & 0x1FF;
			uint16_t link = context->sat_cache[cache_address+3] & 0x7F;
			uint8_t pal = context->vdpmem[address + 4] >> 5 & 0x3;
			uint8_t pri = context->vdpmem[address + 4] >> 7;
			uint16_t pattern = ((context->vdpmem[address + 4] << 8 | context->vdpmem[address + 5]) & 0x7FF) << 5;
			printf("Sprite %d: X=%d(%d), Y=%d(%d), Width=%u, Height=%u, Link=%u, Pal=%u, Pri=%u, Pat=%X\n", current_index, x, x-128, y, y-128, width, height, link, pal, pri, pattern);
			current_index = link;
			count++;
		} while (current_index != 0 && count < 80);
	} else {
		uint16_t sat_address = (context->regs[REG_SAT] & 0x7E) << 7;
		for (int i = 0; i < 64; i++)
		{
			uint8_t y = context->vdpmem[mode4_address_map[sat_address + (i ^ 1)]];
			if (y == 0xD0) {
				break;
			}
			uint8_t x = context->vdpmem[mode4_address_map[sat_address + 0x80 + i*2 + 1]];
			uint16_t tile_address = context->vdpmem[mode4_address_map[sat_address + 0x80 + i*2]] * 32
				+ (context->regs[REG_STILE_BASE] << 11 & 0x2000);
			if (context->regs[REG_MODE_2] & BIT_SPRITE_SZ) {
				tile_address &= ~32;
			}
			printf("Sprite %d: X=%d, Y=%d, Pat=%X\n", i, x, y, tile_address);
		}
	}
}

#define VRAM_READ 0 //0000
#define VRAM_WRITE 1 //0001
//2 would trigger register write 0010
#define CRAM_WRITE 3 //0011
#define VSRAM_READ 4 //0100
#define VSRAM_WRITE 5//0101
//6 would trigger regsiter write 0110
//7 is a mystery //0111
#define CRAM_READ 8  //1000
//9 is also a mystery //1001
//A would trigger register write 1010
//B is a mystery 1011
#define VRAM_READ8 0xC //1100
//D is a mystery 1101
//E would trigger register write 1110
//F is a mystery 1111

//Possible theory on how bits work
//CD0 = Read/Write flag
//CD2,(CD1|CD3) = RAM type
//  00 = VRAM
//  01 = CRAM
//  10 = VSRAM
//  11 = VRAM8
//Would result in
//  7 = VRAM8 write
//  9 = CRAM write alias
//  B = CRAM write alias
//  D = VRAM8 write alias
//  F = VRAM8 write alais

#define DMA_START 0x20

static const char * cd_name(uint8_t cd)
{
	switch (cd & 0xF)
	{
	case VRAM_READ:
		return "VRAM read";
	case VRAM_WRITE:
		return "VRAM write";
	case CRAM_WRITE:
		return "CRAM write";
	case VSRAM_READ:
		return "VSRAM read";
	case VSRAM_WRITE:
		return "VSRAM write";
	case VRAM_READ8:
		return "VRAM read (undocumented 8-bit mode)";
	default:
		return "invalid";
	}
}

void vdp_print_reg_explain(vdp_context * context)
{
	char * hscroll[] = {"full", "7-line", "cell", "line"};
	printf("**Mode Group**\n"
	       "00: %.2X | H-ints %s, Pal Select %d, HVC latch %s, Display gen %s\n"
	       "01: %.2X | Display %s, V-ints %s, Height: %d, Mode %d, %dK VRAM\n"
	       "0B: %.2X | E-ints %s, V-Scroll: %s, H-Scroll: %s\n"
	       "0C: %.2X | Width: %d, Shadow/Highlight: %s\n",
	       context->regs[REG_MODE_1], context->regs[REG_MODE_1] & BIT_HINT_EN ? "enabled" : "disabled", (context->regs[REG_MODE_1] & BIT_PAL_SEL) != 0,
	           context->regs[REG_MODE_1] & BIT_HVC_LATCH ? "enabled" : "disabled", context->regs[REG_MODE_1] & BIT_DISP_DIS ? "disabled" : "enabled",
	       context->regs[REG_MODE_2], context->regs[REG_MODE_2] & BIT_DISP_EN ? "enabled" : "disabled", context->regs[REG_MODE_2] & BIT_VINT_EN ? "enabled" : "disabled",
	           context->regs[REG_MODE_2] & BIT_PAL ? 30 : 28, context->regs[REG_MODE_2] & BIT_MODE_5 ? 5 : 4, context->regs[REG_MODE_1] & BIT_128K_VRAM ? 128 : 64, 
	       context->regs[REG_MODE_3], context->regs[REG_MODE_3] & BIT_EINT_EN ? "enabled" : "disabled", context->regs[REG_MODE_3] & BIT_VSCROLL ? "2 cell" : "full",
	           hscroll[context->regs[REG_MODE_3] & 0x3],
	       context->regs[REG_MODE_4], context->regs[REG_MODE_4] & BIT_H40 ? 40 : 32, context->regs[REG_MODE_4] & BIT_HILIGHT ? "enabled" : "disabled");
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		printf("\n**Table Group**\n"
			   "02: %.2X | Scroll A Name Table:    $%.4X\n"
			   "03: %.2X | Window Name Table:      $%.4X\n"
			   "04: %.2X | Scroll B Name Table:    $%.4X\n"
			   "05: %.2X | Sprite Attribute Table: $%.4X\n"
			   "0D: %.2X | HScroll Data Table:     $%.4X\n",
			   context->regs[REG_SCROLL_A], (context->regs[REG_SCROLL_A] & 0x38) << 10,
			   context->regs[REG_WINDOW], (context->regs[REG_WINDOW] & (context->regs[REG_MODE_4] & BIT_H40 ? 0x3C : 0x3E)) << 10,
			   context->regs[REG_SCROLL_B], (context->regs[REG_SCROLL_B] & 0x7) << 13,
			   context->regs[REG_SAT], mode5_sat_address(context),
			   context->regs[REG_HSCROLL], (context->regs[REG_HSCROLL] & 0x3F) << 10);
	} else {
		printf("\n**Table Group**\n"
			   "02: %.2X | Background Name Table:  $%.4X\n"
			   "05: %.2X | Sprite Attribute Table: $%.4X\n"
			   "06: %.2X | Sprite Tile Base:       $%.4X\n"
			   "08: %.2X | Background X Scroll:    %d\n"
			   "09: %.2X | Background Y Scroll:    %d\n",
			   context->regs[REG_SCROLL_A], (context->regs[REG_SCROLL_A] & 0xE) << 10,
			   context->regs[REG_SAT], (context->regs[REG_SAT] & 0x7E) << 7,
			   context->regs[REG_STILE_BASE], (context->regs[REG_STILE_BASE] & 2) << 11,
			   context->regs[REG_X_SCROLL], context->regs[REG_X_SCROLL],
			   context->regs[REG_Y_SCROLL], context->regs[REG_Y_SCROLL]);
			   
	}
	char * sizes[] = {"32", "64", "invalid", "128"};
	printf("\n**Misc Group**\n"
	       "07: %.2X | Backdrop Color: $%X\n"
	       "0A: %.2X | H-Int Counter: %u\n"
	       "0F: %.2X | Auto-increment: $%X\n"
	       "10: %.2X | Scroll A/B Size: %sx%s\n",
	       context->regs[REG_BG_COLOR], context->regs[REG_BG_COLOR],
	       context->regs[REG_HINT], context->regs[REG_HINT],
	       context->regs[REG_AUTOINC], context->regs[REG_AUTOINC],
	       context->regs[REG_SCROLL], sizes[context->regs[REG_SCROLL] & 0x3], sizes[context->regs[REG_SCROLL] >> 4 & 0x3]);
	char * src_types[] = {"68K", "68K", "Copy", "Fill"};
	printf("\n**DMA Group**\n"
	       "13: %.2X |\n"
		   "14: %.2X | DMA Length: $%.4X words\n"
		   "15: %.2X |\n"
		   "16: %.2X |\n"
		   "17: %.2X | DMA Source Address: $%.6X, Type: %s\n",
		   context->regs[REG_DMALEN_L],
		   context->regs[REG_DMALEN_H], context->regs[REG_DMALEN_H] << 8 | context->regs[REG_DMALEN_L],
		   context->regs[REG_DMASRC_L],
		   context->regs[REG_DMASRC_M],
		   context->regs[REG_DMASRC_H],
		       context->regs[REG_DMASRC_H] << 17 | context->regs[REG_DMASRC_M] << 9 | context->regs[REG_DMASRC_L] << 1,
			   src_types[context->regs[REG_DMASRC_H] >> 6 & 3]);
	printf("\n**Internal Group**\n"
	       "Address: %X\n"
	       "CD:      %X - %s\n"
	       "Pending: %s\n"
		   "VCounter: %d\n"
		   "HCounter: %d\n"
		   "VINT Pending: %s\n"
		   "HINT Pending: %s\n"
		   "Status: %X\n",
	       context->address, context->cd, cd_name(context->cd), 
		   (context->flags & FLAG_PENDING) ? "word" : (context->flags2 & FLAG2_BYTE_PENDING) ? "byte" : "none",
		   context->vcounter, context->hslot*2, (context->flags2 & FLAG2_VINT_PENDING) ? "true" : "false",
		   (context->flags2 & FLAG2_HINT_PENDING) ? "true" : "false", vdp_control_port_read(context));

	//TODO: Window Group, DMA Group
}

static uint8_t is_active(vdp_context *context)
{
	return context->state != INACTIVE && (context->regs[REG_MODE_2] & BIT_DISP_EN) != 0;
}

static void scan_sprite_table(uint32_t line, vdp_context * context)
{
	if (context->sprite_index && ((uint8_t)context->slot_counter) < context->max_sprites_line) {
		line += 1;
		uint16_t ymask, ymin;
		uint8_t height_mult;
		if (context->double_res) {
			line *= 2;
			if (context->flags2 & FLAG2_EVEN_FIELD) {
				line++;
			}
			ymask = 0x3FF;
			ymin = 256;
			height_mult = 16;
		} else {
			ymask = 0x1FF;
			ymin = 128;
			height_mult = 8;
		}
		context->sprite_index &= 0x7F;
		//TODO: Implement squirelly behavior documented by Kabuto
		if (context->sprite_index >= context->max_sprites_frame) {
			context->sprite_index = 0;
			return;
		}
		uint16_t address = context->sprite_index * 4;
		line += ymin;
		line &= ymask;
		uint16_t y = ((context->sat_cache[address] & 0x3) << 8 | context->sat_cache[address+1]) & ymask;
		uint8_t height = ((context->sat_cache[address+2] & 0x3) + 1) * height_mult;
		//printf("Sprite %d | y: %d, height: %d\n", context->sprite_index, y, height);
		if (y <= line && line < (y + height)) {
			//printf("Sprite %d at y: %d with height %d is on line %d\n", context->sprite_index, y, height, line);
			context->sprite_info_list[context->slot_counter].size = context->sat_cache[address+2];
			context->sprite_info_list[context->slot_counter++].index = context->sprite_index;
		}
		context->sprite_index = context->sat_cache[address+3] & 0x7F;
		if (context->sprite_index && ((uint8_t)context->slot_counter) < context->max_sprites_line)
		{
			//TODO: Implement squirelly behavior documented by Kabuto
			if (context->sprite_index >= context->max_sprites_frame) {
				context->sprite_index = 0;
				return;
			}
			address = context->sprite_index * 4;
			y = ((context->sat_cache[address] & 0x3) << 8 | context->sat_cache[address+1]) & ymask;
			height = ((context->sat_cache[address+2] & 0x3) + 1) * height_mult;
			//printf("Sprite %d | y: %d, height: %d\n", context->sprite_index, y, height);
			if (y <= line && line < (y + height)) {
				//printf("Sprite %d at y: %d with height %d is on line %d\n", context->sprite_index, y, height, line);
				context->sprite_info_list[context->slot_counter].size = context->sat_cache[address+2];
				context->sprite_info_list[context->slot_counter++].index = context->sprite_index;
			}
			context->sprite_index = context->sat_cache[address+3] & 0x7F;
		}
	}
	//TODO: Seems like the overflow flag should be set here if we run out of sprite info slots without hitting the end of the list
}

static void scan_sprite_table_mode4(vdp_context * context)
{
	if (context->sprite_index < MAX_SPRITES_FRAME_H32) {
		uint32_t line = context->vcounter;
		line &= 0xFF;
		
		uint32_t sat_address = mode4_address_map[(context->regs[REG_SAT] << 7 & 0x3F00) + context->sprite_index];
		uint32_t y = context->vdpmem[sat_address+1];
		uint32_t size = (context->regs[REG_MODE_2] & BIT_SPRITE_SZ) ? 16 : 8;
		
		if (y == 0xd0) {
			context->sprite_index = MAX_SPRITES_FRAME_H32;
			return;
		} else {
			if (y <= line && line < (y + size)) {
				if (!context->slot_counter) {
					context->sprite_index = MAX_SPRITES_FRAME_H32;
					context->flags |= FLAG_DOT_OFLOW;
					return;
				}
				context->sprite_info_list[--(context->slot_counter)].size = size;
				context->sprite_info_list[context->slot_counter].index = context->sprite_index;
				context->sprite_info_list[context->slot_counter].y = y;
			}
			context->sprite_index++;
		}
		
		if (context->sprite_index < MAX_SPRITES_FRAME_H32) {
			y = context->vdpmem[sat_address];
			if (y == 0xd0) {
				context->sprite_index = MAX_SPRITES_FRAME_H32;
				return;
			} else {
				if (y <= line && line < (y + size)) {
					if (!context->slot_counter) {
						context->sprite_index = MAX_SPRITES_FRAME_H32;
						context->flags |= FLAG_DOT_OFLOW;
						return;
					}
					context->sprite_info_list[--(context->slot_counter)].size = size;
					context->sprite_info_list[context->slot_counter].index = context->sprite_index;
					context->sprite_info_list[context->slot_counter].y = y;
				}
				context->sprite_index++;
			}
		}
		
	}
}

static void read_sprite_x(uint32_t line, vdp_context * context)
{
	if (context->cur_slot == context->max_sprites_line) {
		context->cur_slot = 0;
	}
	if (context->cur_slot < context->slot_counter) {
		if (context->sprite_draws) {
			line += 1;
			//in tiles
			uint8_t width = ((context->sprite_info_list[context->cur_slot].size >> 2) & 0x3) + 1;
			//in pixels
			uint8_t height = ((context->sprite_info_list[context->cur_slot].size & 0x3) + 1) * 8;
			if (context->double_res) {
				line *= 2;
				if (context->flags2 & FLAG2_EVEN_FIELD) {
					line++;
				}
				height *= 2;
			}
			uint16_t ymask, ymin;
			if (context->double_res) {
				ymask = 0x3FF;
				ymin = 256;
			} else {
				ymask = 0x1FF;
				ymin = 128;
			}
			uint16_t att_addr = mode5_sat_address(context) + context->sprite_info_list[context->cur_slot].index * 8 + 4;
			uint16_t tileinfo = (context->vdpmem[att_addr] << 8) | context->vdpmem[att_addr+1];
			uint8_t pal_priority = (tileinfo >> 9) & 0x70;
			uint8_t row;
			uint16_t cache_addr = context->sprite_info_list[context->cur_slot].index * 4;
			line = (line + ymin) & ymask;
			int16_t y = ((context->sat_cache[cache_addr] << 8 | context->sat_cache[cache_addr+1]) & ymask)/* - ymin*/;
			if (tileinfo & MAP_BIT_V_FLIP) {
				row = (y + height - 1) - line;
			} else {
				row = line-y;
			}
			row &= ymask >> 4;
			uint16_t address;
			if (context->double_res) {
				address = ((tileinfo & 0x3FF) << 6) + row * 4;
			} else {
				address = ((tileinfo & 0x7FF) << 5) + row * 4;
			}
			int16_t x = ((context->vdpmem[att_addr+ 2] & 0x3) << 8 | context->vdpmem[att_addr + 3]) & 0x1FF;
			if (x) {
				context->flags |= FLAG_CAN_MASK;
			} else if(context->flags & (FLAG_CAN_MASK | FLAG_DOT_OFLOW)) {
				context->flags |= FLAG_MASKED;
			}

			context->flags &= ~FLAG_DOT_OFLOW;
			int16_t i;
			if (context->flags & FLAG_MASKED) {
				for (i=0; i < width && context->sprite_draws; i++) {
					--context->sprite_draws;
					context->sprite_draw_list[context->sprite_draws].x_pos = -128;
					context->sprite_draw_list[context->sprite_draws].address = address + i * height * 4;
				}
			} else {
				x -= 128;
				int16_t base_x = x;
				int16_t dir;
				if (tileinfo & MAP_BIT_H_FLIP) {
					x += (width-1) * 8;
					dir = -8;
				} else {
					dir = 8;
				}
				//printf("Sprite %d | x: %d, y: %d, width: %d, height: %d, pal_priority: %X, row: %d, tile addr: %X\n", context->sprite_info_list[context->cur_slot].index, x, context->sprite_info_list[context->cur_slot].y, width, height, pal_priority, row, address);
				for (i=0; i < width && context->sprite_draws; i++, x += dir) {
					--context->sprite_draws;
					context->sprite_draw_list[context->sprite_draws].address = address + i * height * 4;
					context->sprite_draw_list[context->sprite_draws].x_pos = x;
					context->sprite_draw_list[context->sprite_draws].pal_priority = pal_priority;
					context->sprite_draw_list[context->sprite_draws].h_flip = (tileinfo & MAP_BIT_H_FLIP) ? 1 : 0;
				}
			}
			//Used to be i < width
			//TODO: Confirm this is the right condition on hardware
			if (!context->sprite_draws) {
				context->flags |= FLAG_DOT_OFLOW;
			}
		} else {
			context->flags |= FLAG_DOT_OFLOW;
		}
	}
	context->cur_slot++;
}

static void read_sprite_x_mode4(vdp_context * context)
{
	if (context->cur_slot >= context->slot_counter) {
		uint32_t address = (context->regs[REG_SAT] << 7 & 0x3F00) + 0x80 + context->sprite_info_list[context->cur_slot].index * 2;
		address = mode4_address_map[address];
		--context->sprite_draws;
		uint32_t tile_address = context->vdpmem[address] * 32 + (context->regs[REG_STILE_BASE] << 11 & 0x2000);
		if (context->regs[REG_MODE_2] & BIT_SPRITE_SZ) {
			tile_address &= ~32;
		}
		tile_address += (context->vcounter - context->sprite_info_list[context->cur_slot].y)* 4;
		context->sprite_draw_list[context->sprite_draws].x_pos = context->vdpmem[address + 1];
		context->sprite_draw_list[context->sprite_draws].address = tile_address;
		context->cur_slot--;
	}
}

#define CRAM_BITS 0xEEE
#define VSRAM_BITS 0x7FF
#define VSRAM_DIRTY_BITS 0xF800

//rough estimate of slot number at which border display starts
#define BG_START_SLOT 6

static void update_color_map(vdp_context *context, uint16_t index, uint16_t value)
{
	context->colors[index] = color_map[value & CRAM_BITS];
	context->colors[index + CRAM_SIZE] = color_map[(value & CRAM_BITS) | FBUF_SHADOW];
	context->colors[index + CRAM_SIZE*2] = color_map[(value & CRAM_BITS) | FBUF_HILIGHT];
	context->colors[index + CRAM_SIZE*3] = color_map[(value & CRAM_BITS) | FBUF_MODE4];
}

void write_cram_internal(vdp_context * context, uint16_t addr, uint16_t value)
{
	context->cram[addr] = value;
	update_color_map(context, addr, value);
}

static void write_cram(vdp_context * context, uint16_t address, uint16_t value)
{
	uint16_t addr;
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		addr = (address/2) & (CRAM_SIZE-1);
	} else {
		addr = address & 0x1F;
		value = (value << 1 & 0xE) | (value << 2 & 0xE0) | (value & 0xE00);
	}
	write_cram_internal(context, addr, value);
	
	if (context->hslot >= BG_START_SLOT && (
		context->vcounter < context->inactive_start + context->border_bot 
		|| context->vcounter > 0x200 - context->border_top
	)) {
		uint8_t bg_end_slot = BG_START_SLOT + (context->regs[REG_MODE_4] & BIT_H40) ? LINEBUF_SIZE/2 : (256+HORIZ_BORDER)/2;
		if (context->hslot < bg_end_slot) {
			uint32_t color = (context->regs[REG_MODE_2] & BIT_MODE_5) ? context->colors[addr] : context->colors[addr + CRAM_SIZE*3];
			context->output[(context->hslot - BG_START_SLOT)*2 + 1] = color;
		}
	}
}

static void vdp_advance_dma(vdp_context * context)
{
	context->regs[REG_DMASRC_L] += 1;
	if (!context->regs[REG_DMASRC_L]) {
		context->regs[REG_DMASRC_M] += 1;
	}
	context->address += context->regs[REG_AUTOINC];
	uint16_t dma_len = ((context->regs[REG_DMALEN_H] << 8) | context->regs[REG_DMALEN_L]) - 1;
	context->regs[REG_DMALEN_H] = dma_len >> 8;
	context->regs[REG_DMALEN_L] = dma_len;
	if (!dma_len) {
		context->flags &= ~FLAG_DMA_RUN;
		context->cd &= 0xF;
	}
}

static void vdp_check_update_sat(vdp_context *context, uint32_t address, uint16_t value)
{
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		if (!(address & 4)) {
			uint32_t sat_address = mode5_sat_address(context);
			if(address >= sat_address && address < (sat_address + SAT_CACHE_SIZE*2)) {
				uint16_t cache_address = address - sat_address;
				cache_address = (cache_address & 3) | (cache_address >> 1 & 0x1FC);
				context->sat_cache[cache_address] = value >> 8;
				context->sat_cache[cache_address^1] = value;
			}
		}
	}
}

void vdp_check_update_sat_byte(vdp_context *context, uint32_t address, uint8_t value)
{
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		if (!(address & 4)) {
			uint32_t sat_address = mode5_sat_address(context);
			if(address >= sat_address && address < (sat_address + SAT_CACHE_SIZE*2)) {
				uint16_t cache_address = address - sat_address;
				cache_address = (cache_address & 3) | (cache_address >> 1 & 0x1FC);
				context->sat_cache[cache_address] = value;
			}
		}
	}
}

static void write_vram_word(vdp_context *context, uint32_t address, uint16_t value)
{
	address = (address & 0x3FC) | (address >> 1 & 0xFC01) | (address >> 9 & 0x2);
	address ^= 1;
	//TODO: Support an option to actually have 128KB of VRAM
	context->vdpmem[address] = value;
}

static void write_vram_byte(vdp_context *context, uint32_t address, uint8_t value)
{
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		address &= 0xFFFF;
	} else {
		address = mode4_address_map[address & 0x3FFF];
	}
	context->vdpmem[address] = value;
}

static void external_slot(vdp_context * context)
{
	if ((context->flags & FLAG_DMA_RUN) && (context->regs[REG_DMASRC_H] & 0xC0) == 0x80 && context->fifo_read < 0) {
		context->fifo_read = (context->fifo_write-1) & (FIFO_SIZE-1);
		fifo_entry * cur = context->fifo + context->fifo_read;
		cur->cycle = context->cycles;
		cur->address = context->address;
		cur->partial = 2;
		vdp_advance_dma(context);
	}
	fifo_entry * start = context->fifo + context->fifo_read;
	if (context->fifo_read >= 0 && start->cycle <= context->cycles) {
		switch (start->cd & 0xF)
		{
		case VRAM_WRITE:
			if ((context->regs[REG_MODE_2] & (BIT_128K_VRAM|BIT_MODE_5)) == (BIT_128K_VRAM|BIT_MODE_5)) {
				vdp_check_update_sat(context, start->address, start->value);
				write_vram_word(context, start->address, start->value);
			} else if (start->partial) {
				//printf("VRAM Write: %X to %X at %d (line %d, slot %d)\n", start->value, start->address ^ 1, context->cycles, context->cycles/MCLKS_LINE, (context->cycles%MCLKS_LINE)/16);
				uint8_t byte = start->partial == 2 ? start->value >> 8 : start->value;
				if (start->partial > 1) {
					vdp_check_update_sat_byte(context, start->address ^ 1, byte);
				}
				write_vram_byte(context, start->address ^ 1, byte);
			} else {
				//printf("VRAM Write High: %X to %X at %d (line %d, slot %d)\n", start->value >> 8, start->address, context->cycles, context->cycles/MCLKS_LINE, (context->cycles%MCLKS_LINE)/16);
				vdp_check_update_sat(context, start->address, start->value);
				write_vram_byte(context, start->address, start->value >> 8);
				start->partial = 1;
				//skip auto-increment and removal of entry from fifo
				return;
			}
			break;
		case CRAM_WRITE: {
			//printf("CRAM Write | %X to %X\n", start->value, (start->address/2) & (CRAM_SIZE-1));
			if (start->partial == 3) {
				uint16_t val;
				if ((start->address & 1) && (context->regs[REG_MODE_2] & BIT_MODE_5)) {
					val = (context->cram[start->address >> 1 & (CRAM_SIZE-1)] & 0xFF) | start->value << 8;
				} else {
					uint16_t address = (context->regs[REG_MODE_2] & BIT_MODE_5) ? start->address >> 1 & (CRAM_SIZE-1) : start->address & 0x1F;
					val = (context->cram[address] & 0xFF00) | start->value;
				}
				write_cram(context, start->address, val);
			} else {
				write_cram(context, start->address, start->partial == 2 ? context->fifo[context->fifo_write].value : start->value);
			}
			break;
		}
		case VSRAM_WRITE:
			if (((start->address/2) & 63) < VSRAM_SIZE) {
				//printf("VSRAM Write: %X to %X @ frame: %d, vcounter: %d, hslot: %d, cycle: %d\n", start->value, start->address, context->frame, context->vcounter, context->hslot, context->cycles);
				if (start->partial == 3) {
					if (start->address & 1) {
						context->vsram[(start->address/2) & 63] &= 0xFF;
						context->vsram[(start->address/2) & 63] |= start->value << 8;
					} else {
						context->vsram[(start->address/2) & 63] &= 0xFF00;
						context->vsram[(start->address/2) & 63] |= start->value;
					}
				} else {
					context->vsram[(start->address/2) & 63] = start->partial == 2 ? context->fifo[context->fifo_write].value : start->value;
				}
			}

			break;
		}
		context->fifo_read = (context->fifo_read+1) & (FIFO_SIZE-1);
		if (context->fifo_read == context->fifo_write) {
			if ((context->cd & 0x20) && (context->regs[REG_DMASRC_H] & 0xC0) == 0x80) {
				context->flags |= FLAG_DMA_RUN;
			}
			context->fifo_read = -1;
		}
	} else if ((context->flags & FLAG_DMA_RUN) && (context->regs[REG_DMASRC_H] & 0xC0) == 0xC0) {
		if (context->flags & FLAG_READ_FETCHED) {
			write_vram_byte(context, context->address ^ 1, context->prefetch);
			
			//Update DMA state
			vdp_advance_dma(context);
			
			context->flags &= ~FLAG_READ_FETCHED;
		} else {
			context->prefetch = context->vdpmem[(context->regs[REG_DMASRC_M] << 8) | context->regs[REG_DMASRC_L] ^ 1];
			
			context->flags |= FLAG_READ_FETCHED;
		}
	} else if (!(context->cd & 1) && !(context->flags & (FLAG_READ_FETCHED|FLAG_PENDING))) {
		switch(context->cd & 0xF)
		{
		case VRAM_READ:
			if (context->flags2 & FLAG2_READ_PENDING) {
				context->prefetch |= context->vdpmem[context->address | 1];
				context->flags |= FLAG_READ_FETCHED;
				context->flags2 &= ~FLAG2_READ_PENDING;
				//Should this happen after the prefetch or after the read?
				increment_address(context);
			} else {
				//TODO: 128K VRAM Mode
				context->prefetch = context->vdpmem[context->address & 0xFFFE] << 8;
				context->flags2 |= FLAG2_READ_PENDING;
			}
			break;
		case VRAM_READ8: {
			uint32_t address = context->address ^ 1;
			if (!(context->regs[REG_MODE_2] & BIT_MODE_5)) {
				address = mode4_address_map[address & 0x3FFF];
			}
			context->prefetch = context->vdpmem[address];
			context->prefetch |= context->fifo[context->fifo_write].value & 0xFF00;
			context->flags |= FLAG_READ_FETCHED;
			//Should this happen after the prefetch or after the read?
			increment_address(context);
			break;
		}
		case CRAM_READ:
			context->prefetch = context->cram[(context->address/2) & (CRAM_SIZE-1)] & CRAM_BITS;
			context->prefetch |= context->fifo[context->fifo_write].value & ~CRAM_BITS;
			context->flags |= FLAG_READ_FETCHED;
			//Should this happen after the prefetch or after the read?
			increment_address(context);
			break;
		case VSRAM_READ: {
			uint16_t address = (context->address /2) & 63;
			if (address >= VSRAM_SIZE) {
				address = 0;
			}
			context->prefetch = context->vsram[address] & VSRAM_BITS;
			context->prefetch |= context->fifo[context->fifo_write].value & VSRAM_DIRTY_BITS;
			context->flags |= FLAG_READ_FETCHED;
			//Should this happen after the prefetch or after the read?
			increment_address(context);
			break;
		}
		}
	}
}

static void run_dma_src(vdp_context * context, int32_t slot)
{
	//TODO: Figure out what happens if CD bit 4 is not set in DMA copy mode
	//TODO: Figure out what happens when CD:0-3 is not set to a write mode in DMA operations
	if (context->fifo_write == context->fifo_read) {
		return;
	}
	fifo_entry * cur = NULL;
	if (!(context->regs[REG_DMASRC_H] & 0x80))
	{
		//68K -> VDP
		if (slot == -1 || !is_refresh(context, slot-1)) {
			cur = context->fifo + context->fifo_write;
			cur->cycle = context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20)*FIFO_LATENCY;
			cur->address = context->address;
			cur->value = read_dma_value((context->regs[REG_DMASRC_H] << 16) | (context->regs[REG_DMASRC_M] << 8) | context->regs[REG_DMASRC_L]);
			cur->cd = context->cd;
			cur->partial = 0;
			if (context->fifo_read < 0) {
				context->fifo_read = context->fifo_write;
			}
			context->fifo_write = (context->fifo_write + 1) & (FIFO_SIZE-1);
			vdp_advance_dma(context);
		}
	}
}

#define WINDOW_RIGHT 0x80
#define WINDOW_DOWN  0x80

static void read_map_scroll(uint16_t column, uint16_t vsram_off, uint32_t line, uint16_t address, uint16_t hscroll_val, vdp_context * context)
{
	uint16_t window_line_shift, v_offset_mask, vscroll_shift;
	if (context->double_res) {
		line *= 2;
		if (context->flags2 & FLAG2_EVEN_FIELD) {
			line++;
		}
		window_line_shift = 4;
		v_offset_mask = 0xF;
		vscroll_shift = 4;
	} else {
		window_line_shift = 3;
		v_offset_mask = 0x7;
		vscroll_shift = 3;
	}
	//TODO: Further research on vscroll latch behavior and the "first column bug"
	if (context->regs[REG_MODE_3] & BIT_VSCROLL) {
		if (!column) {
			if (context->regs[REG_MODE_4] & BIT_H40) {
				//Based on observed behavior documented by Eke-Eke, I'm guessing the VDP
				//ends up fetching the last value on the VSRAM bus in the H40 case
				//getting the last latched value should be close enough for now
				if (!vsram_off) {
					context->vscroll_latch[0] = context->vscroll_latch[1];
				}
			} else {
				//supposedly it's always forced to 0 in the H32 case
				context->vscroll_latch[0] = context->vscroll_latch[1] = 0;
			}
		} else if (context->regs[REG_MODE_3] & BIT_VSCROLL) {
			context->vscroll_latch[vsram_off] = context->vsram[column - 2 + vsram_off];
		}
	}
	if (!vsram_off) {
		uint16_t left_col, right_col;
		if (context->regs[REG_WINDOW_H] & WINDOW_RIGHT) {
			left_col = (context->regs[REG_WINDOW_H] & 0x1F) * 2 + 2;
			right_col = 42;
		} else {
			left_col = 0;
			right_col = (context->regs[REG_WINDOW_H] & 0x1F) * 2;
			if (right_col) {
				right_col += 2;
			}
		}
		uint16_t top_line, bottom_line;
		if (context->regs[REG_WINDOW_V] & WINDOW_DOWN) {
			top_line = (context->regs[REG_WINDOW_V] & 0x1F) << window_line_shift;
			bottom_line = context->double_res ? 481 : 241;
		} else {
			top_line = 0;
			bottom_line = (context->regs[REG_WINDOW_V] & 0x1F) << window_line_shift;
		}
		if ((column >= left_col && column < right_col) || (line >= top_line && line < bottom_line)) {
			uint16_t address = context->regs[REG_WINDOW] << 10;
			uint16_t line_offset, offset, mask;
			if (context->regs[REG_MODE_4] & BIT_H40) {
				address &= 0xF000;
				line_offset = (((line) >> vscroll_shift) * 64 * 2) & 0xFFF;
				mask = 0x7F;

			} else {
				address &= 0xF800;
				line_offset = (((line) >> vscroll_shift) * 32 * 2) & 0xFFF;
				mask = 0x3F;
			}
			if (context->double_res) {
				mask <<= 1;
				mask |= 1;
			}
			offset = address + line_offset + (((column - 2) * 2) & mask);
			context->col_1 = (context->vdpmem[offset] << 8) | context->vdpmem[offset+1];
			//printf("Window | top: %d, bot: %d, left: %d, right: %d, base: %X, line: %X offset: %X, tile: %X, reg: %X\n", top_line, bottom_line, left_col, right_col, address, line_offset, offset, ((context->col_1 & 0x3FF) << 5), context->regs[REG_WINDOW]);
			offset = address + line_offset + (((column - 1) * 2) & mask);
			context->col_2 = (context->vdpmem[offset] << 8) | context->vdpmem[offset+1];
			context->v_offset = (line) & v_offset_mask;
			context->flags |= FLAG_WINDOW;
			return;
		}
		context->flags &= ~FLAG_WINDOW;
	}
	//TODO: Verify behavior for 0x20 case
	uint16_t vscroll = 0xFF | (context->regs[REG_SCROLL] & 0x30) << 4;
	if (context->double_res) {
		vscroll <<= 1;
		vscroll |= 1;
	}
	vscroll &= context->vscroll_latch[vsram_off] + line;
	context->v_offset = vscroll & v_offset_mask;
	//printf("%s | line %d, vsram: %d, vscroll: %d, v_offset: %d\n",(vsram_off ? "B" : "A"), line, context->vsram[context->regs[REG_MODE_3] & 0x4 ? column : 0], vscroll, context->v_offset);
	vscroll >>= vscroll_shift;
	uint16_t hscroll_mask;
	uint16_t v_mul;
	switch(context->regs[REG_SCROLL] & 0x3)
	{
	case 0:
		hscroll_mask = 0x1F;
		v_mul = 64;
		break;
	case 0x1:
		hscroll_mask = 0x3F;
		v_mul = 128;
		break;
	case 0x2:
		//TODO: Verify this behavior
		hscroll_mask = 0x1F;
		v_mul = 0;
		break;
	case 0x3:
		hscroll_mask = 0x7F;
		v_mul = 256;
		break;
	}
	uint16_t hscroll, offset;
	for (int i = 0; i < 2; i++) {
		hscroll = (column - 2 + i - ((hscroll_val/8) & 0xFFFE)) & hscroll_mask;
		offset = address + ((vscroll * v_mul + hscroll*2) & 0x1FFF);
		//printf("%s | line: %d, col: %d, x: %d, hs_mask %X, scr reg: %X, tbl addr: %X\n", (vsram_off ? "B" : "A"), line, (column-2+i), hscroll, hscroll_mask, context->regs[REG_SCROLL], offset);
		uint16_t col_val = (context->vdpmem[offset] << 8) | context->vdpmem[offset+1];
		if (i) {
			context->col_2 = col_val;
		} else {
			context->col_1 = col_val;
		}
	}
}

static void read_map_scroll_a(uint16_t column, uint32_t line, vdp_context * context)
{
	read_map_scroll(column, 0, line, (context->regs[REG_SCROLL_A] & 0x38) << 10, context->hscroll_a, context);
}

static void read_map_scroll_b(uint16_t column, uint32_t line, vdp_context * context)
{
	read_map_scroll(column, 1, line, (context->regs[REG_SCROLL_B] & 0x7) << 13, context->hscroll_b, context);
}

static void read_map_mode4(uint16_t column, uint32_t line, vdp_context * context)
{
	uint32_t address = (context->regs[REG_SCROLL_A] & 0xE) << 10;
	//add row
	uint32_t vscroll = line;
	if (column < 24 || !(context->regs[REG_MODE_1] & BIT_VSCRL_LOCK)) {
		vscroll += context->regs[REG_Y_SCROLL];
	}
	if (vscroll > 223) {
		vscroll -= 224;
	}
	address += (vscroll >> 3) * 2 * 32;
	//add column
	address += ((column - (context->hscroll_a >> 3)) & 31) * 2;
	//adjust for weird VRAM mapping in Mode 4
	address = mode4_address_map[address];
	context->col_1 = (context->vdpmem[address] << 8) | context->vdpmem[address+1];
}

static void render_map(uint16_t col, uint8_t * tmp_buf, uint8_t offset, vdp_context * context)
{
	uint16_t address;
	uint16_t vflip_base;
	if (context->double_res) {
		address = ((col & 0x3FF) << 6);
		vflip_base = 60;
	} else {
		address = ((col & 0x7FF) << 5);
		vflip_base = 28;
	}
	if (col & MAP_BIT_V_FLIP) {
		address +=  vflip_base - 4 * context->v_offset;
	} else {
		address += 4 * context->v_offset;
	}
	uint16_t pal_priority = (col >> 9) & 0x70;
	int32_t dir;
	if (col & MAP_BIT_H_FLIP) {
		offset += 7;
		offset &= SCROLL_BUFFER_MASK;
		dir = -1;
	} else {
		dir = 1;
	}
	for (uint32_t i=0; i < 4; i++, address++)
	{
		tmp_buf[offset] = pal_priority | (context->vdpmem[address] >> 4);
		offset += dir;
		offset &= SCROLL_BUFFER_MASK;
		tmp_buf[offset] = pal_priority | (context->vdpmem[address] & 0xF);
		offset += dir;
		offset &= SCROLL_BUFFER_MASK;
	}
}

static void render_map_1(vdp_context * context)
{
	render_map(context->col_1, context->tmp_buf_a, context->buf_a_off, context);
}

static void render_map_2(vdp_context * context)
{
	render_map(context->col_2, context->tmp_buf_a, context->buf_a_off+8, context);
}

static void render_map_3(vdp_context * context)
{
	render_map(context->col_1, context->tmp_buf_b, context->buf_b_off, context);
}

static void fetch_map_mode4(uint16_t col, uint32_t line, vdp_context *context)
{
	//calculate pixel row to fetch
	uint32_t vscroll = line;
	if (col < 24 || !(context->regs[REG_MODE_1] & BIT_VSCRL_LOCK)) {
		vscroll += context->regs[REG_Y_SCROLL];
	}
	if (vscroll > 223) {
		vscroll -= 224;
	}
	vscroll &= 7;
	if (context->col_1 & 0x400) {
		vscroll = 7 - vscroll;
	}
	
	uint32_t address = mode4_address_map[((context->col_1 & 0x1FF) * 32) + vscroll * 4];
	context->fetch_tmp[0] = context->vdpmem[address];
	context->fetch_tmp[1] = context->vdpmem[address+1];
}

static void render_map_output(uint32_t line, int32_t col, vdp_context * context)
{
	uint32_t *dst;
	uint8_t output_disabled = (context->test_port & TEST_BIT_DISABLE) != 0;
	uint8_t test_layer = context->test_port >> 7 & 3;
	if (context->state == PREPARING && !test_layer) {
		if (col) {
			col -= 2;
			dst = context->output + BORDER_LEFT + col * 8;
		} else {
			dst = context->output;
			uint32_t bg_color = context->colors[context->regs[REG_BG_COLOR] & 0x3F];
			for (int i = 0; i < BORDER_LEFT; i++, dst++)
			{
				*dst = bg_color;
			}
			context->done_output = dst;
			return;
		}
		uint32_t color = context->colors[context->regs[REG_BG_COLOR] & 0x3F];
		for (int i = 0; i < 16; i++)
		{
			*(dst++) = color;
		}
		context->done_output = dst;
		return;
	}
	line &= 0xFF;
	render_map(context->col_2, context->tmp_buf_b, context->buf_b_off+8, context);
	uint8_t *sprite_buf,  *plane_a, *plane_b;
	int plane_a_off, plane_b_off;
	if (col)
	{
		col-=2;
		dst = context->output + BORDER_LEFT + col * 8;
		if (context->debug < 2) {
			sprite_buf = context->linebuf + col * 8;
			uint8_t a_src, src;
			if (context->flags & FLAG_WINDOW) {
				plane_a_off = context->buf_a_off;
				a_src = DBG_SRC_W;
			} else {
				plane_a_off = context->buf_a_off - (context->hscroll_a & 0xF);
				a_src = DBG_SRC_A;
			}
			plane_b_off = context->buf_b_off - (context->hscroll_b & 0xF);
			//printf("A | tmp_buf offset: %d\n", 8 - (context->hscroll_a & 0x7));

			if (context->regs[REG_MODE_4] & BIT_HILIGHT) {
				for (int i = 0; i < 16; ++plane_a_off, ++plane_b_off, ++sprite_buf, ++i) {
					plane_a = context->tmp_buf_a + (plane_a_off & SCROLL_BUFFER_MASK);
					plane_b = context->tmp_buf_b + (plane_b_off & SCROLL_BUFFER_MASK);
					uint8_t pixel = context->regs[REG_BG_COLOR];
					uint32_t *colors = context->colors;
					src = DBG_SRC_BG;
					if (*plane_b & 0xF) {
						pixel = *plane_b;
						src = DBG_SRC_B;
					}
					uint8_t intensity = *plane_b & BUF_BIT_PRIORITY;
					if (*plane_a & 0xF && (*plane_a & BUF_BIT_PRIORITY) >= (pixel & BUF_BIT_PRIORITY)) {
						pixel = *plane_a;
						src = a_src;
					}
					intensity |= *plane_a & BUF_BIT_PRIORITY;
					if (*sprite_buf & 0xF && (*sprite_buf & BUF_BIT_PRIORITY) >= (pixel & BUF_BIT_PRIORITY)) {
						if ((*sprite_buf & 0x3F) == 0x3E) {
							intensity += BUF_BIT_PRIORITY;
						} else if ((*sprite_buf & 0x3F) == 0x3F) {
							intensity = 0;
						} else {
							pixel = *sprite_buf;
							src = DBG_SRC_S;
							if ((pixel & 0xF) == 0xE) {
								intensity = BUF_BIT_PRIORITY;
							} else {
								intensity |= pixel & BUF_BIT_PRIORITY;
							}
						}
					}
					if (output_disabled) {
						pixel = 0x3F;
					}
					if (!intensity) {
						src |= DBG_SHADOW;
						colors += CRAM_SIZE;
					} else if (intensity ==  BUF_BIT_PRIORITY*2) {
						src |= DBG_HILIGHT;
						colors += CRAM_SIZE*2;
					}
					//TODO: Verify how test register stuff interacts with shadow/highlight
					//TODO: Simulate CRAM corruption from bus fight
					switch (test_layer)
					{
					case 1:
						pixel &= *sprite_buf;
						if (output_disabled && pixel) {
							src = DBG_SRC_S;
						}
						break;
					case 2:
						pixel &= *plane_a;
						if (output_disabled && pixel) {
							src = DBG_SRC_A;
						}
						break;
					case 3:
						pixel &= *plane_b;
						if (output_disabled && pixel) {
							src = DBG_SRC_B;
						}
						break;
					}

					uint32_t outpixel;
					if (context->debug) {
						outpixel = context->debugcolors[src];
					} else {
						outpixel = colors[pixel & 0x3F];
					}
					*(dst++) = outpixel;
				}
			} else {
				for (int i = 0; i < 16; ++plane_a_off, ++plane_b_off, ++sprite_buf, ++i) {
					plane_a = context->tmp_buf_a + (plane_a_off & SCROLL_BUFFER_MASK);
					plane_b = context->tmp_buf_b + (plane_b_off & SCROLL_BUFFER_MASK);
					uint8_t pixel = context->regs[REG_BG_COLOR];
					src = DBG_SRC_BG;
					if (output_disabled) {
						pixel = 0x3F;
					} else {
						if (*plane_b & 0xF) {
							pixel = *plane_b;
							src = DBG_SRC_B;
						}
						if (*plane_a & 0xF && (*plane_a & BUF_BIT_PRIORITY) >= (pixel & BUF_BIT_PRIORITY)) {
							pixel = *plane_a;
							src = a_src;
						}
						if (*sprite_buf & 0xF && (*sprite_buf & BUF_BIT_PRIORITY) >= (pixel & BUF_BIT_PRIORITY)) {
							pixel = *sprite_buf;
							src = DBG_SRC_S;
						}
					}
					//TODO: Simulate CRAM corruption from bus fight
					switch (test_layer)
					{
					case 1:
						pixel &= *sprite_buf;
						if (output_disabled && pixel) {
							src = DBG_SRC_S;
						}
						break;
					case 2:
						pixel &= *plane_a;
						if (output_disabled && pixel) {
							src = DBG_SRC_A;
						}
						break;
					case 3:
						pixel &= *plane_b;
						if (output_disabled && pixel) {
							src = DBG_SRC_B;
						}
						break;
					}
					uint32_t outpixel;
					if (context->debug) {
						outpixel = context->debugcolors[src];
					} else {
						outpixel = context->colors[pixel & 0x3F];
					}
					*(dst++) = outpixel;
				}
			}
		} else if (context->debug == 2) {
			if (col < 32) {
				*(dst++) = context->colors[col * 2];
				*(dst++) = context->colors[col * 2];
				*(dst++) = context->colors[col * 2];
				*(dst++) = context->colors[col * 2];
				*(dst++) = context->colors[col * 2 + 1];
				*(dst++) = context->colors[col * 2 + 1];
				*(dst++) = context->colors[col * 2 + 1];
				*(dst++) = context->colors[col * 2 + 1];
				*(dst++) = context->colors[col * 2 + 2];
				*(dst++) = context->colors[col * 2 + 2];
				*(dst++) = context->colors[col * 2 + 2];
				*(dst++) = context->colors[col * 2 + 2];
				*(dst++) = context->colors[col * 2 + 3];
				*(dst++) = context->colors[col * 2 + 3];
				*(dst++) = context->colors[col * 2 + 3];
				*(dst++) = context->colors[col * 2 + 3];
			} else if (col == 32 || line >= 192) {
				for (int32_t i = 0; i < 16; i ++) {
					*(dst++) = 0;
				}
			} else {
				for (int32_t i = 0; i < 16; i ++) {
					*(dst++) = context->colors[line / 3 + (col - 34) * 0x20];
				}
			}
		} else {
			uint32_t base = (context->debug - 3) * 0x200;
			uint32_t cell = base + (line / 8) * (context->regs[REG_MODE_4] & BIT_H40 ? 40 : 32) + col;
			uint32_t address = (cell * 32 + (line % 8) * 4) & 0xFFFF;
			for (int32_t i = 0; i < 4; i ++) {
				*(dst++) = context->colors[(context->debug_pal << 4) | (context->vdpmem[address] >> 4)];
				*(dst++) = context->colors[(context->debug_pal << 4) | (context->vdpmem[address] & 0xF)];
				address++;
			}
			cell++;
			address = (cell * 32 + (line % 8) * 4) & 0xFFFF;
			for (int32_t i = 0; i < 4; i ++) {
				*(dst++) = context->colors[(context->debug_pal << 4) | (context->vdpmem[address] >> 4)];
				*(dst++) = context->colors[(context->debug_pal << 4) | (context->vdpmem[address] & 0xF)];
				address++;
			}
		}
	} else {
		dst = context->output;
		uint8_t pixel = context->regs[REG_BG_COLOR] & 0x3F;
		if (output_disabled) {
			pixel = 0x3F;
		}
		uint32_t bg_color = context->colors[pixel];
		if (test_layer) {
			switch(test_layer)
			{
			case 1:
				bg_color = context->colors[0];
				for (int i = 0; i < BORDER_LEFT; i++, dst++)
				{
					*dst = bg_color;
				}
				break;
			case 2: {
				//plane A
				//TODO: Deal with Window layer
				int i;
				i = 0;
				uint8_t buf_off = context->buf_a_off - (context->hscroll_a & 0xF) + (16 - BORDER_LEFT);
				//uint8_t *src = context->tmp_buf_a + ((context->buf_a_off + (i ? 0 : (16 - BORDER_LEFT) - (context->hscroll_a & 0xF))) & SCROLL_BUFFER_MASK); 
				for (; i < BORDER_LEFT; buf_off++, i++, dst++)
				{
					*dst = context->colors[context->tmp_buf_a[buf_off & SCROLL_BUFFER_MASK]];
				}
				break;
			}
			case 3: {
				//plane B
				int i;
				i = 0;
				uint8_t buf_off = context->buf_b_off - (context->hscroll_b & 0xF) + (16 - BORDER_LEFT);
				//uint8_t *src = context->tmp_buf_b + ((context->buf_b_off + (i ? 0 : (16 - BORDER_LEFT) - (context->hscroll_b & 0xF))) & SCROLL_BUFFER_MASK); 
				for (; i < BORDER_LEFT; buf_off++, i++, dst++)
				{
					*dst = context->colors[context->tmp_buf_b[buf_off & SCROLL_BUFFER_MASK]];
				}
				break;
			}
			}
		} else {
			for (int i = 0; i < BORDER_LEFT; i++, dst++)
			{
				*dst = bg_color;
			}
		}
	}
	context->done_output = dst;
	context->buf_a_off = (context->buf_a_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;
	context->buf_b_off = (context->buf_b_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;
}

static void render_map_mode4(uint32_t line, int32_t col, vdp_context * context)
{
	uint32_t vscroll = line;
	if (col < 24 || !(context->regs[REG_MODE_1] & BIT_VSCRL_LOCK)) {
		vscroll += context->regs[REG_Y_SCROLL];
	}
	if (vscroll > 223) {
		vscroll -= 224;
	}
	vscroll &= 7;
	if (context->col_1 & 0x400) {
		//vflip
		vscroll = 7 - vscroll;
	}
	
	uint32_t pixels = planar_to_chunky[context->fetch_tmp[0]] << 1;
	pixels |=  planar_to_chunky[context->fetch_tmp[1]];
	
	uint32_t address = mode4_address_map[((context->col_1 & 0x1FF) * 32) + vscroll * 4 + 2];
	pixels |= planar_to_chunky[context->vdpmem[address]] << 3;
	pixels |= planar_to_chunky[context->vdpmem[address+1]] << 2;
	
	int i, i_inc, i_limit;
	if (context->col_1 & 0x200) {
		//hflip
		i = 0;
		i_inc = 4;
		i_limit = 32;
	} else {
		i = 28;
		i_inc = -4;
		i_limit = -4;
	}
	uint8_t pal_priority = (context->col_1 >> 7 & 0x10) | (context->col_1 >> 6 & 0x40);
	for (uint8_t *dst = context->tmp_buf_a + context->buf_a_off; i != i_limit; i += i_inc, dst++)
	{
		*dst = (pixels >> i & 0xF) | pal_priority;
	}
	context->buf_a_off = (context->buf_a_off + 8) & 15;
	
	uint8_t bgcolor = 0x10 | (context->regs[REG_BG_COLOR] & 0xF) + CRAM_SIZE*3;
	uint32_t *dst = context->output + col * 8 + BORDER_LEFT;
	if (context->state == PREPARING) {
		for (int i = 0; i < 16; i++)
		{
			*(dst++) = context->colors[bgcolor];
		}
		context->done_output = dst;
		return;
	}
	if (context->debug < 2) {
		if (col || !(context->regs[REG_MODE_1] & BIT_COL0_MASK)) {
			uint8_t *sprite_src = context->linebuf + col * 8;
			if (context->regs[REG_MODE_1] & BIT_SPRITE_8PX) {
				sprite_src += 8;
			}
			for (int i = 0; i < 8; i++, sprite_src++)
			{
				uint8_t *bg_src = context->tmp_buf_a + ((8 + i + col * 8 - (context->hscroll_a & 0x7)) & 15);
				if ((*bg_src & 0x4F) > 0x40 || !*sprite_src) {
					//background plane has priority and is opaque or sprite layer is transparent
					if (context->debug) {
						*(dst++) = context->debugcolors[DBG_SRC_A];
					} else {
						*(dst++) = context->colors[(*bg_src & 0x1F) + CRAM_SIZE*3];
					}
				} else {
					//sprite layer is opaque and not covered by high priority BG pixels
					if (context->debug) {
						*(dst++) = context->debugcolors[DBG_SRC_S];
					} else {
						*(dst++) = context->colors[*sprite_src | 0x10 + CRAM_SIZE*3];
					}
				}
			}
		} else {
			for (int i = 0; i < 8; i++)
			{
				*(dst++) = context->colors[bgcolor];
			}
		}
	} else if (context->debug == 2) {
		for (int i = 0; i < 8; i++)
		{
			*(dst++) = context->colors[CRAM_SIZE*3 + col];
		}
	} else {
		uint32_t cell = (line / 8) * 32 + col;
		uint32_t address = cell * 32 + (line % 8) * 4;
		uint32_t m4_address = mode4_address_map[address & 0x3FFF];
		uint32_t pixel = planar_to_chunky[context->vdpmem[m4_address]] << 1;
		pixel |= planar_to_chunky[context->vdpmem[m4_address + 1]];
		m4_address = mode4_address_map[(address + 2) & 0x3FFF];
		pixel |= planar_to_chunky[context->vdpmem[m4_address]] << 3;
		pixel |= planar_to_chunky[context->vdpmem[m4_address + 1]] << 2;
		if (context->debug_pal < 2) {
			for (int i = 28; i >= 0; i -= 4)
			{
				*(dst++) = context->colors[CRAM_SIZE*3 | (context->debug_pal << 4) | (pixel >> i & 0xF)];
			}
		} else {
			for (int i = 28; i >= 0; i -= 4)
			{
				uint8_t value = (pixel >> i & 0xF) * 17;
				if (context->debug_pal == 3) {
					value = 255 - value;
				}
				*(dst++) = render_map_color(value, value, value);
			}
		}
	}
	context->done_output = dst;
}

static uint32_t const h40_hsync_cycles[] = {19, 20, 20, 20, 18, 20, 20, 20, 18, 20, 20, 20, 18, 20, 20, 20, 19};

static void vdp_advance_line(vdp_context *context)
{
#ifdef TIMING_DEBUG
	static uint32_t last_line = 0xFFFFFFFF;
	if (last_line != 0xFFFFFFFF) {
		uint32_t diff = context->cycles - last_line;
		if (diff != MCLKS_LINE) {
			printf("Line %d took %d cycles\n", context->vcounter, diff);
		}
	}
	last_line = context->cycles;
#endif
	context->vcounter++;
	
	uint8_t is_mode_5 = context->regs[REG_MODE_2] & BIT_MODE_5;
	if (is_mode_5) {
		if (context->flags2 & FLAG2_REGION_PAL) {
			if (context->regs[REG_MODE_2] & BIT_PAL) {
				if (context->vcounter == 0x10B) {
					context->vcounter = 0x1D2;
				}
			} else if (context->vcounter == 0x103){
				context->vcounter = 0x1CA;
			}
		} else {
			if (context->regs[REG_MODE_2] & BIT_PAL) {
				if (context->vcounter == 0x100) {
					context->vcounter = 0x1FA;
				}
			} else if (context->vcounter == 0xEB) {
				context->vcounter = 0x1E5;
			}
		}
	} else if (context->vcounter == 0xDB) {
		context->vcounter = 0x1D5;
	}
	context->vcounter &= 0x1FF;
	if (context->state == PREPARING) {
		context->state = ACTIVE;
	}
	if (context->vcounter == 0x1FF) {
		context->flags2 &= ~FLAG2_PAUSE;
	}

	if (context->state != ACTIVE) {
		context->hint_counter = context->regs[REG_HINT];
	} else if (context->hint_counter) {
		context->hint_counter--;
	} else {
		context->flags2 |= FLAG2_HINT_PENDING;
		context->pending_hint_start = context->cycles;
		context->hint_counter = context->regs[REG_HINT];
	}
}

static void advance_output_line(vdp_context *context)
{
	if (headless) {
		if (context->vcounter == context->inactive_start) {
			context->frame++;
		}
		context->vcounter &= 0x1FF;
	} else {
		uint16_t lines_max = (context->flags2 & FLAG2_REGION_PAL) 
			? 240 + BORDER_TOP_V30_PAL + BORDER_BOT_V30_PAL 
			: 224 + BORDER_TOP_V28 + BORDER_BOT_V28;

		if (context->output_lines == lines_max) {
			render_framebuffer_updated(context->cur_buffer, context->h40_lines > (context->inactive_start + context->border_top) / 2 ? LINEBUF_SIZE : (256+HORIZ_BORDER));
			context->cur_buffer = context->flags2 & FLAG2_EVEN_FIELD ? FRAMEBUFFER_EVEN : FRAMEBUFFER_ODD;
			context->fb = render_get_framebuffer(context->cur_buffer, &context->output_pitch);
			context->h40_lines = 0;
			context->frame++;
			context->output_lines = 0;
		}
		uint32_t output_line = context->vcounter;
		if (!(context->regs[REG_MODE_2] & BIT_MODE_5)) {
			//vcounter increment occurs much later in Mode 4
			output_line++;
		} 
		if (output_line < context->inactive_start + context->border_bot && context->output_lines > 0) {
			output_line = context->output_lines++;//context->border_top + context->vcounter;
		} else if (output_line >= 0x200 - context->border_top) {
			if (output_line == 0x200 - context->border_top) {
				//We're at the top of the display, force context->output_lines to be zero to avoid
				//potential screen rolling if the mode is changed at an inopportune time
				context->output_lines = 0;
			}
			output_line = context->output_lines++;//context->vcounter - (0x200 - context->border_top);
		} else {
			output_line = INVALID_LINE;
		}
		context->output = (uint32_t *)(((char *)context->fb) + context->output_pitch * output_line);
		context->done_output = context->output;
#ifdef DEBUG_FB_FILL
		for (int i = 0; i < LINEBUF_SIZE; i++)
		{
			context->output[i] = 0xFFFF00FF;
		}
#endif	
		if (output_line != INVALID_LINE && (context->regs[REG_MODE_4] & BIT_H40)) {
			context->h40_lines++;
		}
	}
}

void vdp_release_framebuffer(vdp_context *context)
{
	render_framebuffer_updated(context->cur_buffer, context->h40_lines > (context->inactive_start + context->border_top) / 2 ? LINEBUF_SIZE : (256+HORIZ_BORDER));
	context->output = context->fb = NULL;
}

void vdp_reacquire_framebuffer(vdp_context *context)
{
	context->fb = render_get_framebuffer(context->cur_buffer, &context->output_pitch);
	uint16_t lines_max = (context->flags2 & FLAG2_REGION_PAL) 
			? 240 + BORDER_TOP_V30_PAL + BORDER_BOT_V30_PAL
			: 224 + BORDER_TOP_V28 + BORDER_BOT_V28;
	if (context->output_lines <= lines_max && context->output_lines > 0) {
		context->output = (uint32_t *)(((char *)context->fb) + context->output_pitch * (context->output_lines - 1));
	} else {
		context->output = (uint32_t *)(((char *)context->fb) + context->output_pitch * INVALID_LINE);
	}
}

static void render_border_garbage(vdp_context *context, uint32_t address, uint8_t *buf, uint8_t buf_off, uint16_t col)
{
	uint8_t base = col >> 9 & 0x30;
	for (int i = 0; i < 4; i++, address++)
	{
		uint8_t byte = context->vdpmem[address & 0xFFFF];
		buf[(buf_off++) & SCROLL_BUFFER_MASK] = base | byte >> 4;
		buf[(buf_off++) & SCROLL_BUFFER_MASK] = base | byte & 0xF;
	}
}

static void draw_right_border(vdp_context *context)
{
	uint32_t *dst = context->output + BORDER_LEFT + ((context->regs[REG_MODE_4] & BIT_H40) ? 320 : 256);
	uint8_t pixel = context->regs[REG_BG_COLOR] & 0x3F;
	if ((context->test_port & TEST_BIT_DISABLE) != 0) {
		pixel = 0x3F;
	}
	uint32_t bg_color = context->colors[pixel];
	uint8_t test_layer = context->test_port >> 7 & 3;
	if (test_layer) {
		switch(test_layer)
			{
			case 1:
				bg_color = context->colors[0];
				for (int i = 0; i < BORDER_RIGHT; i++, dst++)
				{
					*dst = bg_color;
				}
				break;
			case 2: {
				//plane A
				//TODO: Deal with Window layer
				int i;
				i = 0;
				uint8_t buf_off = context->buf_a_off - (context->hscroll_a & 0xF);
				//uint8_t *src = context->tmp_buf_a + ((context->buf_a_off + (i ? 0 : (16 - BORDER_LEFT) - (context->hscroll_a & 0xF))) & SCROLL_BUFFER_MASK); 
				for (; i < BORDER_RIGHT; buf_off++, i++, dst++)
				{
					*dst = context->colors[context->tmp_buf_a[buf_off & SCROLL_BUFFER_MASK] & 0x3F];
				}
				break;
			}
			case 3: {
				//plane B
				int i;
				i = 0;
				uint8_t buf_off = context->buf_b_off - (context->hscroll_b & 0xF);
				//uint8_t *src = context->tmp_buf_b + ((context->buf_b_off + (i ? 0 : (16 - BORDER_LEFT) - (context->hscroll_b & 0xF))) & SCROLL_BUFFER_MASK); 
				for (; i < BORDER_RIGHT; buf_off++, i++, dst++)
				{
					*dst = context->colors[context->tmp_buf_b[buf_off & SCROLL_BUFFER_MASK] & 0x3F];
				}
				break;
			}
			}
	} else {
		for (int i = 0; i < BORDER_RIGHT; i++, dst++)
		{
			*dst = bg_color;
		}
	}
	context->done_output = dst;
	context->buf_a_off = (context->buf_a_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;
	context->buf_b_off = (context->buf_b_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;
}

#define CHECK_ONLY if (context->cycles >= target_cycles) { return; }
#define CHECK_LIMIT if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, -1); } context->hslot++; context->cycles += slot_cycles; CHECK_ONLY

#define COLUMN_RENDER_BLOCK(column, startcyc) \
	case startcyc:\
		read_map_scroll_a(column, context->vcounter, context);\
		CHECK_LIMIT\
	case ((startcyc+1)&0xFF):\
		external_slot(context);\
		CHECK_LIMIT\
	case ((startcyc+2)&0xFF):\
		render_map_1(context);\
		CHECK_LIMIT\
	case ((startcyc+3)&0xFF):\
		render_map_2(context);\
		CHECK_LIMIT\
	case ((startcyc+4)&0xFF):\
		read_map_scroll_b(column, context->vcounter, context);\
		CHECK_LIMIT\
	case ((startcyc+5)&0xFF):\
		read_sprite_x(context->vcounter, context);\
		CHECK_LIMIT\
	case ((startcyc+6)&0xFF):\
		render_map_3(context);\
		CHECK_LIMIT\
	case ((startcyc+7)&0xFF):\
		render_map_output(context->vcounter, column, context);\
		CHECK_LIMIT

#define COLUMN_RENDER_BLOCK_REFRESH(column, startcyc) \
	case startcyc:\
		read_map_scroll_a(column, context->vcounter, context);\
		CHECK_LIMIT\
	case (startcyc+1):\
		/* refresh, no don't run dma src */\
		context->hslot++;\
		context->cycles += slot_cycles;\
		CHECK_ONLY\
	case (startcyc+2):\
		render_map_1(context);\
		CHECK_LIMIT\
	case (startcyc+3):\
		render_map_2(context);\
		CHECK_LIMIT\
	case (startcyc+4):\
		read_map_scroll_b(column, context->vcounter, context);\
		CHECK_LIMIT\
	case (startcyc+5):\
		read_sprite_x(context->vcounter, context);\
		CHECK_LIMIT\
	case (startcyc+6):\
		render_map_3(context);\
		CHECK_LIMIT\
	case (startcyc+7):\
		render_map_output(context->vcounter, column, context);\
		CHECK_LIMIT
		
#define COLUMN_RENDER_BLOCK_MODE4(column, startcyc) \
	case startcyc:\
		read_map_mode4(column, context->vcounter, context);\
		CHECK_LIMIT\
	case ((startcyc+1)&0xFF):\
		if (column & 3) {\
			scan_sprite_table_mode4(context);\
		} else {\
			external_slot(context);\
		}\
		CHECK_LIMIT\
	case ((startcyc+2)&0xFF):\
		fetch_map_mode4(column, context->vcounter, context);\
		CHECK_LIMIT\
	case ((startcyc+3)&0xFF):\
		render_map_mode4(context->vcounter, column, context);\
		CHECK_LIMIT
		
#define CHECK_LIMIT_HSYNC(slot) \
	if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, -1); } \
	if (slot >= HSYNC_SLOT_H40 && slot < HSYNC_END_H40) {\
		context->cycles += h40_hsync_cycles[slot - HSYNC_SLOT_H40];\
	} else {\
		context->cycles += slot_cycles;\
	}\
	if (slot == 182) {\
		context->hslot = 229;\
	} else {\
		context->hslot++;\
	}\
	CHECK_ONLY

#define SPRITE_RENDER_H40(slot) \
	case slot:\
		if ((slot) == BG_START_SLOT + LINEBUF_SIZE/2) {\
			advance_output_line(context);\
		}\
		if (slot == 168 || slot == 247 || slot == 248) {\
			render_border_garbage(\
				context,\
				context->sprite_draw_list[context->cur_slot].address,\
				context->tmp_buf_b,\
				context->buf_b_off + (slot == 247 ? 0 : 8),\
				slot == 247 ? context->col_1 : context->col_2\
			);\
			if (slot == 248) {\
				context->buf_a_off = (context->buf_a_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;\
				context->buf_b_off = (context->buf_b_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;\
			}\
		} else if (slot == 243) {\
			render_border_garbage(\
				context,\
				context->sprite_draw_list[context->cur_slot].address,\
				context->tmp_buf_a,\
				context->buf_a_off,\
				context->col_1\
			);\
		} else if (slot == 169) {\
			draw_right_border(context);\
		}\
		render_sprite_cells( context);\
		scan_sprite_table(context->vcounter, context);\
		CHECK_LIMIT_HSYNC(slot)

//Note that the line advancement check will fail if BG_START_SLOT is > 6
//as we're bumping up against the hcounter jump
#define SPRITE_RENDER_H32(slot) \
	case slot:\
		if ((slot) == BG_START_SLOT + (256+HORIZ_BORDER)/2) {\
			advance_output_line(context);\
		}\
		if (slot == 136 || slot == 247 || slot == 248) {\
			render_border_garbage(\
				context,\
				context->sprite_draw_list[context->cur_slot].address,\
				context->tmp_buf_b,\
				context->buf_b_off + (slot == 247 ? 0 : 8),\
				slot == 247 ? context->col_1 : context->col_2\
			);\
			if (slot == 248) {\
				context->buf_a_off = (context->buf_a_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;\
				context->buf_b_off = (context->buf_b_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;\
			}\
		} else if (slot == 137) {\
			draw_right_border(context);\
		}\
		render_sprite_cells( context);\
		scan_sprite_table(context->vcounter, context);\
		if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, -1); } \
		if (slot == 147) {\
			context->hslot = 233;\
		} else {\
			context->hslot++;\
		}\
		context->cycles += slot_cycles;\
		CHECK_ONLY
		
#define MODE4_CHECK_SLOT_LINE(slot) \
		if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, -1); } \
		if ((slot) == BG_START_SLOT + (256+HORIZ_BORDER)/2) {\
			advance_output_line(context);\
		}\
		if ((slot) == 147) {\
			context->hslot = 233;\
		} else {\
			context->hslot++;\
		}\
		context->cycles += slot_cycles;\
		if ((slot+1) == LINE_CHANGE_MODE4) {\
			vdp_advance_line(context);\
			if (context->vcounter == 192) {\
				return;\
			}\
		}\
		CHECK_ONLY

#define CALC_SLOT(slot, increment) ((slot+increment) > 147 && (slot+increment) < 233 ? (slot+increment-148+233): (slot+increment))
		
#define SPRITE_RENDER_H32_MODE4(slot) \
	case slot:\
		read_sprite_x_mode4(context);\
		MODE4_CHECK_SLOT_LINE(slot)\
	case CALC_SLOT(slot, 1):\
		read_sprite_x_mode4(context);\
		MODE4_CHECK_SLOT_LINE(CALC_SLOT(slot,1))\
	case CALC_SLOT(slot, 2):\
		fetch_sprite_cells_mode4(context);\
		MODE4_CHECK_SLOT_LINE(CALC_SLOT(slot, 2))\
	case CALC_SLOT(slot, 3):\
		if ((slot + 3) == 140) {\
			uint32_t *dst = context->output + BORDER_LEFT + 256 + 8;\
			uint32_t bgcolor = context->colors[0x10 | (context->regs[REG_BG_COLOR] & 0xF) + CRAM_SIZE*3];\
			for (int i = 0; i < BORDER_RIGHT-8; i++, dst++)\
			{\
				*dst = bgcolor;\
			}\
			context->done_output = dst;\
		}\
		render_sprite_cells_mode4(context);\
		MODE4_CHECK_SLOT_LINE(CALC_SLOT(slot, 3))\
	case CALC_SLOT(slot, 4):\
		fetch_sprite_cells_mode4(context);\
		MODE4_CHECK_SLOT_LINE(CALC_SLOT(slot, 4))\
	case CALC_SLOT(slot, 5):\
		render_sprite_cells_mode4(context);\
		MODE4_CHECK_SLOT_LINE(CALC_SLOT(slot, 5))

static void vdp_h40(vdp_context * context, uint32_t target_cycles)
{
	uint16_t address;
	uint32_t mask;
	uint32_t const slot_cycles = MCLKS_SLOT_H40;
	switch(context->hslot)
	{
	for (;;)
	{
	case 165:
		if (!(context->regs[REG_MODE_3] & BIT_VSCROLL)) {
			//TODO: Develop some tests on hardware to see when vscroll latch actually happens for full plane mode
			//See note in vdp_h32 for why this was originally moved out of read_map_scroll
			//Skitchin' has a similar problem, but uses H40 mode. It seems to be able to hit the extern slot at 232
			//pretty consistently
			context->vscroll_latch[0] = context->vsram[0];
			context->vscroll_latch[1] = context->vsram[1];
		}
		if (context->state == PREPARING) {
			uint32_t bg_color = context->colors[context->regs[REG_BG_COLOR] & 0x3F];
			uint32_t *dst = context->output + (context->hslot - BG_START_SLOT) * 2;
			if (dst >= context->done_output) {
				*dst = bg_color;
			}
			dst++;
			if (dst >= context->done_output) {
				*dst = bg_color;
			}
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		CHECK_LIMIT
	case 166:
		if (context->state == PREPARING) {
			uint32_t bg_color = context->colors[context->regs[REG_BG_COLOR] & 0x3F];
			uint32_t *dst = context->output + (context->hslot - BG_START_SLOT) * 2;
			if (dst >= context->done_output) {
				*dst = bg_color;
			}
			dst++;
			if (dst >= context->done_output) {
				*dst = bg_color;
			}
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		if (context->vcounter == context->inactive_start) {
			context->hslot++;
			context->cycles += slot_cycles;
			return;
		}
		CHECK_LIMIT
	//sprite attribute table scan starts
	case 167:
		if (context->state == PREPARING) {
			uint32_t bg_color = context->colors[context->regs[REG_BG_COLOR] & 0x3F];
			uint32_t *dst = context->output + (context->hslot - BG_START_SLOT) * 2;
			for (int i = 0; i < LINEBUF_SIZE - 2 * (context->hslot - BG_START_SLOT); i++, dst++)
			{
				if (dst >= context->done_output) {
					*dst = bg_color;
				}
			}
		}
		context->sprite_index = 0x80;
		context->slot_counter = 0;
		render_border_garbage(
			context,
			context->sprite_draw_list[context->cur_slot].address,
			context->tmp_buf_b, context->buf_b_off,
			context->col_1
		);
		render_sprite_cells(context);
		scan_sprite_table(context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H40(168)
	SPRITE_RENDER_H40(169)
	SPRITE_RENDER_H40(170)
	SPRITE_RENDER_H40(171)
	SPRITE_RENDER_H40(172)
	SPRITE_RENDER_H40(173)
	SPRITE_RENDER_H40(174)
	SPRITE_RENDER_H40(175)
	SPRITE_RENDER_H40(176)
	SPRITE_RENDER_H40(177)//End of border?
	SPRITE_RENDER_H40(178)
	SPRITE_RENDER_H40(179)
	SPRITE_RENDER_H40(180)
	SPRITE_RENDER_H40(181)
	SPRITE_RENDER_H40(182)
	SPRITE_RENDER_H40(229)
	//!HSYNC asserted
	SPRITE_RENDER_H40(230)
	SPRITE_RENDER_H40(231)
	case 232:
		external_slot(context);
		CHECK_LIMIT_HSYNC(232)
	SPRITE_RENDER_H40(233)
	SPRITE_RENDER_H40(234)
	SPRITE_RENDER_H40(235)
	SPRITE_RENDER_H40(236)
	SPRITE_RENDER_H40(237)
	SPRITE_RENDER_H40(238)
	SPRITE_RENDER_H40(239)
	SPRITE_RENDER_H40(240)
	SPRITE_RENDER_H40(241)
	SPRITE_RENDER_H40(242)
	SPRITE_RENDER_H40(243) //provides "garbage" for border when plane A selected
	case 244:
		address = (context->regs[REG_HSCROLL] & 0x3F) << 10;
		mask = 0;
		if (context->regs[REG_MODE_3] & 0x2) {
			mask |= 0xF8;
		}
		if (context->regs[REG_MODE_3] & 0x1) {
			mask |= 0x7;
		}
		render_border_garbage(context, address, context->tmp_buf_a, context->buf_a_off+8, context->col_2);
		address += (context->vcounter & mask) * 4;
		context->hscroll_a = context->vdpmem[address] << 8 | context->vdpmem[address+1];
		context->hscroll_b = context->vdpmem[address+2] << 8 | context->vdpmem[address+3];
		//printf("%d: HScroll A: %d, HScroll B: %d\n", context->vcounter, context->hscroll_a, context->hscroll_b);
		if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, -1); }
		context->hslot++;
		context->cycles += h40_hsync_cycles[14];
		CHECK_ONLY //provides "garbage" for border when plane A selected
	//!HSYNC high
	SPRITE_RENDER_H40(245)
	SPRITE_RENDER_H40(246)
	SPRITE_RENDER_H40(247) //provides "garbage" for border when plane B selected
	SPRITE_RENDER_H40(248) //provides "garbage" for border when plane B selected
	case 249:
		read_map_scroll_a(0, context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H40(250)
	case 251:
		render_map_1(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 252:
		render_map_2(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 253:
		read_map_scroll_b(0, context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H40(254)
	case 255:
		render_map_3(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 0:
		render_map_output(context->vcounter, 0, context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		//seems like the sprite table scan fills a shift register
		//values are FIFO, but unused slots precede used slots
		//so we set cur_slot to slot_counter and let it wrap around to
		//the beginning of the list
		context->cur_slot = context->slot_counter;
		context->sprite_draws = MAX_DRAWS;
		context->flags &= (~FLAG_CAN_MASK & ~FLAG_MASKED);
		CHECK_LIMIT
	COLUMN_RENDER_BLOCK(2, 1)
	COLUMN_RENDER_BLOCK(4, 9)
	COLUMN_RENDER_BLOCK(6, 17)
	COLUMN_RENDER_BLOCK_REFRESH(8, 25)
	COLUMN_RENDER_BLOCK(10, 33)
	COLUMN_RENDER_BLOCK(12, 41)
	COLUMN_RENDER_BLOCK(14, 49)
	COLUMN_RENDER_BLOCK_REFRESH(16, 57)
	COLUMN_RENDER_BLOCK(18, 65)
	COLUMN_RENDER_BLOCK(20, 73)
	COLUMN_RENDER_BLOCK(22, 81)
	COLUMN_RENDER_BLOCK_REFRESH(24, 89)
	COLUMN_RENDER_BLOCK(26, 97)
	COLUMN_RENDER_BLOCK(28, 105)
	COLUMN_RENDER_BLOCK(30, 113)
	COLUMN_RENDER_BLOCK_REFRESH(32, 121)
	COLUMN_RENDER_BLOCK(34, 129)
	COLUMN_RENDER_BLOCK(36, 137)
	COLUMN_RENDER_BLOCK(38, 145)
	COLUMN_RENDER_BLOCK_REFRESH(40, 153)
	case 161:
		external_slot(context);
		CHECK_LIMIT
	case 162:
		external_slot(context);
		CHECK_LIMIT
	//sprite render to line buffer starts
	case 163:
		context->cur_slot = MAX_DRAWS-1;
		memset(context->linebuf, 0, LINEBUF_SIZE);
		render_border_garbage(
			context,
			context->sprite_draw_list[context->cur_slot].address,
			context->tmp_buf_a, context->buf_a_off,
			context->col_1
		);
		render_sprite_cells(context);
		CHECK_LIMIT
	case 164:
		render_border_garbage(
			context,
			context->sprite_draw_list[context->cur_slot].address,
			context->tmp_buf_a, context->buf_a_off + 8,
			context->col_2
		);
		render_sprite_cells(context);
		if (context->flags & FLAG_DMA_RUN) {
			run_dma_src(context, -1);
		}
		context->hslot++;
		context->cycles += slot_cycles;
		vdp_advance_line(context);
		CHECK_ONLY
	}
	default:
		context->hslot++;
		context->cycles += slot_cycles;
		return;
	}
}

static void vdp_h32(vdp_context * context, uint32_t target_cycles)
{
	uint16_t address;
	uint32_t mask;
	uint32_t const slot_cycles = MCLKS_SLOT_H32;
	switch(context->hslot)
	{
	for (;;)
	{
	case 133:
		if (context->state == PREPARING) {
			uint32_t bg_color = context->colors[context->regs[REG_BG_COLOR] & 0x3F];
			uint32_t *dst = context->output + (context->hslot - BG_START_SLOT) * 2;
			if (dst >= context->done_output) {
				*dst = bg_color;
			}
			dst++;
			if (dst >= context->done_output) {
				*dst = bg_color;
			}
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		CHECK_LIMIT
	case 134:
		if (context->state == PREPARING) {
			uint32_t bg_color = context->colors[context->regs[REG_BG_COLOR] & 0x3F];
			uint32_t *dst = context->output + (context->hslot - BG_START_SLOT) * 2;
			if (dst >= context->done_output) {
				*dst = bg_color;
			}
			dst++;
			if (dst >= context->done_output) {
				*dst = bg_color;
			}
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		if (context->vcounter == context->inactive_start) {
			context->hslot++;
			context->cycles += slot_cycles;
			return;
		}
		CHECK_LIMIT
	//sprite attribute table scan starts
	case 135:
		if (context->state == PREPARING) {
			uint32_t bg_color = context->colors[context->regs[REG_BG_COLOR] & 0x3F];
			uint32_t *dst = context->output + (context->hslot - BG_START_SLOT) * 2;
			for (int i = 0; i < (256+HORIZ_BORDER) - 2 * (context->hslot - BG_START_SLOT); i++)
			{
				if (dst >= context->done_output) {
					*(dst++) = bg_color;
				}
			}
		}
		context->sprite_index = 0x80;
		context->slot_counter = 0;
		render_border_garbage(
			context,
			context->sprite_draw_list[context->cur_slot].address,
			context->tmp_buf_b, context->buf_b_off,
			context->col_1
		);
		render_sprite_cells(context);
		scan_sprite_table(context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H32(136)
	SPRITE_RENDER_H32(137)
	SPRITE_RENDER_H32(138)
	SPRITE_RENDER_H32(139)
	SPRITE_RENDER_H32(140)
	SPRITE_RENDER_H32(141)
	SPRITE_RENDER_H32(142)
	SPRITE_RENDER_H32(143)
	SPRITE_RENDER_H32(144)
	case 145:
		external_slot(context);
		CHECK_LIMIT
	SPRITE_RENDER_H32(146)
	SPRITE_RENDER_H32(147)
	SPRITE_RENDER_H32(233)
	SPRITE_RENDER_H32(234)
	SPRITE_RENDER_H32(235)
	//HSYNC start
	SPRITE_RENDER_H32(236)
	SPRITE_RENDER_H32(237)
	SPRITE_RENDER_H32(238)
	SPRITE_RENDER_H32(239)
	SPRITE_RENDER_H32(240)
	SPRITE_RENDER_H32(241)
	SPRITE_RENDER_H32(242)
	case 243:
		if (!(context->regs[REG_MODE_3] & BIT_VSCROLL)) {
			//TODO: Develop some tests on hardware to see when vscroll latch actually happens for full plane mode
			//Top Gear 2 has a very efficient HINT routine that can occassionally hit this slot with a VSRAM write
			//Since CRAM-updatnig HINT routines seem to indicate that my HINT latency is perhaps slightly too high
			//the most reasonable explanation is that vscroll is latched before this slot, but tests are needed
			//to confirm that one way or another
			context->vscroll_latch[0] = context->vsram[0];
			context->vscroll_latch[1] = context->vsram[1];
		}
		external_slot(context);
		//provides "garbage" for border when plane A selected
		render_border_garbage(
				context,
				context->sprite_draw_list[context->cur_slot].address,
				context->tmp_buf_a,
				context->buf_a_off,
				context->col_1
			);
		CHECK_LIMIT
	case 244:
		address = (context->regs[REG_HSCROLL] & 0x3F) << 10;
		mask = 0;
		if (context->regs[REG_MODE_3] & 0x2) {
			mask |= 0xF8;
		}
		if (context->regs[REG_MODE_3] & 0x1) {
			mask |= 0x7;
		}
		render_border_garbage(context, address, context->tmp_buf_a, context->buf_a_off+8, context->col_2);
		address += (context->vcounter & mask) * 4;
		context->hscroll_a = context->vdpmem[address] << 8 | context->vdpmem[address+1];
		context->hscroll_b = context->vdpmem[address+2] << 8 | context->vdpmem[address+3];
		//printf("%d: HScroll A: %d, HScroll B: %d\n", context->vcounter, context->hscroll_a, context->hscroll_b);
		CHECK_LIMIT //provides "garbage" for border when plane A selected
	SPRITE_RENDER_H32(245)
	SPRITE_RENDER_H32(246)
	SPRITE_RENDER_H32(247) //provides "garbage" for border when plane B selected
	SPRITE_RENDER_H32(248) //provides "garbage" for border when plane B selected
	//!HSYNC high
	case 249:
		read_map_scroll_a(0, context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H32(250)
	case 251:
		render_map_1(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 252:
		render_map_2(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 253:
		read_map_scroll_b(0, context->vcounter, context);
		CHECK_LIMIT
	case 254:
		render_sprite_cells(context);
		scan_sprite_table(context->vcounter, context);
		CHECK_LIMIT
	case 255:
		render_map_3(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 0:
		render_map_output(context->vcounter, 0, context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		//reverse context slot counter so it counts the number of sprite slots
		//filled rather than the number of available slots
		//context->slot_counter = MAX_SPRITES_LINE - context->slot_counter;
		context->cur_slot = context->slot_counter;
		context->sprite_draws = MAX_DRAWS_H32;
		context->flags &= (~FLAG_CAN_MASK & ~FLAG_MASKED);
		CHECK_LIMIT
	COLUMN_RENDER_BLOCK(2, 1)
	COLUMN_RENDER_BLOCK(4, 9)
	COLUMN_RENDER_BLOCK(6, 17)
	COLUMN_RENDER_BLOCK_REFRESH(8, 25)
	COLUMN_RENDER_BLOCK(10, 33)
	COLUMN_RENDER_BLOCK(12, 41)
	COLUMN_RENDER_BLOCK(14, 49)
	COLUMN_RENDER_BLOCK_REFRESH(16, 57)
	COLUMN_RENDER_BLOCK(18, 65)
	COLUMN_RENDER_BLOCK(20, 73)
	COLUMN_RENDER_BLOCK(22, 81)
	COLUMN_RENDER_BLOCK_REFRESH(24, 89)
	COLUMN_RENDER_BLOCK(26, 97)
	COLUMN_RENDER_BLOCK(28, 105)
	COLUMN_RENDER_BLOCK(30, 113)
	COLUMN_RENDER_BLOCK_REFRESH(32, 121)
	case 129:
		external_slot(context);
		CHECK_LIMIT
	case 130: {
		external_slot(context);
		CHECK_LIMIT
	}
	//sprite render to line buffer starts
	case 131:
		context->cur_slot = MAX_DRAWS_H32-1;
		memset(context->linebuf, 0, LINEBUF_SIZE);
		render_border_garbage(
			context,
			context->sprite_draw_list[context->cur_slot].address,
			context->tmp_buf_a, context->buf_a_off,
			context->col_1
		);
		render_sprite_cells(context);
		CHECK_LIMIT
	case 132:
		render_border_garbage(
			context,
			context->sprite_draw_list[context->cur_slot].address,
			context->tmp_buf_a, context->buf_a_off + 8,
			context->col_2
		);
		render_sprite_cells(context);
		if (context->flags & FLAG_DMA_RUN) {
			run_dma_src(context, -1);
		}
		context->hslot++;
		context->cycles += slot_cycles;
		vdp_advance_line(context);
		CHECK_ONLY
	}
	default:
		context->hslot++;
		context->cycles += MCLKS_SLOT_H32;
	}
}

static void vdp_h32_mode4(vdp_context * context, uint32_t target_cycles)
{
	uint16_t address;
	uint32_t mask;
	uint32_t const slot_cycles = MCLKS_SLOT_H32;
	switch(context->hslot)
	{
	for (;;)
	{
	//sprite rendering starts
	SPRITE_RENDER_H32_MODE4(137)
	SPRITE_RENDER_H32_MODE4(143)
	case 234:
		external_slot(context);
		CHECK_LIMIT
	case 235:
		external_slot(context);
		CHECK_LIMIT
	//!HSYNC low
	case 236:
		external_slot(context);
		CHECK_LIMIT
	case 237:
		external_slot(context);
		CHECK_LIMIT
	case 238:
		external_slot(context);
		CHECK_LIMIT
	SPRITE_RENDER_H32_MODE4(239)
	SPRITE_RENDER_H32_MODE4(245)
	case 251:
		external_slot(context);
		CHECK_LIMIT
	case 252:
		external_slot(context);
		if (context->regs[REG_MODE_1] & BIT_HSCRL_LOCK && context->vcounter < 16) {
			context->hscroll_a = 0;
		} else {
			context->hscroll_a = context->regs[REG_X_SCROLL];
		}
		CHECK_LIMIT
	case 253:
		context->sprite_index = 0;
		context->slot_counter = MAX_DRAWS_H32_MODE4;
		scan_sprite_table_mode4(context);
		CHECK_LIMIT
	case 254:
		scan_sprite_table_mode4(context);
		CHECK_LIMIT
	case 255:
		scan_sprite_table_mode4(context);
		CHECK_LIMIT
	case 0: {
		scan_sprite_table_mode4(context);
		uint32_t *dst = context->output;;
		uint32_t bgcolor = context->colors[0x10 | (context->regs[REG_BG_COLOR] & 0xF) + CRAM_SIZE*3];
		for (int i = 0; i < BORDER_LEFT-8; i++, dst++)
		{
			*dst = bgcolor;
		}
		context->done_output = dst;
		CHECK_LIMIT
	}
	case 1:
		scan_sprite_table_mode4(context);
		CHECK_LIMIT
	case 2:
		scan_sprite_table_mode4(context);
		CHECK_LIMIT
	case 3:
		scan_sprite_table_mode4(context);
		CHECK_LIMIT
	case 4: {
		scan_sprite_table_mode4(context);
		context->buf_a_off = 8;
		memset(context->tmp_buf_a, 0, 8);
		uint32_t *dst = context->output + BORDER_LEFT - 8;
		uint32_t bgcolor = context->colors[0x10 | (context->regs[REG_BG_COLOR] & 0xF) + CRAM_SIZE*3];
		for (int i = 0; i < 8; i++, dst++)
		{
			*dst = bgcolor;
		}
		context->done_output = dst;
		CHECK_LIMIT
	}
	COLUMN_RENDER_BLOCK_MODE4(0, 5)
	COLUMN_RENDER_BLOCK_MODE4(1, 9)
	COLUMN_RENDER_BLOCK_MODE4(2, 13)
	COLUMN_RENDER_BLOCK_MODE4(3, 17)
	COLUMN_RENDER_BLOCK_MODE4(4, 21)
	COLUMN_RENDER_BLOCK_MODE4(5, 25)
	COLUMN_RENDER_BLOCK_MODE4(6, 29)
	COLUMN_RENDER_BLOCK_MODE4(7, 33)
	COLUMN_RENDER_BLOCK_MODE4(8, 37)
	COLUMN_RENDER_BLOCK_MODE4(9, 41)
	COLUMN_RENDER_BLOCK_MODE4(10, 45)
	COLUMN_RENDER_BLOCK_MODE4(11, 49)
	COLUMN_RENDER_BLOCK_MODE4(12, 53)
	COLUMN_RENDER_BLOCK_MODE4(13, 57)
	COLUMN_RENDER_BLOCK_MODE4(14, 61)
	COLUMN_RENDER_BLOCK_MODE4(15, 65)
	COLUMN_RENDER_BLOCK_MODE4(16, 69)
	COLUMN_RENDER_BLOCK_MODE4(17, 73)
	COLUMN_RENDER_BLOCK_MODE4(18, 77)
	COLUMN_RENDER_BLOCK_MODE4(19, 81)
	COLUMN_RENDER_BLOCK_MODE4(20, 85)
	COLUMN_RENDER_BLOCK_MODE4(21, 89)
	COLUMN_RENDER_BLOCK_MODE4(22, 93)
	COLUMN_RENDER_BLOCK_MODE4(23, 97)
	COLUMN_RENDER_BLOCK_MODE4(24, 101)
	COLUMN_RENDER_BLOCK_MODE4(25, 105)
	COLUMN_RENDER_BLOCK_MODE4(26, 109)
	COLUMN_RENDER_BLOCK_MODE4(27, 113)
	COLUMN_RENDER_BLOCK_MODE4(28, 117)
	COLUMN_RENDER_BLOCK_MODE4(29, 121)
	COLUMN_RENDER_BLOCK_MODE4(30, 125)
	COLUMN_RENDER_BLOCK_MODE4(31, 129)
	case 133:
		external_slot(context);
		CHECK_LIMIT
	case 134:
		external_slot(context);
		CHECK_LIMIT
	case 135:
		external_slot(context);
		CHECK_LIMIT
	case 136: {
		external_slot(context);
		//set things up for sprite rendering in the next slot
		memset(context->linebuf, 0, LINEBUF_SIZE);
		context->cur_slot = context->sprite_index = MAX_DRAWS_H32_MODE4-1;
		context->sprite_draws = MAX_DRAWS_H32_MODE4;
		uint32_t *dst = context->output + BORDER_LEFT + 256;
		uint32_t bgcolor = context->colors[0x10 | (context->regs[REG_BG_COLOR] & 0xF) + CRAM_SIZE*3];
		for (int i = 0; i < 8; i++, dst++)
		{
			*dst = bgcolor;
		}
		context->done_output = dst;
		CHECK_LIMIT
	}}
	default:
		context->hslot++;
		context->cycles += MCLKS_SLOT_H32;
	}
}

static void inactive_test_output(vdp_context *context, uint8_t is_h40, uint8_t test_layer)
{
	uint8_t max_slot = is_h40 ? 169 : 136;
	if (context->hslot > max_slot) {
		return;
	}
	uint32_t *dst = context->output + (context->hslot >> 3) * SCROLL_BUFFER_DRAW;
	int32_t len;
	uint32_t src_off;
	if (context->hslot) {
		dst -= SCROLL_BUFFER_DRAW - BORDER_LEFT;
		src_off = 0;
		len = context->hslot == max_slot ? BORDER_RIGHT : SCROLL_BUFFER_DRAW;
	} else {
		src_off = SCROLL_BUFFER_DRAW - BORDER_LEFT;
		len = BORDER_LEFT;
	}
	uint8_t *src;
	if (test_layer == 2) {
		//plane A
		src_off += context->buf_a_off + context->hscroll_a;
		src = context->tmp_buf_a;
	} else if (test_layer == 3){
		//plane B
		src_off += context->buf_b_off + context->hscroll_b;
		src = context->tmp_buf_b;
	} else {
		//sprite layer
		for (; len >=0; len--, dst++, src_off++)
		{
			*dst = context->colors[0];
		}
	}
	for (; len >=0; len--, dst++, src_off++)
	{
		*dst = context->colors[src[src_off & SCROLL_BUFFER_MASK] & 0x3F];
	}
	context->done_output = dst;
	context->buf_a_off = (context->buf_a_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_DRAW;
	context->buf_b_off = (context->buf_b_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_DRAW;
}

static void check_switch_inactive(vdp_context *context, uint8_t is_h40)
{
	//technically the second hcounter check should be different for H40, but this is probably close enough for now
	if (context->state == ACTIVE && context->vcounter == context->inactive_start && (context->hslot >= (is_h40 ? 167 : 135) || context->hslot < 133)) {
		context->state = INACTIVE;
	}
}

static void vdp_inactive(vdp_context *context, uint32_t target_cycles, uint8_t is_h40, uint8_t mode_5)
{
	uint8_t buf_clear_slot, index_reset_slot, bg_end_slot, vint_slot, line_change, jump_start, jump_dest, latch_slot;
	uint8_t index_reset_value, max_draws, max_sprites;
	uint16_t vint_line, active_line;
	uint32_t bg_color;
	
	if (mode_5) {
		if (is_h40) {
			latch_slot = 165;
			buf_clear_slot = 163;
			index_reset_slot = 167;
			bg_end_slot = BG_START_SLOT + LINEBUF_SIZE/2;
			max_draws = MAX_DRAWS-1;
			max_sprites = MAX_SPRITES_LINE;
			index_reset_value = 0x80;
			vint_slot = VINT_SLOT_H40;
			line_change = LINE_CHANGE_H40;
			jump_start = 182;
			jump_dest = 229;
		} else {
			bg_end_slot = BG_START_SLOT + (256+HORIZ_BORDER)/2;
			max_draws = MAX_DRAWS_H32-1;
			max_sprites = MAX_SPRITES_LINE_H32;
			buf_clear_slot = 128;
			index_reset_slot = 132;
			index_reset_value = 0x80;
			vint_slot = VINT_SLOT_H32;
			line_change = LINE_CHANGE_H32;
			jump_start = 147;
			jump_dest = 233;
			latch_slot = 243;
		}
		vint_line = context->inactive_start;
		active_line = 0x1FF;
		if (context->regs[REG_MODE_3] & BIT_VSCROLL) {
			latch_slot = 220;
		}
	} else {
		latch_slot = 220;
		bg_end_slot = BG_START_SLOT + (256+HORIZ_BORDER)/2;
		max_draws = MAX_DRAWS_H32_MODE4;
		max_sprites = 8;
		buf_clear_slot = 136;
		index_reset_slot = 253;
		index_reset_value = 0;
		vint_line = context->inactive_start + 1;
		vint_slot = VINT_SLOT_MODE4;
		line_change = LINE_CHANGE_MODE4;
		bg_color = render_map_color(0, 0, 0);
		jump_start = 147;
		jump_dest = 233;
		if (context->regs[REG_MODE_1] & BIT_MODE_4) {
			active_line = 0x1FF;
		} else {
			//never active unless either mode 4 or mode 5 is turned on
			active_line = 0x200;
		}
	}
	uint32_t *dst = (
		context->vcounter < context->inactive_start + context->border_bot 
		|| context->vcounter >= 0x200 - context->border_top
	) && context->hslot >= BG_START_SLOT && context->hslot < bg_end_slot
		? context->output + 2 * (context->hslot - BG_START_SLOT)
		: NULL;
		
	if (
		!dst && context->vcounter == context->inactive_start + context->border_bot
		&& context->hslot >= line_change  && context->hslot < bg_end_slot
	) {
		dst = context->output + 2 * (context->hslot - BG_START_SLOT);
	}
		
	uint8_t test_layer = context->test_port >> 7 & 3;
	if (test_layer) {
		dst = NULL;
	}
	
	while(context->cycles < target_cycles)
	{
		check_switch_inactive(context, is_h40);
		if (context->hslot == BG_START_SLOT && !test_layer && (
			context->vcounter < context->inactive_start + context->border_bot 
			|| context->vcounter >= 0x200 - context->border_top
		)) {
			dst = context->output + (context->hslot - BG_START_SLOT) * 2;
		} else if (context->hslot == bg_end_slot) {
			advance_output_line(context);
			dst = NULL;
		}
		//this will need some tweaking to properly interact with 128K mode, 
		//but this should be good enough for now
		context->serial_address += 1024;
		if (test_layer) {
			switch (context->hslot & 7)
			{
			case 3:
				render_border_garbage(context, context->serial_address, context->tmp_buf_a, context->buf_a_off, context->col_1);
				break;
			case 4:
				render_border_garbage(context, context->serial_address, context->tmp_buf_a, context->buf_a_off+8, context->col_2);
				break;
			case 7:
				render_border_garbage(context, context->serial_address, context->tmp_buf_b, context->buf_b_off, context->col_1);
				break;
			case 0:
				render_border_garbage(context, context->serial_address, context->tmp_buf_b, context->buf_b_off+8, context->col_2);
				inactive_test_output(context, is_h40, test_layer);
				break;
			}
		}
		
		if (context->hslot == buf_clear_slot) {
			if (mode_5) {
				context->cur_slot = max_draws;
			} else {
				context->cur_slot = context->sprite_index = MAX_DRAWS_H32_MODE4-1;
				context->sprite_draws = MAX_DRAWS_H32_MODE4;
			}
			memset(context->linebuf, 0, LINEBUF_SIZE);
		} else if (context->hslot == index_reset_slot) {
			context->sprite_index = index_reset_value;
			context->slot_counter = mode_5 ? 0 : max_sprites;
		} else if (context->hslot == latch_slot) {
			//it seems unlikely to me that vscroll actually gets latched when the display is off
			//but it's the only straightforward way to reconcile what I'm seeing between Skitchin 
			//(which seems to expect vscroll to be latched early) and the intro of Gunstar Heroes
			//(which disables the display and ends up with garbage if vscroll is latched during that period)
			//without it. Some more tests are definitely needed
			context->vscroll_latch[0] = context->vsram[0];
			context->vscroll_latch[1] = context->vsram[1];
		} else if (context->vcounter == vint_line && context->hslot == vint_slot) {
			context->flags2 |= FLAG2_VINT_PENDING;
			context->pending_vint_start = context->cycles;
		} else if (context->vcounter == context->inactive_start && context->hslot == 1 && (context->regs[REG_MODE_4] & BIT_INTERLACE)) {
			context->flags2 ^= FLAG2_EVEN_FIELD;
		}
		
		if (dst) {
			if (mode_5) {
				bg_color = context->colors[context->regs[REG_BG_COLOR] & 0x3F];
			} else if (context->regs[REG_MODE_1] & BIT_MODE_4) {
				bg_color = context->colors[CRAM_SIZE * 3 + 0x10 + (context->regs[REG_BG_COLOR] & 0xF)];
			}
			if (dst >= context->done_output) {
				*(dst++) = bg_color;
			} else {
				dst++;
			}
			if (dst >= context->done_output) {
				*(dst++) = bg_color;
				context->done_output = dst;
			} else {
				dst++;
			}
			if (context->hslot == (bg_end_slot-1)) {
				*(dst++) = bg_color;
				context->done_output = dst;
			}
		}
		
		if (!is_refresh(context, context->hslot)) {
			external_slot(context);
			if (context->flags & FLAG_DMA_RUN && !is_refresh(context, context->hslot)) {
				run_dma_src(context, context->hslot);
			}
		}
		
		if (is_h40) {
			if (context->hslot >= HSYNC_SLOT_H40 && context->hslot < HSYNC_END_H40) {
				context->cycles += h40_hsync_cycles[context->hslot - HSYNC_SLOT_H40];
			} else {
				context->cycles += MCLKS_SLOT_H40;
			}
		} else {
			context->cycles += MCLKS_SLOT_H32;
		}
		if (context->hslot == jump_start) {
			context->hslot = jump_dest;
		} else {
			context->hslot++;
		}
		if (context->hslot == line_change) {
			vdp_advance_line(context);
			if (context->vcounter == active_line) {
				context->state = PREPARING;
				return;
			}
		}
	}
}

void vdp_run_context_full(vdp_context * context, uint32_t target_cycles)
{
	uint8_t is_h40 = context->regs[REG_MODE_4] & BIT_H40;
	uint8_t mode_5 = context->regs[REG_MODE_2] & BIT_MODE_5;
	while(context->cycles < target_cycles)
	{
		check_switch_inactive(context, is_h40);
		
		if (is_active(context)) {
			if (mode_5) {
				if (is_h40) {
					vdp_h40(context, target_cycles);
				} else {
					vdp_h32(context, target_cycles);
				}
			} else {
				vdp_h32_mode4(context, target_cycles);
			}
		} else {
			vdp_inactive(context, target_cycles, is_h40, mode_5);
		}
	}
}

void vdp_run_context(vdp_context *context, uint32_t target_cycles)
{
	//TODO: Deal with H40 hsync shenanigans
	uint32_t slot_cyc = context->regs[REG_MODE_4] & BIT_H40 ? 15 : 19;
	if (target_cycles < slot_cyc) {
		//avoid overflow
		return;
	}
	vdp_run_context_full(context, target_cycles - slot_cyc);
}

uint32_t vdp_run_to_vblank(vdp_context * context)
{
	uint32_t old_frame = context->frame;
	while (context->frame == old_frame) {
		vdp_run_context_full(context, context->cycles + MCLKS_LINE);
	}
	return context->cycles;
}

void vdp_run_dma_done(vdp_context * context, uint32_t target_cycles)
{
	for(;;) {
		uint32_t dmalen = (context->regs[REG_DMALEN_H] << 8) | context->regs[REG_DMALEN_L];
		if (!dmalen) {
			dmalen = 0x10000;
		}
		uint32_t min_dma_complete = dmalen * (context->regs[REG_MODE_4] & BIT_H40 ? 16 : 20);
		if (
			(context->regs[REG_DMASRC_H] & 0xC0) == 0xC0 
			|| (((context->cd & 0xF) == VRAM_WRITE) && !(context->regs[REG_MODE_2] & BIT_128K_VRAM))) {
			//DMA copies take twice as long to complete since they require a read and a write
			//DMA Fills and transfers to VRAM also take twice as long as it requires 2 writes for a single word
			//unless 128KB mode is enabled
			min_dma_complete *= 2;
		}
		min_dma_complete += context->cycles;
		if (target_cycles < min_dma_complete) {
			vdp_run_context_full(context, target_cycles);
			return;
		} else {
			vdp_run_context_full(context, min_dma_complete);
			if (!(context->flags & FLAG_DMA_RUN)) {
				return;
			}
		}
	}
}

static uint16_t get_ext_vcounter(vdp_context *context)
{
	uint16_t line= context->vcounter;
	if (context->regs[REG_MODE_4] & BIT_INTERLACE) {
		if (context->double_res) {
			line <<= 1;
		} else {
			line &= 0x1FE;
		}
		if (line & 0x100) {
			line |= 1;
		}
	}
	return line << 8;
}

void vdp_latch_hv(vdp_context *context)
{
	context->hv_latch = context->hslot | get_ext_vcounter(context);
}

uint16_t vdp_hv_counter_read(vdp_context * context)
{
	if ((context->regs[REG_MODE_2] & BIT_MODE_5) && (context->regs[REG_MODE_1] & BIT_HVC_LATCH)) {
		return context->hv_latch;
	}
	uint16_t hv;
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		hv = context->hslot;
	} else {
		hv = context->hv_latch & 0xFF;
	}
	hv |= get_ext_vcounter(context);
	
	return hv;
}

int vdp_control_port_write(vdp_context * context, uint16_t value)
{
	//printf("control port write: %X at %d\n", value, context->cycles);
	if (context->flags & FLAG_DMA_RUN) {
		return -1;
	}
	if (context->flags & FLAG_PENDING) {
		context->address = (context->address & 0x3FFF) | (value << 14 & 0x1C000);
		//It seems like the DMA enable bit doesn't so much enable DMA so much 
		//as it enables changing CD5 from control port writes
		uint8_t preserve = (context->regs[REG_MODE_2] & BIT_DMA_ENABLE) ? 0x3 : 0x23;
		context->cd = (context->cd & preserve) | ((value >> 2) & ~preserve & 0xFF);
		context->flags &= ~FLAG_PENDING;
		//Should these be taken care of here or after the first write?
		context->flags &= ~FLAG_READ_FETCHED;
		context->flags2 &= ~FLAG2_READ_PENDING;
		//printf("New Address: %X, New CD: %X\n", context->address, context->cd);
		if (context->cd & 0x20) {
			//
			if((context->regs[REG_DMASRC_H] & 0xC0) != 0x80) {
				//DMA copy or 68K -> VDP, transfer starts immediately
				//printf("DMA start (length: %X) at cycle %d, frame: %d, vcounter: %d, hslot: %d\n", (context->regs[REG_DMALEN_H] << 8) | context->regs[REG_DMALEN_L], context->cycles, context->frame, context->vcounter, context->hslot);
				if (!(context->regs[REG_DMASRC_H] & 0x80)) {
					//printf("DMA Address: %X, New CD: %X, Source: %X, Length: %X\n", context->address, context->cd, (context->regs[REG_DMASRC_H] << 17) | (context->regs[REG_DMASRC_M] << 9) | (context->regs[REG_DMASRC_L] << 1), context->regs[REG_DMALEN_H] << 8 | context->regs[REG_DMALEN_L]);
					//68K -> VDP DMA takes a few slots to actually start reading even though it acquires the bus immediately
					//logic analyzer captures made it seem like the proper value is 4 slots, but that seems to cause trouble with the Nemesis' FIFO Wait State test
					//only captures are from a direct color DMA demo which will generally start DMA at a very specific point in display so other values are plausible
					//sticking with 3 slots for now until I can do some more captures
					vdp_run_context_full(context, context->cycles + 12 * ((context->regs[REG_MODE_2] & BIT_MODE_5) && (context->regs[REG_MODE_4] & BIT_H40) ? 4 : 5));
					context->flags |= FLAG_DMA_RUN;
					return 1;
				} else {
					context->flags |= FLAG_DMA_RUN;
					//printf("DMA Copy Address: %X, New CD: %X, Source: %X\n", context->address, context->cd, (context->regs[REG_DMASRC_M] << 8) | context->regs[REG_DMASRC_L]);
				}
			} else {
				//printf("DMA Fill Address: %X, New CD: %X\n", context->address, context->cd);
			}
		}
	} else {
		uint8_t mode_5 = context->regs[REG_MODE_2] & BIT_MODE_5;
		context->address = (context->address &0xC000) | (value & 0x3FFF);
		context->cd = (context->cd & 0x3C) | (value >> 14);
		if ((value & 0xC000) == 0x8000) {
			//Register write
			uint8_t reg = (value >> 8) & 0x1F;
			if (reg < (mode_5 ? VDP_REGS : 0xB)) {
				//printf("register %d set to %X\n", reg, value & 0xFF);
				if (reg == REG_MODE_1 && (value & BIT_HVC_LATCH) && !(context->regs[reg] & BIT_HVC_LATCH)) {
					vdp_latch_hv(context);
				}
				if (reg == REG_BG_COLOR) {
					value &= 0x3F;
				}
				/*if (reg == REG_MODE_4 && ((value ^ context->regs[reg]) & BIT_H40)) {
					printf("Mode changed from H%d to H%d @ %d, frame: %d\n", context->regs[reg] & BIT_H40 ? 40 : 32, value & BIT_H40 ? 40 : 32, context->cycles, context->frame);
				}*/
				context->regs[reg] = value;
				if (reg == REG_MODE_4) {
					context->double_res = (value & (BIT_INTERLACE | BIT_DOUBLE_RES)) == (BIT_INTERLACE | BIT_DOUBLE_RES);
					if (!context->double_res) {
						context->flags2 &= ~FLAG2_EVEN_FIELD;
					}
				}
				if (reg == REG_MODE_1 || reg == REG_MODE_2 || reg == REG_MODE_4) {
					update_video_params(context);
				}
			}
		} else if (mode_5) {
			context->flags |= FLAG_PENDING;
			//Should these be taken care of here or after the second write?
			//context->flags &= ~FLAG_READ_FETCHED;
			//context->flags2 &= ~FLAG2_READ_PENDING;
		} else {
			context->flags &= ~FLAG_READ_FETCHED;
			context->flags2 &= ~FLAG2_READ_PENDING;
		}
	}
	return 0;
}

void vdp_control_port_write_pbc(vdp_context *context, uint8_t value)
{
	if (context->flags2 & FLAG2_BYTE_PENDING) {
		uint16_t full_val = value << 8 | context->pending_byte;
		context->flags2 &= ~FLAG2_BYTE_PENDING;
		//TODO: Deal with fact that Vbus->VDP DMA doesn't do anything in PBC mode
		vdp_control_port_write(context, full_val);
		if (context->cd == VRAM_READ) {
			context->cd = VRAM_READ8;
		}
	} else {
		context->pending_byte = value;
		context->flags2 |= FLAG2_BYTE_PENDING;
	}
}

int vdp_data_port_write(vdp_context * context, uint16_t value)
{
	//printf("data port write: %X at %d\n", value, context->cycles);
	if (context->flags & FLAG_DMA_RUN && (context->regs[REG_DMASRC_H] & 0xC0) != 0x80) {
		return -1;
	}
	if (context->flags & FLAG_PENDING) {
		context->flags &= ~FLAG_PENDING;
		//Should these be cleared here?
		context->flags &= ~FLAG_READ_FETCHED;
		context->flags2 &= ~FLAG2_READ_PENDING;
	}
	/*if (context->fifo_cur == context->fifo_end) {
		printf("FIFO full, waiting for space before next write at cycle %X\n", context->cycles);
	}*/
	if (context->cd & 0x20 && (context->regs[REG_DMASRC_H] & 0xC0) == 0x80) {
		context->flags &= ~FLAG_DMA_RUN;
	}
	while (context->fifo_write == context->fifo_read) {
		vdp_run_context_full(context, context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20));
	}
	fifo_entry * cur = context->fifo + context->fifo_write;
	cur->cycle = context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20)*FIFO_LATENCY;
	cur->address = context->address;
	cur->value = value;
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		cur->cd = context->cd;
	} else {
		cur->cd = (context->cd & 2) | 1;
	}
	cur->partial = 0;
	if (context->fifo_read < 0) {
		context->fifo_read = context->fifo_write;
	}
	context->fifo_write = (context->fifo_write + 1) & (FIFO_SIZE-1);
	increment_address(context);
	return 0;
}

void vdp_data_port_write_pbc(vdp_context * context, uint8_t value)
{
	if (context->flags & FLAG_PENDING) {
		context->flags &= ~FLAG_PENDING;
		//Should these be cleared here?
		context->flags &= ~FLAG_READ_FETCHED;
		context->flags2 &= ~FLAG2_READ_PENDING;
	}
	context->flags2 &= ~FLAG2_BYTE_PENDING;
	/*if (context->fifo_cur == context->fifo_end) {
		printf("FIFO full, waiting for space before next write at cycle %X\n", context->cycles);
	}*/
	if (context->cd & 0x20 && (context->regs[REG_DMASRC_H] & 0xC0) == 0x80) {
		context->flags &= ~FLAG_DMA_RUN;
	}
	while (context->fifo_write == context->fifo_read) {
		vdp_run_context_full(context, context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20));
	}
	fifo_entry * cur = context->fifo + context->fifo_write;
	cur->cycle = context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20)*FIFO_LATENCY;
	cur->address = context->address;
	cur->value = value;
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		cur->cd = context->cd;
	} else {
		cur->cd = (context->cd & 2) | 1;
	}
	cur->partial = 3;
	if (context->fifo_read < 0) {
		context->fifo_read = context->fifo_write;
	}
	context->fifo_write = (context->fifo_write + 1) & (FIFO_SIZE-1);
	increment_address(context);
}

void vdp_test_port_write(vdp_context * context, uint16_t value)
{
	context->test_port = value;
}

uint16_t vdp_control_port_read(vdp_context * context)
{
	context->flags &= ~FLAG_PENDING;
	context->flags2 &= ~FLAG2_BYTE_PENDING;
	//Bits 15-10 are not fixed like Charles MacDonald's doc suggests, but instead open bus values that reflect 68K prefetch
	uint16_t value = context->system->get_open_bus_value(context->system) & 0xFC00;
	if (context->fifo_read < 0) {
		value |= 0x200;
	}
	if (context->fifo_read == context->fifo_write) {
		value |= 0x100;
	}
	if (context->flags2 & FLAG2_VINT_PENDING) {
		value |= 0x80;
	}
	if (context->flags & FLAG_DOT_OFLOW) {
		value |= 0x40;
		context->flags &= ~FLAG_DOT_OFLOW;
	}
	if (context->flags2 & FLAG2_SPRITE_COLLIDE) {
		value |= 0x20;
		context->flags2 &= ~FLAG2_SPRITE_COLLIDE;
	}
	if ((context->regs[REG_MODE_4] & BIT_INTERLACE) && !(context->flags2 & FLAG2_EVEN_FIELD)) {
		value |= 0x10;
	}
	uint32_t slot = context->hslot;
	if (!is_active(context)) {
		value |= 0x8;
	}
	if (context->regs[REG_MODE_4] & BIT_H40) {
		if (slot < HBLANK_END_H40 || slot > HBLANK_START_H40) {
			value |= 0x4;
		}
	} else {
		if (slot < HBLANK_END_H32 || slot > HBLANK_START_H32) {
			value |= 0x4;
		}
	}
	if (context->cd & 0x20) {
		value |= 0x2;
	}
	if (context->flags2 & FLAG2_REGION_PAL) {
		value |= 0x1;
	}
	//printf("status read at cycle %d returned %X\n", context->cycles, value);
	return value;
}

uint16_t vdp_data_port_read(vdp_context * context)
{
	if (context->flags & FLAG_PENDING) {
		context->flags &= ~FLAG_PENDING;
		//Should these be cleared here?
		context->flags &= ~FLAG_READ_FETCHED;
		context->flags2 &= ~FLAG2_READ_PENDING;
	}
	if (context->cd & 1) {
		warning("Read from VDP data port while writes are configured, CPU is now frozen. VDP Address: %X, CD: %X\n", context->address, context->cd);
	}
	while (!(context->flags & FLAG_READ_FETCHED)) {
		vdp_run_context_full(context, context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20));
	}
	context->flags &= ~FLAG_READ_FETCHED;
	return context->prefetch;
}

uint8_t vdp_data_port_read_pbc(vdp_context * context)
{
	context->flags &= ~(FLAG_PENDING | FLAG_READ_FETCHED);
	context->flags2 &= ~FLAG2_BYTE_PENDING;
		
	context->cd = VRAM_READ8;
	return context->prefetch;
}

uint16_t vdp_test_port_read(vdp_context * context)
{
	//TODO: Find out what actually gets returned here
	return context->test_port;
}

void vdp_adjust_cycles(vdp_context * context, uint32_t deduction)
{
	context->cycles -= deduction;
	if (context->pending_vint_start >= deduction) {
		context->pending_vint_start -= deduction;
	} else {
		context->pending_vint_start = 0;
	}
	if (context->pending_hint_start >= deduction) {
		context->pending_hint_start -= deduction;
	} else {
		context->pending_hint_start = 0;
	}
	if (context->fifo_read >= 0) {
		int32_t idx = context->fifo_read;
		do {
			if (context->fifo[idx].cycle >= deduction) {
				context->fifo[idx].cycle -= deduction;
			} else {
				context->fifo[idx].cycle = 0;
			}
			idx = (idx+1) & (FIFO_SIZE-1);
		} while(idx != context->fifo_write);
	}
}

static uint32_t vdp_cycles_hslot_wrap_h40(vdp_context * context)
{
	if (context->hslot < 183) {
		return MCLKS_LINE - context->hslot * MCLKS_SLOT_H40;
	} else if (context->hslot < HSYNC_END_H40) {
		uint32_t before_hsync = context->hslot < HSYNC_SLOT_H40 ? (HSYNC_SLOT_H40 - context->hslot) * MCLKS_SLOT_H40 : 0;
		uint32_t hsync = 0;
		for (int i = context->hslot <= HSYNC_SLOT_H40 ? 0 : context->hslot - HSYNC_SLOT_H40; i < sizeof(h40_hsync_cycles)/sizeof(uint32_t); i++)
		{
			hsync += h40_hsync_cycles[i];
		}
		uint32_t after_hsync = (256- HSYNC_END_H40) * MCLKS_SLOT_H40;
		return before_hsync + hsync + after_hsync;
	} else {
		return (256-context->hslot) * MCLKS_SLOT_H40;
	}
}

static uint32_t vdp_cycles_next_line(vdp_context * context)
{
	if (context->regs[REG_MODE_4] & BIT_H40) {
		//TODO: Handle "illegal" Mode 4/H40 combo
		if (context->hslot < LINE_CHANGE_H40) {
			return (LINE_CHANGE_H40 - context->hslot) * MCLKS_SLOT_H40;
		} else {
			return vdp_cycles_hslot_wrap_h40(context) + LINE_CHANGE_H40 * MCLKS_SLOT_H40;
		}
	} else {
		if (context->regs[REG_MODE_2] & BIT_MODE_5) {
			if (context->hslot < LINE_CHANGE_H32) {
				return (LINE_CHANGE_H32 - context->hslot) * MCLKS_SLOT_H32;
			} else if (context->hslot < 148) {
				return MCLKS_LINE - (context->hslot - LINE_CHANGE_H32) * MCLKS_SLOT_H32;
			} else {
				return (256-context->hslot + LINE_CHANGE_H32) * MCLKS_SLOT_H32;
			}
		} else {
			if (context->hslot < 148) {
				return (148 - context->hslot + LINE_CHANGE_MODE4 - 233) * MCLKS_SLOT_H32;
			} else if (context->hslot < LINE_CHANGE_MODE4) {
				return (LINE_CHANGE_MODE4 - context->hslot) * MCLKS_SLOT_H32;
			} else {
				return MCLKS_LINE - (context->hslot - LINE_CHANGE_MODE4) * MCLKS_SLOT_H32;
			}
		}
	}
}

static void get_jump_params(vdp_context *context, uint32_t *jump_start, uint32_t *jump_dst)
{
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		if (context->flags2 & FLAG2_REGION_PAL) {
			if (context->regs[REG_MODE_2] & BIT_PAL) {
				*jump_start = 0x10B;
				*jump_dst = 0x1D2;
			} else {
				*jump_start = 0x103;
				*jump_dst = 0x1CA;
			}
		} else {
			if (context->regs[REG_MODE_2] & BIT_PAL) {
				*jump_start = 0x100;
				*jump_dst = 0x1FA;
			} else {
				*jump_start = 0xEB;
				*jump_dst = 0x1E5;
			}
		}
	} else {
		*jump_start = 0xDB;
		*jump_dst = 0x1D5;
	}
}

static uint32_t vdp_cycles_to_line(vdp_context * context, uint32_t target)
{
	uint32_t jump_start, jump_dst;
	get_jump_params(context, &jump_start, &jump_dst);
	uint32_t lines;
	if (context->vcounter < target) {
		if (target < jump_start || context->vcounter > jump_start) {
			lines = target - context->vcounter;
		} else {
			lines = jump_start - context->vcounter + target - jump_dst;
		}
	} else {
		if (context->vcounter < jump_start) {
			lines = jump_start - context->vcounter + 512 - jump_dst;
		} else {
			lines = 512 - context->vcounter;
		}
		if (target < jump_start) {
			lines += target;
		} else {
			lines += jump_start + target - jump_dst;
		}
	}
	return MCLKS_LINE * (lines - 1) + vdp_cycles_next_line(context);
}

uint32_t vdp_cycles_to_frame_end(vdp_context * context)
{
	return context->cycles + vdp_cycles_to_line(context, context->inactive_start);
}

uint32_t vdp_next_hint(vdp_context * context)
{
	if (!(context->regs[REG_MODE_1] & BIT_HINT_EN)) {
		return 0xFFFFFFFF;
	}
	if (context->flags2 & FLAG2_HINT_PENDING) {
		return context->pending_hint_start;
	}
	uint32_t hint_line;
	if (context->state != ACTIVE) {
		hint_line = context->regs[REG_HINT];
		if (hint_line > context->inactive_start) {
			return 0xFFFFFFFF;
		}
	} else {
		hint_line = context->vcounter + context->hint_counter + 1;
		if (context->vcounter < context->inactive_start) {
			if (hint_line > context->inactive_start) {
				hint_line = context->regs[REG_HINT];
				if (hint_line > context->inactive_start) {
					return 0xFFFFFFFF;
				}
				if (hint_line >= context->vcounter) {
					//Next interrupt is for a line in the next frame that
					//is higher than the line we're on now so just passing
					//that line number to vdp_cycles_to_line will yield the wrong
					//result
					return context->cycles + vdp_cycles_to_line(context,  0) + hint_line * MCLKS_LINE;
				}
			}
		} else {
			uint32_t jump_start, jump_dst;
			get_jump_params(context, &jump_start, &jump_dst);
			if (hint_line >= jump_start && context->vcounter < jump_dst) {
				hint_line = (hint_line + jump_dst - jump_start) & 0x1FF;
			}
			if (hint_line < context->vcounter && hint_line > context->inactive_start) {
				return 0xFFFFFFFF;
			}
		}
	}
	return context->cycles + vdp_cycles_to_line(context, hint_line);
}

static uint32_t vdp_next_vint_real(vdp_context * context)
{
	if (!(context->regs[REG_MODE_2] & BIT_VINT_EN)) {
		return 0xFFFFFFFF;
	}
	if (context->flags2 & FLAG2_VINT_PENDING) {
		return context->pending_vint_start;
	}


	return vdp_next_vint_z80(context);
}

uint32_t vdp_next_vint(vdp_context *context)
{
	uint32_t ret = vdp_next_vint_real(context);
#ifdef TIMING_DEBUG
	static uint32_t last = 0xFFFFFFFF;
	if (last != ret) {
		printf("vdp_next_vint is %d at frame %d, line %d, hslot %d\n", ret, context->frame, context->vcounter, context->hslot);
	}
	last = ret;
#endif
	return ret;
}

uint32_t vdp_next_vint_z80(vdp_context * context)
{
	uint16_t vint_line = (context->regs[REG_MODE_2] & BIT_MODE_5) ? context->inactive_start : context->inactive_start + 1;
	if (context->vcounter == vint_line) {
		if (context->regs[REG_MODE_2] & BIT_MODE_5) {
			if (context->regs[REG_MODE_4] & BIT_H40) {
				if (context->hslot >= LINE_CHANGE_H40 || context->hslot <= VINT_SLOT_H40) {
					uint32_t cycles = context->cycles;
					if (context->hslot >= LINE_CHANGE_H40) {
						if (context->hslot < 183) {
							cycles += (183 - context->hslot) * MCLKS_SLOT_H40;
						}
						
						if (context->hslot < HSYNC_SLOT_H40) {
							cycles += (HSYNC_SLOT_H40 - (context->hslot >= 229 ? context->hslot : 229)) * MCLKS_SLOT_H40;
						}
						for (int slot = context->hslot <= HSYNC_SLOT_H40 ? HSYNC_SLOT_H40 : context->hslot; slot < HSYNC_END_H40; slot++ )
						{
							cycles += h40_hsync_cycles[slot - HSYNC_SLOT_H40];
						}
						cycles += (256 - (context->hslot > HSYNC_END_H40 ? context->hslot : HSYNC_END_H40)) * MCLKS_SLOT_H40;
					}
					
					cycles += (VINT_SLOT_H40 - (context->hslot >= LINE_CHANGE_H40 ? 0 : context->hslot)) * MCLKS_SLOT_H40;
					return cycles;
				}
			} else {
				if (context->hslot >= LINE_CHANGE_H32 || context->hslot <= VINT_SLOT_H32) {
					if (context->hslot <= VINT_SLOT_H32) {
						return context->cycles + (VINT_SLOT_H32 - context->hslot) * MCLKS_SLOT_H32;
					} else if (context->hslot < 233) {
						return context->cycles + (VINT_SLOT_H32 + 256 - 233 + 148 - context->hslot) * MCLKS_SLOT_H32;
					} else {
						return context->cycles + (VINT_SLOT_H32 + 256 - context->hslot) * MCLKS_SLOT_H32;
					}
				}
			}
		} else {
			if (context->hslot >= LINE_CHANGE_MODE4) {
				return context->cycles + (VINT_SLOT_MODE4 + 256 - context->hslot) * MCLKS_SLOT_H32;
			}
			if (context->hslot <= VINT_SLOT_MODE4) {
				return context->cycles + (VINT_SLOT_MODE4 - context->hslot) * MCLKS_SLOT_H32;
			}
		}
	}
	int32_t cycles_to_vint = vdp_cycles_to_line(context, vint_line);
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		if (context->regs[REG_MODE_4] & BIT_H40) {
			cycles_to_vint += MCLKS_LINE - (LINE_CHANGE_H40 - VINT_SLOT_H40) * MCLKS_SLOT_H40;
		} else {
			cycles_to_vint += (VINT_SLOT_H32 + 256 - 233 + 148 - LINE_CHANGE_H32) * MCLKS_SLOT_H32;
		}
	} else {
		cycles_to_vint += (256 - LINE_CHANGE_MODE4 + VINT_SLOT_MODE4) * MCLKS_SLOT_H32;
	}
	return context->cycles + cycles_to_vint;
}

uint32_t vdp_next_nmi(vdp_context *context)
{
	if (!(context->flags2 & FLAG2_PAUSE)) {
		return 0xFFFFFFFF;
	}
	return context->cycles + vdp_cycles_to_line(context, 0x1FF);
}

void vdp_pbc_pause(vdp_context *context)
{
	context->flags2 |= FLAG2_PAUSE;
}

void vdp_int_ack(vdp_context * context)
{
	//CPU interrupt acknowledge is only used in Mode 5
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		//Apparently the VDP interrupt controller is not very smart
		//Instead of paying attention to what interrupt is being acknowledged it just 
		//clears the pending flag for whatever interrupt it is currently asserted
		//which may be different from the interrupt it was asserting when the 68k
		//started the interrupt process. The window for this is narrow and depends
		//on the latency between the int enable register write and the interrupt being
		//asserted, but Fatal Rewind depends on this due to some buggy code
		if ((context->flags2 & FLAG2_VINT_PENDING) && (context->regs[REG_MODE_2] & BIT_VINT_EN)) {
			context->flags2 &= ~FLAG2_VINT_PENDING;
		} else if((context->flags2 & FLAG2_HINT_PENDING) && (context->regs[REG_MODE_1] & BIT_HINT_EN)) {
			context->flags2 &= ~FLAG2_HINT_PENDING;
		}
	}
}

void vdp_serialize(vdp_context *context, serialize_buffer *buf)
{
	save_int8(buf, VRAM_SIZE / 1024);//VRAM size in KB, needed for future proofing
	save_buffer8(buf, context->vdpmem, VRAM_SIZE);
	save_buffer16(buf, context->cram, CRAM_SIZE);
	save_buffer16(buf, context->vsram, VSRAM_SIZE);
	save_buffer8(buf, context->sat_cache, SAT_CACHE_SIZE);
	for (int i = 0; i <= REG_DMASRC_H; i++)
	{
		save_int8(buf, context->regs[i]);
	}
	save_int32(buf, context->address);
	save_int32(buf, context->serial_address);
	save_int8(buf, context->cd);
	uint8_t fifo_size;
	if (context->fifo_read < 0) {
		fifo_size = 0;
	} else if (context->fifo_write > context->fifo_read) {
		fifo_size = context->fifo_write - context->fifo_read;
	} else {
		fifo_size = context->fifo_write + FIFO_SIZE - context->fifo_read;
	}
	save_int8(buf, fifo_size);
	for (int i = 0, cur = context->fifo_read; i < fifo_size; i++)
	{
		fifo_entry *entry = context->fifo + cur;
		cur = (cur + 1) & (FIFO_SIZE - 1);
		save_int32(buf, entry->cycle);
		save_int32(buf, entry->address);
		save_int16(buf, entry->value);
		save_int8(buf, entry->cd);
		save_int8(buf, entry->partial);
	}
	//FIXME: Flag bits should be rearranged for maximum correspondence to status reg
	save_int16(buf, context->flags2 << 8 | context->flags);
	save_int32(buf, context->frame);
	save_int16(buf, context->vcounter);
	save_int8(buf, context->hslot);
	save_int16(buf, context->hv_latch);
	save_int8(buf, context->state);
	save_int16(buf, context->hscroll_a);
	save_int16(buf, context->hscroll_b);
	save_int16(buf, context->vscroll_latch[0]);
	save_int16(buf, context->vscroll_latch[1]);
	save_int16(buf, context->col_1);
	save_int16(buf, context->col_2);
	save_int16(buf, context->test_port);
	save_buffer8(buf, context->tmp_buf_a, SCROLL_BUFFER_SIZE);
	save_buffer8(buf, context->tmp_buf_b, SCROLL_BUFFER_SIZE);
	save_int8(buf, context->buf_a_off);
	save_int8(buf, context->buf_b_off);
	//FIXME: Sprite rendering state is currently a mess
	save_int8(buf, context->sprite_index);
	save_int8(buf, context->sprite_draws);
	save_int8(buf, context->slot_counter);
	save_int8(buf, context->cur_slot);
	for (int i = 0; i < MAX_DRAWS; i++)
	{
		sprite_draw *draw = context->sprite_draw_list + i;
		save_int16(buf, draw->address);
		save_int16(buf, draw->x_pos);
		save_int8(buf, draw->pal_priority);
		save_int8(buf, draw->h_flip);
	}
	for (int i = 0; i < MAX_SPRITES_LINE; i++)
	{
		sprite_info *info = context->sprite_info_list + i;
		save_int8(buf, info->size);
		save_int8(buf, info->index);
		save_int16(buf, info->y);
	}
	save_buffer8(buf, context->linebuf, LINEBUF_SIZE);
	
	save_int32(buf, context->cycles);
	save_int32(buf, context->pending_vint_start);
	save_int32(buf, context->pending_hint_start);
}

void vdp_deserialize(deserialize_buffer *buf, void *vcontext)
{
	vdp_context *context = vcontext;
	uint8_t vramk = load_int8(buf);
	load_buffer8(buf, context->vdpmem, (vramk * 1024) <= VRAM_SIZE ? vramk * 1024 : VRAM_SIZE);
	if ((vramk * 1024) > VRAM_SIZE) {
		buf->cur_pos += (vramk * 1024) - VRAM_SIZE;
	}
	load_buffer16(buf, context->cram, CRAM_SIZE);
	for (int i = 0; i < CRAM_SIZE; i++)
	{
		update_color_map(context, i, context->cram[i]);
	}
	load_buffer16(buf, context->vsram, VSRAM_SIZE);
	load_buffer8(buf, context->sat_cache, SAT_CACHE_SIZE);
	for (int i = 0; i <= REG_DMASRC_H; i++)
	{
		context->regs[i] = load_int8(buf);
	}
	context->address = load_int32(buf);
	context->serial_address = load_int32(buf);
	context->cd = load_int8(buf);
	uint8_t fifo_size = load_int8(buf);
	if (fifo_size > FIFO_SIZE) {
		fatal_error("Invalid fifo size %d", fifo_size);
	}
	if (fifo_size) {
		context->fifo_read = 0;
		context->fifo_write = fifo_size & (FIFO_SIZE - 1);
		for (int i = 0; i < fifo_size; i++)
		{
			fifo_entry *entry = context->fifo + i;
			entry->cycle = load_int32(buf);
			entry->address = load_int32(buf);
			entry->value = load_int16(buf);
			entry->cd = load_int8(buf);
			entry->partial = load_int8(buf);
		}
	} else {
		context->fifo_read = -1;
		context->fifo_write = 0;
	}
	uint16_t flags = load_int16(buf);
	context->flags2 = flags >> 8;
	context->flags = flags;
	context->frame = load_int32(buf);
	context->vcounter = load_int16(buf);
	context->hslot = load_int8(buf);
	context->hv_latch = load_int16(buf);
	context->state = load_int8(buf);
	context->hscroll_a = load_int16(buf);
	context->hscroll_b = load_int16(buf);
	context->vscroll_latch[0] = load_int16(buf);
	context->vscroll_latch[1] = load_int16(buf);
	context->col_1 = load_int16(buf);
	context->col_2 = load_int16(buf);
	context->test_port = load_int16(buf);
	load_buffer8(buf, context->tmp_buf_a, SCROLL_BUFFER_SIZE);
	load_buffer8(buf, context->tmp_buf_b, SCROLL_BUFFER_SIZE);
	context->buf_a_off = load_int8(buf) & SCROLL_BUFFER_MASK;
	context->buf_b_off = load_int8(buf) & SCROLL_BUFFER_MASK;
	context->sprite_index = load_int8(buf);
	context->sprite_draws = load_int8(buf);
	context->slot_counter = load_int8(buf);
	context->cur_slot = load_int8(buf);
	for (int i = 0; i < MAX_DRAWS; i++)
	{
		sprite_draw *draw = context->sprite_draw_list + i;
		draw->address = load_int16(buf);
		draw->x_pos = load_int16(buf);
		draw->pal_priority = load_int8(buf);
		draw->h_flip = load_int8(buf);
	}
	for (int i = 0; i < MAX_SPRITES_LINE; i++)
	{
		sprite_info *info = context->sprite_info_list + i;
		info->size = load_int8(buf);
		info->index = load_int8(buf);
		info->y = load_int16(buf);
	}
	load_buffer8(buf, context->linebuf, LINEBUF_SIZE);
	
	context->cycles = load_int32(buf);
	context->pending_vint_start = load_int32(buf);
	context->pending_hint_start = load_int32(buf);
	update_video_params(context);
}
