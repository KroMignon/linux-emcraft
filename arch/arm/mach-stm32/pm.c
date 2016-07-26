/*
 * arch/arm/mach-stm32/pm.c
 *
 * STM32 Power Management code
 *
 * Copyright (C) 2016 Emcraft Systems
 * Yuri Tikhonov, Emcraft Systems, <yur@emcraft.com>
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <linux/suspend.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/interrupt.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/delay.h>

#include <asm/gpio.h>

#include <mach/platform.h>
#include <mach/stm32.h>
#include <mach/irqs.h>
#include <mach/fb.h>

/*
 * Assembler-level imports (from SRAM)
 */
#define SRAM_TEXT	__attribute__((section (".sram.text"),__long_call__))
SRAM_TEXT void stm32_suspend_to_ram(void);
extern u32 stm32_suspend_moder[STM32_GPIO_PORTS];

/*
 * Wake-up GPIO
 */
#define STM32_WAKEUP_PORT	0	/* PA0 */
#define STM32_WAKEUP_PIN	0
#define STM32_WAKEUP_GPIO	STM32_GPIO_PORTPIN2NUM(STM32_WAKEUP_PORT, \
						       STM32_WAKEUP_PIN)

#if !defined(CONFIG_STM32_GPIO_INT) || (STM32_WAKEUP_GPIO >= STM32_GPIO_NUM)
# error "Bad wake-up GPIO or GPIO interrupts disabled."
#endif

/*
 * PHY switch GPIO
 */
#define STM32_PHY_PORT		6	/* PG6 */
#define STM32_PHY_PIN		6
#define STM32_PHY_GPIO		STM32_GPIO_PORTPIN2NUM(STM32_PHY_PORT, \
						       STM32_PHY_PIN)

/*
 * Cortex-M3 System Control Register
 */
#define CM3_SCR_BASE		0xE000ED10
#define CM3_SCR_SLEEPDEEP	(1 << 2)

/*
 * RCC registers bits and masks
 */
#define STM32_RCC_CR_HSE_BIT	16
#define STM32_RCC_CR_PLL_BIT	24
#define STM32_RCC_CR_I2S_BIT	26
#define STM32_RCC_CR_SAI_BIT	28

#define STM32_RCC_CFGR_SW_MSK	0x3

/*
 * PWR registers bits and masks
 */
#define STM32_PWR_CR_LPDS	(1 << 0)
#define STM32_PWR_CR_FPDS	(1 << 9)
#define STM32_PWR_CR_LPUDS	(1 << 10)
#define STM32_PWR_CR_ODEN	(1 << 16)
#define STM32_PWR_CR_ODSWEN	(1 << 17)
#define STM32_PWR_CR_UDEN	(0x3 << 18)

#define STM32_PWR_CSR_ODRDY	(1 << 16)

/*
 * Different data we saved on entering, and restore on exiting
 * from PM
 */
static struct {
	struct {
		u32	cr;
		u32	cfgr;
	} rcc;
	struct {
		u32	cr;
	} pwr;
	struct {
		u32	gcr;
	} ltdc;
} stm32_pm_bck;

/*
 * Validate suspend state
 * @state		State being entered
 * @returns		1->valid, 0->invalid
 */
static int stm32_pm_valid(suspend_state_t state)
{
	int ret;

	switch (state) {

	case PM_SUSPEND_ON:
	case PM_SUSPEND_STANDBY:
	case PM_SUSPEND_MEM:
		ret = 1;
		break;
	default:
		ret = 0;
		break;
	}

	return ret;
}

/*
 * Prepare system hardware to suspend. Some settings are reset on
 * exiting from Stop, so we save these here, and restore then
 */
static void stm32_pm_prepare_to_suspend(void)
{
	/*
	 * Save RCC
	 */
	stm32_pm_bck.rcc.cr = STM32_RCC->cr;
	stm32_pm_bck.rcc.cfgr = STM32_RCC->cfgr & STM32_RCC_CFGR_SW_MSK;

	/*
	 * Save over-drive settings
	 */
	stm32_pm_bck.pwr.cr = STM32_PWR->cr;

	/*
	 * Switch off PHY power
	 */
	if (stm32_platform_get() == PLATFORM_STM32_STM_STM32F7_SOM)
		gpio_direction_output(STM32_PHY_GPIO, 1);

	/*
	 * FB driver may be off, so always stop LTDC here to avoid SDRAM access
	 */
	stm32_pm_bck.ltdc.gcr = readl(STM32F4_LTDC_BASE + LTDC_GCR);
	writel(stm32_pm_bck.ltdc.gcr & ~1, STM32F4_LTDC_BASE + LTDC_GCR);
}

/*
 * Prepare system hardware to resume
 */
