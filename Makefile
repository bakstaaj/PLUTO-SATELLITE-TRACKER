CFLAGS ?= -O2 -g -Wall -Wextra -std=c99
LDFLAGS ?= -pthread
LDLIBS ?= -lm

PROGNAME = pluto_sat_tracker
HELPERNAME = pluto_fm_receiver
DECODERNAME = pluto_digital_decoder
SOURCES = src/pluto_sat_tracker.c
OBJECTS = $(SOURCES:.c=.o)
HELPER_SOURCES = src/pluto_fm_receiver.c
HELPER_OBJECTS = $(HELPER_SOURCES:.c=.o)
DECODER_SOURCES = src/pluto_digital_decoder.c
DECODER_OBJECTS = $(DECODER_SOURCES:.c=.o)

.PHONY: all clean

all: $(PROGNAME) $(HELPERNAME) $(DECODERNAME)

$(PROGNAME): $(OBJECTS)
	$(CC) -o $@ $(OBJECTS) $(LDFLAGS) $(LDLIBS)

$(HELPERNAME): 