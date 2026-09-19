# Sidera 构建（宿主机 amd64；aarch64 见 README / 手写交叉命令）
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -fPIC
PKGS      = Qt5Core Qt5Gui Qt5Widgets Qt5Network
QTFLAGS   = $(shell pkg-config --cflags $(PKGS))
QTLIBS    = $(shell pkg-config --libs $(PKGS))
LIBS      = -lX11 -lxcb -lXext -lXtst
SRC       = $(wildcard src/*.cpp)
HDR       = $(wildcard src/*.h)

all: annotate_amd64

annotate_amd64: $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) $(QTFLAGS) $(SRC) $(QTLIBS) $(LIBS) -o $@

clean:
	rm -f annotate_amd64 annotate_aarch64

test:
	python3 tests/test_bridge_api.py

.PHONY: all clean test
