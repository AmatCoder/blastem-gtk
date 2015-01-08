/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "gen_x86.h"
#include "m68k_core.h"
#include "m68k_internal.h"
#include "68kinst.h"
#include "mem.h"
#include "backend.h"
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
	FLAG_X,
	FLAG_N,
	FLAG_Z,
	FLAG_V,
	FLAG_C
};

void set_flag(m68k_options * opts, uint8_t val, uint8_t flag)
{
	if (opts->flag_regs[flag] >= 0) {
		mov_ir(&opts->gen.code, val, opts->flag_regs[flag], SZ_B);
	} else {
		int8_t offset = offsetof(m68k_context, flags) + flag;
		if (offset) {
			mov_irdisp(&opts->gen.code, val, opts->gen.context_reg, offset, SZ_B);
		} else {
			mov_irind(&opts->gen.code, val, opts->gen.context_reg, SZ_B);
		}
	}
}

void set_flag_cond(m68k_options *opts, uint8_t cond, uint8_t flag)
{
	if (opts->flag_regs[flag] >= 0) {
		setcc_r(&opts->gen.code, cond, opts->flag_regs[flag]);
	} else {
		int8_t offset = offsetof(m68k_context, flags) + flag;
		if (offset) {
			setcc_rdisp(&opts->gen.code, cond, opts->gen.context_reg, offset);
		} else {
			setcc_rind(&opts->gen.code, cond, opts->gen.context_reg);
		}
	}
}

void check_flag(m68k_options *opts, uint8_t flag)
{
	if (opts->flag_regs[flag] >= 0) {
		cmp_ir(&opts->gen.code, 0, opts->flag_regs[flag], SZ_B);
	} else {
		cmp_irdisp(&opts->gen.code, 0, opts->gen.context_reg, offsetof(m68k_context, flags) + flag, SZ_B);
	}
}

void flag_to_reg(m68k_options *opts, uint8_t flag, uint8_t reg)
{
	if (opts->flag_regs[flag] >= 0) {
		mov_rr(&opts->gen.code, opts->flag_regs[flag], reg, SZ_B);
	} else {
		int8_t offset = offsetof(m68k_context, flags) + flag;
		if (offset) {
			mov_rdispr(&opts->gen.code, opts->gen.context_reg, offset, reg, SZ_B);
		} else {
			mov_rindr(&opts->gen.code, opts->gen.context_reg, reg, SZ_B);
		}
	}
}

void reg_to_flag(m68k_options *opts, uint8_t reg, uint8_t flag)
{
	if (opts->flag_regs[flag] >= 0) {
		mov_rr(&opts->gen.code, reg, opts->flag_regs[flag], SZ_B);
	} else {
		int8_t offset = offsetof(m68k_context, flags) + flag;
		if (offset) {
			mov_rrdisp(&opts->gen.code, reg, opts->gen.context_reg, offset, SZ_B);
		} else {
			mov_rrind(&opts->gen.code, reg, opts->gen.context_reg, SZ_B);
		}
	}
}

void flag_to_flag(m68k_options *opts, uint8_t flag1, uint8_t flag2)
{
	code_info *code = &opts->gen.code;
	if (opts->flag_regs[flag1] >= 0 && opts->flag_regs[flag2] >= 0) {
		mov_rr(code, opts->flag_regs[flag1], opts->flag_regs[flag2], SZ_B);
	} else if(opts->flag_regs[flag1] >= 0) {
		mov_rrdisp(code, opts->flag_regs[flag1], opts->gen.context_reg, offsetof(m68k_context, flags) + flag2, SZ_B);
	} else if (opts->flag_regs[flag2] >= 0) {
		mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, flags) + flag1, opts->flag_regs[flag2], SZ_B);
	} else {
		push_r(code, opts->gen.scratch1);
		mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, flags) + flag1, opts->gen.scratch1, SZ_B);
		mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, flags) + flag2, SZ_B);
		pop_r(code, opts->gen.scratch1);
	}
}

void update_flags(m68k_options *opts, uint32_t update_mask)
{
	uint8_t native_flags[] = {0, CC_S, CC_Z, CC_O, CC_C};
	for (int8_t flag = FLAG_C; flag >= FLAG_X; --flag)
	{
		if (update_mask & X0 << (flag*3)) {
			set_flag(opts, 0, flag);
		} else if(update_mask & X1 << (flag*3)) {
			set_flag(opts, 1, flag);
		} else if(update_mask & X << (flag*3)) {
			if (flag == FLAG_X) {
				if (opts->flag_regs[FLAG_C] >= 0 || !(update_mask & (C0|C1|C))) {
					flag_to_flag(opts, FLAG_C, FLAG_X);
				} else if(update_mask & C0) {
					set_flag(opts, 0, flag);
				} else if(update_mask & C1) {
					set_flag(opts, 1, flag);
				} else {
					set_flag_cond(opts, CC_C, flag);
				}
			} else {
				set_flag_cond(opts, native_flags[flag], flag);
			}
		}
	}
}

void flag_to_carry(m68k_options * opts, uint8_t flag)
{
	if (opts->flag_regs[flag] >= 0) {
		bt_ir(&opts->gen.code, 0, opts->flag_regs[flag], SZ_B);
	} else {
		bt_irdisp(&opts->gen.code, 0, opts->gen.context_reg, offsetof(m68k_context, flags) + flag, SZ_B);
	}
}

void or_flag_to_reg(m68k_options *opts, uint8_t flag, uint8_t reg)
{
	if (opts->flag_regs[flag] >= 0) {
		or_rr(&opts->gen.code, opts->flag_regs[flag], reg, SZ_B);
	} else {
		or_rdispr(&opts->gen.code, opts->gen.context_reg, offsetof(m68k_context, flags) + flag, reg, SZ_B);
	}
}

void xor_flag_to_reg(m68k_options *opts, uint8_t flag, uint8_t reg)
{
	if (opts->flag_regs[flag] >= 0) {
		xor_rr(&opts->gen.code, opts->flag_regs[flag], reg, SZ_B);
	} else {
		xor_rdispr(&opts->gen.code, opts->gen.context_reg, offsetof(m68k_context, flags) + flag, reg, SZ_B);
	}
}

void xor_flag(m68k_options *opts, uint8_t val, uint8_t flag)
{
	if (opts->flag_regs[flag] >= 0) {
		xor_ir(&opts->gen.code, val, opts->flag_regs[flag], SZ_B);
	} else {
		xor_irdisp(&opts->gen.code, val, opts->gen.context_reg, offsetof(m68k_context, flags) + flag, SZ_B);
	}
}

void cmp_flags(m68k_options *opts, uint8_t flag1, uint8_t flag2)
{
	code_info *code = &opts->gen.code;
	if (opts->flag_regs[flag1] >= 0 && opts->flag_regs[flag2] >= 0) {
		cmp_rr(code, opts->flag_regs[flag1], opts->flag_regs[flag2], SZ_B);
	} else if(opts->flag_regs[flag1] >= 0 || opts->flag_regs[flag2] >= 0) {
		if (opts->flag_regs[flag2] >= 0) {
			uint8_t tmp = flag1;
			flag1 = flag2;
			flag2 = tmp;
		}
		cmp_rrdisp(code, opts->flag_regs[flag1], opts->gen.context_reg, offsetof(m68k_context, flags) + flag2, SZ_B);
	} else {
		mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, flags) + flag1, opts->gen.scratch1, SZ_B);
		cmp_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, flags) + flag2, SZ_B);
	}
}

void areg_to_native(m68k_options *opts, uint8_t reg, uint8_t native_reg)
{
	if (opts->aregs[reg] >= 0) {
		mov_rr(&opts->gen.code, opts->aregs[reg], native_reg, SZ_D);
	} else {
		mov_rdispr(&opts->gen.code, opts->gen.context_reg,  areg_offset(reg), native_reg, SZ_D);
	}
}

void dreg_to_native(m68k_options *opts, uint8_t reg, uint8_t native_reg)
{
	if (opts->dregs[reg] >= 0) {
		mov_rr(&opts->gen.code, opts->dregs[reg], native_reg, SZ_D);
	} else {
		mov_rdispr(&opts->gen.code, opts->gen.context_reg,  dreg_offset(reg), native_reg, SZ_D);
	}
}

void areg_to_native_sx(m68k_options *opts, uint8_t reg, uint8_t native_reg)
{
	if (opts->aregs[reg] >= 0) {
		movsx_rr(&opts->gen.code, opts->aregs[reg], native_reg, SZ_W, SZ_D);
	} else {
		movsx_rdispr(&opts->gen.code, opts->gen.context_reg,  areg_offset(reg), native_reg, SZ_W, SZ_D);
	}
}

void dreg_to_native_sx(m68k_options *opts, uint8_t reg, uint8_t native_reg)
{
	if (opts->dregs[reg] >= 0) {
		movsx_rr(&opts->gen.code, opts->dregs[reg], native_reg, SZ_W, SZ_D);
	} else {
		movsx_rdispr(&opts->gen.code, opts->gen.context_reg,  dreg_offset(reg), native_reg, SZ_W, SZ_D);
	}
}

void native_to_areg(m68k_options *opts, uint8_t native_reg, uint8_t reg)
	{
	if (opts->aregs[reg] >= 0) {
		mov_rr(&opts->gen.code, native_reg, opts->aregs[reg], SZ_D);
	} else {
		mov_rrdisp(&opts->gen.code, native_reg, opts->gen.context_reg, areg_offset(reg), SZ_D);
	}
}

void native_to_dreg(m68k_options *opts, uint8_t native_reg, uint8_t reg)
{
	if (opts->dregs[reg] >= 0) {
		mov_rr(&opts->gen.code, native_reg, opts->dregs[reg], SZ_D);
	} else {
		mov_rrdisp(&opts->gen.code, native_reg, opts->gen.context_reg, dreg_offset(reg), SZ_D);
	}
}

void ldi_areg(m68k_options *opts, int32_t value, uint8_t reg)
{
	if (opts->aregs[reg] >= 0) {
		mov_ir(&opts->gen.code, value, opts->aregs[reg], SZ_D);
	} else {
		mov_irdisp(&opts->gen.code, value, opts->gen.context_reg, areg_offset(reg), SZ_D);
	}
}

void ldi_native(m68k_options *opts, int32_t value, uint8_t reg)
{
	mov_ir(&opts->gen.code, value, reg, SZ_D);
}

void addi_native(m68k_options *opts, int32_t value, uint8_t reg)
{
	add_ir(&opts->gen.code, value, reg, SZ_D);
			}

void subi_native(m68k_options *opts, int32_t value, uint8_t reg)
{
	sub_ir(&opts->gen.code, value, reg, SZ_D);
}

void push_native(m68k_options *opts, uint8_t reg)
{
	push_r(&opts->gen.code, reg);
}

void pop_native(m68k_options *opts, uint8_t reg)
{
	pop_r(&opts->gen.code, reg);
}

void sign_extend16_native(m68k_options *opts, uint8_t reg)
{
	movsx_rr(&opts->gen.code, reg, reg, SZ_W, SZ_D);
}

void addi_areg(m68k_options *opts, int32_t val, uint8_t reg)
{
	if (opts->aregs[reg] >= 0) {
		add_ir(&opts->gen.code, val, opts->aregs[reg], SZ_D);
	} else {
		add_irdisp(&opts->gen.code, val, opts->gen.context_reg, areg_offset(reg), SZ_D);
	}
}

void subi_areg(m68k_options *opts, int32_t val, uint8_t reg)
{
	if (opts->aregs[reg] >= 0) {
		sub_ir(&opts->gen.code, val, opts->aregs[reg], SZ_D);
	} else {
		sub_irdisp(&opts->gen.code, val, opts->gen.context_reg, areg_offset(reg), SZ_D);
	}
}

void add_areg_native(m68k_options *opts, uint8_t reg, uint8_t native_reg)
{
	if (opts->aregs[reg] >= 0) {
		add_rr(&opts->gen.code, opts->aregs[reg], native_reg, SZ_D);
	} else {
		add_rdispr(&opts->gen.code, opts->gen.context_reg, areg_offset(reg), native_reg, SZ_D);
	}
}

void add_dreg_native(m68k_options *opts, uint8_t reg, uint8_t native_reg)
{
	if (opts->dregs[reg] >= 0) {
		add_rr(&opts->gen.code, opts->dregs[reg], native_reg, SZ_D);
	} else {
		add_rdispr(&opts->gen.code, opts->gen.context_reg, dreg_offset(reg), native_reg, SZ_D);
	}
}

void calc_areg_displace(m68k_options *opts, m68k_op_info *op, uint8_t native_reg)
{
	areg_to_native(opts, op->params.regs.pri, native_reg);
	add_ir(&opts->gen.code, op->params.regs.displacement, native_reg, SZ_D);
}

void calc_index_disp8(m68k_options *opts, m68k_op_info *op, uint8_t native_reg)
{
	uint8_t sec_reg = (op->params.regs.sec >> 1) & 0x7;
	if (op->params.regs.sec & 1) {
		if (op->params.regs.sec & 0x10) {
			add_areg_native(opts, sec_reg, native_reg);
		} else {
			add_dreg_native(opts, sec_reg, native_reg);
		}
	} else {
		uint8_t other_reg = native_reg == opts->gen.scratch1 ? opts->gen.scratch2 : opts->gen.scratch1;
		if (op->params.regs.sec & 0x10) {
			areg_to_native_sx(opts, sec_reg, other_reg);
		} else {
			dreg_to_native_sx(opts, sec_reg, other_reg);
		}
		add_rr(&opts->gen.code, other_reg, native_reg, SZ_D);
	}
	if (op->params.regs.displacement) {
		add_ir(&opts->gen.code, op->params.regs.displacement, native_reg, SZ_D);
	}
}

void calc_areg_index_disp8(m68k_options *opts, m68k_op_info *op, uint8_t native_reg)
{
	areg_to_native(opts, op->params.regs.pri, native_reg);
	calc_index_disp8(opts, op, native_reg);
}

