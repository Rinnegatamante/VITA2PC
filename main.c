#include <vitasdk.h>
#include <taihen.h>
#include <taipool.h>
#include <libk/stdlib.h>
#include <libk/stdio.h>
#include "renderer.h"
#include "encoder.h"

#define HOOKS_NUM       7
#define MENU_ENTRIES    7
#define QUALITY_ENTRIES 5

#define STREAM_PORT    5000    // Port used for screen streaming
#define STREAM_BUFSIZE 0x80000 // Size of stream buffer
#define NET_SIZE       0x90000 // Size of net module buffer

#define NOT_TRIGGERED   0
#define CONFIG_MENU     1
#define LISTENING       2
#define SYNC_BROADCAST  3
#define ASYNC_BROADCAST 4

static SceNetSockaddrIn addrTo, addrFrom;
static SceUID g_hooks[HOOKS_NUM], stream_thread_id, async_mutex;
static tai_hook_ref_t ref[HOOKS_NUM];
static uint8_t cur_hook = 0;
static SceUID isEncoderUnavailable = 0;
static SceUID isNetAvailable = 1;
static SceUID firstBoot = 1;
static uint8_t video_quality = 255;
static encoder jpeg_encoder;
static char vita_ip[32];
static uint64_t vita_addr;
static uint8_t status = NOT_TRIGGERED;
static int stream_skt = -1;
static int loopDrawing = 0;
static uint32_t old_buttons;
static int cfg_i = 0;
static int qual_i = 2;
static uint8_t* mem;
static char* qualities[] = {"Best", "High", "Default", "Low", "Worst"};
static uint8_t qual_val[] = {0, 64, 128, 192, 255};
static uint8_t frameskip = 0;
static uint8_t stream_type = 1;
static char* menu[] = {"Video Quality: ", "Video Codec: MJPEG", "Hardware Acceleration: ", "Downscaler: ","Frame Skip: ", "Stream Type: ", "Start Screen Streaming"};
static uint32_t* rescale_buffer = NULL;
static uint8_t enforce_sw = 0;
static uint8_t skip_net_init = 0;
static uint32_t mempool_size = 0x500000;
static char titleid[16];

// Config Menu Renderer
void drawConfigMenu(){
	int i;
	for (i = 0; i < MENU_ENTRIES; i++){
		(i == cfg_i) ? setTextColor(0x0000FF00) : setTextColor(0x00FFFFFF);
		switch (i){
			case 0:
				drawStringF(5, 70 + i*20, "%s%s", menu[i], qualities[qual_i]);
				break;
			case 2:
				drawStringF(5, 70 + i*20, "%s%s", menu[i], jpeg_encoder.isHwAccelerated ? "Enabled" : "Disabled");
				break;
			case 3:
				drawStringF(5, 70 + i*20, "%s%s", menu[i], (jpeg_encoder.rescale_buffer != NULL) ? "Enabled" : "Disabled");
				break;
			case 4:
				drawStringF(5, 70 + i*20, "%s%u", menu[i], frameskip);
				break;
			case 5:
				drawStringF(5, 70 + i*20, "%s%s", menu[i], stream_type ? "Asynchronous" : "Synchronous");
				break;
			default:
				drawString(5, 70 + i*20, menu[i]);
				break;
		}
	}
	setTextColor(0x00FFFFFF);
}

// Generic hooking function
void hookFunction(uint32_t nid, const void* func){
	g_hooks[cur_hook] = taiHookFunctionImport(&ref[cur_hook], TAI_MAIN_MODULE, TAI_ANY_LIBRARY, nid, func);
	cur_hook++;
}

// CPU downscaling function
void rescaleBuffer(uint32_t* src, uint32_t* dst, int width, int height, int pitch){
	int i,j,z,k,ptr;
	z=0;
	k=0;
	for (i=0;i < height; i+=2){
		ptr = pitch * i;
		z = 512 * (k++);
		for (j=0;j < width; j+=2){
			dst[z++]=src[ptr+j];
		}
	}
}

// Asynchronous streaming thread
int stream_thread(SceSize args, void *argp){
	int mem_size;
	SceDisplayFrameBuf param;
	param.size = sizeof(SceDisplayFrameBuf);
	sceKernelWaitSema(async_mutex, 1, NULL);
	for (;;){
		sceDisplayGetFrameBuf(&param, SCE_DISPLAY_SETBUF_NEXTFRAME);
		if (rescale_buffer != NULL){ // Downscaler available
			rescaleBuffer((uint32_t*)param.base, rescale_buffer, param.width, param.height, param.pitch);
			mem = encodeARGB(&jpeg_encoder, rescale_buffer, 512, &mem_size);
		}else mem = encodeARGB(&jpeg_encoder, param.base, param.pitch, &mem_size);
		sceNetSendto(stream_skt, mem, mem_size, 0, (SceNetSockaddr*)&addrFrom, sizeof(addrFrom));
	}
}

// We prevent the application to manually reset clocks to lower values
int SetGenericClockFrequency(int freq, tai_hook_ref_t ref){
	return TAI_CONTINUE(int, ref, freq);
}

