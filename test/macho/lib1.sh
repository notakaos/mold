#!/bin/bash
export LC_ALL=C
set -e
CC="${CC:-cc}"
CXX="${CXX:-c++}"
testname=$(basename "$0" .sh)
echo -n "Testing $testname ... "
cd "$(dirname "$0")"/../..
mold="$(pwd)/ld64.mold"
t=out/test/macho/$testname
mkdir -p $t

cat <<EOF | $CC -o $t/libfoo.dylib -shared -xc -
#include <stdio.h>
void hello() {
  printf("Hello world\n");
}
EOF

cat <<EOF | $CC -o $t/a.o -c -xc -
int main() {}
EOF

clang -fuse-ld="$mold" -o $t/exe $t/a.o -L$t -lfoo
$t/exe

echo OK
