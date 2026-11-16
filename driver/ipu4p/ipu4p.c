// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013--2024 Intel Corporation
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/pci-ats.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <media/ipu-bridge.h>
/* IPU4P: single PCI id (Ice Lake ISP). Table defined below. */
#define IPU4P_PCI_ID	0x8a19
static const struct pci_device_id ipu4p_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, IPU4P_PCI_ID) },
	{ }
};

#include "ipu4p.h"
#include "ipu4p-bus.h"
#include "ipu4p-buttress.h"
#include "ipu4p-cpd.h"
#include "ipu4p-isys.h"
#include "ipu4p-mmu.h"
#include "ipu4p-platform-buttress-regs.h"
#include "ipu4p-platform-isys-csi2-reg.h"
#include "ipu4p-platform-regs.h"

#define IPU4P_PCI_BAR		0

struct ipu4p_cell_program {
	u32 magic_number;

	u32 blob_offset;
	u32 blob_size;

	u32 start[3];

	u32 icache_source;
	u32 icache_target;
	u32 icache_size;

	u32 pmem_source;
	u32 pmem_target;
	u32 pmem_size;

	u32 data_source;
	u32 data_target;
	u32 data_size;

	u32 bss_target;
	u32 bss_size;

	u32 cell_id;
	u32 regs_addr;

	u32 cell_pmem_data_bus_address;
	u32 cell_dmem_data_bus_address;
	u32 cell_pmem_control_bus_address;
	u32 cell_dmem_control_bus_address;

	u32 next;
	u32 dummy[2];
};

/*
 * IPU4P (Ice Lake) layout, from intel/linux-intel-lts 4.19 ipu4/ipu4.c
 * and ipu4/ipu-platform-regs.h (CONFIG_VIDEO_INTEL_IPU4P section).
 */
#define IPU4P_ICL_ISYS_OFFSET		0x00100000
#define IPU4P_ICL_PSYS_OFFSET		0x00400000
#define IPU4P_ICL_ISYS_IOMMU0_OFFSET	0x000e0000
#define IPU4P_ICL_ISYS_IOMMU1_OFFSET	0x000e0100
#define IPU4P_ICL_PSYS_IOMMU0_OFFSET	0x000b0000
#define IPU4P_ICL_PSYS_IOMMU1_OFFSET	0x000b0100
#define IPU4P_ICL_PSYS_IOMMU1R_OFFSET	0x000b0600
#define IPU4P_ICL_DMEM_OFFSET		0x008000
#define IPU4P_ICL_SPC_OFFSET		0x000000
#define IPU4P_ICL_MMU_L1_SID_REG	0x0c
#define IPU4P_ICL_MMU_L2_SID_REG	0x4c
#define IPU4P_ICL_INFO_DEST_PRIMARY	BIT(4)
#define IPU4P_ICL_INFO_STREAM_ID_SET(a)	(((a) & 0xF) << 4)

static struct ipu4p_isys_internal_pdata isys_ipdata = {
	.hw_variant = {
		.offset = IPU4P_ICL_ISYS_OFFSET,
		.nr_mmus = 2,
		.mmu_hw = {
			{
				.offset = IPU4P_ICL_ISYS_IOMMU0_OFFSET,
				.info_bits = IPU4P_ICL_INFO_DEST_PRIMARY,
				.nr_l1streams = 0,
				.nr_l2streams = 0,
				.insert_read_before_invalidate = true,
			},
			{
				.offset = IPU4P_ICL_ISYS_IOMMU1_OFFSET,
				.info_bits = IPU4P_ICL_INFO_STREAM_ID_SET(0),
				.nr_l1streams = 16,
				.l1_block_sz = {
					5, 16, 6, 6, 6, 6, 6, 8, 0,
					0, 0, 0, 0, 0, 0, 5
				},
				.l1_zlw_en = {
					0, 1, 1, 1, 1, 1, 1, 1, 0, 0,
					0, 0, 0, 0, 0, 0
				},
				.l1_zlw_1d_mode = {
					0, 1, 1, 1, 1, 1, 1, 1, 0,
					0, 0, 0, 0, 0, 0, 0
				},
				.l1_ins_zlw_ahead_pages = {
					0, 3, 3, 3, 3, 3, 3, 3,
					0, 0, 0, 0, 0, 0, 0, 0
				},
				.l1_zlw_2d_mode = {
					0, 0, 0, 0, 0, 0, 0, 0, 0,
					0, 0, 0, 0, 0, 0, 0
				},
				.nr_l2streams = 16,
				.l2_block_sz = {
					2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
					2, 2, 2, 2, 2, 2
				},
				.insert_read_before_invalidate = false,
				.l1_stream_id_reg_offset =
					IPU4P_ICL_MMU_L1_SID_REG,
				.l2_stream_id_reg_offset =
					IPU4P_ICL_MMU_L2_SID_REG,
			},
		},
		.cdc_fifos = 0,
		.dmem_offset = IPU4P_ICL_DMEM_OFFSET,
		.spc_offset = IPU4P_ICL_SPC_OFFSET,
	},
	.isys_dma_overshoot = IPU4P_ISYS_OVERALLOC_MIN,
};

