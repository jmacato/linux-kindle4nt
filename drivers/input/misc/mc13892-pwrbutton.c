/**
 * Copyright (C) 2011 Philippe Rétornaz
 *
 * Based on twl4030-pwrbutton driver by:
 *     Peter De Schrijver <peter.de-schrijver@nokia.com>
 *     Felipe Balbi <felipe.balbi@nokia.com>
 *
 * This file is subject to the terms and conditions of the GNU General
 * Public License. See the file "COPYING" in the main directory of this
 * archive for more details.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335  USA
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/mfd/mc13892.h>
#include <linux/sched.h>
#include <linux/slab.h>

struct mc13892_pwrb {
	struct input_dev *pwr;
	struct mc13xxx *mc13892;
#define MC13892_PWRB_B1_POL_INVERT (1 << 0)
#define MC13892_PWRB_B2_POL_INVERT (1 << 1)
#define MC13892_PWRB_B3_POL_INVERT (1 << 2)
	int flags;
	unsigned short keymap[3];
};

#define MC13892_REG_INTERRUPT_SENSE_1 5
#define MC13892_IRQSENSE1_ONOFD1S (1 << 3)
#define MC13892_IRQSENSE1_ONOFD2S (1 << 4)
#define MC13892_IRQSENSE1_ONOFD3S (1 << 5)

#define MC13892_REG_POWER_CONTROL_2 15
#define MC13892_POWER_CONTROL_2_ON1BDBNC 4
#define MC13892_POWER_CONTROL_2_ON2BDBNC 6
#define MC13892_POWER_CONTROL_2_ON3BDBNC 8
#define MC13892_POWER_CONTROL_2_ON1BRSTEN (1 << 1)
#define MC13892_POWER_CONTROL_2_ON2BRSTEN (1 << 2)
#define MC13892_POWER_CONTROL_2_ON3BRSTEN (1 << 3)

#define MC13892_IRQ_ONOFD3 26
#define MC13892_IRQ_ONOFD1 27
#define MC13892_IRQ_ONOFD2 28

static irqreturn_t button_irq(int irq, void *_priv)
{
	struct mc13892_pwrb *priv = _priv;
	int val;
	int pwr1;

	mc13xxx_irq_ack(priv->mc13892, irq);
	mc13xxx_reg_read(priv->mc13892, MC13892_REG_INTERRUPT_SENSE_1, &val);

	pwr1 = val & MC13892_IRQSENSE1_ONOFD1S ? 1 : 0;

	input_report_key(priv->pwr, priv->keymap[0], pwr1);

	input_sync(priv->pwr);

	return IRQ_HANDLED;
}

static int mc13892_pwrbutton_probe(struct platform_device *pdev)
{
	// const struct mc13xxx_buttons_platform_data *pdata;
	struct mc13xxx *mc13892 = dev_get_drvdata(pdev->dev.parent);
	struct input_dev *pwr;
	struct mc13892_pwrb *priv;
	int err = 0;
	int reg = 0;
	int fail = 0;
	int key = 0;

	key = KEY_POWER;

	// pdata = dev_get_platdata(&pdev->dev);
	// if (!pdata) {
	// 	dev_err(&pdev->dev, "missing platform data\n");
	// 	return -ENODEV;
	// }

	pwr = input_allocate_device();
	if (!pwr) {
		dev_dbg(&pdev->dev, "Can't allocate power button\n");
		return -ENOMEM;
	}

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		err = -ENOMEM;
		dev_dbg(&pdev->dev, "Can't allocate power button\n");
		goto free_input_dev;
	}

	reg |= MC13892_POWER_CONTROL_2_ON1BDBNC;
	reg |= MC13892_POWER_CONTROL_2_ON2BDBNC;
	reg |= MC13892_POWER_CONTROL_2_ON3BDBNC;

	priv->pwr = pwr;
	priv->mc13892 = mc13892;

	mc13xxx_lock(mc13892);

	priv->keymap[0] = key;

	if (key != KEY_RESERVED)
		__set_bit(key, pwr->keybit);

	err = mc13xxx_irq_request(mc13892, MC13892_IRQ_ONOFD1, button_irq,
				  "b1on", priv);

	if (err) {
		dev_dbg(&pdev->dev, "Can't request irq\n");
		fail = 1;
		goto free_priv;
	}

	// Wakes the device up since it's a power button...
	device_init_wakeup(&pdev->dev, 1);

	mc13xxx_reg_rmw(mc13892, MC13892_REG_POWER_CONTROL_2, 0x3FE, reg);

	mc13xxx_unlock(mc13892);

	pwr->name = "mc13892_pwrbutton";
	pwr->phys = "mc13892_pwrbutton/input0";
	pwr->dev.parent = &pdev->dev;

	pwr->keycode = priv->keymap;
	pwr->keycodemax = ARRAY_SIZE(priv->keymap);
	pwr->keycodesize = sizeof(priv->keymap[0]);
	__set_bit(EV_KEY, pwr->evbit);

	err = input_register_device(pwr);
	if (err) {
		dev_dbg(&pdev->dev, "Can't register power button: %d\n", err);
		fail = 1;
		goto free_priv;
	}

	platform_set_drvdata(pdev, priv);

	return 0;

free_priv:

	if (fail) {
		mc13xxx_lock(mc13892);
		// mc13xxx_irq_free(mc13892, MC13892_IRQ_ONOFD3, priv);
		// mc13xxx_irq_free(mc13892, MC13892_IRQ_ONOFD2, priv);
		mc13xxx_irq_free(mc13892, MC13892_IRQ_ONOFD1, priv);
		mc13xxx_unlock(mc13892);
	}

	kfree(priv);

free_input_dev:
	input_free_device(pwr);

	return err;
}

static int mc13892_pwrbutton_remove(struct platform_device *pdev)
{
	struct mc13892_pwrb *priv = platform_get_drvdata(pdev);
	const struct mc13xxx_buttons_platform_data *pdata;

	pdata = dev_get_platdata(&pdev->dev);

	mc13xxx_lock(priv->mc13892);

	// mc13xxx_irq_free(priv->mc13892, MC13892_IRQ_ONOFD3, priv);
	// mc13xxx_irq_free(priv->mc13892, MC13892_IRQ_ONOFD2, priv);
	mc13xxx_irq_free(priv->mc13892, MC13892_IRQ_ONOFD1, priv);

	mc13xxx_unlock(priv->mc13892);

	input_unregister_device(priv->pwr);
	kfree(priv);

	return 0;
}

static struct platform_driver mc13892_pwrbutton_driver = {
	.probe		= mc13892_pwrbutton_probe,
	.remove		= mc13892_pwrbutton_remove,
	.driver		= {
		.name	= "mc13892-pwrbutton",
	},
};

module_platform_driver(mc13892_pwrbutton_driver);

MODULE_ALIAS("platform:mc13892-pwrbutton");
MODULE_DESCRIPTION("MC13892 Power Button - Kindle 4 NT Version");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Philippe Retornaz, Jumar Macato");
