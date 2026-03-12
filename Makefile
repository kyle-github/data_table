CC = gcc
CFLAGS = -Wall -O2
TARGET = data_table
SOURCES = data_table.c arena.c bytes.c cip.c
HEADERS = arena.h bytes.h cip.h
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: run
run: $(TARGET)
	./$(TARGET)