static struct ipu4p_psys_internal_pdata psys_ipdata = {
	.hw_variant = {
		.offset = IPU4P_ICL_PSYS_OFFSET,
		.nr_mmus = 3,
		.mmu_hw = {
			{
				.offset = IPU4P_ICL_PSYS_IOMMU0_OFFSET,
				.info_bits = IPU4P_ICL_INFO_DEST_PRIMARY,
				.nr_l1streams = 0,
				.nr_l2streams = 0,
				.insert_read_before_invalidate = true,
			},
			{
				.offset = IPU4P_ICL_PSYS_IOMMU1_OFFSET,
				.info_bits = IPU4P_ICL_INFO_STREAM_ID_SET(0),
				.nr_l1streams = 16,
				.l1_block_sz = {
					2, 5, 4, 2, 2, 10, 5, 16, 10,
					5, 0, 0, 0, 0, 0, 3
				},
				.l1_zlw_en = {
					0, 0, 1, 1, 1, 1, 1, 1, 1, 1,
					0, 0, 0, 0, 0, 0
				},
				.l1_zlw_1d_mode = {
					0, 0, 1, 1, 1, 1, 1, 1, 1,
					1, 0, 0, 0, 0, 0, 0
				},
				.l1_ins_zlw_ahead_pages = {
					0, 0, 3, 3, 3, 3, 3, 3, 3, 3,
					0, 0, 0, 0, 0, 0
				},
				.l1_zlw_2d_mode = {
					0, 0, 0, 0, 0, 0, 0, 0, 0,
					0, 0, 0, 0, 0, 0, 0
				},
				.nr_l2streams = 16,
				.l2_block_sz = {
					2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
					2, 2, 2, 2, 2, 2
				},
				.insert_read_before_invalidate = false,
				.l1_stream_id_reg_offset =
					IPU4P_ICL_MMU_L1_SID_REG,
				.l2_stream_id_reg_offset =
					IPU4P_ICL_MMU_L2_SID_REG,
			},
			{
				.offset = IPU4P_ICL_PSYS_IOMMU1R_OFFSET,
				.info_bits = IPU4P_ICL_INFO_STREAM_ID_SET(0),
				.nr_l1streams = 16,
				.l1_block_sz = {
					2, 6, 5, 16, 16, 8, 8, 0, 0,
					0, 0, 0, 0, 0, 0, 3
				},
				.l1_zlw_en = {
					0, 0, 1, 1, 0, 0, 0, 0, 0, 0,
					0, 0, 0, 0, 0, 0
				},
				.l1_zlw_1d_mode = {
					0, 0, 1, 1, 0, 0, 0, 0, 0,
					0, 0, 0, 0, 0, 0, 0
				},
				.l1_ins_zlw_ahead_pages = {
					0, 0, 3, 3, 0, 0, 0, 0,
					0, 0, 0, 0, 0, 0, 0, 0
				},
				.l1_zlw_2d_mode = {
					0, 0, 0, 0, 0, 0, 0, 0, 0,
					0, 0, 0, 0, 0, 0, 0
				},
				.nr_l2streams = 16,
				.l2_block_sz = {
					2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
					2, 2, 2, 2, 2, 2
				},
				.insert_read_before_invalidate = false,
				.l1_stream_id_reg_offset =
					IPU4P_ICL_MMU_L1_SID_REG,
				.l2_stream_id_reg_offset =
					IPU4P_ICL_MMU_L2_SID_REG,
			},
		},
		.dmem_offset = IPU4P_ICL_DMEM_OFFSET,
		.spc_offset = IPU4P_ICL_SPC_OFFSET,
	},
};