void translate_m68k_op(m68kinst * inst, host_ea * ea, m68k_options * opts, uint8_t dst)
{
	code_info *code = &opts->gen.code;
	m68k_op_info *op = dst ? &inst->dst : &inst->src;
	int8_t reg = native_reg(op, opts);
	uint8_t sec_reg;
	int32_t dec_amount, inc_amount;
	if (reg >= 0) {
		ea->mode = MODE_REG_DIRECT;
		if (!dst && inst->dst.addr_mode == MODE_AREG && inst->extra.size == OPSIZE_WORD) {
			movsx_rr(code, reg, opts->gen.scratch1, SZ_W, SZ_D);
			ea->base = opts->gen.scratch1;
		} else {
			ea->base = reg;
		}
		return;
	}
	switch (op->addr_mode)
	{
	case MODE_REG:
	case MODE_AREG:
		//We only get one memory parameter, so if the dst operand is a register in memory,
		//we need to copy this to a temp register first if we're translating the src operand
		if (dst || native_reg(&(inst->dst), opts) >= 0 || inst->dst.addr_mode == MODE_UNUSED || !(inst->dst.addr_mode == MODE_REG || inst->dst.addr_mode == MODE_AREG)
		    || inst->op == M68K_EXG) {

			ea->mode = MODE_REG_DISPLACE8;
			ea->base = opts->gen.context_reg;
			ea->disp = reg_offset(op);
		} else {
			if (inst->dst.addr_mode == MODE_AREG && inst->extra.size == OPSIZE_WORD) {
				movsx_rdispr(code, opts->gen.context_reg, reg_offset(op), opts->gen.scratch1, SZ_W, SZ_D);
			} else {
				mov_rdispr(code, opts->gen.context_reg, reg_offset(op), opts->gen.scratch1, inst->extra.size);
			}
			ea->mode = MODE_REG_DIRECT;
			ea->base = opts->gen.scratch1;
			//we're explicitly handling the areg dest here, so we exit immediately
			return;
		}
		break;
	case MODE_AREG_PREDEC:
		if (dst && inst->src.addr_mode == MODE_AREG_PREDEC) {
			push_r(code, opts->gen.scratch1);
		}
		dec_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : (op->params.regs.pri == 7 ? 2 :1));
		if (!dst) {
			cycles(&opts->gen, PREDEC_PENALTY);
		}
		subi_areg(opts, dec_amount, op->params.regs.pri);
	case MODE_AREG_INDIRECT:
	case MODE_AREG_POSTINC:
		areg_to_native(opts, op->params.regs.pri, opts->gen.scratch1);
		m68k_read_size(opts, inst->extra.size);

		if (dst) {
			if (inst->src.addr_mode == MODE_AREG_PREDEC) {
				//restore src operand to opts->gen.scratch2
				pop_r(code, opts->gen.scratch2);
			} else {
				//save reg value in opts->gen.scratch2 so we can use it to save the result in memory later
				areg_to_native(opts, op->params.regs.pri, opts->gen.scratch2);
			}
		}

		if (op->addr_mode == MODE_AREG_POSTINC) {
			inc_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : (op->params.regs.pri == 7 ? 2 : 1));
			addi_areg(opts, inc_amount, op->params.regs.pri);
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = (!dst && inst->dst.addr_mode == MODE_AREG_PREDEC && inst->op != M68K_MOVE) ? opts->gen.scratch2 : opts->gen.scratch1;
		break;
	case MODE_AREG_DISPLACE:
		cycles(&opts->gen, BUS);
		calc_areg_displace(opts, op, opts->gen.scratch1);
		if (dst) {
			push_r(code, opts->gen.scratch1);
		}
		m68k_read_size(opts, inst->extra.size);
		if (dst) {
			pop_r(code, opts->gen.scratch2);
		}

		ea->mode = MODE_REG_DIRECT;
		ea->base = opts->gen.scratch1;
		break;
	case MODE_AREG_INDEX_DISP8:
		cycles(&opts->gen, 6);
		calc_areg_index_disp8(opts, op, opts->gen.scratch1);
		if (dst) {
			push_r(code, opts->gen.scratch1);
		}
		m68k_read_size(opts, inst->extra.size);
		if (dst) {
			pop_r(code, opts->gen.scratch2);
		}

		ea->mode = MODE_REG_DIRECT;
		ea->base = opts->gen.scratch1;
		break;
	case MODE_PC_DISPLACE:
		cycles(&opts->gen, BUS);
		mov_ir(code, op->params.regs.displacement + inst->address+2, opts->gen.scratch1, SZ_D);
		if (dst) {
			push_r(code, opts->gen.scratch1);
		}
		m68k_read_size(opts, inst->extra.size);
		if (dst) {
			pop_r(code, opts->gen.scratch2);
		}

		ea->mode = MODE_REG_DIRECT;
		ea->base = opts->gen.scratch1;
		break;
	case MODE_PC_INDEX_DISP8:
		cycles(&opts->gen, 6);
		mov_ir(code, inst->address+2, opts->gen.scratch1, SZ_D);
		calc_index_disp8(opts, op, opts->gen.scratch1);
		if (dst) {
			push_r(code, opts->gen.scratch1);
		}
		m68k_read_size(opts, inst->extra.size);
		if (dst) {
			pop_r(code, opts->gen.scratch2);
		}

		ea->mode = MODE_REG_DIRECT;
		ea->base = opts->gen.scratch1;
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		cycles(&opts->gen, op->addr_mode == MODE_ABSOLUTE ? BUS*2 : BUS);
		mov_ir(code, op->params.immed, opts->gen.scratch1, SZ_D);
		if (dst) {
			push_r(code, opts->gen.scratch1);
		}
		m68k_read_size(opts, inst->extra.size);
		if (dst) {
			pop_r(code, opts->gen.scratch2);
		}

		ea->mode = MODE_REG_DIRECT;
		ea->base = opts->gen.scratch1;
		break;
	case MODE_IMMEDIATE:
	case MODE_IMMEDIATE_WORD:
		if (inst->variant != VAR_QUICK) {
			cycles(&opts->gen, (inst->extra.size == OPSIZE_LONG && op->addr_mode == MODE_IMMEDIATE) ? BUS*2 : BUS);
		}
		ea->mode = MODE_IMMED;
		ea->disp = op->params.immed;
		//sign extend value when the destination is an address register
		if (inst->dst.addr_mode == MODE_AREG && inst->extra.size == OPSIZE_WORD && ea->disp & 0x8000) {
			ea->disp |= 0xFFFF0000;
		}
		return;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%X: %s\naddress mode %d not implemented (%s)\n", inst->address, disasm_buf, op->addr_mode, dst ? "dst" : "src");
		exit(1);
	}
	if (!dst && inst->dst.addr_mode == MODE_AREG && inst->extra.size == OPSIZE_WORD) {
		if (ea->mode == MODE_REG_DIRECT) {
			movsx_rr(code, ea->base, opts->gen.scratch1, SZ_W, SZ_D);
		} else {
			movsx_rdispr(code, ea->base, ea->disp, opts->gen.scratch1, SZ_W, SZ_D);
			ea->mode = MODE_REG_DIRECT;
		}
		ea->base = opts->gen.scratch1;
	}
}

void m68k_save_result(m68kinst * inst, m68k_options * opts)
{
	code_info *code = &opts->gen.code;
	if (inst->dst.addr_mode != MODE_REG && inst->dst.addr_mode != MODE_AREG && inst->dst.addr_mode != MODE_UNUSED) {
		if (inst->dst.addr_mode == MODE_AREG_PREDEC && inst->src.addr_mode == MODE_AREG_PREDEC && inst->op != M68K_MOVE) {
			areg_to_native(opts, inst->dst.params.regs.pri, opts->gen.scratch2);
		}
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			call(code, opts->write_8);
			break;
		case OPSIZE_WORD:
			call(code, opts->write_16);
			break;
		case OPSIZE_LONG:
			call(code, opts->write_32_lowfirst);
			break;
		}
	}
}

void translate_m68k_move(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	int8_t reg, flags_reg, sec_reg;
	uint8_t dir = 0;
	int32_t offset;
	int32_t inc_amount, dec_amount;
	host_ea src;
	translate_m68k_op(inst, &src, opts, 0);
	reg = native_reg(&(inst->dst), opts);

	if (inst->dst.addr_mode != MODE_AREG) {
		if (src.mode == MODE_REG_DIRECT) {
			flags_reg = src.base;
		} else {
			if (reg >= 0) {
				flags_reg = reg;
			} else {
				if(src.mode == MODE_REG_DISPLACE8) {
					mov_rdispr(code, src.base, src.disp, opts->gen.scratch1, inst->extra.size);
				} else {
					mov_ir(code, src.disp, opts->gen.scratch1, inst->extra.size);
				}
				src.mode = MODE_REG_DIRECT;
				flags_reg = src.base = opts->gen.scratch1;
			}
		}
	}
	uint8_t size = inst->extra.size;
	switch(inst->dst.addr_mode)
	{
	case MODE_AREG:
		size = OPSIZE_LONG;
	case MODE_REG:
		if (reg >= 0) {
			if (src.mode == MODE_REG_DIRECT) {
				mov_rr(code, src.base, reg, size);
			} else if (src.mode == MODE_REG_DISPLACE8) {
				mov_rdispr(code, src.base, src.disp, reg, size);
			} else {
				mov_ir(code, src.disp, reg, size);
			}
		} else if(src.mode == MODE_REG_DIRECT) {
			mov_rrdisp(code, src.base, opts->gen.context_reg, reg_offset(&(inst->dst)), size);
		} else {
			mov_irdisp(code, src.disp, opts->gen.context_reg, reg_offset(&(inst->dst)), size);
		}
		break;
	case MODE_AREG_PREDEC:
		dec_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : (inst->dst.params.regs.pri == 7 ? 2 : 1));
	case MODE_AREG_INDIRECT:
	case MODE_AREG_POSTINC:
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != opts->gen.scratch1) {
				mov_rr(code, src.base, opts->gen.scratch1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			mov_rdispr(code, src.base, src.disp, opts->gen.scratch1, inst->extra.size);
		} else {
			mov_ir(code, src.disp, opts->gen.scratch1, inst->extra.size);
		}
		if (inst->dst.addr_mode == MODE_AREG_PREDEC) {
			subi_areg(opts, dec_amount, inst->dst.params.regs.pri);
		}
		areg_to_native(opts, inst->dst.params.regs.pri, opts->gen.scratch2);
		break;
	case MODE_AREG_DISPLACE:
		cycles(&opts->gen, BUS);
		calc_areg_displace(opts, &inst->dst, opts->gen.scratch2);
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != opts->gen.scratch1) {
				mov_rr(code, src.base, opts->gen.scratch1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			mov_rdispr(code, src.base, src.disp, opts->gen.scratch1, inst->extra.size);
		} else {
			mov_ir(code, src.disp, opts->gen.scratch1, inst->extra.size);
		}
		break;
	case MODE_AREG_INDEX_DISP8:
		cycles(&opts->gen, 6);//TODO: Check to make sure this is correct
		//calc_areg_index_disp8 will clober scratch1 when a 16-bit index is used
		if (src.base == opts->gen.scratch1 && !(inst->dst.params.regs.sec & 1)) {
			push_r(code, opts->gen.scratch1);
		}
		calc_areg_index_disp8(opts, &inst->dst, opts->gen.scratch2);
		if (src.base == opts->gen.scratch1 && !(inst->dst.params.regs.sec & 1)) {
			pop_r(code, opts->gen.scratch1);
		}
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != opts->gen.scratch1) {
				mov_rr(code, src.base, opts->gen.scratch1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			mov_rdispr(code, src.base, src.disp, opts->gen.scratch1, inst->extra.size);
		} else {
			mov_ir(code, src.disp, opts->gen.scratch1, inst->extra.size);
		}
		break;
	case MODE_PC_DISPLACE:
		cycles(&opts->gen, BUS);
		mov_ir(code, inst->dst.params.regs.displacement + inst->address+2, opts->gen.scratch2, SZ_D);
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != opts->gen.scratch1) {
				mov_rr(code, src.base, opts->gen.scratch1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			mov_rdispr(code, src.base, src.disp, opts->gen.scratch1, inst->extra.size);
		} else {
			mov_ir(code, src.disp, opts->gen.scratch1, inst->extra.size);
		}
		break;
	case MODE_PC_INDEX_DISP8:
		cycles(&opts->gen, 6);//TODO: Check to make sure this is correct
		mov_ir(code, inst->address, opts->gen.scratch2, SZ_D);
		if (src.base == opts->gen.scratch1 && !(inst->dst.params.regs.sec & 1)) {
			push_r(code, opts->gen.scratch1);
				}
		calc_index_disp8(opts, &inst->dst, opts->gen.scratch2);
		if (src.base == opts->gen.scratch1 && !(inst->dst.params.regs.sec & 1)) {
			pop_r(code, opts->gen.scratch1);
		}
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != opts->gen.scratch1) {
				mov_rr(code, src.base, opts->gen.scratch1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			mov_rdispr(code, src.base, src.disp, opts->gen.scratch1, inst->extra.size);
		} else {
			mov_ir(code, src.disp, opts->gen.scratch1, inst->extra.size);
		}
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != opts->gen.scratch1) {
				mov_rr(code, src.base, opts->gen.scratch1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			mov_rdispr(code, src.base, src.disp, opts->gen.scratch1, inst->extra.size);
		} else {
			mov_ir(code, src.disp, opts->gen.scratch1, inst->extra.size);
		}
		if (inst->dst.addr_mode == MODE_ABSOLUTE) {
			cycles(&opts->gen, BUS*2);
		} else {
			cycles(&opts->gen, BUS);
		}
		mov_ir(code, inst->dst.params.immed, opts->gen.scratch2, SZ_D);
		break;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%X: %s\naddress mode %d not implemented (move dst)\n", inst->address, disasm_buf, inst->dst.addr_mode);
		exit(1);
	}

	if (inst->dst.addr_mode != MODE_AREG) {
		cmp_ir(code, 0, flags_reg, inst->extra.size);
		update_flags(opts, N|Z|V0|C0);
}
	if (inst->dst.addr_mode != MODE_REG && inst->dst.addr_mode != MODE_AREG) {
		m68k_write_size(opts, inst->extra.size);
		if (inst->dst.addr_mode == MODE_AREG_POSTINC) {
			inc_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : (inst->dst.params.regs.pri == 7 ? 2 : 1));
			addi_areg(opts, inc_amount, inst->dst.params.regs.pri);
		}
	}

	//add cycles for prefetch
	cycles(&opts->gen, BUS);
}

