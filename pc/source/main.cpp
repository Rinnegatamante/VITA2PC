extern "C"{
	#include "SDL/SDL.h"
	#include "SDL/SDL_image.h"
	#include "SDL/SDL_mixer.h"
	#include "SDL/SDL_syswm.h"
	#include "SDL/SDL_opengl.h"
	#include "AL/al.h"
	#include "AL/alc.h"
	#include "icon.h"
}
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <windows.h>
#include <shellapi.h>
#ifdef __WIN32__
# include <winsock2.h>
#else
# include <sys/socket.h>
# include <netinet/in.h>
# include <netdb.h>
# include <arpa/inet.h>
#endif

#define VIDEO_PORT     5000     // Port to use for video streaming
#define AUDIO_PORT     4000     // Starting port to use for audio streaming
#define RCV_BUFSIZE    0x800000 // Size of the buffer used to store received packets
#define BUFSIZE_SWAP   0x100000 // Offset of buffer swap for audio buffers
#define AUDIO_CHANNELS 8        // PSVITA has 8 available audio channels
#define DISPLAY_MODES  4        // Available rendering modes

// Display modes names
const char* modes[DISPLAY_MODES] = {
	"Original Resolution (No Filter)",
	"Original Resolution (Bilinear Filter)",
	"Vita Resolution (No Filter)",
	"Vita Resolution (Bilinear Filter)"
};

// Audioports struct
typedef struct audioPort{
	int len;
	int samplerate;
	int mode;
	uint8_t buffer[RCV_BUFSIZE];
	Mix_Chunk* chunk;
} audioPort;

typedef struct{
	uint32_t sock;
	struct sockaddr_in addrTo;
} Socket;

int width, height, size, samplerate, dwidth, dheight;
SDL_Surface* frame = NULL;
SDL_Surface* screen = NULL;
SDL_Surface* new_frame = NULL;
uint8_t* buffer;
GLint nofcolors = 3;
GLenum texture_format=GL_RGB;
GLuint texture=0, min_filter, mag_filter;
char host[32];
static audioPort ports[AUDIO_CHANNELS];
static int thdId[AUDIO_CHANNELS] = {0,1,2,3,4,5,6,7};
static int mix_started = 0;
int audio_mode = 0;

// OpenAL config
ALuint snd_data[AUDIO_CHANNELS];
ALuint snd_src[AUDIO_CHANNELS];
ALCdevice* dev;
ALCcontext* ctx;
volatile int queued[AUDIO_CHANNELS];

int mode = 0;

void updateFrame(){

	// Loading frame
	SDL_RWops* rw = SDL_RWFromMem(buffer,size);
	new_frame = IMG_Load_RW(rw, 1);
	if (new_frame != NULL){
		SDL_FreeSurface(frame);
		frame = new_frame;
	}else printf("\nSDL Error: %s", SDL_GetError());
	if (frame == NULL) return;
	
	fflush(stdout);
	glTexImage2D( GL_TEXTURE_2D, 0, nofcolors, frame->w, frame->h, 0, texture_format, GL_UNSIGNED_BYTE, frame->pixels );
}

// Drawing function using openGL
void drawFrame(){
	if (texture == 0) return;	
	glClear( GL_COLOR_BUFFER_BIT );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
	glBegin( GL_QUADS );
	glTexCoord2i( 0, 0 );
	glVertex3f( 0, 0, 0 );
	glTexCoord2i( 1, 0 );
	glVertex3f( dwidth, 0, 0 );
	glTexCoord2i( 1, 1 );
	glVertex3f( dwidth, dheight, 0 );
	glTexCoord2i( 0, 1 );
	glVertex3f( 0, dheight, 0 );
	glEnd();
	glLoadIdentity();
	SDL_GL_SwapBuffers();
}

