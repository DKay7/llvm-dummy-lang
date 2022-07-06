CXX = clang++
CXXFLAGS = -O2 -g `llvm-config --cxxflags --ldflags --system-libs --libs core` -o $@ 

lang: lang.cpp
	$(CXX)  $< $(CXXFLAGS)