void translate_m68k_clr(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	update_flags(opts, N0|V0|C0|Z1);
	int8_t reg = native_reg(&(inst->dst), opts);
	if (reg >= 0) {
		cycles(&opts->gen, (inst->extra.size == OPSIZE_LONG ? 6 : 4));
		xor_rr(code, reg, reg, inst->extra.size);
		return;
	}
	host_ea dst_op;
	//TODO: fix timing
	translate_m68k_op(inst, &dst_op, opts, 1);
	if (dst_op.mode == MODE_REG_DIRECT) {
		xor_rr(code, dst_op.base, dst_op.base, inst->extra.size);
	} else {
		mov_irdisp(code, 0, dst_op.base, dst_op.disp, inst->extra.size);
	}
	m68k_save_result(inst, opts);
}

void translate_m68k_ext(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	host_ea dst_op;
	uint8_t dst_size = inst->extra.size;
	inst->extra.size--;
	translate_m68k_op(inst, &dst_op, opts, 1);
	if (dst_op.mode == MODE_REG_DIRECT) {
		movsx_rr(code, dst_op.base, dst_op.base, inst->extra.size, dst_size);
		cmp_ir(code, 0, dst_op.base, dst_size);
	} else {
		movsx_rdispr(code, dst_op.base, dst_op.disp, opts->gen.scratch1, inst->extra.size, dst_size);
		cmp_ir(code, 0, opts->gen.scratch1, dst_size);
		mov_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, dst_size);
	}
	inst->extra.size = dst_size;
	update_flags(opts, N|V0|C0|Z);
	//M68K EXT only operates on registers so no need for a call to save result here
}

uint8_t m68k_eval_cond(m68k_options * opts, uint8_t cc)
{
	uint8_t cond = CC_NZ;
	switch (cc)
	{
	case COND_HIGH:
		cond = CC_Z;
	case COND_LOW_SAME:
		flag_to_reg(opts, FLAG_Z, opts->gen.scratch1);
		or_flag_to_reg(opts, FLAG_C, opts->gen.scratch1);
		break;
	case COND_CARRY_CLR:
		cond = CC_Z;
	case COND_CARRY_SET:
		check_flag(opts, FLAG_C);
		break;
	case COND_NOT_EQ:
		cond = CC_Z;
	case COND_EQ:
		check_flag(opts, FLAG_Z);
		break;
	case COND_OVERF_CLR:
		cond = CC_Z;
	case COND_OVERF_SET:
		check_flag(opts, FLAG_V);
		break;
	case COND_PLUS:
		cond = CC_Z;
	case COND_MINUS:
		check_flag(opts, FLAG_N);
		break;
	case COND_GREATER_EQ:
		cond = CC_Z;
	case COND_LESS:
		cmp_flags(opts, FLAG_N, FLAG_V);
		break;
	case COND_GREATER:
		cond = CC_Z;
	case COND_LESS_EQ:
		flag_to_reg(opts, FLAG_V, opts->gen.scratch1);
		xor_flag_to_reg(opts, FLAG_N, opts->gen.scratch1);
		or_flag_to_reg(opts, FLAG_Z, opts->gen.scratch1);
		break;
	}
	return cond;
}

void translate_m68k_bcc(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	cycles(&opts->gen, 10);//TODO: Adjust this for branch not taken case
	int32_t disp = inst->src.params.immed;
	uint32_t after = inst->address + 2;
	if (inst->extra.cond == COND_TRUE) {
		jump_m68k_abs(opts, after + disp);
	} else {
		code_ptr dest_addr = get_native_address(opts->gen.native_code_map, after + disp);
		uint8_t cond = m68k_eval_cond(opts, inst->extra.cond);
		if (!dest_addr) {
			opts->gen.deferred = defer_address(opts->gen.deferred, after + disp, code->cur + 2);
			//dummy address to be replaced later, make sure it generates a 4-byte displacement
			dest_addr = code->cur + 256;
		}
		jcc(code, cond, dest_addr);
	}
}

void translate_m68k_scc(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	uint8_t cond = inst->extra.cond;
	host_ea dst_op;
	inst->extra.size = OPSIZE_BYTE;
	translate_m68k_op(inst, &dst_op, opts, 1);
	if (cond == COND_TRUE || cond == COND_FALSE) {
		if ((inst->dst.addr_mode == MODE_REG || inst->dst.addr_mode == MODE_AREG) && inst->extra.cond == COND_TRUE) {
			cycles(&opts->gen, 6);
		} else {
			cycles(&opts->gen, BUS);
		}
		if (dst_op.mode == MODE_REG_DIRECT) {
			mov_ir(code, cond == COND_TRUE ? 0xFF : 0, dst_op.base, SZ_B);
		} else {
			mov_irdisp(code, cond == COND_TRUE ? 0xFF : 0, dst_op.base, dst_op.disp, SZ_B);
		}
	} else {
		uint8_t cc = m68k_eval_cond(opts, cond);
		check_alloc_code(code, 6*MAX_INST_LEN);
		code_ptr true_off = code->cur + 1;
		jcc(code, cc, code->cur+2);
		cycles(&opts->gen, BUS);
		if (dst_op.mode == MODE_REG_DIRECT) {
			mov_ir(code, 0, dst_op.base, SZ_B);
		} else {
			mov_irdisp(code, 0, dst_op.base, dst_op.disp, SZ_B);
		}
		code_ptr end_off = code->cur+1;
		jmp(code, code->cur+2);
		*true_off = code->cur - (true_off+1);
		cycles(&opts->gen, 6);
		if (dst_op.mode == MODE_REG_DIRECT) {
			mov_ir(code, 0xFF, dst_op.base, SZ_B);
		} else {
			mov_irdisp(code, 0xFF, dst_op.base, dst_op.disp, SZ_B);
		}
		*end_off = code->cur - (end_off+1);
	}
	m68k_save_result(inst, opts);
}

void translate_m68k_dbcc(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	//best case duration
	cycles(&opts->gen, 10);
	code_ptr skip_loc = NULL;
	//TODO: Check if COND_TRUE technically valid here even though
	//it's basically a slow NOP
	if (inst->extra.cond != COND_FALSE) {
		uint8_t cond = m68k_eval_cond(opts, inst->extra.cond);
		check_alloc_code(code, 6*MAX_INST_LEN);
		skip_loc = code->cur + 1;
		jcc(code, cond, code->cur + 2);
	}
	if (opts->dregs[inst->dst.params.regs.pri] >= 0) {
		sub_ir(code, 1, opts->dregs[inst->dst.params.regs.pri], SZ_W);
		cmp_ir(code, -1, opts->dregs[inst->dst.params.regs.pri], SZ_W);
	} else {
		sub_irdisp(code, 1, opts->gen.context_reg, offsetof(m68k_context, dregs) + 4 * inst->dst.params.regs.pri, SZ_W);
		cmp_irdisp(code, -1, opts->gen.context_reg, offsetof(m68k_context, dregs) + 4 * inst->dst.params.regs.pri, SZ_W);
	}
	code_ptr loop_end_loc = code->cur + 1;
	jcc(code, CC_Z, code->cur + 2);
	uint32_t after = inst->address + 2;
	jump_m68k_abs(opts, after + inst->src.params.immed);
	*loop_end_loc = code->cur - (loop_end_loc+1);
	if (skip_loc) {
		cycles(&opts->gen, 2);
		*skip_loc = code->cur - (skip_loc+1);
		cycles(&opts->gen, 2);
	} else {
		cycles(&opts->gen, 4);
	}
}

void translate_m68k_movep(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	int8_t reg;
	cycles(&opts->gen, BUS*2);
	if (inst->src.addr_mode == MODE_REG) {
		calc_areg_displace(opts, &inst->dst, opts->gen.scratch2);
		reg = native_reg(&(inst->src), opts);
		if (inst->extra.size == OPSIZE_LONG) {
			if (reg >= 0) {
				mov_rr(code, reg, opts->gen.scratch1, SZ_D);
				shr_ir(code, 24, opts->gen.scratch1, SZ_D);
				push_r(code, opts->gen.scratch2);
				call(code, opts->write_8);
				pop_r(code, opts->gen.scratch2);
				mov_rr(code, reg, opts->gen.scratch1, SZ_D);
				shr_ir(code, 16, opts->gen.scratch1, SZ_D);

			} else {
				mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->src))+3, opts->gen.scratch1, SZ_B);
				push_r(code, opts->gen.scratch2);
				call(code, opts->write_8);
				pop_r(code, opts->gen.scratch2);
				mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->src))+2, opts->gen.scratch1, SZ_B);
			}
			add_ir(code, 2, opts->gen.scratch2, SZ_D);
			push_r(code, opts->gen.scratch2);
			call(code, opts->write_8);
			pop_r(code, opts->gen.scratch2);
			add_ir(code, 2, opts->gen.scratch2, SZ_D);
		}
		if (reg >= 0) {
			mov_rr(code, reg, opts->gen.scratch1, SZ_W);
			shr_ir(code, 8, opts->gen.scratch1, SZ_W);
			push_r(code, opts->gen.scratch2);
			call(code, opts->write_8);
			pop_r(code, opts->gen.scratch2);
			mov_rr(code, reg, opts->gen.scratch1, SZ_W);
		} else {
			mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->src))+1, opts->gen.scratch1, SZ_B);
			push_r(code, opts->gen.scratch2);
			call(code, opts->write_8);
			pop_r(code, opts->gen.scratch2);
			mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->src)), opts->gen.scratch1, SZ_B);
		}
		add_ir(code, 2, opts->gen.scratch2, SZ_D);
		call(code, opts->write_8);
	} else {
		calc_areg_displace(opts, &inst->src, opts->gen.scratch1);
		reg = native_reg(&(inst->dst), opts);
		if (inst->extra.size == OPSIZE_LONG) {
			if (reg >= 0) {
				push_r(code, opts->gen.scratch1);
				call(code, opts->read_8);
				shl_ir(code, 24, opts->gen.scratch1, SZ_D);
				mov_rr(code, opts->gen.scratch1, reg, SZ_D);
				pop_r(code, opts->gen.scratch1);
				add_ir(code, 2, opts->gen.scratch1, SZ_D);
				push_r(code, opts->gen.scratch1);
				call(code, opts->read_8);
				shl_ir(code, 16, opts->gen.scratch1, SZ_D);
				or_rr(code, opts->gen.scratch1, reg, SZ_D);
			} else {
				push_r(code, opts->gen.scratch1);
				call(code, opts->read_8);
				mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, reg_offset(&(inst->dst))+3, SZ_B);
				pop_r(code, opts->gen.scratch1);
				add_ir(code, 2, opts->gen.scratch1, SZ_D);
				push_r(code, opts->gen.scratch1);
				call(code, opts->read_8);
				mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, reg_offset(&(inst->dst))+2, SZ_B);
			}
			pop_r(code, opts->gen.scratch1);
			add_ir(code, 2, opts->gen.scratch1, SZ_D);
		}
		push_r(code, opts->gen.scratch1);
		call(code, opts->read_8);
		if (reg >= 0) {

			shl_ir(code, 8, opts->gen.scratch1, SZ_W);
			mov_rr(code, opts->gen.scratch1, reg, SZ_W);
			pop_r(code, opts->gen.scratch1);
			add_ir(code, 2, opts->gen.scratch1, SZ_D);
			call(code, opts->read_8);
			mov_rr(code, opts->gen.scratch1, reg, SZ_B);
		} else {
			mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, reg_offset(&(inst->dst))+1, SZ_B);
			pop_r(code, opts->gen.scratch1);
			add_ir(code, 2, opts->gen.scratch1, SZ_D);
			call(code, opts->read_8);
			mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, reg_offset(&(inst->dst)), SZ_B);
		}
	}
}

typedef void (*shift_ir_t)(code_info *code, uint8_t val, uint8_t dst, uint8_t size);
typedef void (*shift_irdisp_t)(code_info *code, uint8_t val, uint8_t dst_base, int32_t disp, uint8_t size);
typedef void (*shift_clr_t)(code_info *code, uint8_t dst, uint8_t size);
typedef void (*shift_clrdisp_t)(code_info *code, uint8_t dst_base, int32_t disp, uint8_t size);