// Changes current display mode
void changeDisplayMode(int mode){
	switch (mode){
		case 0:
		case 2:
			mag_filter = min_filter = GL_NEAREST;
			break;
		case 1:
		case 3:
			mag_filter = min_filter = GL_LINEAR;
			break;
	}
	if (mode < 2){
		dwidth = width;
		dheight = height;
	}else{
		dwidth = 960;
		dheight = 544;
	}
	if (mode == 0 || mode == 2){
		screen = SDL_SetVideoMode(dwidth, dheight, 32, SDL_OPENGL);
		glClearColor(0, 0, 0, 0);
		glEnable(GL_TEXTURE_2D);
		glViewport( 0, 0, dwidth, dheight);
		glMatrixMode( GL_PROJECTION );
		glLoadIdentity();
		glOrtho(0, dwidth, dheight, 0, -1, 1);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
	}
}

DWORD WINAPI SDLaudioThread(void* data);
Socket* audio_socket[AUDIO_CHANNELS];

void swapChunk_CB(int chn){
	
	int rbytes = recv(audio_socket[chn]->sock, (char*)ports[chn].buffer, RCV_BUFSIZE, 0);
	if (rbytes <= 0){
		memset(ports[chn].buffer, 0, 8192);
		rbytes = 8192;
	}
	
	// Audio port closed on Vita side
	if (rbytes < 512){
		printf("\nAudio channel %d closed", chn);
		CreateThread(NULL, 0, SDLaudioThread, &thdId[chn], 0, NULL);
		return;
	}
	
	ports[chn].chunk = Mix_QuickLoad_RAW(ports[chn].buffer, rbytes);
	int err = Mix_PlayChannel(chn, ports[chn].chunk, 0);
	//if (err == -1) printf("\nERROR: Failed outputting audio chunk.\n%s",Mix_GetError());
}

DWORD WINAPI SDLaudioThread(void* data) {
	
	int* ptr = (int*)data;
	int id = ptr[0];
	
	printf("\nAudio thread for channel %d started", id);
	
	// Creating client socket
	audio_socket[id] = (Socket*) malloc(sizeof(Socket));
	memset(&audio_socket[id]->addrTo, '0', sizeof(audio_socket[id]->addrTo));
	audio_socket[id]->addrTo.sin_family = AF_INET;
	audio_socket[id]->addrTo.sin_port = htons(AUDIO_PORT + id);
	audio_socket[id]->addrTo.sin_addr.s_addr = inet_addr(host);
	int addrLen = sizeof(audio_socket[id]->addrTo);
	audio_socket[id]->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	
	// Connecting to VITA2PC
	int err = connect(audio_socket[id]->sock, (struct sockaddr*)&audio_socket[id]->addrTo, sizeof(audio_socket[id]->addrTo));
	
	int rbytes;
	do{
		send(audio_socket[id]->sock, "request", 8, 0);
		rbytes = recv(audio_socket[id]->sock, (char*)&ports[id], RCV_BUFSIZE, 0);
	}while (rbytes <= 0);
	
	send(audio_socket[id]->sock, "request", 8, 0);
	audioPort* port = (audioPort*)&ports[id];
	printf("\nAudio thread for port %d operative (Samplerate: %d Hz, Mode: %s)", id, port->samplerate, port->mode == 0 ? "Mono" : "Stereo");
		
	printf("\nStarted Mixer with Samplerate: %d Hz, Mode: %s, Chunk Length: %d", port->samplerate, port->mode == 0 ? "Mono" : "Stereo", port->len);
	err = Mix_OpenAudio(port->samplerate, AUDIO_S16LSB, port->mode + 1, port->len);
	if (err < 0) printf("\nERROR: Failed starting mixer.\n%s",Mix_GetError());
	Mix_ChannelFinished(swapChunk_CB);
	
	int rcvbuf = RCV_BUFSIZE;
	setsockopt(audio_socket[id]->sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf));
	u_long _true = 1;
	ioctlsocket(audio_socket[id]->sock, FIONBIO, &_true);
	do{
		rbytes = recv(audio_socket[id]->sock, (char*)port->buffer, RCV_BUFSIZE, 0);
	}while (rbytes <= 0);
	
	port->chunk = Mix_QuickLoad_RAW(port->buffer, rbytes);
	if (port->chunk == NULL) printf("\nERROR: Failed opening audio chunk.\n%s",Mix_GetError());
	int ch = Mix_PlayChannel(id, port->chunk, 0);
	if (ch == -1) printf("\nERROR: Failed outputting audio chunk.\n%s",Mix_GetError());
	else printf("\nStarting audio playback on channel %d. (Chunks size: %d bytes)", ch, rbytes);
	
	return 0;
}

