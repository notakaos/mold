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
int foo();

int main() {
  foo();
}
EOF

! "$mold" -o $t/exe $t/a.o 2> $t/log || false
grep -q 'undefined symbol: .*\.o: foo' $t/log

echo OK
