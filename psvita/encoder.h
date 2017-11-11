#ifndef _ENCODER_H_
#define _ENCODER_H_

#include <libk/stdlib.h>
#include <psp2/jpegenc.h>

typedef struct encoder{
	uint8_t isHwAccelerated;
	uint8_t vram_usage;
	SceUID gpublock;
	void* tempbuf_addr;
	void* rescale_buffer;
	uint32_t in_size;
	uint32_t out_size;
	uint8_t quality;
	SceJpegEncoderContext context; // used only by sceJpegEnc
	uint32_t rowstride;            // Used only by libjpeg-turbo
}encoder;

SceUID encoderInit(int width, int height, int pitch, encoder* enc, uint16_t video_quality, uint8_t enforce_sw, uint8_t enforce_fullres);
void encoderTerm(encoder* enc);
void* encodeARGB(encoder* enc, void* buffer, int pitch, int* outSize);
void encoderSetQuality(encoder* enc, uint16_t video_quality);
void encoderSetRescaler(encoder* enc, uint8_t use);

#endif