static const struct ipu4p_buttress_ctrl isys_buttress_ctrl = {
	.ratio = IPU4P_IS_FREQ_CTL_DIVISOR,
	.ratio_shift = IPU4P_IS_FREQ_CTL_RATIO_SHIFT,
	.qos_floor = 0,
	.ovrd = 0,
	.ovrd_shift = 0,
	.freq_ctl = IPU4P_BUTTRESS_REG_IS_FREQ_CTL,
	.pwr_sts_shift = IPU4P_BUTTRESS_PWR_STATE_IS_PWR_FSM_SHIFT,
	.pwr_sts_mask = IPU4P_BUTTRESS_PWR_STATE_IS_PWR_FSM_MASK,
	.pwr_sts_on = IPU4P_BUTTRESS_PWR_STATE_IS_PWR_FSM_IS_RDY,
	.pwr_sts_off = IPU4P_BUTTRESS_PWR_STATE_IS_PWR_FSM_IDLE,
};

static const struct ipu4p_buttress_ctrl psys_buttress_ctrl = {
	.ratio = IPU4P_PS_FREQ_CTL_DEFAULT_RATIO,
	.ratio_shift = IPU4P_PS_FREQ_CTL_RATIO_SHIFT,
	.qos_floor = IPU4P_PS_FREQ_CTL_DEFAULT_RATIO,
	.ovrd = 1,
	.ovrd_shift = IPU4P_PS_FREQ_CTL_OVRD_SHIFT,
	.freq_ctl = IPU4P_BUTTRESS_REG_PS_FREQ_CTL,
	.pwr_sts_shift = IPU4P_BUTTRESS_PWR_STATE_PS_PWR_FSM_SHIFT,
	.pwr_sts_mask = IPU4P_BUTTRESS_PWR_STATE_PS_PWR_FSM_MASK,
	.pwr_sts_on = IPU4P_BUTTRESS_PWR_STATE_PS_PWR_FSM_PS_PWR_UP,
	.pwr_sts_off = IPU4P_BUTTRESS_PWR_STATE_PS_PWR_FSM_IDLE,
};

static void
ipu4p_pkg_dir_configure_spc(struct ipu4p_device *isp,
			   const struct ipu4p_hw_variants *hw_variant,
			   int pkg_dir_idx, void __iomem *base,
			   u64 *pkg_dir, dma_addr_t pkg_dir_vied_address)
{
	struct ipu4p_cell_program *prog;
	void __iomem *spc_base;
	u32 server_fw_addr;
	dma_addr_t dma_addr;
	u32 pg_offset;

	server_fw_addr = lower_32_bits(*(pkg_dir + (pkg_dir_idx + 1) * 2));
	if (pkg_dir_idx == IPU4P_CPD_PKG_DIR_ISYS_SERVER_IDX)
		dma_addr = sg_dma_address(isp->isys->fw_sgt.sgl);
	else
		dma_addr = sg_dma_address(isp->psys->fw_sgt.sgl);

	pg_offset = server_fw_addr - dma_addr;
	prog = (struct ipu4p_cell_program *)((uintptr_t)isp->cpd_fw->data +
					    pg_offset);
	spc_base = base + prog->regs_addr;
	if (spc_base != (base + hw_variant->spc_offset))
		dev_warn(&isp->pdev->dev,
			 "SPC reg addr %p not matching value from CPD %p\n",
			 base + hw_variant->spc_offset, spc_base);
	writel(server_fw_addr + prog->blob_offset +
	       prog->icache_source, spc_base + IPU4P_PSYS_REG_SPC_ICACHE_BASE);
	writel(IPU4P_INFO_REQUEST_DESTINATION_IOSF,
	       spc_base + IPU4P_REG_PSYS_INFO_SEG_0_CONFIG_ICACHE_MASTER);
	writel(prog->start[1], spc_base + IPU4P_PSYS_REG_SPC_START_PC);
	writel(pkg_dir_vied_address, base + hw_variant->dmem_offset);
}

void ipu4p_configure_spc(struct ipu4p_device *isp,
			const struct ipu4p_hw_variants *hw_variant,
			int pkg_dir_idx, void __iomem *base, u64 *pkg_dir,
			dma_addr_t pkg_dir_dma_addr)
{
	void __iomem *dmem_base = base + hw_variant->dmem_offset;
	void __iomem *spc_regs_base = base + hw_variant->spc_offset;
	u32 val;

	val = readl(spc_regs_base + IPU4P_PSYS_REG_SPC_STATUS_CTRL);
	val |= IPU4P_PSYS_SPC_STATUS_CTRL_ICACHE_INVALIDATE;
	writel(val, spc_regs_base + IPU4P_PSYS_REG_SPC_STATUS_CTRL);

	if (isp->secure_mode)
		writel(IPU4P_PKG_DIR_IMR_OFFSET, dmem_base);
	else
		ipu4p_pkg_dir_configure_spc(isp, hw_variant, pkg_dir_idx, base,
					   pkg_dir, pkg_dir_dma_addr);
}
EXPORT_SYMBOL_NS_GPL(ipu4p_configure_spc, "INTEL_IPU4P");