void translate_shift(m68k_options * opts, m68kinst * inst, host_ea *src_op, host_ea * dst_op, shift_ir_t shift_ir, shift_irdisp_t shift_irdisp, shift_clr_t shift_clr, shift_clrdisp_t shift_clrdisp, shift_ir_t special, shift_irdisp_t special_disp)
{
	code_info *code = &opts->gen.code;
	code_ptr end_off = NULL;
	code_ptr nz_off = NULL;
	code_ptr z_off = NULL;
	if (inst->src.addr_mode == MODE_UNUSED) {
		cycles(&opts->gen, BUS);
		//Memory shift
		shift_ir(code, 1, dst_op->base, SZ_W);
	} else {
		if (src_op->mode == MODE_IMMED) {
			cycles(&opts->gen, (inst->extra.size == OPSIZE_LONG ? 8 : 6) + 2 * src_op->disp);
			if (src_op->disp != 1 && inst->op == M68K_ASL) {
				set_flag(opts, 0, FLAG_V);
				for (int i = 0; i < src_op->disp; i++) {
					if (dst_op->mode == MODE_REG_DIRECT) {
						shift_ir(code, 1, dst_op->base, inst->extra.size);
					} else {
						shift_irdisp(code, 1, dst_op->base, dst_op->disp, inst->extra.size);
					}
					check_alloc_code(code, 2*MAX_INST_LEN);
					code_ptr after_flag_set = code->cur + 1;
					jcc(code, CC_NO, code->cur + 2);
					set_flag(opts, 1, FLAG_V);
					*after_flag_set = code->cur - (after_flag_set+1);
				}
			} else {
				if (dst_op->mode == MODE_REG_DIRECT) {
					shift_ir(code, src_op->disp, dst_op->base, inst->extra.size);
				} else {
					shift_irdisp(code, src_op->disp, dst_op->base, dst_op->disp, inst->extra.size);
				}
				set_flag_cond(opts, CC_O, FLAG_V);
			}
		} else {
			cycles(&opts->gen, inst->extra.size == OPSIZE_LONG ? 8 : 6);
			if (src_op->base != RCX) {
				if (src_op->mode == MODE_REG_DIRECT) {
					mov_rr(code, src_op->base, RCX, SZ_B);
				} else {
					mov_rdispr(code, src_op->base, src_op->disp, RCX, SZ_B);
				}

			}
			and_ir(code, 63, RCX, SZ_D);
			check_alloc_code(code, 7*MAX_INST_LEN);
			nz_off = code->cur + 1;
			jcc(code, CC_NZ, code->cur + 2);
			//Flag behavior for shift count of 0 is different for x86 than 68K
			if (dst_op->mode == MODE_REG_DIRECT) {
				cmp_ir(code, 0, dst_op->base, inst->extra.size);
			} else {
				cmp_irdisp(code, 0, dst_op->base, dst_op->disp, inst->extra.size);
			}
			set_flag_cond(opts, CC_Z, FLAG_Z);
			set_flag_cond(opts, CC_S, FLAG_N);
			set_flag(opts, 0, FLAG_C);
			//For other instructions, this flag will be set below
			if (inst->op == M68K_ASL) {
				set_flag(opts, 0, FLAG_V);
			}
			z_off = code->cur + 1;
			jmp(code, code->cur + 2);
			*nz_off = code->cur - (nz_off + 1);
			//add 2 cycles for every bit shifted
			mov_ir(code, 2 * opts->gen.clock_divider, opts->gen.scratch2, SZ_D);
			imul_rr(code, RCX, opts->gen.scratch2, SZ_D);
			add_rr(code, opts->gen.scratch2, opts->gen.cycles, SZ_D);
			if (inst->op == M68K_ASL) {
				//ASL has Overflow flag behavior that depends on all of the bits shifted through the MSB
				//Easiest way to deal with this is to shift one bit at a time
				set_flag(opts, 0, FLAG_V);
				check_alloc_code(code, 5*MAX_INST_LEN);
				code_ptr loop_start = code->cur;
				if (dst_op->mode == MODE_REG_DIRECT) {
					shift_ir(code, 1, dst_op->base, inst->extra.size);
				} else {
					shift_irdisp(code, 1, dst_op->base, dst_op->disp, inst->extra.size);
				}
				code_ptr after_flag_set = code->cur + 1;
				jcc(code, CC_NO, code->cur + 2);
				set_flag(opts, 1, FLAG_V);
				*after_flag_set = code->cur - (after_flag_set+1);
				loop(code, loop_start);
			} else {
				//x86 shifts modulo 32 for operand sizes less than 64-bits
				//but M68K shifts modulo 64, so we need to check for large shifts here
				cmp_ir(code, 32, RCX, SZ_B);
				check_alloc_code(code, 14*MAX_INST_LEN);
				code_ptr norm_shift_off = code->cur + 1;
				jcc(code, CC_L, code->cur + 2);
				if (special) {
					code_ptr after_flag_set = NULL;
					if (inst->extra.size == OPSIZE_LONG) {
						code_ptr neq_32_off = code->cur + 1;
						jcc(code, CC_NZ, code->cur + 2);

						//set the carry bit to the lsb
						if (dst_op->mode == MODE_REG_DIRECT) {
							special(code, 1, dst_op->base, SZ_D);
						} else {
							special_disp(code, 1, dst_op->base, dst_op->disp, SZ_D);
						}
						set_flag_cond(opts, CC_C, FLAG_C);
						after_flag_set = code->cur + 1;
						jmp(code, code->cur + 2);
						*neq_32_off = code->cur - (neq_32_off+1);
					}
					set_flag(opts, 0, FLAG_C);
					if (after_flag_set) {
						*after_flag_set = code->cur - (after_flag_set+1);
					}
					set_flag(opts, 1, FLAG_Z);
					set_flag(opts, 0, FLAG_N);
					if (dst_op->mode == MODE_REG_DIRECT) {
						xor_rr(code, dst_op->base, dst_op->base, inst->extra.size);
					} else {
						mov_irdisp(code, 0, dst_op->base, dst_op->disp, inst->extra.size);
					}
				} else {
					if (dst_op->mode == MODE_REG_DIRECT) {
						shift_ir(code, 31, dst_op->base, inst->extra.size);
						shift_ir(code, 1, dst_op->base, inst->extra.size);
					} else {
						shift_irdisp(code, 31, dst_op->base, dst_op->disp, inst->extra.size);
						shift_irdisp(code, 1, dst_op->base, dst_op->disp, inst->extra.size);
					}

				}
				end_off = code->cur + 1;
				jmp(code, code->cur + 2);
				*norm_shift_off = code->cur - (norm_shift_off+1);
				if (dst_op->mode == MODE_REG_DIRECT) {
					shift_clr(code, dst_op->base, inst->extra.size);
				} else {
					shift_clrdisp(code, dst_op->base, dst_op->disp, inst->extra.size);
				}
			}
		}

	}
	if (!special && end_off) {
		*end_off = code->cur - (end_off + 1);
	}
	update_flags(opts, C|Z|N);
	if (special && end_off) {
		*end_off = code->cur - (end_off + 1);
	}
	//set X flag to same as C flag
	if (opts->flag_regs[FLAG_C] >= 0) {
		flag_to_flag(opts, FLAG_C, FLAG_X);
	} else {
		set_flag_cond(opts, CC_C, FLAG_X);
	}
	if (z_off) {
		*z_off = code->cur - (z_off + 1);
	}
	if (inst->op != M68K_ASL) {
		set_flag(opts, 0, FLAG_V);
	}
	if (inst->src.addr_mode == MODE_UNUSED) {
		m68k_save_result(inst, opts);
	}
}

void op_ir(code_info *code, m68kinst *inst, int32_t val, uint8_t dst, uint8_t size)
{
	switch (inst->op)
	{
	case M68K_ADD:  add_ir(code, val, dst, size); break;
	case M68K_ADDX: adc_ir(code, val, dst, size); break;
	case M68K_AND:  and_ir(code, val, dst, size); break;
	case M68K_BTST: bt_ir(code, val, dst, size); break;
	case M68K_BSET: bts_ir(code, val, dst, size); break;
	case M68K_BCLR: btr_ir(code, val, dst, size); break;
	case M68K_BCHG: btc_ir(code, val, dst, size); break;
	case M68K_CMP:  cmp_ir(code, val, dst, size); break;
	case M68K_EOR:  xor_ir(code, val, dst, size); break;
	case M68K_OR:   or_ir(code, val, dst, size); break;
	case M68K_ROL:  rol_ir(code, val, dst, size); break;
	case M68K_ROR:  ror_ir(code, val, dst, size); break;
	case M68K_ROXL: rcl_ir(code, val, dst, size); break;
	case M68K_ROXR: rcr_ir(code, val, dst, size); break;
	case M68K_SUB:  sub_ir(code, val, dst, size); break;
	case M68K_SUBX: sbb_ir(code, val, dst, size); break;
	}
}

void op_irdisp(code_info *code, m68kinst *inst, int32_t val, uint8_t dst, int32_t disp, uint8_t size)
{
	switch (inst->op)
	{
	case M68K_ADD:  add_irdisp(code, val, dst, disp, size); break;
	case M68K_ADDX: adc_irdisp(code, val, dst, disp, size); break;
	case M68K_AND:  and_irdisp(code, val, dst, disp, size); break;
	case M68K_BTST: bt_irdisp(code, val, dst, disp, size); break;
	case M68K_BSET: bts_irdisp(code, val, dst, disp, size); break;
	case M68K_BCLR: btr_irdisp(code, val, dst, disp, size); break;
	case M68K_BCHG: btc_irdisp(code, val, dst, disp, size); break;
	case M68K_CMP:  cmp_irdisp(code, val, dst, disp, size); break;
	case M68K_EOR:  xor_irdisp(code, val, dst, disp, size); break;
	case M68K_OR:   or_irdisp(code, val, dst, disp, size); break;
	case M68K_ROL:  rol_irdisp(code, val, dst, disp, size); break;
	case M68K_ROR:  ror_irdisp(code, val, dst, disp, size); break;
	case M68K_ROXL: rcl_irdisp(code, val, dst, disp, size); break;
	case M68K_ROXR: rcr_irdisp(code, val, dst, disp, size); break;
	case M68K_SUB:  sub_irdisp(code, val, dst, disp, size); break;
	case M68K_SUBX: sbb_irdisp(code, val, dst, disp, size); break;
	}
}

void op_rr(code_info *code, m68kinst *inst, uint8_t src, uint8_t dst, uint8_t size)
{
	switch (inst->op)
	{
	case M68K_ADD:  add_rr(code, src, dst, size); break;
	case M68K_ADDX: adc_rr(code, src, dst, size); break;
	case M68K_AND:  and_rr(code, src, dst, size); break;
	case M68K_BTST: bt_rr(code, src, dst, size); break;
	case M68K_BSET: bts_rr(code, src, dst, size); break;
	case M68K_BCLR: btr_rr(code, src, dst, size); break;
	case M68K_BCHG: btc_rr(code, src, dst, size); break;
	case M68K_CMP:  cmp_rr(code, src, dst, size); break;
	case M68K_EOR:  xor_rr(code, src, dst, size); break;
	case M68K_OR:   or_rr(code, src, dst, size); break;
	case M68K_SUB:  sub_rr(code, src, dst, size); break;
	case M68K_SUBX: sbb_rr(code, src, dst, size); break;
	}
}

void op_rrdisp(code_info *code, m68kinst *inst, uint8_t src, uint8_t dst, int32_t disp, uint8_t size)
{
	switch(inst->op)
	{
	case M68K_ADD:  add_rrdisp(code, src, dst, disp, size); break;
	case M68K_ADDX: adc_rrdisp(code, src, dst, disp, size); break;
	case M68K_AND:  and_rrdisp(code, src, dst, disp, size); break;
	case M68K_BTST: bt_rrdisp(code, src, dst, disp, size); break;
	case M68K_BSET: bts_rrdisp(code, src, dst, disp, size); break;
	case M68K_BCLR: btr_rrdisp(code, src, dst, disp, size); break;
	case M68K_BCHG: btc_rrdisp(code, src, dst, disp, size); break;
	case M68K_CMP:  cmp_rrdisp(code, src, dst, disp, size); break;
	case M68K_EOR:  xor_rrdisp(code, src, dst, disp, size); break;
	case M68K_OR:   or_rrdisp(code, src, dst, disp, size); break;
	case M68K_SUB:  sub_rrdisp(code, src, dst, disp, size); break;
	case M68K_SUBX: sbb_rrdisp(code, src, dst, disp, size); break;
	}
}

void op_rdispr(code_info *code, m68kinst *inst, uint8_t src, int32_t disp, uint8_t dst, uint8_t size)
{
	switch (inst->op)
	{
	case M68K_ADD:  add_rdispr(code, src, disp, dst, size); break;
	case M68K_ADDX: adc_rdispr(code, src, disp, dst, size); break;
	case M68K_AND:  and_rdispr(code, src, disp, dst, size); break;
	case M68K_CMP:  cmp_rdispr(code, src, disp, dst, size); break;
	case M68K_EOR:  xor_rdispr(code, src, disp, dst, size); break;
	case M68K_OR:   or_rdispr(code, src, disp, dst, size); break;
	case M68K_SUB:  sub_rdispr(code, src, disp, dst, size); break;
	case M68K_SUBX: sbb_rdispr(code, src, disp, dst, size); break;
	}
}

void translate_m68k_arith(m68k_options *opts, m68kinst * inst, uint32_t flag_mask, host_ea *src_op, host_ea *dst_op)
{
	code_info *code = &opts->gen.code;
	cycles(&opts->gen, BUS);
	if (inst->op == M68K_ADDX || inst->op == M68K_SUBX) {
		flag_to_carry(opts, FLAG_X);
	}
	uint8_t size = inst->dst.addr_mode == MODE_AREG ? OPSIZE_LONG : inst->extra.size;
	if (src_op->mode == MODE_REG_DIRECT) {
		if (dst_op->mode == MODE_REG_DIRECT) {
			op_rr(code, inst, src_op->base, dst_op->base, size);
		} else {
			op_rrdisp(code, inst, src_op->base, dst_op->base, dst_op->disp, size);
		}
	} else if (src_op->mode == MODE_REG_DISPLACE8) {
		op_rdispr(code, inst, src_op->base, src_op->disp, dst_op->base, size);
	} else {
		if (dst_op->mode == MODE_REG_DIRECT) {
			op_ir(code, inst, src_op->disp, dst_op->base, size);
		} else {
			op_irdisp(code, inst, src_op->disp, dst_op->base, dst_op->disp, size);
		}
	}
	if (inst->dst.addr_mode != MODE_AREG || inst->op == M68K_CMP) {
		update_flags(opts, flag_mask);
		if (inst->op == M68K_ADDX || inst->op == M68K_SUBX) {
			check_alloc_code(code, 2*MAX_INST_LEN);
			code_ptr after_flag_set = code->cur + 1;
			jcc(code, CC_Z, code->cur + 2);
			set_flag(opts, 0, FLAG_Z);
			*after_flag_set = code->cur - (after_flag_set+1);
		}
	}
	if (inst->op != M68K_CMP) {
		m68k_save_result(inst, opts);
	}
}

