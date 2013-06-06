#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "ym2612.h"
#include "render.h"

//#define DO_DEBUG_PRINT
#ifdef DO_DEBUG_PRINT
#define dfprintf fprintf
#define dfopen(var, fname, mode) var=fopen(fname, mode)
#else
#define dfprintf
#define dfopen(var, fname, mode)
#endif

#define BUSY_CYCLES 17
#define OP_UPDATE_PERIOD 144

enum {
	REG_TIMERA_HIGH  = 0x24,
	REG_TIMERA_LOW,
	REG_TIMERB,
	REG_TIME_CTRL,
	REG_KEY_ONOFF,
	REG_DAC          = 0x2A,
	REG_DAC_ENABLE,

	REG_DETUNE_MULT  = 0x30,
	REG_TOTAL_LEVEL  = 0x40,
	REG_ATTACK_KS    = 0x50,
	REG_DECAY_AM     = 0x60,
	REG_SUSTAIN_RATE = 0x70,
	REG_S_LVL_R_RATE = 0x80,

	REG_FNUM_LOW     = 0xA0,
	REG_BLOCK_FNUM_H = 0xA4,
	REG_FNUM_LOW_CH3 = 0xA8,
	REG_BLOCK_FN_CH3 = 0xAC,
	REG_ALG_FEEDBACK = 0xB0,
	REG_LR_AMS_PMS   = 0xB4
};

#define BIT_TIMERA_ENABLE 0x1
#define BIT_TIMERB_ENABLE 0x2
#define BIT_TIMERA_OVEREN 0x4
#define BIT_TIMERB_OVEREN 0x8
#define BIT_TIMERA_RESET  0x10
#define BIT_TIMERB_RESET  0x20

#define BIT_STATUS_TIMERA 0x1
#define BIT_STATUS_TIMERB 0x2

enum {
	PHASE_ATTACK,
	PHASE_DECAY,
	PHASE_SUSTAIN,
	PHASE_RELEASE
};

uint8_t did_tbl_init = 0;
//According to Nemesis, real hardware only uses a 256 entry quarter sine table; however,
//memory is cheap so using a half sine table will probably save some cycles
//a full sine table would be nice, but negative numbers don't get along with log2
#define SINE_TABLE_SIZE 512
uint16_t sine_table[SINE_TABLE_SIZE];
//Similar deal here with the power table for log -> linear conversion
//According to Nemesis, real hardware only uses a 256 entry table for the fractional part
//and uses the whole part as a shift amount.
#define POW_TABLE_SIZE (1 << 13)
uint16_t pow_table[POW_TABLE_SIZE];

uint16_t rate_table_base[] = {
	//main portion
	0,1,0,1,0,1,0,1,
	0,1,0,1,1,1,0,1,
	0,1,1,1,0,1,1,1,
	0,1,1,1,1,1,1,1,
	//top end
	1,1,1,1,1,1,1,1,
	1,1,1,2,1,1,1,2,
	1,2,1,2,1,2,1,2,
	1,2,2,2,1,2,2,2,
};

uint16_t rate_table[64*8];

#define MAX_ENVELOPE 0xFFC
#define YM_DIVIDER 2
#define CYCLE_NEVER 0xFFFFFFFF

uint16_t round_fixed_point(double value, int dec_bits)
{
	return value * (1 << dec_bits) + 0.5;
}

FILE * debug_file = NULL;
uint32_t first_key_on=0;

