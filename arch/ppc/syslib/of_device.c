#include <linux/config.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <asm/errno.h>
#include <asm/of_device.h>

/**
 * of_match_device - Tell if an of_device structure has a matching
 * of_match structure
 * @ids: array of of device match structures to search in
 * @dev: the of device structure to match against
 * 
 * Used by a driver to check whether an of_device present in the
 * system is in its list of supported devices. 
 */
const struct of_match *
of_match_device(const struct of_match *matches, const struct of_device *dev)
{
	if (!dev->node)
		return NULL;
	while (matches->name || matches->type || matches->compatible) {
		int match = 1;
		if (matches->name && matches->name != OF_ANY_MATCH)
			match &= dev->node->name
				&& !strcmp(matches->name, dev->node->name);
		if (matches->type && matches->type != OF_ANY_MATCH)
			match &= dev->node->type
				&& !strcmp(matches->type, dev->node->type);
		if (matches->compatible && matches->compatible != OF_ANY_MATCH)
			match &= device_is_compatible(dev->node,
				matches->compatible);
		if (match)
			return matches;
		matches++;
	}
	return NULL;
}

static int
of_platform_bus_match(struct device *dev, struct device_driver *drv) 
{
	struct of_device * of_dev = to_of_device(dev);
	struct of_platform_driver * of_drv = to_of_platform_driver(drv);
	const struct of_match * matches = of_drv->match_table;

	if (!matches) 
		return 0;

	return of_match_device(matches, of_dev) != NULL;
}

struct bus_type of_platform_bus_type = {
       name:	"of_platform",
       match:	of_platform_bus_match,
};

static int __init
of_bus_driver_init(void)
{
	return bus_register(&of_platform_bus_type);
}

postcore_initcall(of_bus_driver_init);

static int
of_device_probe(struct device *dev)
{
	int error = -ENODEV;
	struct of_platform_driver *drv;
	struct of_device *of_dev;
	const struct of_match *match;

	drv = to_of_platform_driver(dev->driver);
	of_dev = to_of_device(dev);

	if (!drv->probe)
		return error;

/*	if (!try_module_get(driver->owner)) {
		printk(KERN_ERR "Can't get a module reference for %s\n", driver->name);
		return error;
	}
*/
	match = of_match_device(drv->match_table, of_dev);
	if (match)
		error = drv->probe(of_dev, match);
/*
 	module_put(driver->owner);
*/	
	return error;
}

static int
of_device_remove(struct device *dev)
{
	struct of_device * of_dev = to_of_device(dev);
	struct of_platform_driver * drv = to_of_platform_driver(of_dev->dev.driver);

	if (drv && drv->remove)
		drv->remove(of_dev);
	return 0;
}

static int
of_device_suspend(struct device *dev, u32 state, u32 level)
{
	struct of_device * of_dev = to_of_device(dev);
	struct of_platform_driver * drv = to_of_platform_driver(of_dev->dev.driver);
	int error = 0;

	if (drv && drv->suspend)
		error = drv->suspend(of_dev, state, level);
	return error;
}

static int
of_device_resume(struct device * dev, u32 level)
{
	struct of_device * of_dev = to_of_device(dev);
	struct of_platform_driver * drv = to_of_platform_driver(of_dev->dev.driver);
	int error = 0;

	if (drv && drv->resume)
		error = drv->resume(of_dev, level);
	return error;
}

int
of_register_driver(struct of_platform_driver *drv)
{
	int count = 0;

	/* initialize common driver fields */
	drv->driver.name = drv->name;
	drv->driver.bus = &of_platform_bus_type;
	drv->driver.probe = of_device_probe;
	drv->driver.resume = of_device_resume;
	drv->driver.suspend = of_device_suspend;
	drv->driver.remove = of_device_remove;

	/* register with core */
	count = driver_register(&drv->driver);
	return count ? count : 1;
}

void
of_unregister_driver(struct of_platform_driver *drv)
{
	driver_unregister(&drv->driver);
}


static ssize_t
dev_show_devspec(struct device *dev, char *buf)
{
	struct of_device *ofdev;

	ofdev = to_of_device(dev);
	return sprintf(buf, "%s", ofdev->node->full_name);
}

static DEVICE_ATTR(devspec, S_IRUGO, dev_show_devspec, NULL);

int
of_device_register(struct of_device *ofdev)
{
	int rc;
	struct of_device **odprop;

	BUG_ON(ofdev->node == NULL);
	
	odprop = (struct of_device **)get_property(ofdev->node, "linux,device", NULL);
	if (!odprop) {
		struct property *new_prop;
		
		new_prop = kmalloc(sizeof(struct property) + sizeof(struct of_device *),
			GFP_KERNEL);
		if (new_prop == NULL)
			return -ENOMEM;
		new_prop->name = "linux,device";
		new_prop->length = sizeof(sizeof(struct of_device *));
		new_prop->value = (unsigned char *)&new_prop[1];
		odprop = (struct of_device **)new_prop->value;
		*odprop = NULL;
		prom_add_property(ofdev->node, new_prop);
	}
	*odprop = ofdev;

	rc = device_register(&ofdev->dev);
	if (rc)
		return rc;

	device_create_file(&ofdev->dev, &dev_attr_devspec);

	return 0;
}

void
of_device_unregister(struct of_device *ofdev)
{
	struct of_device **odprop;

	device_remove_file(&ofdev->dev, &dev_attr_devspec);
	device_unregister(&ofdev->dev);

	odprop = (struct of_device **)get_property(ofdev->node, "linux,device", NULL);
	if (odprop)
		*odprop = NULL;
}

struct of_device*
of_platform_device_create(struct device_node *np, const char *bus_id)
{
	struct of_device *dev;
	u32 *reg;
	
	dev = kmalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return NULL;
	memset(dev, 0, sizeof(*dev));

	dev->node = np;
	dev->dma_mask = 0xffffffffUL;
	dev->dev.dma_mask = &dev->dma_mask;
	dev->dev.parent = NULL;
	dev->dev.bus = &of_platform_bus_type;

	/* XXX Make something better here ? */
	snprintf(dev->dev.name, DEVICE_NAME_SIZE, "Platform device %s", np->name);
	reg = (u32 *)get_property(np, "reg", NULL);
	strlcpy(dev->dev.bus_id, bus_id, BUS_ID_SIZE);

	if (of_device_register(dev) != 0) {
		kfree(dev);
		return NULL;
	}

	return dev;
}

EXPORT_SYMBOL(of_match_device);
EXPORT_SYMBOL(of_platform_bus_type);
EXPORT_SYMBOL(of_register_driver);
EXPORT_SYMBOL(of_unregister_driver);
EXPORT_SYMBOL(of_device_register);
EXPORT_SYMBOL(of_device_unregister);
EXPORT_SYMBOL(of_platform_device_create);