void translate_m68k_cmp(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	uint8_t size = inst->extra.size;
	host_ea src_op, dst_op;
	translate_m68k_op(inst, &src_op, opts, 0);
	if (inst->dst.addr_mode == MODE_AREG_POSTINC) {
		push_r(code, opts->gen.scratch1);
		translate_m68k_op(inst, &dst_op, opts, 1);
		pop_r(code, opts->gen.scratch2);
		src_op.base = opts->gen.scratch2;
	} else {
		translate_m68k_op(inst, &dst_op, opts, 1);
		if (inst->dst.addr_mode == MODE_AREG && size == OPSIZE_WORD) {
			size = OPSIZE_LONG;
		}
	}
	translate_m68k_arith(opts, inst, N|Z|V|C, &src_op, &dst_op);
}

void op_r(code_info *code, m68kinst *inst, uint8_t dst, uint8_t size)
{
	switch(inst->op)
	{
	case M68K_NEG:   neg_r(code, dst, size); break;
	case M68K_NOT:   not_r(code, dst, size); cmp_ir(code, 0, dst, size); break;
	case M68K_ROL:   rol_clr(code, dst, size); break;
	case M68K_ROR:   ror_clr(code, dst, size); break;
	case M68K_ROXL:  rcl_clr(code, dst, size); break;
	case M68K_ROXR:  rcr_clr(code, dst, size); break;
	case M68K_SWAP:  rol_ir(code, 16, dst, SZ_D); cmp_ir(code, 0, dst, SZ_D); break;
	case M68K_TST:   cmp_ir(code, 0, dst, size); break;
	}
}

void op_rdisp(code_info *code, m68kinst *inst, uint8_t dst, int32_t disp, uint8_t size)
{
	switch(inst->op)
	{
	case M68K_NEG:   neg_rdisp(code, dst, disp, size); break;
	case M68K_NOT:   not_rdisp(code, dst, disp, size); cmp_irdisp(code, 0, dst, disp, size); break;
	case M68K_ROL:   rol_clrdisp(code, dst, disp, size); break;
	case M68K_ROR:   ror_clrdisp(code, dst, disp, size); break;
	case M68K_ROXL:  rcl_clrdisp(code, dst, disp, size); break;
	case M68K_ROXR:  rcr_clrdisp(code, dst, disp, size); break;
	case M68K_SWAP:  rol_irdisp(code, 16, dst, disp, SZ_D); cmp_irdisp(code, 0, dst, disp, SZ_D); break;
	case M68K_TST:   cmp_irdisp(code, 0, dst, disp, size); break;
	}
}

void translate_m68k_unary(m68k_options *opts, m68kinst *inst, uint32_t flag_mask, host_ea *dst_op)
{
	code_info *code = &opts->gen.code;
	cycles(&opts->gen, BUS);
	if (dst_op->mode == MODE_REG_DIRECT) {
		op_r(code, inst, dst_op->base, inst->extra.size);
	} else {
		op_rdisp(code, inst, dst_op->base, dst_op->disp, inst->extra.size);
	}
	update_flags(opts, flag_mask);
	m68k_save_result(inst, opts);
}

void translate_m68k_invalid(m68k_options *opts, m68kinst *inst)
{
	code_info *code = &opts->gen.code;
	if (inst->src.params.immed == 0x7100) {
		retn(code);
		return;
	}
	mov_ir(code, inst->address, opts->gen.scratch1, SZ_D);
	call(code, (code_ptr)m68k_invalid);
}

void translate_m68k_abcd_sbcd(m68k_options *opts, m68kinst *inst, host_ea *src_op, host_ea *dst_op)
{
	code_info *code = &opts->gen.code;
	if (src_op->base != opts->gen.scratch2) {
		if (src_op->mode == MODE_REG_DIRECT) {
			mov_rr(code, src_op->base, opts->gen.scratch2, SZ_B);
		} else {
			mov_rdispr(code, src_op->base, src_op->disp, opts->gen.scratch2, SZ_B);
		}
	}
	if (dst_op->base != opts->gen.scratch1) {
		if (dst_op->mode == MODE_REG_DIRECT) {
			mov_rr(code, dst_op->base, opts->gen.scratch1, SZ_B);
		} else {
			mov_rdispr(code, dst_op->base, dst_op->disp, opts->gen.scratch1, SZ_B);
		}
	}
	uint8_t other_reg;
	//WARNING: This may need adjustment if register assignments change
	if (opts->gen.scratch2 > RBX) {
		other_reg = RAX;
		xchg_rr(code, opts->gen.scratch2, RAX, SZ_D);
	} else {
		other_reg = opts->gen.scratch2;
	}
	mov_rr(code, opts->gen.scratch1, opts->gen.scratch1 + (AH-RAX), SZ_B);
	mov_rr(code, other_reg, other_reg + (AH-RAX), SZ_B);
	and_ir(code, 0xF0, opts->gen.scratch1, SZ_B);
	and_ir(code, 0xF0, other_reg, SZ_B);
	and_ir(code, 0xF, opts->gen.scratch1 + (AH-RAX), SZ_B);
	and_ir(code, 0xF, other_reg + (AH-RAX), SZ_B);
	//do op on low nibble
	flag_to_carry(opts, FLAG_X);
	if (inst->op == M68K_ABCD) {
		adc_rr(code, other_reg + (AH-RAX), opts->gen.scratch1 + (AH-RAX), SZ_B);
	} else {
		sbb_rr(code, other_reg + (AH-RAX), opts->gen.scratch1 + (AH-RAX), SZ_B);
	}
	cmp_ir(code, 0xA, opts->gen.scratch1 + (AH-RAX), SZ_B);
	code_ptr no_adjust = code->cur+1;
	//add correction factor if necessary
	jcc(code, CC_B, no_adjust);
	if (inst->op == M68K_ABCD) {
		add_ir(code, 6, opts->gen.scratch1 + (AH-RAX), SZ_B);
	} else {
		sub_ir(code, 6, opts->gen.scratch1 + (AH-RAX), SZ_B);
	}
	*no_adjust = code->cur - (no_adjust+1);
	//add low nibble result to one of the high nibble operands
	add_rr(code, opts->gen.scratch1 + (AH-RAX), opts->gen.scratch1, SZ_B);
	if (inst->op == M68K_ABCD) {
		add_rr(code, other_reg, opts->gen.scratch1, SZ_B);
	} else {
		sub_rr(code, other_reg, opts->gen.scratch1, SZ_B);
	}
	if (opts->gen.scratch2 > RBX) {
		mov_rr(code, opts->gen.scratch2, RAX, SZ_D);
	}
	set_flag(opts, 0, FLAG_C);
	set_flag(opts, 0, FLAG_V);
	code_ptr def_adjust = code->cur+1;
	jcc(code, CC_C, def_adjust);
	cmp_ir(code, 0xA0, opts->gen.scratch1, SZ_B);
	no_adjust = code->cur+1;
	jcc(code, CC_B, no_adjust);
	*def_adjust = code->cur - (def_adjust + 1);
	set_flag(opts, 1, FLAG_C);
	if (inst->op == M68K_ABCD) {
		add_ir(code, 0x60, opts->gen.scratch1, SZ_B);
	} else {
		sub_ir(code, 0x60, opts->gen.scratch1, SZ_B);
	}
	//V flag is set based on the result of the addition of the
	//result and the correction factor
	set_flag_cond(opts, CC_O, FLAG_V);
	*no_adjust = code->cur - (no_adjust+1);
	flag_to_flag(opts, FLAG_C, FLAG_X);

	cmp_ir(code, 0, opts->gen.scratch1, SZ_B);
	set_flag_cond(opts, CC_S, FLAG_N);
	jcc(code, CC_Z, code->cur + 4);
	set_flag(opts, 0, FLAG_Z);
	if (dst_op->base != opts->gen.scratch1) {
		if (dst_op->mode == MODE_REG_DIRECT) {
			mov_rr(code, opts->gen.scratch1, dst_op->base, SZ_B);
		} else {
			mov_rrdisp(code, opts->gen.scratch1, dst_op->base, dst_op->disp, SZ_B);
		}
	}
	m68k_save_result(inst, opts);
}

void translate_m68k_sl(m68k_options *opts, m68kinst *inst, host_ea *src_op, host_ea *dst_op)
{
	translate_shift(opts, inst, src_op, dst_op, shl_ir, shl_irdisp, shl_clr, shl_clrdisp, shr_ir, shr_irdisp);
}

void translate_m68k_asr(m68k_options *opts, m68kinst *inst, host_ea *src_op, host_ea *dst_op)
{
	translate_shift(opts, inst, src_op, dst_op, sar_ir, sar_irdisp, sar_clr, sar_clrdisp, NULL, NULL);
}

void translate_m68k_lsr(m68k_options *opts, m68kinst *inst, host_ea *src_op, host_ea *dst_op)
{
	translate_shift(opts, inst, src_op, dst_op, shr_ir, shr_irdisp, shr_clr, shr_clrdisp, shl_ir, shl_irdisp);
}

void translate_m68k_bit(m68k_options *opts, m68kinst *inst, host_ea *src_op, host_ea *dst_op)
{
	code_info *code = &opts->gen.code;
	cycles(&opts->gen, inst->extra.size == OPSIZE_BYTE ? 4 : (
			inst->op == M68K_BTST ? 6 : (inst->op == M68K_BCLR ? 10 : 8))
	);
	if (src_op->mode == MODE_IMMED) {
		if (inst->extra.size == OPSIZE_BYTE) {
			src_op->disp &= 0x7;
		}
		if (dst_op->mode == MODE_REG_DIRECT) {
			op_ir(code, inst, src_op->disp, dst_op->base, inst->extra.size);
		} else {
			op_irdisp(code, inst, src_op->disp, dst_op->base, dst_op->disp, inst->extra.size);
		}
	} else {
		if (src_op->mode == MODE_REG_DISPLACE8 || (inst->dst.addr_mode != MODE_REG && src_op->base != opts->gen.scratch1 && src_op->base != opts->gen.scratch2)) {
			if (dst_op->base == opts->gen.scratch1) {
				push_r(code, opts->gen.scratch2);
				if (src_op->mode == MODE_REG_DIRECT) {
					mov_rr(code, src_op->base, opts->gen.scratch2, SZ_B);
				} else {
					mov_rdispr(code, src_op->base, src_op->disp, opts->gen.scratch2, SZ_B);
				}
				src_op->base = opts->gen.scratch2;
			} else {
				if (src_op->mode == MODE_REG_DIRECT) {
					mov_rr(code, src_op->base, opts->gen.scratch1, SZ_B);
				} else {
					mov_rdispr(code, src_op->base, src_op->disp, opts->gen.scratch1, SZ_B);
				}
				src_op->base = opts->gen.scratch1;
				}
			}
			uint8_t size = inst->extra.size;
		if (dst_op->mode == MODE_REG_DISPLACE8) {
			if (src_op->base != opts->gen.scratch1 && src_op->base != opts->gen.scratch2) {
				if (src_op->mode == MODE_REG_DIRECT) {
					mov_rr(code, src_op->base, opts->gen.scratch1, SZ_D);
				} else {
					mov_rdispr(code, src_op->base, src_op->disp, opts->gen.scratch1, SZ_D);
					src_op->mode = MODE_REG_DIRECT;
				}
				src_op->base = opts->gen.scratch1;
			}
			//b### with register destination is modulo 32
			//x86 with a memory destination isn't modulo anything
			//so use an and here to force the value to be modulo 32
			and_ir(code, 31, opts->gen.scratch1, SZ_D);
		} else if(inst->dst.addr_mode != MODE_REG) {
			//b### with memory destination is modulo 8
			//x86-64 doesn't support 8-bit bit operations
			//so we fake it by forcing the bit number to be modulo 8
			and_ir(code, 7, src_op->base, SZ_D);
			size = SZ_D;
		}
		if (dst_op->mode == MODE_REG_DIRECT) {
			op_rr(code, inst, src_op->base, dst_op->base, size);
		} else {
			op_rrdisp(code, inst, src_op->base, dst_op->base, dst_op->disp, size);
		}
		if (src_op->base == opts->gen.scratch2) {
			pop_r(code, opts->gen.scratch2);
		}
	}
	//x86 sets the carry flag to the value of the bit tested
	//68K sets the zero flag to the complement of the bit tested
	set_flag_cond(opts, CC_NC, FLAG_Z);
	if (inst->op != M68K_BTST) {
		m68k_save_result(inst, opts);
	}
}

void translate_m68k_chk(m68k_options *opts, m68kinst *inst, host_ea *src_op, host_ea *dst_op)
	{
	code_info *code = &opts->gen.code;
	cycles(&opts->gen, 6);
	if (dst_op->mode == MODE_REG_DIRECT) {
		cmp_ir(code, 0, dst_op->base, inst->extra.size);
	} else {
		cmp_irdisp(code, 0, dst_op->base, dst_op->disp, inst->extra.size);
	}
	uint32_t isize;
	switch(inst->src.addr_mode)
	{
	case MODE_AREG_DISPLACE:
	case MODE_AREG_INDEX_DISP8:
	case MODE_ABSOLUTE_SHORT:
	case MODE_PC_INDEX_DISP8:
	case MODE_PC_DISPLACE:
	case MODE_IMMEDIATE:
		isize = 4;
		break;
	case MODE_ABSOLUTE:
		isize = 6;
		break;
	default:
		isize = 2;
	}
	//make sure we won't start a new chunk in the middle of these branches
	check_alloc_code(code, MAX_INST_LEN * 11);
	code_ptr passed = code->cur + 1;
	jcc(code, CC_GE, code->cur + 2);
	set_flag(opts, 1, FLAG_N);
	mov_ir(code, VECTOR_CHK, opts->gen.scratch2, SZ_D);
	mov_ir(code, inst->address+isize, opts->gen.scratch1, SZ_D);
	jmp(code, opts->trap);
	*passed = code->cur - (passed+1);
	if (dst_op->mode == MODE_REG_DIRECT) {
		if (src_op->mode == MODE_REG_DIRECT) {
			cmp_rr(code, src_op->base, dst_op->base, inst->extra.size);
		} else if(src_op->mode == MODE_REG_DISPLACE8) {
			cmp_rdispr(code, src_op->base, src_op->disp, dst_op->base, inst->extra.size);
		} else {
			cmp_ir(code, src_op->disp, dst_op->base, inst->extra.size);
		}
	} else if(dst_op->mode == MODE_REG_DISPLACE8) {
		if (src_op->mode == MODE_REG_DIRECT) {
			cmp_rrdisp(code, src_op->base, dst_op->base, dst_op->disp, inst->extra.size);
		} else {
			cmp_irdisp(code, src_op->disp, dst_op->base, dst_op->disp, inst->extra.size);
		}
	}
	passed = code->cur + 1;
	jcc(code, CC_LE, code->cur + 2);
	set_flag(opts, 0, FLAG_N);
	mov_ir(code, VECTOR_CHK, opts->gen.scratch2, SZ_D);
	mov_ir(code, inst->address+isize, opts->gen.scratch1, SZ_D);
	jmp(code, opts->trap);
	*passed = code->cur - (passed+1);
	cycles(&opts->gen, 4);
}

