/*******************************************************************************
*
*  NetFPGA-10G http://www.netfpga.org
*
*  File:
*        nf10_main.c
*
*  Project:
*
*
*  Author:
*        Hwanju Kim
*
*  Description:
*        The main driver module independent of DMA core is doing management jobs
*        and providing wrapper functions to DMA-dependent ones.
*        It uses hw_ops in nf10_adapter to call DMA-dependent functions, and
*        initially 'lbuf', which is large buffer-based DMA engine and is
*        designed by Marco Forconesi. In the future, if a new DMA core is added
*        do comply with hw_ops interface and add new mode to dma_mode, and
*        finally change nf10_init to set a proper hw_ops.
*        Another interface is user_ops, which is typically used for a user app
*        to directly control DMA engine via ioctl/mmap. The current user_ops is
*        merely a minimum set, which helps to prove the DMA performance
*        precluding kernel overheads. The user_ops is used by DMA-dependent
*        module like nf10_lbuf.c through nf10_adapter.
*
*        TODO: 
*		Once interrupt control logic is confirmed by DMA core,
*		irq enable/disable can be done in NAPI-related parts.
*
*	 This code is initially developed for the Network-as-a-Service (NaaS) project.
*        
*
*  Copyright notice:
*        Copyright (C) 2014 University of Cambridge
*
*  Licence:
*        This file is part of the NetFPGA 10G development base package.
*
*        This file is free code: you can redistribute it and/or modify it under
*        the terms of the GNU Lesser General Public License version 2.1 as
*        published by the Free Software Foundation.
*
*        This package is distributed in the hope that it will be useful, but
*        WITHOUT ANY WARRANTY; without even the implied warranty of
*        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*        Lesser General Public License for more details.
*
*        You should have received a copy of the GNU Lesser General Public
*        License along with the NetFPGA source package.  If not, see
*        http://www.gnu.org/licenses/.
*
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>

#include "nf10.h"
#include "nf10_user.h"

u64 nf10_test_dev_addr = 0x000f530dd164;

#define DEFAULT_MSG_ENABLE (NETIF_MSG_DRV|NETIF_MSG_PROBE|NETIF_MSG_LINK|NETIF_MSG_IFDOWN|NETIF_MSG_IFUP|NETIF_MSG_RX_ERR)
static int debug = -1;
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,12,0)
static bool reset = true;	/* assume it's clean right after booting */
module_param(reset, bool, 0644);
MODULE_PARM_DESC(reset, "PCIe reset sent");
#endif

/* DMA engine-dependent functions */
enum {
	DMA_LARGE_BUFFER = 0,
};
static int dma_mode = DMA_LARGE_BUFFER;
module_param(dma_mode, int, 0);
MODULE_PARM_DESC(dma_mode, "nf10 DMA version (0: large buffer)");

static bool buffer_initialized = false;

#ifdef CONFIG_PHY_INIT
extern irqreturn_t mdio_access_interrupt_handler(int irq, void *dev_id);
extern int configure_ael2005_phy_chips(struct nf10_adapter *adapter);

static int nf10_init_phy(struct pci_dev *pdev)
{
	/* AEL2005 MDIO configuration */
	int err = 0;
	struct nf10_adapter *adapter = pci_get_drvdata(pdev);
	if ((err = request_irq(pdev->irq, mdio_access_interrupt_handler,
					0, NF10_DRV_NAME, pdev)))
		return err;
	err = configure_ael2005_phy_chips(adapter);
	free_irq(pdev->irq, pdev);

	return err;
}
#endif

static int nf10_init(struct nf10_adapter *adapter)
{
	if (dma_mode == DMA_LARGE_BUFFER)
		nf10_lbuf_set_hw_ops(adapter);
	else
		return -EINVAL;

	if (unlikely(adapter->hw_ops == NULL))
		return -EINVAL;

	return adapter->hw_ops->init(adapter);
}

static void nf10_free(struct nf10_adapter *adapter)
{
	adapter->hw_ops->free(adapter);
}