#define IPU4P_ISYS_CSI2_NPORTS		4
#define IPU4PSE_ISYS_CSI2_NPORTS		4
#define IPU4P_TGL_ISYS_CSI2_NPORTS	8
#define IPU4PEP_MTL_ISYS_CSI2_NPORTS	6

static void ipu4p_internal_pdata_init(struct ipu4p_device *isp)
{
	/* IPU4P (Ice Lake): one variant, values from the 4.19 tree */
	isys_ipdata.num_parallel_streams = IPU4P_ISYS_NUM_STREAMS;
	isys_ipdata.sram_gran_shift = IPU4P_SRAM_GRANULARITY_SHIFT;
	isys_ipdata.sram_gran_size = IPU4P_SRAM_GRANULARITY_SIZE;
	isys_ipdata.max_sram_size = IPU4P_MAX_SRAM_SIZE;
	isys_ipdata.sensor_type_start = IPU4P_FW_ISYS_SENSOR_TYPE_START;
	isys_ipdata.sensor_type_end = IPU4P_FW_ISYS_SENSOR_TYPE_END;
	isys_ipdata.max_streams = IPU4P_ISYS_NUM_STREAMS;
	isys_ipdata.max_send_queues = IPU4P_N_MAX_SEND_QUEUES;
	isys_ipdata.max_sram_blocks = IPU4P_NOF_SRAM_BLOCKS_MAX;
	isys_ipdata.max_devq_size = IPU4P_DEV_SEND_QUEUE_SIZE;
	/* 5 rx ports: s0p3, s1p0..s1p3 */
	isys_ipdata.csi2.nports = 5;
	/* IPU4: rx errors are the low 17 bits of the per-port ctrl0 block */
	isys_ipdata.csi2.irq_mask = GENMASK(16, 0);
	/*
	 * IPU4 has no separate "CSI top" irq controller. The csi2 summary
	 * bits live in the same UNISPART irq registers.
	 */
	isys_ipdata.csi2.ctrl0_irq_edge = IPU4P_REG_ISYS_UNISPART_IRQ_EDGE;
	isys_ipdata.csi2.ctrl0_irq_clear = IPU4P_REG_ISYS_UNISPART_IRQ_CLEAR;
	isys_ipdata.csi2.ctrl0_irq_mask = IPU4P_REG_ISYS_UNISPART_IRQ_MASK;
	isys_ipdata.csi2.ctrl0_irq_enable = IPU4P_REG_ISYS_UNISPART_IRQ_ENABLE;
	isys_ipdata.csi2.ctrl0_irq_status = IPU4P_REG_ISYS_UNISPART_IRQ_STATUS;
	isys_ipdata.csi2.ctrl0_irq_lnp =
		IPU4P_REG_ISYS_UNISPART_IRQ_LEVEL_NOT_PULSE;
	isys_ipdata.enhanced_iwake = false;
	psys_ipdata.hw_variant.spc_offset = IPU4P_ICL_SPC_OFFSET;
}

static struct ipu4p_bus_device *
ipu4p_isys_init(struct pci_dev *pdev, struct device *parent,
	       struct ipu4p_buttress_ctrl *ctrl, void __iomem *base,
	       const struct ipu4p_isys_internal_pdata *ipdata)
{
	struct device *dev = &pdev->dev;
	struct ipu4p_bus_device *isys_adev;
	struct ipu4p_isys_pdata *pdata;
	int ret;

	ret = ipu_bridge_init(dev, ipu_bridge_parse_ssdb);
	if (ret) {
		dev_err_probe(dev, ret, "IPU4P bridge init failed\n");
		return ERR_PTR(ret);
	}

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	pdata->base = base;
	pdata->ipdata = ipdata;

	isys_adev = ipu4p_bus_initialize_device(pdev, parent, pdata, ctrl,
					       IPU4P_ISYS_NAME);
	if (IS_ERR(isys_adev)) {
		kfree(pdata);
		return dev_err_cast_probe(dev, isys_adev,
				"ipu4p_bus_initialize_device isys failed\n");
	}

	isys_adev->mmu = ipu4p_mmu_init(dev, base, ISYS_MMID,
				       &ipdata->hw_variant);
	if (IS_ERR(isys_adev->mmu)) {
		put_device(&isys_adev->auxdev.dev);
		kfree(pdata);
		return dev_err_cast_probe(dev, isys_adev->mmu,
				"ipu4p_mmu_init(isys_adev->mmu) failed\n");
	}

	isys_adev->mmu->dev = &isys_adev->auxdev.dev;

	ret = ipu4p_bus_add_device(isys_adev);
	if (ret) {
		kfree(pdata);
		return ERR_PTR(ret);
	}

	return isys_adev;
}

