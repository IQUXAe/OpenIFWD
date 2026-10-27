CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -std=c11 -Iinclude $(shell pkg-config --cflags libusb-1.0 2>/dev/null)
LDFLAGS ?= $(shell pkg-config --libs libusb-1.0 2>/dev/null || echo "-lusb-1.0")

GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0 pixman-1 2>/dev/null)
GTK_LDFLAGS = -rdynamic $(shell pkg-config --libs gtk+-3.0 pixman-1 2>/dev/null || echo "-lpixman-1") -lpthread

SRC = src/fls.c src/protocol.c src/transport_tty.c src/transport_usb.c src/flasher.c src/open_psi_embedded.c src/romdumper_embedded.c src/ram_dumper_embedded.c
OBJS = $(SRC:.c=.o)

MAIN_SRC = src/main.c
MAIN_OBJ = $(MAIN_SRC:.c=.o)

GUI_SRC = src/gui.c
GUI_OBJ = $(GUI_SRC:.c=.o)

TEST_SRC = tests/test_fls.c
TEST_OBJ = $(TEST_SRC:.c=.o)

TARGET = openifwd
GUI_TARGET = openifwd-gui
TEST_TARGET = test_runner

all: $(TARGET) $(GUI_TARGET)

$(TARGET): $(OBJS) $(MAIN_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(GUI_TARGET): $(OBJS) $(GUI_OBJ)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -o $@ $^ $(LDFLAGS) $(GTK_LDFLAGS)

src/gui.o: src/gui.c
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -c $< -o $@

$(TEST_TARGET): $(OBJS) $(TEST_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(MAIN_OBJ) $(GUI_OBJ) $(TEST_OBJ) $(TARGET) $(GUI_TARGET) $(TEST_TARGET) flashtool flashtool-gui

.PHONY: all clean test
