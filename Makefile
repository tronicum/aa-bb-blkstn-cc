# dd - ASCII Art Flag Demo (headless)  //  SPDX-License-Identifier: CC0-1.0
#
# Targets:
#   make          Preflight-Check + Build
#   make check    prueft nur die Toolchain (Compiler, libcurl, curl/make)
#   make clean
#
# macOS:  libcurl + Header sind in den Xcode Command Line Tools enthalten
#         (xcode-select --install) - Homebrew ist NICHT noetig.
# Linux:  apt install build-essential libcurl4-openssl-dev

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -pedantic
LDLIBS   = -lcurl -lm
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
LDLIBS  += -framework AudioToolbox -framework CoreFoundation
endif

STB_URL  = https://raw.githubusercontent.com/nothings/stb/master/stb_image.h

dd: check stb_image.h dd.c
	$(CC) $(CFLAGS) -o $@ dd.c $(LDLIBS)

check:
	@echo "== Preflight =="
	@command -v $(CC) >/dev/null 2>&1 \
	    && echo "compiler ($(CC)): OK ($$($(CC) --version | head -1))" \
	    || { echo "FEHLT: C-Compiler."; \
	         echo "  macOS: xcode-select --install"; \
	         echo "  Linux: apt install build-essential"; exit 1; }
	@command -v curl >/dev/null 2>&1 \
	    && echo "curl (CLI): OK" \
	    || { echo "FEHLT: curl (zum Holen von stb_image.h)."; exit 1; }
	@printf '#include <curl/curl.h>\nint main(void){return 0;}\n' \
	    | $(CC) -x c - $(LDLIBS) -o /dev/null 2>/dev/null \
	    && echo "libcurl (Header+Lib): OK" \
	    || { echo "FEHLT: libcurl-Entwicklungsdateien."; \
	         echo "  macOS: xcode-select --install  (brew NICHT noetig)"; \
	         echo "  Linux: apt install libcurl4-openssl-dev"; exit 1; }
	@echo "== Preflight OK =="

stb_image.h:
	curl -fsSL -o $@ $(STB_URL)

clean:
	rm -f dd

.PHONY: check clean
