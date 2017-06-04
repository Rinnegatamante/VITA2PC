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

SceUID encoderInit(int width, int height, int pitch, encoder* enc, uint8_t video_quality){
	enc->in_size = ALIGN((width*height)<<1, 0x100);
	enc->out_size = ALIGN(width*height, 0x100);
	uint32_t tempbuf_size = ALIGN(enc->in_size + enc->out_size,0x40000);
	enc->memblocks[0] = sceKernelAllocMemBlock("encoderBuffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, tempbuf_size, NULL);
	if (enc->memblocks[0] < 0){ // Not enough vram, will use libjpeg-turbo
		enc->isHwAccelerated = 0;
		enc->tempbuf_addr = malloc(tempbuf_size);
		enc->out_size = tempbuf_size;
		cinfo.err = jpeg_std_error(&jerr);
		jpeg_create_compress(&cinfo);
		cinfo.image_width = pitch;
		enc->rowstride = pitch<<2;
		cinfo.image_height = height;
		cinfo.input_components = 4;
		cinfo.in_color_space = JCS_EXT_RGBA;
		jpeg_set_defaults(&cinfo);
		cinfo.dct_method = JDCT_FLOAT;
		jpeg_set_colorspace(&cinfo, JCS_YCbCr);
	}else{ // Will use sceJpegEnc
		enc->isHwAccelerated = 1;
		sceKernelGetMemBlockBase(enc->memblocks[0], &enc->tempbuf_addr);
		enc->memblocks[1] = sceKernelAllocMemBlock("contextBuffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, ALIGN(sceJpegEncoderGetContextSize(),0x40000), NULL);
		if (enc->memblocks[1] < 0){
			sceKernelFreeMemBlock(enc->memblocks[0]);
			return enc->memblocks[1];
		}
		sceKernelGetMemBlockBase(enc->memblocks[1], &enc->context);
		sceJpegEncoderInit(enc->context, width, height, PIXELFORMAT_YCBCR420 | PIXELFORMAT_CSC_ARGB_YCBCR, enc->tempbuf_addr + enc->in_size, enc->out_size);
		sceJpegEncoderSetValidRegion(enc->context, width, height);
	}
	encoderSetQuality(enc, video_quality);
	return 0;
}

void encoderTerm(encoder* enc){
	if (enc->isHwAccelerated){
		sceJpegEncoderEnd(enc->context);
		sceKernelFreeMemBlock(enc->memblocks[0]);
		sceKernelFreeMemBlock(enc->memblocks[1]);
	}else{
		jpeg_destroy_compress(&cinfo);
	}
}

void* encodeARGB(encoder* enc, void* buffer, int width, int height, int pitch, int* outSize){
	if (enc->isHwAccelerated){
		sceJpegEncoderCsc(enc->context, enc->tempbuf_addr, buffer, pitch, PIXELFORMAT_ARGB8888);
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