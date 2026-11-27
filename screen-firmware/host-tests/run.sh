#!/bin/sh
# Build and run the PC tests of the lathe screen's core logic (needs g++ or clang++).
set -e
cd "$(dirname "$0")"
CXX="${CXX:-g++}"
"$CXX" -std=c++17 -Wall -Wextra -O1 -g -I. -I../fluidnc/FluidNC/lathe \
    test_main.cpp ../fluidnc/FluidNC/lathe/core/*.cpp -o lathe_tests
./lathe_tests
