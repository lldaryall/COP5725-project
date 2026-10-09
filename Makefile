# Plain-make build (no CMake needed). CMakeLists.txt builds the same targets.
#   make            -> build/bench, build/tests
#   make test       -> build and run tests
#   make check      -> run tests under ASan + UBSan
#   make NATIVE=1   -> tune for the local CPU
#   make reference  -> build/bench_ref with the authors' ALEX as --index alexref
#                      (run scripts/fetch_alex_reference.sh first; on Apple
#                      Silicon this builds for x86_64 and runs under Rosetta)

# CXXFLAGS is for the user (e.g. make CXXFLAGS="-O2 -g"); the flags the
# build needs live in BASE_FLAGS so overriding CXXFLAGS cannot drop them.
CXX      ?= c++
CXXFLAGS ?= -O3 -DNDEBUG
BASE_FLAGS := -std=c++17 -Wall -Wextra -Iinclude -Ithird_party/tlx
ifeq ($(NATIVE),1)
  ifneq ($(filter arm64 aarch64,$(shell uname -m)),)
    BASE_FLAGS += -mcpu=native
  else
    BASE_FLAGS += -march=native
  endif
endif
FLAGS = $(BASE_FLAGS) $(CXXFLAGS)

HEADERS := $(wildcard include/*.h)
TLX_OBJ := build/tlx_die_core.o

all: build/bench build/tests

build:
	mkdir -p build

$(TLX_OBJ): third_party/tlx/tlx/die/core.cpp | build
	$(CXX) $(FLAGS) -c $< -o $@

build/bench: src/bench.cpp $(HEADERS) $(TLX_OBJ) | build
	$(CXX) $(FLAGS) $< $(TLX_OBJ) -o $@

build/tests: tests/test_main.cpp $(HEADERS) $(TLX_OBJ) | build
	$(CXX) $(FLAGS) $< $(TLX_OBJ) -o $@

# The reference ALEX needs x86 intrinsics. Everything in bench_ref is built
# for the same architecture so the comparison stays like-for-like.
REF_ARCH := $(if $(filter arm64,$(shell uname -m)),-arch x86_64 -mpopcnt,$(if $(filter x86_64,$(shell uname -m)),-mpopcnt,))
REF_FLAGS = $(filter-out -mcpu=native -march=native,$(FLAGS)) $(REF_ARCH) \
             -Ithird_party -DCOP5725_ALEX_REF -Wno-unused-parameter -Wno-sign-compare \
             -Wno-unused-variable -Wno-unused-but-set-variable -Wno-deprecated-declarations -Wno-logical-op-parentheses

reference: build/bench_ref

build/bench_ref: src/bench.cpp src/alex_reference.h $(HEADERS) | build
	@test -d third_party/ALEX || { echo "run scripts/fetch_alex_reference.sh first"; exit 1; }
	$(CXX) $(REF_FLAGS) $< third_party/tlx/tlx/die/core.cpp -o $@

test: build/tests
	./build/tests

# Tests under AddressSanitizer + UndefinedBehaviorSanitizer.
check: build
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Iinclude -Ithird_party/tlx tests/test_main.cpp third_party/tlx/tlx/die/core.cpp \
	  -o build/tests_asan
	./build/tests_asan

clean:
	rm -rf build

.PHONY: all test check clean reference
