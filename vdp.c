#include "vdp.h"
#include "blastem.h"
#include <stdlib.h>
#include <string.h>

#define NTSC_ACTIVE 225
#define PAL_ACTIVE 241
#define BUF_BIT_PRIORITY 0x40
#define MAP_BIT_PRIORITY 0x8000
#define MAP_BIT_H_FLIP 0x800
#define MAP_BIT_V_FLIP 0x1000

#define BIT_PAL        0x8
#define BIT_DMA_ENABLE 0x4
#define BIT_H40        0x1

#define SCROLL_BUFFER_SIZE 32
#define SCROLL_BUFFER_DRAW 16

#define FIFO_SIZE 4

void init_vdp_context(vdp_context * context)
{
	memset(context, 0, sizeof(*context));
	context->vdpmem = malloc(VRAM_SIZE);
	memset(context->vdpmem, 0, VRAM_SIZE);
	context->framebuf = malloc(FRAMEBUF_SIZE);
	memset(context->framebuf, 0, FRAMEBUF_SIZE);
	context->linebuf = malloc(LINEBUF_SIZE + SCROLL_BUFFER_SIZE*2);
	memset(context->linebuf, 0, LINEBUF_SIZE + SCROLL_BUFFER_SIZE*2);
	context->tmp_buf_a = context->linebuf + LINEBUF_SIZE;
	context->tmp_buf_b = context->tmp_buf_a + SCROLL_BUFFER_SIZE;
	context->sprite_draws = MAX_DRAWS;
	context->fifo_cur = malloc(sizeof(fifo_entry) * FIFO_SIZE);
	context->fifo_end = context->fifo_cur + FIFO_SIZE;
}

void render_sprite_cells(vdp_context * context)
{
	if (context->cur_slot >= context->sprite_draws) {
		sprite_draw * d = context->sprite_draw_list + context->cur_slot;
		
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
		for (uint16_t address = d->address; address < d->address+4; address++) {
			if (x >= 0 && x < 320 && !(context->linebuf[x] & 0xF)) {
				context->linebuf[x] = (context->vdpmem[address] >> 4) | d->pal_priority;
			}
			x += dir;
			if (x >= 0 && x < 320 && !(context->linebuf[x] & 0xF)) {
				context->linebuf[x] = (context->vdpmem[address] & 0xF)  | d->pal_priority;
			}
			x += dir;
		}
	}
}

void scan_sprite_table(uint32_t line, vdp_context * context)
{
	if (context->sprite_index && context->slot_counter) {
		line += 1;
		line &= 0xFF;
		context->sprite_index &= 0x7F;
		if (context->latched_mode & BIT_H40) {
			if (context->sprite_index >= MAX_SPRITES_FRAME) {
				context->sprite_index = 0;
				return;
			}
		} else if(context->sprite_index >= MAX_SPRITES_FRAME_H32) {
			context->sprite_index = 0;
			return;
		}
		//TODO: Read from SAT cache rather than from VRAM
		uint16_t sat_address = (context->regs[REG_SAT] & 0x7F) << 9;
		uint16_t address = context->sprite_index * 8 + sat_address;
		int16_t y = ((context->vdpmem[address] & 0x3) << 8 | context->vdpmem[address+1]) - 128;
		uint8_t height = ((context->vdpmem[address+2] & 0x3) + 1) * 8;
		//printf("Sprite %d | y: %d, height: %d\n", context->sprite_index, y, height);
		if (y <= line && line < (y + height)) {
			//printf("Sprite %d at y: %d with height %d is on line %d\n", context->sprite_index, y, height, line);
			context->sprite_info_list[--(context->slot_counter)].size = context->vdpmem[address+2];
			context->sprite_info_list[context->slot_counter].index = context->sprite_index;
			context->sprite_info_list[context->slot_counter].y = y;
		}
		context->sprite_index = context->vdpmem[address+3] & 0x7F;
		if (context->sprite_index && context->slot_counter)
		{
			address = context->sprite_index * 8 + sat_address;
			y = ((context->vdpmem[address] & 0x3) << 8 | context->vdpmem[address+1]) - 128;
			height = ((context->vdpmem[address+2] & 0x3) + 1) * 8;
			if (y <= line && line < (y + height)) {
				//printf("Sprite %d at y: %d with height %d is on line %d\n", context->sprite_index, y, height, line);
				context->sprite_info_list[--(context->slot_counter)].size = context->vdpmem[address+2];
				context->sprite_info_list[context->slot_counter].index = context->sprite_index;
				context->sprite_info_list[context->slot_counter].y = y;
			}
			context->sprite_index = context->vdpmem[address+3] & 0x7F;
		}
	}
}