void ym_init(ym2612_context * context, uint32_t sample_rate, uint32_t master_clock, uint32_t clock_div, uint32_t sample_limit)
{
	dfopen(debug_file, "ym_debug.txt", "w");
	memset(context, 0, sizeof(*context));
	context->audio_buffer = malloc(sizeof(*context->audio_buffer) * sample_limit*2);
	context->back_buffer = malloc(sizeof(*context->audio_buffer) * sample_limit*2);
	context->buffer_inc = ((double)sample_rate / (double)master_clock) * clock_div * 6;
	context->clock_inc = clock_div * 6;
	context->sample_limit = sample_limit*2;
	context->write_cycle = CYCLE_NEVER;
	for (int i = 0; i < NUM_OPERATORS; i++) {
		context->operators[i].envelope = MAX_ENVELOPE;
		context->operators[i].env_phase = PHASE_RELEASE;
	}
	//some games seem to expect that the LR flags start out as 1
	for (int i = 0; i < NUM_CHANNELS; i++) {
		context->channels[i].lr = 0xC0;
	}
	if (!did_tbl_init) {
		//populate sine table
		for (int32_t i = 0; i < 512; i++) {
			double sine = sin( ((double)(i*2+1) / SINE_TABLE_SIZE) * M_PI_2 );
			
			//table stores 4.8 fixed pointed representation of the base 2 log
			sine_table[i] = round_fixed_point(-log2(sine), 8);
		}
		//populate power table
		for (int32_t i = 0; i < POW_TABLE_SIZE; i++) {
			double linear = pow(2, -((double)((i & 0xFF)+1) / 256.0));
			int32_t tmp = round_fixed_point(linear, 11);
			int32_t shift = (i >> 8) - 2;
			if (shift < 0) {
				tmp <<= 0-shift;
			} else {
				tmp >>= shift;
			}
			pow_table[i] =  tmp;
		}
		//populate envelope generator rate table, from small base table
		for (int rate = 0; rate < 64; rate++) {
			for (int cycle = 0; cycle < 8; cycle++) {
				uint16_t value;
				if (rate < 2) {
					value = 0;
				} else if (rate >= 60) {
					value = 8;
				} else if (rate < 8) {
					value = rate_table_base[((rate & 6) == 6 ? 16 : 0) + cycle];
				} else if (rate < 48) {
					value = rate_table_base[(rate & 0x3) * 8 + cycle];
				} else {
					value = rate_table_base[32 + (rate & 0x3) * 8 + cycle] << ((rate - 48) >> 2);
				}
				rate_table[rate * 8 + cycle] = value;
			}
		}
	}
}

#define YM_VOLUME_DIVIDER 2
#define YM_MOD_SHIFT 1