DWORD WINAPI ALaudioThread(void* data) {
	
	int* ptr = (int*)data;
	int id = ptr[0];
	int buf_idx = 0;
	int al_idx = id * 3;
	
	printf("\nAudio thread for channel %d started", id);
	
	// Creating client socket
	audio_socket[id] = (Socket*) malloc(sizeof(Socket));
	memset(&audio_socket[id]->addrTo, '0', sizeof(audio_socket[id]->addrTo));
	audio_socket[id]->addrTo.sin_family = AF_INET;
	audio_socket[id]->addrTo.sin_port = htons(AUDIO_PORT + id);
	audio_socket[id]->addrTo.sin_addr.s_addr = inet_addr(host);
	int addrLen = sizeof(audio_socket[id]->addrTo);
	audio_socket[id]->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	
	// Connecting to VITA2PC
	int err = connect(audio_socket[id]->sock, (struct sockaddr*)&audio_socket[id]->addrTo, sizeof(audio_socket[id]->addrTo));
	
	int rbytes;
	do{
		usleep(10000);
		send(audio_socket[id]->sock, "request", 8, 0);
		rbytes = recv(audio_socket[id]->sock, (char*)&ports[id], RCV_BUFSIZE, 0);
	}while (rbytes <= 0);
	
	send(audio_socket[id]->sock, "request", 8, 0);
	audioPort* port = (audioPort*)&ports[id];
	memset(port->buffer, 0, RCV_BUFSIZE);
	printf("\nAudio thread for port %d operative (Samplerate: %d Hz, Mode: %s)", id, port->samplerate, port->mode == 0 ? "Mono" : "Stereo");
		
	printf("\nStarted device with Samplerate: %d Hz, Mode: %s, Chunk Length: %d", port->samplerate, port->mode == 0 ? "Mono" : "Stereo", port->len);
	if (port->len < 4096) port->len = 4096;
	
	int rcvbuf = RCV_BUFSIZE;
	setsockopt(audio_socket[id]->sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf));
	u_long _true = 1;
	ioctlsocket(audio_socket[id]->sock, FIONBIO, &_true);
	
	ALenum format = port->mode == 0 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
	alBufferData(snd_data[al_idx + buf_idx], format, port->buffer, port->len, port->samplerate);
	alBufferData(snd_data[al_idx + buf_idx + 1], format, port->buffer, port->len, port->samplerate);
	alBufferData(snd_data[al_idx + buf_idx + 2], format, port->buffer, port->len, port->samplerate);
	alSourceQueueBuffers(snd_src[id], 3, &snd_data[al_idx + buf_idx]);
	alSourcePlay(snd_src[id]);
	
	ALuint buffer;
	ALint val;
	
	for (;;){
	
		alGetSourcei(snd_src[id], AL_BUFFERS_PROCESSED, &val);
		if(val <= 0) continue;

		while (val--){
			do{
				rbytes = recv(audio_socket[id]->sock, (char*)port->buffer + BUFSIZE_SWAP * buf_idx, RCV_BUFSIZE, 0);
			}while (rbytes <= 0);
			if (rbytes < 512){
				do{
					alGetSourcei(snd_src[id], AL_SOURCE_STATE, &val);
				}while(val == AL_PLAYING);
				alSourceUnqueueBuffers(snd_src[id], 3, &snd_data[al_idx + buf_idx]);
				printf("\nAudio channel %d closed", id);
				CreateThread(NULL, 0, ALaudioThread, &thdId[id], 0, NULL);
				return 0;
			}
			alSourceUnqueueBuffers(snd_src[id], 1, &buffer);
			alBufferData(buffer, format, port->buffer + BUFSIZE_SWAP * buf_idx, port->len, port->samplerate);
			alSourceQueueBuffers(snd_src[id], 1, &buffer);
			buf_idx = (buf_idx + 1) % 3;
		}
	
		alGetSourcei(snd_src[id], AL_SOURCE_STATE, &val);
		if(val != AL_PLAYING) alSourcePlay(snd_src[id]);
		
	}

}