void read_sprite_x(uint32_t line, vdp_context * context)
{
	if (context->cur_slot >= context->slot_counter) {
		if (context->sprite_draws) {
			line += 1;
			line &= 0xFF;
			//in tiles
			uint8_t width = ((context->sprite_info_list[context->cur_slot].size >> 2) & 0x3) + 1;
			//in pixels
			uint8_t height = ((context->sprite_info_list[context->cur_slot].size & 0x3) + 1) * 8;
			uint16_t att_addr = ((context->regs[REG_SAT] & 0x7F) << 9) + context->sprite_info_list[context->cur_slot].index * 8 + 4;
			uint16_t tileinfo = (context->vdpmem[att_addr] << 8) | context->vdpmem[att_addr+1];		
			uint8_t pal_priority = (tileinfo >> 9) & 0x70;
			uint8_t row;
			if (tileinfo & MAP_BIT_V_FLIP) {
				row = (context->sprite_info_list[context->cur_slot].y + height - 1) - line;
			} else {
				row = line-context->sprite_info_list[context->cur_slot].y;
			}
			uint16_t address = ((tileinfo & 0x7FF) << 5) + row * 4;
			int16_t x = ((context->vdpmem[att_addr+ 2] & 0x3) << 8) | context->vdpmem[att_addr + 3];
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
			if (i < width) {
				context->flags |= FLAG_DOT_OFLOW;
			}
			context->cur_slot--;
		} else {
			context->flags |= FLAG_DOT_OFLOW;
		}
	}
}

#define VRAM_READ 0
#define VRAM_WRITE 1
#define CRAM_READ 8
#define CRAM_WRITE 3
#define VSRAM_READ 4
#define VSRAM_WRITE 5
#define DMA_START 0x20

void external_slot(vdp_context * context)
{
	//TODO: Figure out what happens if CD bit 4 is not set in DMA copy mode
	//TODO: Figure out what happens when CD:0-3 is not set to a write mode in DMA operations
	//TODO: Figure out what happens if DMA gets disabled part way through a DMA fill or DMA copy
	if((context->regs[REG_MODE_2] & BIT_DMA_ENABLE) && (context->flags & FLAG_DMA_RUN)) {
		uint16_t dma_len;
		switch(context->regs[REG_DMASRC_H] & 0xC0)
		{
		//68K -> VDP
		case 0:
		case 0x40:
			switch(context->dma_cd & 0xF)
			{
			case VRAM_WRITE:
				if (context->flags & FLAG_DMA_PROG) {
					context->vdpmem[context->address ^ 1] = context->dma_val;
					context->flags &= ~FLAG_DMA_PROG;
				} else {
					context->dma_val = read_dma_value((context->regs[REG_DMASRC_H] << 16) | (context->regs[REG_DMASRC_M] << 8) | context->regs[REG_DMASRC_L]);
					context->vdpmem[context->address] = context->dma_val >> 8;
					context->flags |= FLAG_DMA_PROG;
				}
				break;
			case CRAM_WRITE:
				context->cram[(context->address/2) & (CRAM_SIZE-1)] = read_dma_value((context->regs[REG_DMASRC_H] << 16) | (context->regs[REG_DMASRC_M] << 8) | context->regs[REG_DMASRC_L]);
				break;
			case VSRAM_WRITE:
				if (((context->address/2) & 63) < VSRAM_SIZE) {
					context->vsram[(context->address/2) & 63] = read_dma_value((context->regs[REG_DMASRC_H] << 16) | (context->regs[REG_DMASRC_M] << 8) | context->regs[REG_DMASRC_L]);
				}
				break;
			}
			break;
		//Fill
		case 0x80:
			switch(context->dma_cd & 0xF)
			{
			case VRAM_WRITE:
				//Charles MacDonald's VDP doc says that the low byte gets written first
				//this doesn't make a lot of sense to me, but until I've had a change to
				//verify it myself, I'll assume it's true
				if (context->flags & FLAG_DMA_PROG) {
					context->vdpmem[context->address ^ 1] = context->dma_val >> 8;
					context->flags &= ~FLAG_DMA_PROG;
				} else {
					context->vdpmem[context->address] = context->dma_val;
					context->flags |= FLAG_DMA_PROG;
				}
				break;
			case CRAM_WRITE:
				context->cram[(context->address/2) & (CRAM_SIZE-1)] = context->dma_val;
				break;
			case VSRAM_WRITE:
				if (((context->address/2) & 63) < VSRAM_SIZE) {
					context->vsram[(context->address/2) & 63] = context->dma_val;
				}
				break;
			}
			break;
		//Copy
		case 0xC0:
			if (context->flags & FLAG_DMA_PROG) {
				switch(context->dma_cd & 0xF)
				{
				case VRAM_WRITE:
					context->vdpmem[context->address] = context->dma_val;
					break;
				case CRAM_WRITE:
					context->cram[(context->address/2) & (CRAM_SIZE-1)] = context->dma_val;
					break;
				case VSRAM_WRITE:
					if (((context->address/2) & 63) < VSRAM_SIZE) {
						context->vsram[(context->address/2) & 63] = context->dma_val;
					}
					break;
				}
				context->flags &= ~FLAG_DMA_PROG;
			} else {
				//I assume, that DMA copy copies from the same RAM as the destination
				//but it's possible I'm mistaken
				switch(context->dma_cd & 0xF)
				{
				case VRAM_WRITE:
					context->dma_val = context->vdpmem[(context->regs[REG_DMASRC_M] << 8) | context->regs[REG_DMASRC_L]];
					break;
				case CRAM_WRITE:
					context->dma_val = context->cram[context->regs[REG_DMASRC_L] & (CRAM_SIZE-1)];
					break;
				case VSRAM_WRITE:
					if ((context->regs[REG_DMASRC_L] & 63) < VSRAM_SIZE) {
						context->dma_val = context->vsram[context->regs[REG_DMASRC_L] & 63];
					} else {
						context->dma_val = 0;
					}
					break;
				}
				context->flags |= FLAG_DMA_PROG;
			}
			break;
		}
		if (!(context->flags & FLAG_DMA_PROG)) {
			context->address += context->regs[REG_AUTOINC];
			context->regs[REG_DMASRC_L] += 1;
			if (!context->regs[REG_DMASRC_L]) {
				context->regs[REG_DMASRC_M] += 1;
			}
			dma_len = ((context->regs[REG_DMALEN_H] << 8) | context->regs[REG_DMALEN_L]) - 1;
			context->regs[REG_DMALEN_H] = dma_len >> 8;
			context->regs[REG_DMALEN_L] = dma_len;
			if (!dma_len) {
				context->flags &= ~FLAG_DMA_RUN;
			}
		}
	} else {
		fifo_entry * start = (context->fifo_end - FIFO_SIZE);
		if (context->fifo_cur != start && start->cycle <= context->cycles) {
			if ((context->regs[REG_MODE_2] & BIT_DMA_ENABLE) && (context->cd & DMA_START)) {
				context->flags |= FLAG_DMA_RUN;
				context->dma_val = start->value;
				context->dma_cd = context->cd;
			} else {
				switch (context->cd & 0xF)
				{
				case VRAM_WRITE:
					if (start->partial) {
						//printf("VRAM Write: %X to %X\n", start->value, context->address ^ 1);
						context->vdpmem[context->address ^ 1] = start->value;
					} else {
						//printf("VRAM Write High: %X to %X\n", start->value >> 8, context->address);
						context->vdpmem[context->address] = start->value >> 8;
						start->partial = 1;
						//skip auto-increment and removal of entry from fifo
						return;
					}
					break;
				case CRAM_WRITE:
					//printf("CRAM Write: %X to %X\n", start->value, context->address);
					context->cram[(context->address/2) & (CRAM_SIZE-1)] = start->value;
					break;
				case VSRAM_WRITE:
					if (((context->address/2) & 63) < VSRAM_SIZE) {
						//printf("VSRAM Write: %X to %X\n", start->value, context->address);
						context->vsram[(context->address/2) & 63] = start->value;
					}
					break;
				}
				context->address += context->regs[REG_AUTOINC];
			}
			fifo_entry * cur = start+1;
			if (cur < context->fifo_cur) {
				memmove(start, cur, sizeof(fifo_entry) * (context->fifo_cur - cur));
			}
			context->fifo_cur -= 1;
		} else {
			context->flags |= FLAG_UNUSED_SLOT;
		}
	}
}