void ym_run(ym2612_context * context, uint32_t to_cycle)
{
	//printf("Running YM2612 from cycle %d to cycle %d\n", context->current_cycle, to_cycle);
	//TODO: Fix channel update order OR remap channels in register write
	for (; context->current_cycle < to_cycle; context->current_cycle += context->clock_inc) {
		//Update timers at beginning of 144 cycle period
		if (!context->current_op && context->timer_control & BIT_TIMERA_ENABLE) {
			if (context->timer_a) {
				context->timer_a--;
			} else {
				if (context->timer_control & BIT_TIMERA_OVEREN) {
					context->status |= BIT_STATUS_TIMERA;
				}
				context->timer_a = context->timer_a_load;
			}
			if (context->timer_control & BIT_TIMERB_ENABLE) {
				uint32_t b_cyc = (context->current_cycle / OP_UPDATE_PERIOD) % 16;
				if (!b_cyc) {
					if (context->timer_b) {
						context->timer_b--;
					} else {
						if (context->timer_control & BIT_TIMERB_OVEREN) {
							context->status |= BIT_STATUS_TIMERB;
						}
						context->timer_b = context->timer_b_load;
					}
				}
			}
		}
		//Update Envelope Generator
		if (!(context->current_op % 3)) {
			uint32_t env_cyc = context->env_counter;
			uint32_t op = context->current_env_op;
			ym_operator * operator = context->operators + op;
			ym_channel * channel = context->channels + op/4;
			uint8_t rate;
			for(;;) {
				rate = operator->rates[operator->env_phase];
				if (rate) {
					uint8_t ks = channel->keycode >> operator->key_scaling;;
					rate = rate*2 + ks;
					if (rate > 63) {
						rate = 63;
					}
				}
				//Deal with "infinite" rates
				//According to Nemesis this should be handled in key-on instead
				if (rate >= 62 && operator->env_phase == PHASE_ATTACK) {
					operator->env_phase = PHASE_DECAY;
					operator->envelope = 0;
				} else {
					break;
				}
			}
			uint32_t cycle_shift = rate < 0x30 ? ((0x2F - rate) >> 2) : 0;
			if (first_key_on) {
				dfprintf(debug_file, "Operator: %d, env rate: %d (2*%d+%d), env_cyc: %d, cycle_shift: %d, env_cyc & ((1 << cycle_shift) - 1): %d\n", op, rate, operator->rates[operator->env_phase], channel->keycode >> operator->key_scaling,env_cyc, cycle_shift, env_cyc & ((1 << cycle_shift) - 1));
			}
			if (!(env_cyc & ((1 << cycle_shift) - 1))) {
				uint32_t update_cycle = env_cyc >> cycle_shift & 0x7;
				//envelope value is 10-bits, but it will be used as a 4.8 value
				uint16_t envelope_inc = rate_table[rate * 8 + update_cycle] << 2;
				if (operator->env_phase == PHASE_ATTACK) {
					//this can probably be optimized to a single shift rather than a multiply + shift
					if (first_key_on) {
						dfprintf(debug_file, "Changing op %d envelope %d by %d(%d * %d) in attack phase\n", op, operator->envelope, (~operator->envelope * envelope_inc) >> 4, ~operator->envelope, envelope_inc);
					}
					operator->envelope += (~operator->envelope * envelope_inc) >> 4;
					operator->envelope &= MAX_ENVELOPE;
					if (!operator->envelope) {
						operator->envelope = 0;
						operator->env_phase = PHASE_DECAY;
					}
				} else {
					if (first_key_on) {
						dfprintf(debug_file, "Changing op %d envelope %d by %d in %s phase\n", op, operator->envelope, envelope_inc, 
							operator->env_phase == PHASE_SUSTAIN ? "sustain" : (operator->env_phase == PHASE_DECAY ? "decay": "release"));
					}
					operator->envelope += envelope_inc;
					//clamp to max attenuation value
					if (operator->envelope > MAX_ENVELOPE) {
						operator->envelope = MAX_ENVELOPE;
					}
					if (operator->env_phase == PHASE_DECAY && operator->envelope >= operator->sustain_level) {
						//operator->envelope = operator->sustain_level;
						operator->env_phase = PHASE_SUSTAIN;
					}
				}
			}
			context->current_env_op++;
			if (context->current_env_op == NUM_OPERATORS) {
				context->current_env_op = 0;
				context->env_counter++;
			}
		}
		
		//Update Phase Generator
		uint32_t channel = context->current_op / 4;
		if (channel != 5 || !context->dac_enable) {
			uint32_t op = context->current_op;
			//printf("updating operator %d of channel %d\n", op, channel);
			ym_operator * operator = context->operators + op;
			ym_channel * chan = context->channels + channel;
			//TODO: Modulate phase by LFO if necessary
			operator->phase_counter += operator->phase_inc;
			uint16_t phase = operator->phase_counter >> 10 & 0x3FF;
			int16_t mod = 0;
			switch (op % 4)
			{
			case 0://Operator 1
				if (chan->feedback) {
					mod = operator->output >> (9-chan->feedback);
				}
				break;
			case 1://Operator 3
				switch(chan->algorithm)
				{
				case 0:
				case 2:
					//modulate by operator 2
					mod = context->operators[op+1].output >> YM_MOD_SHIFT;
					break;
				case 1:
					//modulate by operator 1+2
					mod = (context->operators[op-1].output + context->operators[op+1].output) >> YM_MOD_SHIFT;
					break;
				case 5:
					//modulate by operator 1
					mod = context->operators[op-1].output >> YM_MOD_SHIFT;
				}
				break;
			case 2://Operator 2
				if (chan->algorithm != 1 && chan->algorithm != 2 || chan->algorithm != 7) {
					//modulate by Operator 1
					mod = context->operators[op-2].output >> YM_MOD_SHIFT;
				}
				break;
			case 3://Operator 4
				switch(chan->algorithm)
				{
				case 0:
				case 1:
				case 4:
					//modulate by operator 3
					mod = context->operators[op-2].output >> YM_MOD_SHIFT;
					break;
				case 2:
					//modulate by operator 1+3
					mod = (context->operators[op-3].output + context->operators[op-2].output) >> YM_MOD_SHIFT;
					break;
				case 3:
					//modulate by operator 2+3
					mod = (context->operators[op-1].output + context->operators[op-2].output) >> YM_MOD_SHIFT;
					break;
				case 5:
					//modulate by operator 1
					mod = context->operators[op-3].output >> YM_MOD_SHIFT;
					break;
				}
				break;
			}
			uint16_t env = operator->envelope + operator->total_level;
			if (env > MAX_ENVELOPE) {
				env = MAX_ENVELOPE;
			}
			if (first_key_on) {
				dfprintf(debug_file, "op %d, base phase: %d, mod: %d, sine: %d, out: %d\n", op, phase, mod, sine_table[(phase+mod) & 0x1FF], pow_table[sine_table[phase & 0x1FF] + env]);
			}
			phase += mod;
			
			int16_t output = pow_table[sine_table[phase & 0x1FF] + env];
			if (phase & 0x200) {
				output = -output;
			}
			operator->output = output;
			//Update the channel output if we've updated all operators
			if (op % 4 == 3) {
				if (chan->algorithm < 4) {
					chan->output = operator->output;
				} else if(chan->algorithm == 4) {
					chan->output = operator->output + context->operators[channel * 4 + 1].output;
				} else {
					output = 0;
					for (uint32_t op = ((chan->algorithm == 7) ? 0 : 1) + channel*4; op < (channel+1)*4; op++) {
						output += context->operators[op].output;
					}
					chan->output = output;
				}
				if (first_key_on) {
					int16_t value = context->channels[channel].output & 0x3FE0;
					if (value & 0x2000) {
						value |= 0xC000;
					}
					dfprintf(debug_file, "channel %d output: %d\n", channel, value / YM_VOLUME_DIVIDER);
				}
			}
			//puts("operator update done");
		}
		context->current_op++;
		context->buffer_fraction += context->buffer_inc;
		if (context->buffer_fraction > 1.0) {
			context->buffer_fraction -= 1.0;
			context->audio_buffer[context->buffer_pos] = 0;
			context->audio_buffer[context->buffer_pos + 1] = 0;
			for (int i = 0; i < NUM_CHANNELS; i++) {
				int16_t value = context->channels[i].output & 0x3FE0;
				if (value & 0x2000) {
					value |= 0xC000;
				}
				if (context->channels[i].lr & 0x80) {
					context->audio_buffer[context->buffer_pos] += value / YM_VOLUME_DIVIDER;
				}
				if (context->channels[i].lr & 0x40) {
					context->audio_buffer[context->buffer_pos+1] += value / YM_VOLUME_DIVIDER;
				}
			}
			context->buffer_pos += 2;
			if (context->buffer_pos == context->sample_limit) {
				render_wait_ym(context);
			}
		}
		if (context->current_op == NUM_OPERATORS) {
			context->current_op = 0;
		}
	}
	if (context->current_cycle >= context->write_cycle + (BUSY_CYCLES * context->clock_inc / 6)) {
		context->status &= 0x7F;
		context->write_cycle = CYCLE_NEVER;
	}
	//printf("Done running YM2612 at cycle %d\n", context->current_cycle, to_cycle);
}