static struct ipu4p_bus_device *
ipu4p_psys_init(struct pci_dev *pdev, struct device *parent,
	       struct ipu4p_buttress_ctrl *ctrl, void __iomem *base,
	       const struct ipu4p_psys_internal_pdata *ipdata)
{
	struct ipu4p_bus_device *psys_adev;
	struct ipu4p_psys_pdata *pdata;
	int ret;

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	pdata->base = base;
	pdata->ipdata = ipdata;

	psys_adev = ipu4p_bus_initialize_device(pdev, parent, pdata, ctrl,
					       IPU4P_PSYS_NAME);
	if (IS_ERR(psys_adev)) {
		kfree(pdata);
		return dev_err_cast_probe(&pdev->dev, psys_adev,
				"ipu4p_bus_initialize_device psys failed\n");
	}

	psys_adev->mmu = ipu4p_mmu_init(&pdev->dev, base, PSYS_MMID,
				       &ipdata->hw_variant);
	if (IS_ERR(psys_adev->mmu)) {
		put_device(&psys_adev->auxdev.dev);
		kfree(pdata);
		return dev_err_cast_probe(&pdev->dev, psys_adev->mmu,
				"ipu4p_mmu_init(psys_adev->mmu) failed\n");
	}

	psys_adev->mmu->dev = &psys_adev->auxdev.dev;

	ret = ipu4p_bus_add_device(psys_adev);
	if (ret) {
		kfree(pdata);
		return ERR_PTR(ret);
	}

	return psys_adev;
}

static int ipu4p_pci_config_setup(struct pci_dev *dev, u8 hw_ver)
{
	int ret;

	/* No PCI msi capability for IPU4PEP */
	if (is_ipu4pep(hw_ver) || is_ipu4pep_mtl(hw_ver)) {
		/* likely do nothing as msi not enabled by default */
		pci_disable_msi(dev);
		return 0;
	}

	ret = pci_alloc_irq_vectors(dev, 1, 1, PCI_IRQ_MSI);
	if (ret < 0)
		return dev_err_probe(&dev->dev, ret, "Request msi failed");

	return 0;
}

static void ipu4p_configure_vc_mechanism(struct ipu4p_device *isp)
{
	u32 val = readl(isp->base + BUTTRESS_REG_BTRS_CTRL);

	if (IPU4P_BTRS_ARB_STALL_MODE_VC0 == IPU4P_BTRS_ARB_MODE_TYPE_STALL)
		val |= BUTTRESS_REG_BTRS_CTRL_STALL_MODE_VC0;
	else
		val &= ~BUTTRESS_REG_BTRS_CTRL_STALL_MODE_VC0;

	if (IPU4P_BTRS_ARB_STALL_MODE_VC1 == IPU4P_BTRS_ARB_MODE_TYPE_STALL)
		val |= BUTTRESS_REG_BTRS_CTRL_STALL_MODE_VC1;
	else
		val &= ~BUTTRESS_REG_BTRS_CTRL_STALL_MODE_VC1;

	writel(val, isp->base + BUTTRESS_REG_BTRS_CTRL);
}