static int nf10_init_buffers(struct nf10_adapter *adapter)
{
	return adapter->hw_ops->init_buffers(adapter);
}

static void nf10_free_buffers(struct nf10_adapter *adapter)
{
	adapter->hw_ops->free_buffers(adapter);
}

static int nf10_napi_budget(struct nf10_adapter *adapter)
{
	return adapter->hw_ops->get_napi_budget();
}

void nf10_process_rx_irq(struct nf10_adapter *adapter, int *work_done, int budget)
{
	adapter->hw_ops->process_rx_irq(adapter, work_done, budget);
}

static netdev_tx_t nf10_start_xmit(struct sk_buff *skb,
				   struct net_device *netdev)
{
	struct nf10_adapter *adapter = netdev_adapter(netdev);
	
	return adapter->hw_ops->start_xmit(skb, netdev);
}

static int nf10_clean_tx_irq(struct nf10_adapter *adapter)
{
	return adapter->hw_ops->clean_tx_irq(adapter);
}

static void nf10_enable_irq(struct nf10_adapter *adapter)
{
	adapter->hw_ops->ctrl_irq(adapter, IRQ_CTRL_ENABLE);
}

int nf10_poll(struct napi_struct *napi, int budget);
static int nf10_up(struct net_device *netdev)
{
	struct nf10_adapter *adapter = netdev_adapter(netdev);
	int err;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,12,0)
	if (reset == false) {
		/* The reason to reset pci here in up handler is
		 * that reset is possible once pdev is probed, but
		 * in probe handler, pdev/bus is locked so defer 
		 * the reset to up handler. The reset is for
		 * initializing all state machines of DMA */
		if ((err = pci_reset_bus(adapter->pdev->bus))) {
			netif_err(adapter, ifup, netdev,
				  "failed to reset bus (err=%d)\n", err);
			return err;
		}
		reset = true;
		netif_info(adapter, ifup, netdev, "PCIe bus is reset\n");
	}
#endif

	if (buffer_initialized == false) {
		if ((err = nf10_init_buffers(adapter))) {
			netif_err(adapter, ifup, netdev,
				  "failed to initialize packet buffers: err=%d\n", err);
			return err;
		}
		buffer_initialized = true;
		nf10_enable_irq(adapter);
		netif_napi_add(netdev, &adapter->napi, nf10_poll,
			       nf10_napi_budget(adapter));
		napi_enable(&adapter->napi);
	}

	netdev_port_up(netdev) = 1;
	netif_start_queue(netdev);

	netif_info(adapter, ifup, netdev, "up\n");
	return 0;
}

static int nf10_down(struct net_device *netdev)
{
	struct nf10_adapter *adapter = netdev_adapter(netdev);
	int i;

	netif_stop_queue(netdev);
	netdev_port_up(netdev) = 0;

	/* check if this interface is the last */
	for (i = 0; i < CONFIG_NR_PORTS; i++)
		if (netdev_port_up(adapter->netdev[i]))
			break;
	if (i == CONFIG_NR_PORTS) {	/* all ifs down */
		/* TODO: current irq control in nf10 is in a toggled way,
		   which should be fixed in an explicit way. After then,
		   nf10_disable_irq() is put here */
		napi_disable(&adapter->napi);
		netif_napi_del(&adapter->napi);
		nf10_free_buffers(adapter);
		buffer_initialized = false;
	}

	netif_info(adapter, ifdown, netdev, "down\n");
	return 0;
}

static int nf10_do_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	return 0;
}

static struct net_device_stats *nf10_get_stats(struct net_device *dev)
{
	return &dev->stats;
}

static const struct net_device_ops nf10_netdev_ops = {
	.ndo_open		= nf10_up,
	.ndo_stop		= nf10_down,
	.ndo_do_ioctl		= nf10_do_ioctl,
	.ndo_get_stats		= nf10_get_stats,
	.ndo_start_xmit		= nf10_start_xmit
};

