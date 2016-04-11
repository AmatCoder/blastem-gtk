#ifndef VGM_H_
#define VGM_H_

#pragma pack(push, 1)
typedef struct {
	char     ident[4];
	uint32_t eof_offset;
	uint32_t version;
	uint32_t sn76489_clk;
	uint32_t ym2413_clk;
	uint32_t gd3_offset;
	uint32_t num_samples;
	uint32_t loop_offset;
	uint32_t loop_samples;
	uint32_t rate;
	uint16_t sn76489_fb;
	uint8_t  sn76489_shift;
	uint8_t  sn76489_flags;
	uint32_t ym2612_clk;
	uint32_t ym2151_clk;
	uint32_t data_offset;
	uint32_t sega_pcm_clk;
	uint32_t sega_pcm_reg;
} vgm_header;

enum {
	CMD_PSG_STEREO = 0x4F,
	CMD_PSG,
	CMD_YM2413,
	CMD_YM2612_0,
	CMD_YM2612_1,
	CMD_YM2151,
	CMD_YM2203,
	CMD_YM2608_0,
	CMD_YM2608_1,
	CMD_YM2610_0,
	CMD_YM2610_1,
	CMD_YM3812,
	CMD_YM3526,
	CMD_Y8950,
	CMD_YMZ280B,
	CMD_YMF262_0,
	CMD_YMF262_1,
	CMD_WAIT = 0x61,
	CMD_WAIT_60,
	CMD_WAIT_50,
	CMD_END = 0x66,
	CMD_DATA,
	CMD_PCM_WRITE,
	CMD_WAIT_SHORT = 0x70,
	CMD_YM2612_DAC = 0x80,
	CMD_DAC_STREAM_SETUP = 0x90,
	CMD_DAC_STREAM_DATA,
	CMD_DAC_STREAM_FREQ,
	CMD_DAC_STREAM_START,
	CMD_DAC_STREAM_STOP,
	CMD_DAC_STREAM_STARTFAST,
	CMD_DATA_SEEK = 0xE0
};

enum {
	DATA_YM2612_PCM = 0
};

#pragma pack(pop)

typedef struct {
	struct data_block *next;
	uint8_t           *data;
	uint32_t          size;
	uint8_t           type;
} data_block;

#endif //VGM_H_
