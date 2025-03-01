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

cat <<EOF | $CC -o $t/a.o -c -xc -
#include <stdio.h>
int main() {
  printf("Hello world\n");
}
EOF

$CC -B. -o $t/exe $t/a.o -Wl,-no-threads
$CC -B. -o $t/exe $t/a.o -Wl,-thread-count=1
$CC -B. -o $t/exe $t/a.o -Wl,-threads
$CC -B. -o $t/exe $t/a.o -Wl,-threads=1
$CC -B. -o $t/exe $t/a.o -Wl,--threads=1

echo OK