#define WINDOW_RIGHT 0x80
#define WINDOW_DOWN  0x80

void read_map_scroll(uint16_t column, uint16_t vsram_off, uint32_t line, uint16_t address, uint16_t hscroll_val, vdp_context * context)
{
	if (!vsram_off) {
		uint16_t left_col, right_col;
		if (context->regs[REG_WINDOW_H] & WINDOW_RIGHT) {
			left_col = (context->regs[REG_WINDOW_H] & 0x1F) * 2;
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
			top_line = (context->regs[REG_WINDOW_V] & 0x1F) * 8;
			bottom_line = 241;
		} else {
			top_line = 0;
			bottom_line = (context->regs[REG_WINDOW_V] & 0x1F) * 8;
		}
		if ((column >= left_col && column < right_col) || (line >= top_line && line < bottom_line)) {
			uint16_t address = context->regs[REG_WINDOW] << 10;
			uint16_t line_offset, offset, mask;
			if (context->latched_mode & BIT_H40) {
				address &= 0xF000;
				line_offset = (((line) / 8) * 64 * 2) & 0xFFF;
				mask = 0x7F;
				
			} else {
				address &= 0xF800;
				line_offset = (((line) / 8) * 32 * 2) & 0xFFF;
				mask = 0x3F;
			}
			offset = address + line_offset + (((column - 2) * 2) & mask);
			context->col_1 = (context->vdpmem[offset] << 8) | context->vdpmem[offset+1];
			//printf("Window | top: %d, bot: %d, left: %d, right: %d, base: %X, line: %X offset: %X, tile: %X, reg: %X\n", top_line, bottom_line, left_col, right_col, address, line_offset, offset, ((context->col_1 & 0x3FF) << 5), context->regs[REG_WINDOW]);
			offset = address + line_offset + (((column - 1) * 2) & mask);
			context->col_2 = (context->vdpmem[offset] << 8) | context->vdpmem[offset+1];
			context->v_offset = (line) & 0x7;
			context->flags |= FLAG_WINDOW;
			return;
		}
		context->flags &= ~FLAG_WINDOW;
	}
	uint16_t vscroll;
	switch(context->regs[REG_SCROLL] & 0x30)
	{
	case 0:
		vscroll = 0xFF;
		break;
	case 0x10:
		vscroll = 0x1FF;
		break;
	case 0x20:
		//TODO: Verify this behavior
		vscroll = 0;
		break;
	case 0x30:
		vscroll = 0x3FF;
		break;
	}
	vscroll &= (context->vsram[(context->regs[REG_MODE_3] & 0x4 ? column : 0) + vsram_off] + line);
	context->v_offset = vscroll & 0x7;
	//printf("%s | line %d, vsram: %d, vscroll: %d, v_offset: %d\n",(vsram_off ? "B" : "A"), line, context->vsram[context->regs[REG_MODE_3] & 0x4 ? column : 0], vscroll, context->v_offset);
	vscroll /= 8;
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
		hscroll_mask = 0;
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

void read_map_scroll_a(uint16_t column, uint32_t line, vdp_context * context)
{
	read_map_scroll(column, 0, line, (context->regs[REG_SCROLL_A] & 0x38) << 10, context->hscroll_a, context);
}

void read_map_scroll_b(uint16_t column, uint32_t line, vdp_context * context)
{
	read_map_scroll(column, 1, line, (context->regs[REG_SCROLL_B] & 0x7) << 13, context->hscroll_b, context);
}

void render_map(uint16_t col, uint8_t * tmp_buf, vdp_context * context)
{
	uint16_t address = ((col & 0x7FF) << 5);
	if (col & MAP_BIT_V_FLIP) {
		address +=  28 - 4 * context->v_offset;
	} else {
		address += 4 * context->v_offset;
	}
	uint16_t pal_priority = (col >> 9) & 0x70;
	int32_t dir;
	if (col & MAP_BIT_H_FLIP) {
		tmp_buf += 7;
		dir = -1;
	} else {
		dir = 1;
	}
	for (uint32_t i=0; i < 4; i++, address++)
	{
		*tmp_buf = pal_priority | (context->vdpmem[address] >> 4);
		tmp_buf += dir;
		*tmp_buf = pal_priority | (context->vdpmem[address] & 0xF);
		tmp_buf += dir;
	}
}

void render_map_1(vdp_context * context)
{
	render_map(context->col_1, context->tmp_buf_a+SCROLL_BUFFER_DRAW, context);
}

void render_map_2(vdp_context * context)
{
	render_map(context->col_2, context->tmp_buf_a+SCROLL_BUFFER_DRAW+8, context);
}

void render_map_3(vdp_context * context)
{
	render_map(context->col_1, context->tmp_buf_b+SCROLL_BUFFER_DRAW, context);
}

void render_map_output(uint32_t line, int32_t col, vdp_context * context)
{
	if (line >= 240) {
		return;
	}
	render_map(context->col_2, context->tmp_buf_b+SCROLL_BUFFER_DRAW+8, context);
	uint16_t *dst, *end;
	uint8_t *sprite_buf, *plane_a, *plane_b;
	if (col)
	{
		col-=2;
		dst = context->framebuf + line * 320 + col * 8;
		sprite_buf = context->linebuf + col * 8;
		uint16_t a_src;
		if (context->flags & FLAG_WINDOW) {
			plane_a = context->tmp_buf_a + SCROLL_BUFFER_DRAW;
			a_src = FBUF_SRC_W;
		} else {
			plane_a = context->tmp_buf_a + SCROLL_BUFFER_DRAW - (context->hscroll_a & 0xF);
			a_src = FBUF_SRC_A;
		}
		plane_b = context->tmp_buf_b + SCROLL_BUFFER_DRAW - (context->hscroll_b & 0xF);
		end = dst + 16;
		uint16_t src;
		//printf("A | tmp_buf offset: %d\n", 8 - (context->hscroll_a & 0x7));
		for (; dst < end; ++plane_a, ++plane_b, ++sprite_buf, ++dst) {
			uint8_t pixel;
			if (*sprite_buf & BUF_BIT_PRIORITY && *sprite_buf & 0xF) {
				pixel = *sprite_buf;
				src = FBUF_SRC_S;
			} else if (*plane_a & BUF_BIT_PRIORITY && *plane_a & 0xF) {
				pixel = *plane_a;
				src = a_src;
			} else if (*plane_b & BUF_BIT_PRIORITY && *plane_b & 0xF) {
				pixel = *plane_b;
				src = FBUF_SRC_B;
			} else if (*sprite_buf & 0xF) {
				pixel = *sprite_buf;
				src = FBUF_SRC_S;
			} else if (*plane_a & 0xF) {
				pixel = *plane_a;
				src = a_src;
			} else if (*plane_b & 0xF){
				pixel = *plane_b;
				src = FBUF_SRC_B;
			} else {
				pixel = context->regs[REG_BG_COLOR] & 0x3F;
				src = FBUF_SRC_BG;
			}
			*dst = context->cram[pixel & 0x3F] | ((pixel & BUF_BIT_PRIORITY) ? FBUF_BIT_PRIORITY : 0) | src;
		}
	} else {
		//dst = context->framebuf + line * 320;
		//sprite_buf = context->linebuf + col * 8;
		//plane_a = context->tmp_buf_a + 16 - (context->hscroll_a & 0x7);
		//plane_b = context->tmp_buf_b + 16 - (context->hscroll_b & 0x7);
		//end = dst + 8;
	}
	
	uint16_t remaining;
	if (!(context->flags & FLAG_WINDOW)) {
		remaining = context->hscroll_a & 0xF;
		memcpy(context->tmp_buf_a + SCROLL_BUFFER_DRAW - remaining, context->tmp_buf_a + SCROLL_BUFFER_SIZE - remaining, remaining);
	}
	remaining = context->hscroll_b & 0xF;
	memcpy(context->tmp_buf_b + SCROLL_BUFFER_DRAW - remaining, context->tmp_buf_b + SCROLL_BUFFER_SIZE - remaining, remaining);
}

#define COLUMN_RENDER_BLOCK(column, startcyc) \
	case startcyc:\
		read_map_scroll_a(column, line, context);\
		break;\
	case (startcyc+1):\
		external_slot(context);\
		break;\
	case (startcyc+2):\
		render_map_1(context);\
		break;\
	case (startcyc+3):\
		render_map_2(context);\
		break;\
	case (startcyc+4):\
		read_map_scroll_b(column, line, context);\
		break;\
	case (startcyc+5):\
		read_sprite_x(line, context);\
		break;\
	case (startcyc+6):\
		render_map_3(context);\
		break;\
	case (startcyc+7):\
		render_map_output(line, column, context);\
		break;

#define COLUMN_RENDER_BLOCK_REFRESH(column, startcyc) \
	case startcyc:\
		read_map_scroll_a(column, line, context);\
		break;\
	case (startcyc+1):\
		break;\
	case (startcyc+2):\
		render_map_1(context);\
		break;\
	case (startcyc+3):\
		render_map_2(context);\
		break;\
	case (startcyc+4):\
		read_map_scroll_b(column, line, context);\
		break;\
	case (startcyc+5):\
		read_sprite_x(line, context);\
		break;\
	case (startcyc+6):\
		render_map_3(context);\
		break;\
	case (startcyc+7):\
		render_map_output(line, column, context);\
		break;

void vdp_h40(uint32_t line, uint32_t linecyc, vdp_context * context)
{
	uint16_t address;
	uint32_t mask;
	switch(linecyc)
	{
	//sprite render to line buffer starts
	case 0:
		context->cur_slot = MAX_DRAWS-1;
		memset(context->linebuf, 0, LINEBUF_SIZE);
		render_sprite_cells(context);
		break;
	case 1:
	case 2:
	case 3:
		render_sprite_cells(context);
		break;
	//sprite attribute table scan starts
	case 4:
		render_sprite_cells( context);
		context->sprite_index = 0x80;
		context->slot_counter = MAX_SPRITES_LINE;
		scan_sprite_table(line, context);
		break;
	case 5:
	case 6:
	case 7:
	case 8:
	case 9:
	case 10:
	case 11:
	case 12:
	case 13:
	case 14:
	case 15:
	case 16:
	case 17:
	case 18:
	case 19:
	case 20:
	//!HSYNC asserted
	case 21:
	case 22:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	case 23:
		external_slot(context);
		break;
	case 24:
	case 25:
	case 26:
	case 27:
	case 28:
	case 29:
	case 30:
	case 31:
	case 32:
	case 33:
	case 34:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	case 35:
		address = (context->regs[REG_HSCROLL] & 0x3F) << 10;
		mask = 0;
		if (context->regs[REG_MODE_3] & 0x2) {
			mask |= 0xF8;
		}
		if (context->regs[REG_MODE_3] & 0x1) {
			mask |= 0x7;
		}
		line &= mask;
		address += line * 4;
		context->hscroll_a = context->vdpmem[address] << 8 | context->vdpmem[address+1];
		context->hscroll_b = context->vdpmem[address+2] << 8 | context->vdpmem[address+3];
		//printf("%d: HScroll A: %d, HScroll B: %d\n", line, context->hscroll_a, context->hscroll_b);
		break;
	case 36:
	//!HSYNC high
	case 37:
	case 38:
	case 39:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	case 40:
		read_map_scroll_a(0, line, context);
		break;
	case 41:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	case 42:
		render_map_1(context);
		scan_sprite_table(line, context);//Just a guess
		break;
	case 43:
		render_map_2(context);
		scan_sprite_table(line, context);//Just a guess
		break;
	case 44:
		read_map_scroll_b(0, line, context);
		break;
	case 45:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	case 46:
		render_map_3(context);
		scan_sprite_table(line, context);//Just a guess
		break;
	case 47:
		render_map_output(line, 0, context);
		scan_sprite_table(line, context);//Just a guess
		//reverse context slot counter so it counts the number of sprite slots
		//filled rather than the number of available slots
		//context->slot_counter = MAX_SPRITES_LINE - context->slot_counter;
		context->cur_slot = MAX_SPRITES_LINE-1;
		context->sprite_draws = MAX_DRAWS;
		context->flags &= (~FLAG_CAN_MASK & ~FLAG_MASKED);
		break;
	COLUMN_RENDER_BLOCK(2, 48)
	COLUMN_RENDER_BLOCK(4, 56)
	COLUMN_RENDER_BLOCK(6, 64)
	COLUMN_RENDER_BLOCK_REFRESH(8, 72)
	COLUMN_RENDER_BLOCK(10, 80)
	COLUMN_RENDER_BLOCK(12, 88)
	COLUMN_RENDER_BLOCK(14, 96)
	COLUMN_RENDER_BLOCK_REFRESH(16, 104)
	COLUMN_RENDER_BLOCK(18, 112)
	COLUMN_RENDER_BLOCK(20, 120)
	COLUMN_RENDER_BLOCK(22, 128)
	COLUMN_RENDER_BLOCK_REFRESH(24, 136)
	COLUMN_RENDER_BLOCK(26, 144)
	COLUMN_RENDER_BLOCK(28, 152)
	COLUMN_RENDER_BLOCK(30, 160)
	COLUMN_RENDER_BLOCK_REFRESH(32, 168)
	COLUMN_RENDER_BLOCK(34, 176)
	COLUMN_RENDER_BLOCK(36, 184)
	COLUMN_RENDER_BLOCK(38, 192)
	COLUMN_RENDER_BLOCK_REFRESH(40, 200)
	case 208:
	case 209:
		external_slot(context);
		break;
	default:
		//leftovers from HSYNC clock change nonsense
		break;
	}
}

void vdp_h32(uint32_t line, uint32_t linecyc, vdp_context * context)
{
	uint16_t address;
	uint32_t mask;
	switch(linecyc)
	{
	//sprite render to line buffer starts
	case 0:
		context->cur_slot = MAX_DRAWS_H32-1;
		memset(context->linebuf, 0, LINEBUF_SIZE);
		render_sprite_cells(context);
		break;
	case 1:
	case 2:
	case 3:
		render_sprite_cells(context);
		break;
	//sprite attribute table scan starts
	case 4:
		render_sprite_cells( context);
		context->sprite_index = 0x80;
		context->slot_counter = MAX_SPRITES_LINE_H32;
		scan_sprite_table(line, context);
		break;
	case 5:
	case 6:
	case 7:
	case 8:
	case 9:
	case 10:
	case 11:
	case 12:
	case 13:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
	case 14:
		external_slot(context);
		break;
	case 15:
	case 16:
	case 17:
	case 18:
	case 19:
	//HSYNC start
	case 20:
	case 21:
	case 22:
	case 23:
	case 24:
	case 25:
	case 26:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	case 27:
		external_slot(context);
		break;
	case 28:
		address = (context->regs[REG_HSCROLL] & 0x3F) << 10;
		mask = 0;
		if (context->regs[REG_MODE_3] & 0x2) {
			mask |= 0xF8;
		}
		if (context->regs[REG_MODE_3] & 0x1) {
			mask |= 0x7;
		}
		line &= mask;
		address += line * 4;
		context->hscroll_a = context->vdpmem[address] << 8 | context->vdpmem[address+1];
		context->hscroll_b = context->vdpmem[address+2] << 8 | context->vdpmem[address+3];
		//printf("%d: HScroll A: %d, HScroll B: %d\n", line, context->hscroll_a, context->hscroll_b);
		break;
	case 29:
	case 30:
	case 31:
	case 32:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	//!HSYNC high
	case 33:
		read_map_scroll_a(0, line, context);
		break;
	case 34:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	case 35:
		render_map_1(context);
		scan_sprite_table(line, context);//Just a guess
		break;
	case 36:
		render_map_2(context);
		scan_sprite_table(line, context);//Just a guess
		break;
	case 37:
		read_map_scroll_b(0, line, context);
		break;
	case 38:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	case 39:
		render_map_3(context);
		scan_sprite_table(line, context);//Just a guess
		break;
	case 40:
		render_map_output(line, 0, context);
		scan_sprite_table(line, context);//Just a guess
		//reverse context slot counter so it counts the number of sprite slots
		//filled rather than the number of available slots
		//context->slot_counter = MAX_SPRITES_LINE - context->slot_counter;
		context->cur_slot = MAX_SPRITES_LINE_H32-1;
		context->sprite_draws = MAX_DRAWS_H32;
		context->flags &= (~FLAG_CAN_MASK & ~FLAG_MASKED);
		break;
	COLUMN_RENDER_BLOCK(2, 41)
	COLUMN_RENDER_BLOCK(4, 49)
	COLUMN_RENDER_BLOCK(6, 57)
	COLUMN_RENDER_BLOCK_REFRESH(8, 65)
	COLUMN_RENDER_BLOCK(10, 73)
	COLUMN_RENDER_BLOCK(12, 81)
	COLUMN_RENDER_BLOCK(14, 89)
	COLUMN_RENDER_BLOCK_REFRESH(16, 97)
	COLUMN_RENDER_BLOCK(18, 105)
	COLUMN_RENDER_BLOCK(20, 113)
	COLUMN_RENDER_BLOCK(22, 121)
	COLUMN_RENDER_BLOCK_REFRESH(24, 129)
	COLUMN_RENDER_BLOCK(26, 137)
	COLUMN_RENDER_BLOCK(28, 145)
	COLUMN_RENDER_BLOCK(30, 153)
	COLUMN_RENDER_BLOCK_REFRESH(32, 161)
	case 169:
	case 170:
		external_slot(context);
		break;
	}
}
void latch_mode(vdp_context * context)
{
	context->latched_mode = (context->regs[REG_MODE_4] & 0x81) | (context->regs[REG_MODE_2] & BIT_PAL);
}

int is_refresh(vdp_context * context)
{
	uint32_t linecyc = context->cycles % MCLKS_LINE;
	if (context->latched_mode & BIT_H40) {
		linecyc = linecyc/16;
		return (linecyc == 73 || linecyc == 105 || linecyc == 137 || linecyc == 169 || linecyc == 201);
	} else {
		linecyc = linecyc/20;
		return (linecyc == 66 || linecyc == 98 || linecyc == 130 || linecyc == 162);
	}
}

void check_render_bg(vdp_context * context, int32_t line)
{
	if (line > 0) {
		line -= 1;
		uint16_t * start = NULL, *end = NULL;
		uint32_t linecyc = (context->cycles % MCLKS_LINE);
		if (context->latched_mode & BIT_H40) {
			linecyc /= 16;
			if (linecyc >= 55 && linecyc <= 207 && !((linecyc-55) % 8)) {
				uint32_t x = ((linecyc-55)&(~0xF))*2;
				start = context->framebuf + line * 320 + x;
				end = start + 16;
			}
		} else {
			linecyc /= 20;
			if (linecyc >= 48 && linecyc <= 168 && !((linecyc-48) % 8)) {
				uint32_t x = ((linecyc-48)&(~0xF))*2;
				start = context->framebuf + line * 256 + x;
				end = start + 16;
			}
		}
		while (start != end) {
			*start = context->regs[REG_BG_COLOR] & 0x3F;
			++start;
		}
	}
}

void vdp_run_context(vdp_context * context, uint32_t target_cycles)
{
	while(context->cycles < target_cycles)
	{
		uint32_t line = context->cycles / MCLKS_LINE;
		uint32_t active_lines = context->latched_mode & BIT_PAL ? PAL_ACTIVE : NTSC_ACTIVE;
		if (!line) {
			latch_mode(context);
		}
		if (line < active_lines && context->regs[REG_MODE_2] & DISPLAY_ENABLE) {
			//first sort-of active line is treated as 255 internally
			//it's used for gathering sprite info for line 
			line = (line - 1) & 0xFF;
			uint32_t linecyc = context->cycles % MCLKS_LINE;
			
			//Convert to slot number
			if (context->latched_mode & BIT_H40){
				//TODO: Deal with nasty clock switching during HBLANK
				linecyc = linecyc/16;
				vdp_h40(line, linecyc, context);
				context->cycles += 16;
			} else {
				linecyc = linecyc/20;
				vdp_h32(line, linecyc, context);
				context->cycles += 20;
			}
		} else {
			if (!is_refresh(context)) {
				external_slot(context);
			}
			if (line < active_lines) {
				check_render_bg(context, line);
			}
			if (context->latched_mode & BIT_H40){
				//TODO: Deal with nasty clock switching during HBLANK
				context->cycles += 16;
			} else {
				context->cycles += 20;
			}
		}
	}
}

uint32_t vdp_run_to_vblank(vdp_context * context)
{
	uint32_t target_cycles = ((context->latched_mode & BIT_PAL) ? PAL_ACTIVE : NTSC_ACTIVE) * MCLKS_LINE;
	vdp_run_context(context, target_cycles);
	return context->cycles;
}

void vdp_run_dma_done(vdp_context * context, uint32_t target_cycles)
{
	for(;;) {
		uint32_t dmalen = (context->regs[REG_DMALEN_H] << 8) | context->regs[REG_DMALEN_L];
		if (!dmalen) {
			dmalen = 0x10000;
		}
		uint32_t min_dma_complete = dmalen * (context->latched_mode & BIT_H40 ? 16 : 20);
		if ((context->regs[REG_DMASRC_H] & 0xC0) == 0xC0 || (context->cd & 0xF) == VRAM_WRITE) {
			//DMA copies take twice as long to complete since they require a read and a write
			//DMA Fills and transfers to VRAM also take twice as long as it requires 2 writes for a single word
			min_dma_complete *= 2;
		}
		min_dma_complete += context->cycles;
		if (target_cycles < min_dma_complete) {
			vdp_run_context(context, target_cycles);
			return;
		} else {
			vdp_run_context(context, min_dma_complete);
			if (!(context->flags & FLAG_DMA_RUN)) {
				return;
			}
		}
	}
}

int vdp_control_port_write(vdp_context * context, uint16_t value)
{
	//printf("control port write: %X\n", value);
	if (context->flags & FLAG_PENDING) {
		context->address = (context->address & 0x3FFF) | (value << 14);
		context->cd = (context->cd & 0x3) | ((value >> 2) & 0x3C);
		context->flags &= ~FLAG_PENDING;
		//printf("New Address: %X, New CD: %X\n", context->address, context->cd);
		if (context->cd & 0x20) {
			if((context->regs[REG_DMASRC_H] & 0xC0) != 0x80) {
				//DMA copy or 68K -> VDP, transfer starts immediately
				context->flags |= FLAG_DMA_RUN;
				context->dma_cd = context->cd;
				if (!(context->regs[REG_DMASRC_H] & 0x80)) {
					return 1;
				}
			}
		}
	} else {
		if ((value & 0xC000) == 0x8000) {
			//Register write
			uint8_t reg = (value >> 8) & 0x1F;
			if (reg < VDP_REGS) {
				//printf("register %d set to %X\n", reg, value & 0xFF);
				context->regs[reg] = value;
				/*if (reg == REG_MODE_2) {
					printf("Display is now %s\n", (context->regs[REG_MODE_2] & DISPLAY_ENABLE) ? "enabled" : "disabled");
				}*/
			}
		} else {
			context->flags |= FLAG_PENDING;
			context->address = (context->address &0xC000) | (value & 0x3FFF);
			context->cd = (context->cd &0x3C) | (value >> 14);
		}
	}
	return 0;
}

void vdp_data_port_write(vdp_context * context, uint16_t value)
{
	//printf("data port write: %X\n", value);
	context->flags &= ~FLAG_PENDING;
	/*if (context->fifo_cur == context->fifo_end) {
		printf("FIFO full, waiting for space before next write at cycle %X\n", context->cycles);
	}*/
	while (context->fifo_cur == context->fifo_end) {
		vdp_run_context(context, context->cycles + ((context->latched_mode & BIT_H40) ? 16 : 20));
	}
	context->fifo_cur->cycle = context->cycles;
	context->fifo_cur->value = value;
	context->fifo_cur->partial = 0;
	context->fifo_cur++;
}

uint16_t vdp_control_port_read(vdp_context * context)
{
	context->flags &= ~FLAG_PENDING;
	uint16_t value = 0x3400;
	if (context->fifo_cur == (context->fifo_end - FIFO_SIZE)) {
		value |= 0x200;
	}
	if (context->fifo_cur == context->fifo_end) {
		value |= 0x100;
	}
	if (context->flags & FLAG_DMA_RUN) {
		value |= 0x20;
	}
	//TODO: Lots of other bits in status port
	return value;
}

uint16_t vdp_data_port_read(vdp_context * context)
{
	context->flags &= ~FLAG_PENDING;
	if (!(context->cd & 1)) {
		return 0;
	}
	//Not sure if the FIFO should be drained before processing a read or not, but it would make sense
	context->flags &= ~FLAG_UNUSED_SLOT;
	while (!(context->flags & FLAG_UNUSED_SLOT)) {
		vdp_run_context(context, context->cycles + ((context->latched_mode & BIT_H40) ? 16 : 20));
	}
	uint16_t value = 0;
	switch (context->cd & 0x7)
	{
	case VRAM_READ:
		value = context->vdpmem[context->address] << 8;
		context->flags &= ~FLAG_UNUSED_SLOT;
		while (!(context->flags & FLAG_UNUSED_SLOT)) {
			vdp_run_context(context, context->cycles + ((context->latched_mode & BIT_H40) ? 16 : 20));
		}
		value |= context->vdpmem[context->address ^ 1];
		break;
	case CRAM_READ:
		value = context->cram[(context->address/2) & (CRAM_SIZE-1)];
		break;
	case VSRAM_READ:
		if (((context->address / 2) & 63) < VSRAM_SIZE) {
			value = context->vsram[context->address & 63];
		}
		break;
	}
	context->address += context->regs[REG_AUTOINC];
	return value;
}

void vdp_adjust_cycles(vdp_context * context, uint32_t deduction)
{
	context->cycles -= deduction;
	for(fifo_entry * start = (context->fifo_end - FIFO_SIZE); start < context->fifo_cur; start++) {
		if (start->cycle >= deduction) {
			start->cycle -= deduction;
		} else {
			start->cycle = 0;
		}
	}
}

#define GST_VDP_REGS 0xFA
#define GST_VDP_MEM 0x12478

void vdp_load_savestate(vdp_context * context, FILE * state_file)
{
	uint8_t tmp_buf[CRAM_SIZE*2];
	fseek(state_file, GST_VDP_REGS, SEEK_SET);
	fread(context->regs, 1, VDP_REGS, state_file);
	latch_mode(context);
	fread(tmp_buf, 1, sizeof(tmp_buf), state_file);
	for (int i = 0; i < CRAM_SIZE; i++) {
		context->cram[i] = (tmp_buf[i*2+1] << 8) | tmp_buf[i*2];
	}
	fread(tmp_buf, 2, VSRAM_SIZE, state_file);
	for (int i = 0; i < VSRAM_SIZE; i++) {
		context->vsram[i] = (tmp_buf[i*2+1] << 8) | tmp_buf[i*2];
	}
	fseek(state_file, GST_VDP_MEM, SEEK_SET);
	fread(context->vdpmem, 1, VRAM_SIZE, state_file);
}

void vdp_save_state(vdp_context * context, FILE * outfile)
{
	uint8_t tmp_buf[CRAM_SIZE*2];
	fseek(outfile, GST_VDP_REGS, SEEK_SET);
	fwrite(context->regs, 1, VDP_REGS, outfile);
	for (int i = 0; i < CRAM_SIZE; i++) {
		tmp_buf[i*2] = context->cram[i];
		tmp_buf[i*2+1] = context->cram[i] >> 8;
	}
	fwrite(tmp_buf, 1, sizeof(tmp_buf), outfile);
	for (int i = 0; i < VSRAM_SIZE; i++) {
		tmp_buf[i*2] = context->vsram[i];
		tmp_buf[i*2+1] = context->vsram[i] >> 8;
	}
	fwrite(tmp_buf, 2, VSRAM_SIZE, outfile);
	fseek(outfile, GST_VDP_MEM, SEEK_SET);
	fwrite(context->vdpmem, 1, VRAM_SIZE, outfile);
}

