CXX ?= clang++
CXXFLAGS ?= -O2 -g -o $@ `llvm-config --cxxflags`


parser: lang.cpp
	$(CXX) $(CXXFLAGS) $<