void ym_address_write_part1(ym2612_context * context, uint8_t address)
{
	//printf("address_write_part1: %X\n", address);
	context->selected_reg = address;
	context->selected_part = 0;
}

void ym_address_write_part2(ym2612_context * context, uint8_t address)
{
	//printf("address_write_part2: %X\n", address);
	context->selected_reg = address;
	context->selected_part = 1;
}

uint8_t fnum_to_keycode[] = {
	//F11 = 0
	0,0,0,0,0,0,0,1,
	//F11 = 1
	2,3,3,3,3,3,3,3
};

//table courtesy of Nemesis
uint32_t detune_table[][4] = {
	{0, 0, 1, 2},   //0  (0x00)
    {0, 0, 1, 2},   //1  (0x01)
    {0, 0, 1, 2},   //2  (0x02)
    {0, 0, 1, 2},   //3  (0x03)
    {0, 1, 2, 2},   //4  (0x04)
    {0, 1, 2, 3},   //5  (0x05)
    {0, 1, 2, 3},   //6  (0x06)
    {0, 1, 2, 3},   //7  (0x07)
    {0, 1, 2, 4},   //8  (0x08)
    {0, 1, 3, 4},   //9  (0x09)
    {0, 1, 3, 4},   //10 (0x0A)
    {0, 1, 3, 5},   //11 (0x0B)
    {0, 2, 4, 5},   //12 (0x0C)
    {0, 2, 4, 6},   //13 (0x0D)
    {0, 2, 4, 6},   //14 (0x0E)
    {0, 2, 5, 7},   //15 (0x0F)
    {0, 2, 5, 8},   //16 (0x10)
    {0, 3, 6, 8},   //17 (0x11)
    {0, 3, 6, 9},   //18 (0x12)
    {0, 3, 7,10},   //19 (0x13)
    {0, 4, 8,11},   //20 (0x14)
    {0, 4, 8,12},   //21 (0x15)
    {0, 4, 9,13},   //22 (0x16)
    {0, 5,10,14},   //23 (0x17)
    {0, 5,11,16},   //24 (0x18)
    {0, 6,12,17},   //25 (0x19)
    {0, 6,13,19},   //26 (0x1A)
    {0, 7,14,20},   //27 (0x1B)
    {0, 8,16,22},   //28 (0x1C)
    {0, 8,16,22},   //29 (0x1D)
    {0, 8,16,22},   //30 (0x1E)
    {0, 8,16,22}
};  //31 (0x1F)