void translate_m68k_div(m68k_options *opts, m68kinst *inst, host_ea *src_op, host_ea *dst_op)
{
	code_info *code = &opts->gen.code;
	check_alloc_code(code, MAX_NATIVE_SIZE);
	//TODO: cycle exact division
	cycles(&opts->gen, inst->op == M68K_DIVS ? 158 : 140);
	set_flag(opts, 0, FLAG_C);
	push_r(code, RDX);
	push_r(code, RAX);
	if (dst_op->mode == MODE_REG_DIRECT) {
		mov_rr(code, dst_op->base, RAX, SZ_D);
	} else {
		mov_rdispr(code, dst_op->base, dst_op->disp, RAX, SZ_D);
	}
	if (src_op->mode == MODE_IMMED) {
		mov_ir(code, (src_op->disp & 0x8000) && inst->op == M68K_DIVS ? src_op->disp | 0xFFFF0000 : src_op->disp, opts->gen.scratch2, SZ_D);
	} else if (src_op->mode == MODE_REG_DIRECT) {
		if (inst->op == M68K_DIVS) {
			movsx_rr(code, src_op->base, opts->gen.scratch2, SZ_W, SZ_D);
		} else {
			movzx_rr(code, src_op->base, opts->gen.scratch2, SZ_W, SZ_D);
		}
	} else if (src_op->mode == MODE_REG_DISPLACE8) {
		if (inst->op == M68K_DIVS) {
			movsx_rdispr(code, src_op->base, src_op->disp, opts->gen.scratch2, SZ_W, SZ_D);
		} else {
			movzx_rdispr(code, src_op->base, src_op->disp, opts->gen.scratch2, SZ_W, SZ_D);
		}
	}
	uint32_t isize = 2;
	switch(inst->src.addr_mode)
	{
	case MODE_AREG_DISPLACE:
	case MODE_AREG_INDEX_DISP8:
	case MODE_ABSOLUTE_SHORT:
	case MODE_PC_INDEX_DISP8:
	case MODE_IMMEDIATE:
		isize = 4;
		break;
	case MODE_ABSOLUTE:
		isize = 6;
		break;
	}
	cmp_ir(code, 0, opts->gen.scratch2, SZ_D);
	check_alloc_code(code, 6*MAX_INST_LEN);
	code_ptr not_zero = code->cur + 1;
	jcc(code, CC_NZ, code->cur + 2);
	pop_r(code, RAX);
	pop_r(code, RDX);
	mov_ir(code, VECTOR_INT_DIV_ZERO, opts->gen.scratch2, SZ_D);
	mov_ir(code, inst->address+isize, opts->gen.scratch1, SZ_D);
	jmp(code, opts->trap);
	*not_zero = code->cur - (not_zero+1);
	if (inst->op == M68K_DIVS) {
		cdq(code);
	} else {
		xor_rr(code, RDX, RDX, SZ_D);
	}
	if (inst->op == M68K_DIVS) {
		idiv_r(code, opts->gen.scratch2, SZ_D);
	} else {
		div_r(code, opts->gen.scratch2, SZ_D);
	}
	code_ptr skip_sec_check, norm_off;
	if (inst->op == M68K_DIVS) {
		cmp_ir(code, 0x8000, RAX, SZ_D);
		skip_sec_check = code->cur + 1;
		jcc(code, CC_GE, code->cur + 2);
		cmp_ir(code, -0x8000, RAX, SZ_D);
		norm_off = code->cur + 1;
		jcc(code, CC_L, code->cur + 2);
	} else {
		cmp_ir(code, 0x10000, RAX, SZ_D);
		norm_off = code->cur + 1;
		jcc(code, CC_NC, code->cur + 2);
	}
	if (dst_op->mode == MODE_REG_DIRECT) {
		mov_rr(code, RDX, dst_op->base, SZ_W);
		shl_ir(code, 16, dst_op->base, SZ_D);
		mov_rr(code, RAX, dst_op->base, SZ_W);
	} else {
		mov_rrdisp(code, RDX, dst_op->base, dst_op->disp, SZ_W);
		shl_irdisp(code, 16, dst_op->base, dst_op->disp, SZ_D);
		mov_rrdisp(code, RAX, dst_op->base, dst_op->disp, SZ_W);
	}
	cmp_ir(code, 0, RAX, SZ_W);
	pop_r(code, RAX);
	pop_r(code, RDX);
	update_flags(opts, V0|Z|N);
	code_ptr end_off = code->cur + 1;
	jmp(code, code->cur + 2);
	*norm_off = code->cur - (norm_off + 1);
	if (inst->op == M68K_DIVS) {
		*skip_sec_check = code->cur - (skip_sec_check+1);
	}
	pop_r(code, RAX);
	pop_r(code, RDX);
	set_flag(opts, 1, FLAG_V);
	*end_off = code->cur - (end_off + 1);
}

void translate_m68k_exg(m68k_options *opts, m68kinst *inst, host_ea *src_op, host_ea *dst_op)
{
	code_info *code = &opts->gen.code;
	cycles(&opts->gen, 6);
	if (dst_op->mode == MODE_REG_DIRECT) {
		mov_rr(code, dst_op->base, opts->gen.scratch2, SZ_D);
		if (src_op->mode == MODE_REG_DIRECT) {
			mov_rr(code, src_op->base, dst_op->base, SZ_D);
			mov_rr(code, opts->gen.scratch2, src_op->base, SZ_D);
		} else {
			mov_rdispr(code, src_op->base, src_op->disp, dst_op->base, SZ_D);
			mov_rrdisp(code, opts->gen.scratch2, src_op->base, src_op->disp, SZ_D);
		}
	} else {
		mov_rdispr(code, dst_op->base, dst_op->disp, opts->gen.scratch2, SZ_D);
		if (src_op->mode == MODE_REG_DIRECT) {
			mov_rrdisp(code, src_op->base, dst_op->base, dst_op->disp, SZ_D);
			mov_rr(code, opts->gen.scratch2, src_op->base, SZ_D);
		} else {
			mov_rdispr(code, src_op->base, src_op->disp, opts->gen.scratch1, SZ_D);
			mov_rrdisp(code, opts->gen.scratch1, dst_op->base, dst_op->disp, SZ_D);
			mov_rrdisp(code, opts->gen.scratch2, src_op->base, src_op->disp, SZ_D);
		}
	}
}

void translate_m68k_mul(m68k_options *opts, m68kinst *inst, host_ea *src_op, host_ea *dst_op)
{
	code_info *code = &opts->gen.code;
	cycles(&opts->gen, 70); //TODO: Calculate the actual value based on the value of the <ea> parameter
	if (src_op->mode == MODE_IMMED) {
		mov_ir(code, inst->op == M68K_MULU ? (src_op->disp & 0xFFFF) : ((src_op->disp & 0x8000) ? src_op->disp | 0xFFFF0000 : src_op->disp), opts->gen.scratch1, SZ_D);
	} else if (src_op->mode == MODE_REG_DIRECT) {
		if (inst->op == M68K_MULS) {
			movsx_rr(code, src_op->base, opts->gen.scratch1, SZ_W, SZ_D);
		} else {
			movzx_rr(code, src_op->base, opts->gen.scratch1, SZ_W, SZ_D);
		}
	} else {
		if (inst->op == M68K_MULS) {
			movsx_rdispr(code, src_op->base, src_op->disp, opts->gen.scratch1, SZ_W, SZ_D);
		} else {
			movzx_rdispr(code, src_op->base, src_op->disp, opts->gen.scratch1, SZ_W, SZ_D);
		}
	}
	uint8_t dst_reg;
	if (dst_op->mode == MODE_REG_DIRECT) {
		dst_reg = dst_op->base;
		if (inst->op == M68K_MULS) {
			movsx_rr(code, dst_reg, dst_reg, SZ_W, SZ_D);
		} else {
			movzx_rr(code, dst_reg, dst_reg, SZ_W, SZ_D);
		}
	} else {
		dst_reg = opts->gen.scratch2;
		if (inst->op == M68K_MULS) {
			movsx_rdispr(code, dst_op->base, dst_op->disp, opts->gen.scratch2, SZ_W, SZ_D);
		} else {
			movzx_rdispr(code, dst_op->base, dst_op->disp, opts->gen.scratch2, SZ_W, SZ_D);
		}
	}
	imul_rr(code, opts->gen.scratch1, dst_reg, SZ_D);
	if (dst_op->mode == MODE_REG_DISPLACE8) {
		mov_rrdisp(code, dst_reg, dst_op->base, dst_op->disp, SZ_D);
	}
	cmp_ir(code, 0, dst_reg, SZ_D);
	update_flags(opts, N|Z|V0|C0);
}

void translate_m68k_negx(m68k_options *opts, m68kinst *inst, host_ea *src_op, host_ea *dst_op)
{
	code_info *code = &opts->gen.code;
	cycles(&opts->gen, BUS);
	if (dst_op->mode == MODE_REG_DIRECT) {
		if (dst_op->base == opts->gen.scratch1) {
			push_r(code, opts->gen.scratch2);
			xor_rr(code, opts->gen.scratch2, opts->gen.scratch2, inst->extra.size);
			flag_to_carry(opts, FLAG_X);
			sbb_rr(code, dst_op->base, opts->gen.scratch2, inst->extra.size);
			mov_rr(code, opts->gen.scratch2, dst_op->base, inst->extra.size);
			pop_r(code, opts->gen.scratch2);
		} else {
			xor_rr(code, opts->gen.scratch1, opts->gen.scratch1, inst->extra.size);
			flag_to_carry(opts, FLAG_X);
			sbb_rr(code, dst_op->base, opts->gen.scratch1, inst->extra.size);
			mov_rr(code, opts->gen.scratch1, dst_op->base, inst->extra.size);
		}
	} else {
		xor_rr(code, opts->gen.scratch1, opts->gen.scratch1, inst->extra.size);
		flag_to_carry(opts, FLAG_X);
		sbb_rdispr(code, dst_op->base, dst_op->disp, opts->gen.scratch1, inst->extra.size);
		mov_rrdisp(code, opts->gen.scratch1, dst_op->base, dst_op->disp, inst->extra.size);
	}
	set_flag_cond(opts, CC_C, FLAG_C);
	code_ptr after_flag_set = code->cur + 1;
	jcc(code, CC_Z, code->cur + 2);
	set_flag(opts, 0, FLAG_Z);
	*after_flag_set = code->cur - (after_flag_set+1);
	set_flag_cond(opts, CC_S, FLAG_N);
	set_flag_cond(opts, CC_O, FLAG_V);
	if (opts->flag_regs[FLAG_C] >= 0) {
		flag_to_flag(opts, FLAG_C, FLAG_X);
	} else {
		set_flag_cond(opts, CC_C, FLAG_X);
	}
	m68k_save_result(inst, opts);
}