static int ipu4p_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct ipu4p_buttress_ctrl *isys_ctrl = NULL, *psys_ctrl = NULL;
	struct device *dev = &pdev->dev;
	void __iomem *isys_base = NULL;
	void __iomem *psys_base = NULL;
	struct ipu4p_device *isp;
	phys_addr_t phys;
	u32 val, version, sku_id;
	int ret;

	isp = devm_kzalloc(dev, sizeof(*isp), GFP_KERNEL);
	if (!isp)
		return -ENOMEM;

	isp->pdev = pdev;
	INIT_LIST_HEAD(&isp->devices);

	ret = pcim_enable_device(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "Enable PCI device failed\n");

	phys = pci_resource_start(pdev, IPU4P_PCI_BAR);
	dev_dbg(dev, "IPU4P PCI bar[%u] = %pa\n", IPU4P_PCI_BAR, &phys);

	isp->base = pcim_iomap_region(pdev, IPU4P_PCI_BAR, IPU4P_NAME);
	if (IS_ERR(isp->base))
		return dev_err_probe(dev, PTR_ERR(isp->base),
				     "Failed to I/O mem remapping\n");

	pci_set_drvdata(pdev, isp);
	pci_set_master(pdev);

	/* IPU4 metadata components carry a 32-byte SHA-256 hash (68 bytes) */
	isp->cpd_metadata_cmpnt_size = sizeof(struct ipu4pse_cpd_metadata_cmpnt);
	/* IPU4P (Ice Lake): one device, one firmware */
	isp->hw_ver = IPU4P_VER_4P;
	isp->cpd_fw_name = IPU4P_FIRMWARE_NAME;

	ipu4p_internal_pdata_init(isp);

	isys_base = isp->base + isys_ipdata.hw_variant.offset;
	psys_base = isp->base + psys_ipdata.hw_variant.offset;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(39));
	if (ret)
		return dev_err_probe(dev, ret, "Failed to set DMA mask\n");

	dma_set_max_seg_size(dev, UINT_MAX);

	ret = ipu4p_pci_config_setup(pdev, isp->hw_ver);
	if (ret)
		return ret;

	ret = ipu4p_buttress_init(isp);
	if (ret)
		return ret;

	ret = request_firmware(&isp->cpd_fw, isp->cpd_fw_name, dev);
	if (ret) {
		dev_err_probe(&isp->pdev->dev, ret,
			      "Requesting signed firmware %s failed\n",
			      isp->cpd_fw_name);
		goto buttress_exit;
	}

	ret = ipu4p_cpd_validate_cpd_file(isp, isp->cpd_fw->data,
					 isp->cpd_fw->size);
	if (ret) {
		dev_err_probe(&isp->pdev->dev, ret,
			      "Failed to validate cpd\n");
		goto out_ipu4p_bus_del_devices;
	}

	isys_ctrl = devm_kmemdup(dev, &isys_buttress_ctrl,
				 sizeof(isys_buttress_ctrl), GFP_KERNEL);
	if (!isys_ctrl) {
		ret = -ENOMEM;
		goto out_ipu4p_bus_del_devices;
	}

	isp->isys = ipu4p_isys_init(pdev, dev, isys_ctrl, isys_base,
				   &isys_ipdata);
	if (IS_ERR(isp->isys)) {
		ret = PTR_ERR(isp->isys);
		goto out_ipu4p_bus_del_devices;
	}

	psys_ctrl = devm_kmemdup(dev, &psys_buttress_ctrl,
				 sizeof(psys_buttress_ctrl), GFP_KERNEL);
	if (!psys_ctrl) {
		ret = -ENOMEM;
		goto out_ipu4p_bus_del_devices;
	}

	isp->psys = ipu4p_psys_init(pdev, &isp->isys->auxdev.dev, psys_ctrl,
				   psys_base, &psys_ipdata);
	if (IS_ERR(isp->psys)) {
		ret = PTR_ERR(isp->psys);
		goto out_ipu4p_bus_del_devices;
	}

	ret = pm_runtime_resume_and_get(&isp->psys->auxdev.dev);
	if (ret < 0)
		goto out_ipu4p_bus_del_devices;

	ret = ipu4p_mmu_hw_init(isp->psys->mmu);
	if (ret) {
		dev_err_probe(&isp->pdev->dev, ret,
			      "Failed to set MMU hardware\n");
		goto out_ipu4p_rpm_put;
	}

	ret = ipu4p_buttress_map_fw_image(isp->psys, isp->cpd_fw,
					 &isp->psys->fw_sgt);
	if (ret) {
		dev_err_probe(&isp->pdev->dev, ret, "failed to map fw image\n");
		goto out_ipu4p_rpm_put;
	}

	ret = ipu4p_cpd_create_pkg_dir(isp->psys, isp->cpd_fw->data);
	if (ret) {
		dev_err_probe(&isp->pdev->dev, ret,
			      "failed to create pkg dir\n");
		goto out_ipu4p_rpm_put;
	}

	ret = devm_request_threaded_irq(dev, pdev->irq, ipu4p_buttress_isr,
					ipu4p_buttress_isr_threaded,
					IRQF_SHARED, IPU4P_NAME, isp);
	if (ret) {
		dev_err_probe(dev, ret, "Requesting irq failed\n");
		goto out_ipu4p_rpm_put;
	}

	ret = ipu4p_buttress_authenticate(isp);
	if (ret) {
		dev_err_probe(&isp->pdev->dev, ret,
			      "FW authentication failed\n");
		goto out_free_irq;
	}

	ipu4p_mmu_hw_cleanup(isp->psys->mmu);
	pm_runtime_put(&isp->psys->auxdev.dev);

	/* Configure the arbitration mechanisms for VC requests */
	ipu4p_configure_vc_mechanism(isp);

	val = readl(isp->base + BUTTRESS_REG_SKU);
	sku_id = FIELD_GET(GENMASK(6, 4), val);
	version = FIELD_GET(GENMASK(3, 0), val);
	dev_info(dev, "IPU%u-v%u[%x] hardware version %d\n", version, sku_id,
		 pdev->device, isp->hw_ver);

	pm_runtime_put_noidle(dev);
	pm_runtime_allow(dev);

	isp->bus_ready_to_probe = true;

	return 0;

