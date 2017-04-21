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
#include "encoder.h"

#define ALIGN(x, a)	(((x) + ((a) - 1)) & ~((a) - 1))

void encoderSetQuality(encoder* enc, uint8_t video_quality){
	sceJpegEncoderSetCompressionRatio(enc->context, video_quality);
	sceJpegEncoderSetOutputAddr(enc->context, enc->tempbuf_addr + enc->in_size, enc->out_size);
}

SceUID encoderInit(int width, int height, int pitch, encoder* enc, uint8_t video_quality){
	enc->in_size = ALIGN((width*height)<<1, 0x100);
	enc->out_size = ALIGN(width*height, 0x100);
	uint32_t tempbuf_size = ALIGN(enc->in_size + enc->out_size,0x40000);
	enc->memblocks[0] = sceKernelAllocMemBlock("encoderBuffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, tempbuf_size, NULL);
	if (enc->memblocks[0] < 0){ // Not enough vram, reporting an error
		return enc->memblocks[0];
	}else{ // Will use sceJpegEnc
		sceKernelGetMemBlockBase(enc->memblocks[0], &enc->tempbuf_addr);
		enc->memblocks[1] = sceKernelAllocMemBlock("contextBuffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, ALIGN(sceJpegEncoderGetContextSize(),0x40000), NULL);
		if (enc->memblocks[1] < 0){
			sceKernelFreeMemBlock(enc->memblocks[0]);
			return enc->memblocks[1];
		}
		sceKernelGetMemBlockBase(enc->memblocks[1], &enc->context);
		sceJpegEncoderInit(enc->context, width, height, PIXELFORMAT_YCBCR420 | PIXELFORMAT_CSC_ARGB_YCBCR, enc->tempbuf_addr + enc->in_size, enc->out_size);
		sceJpegEncoderSetValidRegion(enc->context, width, height);
		encoderSetQuality(enc, video_quality);
		return 0;
	}
}

void encoderTerm(encoder* enc){
	sceJpegEncoderEnd(enc->context);
	sceKernelFreeMemBlock(enc->memblocks[0]);
	sceKernelFreeMemBlock(enc->memblocks[1]);
}

void* encodeARGB(encoder* enc, void* buffer, int width, int height, int pitch, int* outSize){
	sceJpegEncoderCsc(enc->context, enc->tempbuf_addr, buffer, pitch, PIXELFORMAT_ARGB8888);
	*outSize = sceJpegEncoderEncode(enc->context, enc->tempbuf_addr);
	return enc->tempbuf_addr + enc->in_size;
}