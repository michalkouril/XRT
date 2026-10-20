// SPDX-License-Identifier: Apache-2.0
//
// Interface to the ConnectX-5 half of an Innova-2, for reading the ConnectX's view of
// which flash image the FPGA booted. Separate from xrt_dext_abi.h on purpose: this is a
// different device with a different vendor, and folding it in would tie its selector
// numbering to the FPGA ABI's churn.
//
// WHAT THIS IS FOR. The FPGA's boot chain ends with the Flex image deciding, from a code
// the ConnectX drives on pin E3, whether to stay resident or hop to the User image
// (HANDOFF 89). Only the ConnectX knows the verdict, and it reports it through the
// FPGA_CTRL register as admin / oper / status. `innova2_app.py` reads that with no
// Mellanox driver, over the PCI Vendor-Specific Capability's gateway, which is what this
// reproduces.
//
// STATUS: the gateway below is implemented from mstflint's `pciconf` access method. The
// register offsets and the semaphore dance are TAKEN FROM THAT, NOT YET CONFIRMED on
// this device - at the time of writing Apple's AppleEthernetMLX5 owns both ConnectX
// functions and macOS refuses to unload a bootKC dext, so nothing here has executed
// against hardware. Treat every constant as a hypothesis until the log says otherwise.
#ifndef XRT_CX5_ABI_H
#define XRT_CX5_ABI_H

#include <stdint.h>

#define XRT_CX5_ABI_VERSION      1
#define XRT_CX5_CLASS            "XrtCx5"

enum xrt_cx5_selector {
  XRT_CX5_GET_INFO      = 0,  // out: struct xrt_cx5_info
  XRT_CX5_GATEWAY_READ  = 1,  // scalar in: address space, address;
                              // scalar out: the dword read
  XRT_CX5_GATEWAY_WRITE = 2,  // scalar in: address space, address, value
  XRT_CX5_SELECTOR_COUNT
};

// Address spaces reachable through the gateway (mstflint's AS_* values). Only the two
// this needs are named; the gateway itself refuses anything else, because a space that
// has not been thought about is not one to hand to a caller.
#define XRT_CX5_AS_CR_SPACE      0x02   /* the device's CR space, which on an Innova-2
                                         * carries the FPGA's own register map:
                                         * image version/date, temperature, power, fan */
#define XRT_CX5_AS_ICMD          0x03   /* the ICMD mailbox, the route to ACCESS_REG and
                                         * therefore to FPGA_CTRL. Not yet used. */

// Offsets WITHIN the Vendor-Specific Capability, from mstflint's pciconf layer.
#define XRT_CX5_VSC_CTRL         0x04   /* bits 0-15 address space; 29-31 status  */
#define XRT_CX5_VSC_COUNTER      0x08   /* read to obtain a semaphore ticket      */
#define XRT_CX5_VSC_SEMAPHORE    0x0c   /* write the ticket, read it back to win  */
#define XRT_CX5_VSC_ADDR         0x10   /* address; bit 31 = operation in flight  */
#define XRT_CX5_VSC_DATA         0x14   /* the dword                              */

#define XRT_CX5_VSC_CTRL_STATUS_SHIFT  29
#define XRT_CX5_VSC_CTRL_STATUS_MASK   0x7
#define XRT_CX5_VSC_STATUS_OK          0x1   /* the space was accepted            */
#define XRT_CX5_VSC_ADDR_BUSY          (1u << 31)

struct xrt_cx5_info {
  uint32_t abi;              /* XRT_CX5_ABI_VERSION                              */
  uint16_t vendor;           /* expect 0x15b3                                    */
  uint16_t device;           /* expect 0x1017 for a ConnectX-5                   */
  uint8_t  bus, dev, func;
  uint8_t  reserved;
  uint32_t vsc_offset;       /* the legacy Vendor-Specific Capability, 0 if absent
                              * - and if it is 0 nothing else here can work      */
  uint32_t vsec_offset;      /* the extended VSEC (0x000b), informational        */
  uint32_t gateway_ok;       /* 1 once a gateway read has actually succeeded     */
};

#endif