out_free_irq:
	devm_free_irq(dev, pdev->irq, isp);
out_ipu4p_rpm_put:
	pm_runtime_put_sync(&isp->psys->auxdev.dev);
out_ipu4p_bus_del_devices:
	if (!IS_ERR_OR_NULL(isp->psys)) {
		ipu4p_cpd_free_pkg_dir(isp->psys);
		ipu4p_buttress_unmap_fw_image(isp->psys, &isp->psys->fw_sgt);
	}
	if (!IS_ERR_OR_NULL(isp->psys) && !IS_ERR_OR_NULL(isp->psys->mmu))
		ipu4p_mmu_cleanup(isp->psys->mmu);
	if (!IS_ERR_OR_NULL(isp->isys) && !IS_ERR_OR_NULL(isp->isys->mmu))
		ipu4p_mmu_cleanup(isp->isys->mmu);
	ipu4p_bus_del_devices(pdev);
	release_firmware(isp->cpd_fw);
buttress_exit:
	ipu4p_buttress_exit(isp);

	return ret;
}

static void ipu4p_pci_remove(struct pci_dev *pdev)
{
	struct ipu4p_device *isp = pci_get_drvdata(pdev);
	struct ipu4p_mmu *isys_mmu = isp->isys->mmu;
	struct ipu4p_mmu *psys_mmu = isp->psys->mmu;

	devm_free_irq(&pdev->dev, pdev->irq, isp);
	ipu4p_cpd_free_pkg_dir(isp->psys);

	ipu4p_buttress_unmap_fw_image(isp->psys, &isp->psys->fw_sgt);
	ipu4p_buttress_exit(isp);

	ipu4p_bus_del_devices(pdev);

	pm_runtime_forbid(&pdev->dev);
	pm_runtime_get_noresume(&pdev->dev);

	release_firmware(isp->cpd_fw);

	ipu4p_mmu_cleanup(psys_mmu);
	ipu4p_mmu_cleanup(isys_mmu);
}

static void ipu4p_pci_reset_prepare(struct pci_dev *pdev)
{
	struct ipu4p_device *isp = pci_get_drvdata(pdev);

	pm_runtime_forbid(&isp->pdev->dev);
}

static void ipu4p_pci_reset_done(struct pci_dev *pdev)
{
	struct ipu4p_device *isp = pci_get_drvdata(pdev);

	ipu4p_buttress_restore(isp);
	if (isp->secure_mode)
		ipu4p_buttress_reset_authentication(isp);

	isp->need_ipc_reset = true;
	pm_runtime_allow(&isp->pdev->dev);
}

/*
 * PCI base driver code requires driver to provide these to enable
 * PCI device level PM state transitions (D0<->D3)
 */
static int ipu4p_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	synchronize_irq(pdev->irq);
	return 0;
}

