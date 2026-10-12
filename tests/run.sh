#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I../main ../main/shaker.cpp ../main/five.cpp ../main/six.cpp ../main/analog_dsp.cpp test_shaker.cpp -o build/test_shaker
./build/test_shaker "$@"
