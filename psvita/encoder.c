#include <vitasdk.h>
#include <libk/stdio.h>

/*
 * This plugin uses a slight modified version of libjpeg-turbo 1.5.1:
 * It has been compiled with flag NO_GETENV and some jerror methods
 * had been made dummy in order to properly compile without newlib
 */
#include <jpeglib.h>
#include <setjmp.h>

#include "encoder.h"

#define ALIGN(x, a)	(((x) + ((a) - 1)) & ~((a) - 1))

// libjpeg stuffs
struct jpeg_error_mgr jerr;
struct jpeg_compress_struct cinfo;

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
	}else{
		jpeg_set_quality(&cinfo, 100 - ((100*video_quality) / 255), TRUE);
	}
	enc->quality = video_quality;
}

SceUID encoderInit(int width, int height, int pitch, encoder* enc, uint16_t video_quality, uint8_t enforce_sw, uint8_t enforce_fullres){
	enc->vram_usage = 1;
	if (width == 960 && height == 544){
		enc->in_size = ALIGN(261120, 0x10);
		enc->out_size = 557056;
	}else{
		enc->in_size = ALIGN((width*height)<<1, 0x10);
		enc->out_size = (pitch*height)<<2;
	}
	enc->rescale_buffer = NULL;
	uint32_t tempbuf_size = ALIGN(enc->in_size + enc->out_size,0x40000);
	if (enforce_sw) enc->gpublock = -1;
	else enc->gpublock = sceKernelAllocMemBlock("encoderBuffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, tempbuf_size, NULL);
	if (enc->gpublock < 0){ // Trying to use hw acceleration without VRAM
		enc->vram_usage = 0;
		enc->gpublock = sceKernelAllocMemBlock("encoderBuffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, tempbuf_size, NULL);
	}
	if (enc->gpublock < 0){ // Not enough vram, will use libjpeg-turbo
		if (width == 960 && height == 544 && (!enforce_fullres)){
			width = 480;
			height = 272;
			pitch = 512;
			enc->rescale_buffer = malloc(pitch*height<<2);
		}
		enc->isHwAccelerated = 0;
		enc->tempbuf_addr = malloc(enc->out_size);
		enc->out_size = tempbuf_size;
		cinfo.err = jpeg_std_error(&jerr);
		jpeg_create_compress(&cinfo);
		cinfo.image_width = width;
		enc->rowstride = pitch<<2;
		cinfo.image_height = height;
		cinfo.input_components = 4;
		cinfo.in_color_space = JCS_EXT_RGBA;
		jpeg_set_defaults(&cinfo);
		cinfo.dct_method = JDCT_FLOAT;
		jpeg_set_colorspace(&cinfo, JCS_YCbCr);
	}else{ // Will use sceJpegEnc
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
	}
	encoderSetQuality(enc, video_quality);
	return 0;
}

void encoderSetRescaler(encoder* enc, uint8_t use){
	if (use){
		if (enc->isHwAccelerated){
			enc->rescale_buffer = enc->tempbuf_addr + enc->in_size;
			sceJpegEncoderEnd(enc->context);
			sceJpegEncoderInit(enc->context, 480, 272, SCE_JPEGENC_PIXELFORMAT_YCBCR420 | SCE_JPEGENC_PIXELFORMAT_CSC_ARGB_YCBCR, enc->tempbuf_addr + enc->in_size, enc->out_size);
			sceJpegEncoderSetValidRegion(enc->context, 480, 272);
		}else{		
			encoderTerm(enc);
			encoderInit(960, 544, 1024, enc, 0xFFFF, 0, 0);
		}
	}else{
		if (enc->isHwAccelerated){
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
		}else{
			cinfo.image_width = 960;
			enc->rowstride = 4096;
			cinfo.image_height = 544;
			cinfo.input_components = 4;
			cinfo.in_color_space = JCS_EXT_RGBA;
			jpeg_set_defaults(&cinfo);
			cinfo.dct_method = JDCT_FLOAT;
			jpeg_set_colorspace(&cinfo, JCS_YCbCr);
			free(enc->rescale_buffer);
			enc->rescale_buffer = NULL;
		}
	}
}

void encoderTerm(encoder* enc){
	if (enc->isHwAccelerated){
		sceJpegEncoderEnd(enc->context);
		if (enc->gpublock >= 0) sceKernelFreeMemBlock(enc->gpublock);
		free(enc->context);
	}else{
		jpeg_destroy_compress(&cinfo);
		free(enc->tempbuf_addr);
		if (enc->rescale_buffer != NULL) free(enc->rescale_buffer);
	}
}

void* encodeARGB(encoder* enc, void* buffer, int pitch, int* outSize){
	if (enc->isHwAccelerated){
		sceJpegEncoderCsc(enc->context, enc->tempbuf_addr, buffer, pitch, SCE_JPEGENC_PIXELFORMAT_ARGB8888);
		*outSize = sceJpegEncoderEncode(enc->context, enc->tempbuf_addr);
		return enc->tempbuf_addr + enc->in_size;
	}else{
		unsigned char* outBuffer = (unsigned char*)enc->tempbuf_addr;
		long unsigned int out_size = enc->out_size;
		jpeg_mem_dest(&cinfo, &outBuffer, &out_size);
		jpeg_start_compress(&cinfo, TRUE);
		int y;
		for (y = 0; y < cinfo.image_height; y++){
			jpeg_write_scanlines(&cinfo, (JSAMPARRAY)&buffer, 1);
			buffer += enc->rowstride;
		}
		jpeg_finish_compress(&cinfo);
		*outSize = out_size;		
		return enc->tempbuf_addr;
	}
}