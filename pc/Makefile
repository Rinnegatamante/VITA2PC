TARGET := VITA2PC
SOURCES := source
BUILD	:= build

LIBS := -lopengl32 -lSDL -lSDL_image -lSDL_mixer -lpsapi -lws2_32

CFLAGS := -DNO_STDIO_REDIRECT -Dmain=SDL_main
LFLAGS :=
LDARGS := -DNO_STDIO_REDIRECT -Dmain=SDL_main

C_FILES	:= $(shell find $(SOURCES) -type f -name '*.c')
OBJ_FILES := $(patsubst %.c,%.o,$(C_FILES))
CC = $(GCC_PREFIX)gcc
LD = $(GCC_PREFIX)gcc

all: $(TARGET).exe

$(TARGET).exe: $(OBJ_FILES)
	@echo Linking object files...
	$(LD) -o $@ $(LDARGS) $^ $(LIBS)

%.o: %.c
	@echo Creating object file $@...
	$(CC) $(CFLAGS) -c $< -o $@
	
clean:
	@echo Cleaning...
	@rm -f $(OBJ_FILES)