CXX = g++
CXXFLAGS = -O2 -W -Wall `pkg-config gtk+-2.0 --cflags`
LDFLAGS = `pkg-config gtk+-2.0 --libs`
OBJ = main.o

all: japlay

%.o:	%.cc
	$(CXX) $(CXXFLAGS) -c $<

japlay:	$(OBJ)
	$(CXX) $(OBJ) -o japlay $(LDFLAGS)
