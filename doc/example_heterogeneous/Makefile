ifndef CODESBASE
$(error CODESBASE is undefined, see README.txt)
endif
ifndef CODESNET
$(error CODESNET is undefined, see README.txt)
endif
ifndef ROSS
$(error ROSS is undefined, see README.txt)
endif

# ross conflates CFLAGS with CPPFLAGS, so use just this one
override CPPFLAGS += $(shell $(ROSS)/bin/ross-config --cflags) -I$(CODESBASE)/include -I$(CODESNET)/include
CC = $(shell $(ROSS)/bin/ross-config --cc)
LDFLAGS = $(shell $(ROSS)/bin/ross-config --ldflags) -L$(CODESBASE)/lib -L$(CODESNET)/lib
LDLIBS = $(shell $(ROSS)/bin/ross-config --libs) -lcodes-net -lcodes-base 

example: example.c

clean:   
	rm -f example