WNDPROC oldProc;
HICON icon;
HWND hwnd;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam){
	if (msg == WM_SETCURSOR){
		if (LOWORD(lParam) == HTCLIENT){
			::SetCursor(::LoadCursor(NULL, IDC_ARROW));
			return TRUE;
		}
	}
	return ::CallWindowProc(oldProc, hwnd, msg, wParam, lParam);
}

void setWindowIconFromRes()
{
	HINSTANCE handle = ::GetModuleHandle(NULL);
	icon = ::LoadIcon(handle, MAKEINTRESOURCE(ICO1));
	SDL_SysWMinfo wminfo;
	SDL_VERSION(&wminfo.version)
	SDL_GetWMInfo(&wminfo);
	hwnd = wminfo.window;
	::SetClassLong(hwnd, GCL_HICON, (LONG) icon);
	oldProc = (WNDPROC) ::SetWindowLong(hwnd, GWL_WNDPROC, (LONG) WndProc);
}

int main(int argc, char* argv[]){
	
	#ifdef __WIN32__
	WORD versionWanted = MAKEWORD(1, 1);
	WSADATA wsaData;
	WSAStartup(versionWanted, &wsaData);
	#endif
	
	int dummy = 4;
	if (argc > 1){
		char* ip = (char*)(argv[1]);
		sprintf(host,"%s",ip);
	}else{
		printf("Insert Vita IP: ");
		scanf("%s",host);
	}
	
	// Writing info on the screen
	printf("IP: %s\nPort: %d\n\n",host, VIDEO_PORT);
	
	// Asking for preferred audio mode
	printf("Select audio driver (0 = SDL Mixer, 1 = OpenAL): ");
	scanf("%d",&audio_mode);
	if (audio_mode != 0 && audio_mode != 1){
		printf("\nInvalid audio mode, SDL Mixer will be used...");
		audio_mode = 0;
	}
	
	printf("\n%s will be used as audio driver.", audio_mode == 0 ? "SDL Mixer" : "OpenAL");
	
	// Creating client socket
	Socket* my_socket = (Socket*) malloc(sizeof(Socket));
	memset(&my_socket->addrTo, '0', sizeof(my_socket->addrTo));
	my_socket->addrTo.sin_family = AF_INET;
	my_socket->addrTo.sin_port = htons(VIDEO_PORT);
	my_socket->addrTo.sin_addr.s_addr = inet_addr(host);
	int addrLen = sizeof(my_socket->addrTo);
	my_socket->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (my_socket->sock < 0){
		printf("\nFailed creating socket.");
		return -1;
	}else printf("\nClient socket created on port %d", VIDEO_PORT);
	
	// Connecting to VITA2PC
	int err = connect(my_socket->sock, (struct sockaddr*)&my_socket->addrTo, sizeof(my_socket->addrTo));
	if (err < 0 ){
		printf("\nFailed connecting server.");
		close(my_socket->sock);
		return -1;
	}else printf("\nConnection established!");
	fflush(stdout);
	u_long _true = 1;
	uint8_t accelerated;
	char sizes[32];
	send(my_socket->sock, "request", 8, 0);
	recv(my_socket->sock, sizes, 32, 0);
	sscanf(sizes, "%d;%d;%hhu", &width, &height, &accelerated);
	printf("\nThe game %s hardware acceleration.", accelerated ? "supports" : "does not support");
	printf("\nGame resolution: %dx%d", width, height);
	fflush(stdout);
	ioctlsocket(my_socket->sock, FIONBIO, &_true);
	int rcvbuf = RCV_BUFSIZE;
	setsockopt(my_socket->sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf));
	getsockopt(my_socket->sock, SOL_SOCKET, SO_RCVBUF, (char*)(&rcvbuf), &dummy);
	printf("\nReceive buffer size set to %d bytes", rcvbuf);
	
	// Initializing SDL
	uint8_t quit = 0;
	SDL_Event event;
	SDL_Init( SDL_INIT_EVERYTHING );
	
	// Initializing window
	setWindowIconFromRes();
	SDL_WM_SetCaption("VITA2PC", NULL);
	SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
	changeDisplayMode(mode);
	
	// Creating taskbar icon
	SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
	NOTIFYICONDATA notifyIconData;
	TCHAR szTIP[64] = TEXT("VITA2PC");
	memset( &notifyIconData, 0, sizeof( NOTIFYICONDATA ) ) ;
	notifyIconData.cbSize = sizeof(NOTIFYICONDATA);
	notifyIconData.hWnd = hwnd;
	notifyIconData.uID = ID_TRAY_APP_ICON;
	notifyIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	notifyIconData.uCallbackMessage = WM_SYSICON;
	notifyIconData.hIcon = (HICON)LoadIcon( GetModuleHandle(NULL), MAKEINTRESOURCE(ICO1) ) ;
	strncpy(notifyIconData.szTip, szTIP, sizeof(szTIP));
	Shell_NotifyIcon(NIM_ADD, &notifyIconData);
	
	// Starting OpenAL if required
	if (audio_mode == 1){
		printf("\nInitializing audio system...");
		const char* devname = alcGetString(NULL, ALC_DEFAULT_DEVICE_SPECIFIER);
		printf("\nDetected %s audio device...", devname);
		dev = alcOpenDevice(devname);
		printf("\nCreating audio context...");
		ctx = alcCreateContext(dev, NULL);
		alcMakeContextCurrent(ctx);
		alGenBuffers(AUDIO_CHANNELS * 3, snd_data);
		alGenSources(AUDIO_CHANNELS, snd_src);
		printf("\nAudio system ready!");
		fflush(stdout);
	}
	
	// Starting audio streaming thread
	for (int i = 0; i < AUDIO_CHANNELS; i++){
		CreateThread(NULL, 0, audio_mode == 0 ? SDLaudioThread : ALaudioThread, &thdId[i], 0, NULL);
	}
	
	// Framebuffer and texture setup
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	buffer = (uint8_t*)malloc((960*544)<<2);
	
	for (;;){

		// Receiving a new frame
		int rbytes = 0;
		while (rbytes <= 0){
			rbytes = recv(my_socket->sock, (char*)buffer, RCV_BUFSIZE, 0);
			while( SDL_PollEvent( &event ) ) {
				switch (event.type){
					case SDL_QUIT: // Closing application
						quit = 1;
						break;
					case SDL_SYSWMEVENT: // Changing display mode
						if (event.syswm.msg->msg == WM_USER + 1){
							if (LOWORD(event.syswm.msg->lParam) == WM_LBUTTONUP){
								mode = (mode + 1) % DISPLAY_MODES;
								printf("\nSwitched to %s mode.", modes[mode]);
								changeDisplayMode(mode);
							}
						}
						break;
					default:
						break;
				}
				if( event.type == SDL_QUIT ) {
					quit = 1;
				} 
			}
			if (quit) break;
			size = rbytes;
		}
		if (quit) break;
		
		updateFrame();
		drawFrame();
		
	}
	
	free(buffer);
	return 0;
}