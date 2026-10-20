#!/bin/bash
source /opt/xilinx/xrt/setup.sh > /dev/null 2>&1
K=/opt/xilinx/dsa/xilinx_vcu1525_xdma_201830_1/test/verify.xclbin
step() { echo "=== $(date +%T) $*"; }
step "1. before sleep"
~/src/hello_installed $K 1 | tail -1
~/src/sec_peek | head -1
step "2. sleep with scheduled wake"
W=$(date -v+120S "+%m/%d/%Y %H:%M:%S")
sudo -n pmset -a disablesleep 0
sudo -n pmset schedule wake "$W"
echo "wake scheduled for $W"
sudo -n pmset sleepnow
sleep 200
step "3. after wake"
sudo -n pmset -a disablesleep 1
pmset -g | grep -i sleepdisabled
pmset -g log | grep -E " Sleep | Wake " | tail -3 | cut -c1-120
sudo -n log show --last 8m --style compact --predicate "eventMessage CONTAINS \"xrt_user: sleep\" OR eventMessage CONTAINS \"xrt_user: wake\"" | grep -v "^Timestamp" | cut -c1-200
ioreg -c IOPCIDevice -r -d 1 | grep -o "pci10ee,[0-9a-f]*@[0-9a-f,]*" | sort -u | tr "\n" " "; echo
~/src/sec_peek | head -1
~/src/cu_peek 0x1800000
echo "--- xrt-smi after wake (expect no xclbin):"; xrt-smi examine -d 0000:03:00.1 -r dynamic-regions --batch 2>&1 | grep -iE "uuid|No xclbin|status" | head -3
~/src/fw_peek | tail -1
~/src/xdma_peek | grep -E "H2C0|C2H0"
step "4. run after wake"
~/src/hello_installed $K 20 | tail -2
xrt-smi validate -d 0000:03:00.1 -r dma --batch 2>&1 | tr -d "\033" | grep -E "Test Status"
step done
