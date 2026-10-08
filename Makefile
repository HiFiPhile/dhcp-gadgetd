CC ?= cc
PKG_CONFIG ?= pkg-config
TARGET := dhcp-gadgetd
TEST_TARGET := tests/config_test
BUS_TEST_TARGET := tests/bus_test
PROTO_TEST_TARGET := tests/proto_test

CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L
CPPFLAGS ?= -Iinclude
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libsystemd)
LDLIBS += $(shell $(PKG_CONFIG) --libs libsystemd)
LDFLAGS ?=

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
HEADERS := $(wildcard include/*.h)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(TEST_TARGET): tests/config_test.c src/config.c src/log.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(BUS_TEST_TARGET): tests/bus_test.c src/bus.c src/allocator.c src/options.c src/log.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS) $(LDLIBS)

$(PROTO_TEST_TARGET): tests/proto_test.c src/dhcp_proto.c src/allocator.c src/options.c src/log.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter-out src/dhcp_proto.c,$(filter %.c,$^)) -o $@ $(LDFLAGS) $(LDLIBS)

test: $(TEST_TARGET) $(BUS_TEST_TARGET) $(PROTO_TEST_TARGET)
	./$(TEST_TARGET)
	./$(PROTO_TEST_TARGET)
	dbus-run-session -- ./$(BUS_TEST_TARGET)

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(TARGET) $(TEST_TARGET) $(BUS_TEST_TARGET) $(PROTO_TEST_TARGET)

-include $(OBJ:.o=.d)
