all: check example.o example

CFLAGS= -Wall
CC=mpicc

check: check-env
	@echo CODESBASE: $(CODESBASE)
	@echo CODESNET: $(CODESNET)
	@echo ROSS: $(ROSS)
	
example.o: example.c
	$(CC) -c $(CFLAGS) -I$(ROSS)/include -I$(CODESBASE)/include -I$(CODESNET)/include example.c 

example: example.o
	$(CC) $(CFLAGS) example.o -o example -L$(ROSS)/lib -lROSS -lm -L$(CODESBASE)/lib -lcodes-base -L$(CODESNET)/lib -lcodes-net

check-env:
ifndef CODESBASE 
	$(error CODESBASE is undefined, see README.txt)
endif
ifndef CODESNET 
	$(error CODESNET is undefined, see README.txt)
endif
ifndef ROSS
	$(error ROSS is undefined, see README.txt)
endif