void translate_m68k_rot(m68k_options *opts, m68kinst *inst, host_ea *src_op, host_ea *dst_op)
{
	code_info *code = &opts->gen.code;
	int32_t init_flags = C|V0;
	if (inst->src.addr_mode == MODE_UNUSED) {
		cycles(&opts->gen, BUS);
		//Memory rotate
		if (inst->op == M68K_ROXR || inst->op == M68K_ROXL) {
			flag_to_carry(opts, FLAG_X);
			init_flags |= X;
		}
		op_ir(code, inst, 1, dst_op->base, inst->extra.size);
		update_flags(opts, init_flags);
		cmp_ir(code, 0, dst_op->base, inst->extra.size);
		update_flags(opts, Z|N);
		m68k_save_result(inst, opts);
	} else {
		if (src_op->mode == MODE_IMMED) {
			cycles(&opts->gen, (inst->extra.size == OPSIZE_LONG ? 8 : 6) + src_op->disp*2);
			if (inst->op == M68K_ROXR || inst->op == M68K_ROXL) {
				flag_to_carry(opts, FLAG_X);
				init_flags |= X;
			}
			if (dst_op->mode == MODE_REG_DIRECT) {
				op_ir(code, inst, src_op->disp, dst_op->base, inst->extra.size);
			} else {
				op_irdisp(code, inst, src_op->disp, dst_op->base, dst_op->disp, inst->extra.size);
			}
			update_flags(opts, init_flags);
		} else {
			if (src_op->mode == MODE_REG_DIRECT) {
				if (src_op->base != opts->gen.scratch1) {
					mov_rr(code, src_op->base, opts->gen.scratch1, SZ_B);
				}
			} else {
				mov_rdispr(code, src_op->base, src_op->disp, opts->gen.scratch1, SZ_B);
			}
			and_ir(code, 63, opts->gen.scratch1, SZ_D);
			code_ptr zero_off = code->cur + 1;
			jcc(code, CC_Z, code->cur + 2);
			//add 2 cycles for every bit shifted
			mov_ir(code, 2 * opts->gen.clock_divider, opts->gen.scratch2, SZ_D);
			imul_rr(code, RCX, opts->gen.scratch2, SZ_D);
			add_rr(code, opts->gen.scratch2, opts->gen.cycles, SZ_D);
			cmp_ir(code, 32, opts->gen.scratch1, SZ_B);
			code_ptr norm_off = code->cur + 1;
			jcc(code, CC_L, code->cur + 2);
			if (inst->op == M68K_ROXR || inst->op == M68K_ROXL) {
				flag_to_carry(opts, FLAG_X);
				init_flags |= X;
			} else {
				sub_ir(code, 32, opts->gen.scratch1, SZ_B);
			}
			if (dst_op->mode == MODE_REG_DIRECT) {
				op_ir(code, inst, 31, dst_op->base, inst->extra.size);
				op_ir(code, inst, 1, dst_op->base, inst->extra.size);
			} else {
				op_irdisp(code, inst, 31, dst_op->base, dst_op->disp, inst->extra.size);
				op_irdisp(code, inst, 1, dst_op->base, dst_op->disp, inst->extra.size);
			}

			if (inst->op == M68K_ROXR || inst->op == M68K_ROXL) {
				set_flag_cond(opts, CC_C, FLAG_X);
				sub_ir(code, 32, opts->gen.scratch1, SZ_B);
				*norm_off = code->cur - (norm_off+1);
				flag_to_carry(opts, FLAG_X);
			} else {
				*norm_off = code->cur - (norm_off+1);
			}
			if (dst_op->mode == MODE_REG_DIRECT) {
				op_r(code, inst, dst_op->base, inst->extra.size);
			} else {
				op_rdisp(code, inst, dst_op->base, dst_op->disp, inst->extra.size);
			}
			update_flags(opts, init_flags);
			code_ptr end_off = code->cur + 1;
			jmp(code, code->cur + 2);
			*zero_off = code->cur - (zero_off+1);
			if (inst->op == M68K_ROXR || inst->op == M68K_ROXL) {
				//Carry flag is set to X flag when count is 0, this is different from ROR/ROL
				flag_to_flag(opts, FLAG_X, FLAG_C);
			} else {
				set_flag(opts, 0, FLAG_C);
			}
			*end_off = code->cur - (end_off+1);
		}
		if (dst_op->mode == MODE_REG_DIRECT) {
			cmp_ir(code, 0, dst_op->base, inst->extra.size);
		} else {
			cmp_irdisp(code, 0, dst_op->base, dst_op->disp, inst->extra.size);
		}
		update_flags(opts, Z|N);
	}
}

void translate_m68k_illegal(m68k_options *opts, m68kinst *inst)
{
	code_info *code = &opts->gen.code;
	call(code, opts->gen.save_context);
	call_args(code, (code_ptr)print_regs_exit, 1, opts->gen.context_reg);
}

#define BIT_SUPERVISOR 5

void translate_m68k_andi_ori_ccr_sr(m68k_options *opts, m68kinst *inst)
{
	code_info *code = &opts->gen.code;
	cycles(&opts->gen, 20);
	//TODO: If ANDI to SR, trap if not in supervisor mode
	uint32_t flag_mask = 0;
	uint32_t base_flag = inst->op == M68K_ANDI_SR || inst->op == M68K_ANDI_CCR ? X0 : X1;
	for (int i = 0; i < 5; i++)
	{
		if ((base_flag == X0) ^ (inst->src.params.immed & 1 << i) > 0)
		{
			flag_mask |= base_flag << ((4 - i) * 3);
		}
	}
	update_flags(opts, flag_mask);
	if (inst->op == M68K_ANDI_SR || inst->op == M68K_ORI_SR) {
		if (inst->op == M68K_ANDI_SR) {
			and_irdisp(code, inst->src.params.immed >> 8, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
		} else {
			or_irdisp(code, inst->src.params.immed >> 8, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
		}
		if (inst->op == M68K_ANDI_SR && !(inst->src.params.immed & (1 << (BIT_SUPERVISOR + 8)))) {
			//leave supervisor mode
			swap_ssp_usp(opts);
		}
		if ((inst->op == M68K_ANDI_SR && (inst->src.params.immed  & 0x700) != 0x700)
		    || (inst->op == M68K_ORI_SR && inst->src.params.immed & 0x700)) {
			call(code, opts->do_sync);
		}
	}
}

void translate_m68k_eori_ccr_sr(m68k_options *opts, m68kinst *inst)
{
	code_info *code = &opts->gen.code;
	cycles(&opts->gen, 20);
	//TODO: If ANDI to SR, trap if not in supervisor mode
	if (inst->src.params.immed & 0x1) {
		xor_flag(opts, 1, FLAG_C);
	}
	if (inst->src.params.immed & 0x2) {
		xor_flag(opts, 1, FLAG_V);
	}
	if (inst->src.params.immed & 0x4) {
		xor_flag(opts, 1, FLAG_Z);
	}
	if (inst->src.params.immed & 0x8) {
		xor_flag(opts, 1, FLAG_N);
	}
	if (inst->src.params.immed & 0x10) {
		xor_flag(opts, 1, FLAG_X);
	}
	if (inst->op == M68K_ORI_SR) {
		xor_irdisp(code, inst->src.params.immed >> 8, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
		if (inst->src.params.immed & 0x700) {
			call(code, opts->do_sync);
		}
	}
}

void set_all_flags(m68k_options *opts, uint8_t flags)
{
	uint32_t flag_mask = flags & 0x10 ? X1 : X0;
	flag_mask |= flags & 0x8 ? N1 : N0;
	flag_mask |= flags & 0x4 ? Z1 : Z0;
	flag_mask |= flags & 0x2 ? V1 : V0;
	flag_mask |= flags & 0x1 ? C1 : C0;
	update_flags(opts, flag_mask);
}

void translate_m68k_move_ccr_sr(m68k_options *opts, m68kinst *inst, host_ea *src_op, host_ea *dst_op)
{
	code_info *code = &opts->gen.code;
	//TODO: Privilege check for MOVE to SR
	if (src_op->mode == MODE_IMMED) {
		set_all_flags(opts, src_op->disp);
		if (inst->op == M68K_MOVE_SR) {
			mov_irdisp(code, (src_op->disp >> 8), opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
			if (!((inst->src.params.immed >> 8) & (1 << BIT_SUPERVISOR))) {
				//leave supervisor mode
				swap_ssp_usp(opts);
			}
			call(code, opts->do_sync);
		}
		cycles(&opts->gen, 12);
	} else {
		if (src_op->base != opts->gen.scratch1) {
			if (src_op->mode == MODE_REG_DIRECT) {
				mov_rr(code, src_op->base, opts->gen.scratch1, SZ_W);
			} else {
				mov_rdispr(code, src_op->base, src_op->disp, opts->gen.scratch1, SZ_W);
			}
		}
		call(code, inst->op == M68K_MOVE_SR ? opts->set_sr : opts->set_ccr);
		cycles(&opts->gen, 12);
	}
}

void translate_m68k_stop(m68k_options *opts, m68kinst *inst)
{
	//TODO: Trap if not in system mode
	//manual says 4 cycles, but it has to be at least 8 since it's a 2-word instruction
	//possibly even 12 since that's how long MOVE to SR takes
	//On further thought prefetch + the fact that this stops the CPU may make
	//Motorola's accounting make sense here
	code_info *code = &opts->gen.code;
	cycles(&opts->gen, BUS*2);
	set_all_flags(opts,  inst->src.params.immed);
	mov_irdisp(code, (inst->src.params.immed >> 8), opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
	if (!((inst->src.params.immed >> 8) & (1 << BIT_SUPERVISOR))) {
		//leave supervisor mode
		swap_ssp_usp(opts);
	}
	code_ptr loop_top = code->cur;
	call(code, opts->do_sync);
	cmp_rr(code, opts->gen.limit, opts->gen.cycles, SZ_D);
	code_ptr normal_cycle_up = code->cur + 1;
	jcc(code, CC_A, code->cur + 2);
	cycles(&opts->gen, BUS);
	code_ptr after_cycle_up = code->cur + 1;
	jmp(code, code->cur + 2);
	*normal_cycle_up = code->cur - (normal_cycle_up + 1);
	mov_rr(code, opts->gen.limit, opts->gen.cycles, SZ_D);
	*after_cycle_up = code->cur - (after_cycle_up+1);
	cmp_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, int_cycle), opts->gen.cycles, SZ_D);
	jcc(code, CC_C, loop_top);
}

void translate_m68k_move_from_sr(m68k_options *opts, m68kinst *inst, host_ea *src_op, host_ea *dst_op)
{
	code_info *code = &opts->gen.code;
	//TODO: Trap if not in system mode
	call(code, opts->get_sr);
	if (dst_op->mode == MODE_REG_DIRECT) {
		mov_rr(code, opts->gen.scratch1, dst_op->base, SZ_W);
	} else {
		mov_rrdisp(code, opts->gen.scratch1, dst_op->base, dst_op->disp, SZ_W);
	}
	m68k_save_result(inst, opts);
}

void translate_m68k_reset(m68k_options *opts, m68kinst *inst)
{
	code_info *code = &opts->gen.code;
	call(code, opts->gen.save_context);
	call_args(code, (code_ptr)print_regs_exit, 1, opts->gen.context_reg);
}

void translate_m68k_rte(m68k_options *opts, m68kinst *inst)
{
	code_info *code = &opts->gen.code;
	//TODO: Trap if not in system mode
	//Read saved SR
	areg_to_native(opts, 7, opts->gen.scratch1);
	call(code, opts->read_16);
	addi_areg(opts, 2, 7);
	call(code, opts->set_sr);
	//Read saved PC
	areg_to_native(opts, 7, opts->gen.scratch1);
	call(code, opts->read_32);
	addi_areg(opts, 4, 7);
	//Check if we've switched to user mode and swap stack pointers if needed
	bt_irdisp(code, 5, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
	code_ptr end_off = code->cur + 1;
	jcc(code, CC_C, code->cur + 2);
	swap_ssp_usp(opts);
	*end_off = code->cur - (end_off+1);
	//Get native address, sync components, recalculate integer points and jump to returned address
	call(code, opts->native_addr_and_sync);
	jmp_r(code, opts->gen.scratch1);
}

void translate_out_of_bounds(code_info *code)
{
	xor_rr(code, RDI, RDI, SZ_D);
	call_args(code, (code_ptr)exit, 1, RDI);
}

void nop_fill_or_jmp_next(code_info *code, code_ptr old_end, code_ptr next_inst)
{
	if (next_inst == old_end && next_inst - code->cur < 2) {
		while (code->cur < old_end) {
			*(code->cur++) = 0x90; //NOP
		}
	} else {
		jmp(code, next_inst);
	}
}

m68k_context * m68k_handle_code_write(uint32_t address, m68k_context * context)
{
	uint32_t inst_start = get_instruction_start(context->native_code_map, address | 0xFF0000);
	if (inst_start) {
		m68k_options * options = context->options;
		code_info *code = &options->gen.code;
		code_ptr dst = get_native_address(context->native_code_map, inst_start);
		code_info orig;
		orig.cur = dst;
		orig.last = dst + 128;
		mov_ir(&orig, inst_start, options->gen.scratch2, SZ_D);

		if (!options->retrans_stub) {
			options->retrans_stub = code->cur;
			call(code, options->gen.save_context);
			push_r(code, options->gen.context_reg);
			call_args(code,(code_ptr)m68k_retranslate_inst, 2, options->gen.scratch2, options->gen.context_reg);
			pop_r(code, options->gen.context_reg);
			mov_rr(code, RAX, options->gen.scratch1, SZ_PTR);
			call(code, options->gen.load_context);
			jmp_r(code, options->gen.scratch1);
		}
		jmp(&orig, options->retrans_stub);
	}
	return context;
}

void insert_breakpoint(m68k_context * context, uint32_t address, code_ptr bp_handler)
{
	static code_ptr bp_stub = NULL;
	m68k_options * opts = context->options;
	code_info native;
	native.cur = get_native_address_trans(context, address);
	native.last = native.cur + 128;
	code_ptr start_native = native.cur;
	mov_ir(&native, address, opts->gen.scratch1, SZ_D);
	if (!bp_stub) {
		code_info *code = &opts->gen.code;
		check_code_prologue(code);
		bp_stub = code->cur;
		call(&native, bp_stub);

		//Calculate length of prologue
		check_cycles_int(&opts->gen, address);
		int check_int_size = code->cur-bp_stub;
		code->cur = bp_stub;

		//Save context and call breakpoint handler
		call(code, opts->gen.save_context);
		push_r(code, opts->gen.scratch1);
		call_args_abi(code, bp_handler, 2, opts->gen.context_reg, opts->gen.scratch1);
		mov_rr(code, RAX, opts->gen.context_reg, SZ_PTR);
		//Restore context
		call(code, opts->gen.load_context);
		pop_r(code, opts->gen.scratch1);
		//do prologue stuff
		cmp_rr(code, opts->gen.cycles, opts->gen.limit, SZ_D);
		code_ptr jmp_off = code->cur + 1;
		jcc(code, CC_NC, code->cur + 7);
		call(code, opts->gen.handle_cycle_limit_int);
		*jmp_off = code->cur - (jmp_off+1);
		//jump back to body of translated instruction
		pop_r(code, opts->gen.scratch1);
		add_ir(code, check_int_size - (native.cur-start_native), opts->gen.scratch1, SZ_PTR);
		jmp_r(code, opts->gen.scratch1);
	} else {
		call(&native, bp_stub);
	}
}

void init_m68k_opts(m68k_options * opts, memmap_chunk * memmap, uint32_t num_chunks, uint32_t clock_divider)
{
	memset(opts, 0, sizeof(*opts));
	opts->gen.memmap = memmap;
	opts->gen.memmap_chunks = num_chunks;
	opts->gen.address_size = SZ_D;
	opts->gen.address_mask = 0xFFFFFF;
	opts->gen.byte_swap = 1;
	opts->gen.max_address = 0x1000000;
	opts->gen.bus_cycles = BUS;
	opts->gen.clock_divider = clock_divider;
	opts->gen.mem_ptr_off = offsetof(m68k_context, mem_pointers);
	opts->gen.ram_flags_off = offsetof(m68k_context, ram_code_flags);
	opts->gen.ram_flags_shift = 11;
	for (int i = 0; i < 8; i++)
	{
		opts->dregs[i] = opts->aregs[i] = -1;
	}
#ifdef X86_64
	opts->dregs[0] = R10;
	opts->dregs[1] = R11;
	opts->dregs[2] = R12;
	opts->dregs[3] = R8;
	opts->aregs[0] = R13;
	opts->aregs[1] = R14;
	opts->aregs[2] = R9;
	opts->aregs[7] = R15;

	opts->flag_regs[0] = -1;
	opts->flag_regs[1] = RBX;
	opts->flag_regs[2] = RDX;
	opts->flag_regs[3] = BH;
	opts->flag_regs[4] = DH;

	opts->gen.scratch2 = RDI;
#else
	opts->dregs[0] = RDX;
	opts->aregs[7] = RDI;

	for (int i = 0; i < 5; i++)
	{
		opts->flag_regs[i] = -1;
	}
	opts->gen.scratch2 = RBX;
#endif
	opts->gen.context_reg = RSI;
	opts->gen.cycles = RAX;
	opts->gen.limit = RBP;
	opts->gen.scratch1 = RCX;


	opts->gen.native_code_map = malloc(sizeof(native_map_slot) * NATIVE_MAP_CHUNKS);
	memset(opts->gen.native_code_map, 0, sizeof(native_map_slot) * NATIVE_MAP_CHUNKS);
	opts->gen.deferred = NULL;
	opts->gen.ram_inst_sizes = malloc(sizeof(uint8_t *) * 64);
	memset(opts->gen.ram_inst_sizes, 0, sizeof(uint8_t *) * 64);

	code_info *code = &opts->gen.code;
	init_code_info(code);

	opts->gen.save_context = code->cur;
	for (int i = 0; i < 5; i++)
		if (opts->flag_regs[i] >= 0) {
			mov_rrdisp(code, opts->flag_regs[i], opts->gen.context_reg, offsetof(m68k_context, flags) + i, SZ_B);
		}
	for (int i = 0; i < 8; i++)
	{
		if (opts->dregs[i] >= 0) {
			mov_rrdisp(code, opts->dregs[i], opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t) * i, SZ_D);
		}
		if (opts->aregs[i] >= 0) {
			mov_rrdisp(code, opts->aregs[i], opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * i, SZ_D);
		}
	}
	mov_rrdisp(code, opts->gen.cycles, opts->gen.context_reg, offsetof(m68k_context, current_cycle), SZ_D);
	retn(code);

	opts->gen.load_context = code->cur;
	for (int i = 0; i < 5; i++)
		if (opts->flag_regs[i] >= 0) {
			mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, flags) + i, opts->flag_regs[i], SZ_B);
		}
	for (int i = 0; i < 8; i++)
	{
		if (opts->dregs[i] >= 0) {
			mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t) * i, opts->dregs[i], SZ_D);
		}
		if (opts->aregs[i] >= 0) {
			mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * i, opts->aregs[i], SZ_D);
		}
	}
	mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, current_cycle), opts->gen.cycles, SZ_D);
	mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, target_cycle), opts->gen.limit, SZ_D);
	retn(code);

	opts->start_context = (start_fun)code->cur;
	save_callee_save_regs(code);
