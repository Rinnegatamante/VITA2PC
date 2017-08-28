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
 
#include "SDL/SDL.h"
#include "SDL/SDL_image.h"
#include "SDL/SDL_mixer.h"
#include "SDL/SDL_opengl.h"
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#ifdef __WIN32__
# include <winsock2.h>
#else
# include <sys/socket.h>
# include <netinet/in.h>
# include <netdb.h>
# include <arpa/inet.h>
#endif

#define VIDEO_PORT 5000 // Port to use for video streaming
#define AUDIO_PORT 4000 // Port to use for audio streaming
#define RCV_BUFSIZE 0x800000 // Size of the buffer used to store received packets

typedef struct{
	uint32_t sock;
	struct sockaddr_in addrTo;
} Socket;

int width, height, size, samplerate;
SDL_Surface* frame = NULL;
SDL_Surface* new_frame = NULL;
char* buffer;
uint8_t* audio_buffer;
GLint nofcolors = 3;
GLenum texture_format=GL_RGB;
GLuint texture=0;
char host[32];

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
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexImage2D( GL_TEXTURE_2D, 0, nofcolors, frame->w, frame->h, 0, texture_format, GL_UNSIGNED_BYTE, frame->pixels );
	
}

// Drawing function using openGL
void drawFrame(){
	if (texture == 0) return;	
	glClear( GL_COLOR_BUFFER_BIT );
	glBindTexture( GL_TEXTURE_2D, texture );
	glBegin( GL_QUADS );
	glTexCoord2i( 0, 0 );
	glVertex3f( 0, 0, 0 );
	glTexCoord2i( 1, 0 );
	glVertex3f( width, 0, 0 );
	glTexCoord2i( 1, 1 );
	glVertex3f( width, height, 0 );
	glTexCoord2i( 0, 1 );
	glVertex3f( 0, height, 0 );
	glEnd();
	glLoadIdentity();
	SDL_GL_SwapBuffers();
}

Socket* audio_socket;
Mix_Chunk* mix_chunk;

void swapChunk_CB(int chn){
	int rbytes;
	do{
		rbytes = recv(audio_socket->sock, audio_buffer, RCV_BUFSIZE, 0);
	}while(rbytes < 0);
	mix_chunk = Mix_QuickLoad_RAW(audio_buffer, rbytes);
	int err = Mix_PlayChannel(chn, mix_chunk, 0);
	if (err == -1) printf("ERROR: Failed outputting audio chunk.\n%s\n",Mix_GetError());
}

DWORD WINAPI audioThread(void* data) {
	
	printf("\nAudio thread started");
	
	// Creating client socket
	audio_socket = (Socket*) malloc(sizeof(Socket));
	memset(&audio_socket->addrTo, '0', sizeof(audio_socket->addrTo));
	audio_socket->addrTo.sin_family = AF_INET;
	audio_socket->addrTo.sin_port = htons(AUDIO_PORT);
	audio_socket->addrTo.sin_addr.s_addr = inet_addr(host);
	int addrLen = sizeof(audio_socket->addrTo);
	audio_socket->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	
	// Connecting to VITA2PC
	int err = connect(audio_socket->sock, (struct sockaddr*)&audio_socket->addrTo, sizeof(audio_socket->addrTo));
	send(audio_socket->sock, "request", 8, 0);
	printf("\nAudio thread operative");
	
	audio_buffer = (uint8_t*)malloc(RCV_BUFSIZE);
	
	int ch = 0xDEADBEEF;;
	int rbytes;
	do{
		rbytes = recv(audio_socket->sock, audio_buffer, RCV_BUFSIZE, 0);
	}while (rbytes < 0);
	
	Mix_OpenAudio(samplerate,AUDIO_S16LSB,2,rbytes<<2);
	Mix_ChannelFinished(swapChunk_CB);
	
	for (;;){
		
		if (ch == 0xDEADBEEF){
			mix_chunk = Mix_QuickLoad_RAW(audio_buffer, rbytes);
			if (mix_chunk == NULL) printf("ERROR: Failed opening audio chunk.\n%s\n",Mix_GetError());
			else printf("\nStarting audio playback. (Chunks size: %d bytes)", rbytes);
			ch = Mix_PlayChannel(-1, mix_chunk, 0);
			if (ch == -1) printf("ERROR: Failed outputting audio chunk.\n%s\n",Mix_GetError());
		}
		
	}
	
	return 0;
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
	sscanf(sizes, "%d;%d;%hhu;%d", &width, &height, &accelerated, &samplerate);
	printf("\nThe game %s hardware acceleration.", accelerated ? "supports" : "does not support");
	printf("\nSetting window resolution to %d x %d", width, height);
	printf("\nAudio samplerate set to %d Hz", samplerate);
	fflush(stdout);
	ioctlsocket(my_socket->sock, FIONBIO, &_true);
	int rcvbuf = RCV_BUFSIZE;
	setsockopt(my_socket->sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf));
	getsockopt(my_socket->sock, SOL_SOCKET, SO_RCVBUF, (char*)(&rcvbuf), &dummy);
	printf("\nReceive buffer size set to %d bytes", rcvbuf);
	
	// Starting audio streaming thread
	HANDLE thread = CreateThread(NULL, 0, audioThread, NULL, 0, NULL);
	
	// Initializing SDL and openGL stuffs
	uint8_t quit = 0;
	SDL_Event event;
	SDL_Surface* screen = NULL;
	SDL_Init( SDL_INIT_EVERYTHING );
	SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
	screen = SDL_SetVideoMode( width, height, 32, SDL_OPENGL );
	glClearColor( 0, 0, 0, 0 );
	glEnable( GL_TEXTURE_2D );
	glViewport( 0, 0, width, height );
	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();
	glOrtho( 0, width, height, 0, -1, 1 );
	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();
	SDL_WM_SetCaption("VITA2PC", NULL);
	
	// Framebuffer & texture stuffs
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	buffer = (uint8_t*)malloc((width*height)<<2);
	
	for (;;){

		// Receiving a new frame
		int rbytes = 0;
		while (rbytes <= 0){
			rbytes = recv(my_socket->sock, buffer, RCV_BUFSIZE, 0);
			while( SDL_PollEvent( &event ) ) {
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