int scePowerSetBusClockFrequency_patched(int freq) {
	return SetGenericClockFrequency(222, ref[0]);
}

int scePowerSetGpuClockFrequency_patched(int freq) {
	return SetGenericClockFrequency(222, ref[1]);
}

int scePowerSetGpuXbarClockFrequency_patched(int freq) {
	return SetGenericClockFrequency(166, ref[2]);
}

int scePowerSetArmClockFrequency_patched(int freq) {
	return SetGenericClockFrequency(444, ref[3]);
}

// Checking buttons startup/closeup
void checkInput(SceCtrlData *ctrl){
	SceDisplayFrameBuf param;
	if (status != NOT_TRIGGERED && status < SYNC_BROADCAST){
		if ((ctrl->buttons & SCE_CTRL_DOWN) && (!(old_buttons & SCE_CTRL_DOWN))){
			cfg_i++;
			if (cfg_i >= MENU_ENTRIES) cfg_i = 0;
		}else if ((ctrl->buttons & SCE_CTRL_UP) && (!(old_buttons & SCE_CTRL_UP))){
			cfg_i--;
			if (cfg_i < 0 ) cfg_i = MENU_ENTRIES-1;
		}else if ((ctrl->buttons & SCE_CTRL_CROSS) && (!(old_buttons & SCE_CTRL_CROSS))){
			switch (cfg_i){
				case 0:
					qual_i = (qual_i + 1) % QUALITY_ENTRIES;
					break;
				case 3:
					param.size = sizeof(SceDisplayFrameBuf);
					sceDisplayGetFrameBuf(&param, SCE_DISPLAY_SETBUF_NEXTFRAME);
					if (param.width == 960 && param.height == 544) encoderSetRescaler(&jpeg_encoder, (rescale_buffer == NULL) ? 1 : 0);
					rescale_buffer = jpeg_encoder.rescale_buffer;
					break;
				case 4:
					frameskip = (frameskip + 1) % 5;
					break;
				case 5:
					stream_type = (stream_type + 1) % 2;
					break;
				case 6:
					encoderSetQuality(&jpeg_encoder, qual_val[qual_i]);
					status = LISTENING;
					break;
				default:
					break;
			}
		}else if ((ctrl->buttons & SCE_CTRL_TRIANGLE) && (!(old_buttons & SCE_CTRL_TRIANGLE))){
			status = NOT_TRIGGERED;
		}
	}else if ((ctrl->buttons & SCE_CTRL_LTRIGGER) && (ctrl->buttons & SCE_CTRL_SELECT)){
		status = CONFIG_MENU;
	}
	old_buttons = ctrl->buttons;
	if (status != NOT_TRIGGERED && status < SYNC_BROADCAST) ctrl->buttons = 0x0; // Nullifing input
}