static void stm32_pm_prepare_to_resume(void)
{
	u32 pll[] = { STM32_RCC_CR_HSE_BIT, STM32_RCC_CR_PLL_BIT,
		      STM32_RCC_CR_I2S_BIT, STM32_RCC_CR_SAI_BIT };
	int i;

	/*
	 * Restore LTDC
	 */
	if (stm32_pm_bck.ltdc.gcr & 1)
		writel(stm32_pm_bck.ltdc.gcr, STM32F4_LTDC_BASE + LTDC_GCR);

	/*
	 * Restore PHY power
	 */
	if (stm32_platform_get() == PLATFORM_STM32_STM_STM32F7_SOM)
		gpio_direction_input(STM32_PHY_GPIO);

	/*
	 * Restore over-drive
	 */
	if (stm32_pm_bck.pwr.cr & STM32_PWR_CR_ODSWEN) {
		STM32_PWR->cr |= STM32_PWR_CR_ODEN;
		while (!(STM32_PWR->csr & STM32_PWR_CSR_ODRDY));
		STM32_PWR->cr |= STM32_PWR_CR_ODSWEN;
	}

	/*
	 * Restore RCC PLLs. Assume here that RDY bit is next after the
	 * appropriate ON bit in RCC CR register
	 */
	for (i = 0; i < ARRAY_SIZE(pll); i++) {
		if (!(stm32_pm_bck.rcc.cr & (1 << pll[i])))
			continue;
		STM32_RCC->cr |= 1 << pll[i];
		while (!(STM32_RCC->cr & (1 << (pll[i] + 1))));
	}
	STM32_RCC->cfgr &= ~STM32_RCC_CFGR_SW_MSK;
	STM32_RCC->cfgr |= stm32_pm_bck.rcc.cfgr;
	while ((STM32_RCC->cfgr & STM32_RCC_CFGR_SW_MSK) !=
		stm32_pm_bck.rcc.cfgr);
}

/*
 * Enter suspend
 * @state		State being entered
 * @returns		0->success, <0->error code
 */
static int stm32_pm_enter(suspend_state_t state)
{
	volatile u32 *scr = (void *)CM3_SCR_BASE;

	/*
	 * Prepare the system hardware to suspend
	 */
	stm32_pm_prepare_to_suspend();

	/*
	 * Allow STOP mode. Enter SLEEP DEEP on WFI.
	 */
	*scr |= CM3_SCR_SLEEPDEEP;

	/*
	 * Jump to suspend code in SRAM
	 */
	stm32_suspend_to_ram();

	/*
	 * Switch to Normal mode. Disable SLEEP DEEP on WFI.
	 */
	*scr &= ~CM3_SCR_SLEEPDEEP;

	/*
	 * Prepare the system hardware to resume
	 */
	stm32_pm_prepare_to_resume();

	return 0;
}

/*
 * Power Management operations
 */
static struct platform_suspend_ops stm32_pm_ops = {
	.valid = stm32_pm_valid,
	.enter = stm32_pm_enter,
};

/*
 * Device data structure
 */
static struct platform_driver stm32_pm_driver = {
	.driver = {
		   .name = "stm32_pm",
	},
};

static irqreturn_t stm32_pm_wakeup_handler(int irq, void *dev)
{
	return IRQ_HANDLED;
}

/*
 * Driver init
 * @returns		0->success, <0->error code
 */
static int __init stm32_pm_init(void)
{
	int i, ret;

	/*
	 * Initialize GPIOx_MODER we'll use in suspend: to get the minimal
	 * consumption we'll configure all GPIOs as Analog Inputs
	 */
	for (i = 0; i < STM32_GPIO_PORTS; i++)
		stm32_suspend_moder[i] = 0xFFFFFFFF;

	/*
	 * Exceptions are GPIOs which are used for WakeUp/Phy controls
	 */
	stm32_suspend_moder[STM32_WAKEUP_PORT] &= ~(3 << (STM32_WAKEUP_PIN *2));
	stm32_suspend_moder[STM32_WAKEUP_PORT] |= 0 << (STM32_WAKEUP_PIN * 2);

	stm32_suspend_moder[STM32_PHY_PORT] &= ~(3 << (STM32_PHY_PIN * 2));
	stm32_suspend_moder[STM32_PHY_PORT] |= 1 << (STM32_PHY_PIN * 2);

	/*
	 * Request PHY control GPIO
	 */
	if (stm32_platform_get() == PLATFORM_STM32_STM_STM32F7_SOM) {
		ret = gpio_request(STM32_PHY_GPIO, "PM");
		if (ret)
			printk(KERN_ERR "%s: gpio request failed\n", __func__);
	}

	/*
	 * Want the lowest consumption in Stop mode
	 */
	STM32_PWR->cr |= STM32_PWR_CR_UDEN | STM32_PWR_CR_LPUDS |
			 STM32_PWR_CR_FPDS | STM32_PWR_CR_LPDS;

	/*
	 * Specify IRQF_TIMER to avoid disabling this IRQ during
	 * suspend_device_irqs() procedure
	 */
	ret = request_irq(NVIC_IRQS + STM32_WAKEUP_GPIO,
			  stm32_pm_wakeup_handler,
			  IRQF_DISABLED | IRQF_TRIGGER_RISING | IRQF_TIMER,
			  "Wake-up GPIO (PA0)", &stm32_pm_driver);
	if (ret) {
		printk(KERN_ERR "%s: irq request failed\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	/*
	 * Register the PM driver
	 */
	if (platform_driver_register(&stm32_pm_driver) != 0) {
		printk(KERN_ERR "%s: register failed\n", __func__);
		ret = -ENODEV;
		goto out;
	}

	/*
	 * Register PM operations
	 */
	suspend_set_ops(&stm32_pm_ops);

	/*
	 * Here, means success
	 */
	printk(KERN_INFO "Power Management for STM32\n");
	ret = 0;
out:
	return ret;
}

/*
 * Driver clean-up
 */
static void __exit stm32_pm_cleanup(void)
{
	platform_driver_unregister(&stm32_pm_driver);
}

module_init(stm32_pm_init);
module_exit(stm32_pm_cleanup);

MODULE_AUTHOR("Yuri Tikhonov");
MODULE_DESCRIPTION("STM32 PM driver");
MODULE_LICENSE("GPL");

