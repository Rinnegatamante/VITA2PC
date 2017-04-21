#include <vitasdk.h>
#include <taihen.h>
#include <libk/stdlib.h>
#include <libk/stdio.h>
#include "renderer.h"
#include "encoder.h"
#include "utils.h"

#define HOOKS_NUM       10
#define MENU_ENTRIES    5
#define QUALITY_ENTRIES 5

#define STREAM_PORT    5000     // Port used for screen streaming
#define STREAM_BUFSIZE 0x80000  // Size of stream buffer
#define NET_SIZE       0x100000 // Size of net module buffer

#define NOT_TRIGGERED 0
#define CONFIG_MENU   1
#define LISTENING     2
#define BROADCASTING  3

static SceUID g_hooks[HOOKS_NUM];
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
static char* qualities[] = {"Best", "High", "Default", "Low", "Worst"};
static uint8_t qual_val[] = {0, 64, 128, 192, 255};
static uint8_t frameskip = 0;
static char* menu[] = {"Video Quality: ", "Video Codec: MJPEG", "Hardware Acceleration: Enabled","Frame Skip: ", "Start Screen Streaming"};

// Config Menu Renderer
void drawConfigMenu(){
	int i;
	for (i = 0; i < MENU_ENTRIES; i++){
		(i == cfg_i) ? setTextColor(0x0000FF00) : setTextColor(0x00FFFFFF);
		switch (i){
			case 0:
				drawStringF(5, 70 + i*20, "%s%s", menu[i], qualities[qual_i]);
				break;
			case 3:
				drawStringF(5, 70 + i*20, "%s%u", menu[i], frameskip);
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

// We hook sceGxmInitialize to perform stuffs on startup
int sceGxmInitialize_patched(const SceGxmInitializeParams *params) {
	
	// Initializing internal renderer
	setTextColor(0x00FFFFFF);
	
	// Setting maximum clocks
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
	
	return TAI_CONTINUE(int, ref[4], params);
}

// This can be considered as our main loop
int sceDisplaySetFrameBuf_patched(const SceDisplayFrameBuf *pParam, int sync) {
	
	if (firstBoot){
		firstBoot = 0;
		
		// Initializing JPG encoder
		isEncoderUnavailable = encoderInit(pParam->width, pParam->height, pParam->pitch, &jpeg_encoder, video_quality);
		
		// Initializing Net if encoder is ready
		if (!isEncoderUnavailable){
			isNetAvailable = (SceUID)malloc(NET_SIZE);
			if (isNetAvailable){
				sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
				int ret = sceNetShowNetstat();
				if (ret == SCE_NET_ERROR_ENOTINIT) {
					SceNetInitParam initparam;
					initparam.memory = isNetAvailable;
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
	
	if (status == NOT_TRIGGERED){
		if (isEncoderUnavailable) drawStringF(5,5, "ERROR: encoderInit -> 0x%X", isEncoderUnavailable);
		else if (!isNetAvailable) drawString(5,5, "ERROR: malloc(NET_SIZE) -> NULL");
	}else if ((!isEncoderUnavailable) && (isNetAvailable)){
		char txt[32], unused[256];
		int sndbuf_size;
		int mem_size;
		uint8_t* mem;
		SceNetSockaddrIn addrTo, addrFrom;
		unsigned int fromLen = sizeof(addrFrom);
		switch (status){
			case CONFIG_MENU:
				drawStringF(5,5, "IP: %s", vita_ip);
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
				sprintf(txt, "%d;%d;%hhu", pParam->pitch, pParam->height, 1);
				sceNetSendto(stream_skt, txt, 32, 0, (SceNetSockaddr*)&addrFrom, sizeof(addrFrom));
				status = BROADCASTING;
				break;
			case BROADCASTING:
				if (loopDrawing == (3 + frameskip)){
					mem = encodeARGB(&jpeg_encoder, pParam->base, pParam->width, pParam->height, pParam->pitch, &mem_size);
					sceNetSendto(stream_skt, mem, mem_size, 0, (SceNetSockaddr*)&addrFrom, sizeof(addrFrom));
					loopDrawing = 0;
				}else loopDrawing++;
				break;
		}
	}
	return TAI_CONTINUE(int, ref[5], pParam, sync);
}

// Checking buttons startup/closeup
void checkInput(SceCtrlData *ctrl){
	if (status != NOT_TRIGGERED && status != BROADCASTING){
		if ((ctrl->buttons & SCE_CTRL_DOWN) && (!(old_buttons & SCE_CTRL_DOWN))){
			cfg_i++;
			if (cfg_i >= MENU_ENTRIES) cfg_i = 0;
		}else if ((ctrl->buttons & SCE_CTRL_UP) && (!(old_buttons & SCE_CTRL_UP))){
			cfg_i--;
			if (cfg_i < 0 ) cfg_i = MENU_ENTRIES-1;
		}else if ((ctrl->buttons & SCE_CTRL_CROSS) && (!(old_buttons & SCE_CTRL_CROSS))){
			switch (cfg_i){
				case 0:
					qual_i++;
					if (qual_i >= QUALITY_ENTRIES) qual_i = 0;
					break;
				case 3:
					frameskip++;
					if (frameskip > 5) frameskip = 0;
					break;
				case 4:
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
	if (status != NOT_TRIGGERED && status != BROADCASTING) ctrl->buttons = 0x0; // Nullifing input
}

int sceCtrlPeekBufferPositive_patched(int port, SceCtrlData *ctrl, int count) {
	int ret = TAI_CONTINUE(int, ref[6], port, ctrl, count);
	checkInput(ctrl);
	return ret;
}

int sceCtrlPeekBufferPositive2_patched(int port, SceCtrlData *ctrl, int count) {
	int ret = TAI_CONTINUE(int, ref[7], port, ctrl, count);
	checkInput(ctrl);
	return ret;
}

int sceCtrlReadBufferPositive_patched(int port, SceCtrlData *ctrl, int count) {
	int ret = TAI_CONTINUE(int, ref[8], port, ctrl, count);
	checkInput(ctrl);
	return ret;
}

int sceCtrlReadBufferPositive2_patched(int port, SceCtrlData *ctrl, int count) {
	int ret = TAI_CONTINUE(int, ref[9], port, ctrl, count);
	checkInput(ctrl);
	return ret;
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args) {
	
	hookFunction(0xB8D7B3FB, scePowerSetBusClockFrequency_patched);
	hookFunction(0x717DB06C, scePowerSetGpuClockFrequency_patched);
	hookFunction(0xA7739DBE, scePowerSetGpuXbarClockFrequency_patched);
	hookFunction(0x74DB5AE5, scePowerSetArmClockFrequency_patched);
	hookFunction(0xB0F1E4EC, sceGxmInitialize_patched);
	hookFunction(0x7A410B64, sceDisplaySetFrameBuf_patched);
	hookFunction(0xA9C3CED6, sceCtrlPeekBufferPositive_patched);
	hookFunction(0x15F81E8C, sceCtrlPeekBufferPositive2_patched);
	hookFunction(0x67E7AB83, sceCtrlReadBufferPositive_patched);
	hookFunction(0xC4226A3E, sceCtrlReadBufferPositive2_patched);
	
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