#ifdef X86_64
	if (opts->gen.scratch2 != RDI) {
		mov_rr(code, RDI, opts->gen.scratch2, SZ_PTR);
	}
#else
	mov_rdispr(code, RSP, 20, opts->gen.scratch2, SZ_D);
	mov_rdispr(code, RSP, 24, opts->gen.context_reg, SZ_D);
#endif
	call(code, opts->gen.load_context);
	call_r(code, opts->gen.scratch2);
	call(code, opts->gen.save_context);
	restore_callee_save_regs(code);
	retn(code);

	opts->native_addr = code->cur;
	call(code, opts->gen.save_context);
	push_r(code, opts->gen.context_reg);
	call_args(code, (code_ptr)get_native_address_trans, 2, opts->gen.context_reg, opts->gen.scratch1);
	mov_rr(code, RAX, opts->gen.scratch1, SZ_PTR); //move result to scratch reg
	pop_r(code, opts->gen.context_reg);
	call(code, opts->gen.load_context);
	retn(code);

	opts->native_addr_and_sync = code->cur;
	call(code, opts->gen.save_context);
	push_r(code, opts->gen.scratch1);

	xor_rr(code, opts->gen.scratch1, opts->gen.scratch1, SZ_D);
	call_args_abi(code, (code_ptr)sync_components, 2, opts->gen.context_reg, opts->gen.scratch1);
	pop_r(code, RSI); //restore saved address from opts->gen.scratch1
	push_r(code, RAX); //save context pointer for later
	call_args(code, (code_ptr)get_native_address_trans, 2, RAX, RSI);
	mov_rr(code, RAX, opts->gen.scratch1, SZ_PTR); //move result to scratch reg
	pop_r(code, opts->gen.context_reg);
	call(code, opts->gen.load_context);
	retn(code);

	opts->gen.handle_cycle_limit = code->cur;
	cmp_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, sync_cycle), opts->gen.cycles, SZ_D);
	code_ptr skip_sync = code->cur + 1;
	jcc(code, CC_C, code->cur + 2);
	opts->do_sync = code->cur;
	push_r(code, opts->gen.scratch1);
	push_r(code, opts->gen.scratch2);
	call(code, opts->gen.save_context);
	xor_rr(code, opts->gen.scratch1, opts->gen.scratch1, SZ_D);
	call_args_abi(code, (code_ptr)sync_components, 2, opts->gen.context_reg, opts->gen.scratch1);
	mov_rr(code, RAX, opts->gen.context_reg, SZ_PTR);
	call(code, opts->gen.load_context);
	pop_r(code, opts->gen.scratch2);
	pop_r(code, opts->gen.scratch1);
	*skip_sync = code->cur - (skip_sync+1);
	retn(code);

	opts->gen.handle_code_write = (code_ptr)m68k_handle_code_write;

	opts->read_16 = gen_mem_fun(&opts->gen, memmap, num_chunks, READ_16, NULL);
	opts->read_8 = gen_mem_fun(&opts->gen, memmap, num_chunks, READ_8, NULL);
	opts->write_16 = gen_mem_fun(&opts->gen, memmap, num_chunks, WRITE_16, NULL);
	opts->write_8 = gen_mem_fun(&opts->gen, memmap, num_chunks, WRITE_8, NULL);

	opts->read_32 = code->cur;
	push_r(code, opts->gen.scratch1);
	call(code, opts->read_16);
	mov_rr(code, opts->gen.scratch1, opts->gen.scratch2, SZ_W);
	pop_r(code, opts->gen.scratch1);
	push_r(code, opts->gen.scratch2);
	add_ir(code, 2, opts->gen.scratch1, SZ_D);
	call(code, opts->read_16);
	pop_r(code, opts->gen.scratch2);
	movzx_rr(code, opts->gen.scratch1, opts->gen.scratch1, SZ_W, SZ_D);
	shl_ir(code, 16, opts->gen.scratch2, SZ_D);
	or_rr(code, opts->gen.scratch2, opts->gen.scratch1, SZ_D);
	retn(code);

	opts->write_32_lowfirst = code->cur;
	push_r(code, opts->gen.scratch2);
	push_r(code, opts->gen.scratch1);
	add_ir(code, 2, opts->gen.scratch2, SZ_D);
	call(code, opts->write_16);
	pop_r(code, opts->gen.scratch1);
	pop_r(code, opts->gen.scratch2);
	shr_ir(code, 16, opts->gen.scratch1, SZ_D);
	jmp(code, opts->write_16);

	opts->write_32_highfirst = code->cur;
	push_r(code, opts->gen.scratch1);
	push_r(code, opts->gen.scratch2);
	shr_ir(code, 16, opts->gen.scratch1, SZ_D);
	call(code, opts->write_16);
	pop_r(code, opts->gen.scratch2);
	pop_r(code, opts->gen.scratch1);
	add_ir(code, 2, opts->gen.scratch2, SZ_D);
	jmp(code, opts->write_16);

	opts->get_sr = code->cur;
	mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, status), opts->gen.scratch1, SZ_B);
	shl_ir(code, 8, opts->gen.scratch1, SZ_W);
	if (opts->flag_regs[FLAG_X] >= 0) {
		mov_rr(code, opts->flag_regs[FLAG_X], opts->gen.scratch1, SZ_B);
	} else {
		int8_t offset = offsetof(m68k_context, flags);
		if (offset) {
			mov_rdispr(code, opts->gen.context_reg, offset, opts->gen.scratch1, SZ_B);
		} else {
			mov_rindr(code, opts->gen.context_reg, opts->gen.scratch1, SZ_B);
		}
	}
	for (int flag = FLAG_N; flag <= FLAG_C; flag++)
	{
		shl_ir(code, 1, opts->gen.scratch1, SZ_B);
		if (opts->flag_regs[flag] >= 0) {
			or_rr(code, opts->flag_regs[flag], opts->gen.scratch1, SZ_B);
		} else {
			or_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, flags) + flag, opts->gen.scratch1, SZ_B);
		}
	}
	retn(code);

	opts->set_sr = code->cur;
	for (int flag = FLAG_C; flag >= FLAG_X; flag--)
	{
		rcr_ir(code, 1, opts->gen.scratch1, SZ_B);
		if (opts->flag_regs[flag] >= 0) {
			setcc_r(code, CC_C, opts->flag_regs[flag]);
		} else {
			int8_t offset = offsetof(m68k_context, flags) + flag;
			if (offset) {
				setcc_rdisp(code, CC_C, opts->gen.context_reg, offset);
			} else {
				setcc_rind(code, CC_C, opts->gen.context_reg);
			}
		}
	}
	shr_ir(code, 8, opts->gen.scratch1, SZ_W);
	mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
	retn(code);

	opts->set_ccr = code->cur;
	for (int flag = FLAG_C; flag >= FLAG_X; flag--)
	{
		rcr_ir(code, 1, opts->gen.scratch1, SZ_B);
		if (opts->flag_regs[flag] >= 0) {
			setcc_r(code, CC_C, opts->flag_regs[flag]);
		} else {
			int8_t offset = offsetof(m68k_context, flags) + flag;
			if (offset) {
				setcc_rdisp(code, CC_C, opts->gen.context_reg, offset);
			} else {
				setcc_rind(code, CC_C, opts->gen.context_reg);
			}
		}
	}
	retn(code);

	opts->gen.handle_cycle_limit_int = code->cur;
	cmp_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, int_cycle), opts->gen.cycles, SZ_D);
	code_ptr do_int = code->cur + 1;
	jcc(code, CC_NC, code->cur + 2);
	cmp_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, sync_cycle), opts->gen.cycles, SZ_D);
	skip_sync = code->cur + 1;
	jcc(code, CC_C, code->cur + 2);
	call(code, opts->gen.save_context);
	call_args_abi(code, (code_ptr)sync_components, 2, opts->gen.context_reg, opts->gen.scratch1);
	mov_rr(code, RAX, opts->gen.context_reg, SZ_PTR);
	jmp(code, opts->gen.load_context);
	*skip_sync = code->cur - (skip_sync+1);
	retn(code);
	*do_int = code->cur - (do_int+1);
	//set target cycle to sync cycle
	mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, sync_cycle), opts->gen.limit, SZ_D);
	//swap USP and SSP if not already in supervisor mode
	bt_irdisp(code, 5, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
	code_ptr already_supervisor = code->cur + 1;
	jcc(code, CC_C, code->cur + 2);
	swap_ssp_usp(opts);
	*already_supervisor = code->cur - (already_supervisor+1);
	//save PC
	subi_areg(opts, 4, 7);
	areg_to_native(opts, 7, opts->gen.scratch2);
	call(code, opts->write_32_lowfirst);
	//save status register
	subi_areg(opts, 2, 7);
	call(code, opts->get_sr);
	areg_to_native(opts, 7, opts->gen.scratch2);
	call(code, opts->write_16);
	//update status register
	and_irdisp(code, 0xF8, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
	mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, int_num), opts->gen.scratch1, SZ_B);
	or_ir(code, 0x20, opts->gen.scratch1, SZ_B);
	or_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
	//calculate interrupt vector address
	mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, int_num), opts->gen.scratch1, SZ_D);
	mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, int_ack), SZ_W);
	shl_ir(code, 2, opts->gen.scratch1, SZ_D);
	add_ir(code, 0x60, opts->gen.scratch1, SZ_D);
	call(code, opts->read_32);
	call(code, opts->native_addr_and_sync);
	cycles(&opts->gen, 24);
	//discard function return address
	pop_r(code, opts->gen.scratch2);
	jmp_r(code, opts->gen.scratch1);

	opts->trap = code->cur;
	push_r(code, opts->gen.scratch2);
	//swap USP and SSP if not already in supervisor mode
	bt_irdisp(code, 5, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
	already_supervisor = code->cur + 1;
	jcc(code, CC_C, code->cur + 2);
	swap_ssp_usp(opts);
	*already_supervisor = code->cur - (already_supervisor+1);
	//save PC
	subi_areg(opts, 4, 7);
	areg_to_native(opts, 7, opts->gen.scratch2);
	call(code, opts->write_32_lowfirst);
	//save status register
	subi_areg(opts, 2, 7);
	call(code, opts->get_sr);
	areg_to_native(opts, 7, opts->gen.scratch2);
	call(code, opts->write_16);
	//set supervisor bit
	or_irdisp(code, 0x20, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
	//calculate vector address
	pop_r(code, opts->gen.scratch1);
	shl_ir(code, 2, opts->gen.scratch1, SZ_D);
	call(code, opts->read_32);
	call(code, opts->native_addr_and_sync);
	cycles(&opts->gen, 18);
	jmp_r(code, opts->gen.scratch1);
}