irqreturn_t nf10_interrupt_handler(int irq, void *data)
{
	struct pci_dev *pdev = data;
	struct nf10_adapter *adapter = pci_get_drvdata(pdev);

	netif_dbg(adapter, intr, default_netdev(adapter), "IRQ delivered\n");

	/* FIXME: will be removed after explicit irq control of nf10 is added */
	if (unlikely(buffer_initialized == false))
		return IRQ_HANDLED;

	/* TODO: IRQ disable */
	
	napi_schedule(&adapter->napi);

	return IRQ_HANDLED;
}

int nf10_poll(struct napi_struct *napi, int budget)
{       
	struct nf10_adapter *adapter = 
		container_of(napi, struct nf10_adapter, napi);
	int tx_clean_complete, work_done = 0;

	tx_clean_complete = nf10_clean_tx_irq(adapter);

	nf10_process_rx_irq(adapter, &work_done, budget);

	/* if uncleaned tx is remaining, don't want to complete napi */
	if (!tx_clean_complete)
		work_done = budget;

	if (work_done < budget) {
		napi_complete(napi);

		/* TODO: enable IRQ */
	}

	return work_done;
}

static void nf10_init_netdev(struct net_device *netdev)
{
	ether_setup(netdev);

	netdev->netdev_ops = &nf10_netdev_ops;
	nf10_set_ethtool_ops(netdev);
}

static void nf10_free_netdev(struct nf10_adapter *adapter)
{
	int i;
	for (i = 0; i < CONFIG_NR_PORTS; i++) {
		if (adapter->netdev[i]) {
			unregister_netdev(adapter->netdev[i]);
			free_netdev(adapter->netdev[i]);
		}
	}
}

static int nf10_create_netdev(struct pci_dev *pdev,
			      struct nf10_adapter *adapter)
{
	struct net_device *netdev;
	struct nf10_netdev_priv *ndev_priv;
	int i;
	int err;

	for (i = 0; i < CONFIG_NR_PORTS; i++) {
		netdev = alloc_netdev(sizeof(struct nf10_netdev_priv),
				      "nf%d", nf10_init_netdev);
		if (netdev == NULL) {
			pr_err("Error: failed to alloc netdev[%d]\n", i);
			err = -ENOMEM;
			goto err_alloc_netdev;
		}
		/* set netdev's parent to pdev->dev */
		SET_NETDEV_DEV(netdev, &pdev->dev);

		/* assign MAC address */
		memcpy(netdev->dev_addr, &nf10_test_dev_addr, ETH_ALEN);
		netdev->dev_addr[ETH_ALEN - 1] = i;

		/* make cross-link between netdev and adapter */
		ndev_priv = netdev_priv(netdev);
		ndev_priv->adapter = adapter;
		ndev_priv->port_num = i;
		ndev_priv->port_up = 0;

		if ((err = register_netdev(netdev))) {
			free_netdev(netdev);
			pr_err("failed to register netdev\n");
			goto err_alloc_netdev;
		}
		/* non-NULL netdev[i] if both allocated and registered */
		adapter->netdev[i] = netdev;
	}
	return 0;

err_alloc_netdev:
	nf10_free_netdev(adapter);
	return err;
}

static int nf10_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct nf10_adapter *adapter;
	int err;

	/* 1. PCI device init */
	if ((err = pci_enable_device(pdev)))
		return err;

	if ((err = pci_set_dma_mask(pdev, DMA_BIT_MASK(64))) ||
	    (err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64)))) {
		pr_err("DMA configuration failed to set 64bit mask\n");
		goto err_dma;
	}

	if ((err = pci_request_regions(pdev, NF10_DRV_NAME)))
		goto err_request_regions;

	pci_set_master(pdev);

	/* 2. create adapter & netdev and link with each other including pdev */
	adapter = kzalloc(sizeof(struct nf10_adapter), GFP_KERNEL);
	if (adapter == NULL) {
		err = -ENOMEM;
		goto err_alloc_adapter;
	}
	pci_set_drvdata(pdev, adapter);

	if ((err = nf10_create_netdev(pdev, adapter)))
		goto err_create_netdev;

	adapter->pdev = pdev;
	adapter->msg_enable = netif_msg_init(debug, DEFAULT_MSG_ENABLE);
	if ((adapter->bar0 = pci_iomap(pdev, 0, 0)) == NULL) {
		err = -EIO;
		goto err_pci_iomap_bar0;
	}

	if ((adapter->bar2 = pci_iomap(pdev, 2, 0)) == NULL) {
		err = -EIO;
		goto err_pci_iomap_bar2;
	}

	/* 3. init interrupt */
	if ((err = pci_enable_msi(pdev))) {
		pr_err("failed to enable MSI: err=%d\n", err);
		goto err_enable_msi;
	}

