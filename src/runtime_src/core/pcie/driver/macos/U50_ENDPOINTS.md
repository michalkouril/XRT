# Alveo U50 `gen3x16-xdma` base shell: endpoint map

Transcribed from `partition_metadata.json` of `xilinx-u50-gen3x16-xdma-base
5-3499627`, as installed on ace at
`/lib/firmware/xilinx/4465409525b4c06aec6d0b479d3febe8/`. That directory is
named by the shell's logic uuid, and the U50 in the Mac's enclosure reports
the same uuid from its UUID ROM, so this describes the card as flashed.

- logic uuid: `4465409525b4c06aec6d0b479d3febe8`
- interface uuid: `16e2362f82d2feab35529da27134b76d`
  (matches `verify.xclbin` in `xilinx-u50-gen3x16-xdma-validate`, so that
  xclbin loads on this card without reflashing)
- schema version: 0x1.0x0

PF 0 is the management function, PF 1 the user function. `offset` is within
the named BAR, except where the abstraction says otherwise - see the caution
at the end.


## PF 0x0, BAR 0x0

| endpoint | offset | size | abstraction |
|---|---|---|---|
| `ep_ert_reset_00` | `0x1000` | `0x1000` | `gpio_aresetn:1.0` |
| `ep_intc_ert_00` | `0x2000` | `0x1000` | `axi_intc:1.0` |
| `ep_ert_debug_uart_00` | `0x3000` | `0x1000` | `axi_uartlite:1.0` |
| `ep_ert_firmware_mem_00` | `0x10000` | `0x8000` | `axi_bram_ctrl:1.0` |
| `ep_pr_isolate_ulp_00` | `0x41000` | `0x1000` | `pr_isolate:1.0` |
| `ep_firewall_ctrl_mgmt_00` | `0x42000` | `0x1000` | `axi_firewall:1.1` |
| `ep_firewall_ctrl_user_00` | `0x43000` | `0x1000` | `axi_firewall:1.1` |
| `ep_firewall_ctrl_debug_00` | `0x44000` | `0x1000` | `axi_firewall:1.1` |
| `ep_firewall_data_h2c_00` | `0x45000` | `0x1000` | `axi_firewall:1.1` |
| `ep_firewall_data_c2h_00` | `0x48000` | `0x1000` | `axi_firewall:1.1` |
| `ep_ddr_mem_calib_00` | `0x63000` | `0x1000` | `cmp_ddr_calib:1.0` |
| `ep_intc_aggregated_mgmt_00` | `0x81000` | `0x1000` | `axi_intc:1.0` |
| `ep_cmc_reset_00` | `0x1e01000` | `0x1000` | `gpio_aresetn:1.0` |
| `ep_cmc_mutex_00` | `0x1e02000` | `0x1000` | `gpio_mutex:1.0` |
| `ep_intc_cmc_00` | `0x1e03000` | `0x1000` | `axi_intc:1.0` |
| `ep_cmc_regmap_00` | `0x1e08000` | `0x2000` | `axi_bram_ctrl:1.0` |
| `ep_cmc_firmware_mem_00` | `0x1e20000` | `0x20000` | `axi_bram_ctrl:1.0` |
| `ep_icap_arb_00` | `0x1e81000` | `0x1000` | `icap_arb:1.0` |
| `ep_fpga_configuration_00` | `0x1e88000` | `0x8000` | `axi_hwicap:1.0` |
| `ep_debug_bscan_mgmt_00` | `0x1e90000` | `0x10000` | `debug_bridge:1.0` |
| `ep_firewall_blp_ctrl_mgmt_00` | `0x1f02000` | `0x1000` | `axi_firewall:1.1` |
| `ep_firewall_blp_ctrl_user_00` | `0x1f03000` | `0x1000` | `axi_firewall:1.1` |
| `ep_uuid_rom_blp_00` | `0x1f04000` | `0x1000` | `uuid_rom:1.0` |
| `ep_pcie_link_mon_00` | `0x1f05000` | `0x1000` | `gpio_pcie_link_mon:1.0` |
| `ep_card_flash_program_00` | `0x1f06000` | `0x1000` | `axi_quad_spi:1.0` |
| `ep_icap_reset_00` | `0x1f07000` | `0x1000` | `gpio_aresetn:1.0` |
| `ep_bar_layout_mgmt_00` | `0x1f08000` | `0x1000` | `bar_layout:1.0` |
| `ep_platform_info_mgmt_00` | `0x1f09000` | `0x1000` | `platform_info:1.0` |
| `ep_debug_uptime_counters_00` | `0x1f0c000` | `0x1000` | `debug_uptime_counters:1.0` |
| `ep_mailbox_mgmt_00` | `0x1f10000` | `0x10000` | `mailbox:1.0` |

