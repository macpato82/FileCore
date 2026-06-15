# gfcref - FileCore G-format reference tool
#
#   make            build ./gfctool (or gfctool.exe on Windows)
#   make test       build + run the smoke test
#   make clean

CC      ?= gcc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
SRC      = gfctool.c gfc_check.c
BIN      = gfctool

ifeq ($(OS),Windows_NT)
  BIN := gfctool.exe
endif

$(BIN): $(SRC) gfc.h
	$(CC) $(CFLAGS) -o $(BIN) $(SRC)

.PHONY: test clean
test: $(BIN)
	./$(BIN) format test.img --size 256M --name TestDisc
	./$(BIN) info  test.img
	./$(BIN) check test.img

clean:
	rm -f $(BIN) test.img
