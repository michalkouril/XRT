#!/bin/bash
source /opt/xilinx/xrt/setup.sh > /dev/null 2>&1
K=/opt/xilinx/dsa/xilinx_vcu1525_xdma_201830_1/test/verify.xclbin
step() { echo "=== $(date +%T) $*"; }
step "1. before reset"
~/src/hello_installed $K 1 | tail -1
~/src/fw_peek | tail -1
step "2. xbmgmt reset --type hot"
sudo -n perl -e "alarm 120; exec @ARGV" /opt/xilinx/xrt/bin/xbmgmt reset --device 0000:03:00.0 --type hot --force 2>&1 | tail -4
echo "reset exit=$?"
sleep 3
step "3. after reset"
ioreg -c IOPCIDevice -r -d 1 | grep -o "pci10ee,[0-9a-f]*@[0-9a-f,]*" | sort -u | tr "\n" " "; echo
/opt/xilinx/xrt/bin/unwrapped/xrt_dext_probe | grep -E "^abi|command|VBNV|SYSMON|CLOCKS"
~/src/fw_peek | tail -1
~/src/xdma_peek | grep -E "H2C0|C2H0"
~/src/cu_peek 0x1800000
step "4. reload and run"
~/src/hello_installed $K 20 | tail -2
xrt-smi validate -d 0000:03:00.1 -r dma --batch 2>&1 | tr -d "\033" | grep -E "Test Status" 
~/src/fw_peek | tail -1
step done
