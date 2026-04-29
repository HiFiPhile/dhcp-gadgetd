CC ?= cc
TARGET := dhcp-gadgetd

CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L
CPPFLAGS ?= -Iinclude
LDFLAGS ?=

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)