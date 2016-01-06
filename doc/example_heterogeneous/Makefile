ifndef CODESBASE
$(error CODESBASE is undefined, see README.txt)
endif
ifndef CODESNET
$(error CODESNET is undefined, see README.txt)
endif
ifndef ROSS
$(error ROSS is undefined, see README.txt)
endif

PKG_PATH = $(ROSS)/lib/pkgconfig:$(CODESBASE)/lib/pkgconfig:$(CODESNET)/lib/pkgconfig

override CPPFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_PATH) pkg-config --cflags codes-net codes-base ross)
LDFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_PATH) pkg-config --libs-only-L ross codes-net codes-base ross)
LDLIBS = $(shell PKG_CONFIG_PATH=$(PKG_PATH) pkg-config --libs-only-l ross codes-net codes-base ross)
CC = mpicc

example: example.c

clean:
	rm -f example