void ym_update_phase_inc(ym2612_context * context, ym_operator * operator, uint32_t op)
{
	uint32_t chan_num = op / 4;
	//printf("ym_update_phase_inc | channel: %d, op: %d\n", chan_num, op);
	//base frequency
	ym_channel * channel = context->channels + chan_num;
	uint32_t inc, detune;
	if (chan_num == 2 && context->ch3_mode && (op < (2*4 + 3))) {
		inc = context->ch3_supp[op-2*4].fnum;
		if (!context->ch3_supp[op-2*4].block) {
			inc >>= 1;
		} else {
			inc <<= (context->ch3_supp[op-2*4].block-1);
		}
		//detune
		detune = detune_table[context->ch3_supp[op-2*4].keycode][operator->detune & 0x3];
	} else {
		inc = channel->fnum;
		if (!channel->block) {
			inc >>= 1;
		} else {
			inc <<= (channel->block-1);
		}
		//detune
		detune = detune_table[channel->keycode][operator->detune & 0x3];
	} 
	if (operator->detune & 0x40) {
		inc -= detune;
		//this can underflow, mask to 17-bit result
		inc &= 0x1FFFF;
	} else {
		inc += detune;
	}
	//multiple
	if (operator->multiple) {
		inc *= operator->multiple;
	} else {
		//0.5
		inc >>= 1;
	}
	//printf("phase_inc for operator %d: %d, block: %d, fnum: %d, detune: %d, multiple: %d\n", op, inc, channel->block, channel->fnum, detune, operator->multiple);
	operator->phase_inc = inc;
}