## PF 0x0, BAR 0x2

| endpoint | offset | size | abstraction |
|---|---|---|---|
| `ep_msix_mgmt_00` | `0x0` | `0x40000` | `msix:1.0` |

## PF 0x1, BAR 0x0

| endpoint | offset | size | abstraction |
|---|---|---|---|
| `ep_intc_cu_00` | `0x0` | `0x1000` | `axi_intc:1.0` |
| `ep_c2h_data_00` | `0x0` | `0x400000000` | `memory:1.0` |
| `ep_intc_cu_01` | `0x1000` | `0x1000` | `axi_intc:1.0` |
| `ep_intc_cu_02` | `0x2000` | `0x1000` | `axi_intc:1.0` |
| `ep_intc_cu_03` | `0x3000` | `0x1000` | `axi_intc:1.0` |
| `ep_ert_config_00` | `0x4000` | `0x1000` | `gpio_ert_config:1.0` |
| `ep_ert_sched_00` | `0x10000` | `0x10000` | `shell_utils_embedded_scheduler_hw:1.0` |
| `ep_remap_p2p_00` | `0x80000` | `0x1000` | `shell_util_addr_remap:1.0` |
| `ep_remap_data_c2h_00` | `0x81000` | `0x1000` | `shell_util_addr_remap:1.0` |
| `ep_mailbox_user_00` | `0x1f00000` | `0x10000` | `mailbox:1.0` |
| `ep_bar_layout_user_00` | `0x1f10000` | `0x1000` | `bar_layout:1.0` |
| `ep_platform_info_user_00` | `0x1f11000` | `0x1000` | `platform_info:1.0` |

## PF 0x1, BAR 0x2

| endpoint | offset | size | abstraction |
|---|---|---|---|
| `ep_xdma_00` | `0x0` | `0x40000` | `xdma:1.0` |

## PF 0x1, BAR 0x4

| endpoint | offset | size | abstraction |
|---|---|---|---|
| `ep_p2p_00` | `0x0` | `0x200200000` | `p2p:1.0` |
| `ep_ert_command_queue_user_00` | `0x8000000000` | `0x10000` | `ert_command_queue:1.0` |

## Cautions

- `ep_c2h_data_00` (PF 1, BAR 0, offset 0, range `0x400000000`) overlaps
  `ep_intc_cu_00` and is far larger than BAR0, which the card presents as
  32 MiB. Its abstraction is `memory:1.0`: it is an AXI address for the C2H
  data path target, not a BAR window. Check the abstraction before treating a
  row as an aperture.
- PF 1 BAR 4 holds `ep_p2p_00` (8 GiB) and `ep_ert_command_queue_user_00` at
  `0x8000000000`. The card presents BAR4 as 256 MiB, so these offsets assume
  the resizable P2P BAR has been expanded. **The ERT command queue is not
  reachable at this offset at the default BAR size.** The macOS port drives CU
  registers directly and does not use ERT, so this costs it nothing today.
- `ep_bar_layout_user_00` (`0x1f10000`) and `ep_platform_info_user_00`
  (`0x1f11000`) are the same two entries `tools/vsec_peek.cpp` reads out of
  the PCIe VSEC table on the live card, which cross-checks probe against
  package.

