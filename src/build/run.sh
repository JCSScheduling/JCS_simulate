if [ -x "CMakeCache.txt" ]; then
    rm CMakeCache.txt
fi
if [ -x "simulate" ]; then
    rm simulate
fi

# cmake .
cmake  ..
echo "=====generate Makefile success!====="
make
echo "=====generate binary file success!====="

echo "===============pcie 128-82 starts============="

echo "prio"
./simulate ../platform/pcie-128-mix.xml ../deploy/deploy-128-82-random.xml prio prio "--log=root.fmt:[%10.6r]%e(%i:%P@%h)%e%m%n"

echo "=====simulate finished!====="
