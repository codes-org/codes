
CXX=g++
CXXFLAGS=-g -Wall -O2 -std=c++11 $(shell pkg-config --cflags libzmq) -Wdeprecated-declarations
LDFLAGS=$(shell pkg-config --libs libzmq) -lm
TARGETS=libzmqmlrequester.so demozmqmlrequester

all: $(TARGETS)

libzmqmlrequester.so: zmqmlrequester.o
	$(CXX) -shared -o $@ $^

zmqmlrequester.o: zmqmlrequester.cpp zmqmlrequester.h
	$(CXX) $(CXXFLAGS) -fPIC -c $< -o $@

demozmqmlrequester: demozmqmlrequester.cpp libzmqmlrequester.so
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) -L./ -lzmqmlrequester

# doxygen
# sphinx

clean:
	rm -f $(TARGETS)
	rm -f *.o
	rm -f tmptestsend.dat

distclean: clean
	rm -f *~
