CFLAGS ?= -O2 -g -Wall -Wextra -std=c99
LDFLAGS ?= -pthread
LDLIBS ?= -lm

PROGNAME = pluto_sat_tracker
SOURCES = src/pluto_sat_tracker.c
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean

all: $(PROGNAME)

$(PROGNAME): $(OBJECTS)
	$(CC) -o $@ $(OBJECTS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(OBJECTS) $(PROGNAME)
