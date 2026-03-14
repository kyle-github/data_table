CC = gcc
CFLAGS = -Wall -O2 -g -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer
TARGET = data_table
LIST_IDENTITY = list_identity
TEST_TARGET = test_bytes
SOURCES = data_table.c arena.c bytes.c cip.c
HEADERS = arena.h bytes.h cip.h
OBJECTS = $(SOURCES:.c=.o)
TEST_OBJECTS = test_bytes.o arena.o bytes.o
LIST_IDENTITY_OBJECTS = list_identity.o arena.o bytes.o cip.o

.PHONY: all clean test

all: $(TARGET) $(LIST_IDENTITY)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^

$(LIST_IDENTITY): $(LIST_IDENTITY_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $<

$(TEST_TARGET): $(TEST_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -f $(OBJECTS) $(TEST_OBJECTS) $(LIST_IDENTITY_OBJECTS) $(TARGET) $(TEST_TARGET) $(LIST_IDENTITY)

.PHONY: run
run: $(TARGET)
	./$(TARGET)
