#ifndef PSG_CONTEXT_H_
#define PSG_CONTEXT_H_

#include <stdint.h>

typedef struct {
	int16_t  *audio_buffer;
	int16_t  *back_buffer;
	double   buffer_fraction;
	double   buffer_inc;
	uint32_t buffer_pos;
	uint32_t clock_inc;
	uint32_t cycles;
	uint32_t samples_frame;
	uint16_t lsfr;
	uint16_t counter_load[4];
	uint16_t counters[4];
	uint8_t  volume[4];
	uint8_t  output_state[4];
	uint8_t  noise_out;
	uint8_t  noise_use_tone;
	uint8_t  noise_type;
	uint8_t  latch;
} psg_context;


void psg_init(psg_context * context, uint32_t sample_rate, uint32_t master_clock, uint32_t clock_div, uint32_t samples_frame);
void psg_write(psg_context * context, uint8_t value);
void psg_run(psg_context * context, uint32_t cycles);

#endif //PSG_CONTEXT_H_

