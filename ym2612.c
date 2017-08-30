/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "ym2612.h"
#include "render.h"
#include "wave.h"
#include "blastem.h"

//#define DO_DEBUG_PRINT
#ifdef DO_DEBUG_PRINT
#define dfprintf fprintf
#define dfopen(var, fname, mode) var=fopen(fname, mode)
#else
#define dfprintf
#define dfopen(var, fname, mode)
#endif

#define BUSY_CYCLES_ADDRESS 17
#define BUSY_CYCLES_DATA_LOW 83
#define BUSY_CYCLES_DATA_HIGH 47
#define OP_UPDATE_PERIOD 144

#define BIT_TIMERA_ENABLE 0x1
#define BIT_TIMERB_ENABLE 0x2
#define BIT_TIMERA_OVEREN 0x4
#define BIT_TIMERB_OVEREN 0x8
#define BIT_TIMERA_RESET  0x10
#define BIT_TIMERB_RESET  0x20

#define BIT_TIMERA_LOAD   0x40
#define BIT_TIMERB_LOAD   0x80

#define BIT_STATUS_TIMERA 0x1
#define BIT_STATUS_TIMERB 0x2

static uint32_t ym_calc_phase_inc(ym2612_context * context, ym_operator * operator, uint32_t op);

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
static uint16_t sine_table[SINE_TABLE_SIZE];
//Similar deal here with the power table for log -> linear conversion
//According to Nemesis, real hardware only uses a 256 entry table for the fractional part
//and uses the whole part as a shift amount.
#define POW_TABLE_SIZE (1 << 13)
static uint16_t pow_table[POW_TABLE_SIZE];

