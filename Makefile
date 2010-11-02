# Makefile for audimg
CC=gcc
CFLAGS=-g -lpthread -lm -lfftw3f -ljack -laubio -lgd -I/usr/local/include -I/usr/local/include/aubio -D_GNU_SOURCE=1 -D_THREAD_SAFE -I/usr/local/include/SDL
LDFLAGS=-lpthread -lm -lfftw3f -ljack -laubio -lgd -framework CoreAudio -framework CoreServices -framework AudioUnit -L/usr/local/lib -ljack -laubio -ljpeg -lfontconfig -lfreetype -lpng12 -lz /usr/local/lib/libiconv.dylib -Wl,-framework,Cocoa -L/usr/local/lib -lSDLmain -lSDL -lSDL_image 
SOURCES=main.c
OBJECTS=$(SOURCES:.c=.o)

all: 
	$(CC) $(CFLAGS) $(LDFLAGS) main.c resize.c -o sonify 

clean:
	rm -rf *o main
	rm -rf sonify
