# PC build of the browser engine and its test tool (the firmware builds it
# as an ESP-IDF component instead; see CMakeLists.txt).
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-format-truncation \
	-Iinclude -Isrc -Ithird_party/tjpgd -Ihost
SRC = src/cy_browser.c src/cyb_font.c src/cyb_html.c src/cyb_image.c \
	src/cyb_layout.c src/cyb_text.c src/cyb_url.c third_party/tjpgd/tjpgd.c
OBJ = $(SRC:%.c=build/%.o)

all: build/cyb_shot build/test_engine

build/%.o: %.c $(wildcard include/*.h src/*.h host/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/libcybrowser.a: $(OBJ)
	$(AR) rcs $@ $^

build/cyb_shot: build/tests/cyb_shot.o build/host/cyb_net_curl.o build/libcybrowser.a
	$(CC) $(CFLAGS) $^ -o $@

build/test_engine: build/tests/test_engine.o build/libcybrowser.a
	$(CC) $(CFLAGS) $^ -o $@

test: build/cyb_shot build/test_engine
	sh tests/run.sh

clean:
	rm -rf build

.PHONY: all test clean
