CFLAGS ?= -O2 -g -Wall -Wextra -std=c99
LDFLAGS ?= -pthread
LDLIBS ?= -lm

PROGNAME = pluto_sat_tracker
HELPERNAME = pluto_fm_receiver
DECODERNAME = pluto_digital_decoder
TESTGENNAME = pluto_test_gen
SOURCES = src/pluto_sat_tracker.c
OBJECTS = $(SOURCES:.c=.o)
HELPER_SOURCES = src/pluto_fm_receiver.c
HELPER_OBJECTS = $(HELPER_SOURCES:.c=.o)
DECODER_SOURCES = src/pluto_digital_decoder.c
DECODER_OBJECTS = $(DECODER_SOURCES:.c=.o)
TESTGEN_SOURCES = src/pluto_test_gen.c
TESTGEN_OBJECTS = $(TESTGEN_SOURCES:.c=.o)

.PHONY: all clean

all: $(PROGNAME) $(HELPERNAME) $(DECODERNAME) $(TESTGENNAME)

$(PROGNAME): $(OBJECTS)
	$(CC) -o $@ $(OBJECTS) $(LDFLAGS) $(LDLIBS)

$(HELPERNAME): $(HELPER_OBJECTS)
	$(CC) -o $@ $(HELPER_OBJECTS) $(LDFLAGS) $(LDLIBS)

$(DECODERNAME): $(DECODER_OBJECTS)
	$(CC) -o $@ $(DECODER_OBJECTS) $(LDFLAGS) $(LDLIBS)

$(TESTGENNAME): $(TESTGEN_OBJECTS)
	$(CC) -o $@ $(TESTGEN_OBJECTS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(OBJECTS) $(HELPER_OBJECTS) $(DECODER_OBJECTS) $(TESTGEN_OBJECTS) \
	      $(PROGNAME) $(HELPERNAME) $(DECODERNAME) $(TESTGENNAME)
