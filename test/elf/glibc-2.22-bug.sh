#!/bin/bash
export LC_ALL=C
set -e
CC="${CC:-cc}"
CXX="${CXX:-c++}"
testname=$(basename "$0" .sh)
echo -n "Testing $testname ... "
cd "$(dirname "$0")"/../..
mold="$(pwd)/mold"
t=out/test/elf/$testname
mkdir -p $t

# glibc 2.22 or prior have a bug that ld-linux.so.2 crashes on dlopen()
# if .rela.dyn and .rela.plt are not contiguous in a given DSO.
# This test verifies that these sections are contiguous in mold's output.

cat <<EOF | $CC -o $t/a.o -fPIC -c -xc -
#include <stdio.h>
int main() {
  printf("Hello world\n");
}
EOF

$CC -B. -o $t/b.so -shared $t/a.o
readelf -W --sections $t/b.so | fgrep -A1 .rela.dyn | fgrep -q .rela.plt

echo OK