void ym_data_write(ym2612_context * context, uint8_t value)
{
	if (context->selected_reg < 0x21 || context->selected_reg > 0xB6 || (context->selected_reg < 0x30 && context->selected_part)) {
		return;
	}
	dfprintf(debug_file, "write of %X to reg %X in part %d\n", value, context->selected_reg, context->selected_part+1);
	if (context->selected_reg < 0x30) {
		//Shared regs
		switch (context->selected_reg)
		{
		//TODO: Test reg and LFO
		case REG_TIMERA_HIGH:
			context->timer_a_load &= 0x3;
			context->timer_a_load |= value << 2;
			break;
		case REG_TIMERA_LOW:
			context->timer_a_load &= 0xFFFC;
			context->timer_a_load |= value & 0x3;
			break;
		case REG_TIMERB:
			context->timer_b_load = value;
			break;
		case REG_TIME_CTRL: {
			context->timer_control = value & 0x3F;
			uint8_t old_mode = context->ch3_mode;
			context->ch3_mode = value & 0xC0;
			if (context->ch3_mode != old_mode) {
				ym_update_phase_inc(context, context->operators + 2*4, 2*4);
				ym_update_phase_inc(context, context->operators + 2*4+1, 2*4+1);
				ym_update_phase_inc(context, context->operators + 2*4+2, 2*4+2);
			}
			break;
		}
		case REG_KEY_ONOFF: {
			uint8_t channel = value & 0x7;
			if (channel != 3 && channel != 7) {
				if (channel > 2) {
					channel--;
				}
				for (uint8_t op = channel * 4, bit = 0x10; op < (channel + 1) * 4; op++, bit <<= 1) {
					if (value & bit) {
						first_key_on = 1;
						//printf("Key On for operator %d in channel %d\n", op, channel);
						context->operators[op].phase_counter = 0;
						context->operators[op].env_phase = PHASE_ATTACK;
						context->operators[op].envelope = MAX_ENVELOPE;
					} else {
						//printf("Key Off for operator %d in channel %d\n", op, channel);
						context->operators[op].env_phase = PHASE_RELEASE;
					}
				}
			}
			break;
		}
		case REG_DAC:
			if (context->dac_enable) {
				context->channels[5].output = (((int16_t)value) - 0x80) << 6;
				//printf("DAC Write %X(%d)\n", value, context->channels[5].output);
			}
			break;
		case REG_DAC_ENABLE:
			//printf("DAC Enable: %X\n", value);
			context->dac_enable = value & 0x80;
			break;
		}
	} else if (context->selected_reg < 0xA0) {
		//part
		uint8_t op = context->selected_part ? (NUM_OPERATORS/2) : 0;
		//channel in part
		if ((context->selected_reg & 0x3) != 0x3) {
			op += 4 * (context->selected_reg & 0x3) + ((context->selected_reg & 0xC) / 4);
			//printf("write targets operator %d (%d of channel %d)\n", op, op % 4, op / 4);
			ym_operator * operator = context->operators + op;
			switch (context->selected_reg & 0xF0)
			{
			case REG_DETUNE_MULT:
				operator->detune = value >> 4 & 0x7;
				operator->multiple = value & 0xF;
				ym_update_phase_inc(context, operator, op);
				break;
			case REG_TOTAL_LEVEL:
				operator->total_level = (value & 0x7F) << 5;
				break;
			case REG_ATTACK_KS:
				operator->key_scaling = 3 - (value >> 6);
				operator->rates[PHASE_ATTACK] = value & 0x1F;
				break;
			case REG_DECAY_AM:
				//TODO: AM flag for LFO
				operator->rates[PHASE_DECAY] = value & 0x1F;
				break;
			case REG_SUSTAIN_RATE:
				operator->rates[PHASE_SUSTAIN] = value & 0x1F;
				break;
			case REG_S_LVL_R_RATE:
				operator->rates[PHASE_RELEASE] = (value & 0xF) << 1 | 1;
				operator->sustain_level = (value & 0xF0) << 4;
				break;
			}
		}
	} else {
		uint8_t channel = context->selected_reg & 0x3;
		if (channel != 3) {
			if (context->selected_part) {
				channel += 3;
			}
			//printf("write targets channel %d\n", channel);
			switch (context->selected_reg & 0xFC)
			{
			case REG_FNUM_LOW:
				context->channels[channel].block = context->channels[channel].block_fnum_latch >> 3 & 0x7;
				context->channels[channel].fnum = (context->channels[channel].block_fnum_latch & 0x7) << 8 | value;
				context->channels[channel].keycode = context->channels[channel].block << 2 | fnum_to_keycode[context->channels[channel].fnum >> 7];
				ym_update_phase_inc(context, context->operators + channel*4, channel*4);
				ym_update_phase_inc(context, context->operators + channel*4+1, channel*4+1);
				ym_update_phase_inc(context, context->operators + channel*4+2, channel*4+2);
				ym_update_phase_inc(context, context->operators + channel*4+3, channel*4+3);
				break;
			case REG_BLOCK_FNUM_H:{
				context->channels[channel].block_fnum_latch = value;
				break;
			}
			case REG_FNUM_LOW_CH3:
				if (channel < 3) {
					context->ch3_supp[channel].block = context->ch3_supp[channel].block_fnum_latch >> 3 & 0x7;
					context->ch3_supp[channel].fnum = (context->ch3_supp[channel].block_fnum_latch & 0x7) << 8 | value;
					context->ch3_supp[channel].keycode = context->ch3_supp[channel].block << 2 | fnum_to_keycode[context->ch3_supp[channel].fnum >> 7];
					if (context->ch3_mode) {
						ym_update_phase_inc(context, context->operators + 2*4 + channel, 2*4);
					}
				}
				break;
			case REG_BLOCK_FN_CH3:
				if (channel < 3) {
					context->ch3_supp[channel].block_fnum_latch = value;
				}
				break;
			case REG_ALG_FEEDBACK:
				context->channels[channel].algorithm = value & 0x7;
				context->channels[channel].feedback = value >> 3 & 0x7;
				break;
			case REG_LR_AMS_PMS:
				context->channels[channel].pms = value & 0x7;
				context->channels[channel].ams = value >> 4 & 0x3;
				context->channels[channel].lr = value & 0xC0;
				//printf("Write of %X to LR_AMS_PMS reg for channel %d\n", value, channel);
				break;
			}
		}
	}
	
	context->write_cycle = context->current_cycle;
	context->status |= 0x80;
}

uint8_t ym_read_status(ym2612_context * context)
{
	return context->status;
}

