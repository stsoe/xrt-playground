// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo CU Sub-device Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors: min.ma@xilinx.com
 */

#include "xocl_drv.h"
#include "xrt_cu.h"

#define XCU_INFO(xcu, fmt, arg...) \
	xocl_info(&xcu->pdev->dev, fmt "\n", ##arg)
#define XCU_ERR(xcu, fmt, arg...) \
	xocl_err(&xcu->pdev->dev, fmt "\n", ##arg)
#define XCU_DBG(xcu, fmt, arg...) \
	xocl_dbg(&xcuc->pdev->dev, fmt "\n", ##arg)

struct xocl_cu {
	struct xrt_cu		 base;
	struct platform_device	*pdev;
};

static int cu_submit(struct platform_device *pdev, struct kds_command *xcmd)
{
	struct xocl_cu *xcu = platform_get_drvdata(pdev);

	xrt_cu_submit(&xcu->base, xcmd);

	return 0;
}

static int cu_probe(struct platform_device *pdev)
{
	xdev_handle_t xdev = xocl_get_xdev(pdev);
	struct xocl_cu *xcu;
	struct resource **res;
	struct xrt_cu_info *info;
	int err = 0;
	void *hdl;
	int i;

	xcu = xocl_drvinst_alloc(&pdev->dev, sizeof(struct xocl_cu));
	if (!xcu)
		return -ENOMEM;

	xcu->pdev = pdev;
	xcu->base.dev = XDEV2DEV(xdev);

	info = XOCL_GET_SUBDEV_PRIV(&pdev->dev);
	memcpy(&xcu->base.info, info, sizeof(struct xrt_cu_info));

	res = vzalloc(sizeof(struct resource *) * xcu->base.info.num_res);
	if (!res) {
		err = -ENOMEM;
		goto err;
	}

	for (i = 0; i < xcu->base.info.num_res; ++i) {
		res[i] = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res[i]) {
			err = -EINVAL;
			goto err1;
		}
	}
	xcu->base.res = res;

	err = xocl_cu_ctrl_add_cu(xdev, &xcu->base);
	if (err) {
		err = 0; //Ignore this error until all platforms support CU controller
		//XCU_ERR(xcu, "Not able to add CU %p to controller", xcu);
		goto err1;
	}

	switch (info->model) {
	case XCU_HLS:
		err = xrt_cu_hls_init(&xcu->base);
		break;
	case XCU_PLRAM:
		err = xrt_cu_plram_init(&xcu->base);
		break;
	default:
		err = -EINVAL;
	}
	if (err) {
		XCU_ERR(xcu, "Not able to initial CU %p", xcu);
		goto err2;
	}

	platform_set_drvdata(pdev, xcu);

	return 0;

err2:
	xocl_cu_ctrl_remove_cu(xdev, &xcu->base);
err1:
	vfree(res);
err:
	xocl_drvinst_release(xcu, &hdl);
	xocl_drvinst_free(hdl);
	return err;
}

static int cu_remove(struct platform_device *pdev)
{
	xdev_handle_t xdev = xocl_get_xdev(pdev);
	struct xrt_cu_info *info;
	struct xocl_cu *xcu;
	void *hdl;

	xcu = platform_get_drvdata(pdev);
	if (!xcu)
		return -EINVAL;

	info = &xcu->base.info;
	switch (info->model) {
	case XCU_HLS:
		xrt_cu_hls_fini(&xcu->base);
		break;
	case XCU_PLRAM:
		xrt_cu_plram_fini(&xcu->base);
		break;
	}

	(void) xocl_cu_ctrl_remove_cu(xdev, &xcu->base);

	if (xcu->base.res)
		vfree(xcu->base.res);

	xocl_drvinst_release(xcu, &hdl);

	platform_set_drvdata(pdev, NULL);
	xocl_drvinst_free(hdl);

	return 0;
}

static struct xocl_cu_funcs cu_ops = {
	.submit	= cu_submit,
};

static struct xocl_drv_private cu_priv = {
	.ops = &cu_ops,
};

static struct platform_device_id cu_id_table[] = {
	{ XOCL_DEVNAME(XOCL_CU), (kernel_ulong_t)&cu_priv },
	{ },
};

static struct platform_driver cu_driver = {
	.probe		= cu_probe,
	.remove		= cu_remove,
	.driver		= {
		.name = XOCL_DEVNAME(XOCL_CU),
	},
	.id_table	= cu_id_table,
};

int __init xocl_init_cu(void)
{
	return platform_driver_register(&cu_driver);
}

void xocl_fini_cu(void)
{
	platform_driver_unregister(&cu_driver);
}
