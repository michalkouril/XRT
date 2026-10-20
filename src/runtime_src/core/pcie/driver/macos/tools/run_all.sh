#!/bin/bash
source /opt/xilinx/xrt/setup.sh > /dev/null 2>&1
K=/opt/xilinx/dsa/xilinx_vcu1525_xdma_201830_1/test/verify.xclbin
cd ~/xrt-tests
run() {
  local name=$1; shift
  perl -e "alarm 600; exec @ARGV" "$@" > $name.log 2>&1
  echo "$name exit=$?" >> results.txt
}
rm -f results.txt
run 00_hello ./00_hello -k $K -c hello
run 22_verify ./22_verify -k $K
run query ./query -d 0
run 56_xclbin ./56_xclbin -k $K
run perf_IOPS ./perf_IOPS -k $K
run 102_multiproc_verify ./102_multiproc_verify -k $K
echo done >> results.txt