// This can be considered as our main loop
int sceDisplaySetFrameBuf_patched(const SceDisplayFrameBuf *pParam, int sync) {
	
	if (firstBoot){
		firstBoot = 0;
		
		// Initializing internal renderer
		setTextColor(0x00FFFFFF);
		
		// Initializing JPG encoder
		isEncoderUnavailable = encoderInit(pParam->width, pParam->height, pParam->pitch, &jpeg_encoder, video_quality, enforce_sw, 0);
		rescale_buffer = (uint32_t*)jpeg_encoder.rescale_buffer;
		
		// Initializing Net if encoder is ready
		if ((!isEncoderUnavailable) || (skip_net_init)){
			isNetAvailable = (SceUID)malloc(NET_SIZE);
			if (isNetAvailable){
				sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
				int ret = sceNetShowNetstat();
				if (ret == SCE_NET_ERROR_ENOTINIT) {
					SceNetInitParam initparam;
					initparam.memory = (void*)isNetAvailable;
					initparam.size = NET_SIZE;
					initparam.flags = 0;
					sceNetInit(&initparam);
				}
				sceNetCtlInit();
				SceNetCtlInfo info;
				sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);
				sprintf(vita_ip,"%s",info.ip_address);
				sceNetInetPton(SCE_NET_AF_INET, info.ip_address, &vita_addr);
			}
		}
		
	}
	
	updateFramebuf(pParam);
	SceCtrlData pad;
	sceCtrlPeekBufferPositive(0, &pad, 1);
	checkInput(&pad);
	
	if (status == NOT_TRIGGERED){
		if (isEncoderUnavailable) drawStringF(5,5, "ERROR: encoderInit -> 0x%X", isEncoderUnavailable);
		else if ((!isNetAvailable) && (!skip_net_init)) drawString(5,5, "ERROR: malloc(NET_SIZE) -> NULL");
	}else if ((!isEncoderUnavailable) && (isNetAvailable || skip_net_init)){
		char txt[32], unused[256];
		int sndbuf_size;
		int mem_size;
		unsigned int fromLen = sizeof(addrFrom);
		switch (status){
			case CONFIG_MENU:
				drawStringF(5,5, "IP: %s", vita_ip);
				drawStringF(5,25, "Title ID: %s", titleid);
				drawString(5, 50, "Standalone ScreenStream v.0.1 Beta - CONFIG MENU");
				drawConfigMenu();
				break;
			case LISTENING:
				if (stream_skt < 0){
					sndbuf_size = STREAM_BUFSIZE;
					stream_skt = sceNetSocket("Stream Socket", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, SCE_NET_IPPROTO_UDP);
					addrTo.sin_family = SCE_NET_AF_INET;
					addrTo.sin_port = sceNetHtons(STREAM_PORT);
					addrTo.sin_addr.s_addr = vita_addr;
					sceNetBind(stream_skt, (SceNetSockaddr*)&addrTo, sizeof(addrTo));			
					sceNetSetsockopt(stream_skt, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDBUF, &sndbuf_size, sizeof(sndbuf_size));
				}
				sceNetRecvfrom(stream_skt, unused, 8, 0, (SceNetSockaddr*)&addrFrom, &fromLen);
				sprintf(txt, "%d;%d;%hhu", (jpeg_encoder.rescale_buffer != NULL) ? 480 : pParam->width, (jpeg_encoder.rescale_buffer != NULL) ? 272 : pParam->height, jpeg_encoder.isHwAccelerated);
				sceNetSendto(stream_skt, txt, 32, 0, (SceNetSockaddr*)&addrFrom, sizeof(addrFrom));
				status = SYNC_BROADCAST + stream_type;
				
				// Sending request to secondary thread
				if (status == ASYNC_BROADCAST) sceKernelSignalSema(async_mutex, 1);
				
				break;
			case SYNC_BROADCAST:
				if (loopDrawing == (3 + frameskip)){				
					if (rescale_buffer != NULL){ // Downscaler available
						rescaleBuffer((uint32_t*)pParam->base, rescale_buffer, pParam->width, pParam->height, pParam->pitch);
						mem = encodeARGB(&jpeg_encoder, rescale_buffer, 512, &mem_size);
					}else mem = encodeARGB(&jpeg_encoder, pParam->base, pParam->pitch, &mem_size);
					sceNetSendto(stream_skt, mem, mem_size, 0, (SceNetSockaddr*)&addrFrom, sizeof(addrFrom));
					loopDrawing = 0;
				}else loopDrawing++;
				break;
			default:
				break;
		}
	}
	
	#ifndef NO_DEBUG
	setTextColor(0x00FFFFFF);
	drawStringF(5, 100, "taipool free space: %lu KBs", (taipool_get_free_space()>>10));
	#endif
	
	return TAI_CONTINUE(int, ref[4], pParam, sync);
}

int sceSysmoduleUnloadModule_patched(SceSysmoduleModuleId module) {
	if (module == SCE_SYSMODULE_NET) return 0;
	else return TAI_CONTINUE(int, ref[5], module);
}

int sceNetTerm_patched(void) {
	return 0;
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args) {
	
	// Setting maximum clocks
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
	
	// Checking if game is blacklisted
	sceAppMgrAppParamGetString(0, 12, titleid , 256);	
	if (strncmp(titleid, "PCSE00491", 9) == 0){ // Minecraft (USA)
		mempool_size = 0x200000;
		skip_net_init = 1;
	}else if (strncmp(titleid, "PCSF00178", 9) == 0){ // Assassin's Creed III: Liberation (EUR)
		mempool_size = 0x200000;
		skip_net_init = 1;
		// TODO: Game disables net feature for single player, that must be prevented
	}
	
	// Mutex for asynchronous streaming triggering
	async_mutex = sceKernelCreateSema("async_mutex", 0, 0, 1, NULL);
	
	// Starting secondary thread for asynchronous streaming
	stream_thread_id = sceKernelCreateThread("stream_thread", stream_thread, 0x40, 0x200000, 0, 0, NULL);
	if (stream_thread_id >= 0) sceKernelStartThread(stream_thread_id, 0, NULL);
	
	// Initializing taipool mempool for dynamic memory managing
	taipool_init(mempool_size);
	
	// Hooking needed functions
	hookFunction(0xB8D7B3FB, scePowerSetBusClockFrequency_patched);
	hookFunction(0x717DB06C, scePowerSetGpuClockFrequency_patched);
	hookFunction(0xA7739DBE, scePowerSetGpuXbarClockFrequency_patched);
	hookFunction(0x74DB5AE5, scePowerSetArmClockFrequency_patched);
	hookFunction(0x7A410B64, sceDisplaySetFrameBuf_patched);
	if (strncmp(titleid, "PCSB0074", 9) == 0){
		hookFunction(0x31D87805, sceSysmoduleUnloadModule_patched);
		hookFunction(0xEA3CC286, sceNetTerm_patched);
	}
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
	
	// Freeing encoder and net related stuffs
	if (!firstBoot){
		if (!isEncoderUnavailable) encoderTerm(&jpeg_encoder);
		if (isNetAvailable){
			sceNetSocketClose(stream_skt);
			free((void*)isNetAvailable); 
		}
	}
	
	// Freeing hooks
	int i;
	for (i = 0; i < HOOKS_NUM; i++){
		taiHookRelease(g_hooks[i], ref[i]);
	}

	return SCE_KERNEL_STOP_SUCCESS;
	
}