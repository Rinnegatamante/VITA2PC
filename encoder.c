/*
 * This File is Part Of : 
 *      ___                       ___           ___           ___           ___           ___                 
 *     /  /\        ___          /__/\         /  /\         /__/\         /  /\         /  /\          ___   
 *    /  /::\      /  /\         \  \:\       /  /:/         \  \:\       /  /:/_       /  /::\        /  /\  
 *   /  /:/\:\    /  /:/          \  \:\     /  /:/           \__\:\     /  /:/ /\     /  /:/\:\      /  /:/  
 *  /  /:/~/:/   /__/::\      _____\__\:\   /  /:/  ___   ___ /  /::\   /  /:/ /:/_   /  /:/~/::\    /  /:/   
 * /__/:/ /:/___ \__\/\:\__  /__/::::::::\ /__/:/  /  /\ /__/\  /:/\:\ /__/:/ /:/ /\ /__/:/ /:/\:\  /  /::\   
 * \  \:\/:::::/    \  \:\/\ \  \:\~~\~~\/ \  \:\ /  /:/ \  \:\/:/__\/ \  \:\/:/ /:/ \  \:\/:/__\/ /__/:/\:\  
 *  \  \::/~~~~      \__\::/  \  \:\  ~~~   \  \:\  /:/   \  \::/       \  \::/ /:/   \  \::/      \__\/  \:\ 
 *   \  \:\          /__/:/    \  \:\        \  \:\/:/     \  \:\        \  \:\/:/     \  \:\           \  \:\
 *    \  \:\         \__\/      \  \:\        \  \::/       \  \:\        \  \::/       \  \:\           \__\/
 *     \__\/                     \__\/         \__\/         \__\/         \__\/         \__\/                
 *
 * Copyright (c) Rinnegatamante <rinnegatamante@gmail.com>
 *
 */
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

void encoderSetQuality(encoder* enc, uint8_t video_quality){
	if (enc->isHwAccelerated){
		sceJpegEncoderSetCompressionRatio(enc->context, video_quality);
		sceJpegEncoderSetOutputAddr(enc->context, enc->tempbuf_addr + enc->in_size, enc->out_size);
	}else{
		jpeg_set_quality(&cinfo, 100 - ((100*video_quality) / 255), TRUE);
	}
}

SceUID encoderInit(int width, int height, int pitch, encoder* enc, uint8_t video_quality, uint8_t enforce_sw){
	enc->in_size = ALIGN((width*height)<<1, 0x10);
	enc->out_size = (pitch*height)<<2;
	enc->rescale_buffer = NULL;
	uint32_t tempbuf_size = ALIGN(enc->in_size + enc->out_size,0x40000);
	if (enforce_sw) enc->gpublock = -1;
	else enc->gpublock = sceKernelAllocMemBlock("encoderBuffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, tempbuf_size, NULL);
	if (enc->gpublock < 0){ // Not enough vram, will use libjpeg-turbo
		if (width == 960 && height == 544){
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
		if (width == 960 && height == 544){ // Setup downscaler for better framerate
			enc->rescale_buffer = enc->tempbuf_addr + enc->in_size;
			sceJpegEncoderInit(enc->context, 480, 272, SCE_JPEGENC_PIXELFORMAT_YCBCR420 | SCE_JPEGENC_PIXELFORMAT_CSC_ARGB_YCBCR, enc->tempbuf_addr + enc->in_size, enc->out_size);
			sceJpegEncoderSetValidRegion(enc->context, 480, 272);
		}else{
			sceJpegEncoderInit(enc->context, width, height, SCE_JPEGENC_PIXELFORMAT_YCBCR420 | SCE_JPEGENC_PIXELFORMAT_CSC_ARGB_YCBCR, enc->tempbuf_addr + enc->in_size, enc->out_size);
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
			cinfo.image_width = 480;
			enc->rowstride = 2048;
			cinfo.image_height = 272;
			cinfo.input_components = 4;
			cinfo.in_color_space = JCS_EXT_RGBA;
			jpeg_set_defaults(&cinfo);
			cinfo.dct_method = JDCT_FLOAT;
			jpeg_set_colorspace(&cinfo, JCS_YCbCr);
			enc->rescale_buffer = malloc(491520);
		}
	}else{
		if (enc->isHwAccelerated){
			enc->rescale_buffer = NULL;
			sceJpegEncoderEnd(enc->context);
			sceJpegEncoderInit(enc->context, 960, 544, SCE_JPEGENC_PIXELFORMAT_YCBCR420 | SCE_JPEGENC_PIXELFORMAT_CSC_ARGB_YCBCR, enc->tempbuf_addr + enc->in_size, enc->out_size);
			sceJpegEncoderSetValidRegion(enc->context, 960, 544);
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
		sceKernelFreeMemBlock(enc->gpublock);
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