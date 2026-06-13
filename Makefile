CFLAGS ?= -O2 -g -Wall -Wextra -std=c99
LDFLAGS ?= -pthread
LDLIBS ?= -lm

PROGNAME = pluto_sat_tracker
HELPERNAME = pluto_fm_receiver
SOURCES = src/pluto_sat_tracker.c
OBJECTS = $(SOURCES:.c=.o)
HELPER_SOURCES = src/pluto_fm_receiver.c
HELPER_OBJECTS = $(HELPER_SOURCES:.c=.o)

.PHONY: all clean

all: $(PROGNAME) $(HELPERNAME)

$(PROGNAME): $(OBJECTS)
	$(CC) -o $@ $(OBJECTS) $(LDFLAGS) $(LDLIBS)

$(HELPERNAME): $(HELPER_OBJECTS)
	$(CC) -o $@ $(HELPER_OBJECTS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(OBJECTS) $(HELPER_OBJECTS) $(PROGNAME) $(HELPERNAME)