static int ipu4p_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct ipu4p_device *isp = pci_get_drvdata(pdev);
	struct ipu4p_buttress *b = &isp->buttress;
	int ret;

	/* Configure the arbitration mechanisms for VC requests */
	ipu4p_configure_vc_mechanism(isp);

	isp->secure_mode = ipu4p_buttress_get_secure_mode(isp);
	dev_info(dev, "IPU4P in %s mode\n",
		 isp->secure_mode ? "secure" : "non-secure");

	ipu4p_buttress_restore(isp);

	ret = ipu4p_buttress_ipc_reset(isp, &b->cse);
	if (ret)
		dev_err(&isp->pdev->dev, "IPC reset protocol failed!\n");

	/*
	 * Firmware authentication needs PSYS powered, but PSYS is a child of
	 * ISYS, which has not finished resuming yet, so a runtime-PM get here
	 * fails and the firmware stays unauthenticated after S3 (ISYS then
	 * fails every stream with "port open ready failed"). Authenticate in
	 * .complete, which runs once every device has resumed and before
	 * user space is thawed.
	 */
	isp->need_resume_auth = true;

	return 0;
}

static void ipu4p_complete(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct ipu4p_device *isp = pci_get_drvdata(pdev);
	int ret;

	if (!isp || !isp->need_resume_auth)
		return;
	isp->need_resume_auth = false;

	ret = pm_runtime_resume_and_get(&isp->psys->auxdev.dev);
	if (ret < 0) {
		dev_err(&isp->psys->auxdev.dev,
			"Failed to get runtime PM after resume: %d\n", ret);
		return;
	}

	/* s2idle usually keeps the IPU powered: nothing to redo. */
	if (ipu4p_buttress_auth_done(isp)) {
		dev_dbg(&isp->pdev->dev, "FW still authenticated after resume\n");
		goto out_put;
	}

	/* As in probe: the firmware image is mapped through the PSYS MMU. */
	ret = ipu4p_mmu_hw_init(isp->psys->mmu);
	if (ret) {
		dev_err(&isp->pdev->dev, "Failed to set MMU hardware after resume: %d\n",
			ret);
		goto out_put;
	}

	ret = ipu4p_buttress_authenticate(isp);
	if (ret)
		dev_err(&isp->pdev->dev, "FW authentication failed after resume(%d)\n",
			ret);
	else
		dev_info(&isp->pdev->dev, "FW re-authenticated after resume\n");

	ipu4p_mmu_hw_cleanup(isp->psys->mmu);
out_put:
	pm_runtime_put(&isp->psys->auxdev.dev);
}

static int ipu4p_runtime_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct ipu4p_device *isp = pci_get_drvdata(pdev);
	int ret;

	ipu4p_configure_vc_mechanism(isp);
	ipu4p_buttress_restore(isp);

	if (isp->need_ipc_reset) {
		struct ipu4p_buttress *b = &isp->buttress;

		isp->need_ipc_reset = false;
		ret = ipu4p_buttress_ipc_reset(isp, &b->cse);
		if (ret)
			dev_err(&isp->pdev->dev, "IPC reset protocol failed\n");
	}

	return 0;
}

static const struct dev_pm_ops ipu4p_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(&ipu4p_suspend, &ipu4p_resume)
	RUNTIME_PM_OPS(&ipu4p_suspend, &ipu4p_runtime_resume, NULL)
	.complete = pm_sleep_ptr(ipu4p_complete),
};

MODULE_DEVICE_TABLE(pci, ipu4p_pci_tbl);

static const struct pci_error_handlers pci_err_handlers = {
	.reset_prepare = ipu4p_pci_reset_prepare,
	.reset_done = ipu4p_pci_reset_done,
};

static struct pci_driver ipu4p_pci_driver = {
	.name = IPU4P_NAME,
	.id_table = ipu4p_pci_tbl,
	.probe = ipu4p_pci_probe,
	.remove = ipu4p_pci_remove,
	.driver = {
		.pm = pm_ptr(&ipu4p_pm_ops),
	},
	.err_handler = &pci_err_handlers,
};

module_pci_driver(ipu4p_pci_driver);

MODULE_IMPORT_NS("INTEL_IPU_BRIDGE");
MODULE_AUTHOR("Sakari Ailus <sakari.ailus@linux.intel.com>");
MODULE_AUTHOR("Tianshu Qiu <tian.shu.qiu@intel.com>");
MODULE_AUTHOR("Bingbu Cao <bingbu.cao@intel.com>");
MODULE_AUTHOR("Qingwu Zhang <qingwu.zhang@intel.com>");
MODULE_AUTHOR("Yunliang Ding <yunliang.ding@intel.com>");
MODULE_AUTHOR("Hongju Wang <hongju.wang@intel.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Intel IPU4P PCI driver");