static uint16_t rate_table_base[] = {
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

static uint16_t rate_table[64*8];

static uint8_t lfo_timer_values[] = {108, 77, 71, 67, 62, 44, 8, 5};
static uint8_t lfo_pm_base[][8] = {
	{0,   0,   0,   0,   0,   0,   0,   0},
	{0,   0,   0,   0,   4,   4,   4,   4},
	{0,   0,   0,   4,   4,   4,   8,   8},
	{0,   0,   4,   4,   8,   8, 0xc, 0xc},
	{0,   0,   4,   8,   8,   8, 0xc,0x10},
	{0,   0,   8, 0xc,0x10,0x10,0x14,0x18},
	{0,   0,0x10,0x18,0x20,0x20,0x28,0x30},
	{0,   0,0x20,0x30,0x40,0x40,0x50,0x60}
};
static int16_t lfo_pm_table[128 * 32 * 8];

int16_t ams_shift[] = {8, 1, -1, -2};

#define MAX_ENVELOPE 0xFFC
#define YM_DIVIDER 2
#define CYCLE_NEVER 0xFFFFFFFF

static uint16_t round_fixed_point(double value, int dec_bits)
{
	return value * (1 << dec_bits) + 0.5;
}

static FILE * debug_file = NULL;
static uint32_t first_key_on=0;

static ym2612_context * log_context = NULL;

static void ym_finalize_log()
{
	if (!log_context) {
		return;
	}
	for (int i = 0; i < NUM_CHANNELS; i++) {
		if (log_context->channels[i].logfile) {
			wave_finalize(log_context->channels[i].logfile);
		}
	}
	log_context = NULL;
}
#define BUFFER_INC_RES 0x40000000UL

void ym_adjust_master_clock(ym2612_context * context, uint32_t master_clock)
{
	uint64_t old_inc = context->buffer_inc;
	context->buffer_inc = ((BUFFER_INC_RES * (uint64_t)context->sample_rate) / (uint64_t)master_clock) * (uint64_t)context->clock_inc * NUM_OPERATORS;
}

#ifdef __ANDROID__
#define log2(x) (log(x)/log(2))
#endif


#define TIMER_A_MAX 1023
#define TIMER_B_MAX 255

void ym_reset(ym2612_context *context)
{
	memset(context->part1_regs, 0, sizeof(context->part1_regs));
	memset(context->part2_regs, 0, sizeof(context->part2_regs));
	memset(context->operators, 0, sizeof(context->operators));
	memset(context->channels, 0, sizeof(context->channels));
	memset(context->ch3_supp, 0, sizeof(context->ch3_supp));
	context->selected_reg = 0;
	context->csm_keyon = 0;
	context->ch3_mode = 0;
	context->dac_enable = 0;
	context->status = 0;
	context->timer_a_load = 0;
	context->timer_b_load = 0;
	//TODO: Confirm these on hardware
	context->timer_a = TIMER_A_MAX;
	context->timer_b = TIMER_B_MAX;
	
	//TODO: Reset LFO state
	
	//some games seem to expect that the LR flags start out as 1
	for (int i = 0; i < NUM_CHANNELS; i++) {
		context->channels[i].lr = 0xC0;
	}
	context->write_cycle = CYCLE_NEVER;
	for (int i = 0; i < NUM_OPERATORS; i++) {
		context->operators[i].envelope = MAX_ENVELOPE;
		context->operators[i].env_phase = PHASE_RELEASE;
	}
}

void ym_init(ym2612_context * context, uint32_t sample_rate, uint32_t master_clock, uint32_t clock_div, uint32_t sample_limit, uint32_t options, uint32_t lowpass_cutoff)
{
	static uint8_t registered_finalize;
	dfopen(debug_file, "ym_debug.txt", "w");
	memset(context, 0, sizeof(*context));
	context->audio_buffer = malloc(sizeof(*context->audio_buffer) * sample_limit*2);
	context->back_buffer = malloc(sizeof(*context->audio_buffer) * sample_limit*2);
	context->sample_rate = sample_rate;
	context->clock_inc = clock_div * 6;
	ym_adjust_master_clock(context, master_clock);
	
	double rc = (1.0 / (double)lowpass_cutoff) / (2.0 * M_PI);
	double dt = 1.0 / ((double)master_clock / (double)(context->clock_inc * NUM_OPERATORS));
	double alpha = dt / (dt + rc);
	context->lowpass_alpha = (int32_t)(((double)0x10000) * alpha);

	context->sample_limit = sample_limit*2;
	
	//some games seem to expect that the LR flags start out as 1
	for (int i = 0; i < NUM_CHANNELS; i++) {
		if (options & YM_OPT_WAVE_LOG) {
			char fname[64];
			sprintf(fname, "ym_channel_%d.wav", i);
			FILE * f = context->channels[i].logfile = fopen(fname, "wb");
			if (!f) {
				fprintf(stderr, "Failed to open WAVE log file %s for writing\n", fname);
				continue;
			}
			if (!wave_init(f, sample_rate, 16, 1)) {
				fclose(f);
				context->channels[i].logfile = NULL;
			}
		}
	}
	if (options & YM_OPT_WAVE_LOG) {
		log_context = context;
		if (!registered_finalize) {
			atexit(ym_finalize_log);
			registered_finalize = 1;
		}
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
		//populate LFO PM table from small base table
		//seems like there must be a better way to derive this
		for (int freq = 0; freq < 128; freq++) {
			for (int pms = 0; pms < 8; pms++) {
				for (int step = 0; step < 32; step++) {
					int16_t value = 0;
					for (int bit = 0x40, shift = 0; bit > 0; bit >>= 1, shift++) {
						if (freq & bit) {
							value += lfo_pm_base[pms][(step & 0x8) ? 7-step & 7 : step & 7] >> shift;
						}
					}
					if (step & 0x10) {
						value = -value;
					}
					lfo_pm_table[freq * 256 + pms * 32 + step] = value;
				}
			}
		}
	}
	ym_reset(context);
}

void ym_free(ym2612_context *context)
{
	if (context == log_context) {
		ym_finalize_log();
	}
	free(context->audio_buffer);
	//TODO: Figure out how to make this 100% safe
	//audio thread could still be using this
	free(context->back_buffer);
	free(context);
}

#define YM_VOLUME_MULTIPLIER 2
#define YM_VOLUME_DIVIDER 3
#define YM_MOD_SHIFT 1

#define CSM_MODE 0x80

#define SSG_ENABLE    8
#define SSG_INVERT    4
#define SSG_ALTERNATE 2
#define SSG_HOLD      1

#define SSG_CENTER 0x800

static void start_envelope(ym_operator *op, ym_channel *channel)
{
	//Deal with "infinite" attack rates
	uint8_t rate = op->rates[PHASE_ATTACK];
	if (rate) {
		uint8_t ks = channel->keycode >> op->key_scaling;;
		rate = rate*2 + ks;
	}
	if (rate >= 62) {
		op->env_phase = PHASE_DECAY;
		op->envelope = 0;
	} else {
		op->env_phase = PHASE_ATTACK;
	}
}

static void keyon(ym_operator *op, ym_channel *channel)
{
	start_envelope(op, channel);
	op->phase_counter = 0;
	op->inverted = op->ssg & SSG_INVERT;
}

static const uint8_t keyon_bits[] = {0x10, 0x40, 0x20, 0x80};

static void keyoff(ym_operator *op)
{
	op->env_phase = PHASE_RELEASE;
	if (op->inverted) {
		//Nemesis says the inversion state doesn't change here, but I don't see how that is observable either way
		op->inverted = 0;
		op->envelope = (SSG_CENTER - op->envelope) & MAX_ENVELOPE;
	}
}

static void csm_keyoff(ym2612_context *context)
{
	context->csm_keyon = 0;
	uint8_t changes = 0xF0 ^ context->channels[2].keyon;
	for (uint8_t op = 2*4, bit = 0; op < 3*4; op++, bit++)
	{
		if (changes & keyon_bits[bit]) {
			keyoff(context->operators + op);
		}
	}
}

void ym_run(ym2612_context * context, uint32_t to_cycle)
{
	//printf("Running YM2612 from cycle %d to cycle %d\n", context->current_cycle, to_cycle);
	//TODO: Fix channel update order OR remap channels in register write
	for (; context->current_cycle < to_cycle; context->current_cycle += context->clock_inc) {
		//Update timers at beginning of 144 cycle period
		if (!context->current_op) {
			if (context->timer_control & BIT_TIMERA_ENABLE) {
				if (context->timer_a != TIMER_A_MAX) {
					context->timer_a++;
					if (context->csm_keyon) {
						csm_keyoff(context);
					}
				} else {
					if (context->timer_control & BIT_TIMERA_LOAD) {
						context->timer_control &= ~BIT_TIMERA_LOAD;
					} else if (context->timer_control & BIT_TIMERA_OVEREN) {
						context->status |= BIT_STATUS_TIMERA;
					}
					context->timer_a = context->timer_a_load;
					if (!context->csm_keyon && context->ch3_mode == CSM_MODE) {
						context->csm_keyon = 0xF0;
						uint8_t changes = 0xF0 ^ context->channels[2].keyon;;
						for (uint8_t op = 2*4, bit = 0; op < 3*4; op++, bit++)
						{
							if (changes & keyon_bits[bit]) {
								keyon(context->operators + op, context->channels + 2);
							}
						}
					}
				}
			}
			if (!context->sub_timer_b) {
				if (context->timer_control & BIT_TIMERB_ENABLE) {
					if (context->timer_b != TIMER_B_MAX) {
						context->timer_b++;
					} else {
						if (context->timer_control & BIT_TIMERB_LOAD) {
							context->timer_control &= ~BIT_TIMERB_LOAD;
						} else if (context->timer_control & BIT_TIMERB_OVEREN) {
							context->status |= BIT_STATUS_TIMERB;
						}
						context->timer_b = context->timer_b_load;
					}
				}
			}
			context->sub_timer_b += 0x10;
			//Update LFO
			if (context->lfo_enable) {
				if (context->lfo_counter) {
					context->lfo_counter--;
				} else {
					context->lfo_counter = lfo_timer_values[context->lfo_freq];
					context->lfo_am_step += 2;
					context->lfo_am_step &= 0xFE;
					context->lfo_pm_step = context->lfo_am_step / 8;
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
			if (operator->env_phase == PHASE_DECAY && operator->envelope >= operator->sustain_level) {
				//operator->envelope = operator->sustain_level;
				operator->env_phase = PHASE_SUSTAIN;
			}
			rate = operator->rates[operator->env_phase];
			if (rate) {
				uint8_t ks = channel->keycode >> operator->key_scaling;;
				rate = rate*2 + ks;
				if (rate > 63) {
					rate = 63;
				}
			}
			uint32_t cycle_shift = rate < 0x30 ? ((0x2F - rate) >> 2) : 0;
			if (first_key_on) {
				dfprintf(debug_file, "Operator: %d, env rate: %d (2*%d+%d), env_cyc: %d, cycle_shift: %d, env_cyc & ((1 << cycle_shift) - 1): %d\n", op, rate, operator->rates[operator->env_phase], channel->keycode >> operator->key_scaling,env_cyc, cycle_shift, env_cyc & ((1 << cycle_shift) - 1));
			}
			if (!(env_cyc & ((1 << cycle_shift) - 1))) {
				uint32_t update_cycle = env_cyc >> cycle_shift & 0x7;
				uint16_t envelope_inc = rate_table[rate * 8 + update_cycle];
				if (operator->env_phase == PHASE_ATTACK) {
					//this can probably be optimized to a single shift rather than a multiply + shift
					if (first_key_on) {
						dfprintf(debug_file, "Changing op %d envelope %d by %d(%d * %d) in attack phase\n", op, operator->envelope, (~operator->envelope * envelope_inc) >> 4, ~operator->envelope, envelope_inc);
					}
					uint16_t old_env = operator->envelope;
					operator->envelope += ((~operator->envelope * envelope_inc) >> 4) & 0xFFFFFFFC;
					if (operator->envelope > old_env) {
						//Handle overflow
						operator->envelope = 0;
					}
					if (!operator->envelope) {
						operator->env_phase = PHASE_DECAY;
					}
				} else {
					if (first_key_on) {
						dfprintf(debug_file, "Changing op %d envelope %d by %d in %s phase\n", op, operator->envelope, envelope_inc,
							operator->env_phase == PHASE_SUSTAIN ? "sustain" : (operator->env_phase == PHASE_DECAY ? "decay": "release"));
					}
					if (operator->ssg) {
						if (operator->envelope < SSG_CENTER) {
							envelope_inc *= 4;
						} else {
							envelope_inc = 0;
						}
					}
					//envelope value is 10-bits, but it will be used as a 4.8 value
					operator->envelope += envelope_inc << 2;
					//clamp to max attenuation value
					if (
						operator->envelope > MAX_ENVELOPE 
						|| (operator->env_phase == PHASE_RELEASE && operator->envelope >= SSG_CENTER)
					) {
						operator->envelope = MAX_ENVELOPE;
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
			uint16_t phase = operator->phase_counter >> 10 & 0x3FF;
			operator->phase_counter += ym_calc_phase_inc(context, operator, context->current_op);
			int16_t mod = 0;
			switch (op % 4)
			{
			case 0://Operator 1
				if (chan->feedback) {
					mod = (chan->op1_old + operator->output) >> (10-chan->feedback);
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
				if (chan->algorithm != 1 && chan->algorithm != 2 && chan->algorithm != 7) {
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
			uint16_t env = operator->envelope;
			if (operator->ssg) {
				if (env >= SSG_CENTER) {
					if (operator->ssg & SSG_ALTERNATE) {
						if (operator->env_phase != PHASE_RELEASE && (
							!(operator->ssg & SSG_HOLD) || ((operator->ssg ^ operator->inverted) & SSG_INVERT) == 0
						)) {
							operator->inverted ^= SSG_INVERT;
						}
					} else if (!(operator->ssg & SSG_HOLD)) {
						phase = operator->phase_counter = 0;
					}
					if (
						(operator->env_phase == PHASE_DECAY || operator->env_phase == PHASE_SUSTAIN) 
						&& !(operator->ssg & SSG_HOLD)
					) {
						start_envelope(operator, chan);
						env = operator->envelope;
					}
				}
				if (operator->inverted) {
					env = (SSG_CENTER - env) & MAX_ENVELOPE;
				}
			}
			env += operator->total_level;
			if (operator->am) {
				uint16_t base_am = (context->lfo_am_step & 0x80 ? context->lfo_am_step : ~context->lfo_am_step) & 0x7E;
				if (ams_shift[chan->ams] >= 0) {
					env += base_am >> ams_shift[chan->ams];
				} else {
					env += base_am << (-ams_shift[chan->ams]);
				}
			}
			if (env > MAX_ENVELOPE) {
				env = MAX_ENVELOPE;
			}
			if (first_key_on) {
				dfprintf(debug_file, "op %d, base phase: %d, mod: %d, sine: %d, out: %d\n", op, phase, mod, sine_table[(phase+mod) & 0x1FF], pow_table[sine_table[phase & 0x1FF] + env]);
			}
			//if ((channel != 0 && channel != 4) || chan->algorithm != 5) {
				phase += mod;
			//}

			int16_t output = pow_table[sine_table[phase & 0x1FF] + env];
			if (phase & 0x200) {
				output = -output;
			}
			if (op % 4 == 0) {
				chan->op1_old = operator->output;
			}
			operator->output = output;
			//Update the channel output if we've updated all operators
			if (op % 4 == 3) {
				if (chan->algorithm < 4) {
					chan->output = operator->output;
				} else if(chan->algorithm == 4) {
					chan->output = operator->output + context->operators[channel * 4 + 2].output;
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
					dfprintf(debug_file, "channel %d output: %d\n", channel, (value * YM_VOLUME_MULTIPLIER) / YM_VOLUME_DIVIDER);
				}
			}
			//puts("operator update done");
		}
		context->current_op++;
		if (context->current_op == NUM_OPERATORS) {
			context->current_op = 0;
			
			context->buffer_fraction += context->buffer_inc;
			int16_t left = 0, right = 0;
			for (int i = 0; i < NUM_CHANNELS; i++) {
				int16_t value = context->channels[i].output;
				if (value > 0x1FE0) {
					value = 0x1FE0;
				} else if (value < -0x1FF0) {
					value = -0x1FF0;
				} else {
					value &= 0x3FE0;
					if (value & 0x2000) {
						value |= 0xC000;
					}
				}
				if (context->channels[i].logfile && context->buffer_fraction > BUFFER_INC_RES) {
					fwrite(&value, sizeof(value), 1, context->channels[i].logfile);
				}
				if (context->channels[i].lr & 0x80) {
					left += (value * YM_VOLUME_MULTIPLIER) / YM_VOLUME_DIVIDER;
				}
				if (context->channels[i].lr & 0x40) {
					right += (value * YM_VOLUME_MULTIPLIER) / YM_VOLUME_DIVIDER;
				}
			}
			int32_t tmp = left * context->lowpass_alpha + context->last_left * (0x10000 - context->lowpass_alpha);
			left = tmp >> 16;
			tmp = right * context->lowpass_alpha + context->last_right * (0x10000 - context->lowpass_alpha);
			right = tmp >> 16;
			while (context->buffer_fraction > BUFFER_INC_RES) {
				context->buffer_fraction -= BUFFER_INC_RES;

				int64_t tmp = context->last_left * ((context->buffer_fraction << 16) / context->buffer_inc);
				tmp += left * (0x10000 - ((context->buffer_fraction << 16) / context->buffer_inc));
				context->audio_buffer[context->buffer_pos] = tmp >> 16;
				
				tmp = context->last_right * ((context->buffer_fraction << 16) / context->buffer_inc);
				tmp += right * (0x10000 - ((context->buffer_fraction << 16) / context->buffer_inc));
				context->audio_buffer[context->buffer_pos+1] = tmp >> 16;
				
				context->buffer_pos += 2;
				if (context->buffer_pos == context->sample_limit) {
					if (!headless) {
						render_wait_ym(context);
					}
				}
			}
			context->last_left = left;
			context->last_right = right;
		}
		
	}
	if (context->current_cycle >= context->write_cycle + (context->busy_cycles * context->clock_inc / 6)) {
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
	context->write_cycle = context->current_cycle;
	context->busy_cycles = BUSY_CYCLES_ADDRESS;
	context->status |= 0x80;
}

void ym_address_write_part2(ym2612_context * context, uint8_t address)
{
	//printf("address_write_part2: %X\n", address);
	context->selected_reg = address;
	context->selected_part = 1;
	context->write_cycle = context->current_cycle;
	context->busy_cycles = BUSY_CYCLES_ADDRESS;
	context->status |= 0x80;
}

static uint8_t fnum_to_keycode[] = {
	//F11 = 0
	0,0,0,0,0,0,0,1,
	//F11 = 1
	2,3,3,3,3,3,3,3
};

//table courtesy of Nemesis
static uint32_t detune_table[][4] = {
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

static uint32_t ym_calc_phase_inc(ym2612_context * context, ym_operator * operator, uint32_t op)
{
	uint32_t chan_num = op / 4;
	//printf("ym_update_phase_inc | channel: %d, op: %d\n", chan_num, op);
	//base frequency
	ym_channel * channel = context->channels + chan_num;
	uint32_t inc, detune;
	if (chan_num == 2 && context->ch3_mode && (op < (2*4 + 3))) {
		//supplemental fnum registers are in a different order than normal slot paramters
		int index = op-2*4;
		if (index < 2) {
			index ^= 1;
		}
		inc = context->ch3_supp[index].fnum;
		if (channel->pms) {
			inc = inc * 2 + lfo_pm_table[(inc & 0x7F0) * 16 + channel->pms + context->lfo_pm_step];
		}
		if (!context->ch3_supp[index].block) {
			inc >>= 1;
		} else {
			inc <<= (context->ch3_supp[index].block-1);
		}
		//detune
		detune = detune_table[context->ch3_supp[index].keycode][operator->detune & 0x3];
	} else {
		inc = channel->fnum;
		if (channel->pms) {
			inc = inc * 2 + lfo_pm_table[(inc & 0x7F0) * 16 + channel->pms + context->lfo_pm_step];
		}
		if (!channel->block) {
			inc >>= 1;
		} else {
			inc <<= (channel->block-1);
		}
		//detune
		detune = detune_table[channel->keycode][operator->detune & 0x3];
	}
	if (channel->pms) {
		inc >>= 1;
	}
	if (operator->detune & 0x4) {
		inc -= detune;
		//this can underflow, mask to 17-bit result
		inc &= 0x1FFFF;
	} else {
		inc += detune;
	}
	//multiple
	if (operator->multiple) {
		inc *= operator->multiple;
		inc &= 0xFFFFF;
	} else {
		//0.5
		inc >>= 1;
	}
	//printf("phase_inc for operator %d: %d, block: %d, fnum: %d, detune: %d, multiple: %d\n", op, inc, channel->block, channel->fnum, detune, operator->multiple);
	return inc;
}

void ym_data_write(ym2612_context * context, uint8_t value)
{
	if (context->selected_reg >= YM_REG_END) {
		return;
	}
	if (context->selected_part) {
		if (context->selected_reg < YM_PART2_START) {
			return;
		}
		context->part2_regs[context->selected_reg - YM_PART2_START] = value;
	} else {
		if (context->selected_reg < YM_PART1_START) {
			return;
		}
		context->part1_regs[context->selected_reg - YM_PART1_START] = value;
	}
	dfprintf(debug_file, "write of %X to reg %X in part %d\n", value, context->selected_reg, context->selected_part+1);
	if (context->selected_reg < 0x30) {
		//Shared regs
		switch (context->selected_reg)
		{
		//TODO: Test reg
		case REG_LFO:
			/*if ((value & 0x8) && !context->lfo_enable) {
				printf("LFO Enabled, Freq: %d\n", value & 0x7);
			}*/
			context->lfo_enable = value & 0x8;
			if (!context->lfo_enable) {
				context->lfo_am_step = context->lfo_pm_step = 0;
			}
			context->lfo_freq = value & 0x7;

			break;
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
			if (value & BIT_TIMERA_ENABLE && !(context->timer_control & BIT_TIMERA_ENABLE)) {
				context->timer_a = TIMER_A_MAX;
				context->timer_control |= BIT_TIMERA_LOAD;
			}
			if (value & BIT_TIMERB_ENABLE && !(context->timer_control & BIT_TIMERB_ENABLE)) {
				context->timer_b = TIMER_B_MAX;
				context->timer_control |= BIT_TIMERB_LOAD;
			}
			context->timer_control &= (BIT_TIMERA_LOAD | BIT_TIMERB_LOAD);
			context->timer_control |= value & 0xF;
			if (value & BIT_TIMERA_RESET) {
				context->status &= ~BIT_STATUS_TIMERA;
			}
			if (value & BIT_TIMERB_RESET) {
				context->status &= ~BIT_STATUS_TIMERB;
			}
			if (context->ch3_mode == CSM_MODE && (value & 0xC0) != CSM_MODE && context->csm_keyon) {
				csm_keyoff(context);
			}
			context->ch3_mode = value & 0xC0;
			break;
		}
		case REG_KEY_ONOFF: {
			uint8_t channel = value & 0x7;
			if (channel != 3 && channel != 7) {
				if (channel > 2) {
					channel--;
				}
				uint8_t changes = channel == 2 
					? (value | context->csm_keyon) ^  (context->channels[channel].keyon | context->csm_keyon)
					: value ^ context->channels[channel].keyon;
				context->channels[channel].keyon = value & 0xF0;
				for (uint8_t op = channel * 4, bit = 0; op < (channel + 1) * 4; op++, bit++) {
					if (changes & keyon_bits[bit]) {
						if (value & keyon_bits[bit]) {
							first_key_on = 1;
							//printf("Key On for operator %d in channel %d\n", op, channel);
							keyon(context->operators + op, context->channels + channel);
						} else {
							//printf("Key Off for operator %d in channel %d\n", op, channel);
							keyoff(context->operators + op);
						}
					}
				}
			}
			break;
		}
		case REG_DAC:
			if (context->dac_enable) {
				context->channels[5].output = (((int16_t)value) - 0x80) << 6;
				//printf("DAC Write %X(%d) @ %d\n", value, context->channels[5].output, context->current_cycle);
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
				operator->am = value & 0x80;
				operator->rates[PHASE_DECAY] = value & 0x1F;
				break;
			case REG_SUSTAIN_RATE:
				operator->rates[PHASE_SUSTAIN] = value & 0x1F;
				break;
			case REG_S_LVL_R_RATE:
				operator->rates[PHASE_RELEASE] = (value & 0xF) << 1 | 1;
				operator->sustain_level = (value & 0xF0) << 3;
				if (operator->sustain_level == 0x780) {
					operator->sustain_level = MAX_ENVELOPE;
				}
				break;
			case REG_SSG_EG:
				if (!(value & SSG_ENABLE)) {
					value = 0;
				}
				if ((value ^ operator->ssg) & SSG_INVERT) {
					operator->inverted ^= SSG_INVERT;
				}
				operator->ssg = value;
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
				//printf("Algorithm %d, feedback %d for channel %d\n", value & 0x7, value >> 3 & 0x7, channel);
				break;
			case REG_LR_AMS_PMS:
				context->channels[channel].pms = (value & 0x7) * 32;
				context->channels[channel].ams = value >> 4 & 0x3;
				context->channels[channel].lr = value & 0xC0;
				//printf("Write of %X to LR_AMS_PMS reg for channel %d\n", value, channel);
				break;
			}
		}
	}

	context->write_cycle = context->current_cycle;
	context->busy_cycles = context->selected_reg < 0xA0 ? BUSY_CYCLES_DATA_LOW : BUSY_CYCLES_DATA_HIGH;
	context->status |= 0x80;
}

uint8_t ym_read_status(ym2612_context * context)
{
	return context->status;
}

void ym_print_channel_info(ym2612_context *context, int channel)
{
	ym_channel *chan = context->channels + channel;
	printf("\n***Channel %d***\n"
	       "Algorithm: %d\n"
		   "Feedback:  %d\n"
		   "Pan:       %s\n"
		   "AMS:       %d\n"
		   "PMS:       %d\n",
		   channel+1, chan->algorithm, chan->feedback,
		   chan->lr == 0xC0 ? "LR" : chan->lr == 0x80 ? "L" : chan->lr == 0x40 ? "R" : "",
		   chan->ams, chan->pms);
	if (channel == 2) {
		printf(
		   "Mode:      %X: %s\n",
		   context->ch3_mode, context->ch3_mode ? "special" : "normal");
	}
	for (int operator = channel * 4; operator < channel * 4+4; operator++)
	{
		int dispnum = operator - channel * 4 + 1;
		if (dispnum == 2) {
			dispnum = 3;
		} else if (dispnum == 3) {
			dispnum = 2;
		}
		ym_operator *op = context->operators + operator;
		printf("\nOperator %d:\n"
		       "    Multiple:      %d\n"
			   "    Detune:        %d\n"
			   "    Total Level:   %d\n"
			   "    Attack Rate:   %d\n"
			   "    Key Scaling:   %d\n"
			   "    Decay Rate:    %d\n"
			   "    Sustain Level: %d\n"
			   "    Sustain Rate:  %d\n"
			   "    Release Rate:  %d\n"
			   "    Amplitude Modulation %s\n",
			   dispnum, op->multiple, op->detune, op->total_level,
			   op->rates[PHASE_ATTACK], op->key_scaling, op->rates[PHASE_DECAY],
			   op->sustain_level, op->rates[PHASE_SUSTAIN], op->rates[PHASE_RELEASE],
			   op->am ? "On" : "Off");
	}
}

void ym_print_timer_info(ym2612_context *context)
{
	printf("***Timer A***\n"
	       "Current Value: %d\n"
		   "Load Value:    %d\n"
		   "Triggered:     %s\n"
		   "Enabled:       %s\n\n",
		   context->timer_a,
		   context->timer_a_load,
		   context->status & BIT_STATUS_TIMERA ? "yes" : "no",
		   context->timer_control & BIT_TIMERA_ENABLE ? "yes" : "no");
	printf("***Timer B***\n"
	       "Current Value: %d\n"
		   "Load Value:    %d\n"
		   "Triggered:     %s\n"
		   "Enabled:       %s\n\n",
		   context->timer_b,
		   context->timer_b_load,
		   context->status & BIT_STATUS_TIMERB ? "yes" : "no",
		   context->timer_control & BIT_TIMERB_ENABLE ? "yes" : "no");
}

void ym_serialize(ym2612_context *context, serialize_buffer *buf)
{
	save_buffer8(buf, context->part1_regs, YM_PART1_REGS);
	save_buffer8(buf, context->part2_regs, YM_PART2_REGS);
	for (int i = 0; i < NUM_OPERATORS; i++)
	{
		save_int32(buf, context->operators[i].phase_counter);
		save_int16(buf, context->operators[i].envelope);
		save_int16(buf, context->operators[i].output);
		save_int8(buf, context->operators[i].env_phase);
		save_int8(buf, context->operators[i].inverted);
	}
	for (int i = 0; i < NUM_CHANNELS; i++)
	{
		save_int16(buf, context->channels[i].output);
		save_int16(buf, context->channels[i].op1_old);
		//Due to the latching behavior, these need to be saved
		//even though duplicate info is probably in the regs array
		save_int8(buf, context->channels[i].block);
		save_int16(buf, context->channels[i].fnum);
		save_int8(buf, context->channels[i].keyon);
	}
	for (int i = 0; i < 3; i++)
	{
		//Due to the latching behavior, these need to be saved
		//even though duplicate info is probably in the regs array
		save_int8(buf, context->ch3_supp[i].block);
		save_int8(buf, context->ch3_supp[i].fnum);
	}
	save_int8(buf, context->timer_control);
	save_int16(buf, context->timer_a);
	save_int8(buf, context->timer_b);
	save_int8(buf, context->sub_timer_b);
	save_int16(buf, context->env_counter);
	save_int8(buf, context->current_op);
	save_int8(buf, context->current_env_op);
	save_int8(buf, context->lfo_counter);
	save_int8(buf, context->csm_keyon);
	save_int8(buf, context->status);
	save_int8(buf, context->selected_reg);
	save_int8(buf, context->selected_part);
	save_int32(buf, context->current_cycle);
	save_int32(buf, context->write_cycle);
	save_int32(buf, context->busy_cycles);
}

void ym_deserialize(deserialize_buffer *buf, void *vcontext)
{
	ym2612_context *context = vcontext;
	uint8_t temp_regs[YM_PART1_REGS];
	load_buffer8(buf, temp_regs, YM_PART1_REGS);
	context->selected_part = 0;
	for (int i = 0; i < YM_PART1_REGS; i++)
	{
		uint8_t reg = YM_PART1_START + i;
		if (reg == REG_TIME_CTRL) {
			context->ch3_mode = temp_regs[i] & 0xC0;
		} else if (reg != REG_FNUM_LOW && reg != REG_KEY_ONOFF) {
			context->selected_reg = reg;
			ym_data_write(context, temp_regs[i]);
		}
	}
	load_buffer8(buf, temp_regs, YM_PART2_REGS);
	context->selected_part = 1;
	for (int i = 0; i < YM_PART2_REGS; i++)
	{
		uint8_t reg = YM_PART2_START + i;
		if (reg != REG_FNUM_LOW) {
			context->selected_reg = reg;
			ym_data_write(context, temp_regs[i]);
		}
	}
	for (int i = 0; i < NUM_OPERATORS; i++)
	{
		context->operators[i].phase_counter = load_int32(buf);
		context->operators[i].envelope = load_int16(buf);
		context->operators[i].output = load_int16(buf);
		context->operators[i].env_phase = load_int8(buf);
		if (context->operators[i].env_phase > PHASE_RELEASE) {
			context->operators[i].env_phase = PHASE_RELEASE;
		}
		context->operators[i].inverted = load_int8(buf) != 0 ? SSG_INVERT : 0;
	}
	for (int i = 0; i < NUM_CHANNELS; i++)
	{
		context->channels[i].output = load_int16(buf);
		context->channels[i].op1_old = load_int16(buf);
		context->channels[i].block = load_int8(buf);
		context->channels[i].fnum = load_int16(buf);
		context->channels[i].keycode = context->channels[i].block << 2 | fnum_to_keycode[context->channels[i].fnum >> 7];
		context->channels[i].keyon = load_int8(buf);
	}
	for (int i = 0; i < 3; i++)
	{
		context->ch3_supp[i].block = load_int8(buf);
		context->ch3_supp[i].fnum = load_int8(buf);
		context->ch3_supp[i].keycode = context->ch3_supp[i].block << 2 | fnum_to_keycode[context->ch3_supp[i].fnum >> 7];
	}
	context->timer_control = load_int8(buf);
	context->timer_a = load_int16(buf);
	context->timer_b = load_int8(buf);
	context->sub_timer_b = load_int8(buf);
	context->env_counter = load_int16(buf);
	context->current_op = load_int8(buf);
	if (context->current_op >= NUM_OPERATORS) {
		context->current_op = 0;
	}
	context->current_env_op = load_int8(buf);
	if (context->current_env_op >= NUM_OPERATORS) {
		context->current_env_op = 0;
	}
	context->lfo_counter = load_int8(buf);
	context->csm_keyon = load_int8(buf);
	context->status = load_int8(buf);
	context->selected_reg = load_int8(buf);
	context->selected_part = load_int8(buf);
	context->current_cycle = load_int32(buf);
	context->write_cycle = load_int32(buf);
	context->busy_cycles = load_int32(buf);
}
