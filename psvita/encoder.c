#include <vitasdk.h>
#include <libk/stdio.h>

#include "encoder.h"

#define ALIGN(x, a)	(((x) + ((a) - 1)) & ~((a) - 1))

int sceJpegEncoderInitAdvanced(SceJpegEncoderContext context, int inWidth, int inHeight, SceJpegEncoderPixelFormat pixelformat, void *outBuffer, SceSize outSize, uint8_t use_vram){
	SceJpegEncoderInitParam param;
	param.size = sizeof(SceJpegEncoderInitParam);
	param.pixelFormat = pixelformat;
	param.outBuffer = outBuffer;
	param.outSize = outSize;
	param.inWidth = inWidth;
	param.inHeight = inHeight;	
	if (use_vram) param.option = SCE_JPEGENC_INIT_PARAM_OPTION_NONE;
	else param.option = SCE_JPEGENC_INIT_PARAM_OPTION_LPDDR2_MEMORY;
	return sceJpegEncoderInitWithParam(context, &param);
}

void encoderSetQuality(encoder* enc, uint16_t video_quality){
	if (video_quality > 0xFF) video_quality = enc->quality;
	if (enc->isHwAccelerated){
		sceJpegEncoderSetCompressionRatio(enc->context, video_quality);
		sceJpegEncoderSetOutputAddr(enc->context, enc->tempbuf_addr + enc->in_size, enc->out_size);
	}
	enc->quality = video_quality;
}

SceUID encoderInit(int width, int height, int pitch, encoder* enc, uint16_t video_quality, uint8_t enforce_sw, uint8_t enforce_fullres){
	enc->vram_usage = 1;
	if (width == 960 && height == 544){
		enc->in_size = ALIGN(261120, 0x10);
		enc->out_size = 557056;
		enc->yuv_size = 261120;
	}else{
		enc->in_size = ALIGN((width*height)<<1, 0x10);
		enc->out_size = (pitch*height)<<2;
		enc->yuv_size = (width*height)<<1;
	}
	enc->rescale_buffer = NULL;
	uint32_t tempbuf_size = ALIGN(enc->in_size + enc->out_size,0x40000);
	if (enforce_sw) enc->gpublock = -1;
	else enc->gpublock = sceKernelAllocMemBlock("encoderBuffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, tempbuf_size, NULL);
	if (enc->gpublock < 0){ // Trying to use hw acceleration without VRAM
		enc->vram_usage = 0;
		enc->gpublock = sceKernelAllocMemBlock("encoderBuffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, tempbuf_size, NULL);
	}
	enc->isHwAccelerated = 1;
	sceKernelGetMemBlockBase(enc->gpublock, &enc->tempbuf_addr);
	enc->context = malloc(ALIGN(sceJpegEncoderGetContextSize(),0x40000));
	if (width == 960 && height == 544 && (!enforce_fullres)){ // Setup downscaler for better framerate
		enc->rescale_buffer = enc->tempbuf_addr + enc->in_size;
		sceJpegEncoderInitAdvanced(enc->context, 480, 272, SCE_JPEGENC_PIXELFORMAT_YCBCR420 | SCE_JPEGENC_PIXELFORMAT_CSC_ARGB_YCBCR, enc->tempbuf_addr + enc->in_size, enc->out_size, enc->vram_usage);
		sceJpegEncoderSetValidRegion(enc->context, 480, 272);
	}else{
		sceJpegEncoderInitAdvanced(enc->context, width, height, SCE_JPEGENC_PIXELFORMAT_YCBCR420 | SCE_JPEGENC_PIXELFORMAT_CSC_ARGB_YCBCR, enc->tempbuf_addr + enc->in_size, enc->out_size, enc->vram_usage);
		sceJpegEncoderSetValidRegion(enc->context, width, height);
	}
	encoderSetQuality(enc, video_quality);
	return 0;
}

void encoderSetRescaler(encoder* enc, uint8_t use){
	if (use){
		enc->rescale_buffer = enc->tempbuf_addr + enc->in_size;
		sceJpegEncoderEnd(enc->context);
		sceJpegEncoderInit(enc->context, 480, 272, SCE_JPEGENC_PIXELFORMAT_YCBCR420 | SCE_JPEGENC_PIXELFORMAT_CSC_ARGB_YCBCR, enc->tempbuf_addr + enc->in_size, enc->out_size);
		sceJpegEncoderSetValidRegion(enc->context, 480, 272);
	}else{
		sceKernelFreeMemBlock(enc->gpublock);
		enc->in_size = ALIGN(1044480, 0x10);
		enc->out_size = 2228224;
		uint32_t tempbuf_size = ALIGN(enc->in_size + enc->out_size,0x40000);
		enc->vram_usage = 1;
		enc->gpublock = sceKernelAllocMemBlock("encoderBuffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, tempbuf_size, NULL);
		if (enc->gpublock < 0){ // Trying to use hw acceleration without VRAM
			enc->vram_usage = 0;
			enc->gpublock = sceKernelAllocMemBlock("encoderBuffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, tempbuf_size, NULL);
		}
		if (enc->gpublock < 0){
			encoderTerm(enc);
			encoderInit(960, 544, 1024, enc, 0xFFFF, 1, 1);
			return;
		}
		sceJpegEncoderEnd(enc->context);
		sceKernelGetMemBlockBase(enc->gpublock, &enc->tempbuf_addr);
		sceJpegEncoderInitAdvanced(enc->context, 960, 544, SCE_JPEGENC_PIXELFORMAT_YCBCR420 | SCE_JPEGENC_PIXELFORMAT_CSC_ARGB_YCBCR, enc->tempbuf_addr + enc->in_size, enc->out_size, enc->vram_usage);
		sceJpegEncoderSetValidRegion(enc->context, 960, 544);
		enc->rescale_buffer = NULL;
	}
}

void encoderTerm(encoder* enc){
	sceJpegEncoderEnd(enc->context);
	if (enc->gpublock >= 0) sceKernelFreeMemBlock(enc->gpublock);
	free(enc->context);
}

void* encodeARGB(encoder* enc, void* buffer, int pitch){
	sceJpegEncoderCsc(enc->context, enc->tempbuf_addr, buffer, pitch, SCE_JPEGENC_PIXELFORMAT_ARGB8888);
	return enc->tempbuf_addr;
}