#ifdef CONFIG_PHY_INIT
	if ((err = nf10_init_phy(pdev))) {
		pr_err("failed to initialize PHY chip\n");
		goto err_enable_msi;
	}
#endif

	if ((err = request_irq(pdev->irq, nf10_interrupt_handler, 0, 
					NF10_DRV_NAME, pdev))) {
		pr_err("failed to request irq%d\n", pdev->irq);
		goto err_request_irq;
	}

	/* 4. init DMA */
	if ((err = nf10_init(adapter))) {
		pr_err("failed to register hw ops\n");
		goto err_register_hw_ops;
	}

	/* 5. init user-level access */
	nf10_init_fops(adapter);
	init_waitqueue_head(&adapter->wq_user_intr);

	netif_info(adapter, probe, default_netdev(adapter),
		   "probe is done successfully\n");

	return 0;

err_register_hw_ops:
	free_irq(pdev->irq, pdev);
err_request_irq:
	pci_disable_msi(pdev);
err_enable_msi:
	pci_iounmap(pdev, adapter->bar2);
err_pci_iomap_bar2:
	pci_iounmap(pdev, adapter->bar0);
err_pci_iomap_bar0:
	nf10_free_netdev(adapter);
err_create_netdev:
	pci_set_drvdata(pdev, NULL);
  kfree(adapter);
err_alloc_adapter:
	pci_clear_master(pdev);
	pci_release_regions(pdev);
err_request_regions:
err_dma:
	pci_disable_device(pdev);
	return err;
}

static void nf10_remove(struct pci_dev *pdev)
{
	struct nf10_adapter *adapter = pci_get_drvdata(pdev);

	if (!adapter)
		return;

	nf10_remove_fops(adapter);
	nf10_free(adapter);
	nf10_free_netdev(adapter);

	free_irq(pdev->irq, pdev);
	pci_disable_msi(pdev);
	pci_iounmap(pdev, adapter->bar2);
	pci_iounmap(pdev, adapter->bar0);
	pci_set_drvdata(pdev, NULL);
	pci_clear_master(pdev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	kfree(adapter);

	pr_info("nf10: remove is done successfully\n");
}

static struct pci_device_id pci_id[] = {
	{PCI_DEVICE(NF10_VENDOR_ID, NF10_DEVICE_ID)},
	{0}
};
MODULE_DEVICE_TABLE(pci, pci_id);

pci_ers_result_t nf10_pcie_error(struct pci_dev *dev, 
				 enum pci_channel_state state)
{
	/* TODO */
	return PCI_ERS_RESULT_RECOVERED;
}

static struct pci_error_handlers pcie_err_handlers = {
	.error_detected = nf10_pcie_error
};

static struct pci_driver pci_driver = {
	.name = NF10_DRV_NAME,
	.id_table = pci_id,
	.probe = nf10_probe,
	.remove = nf10_remove,
	.err_handler = &pcie_err_handlers
};

static int __init nf10_drv_init(void)
{
	return pci_register_driver(&pci_driver);
}
module_init(nf10_drv_init);

static void __exit nf10_drv_exit(void)
{
	pci_unregister_driver(&pci_driver);
}
module_exit(nf10_drv_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Cambridge NaaS Team");
MODULE_DESCRIPTION("Device driver for NetFPGA 10g reference NIC");