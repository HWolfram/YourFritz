/* SPDX-License-Identifier: GPL-2.0-or-later */

/************************************************************************************************
 *                                                                                              *
 * @file        yf_patchkernel.c                                                                *
 * @see         https://www.ip-phone-forum.de/threads/fritz-os7-openvpn-auf-7590-kein-tun-      *
 *              modul.300433/page-3#post-2309487                                                *
 * @brief       patch kernel instructions while loading this module                             *
 * @version     0.2                                                                             *
 * @author      PeH                                                                             *
 * @date        17.01.2019                                                                      *
 *                                                                                              *
 ************************************************************************************************
 *                                                                                              *
 * Copyright (C) 2019 Peter Haemmerlein (peterpawn@yourfritz.de)                                *
 *                                                                                              *
 ************************************************************************************************
 *                                                                                              *
 * This project is free software, you can redistribute it and/or modify it under the terms of   *
 * the GNU General Public License (version 2) as published by the Free Software Foundation.     *
 *                                                                                              *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;    *
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.    *
 * See the GNU General Public License under https://www.gnu.org/licenses/gpl-2.0.html for more  *
 * details.                                                                                     *
 *                                                                                              *
 ************************************************************************************************
 *                                                                                              *
 * This loadable kernel module looks for machine instructions at specified locations in the     *
 * running kernel and replaces them (in case of a hit) with another instruction (only in-place  *
 * patches are supported).                                                                      *
 *                                                                                              *
 ************************************************************************************************
*/

#ifndef MODULE
#error yf_patchkernel has to be compiled as a loadable kernel module.
#endif

#if ! ( defined __MIPS__ || defined __mips__ )
#error yf_patchkernel supports only MIPS architecture yet.
#endif

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/skbuff.h>
#include <linux/kallsyms.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Peter Haemmerlein");
MODULE_DESCRIPTION("Patches some forgotten AVM traps on MIPS kernels.");
MODULE_VERSION("0.2");

#define MIPS_NOP       0x00000000 // it's a shift instruction, which does nothing: sll zero, zero, 0
#define MIPS_ADDIU     0x24000000 // add immediate value to RS and store the result in RT
#define MIPS_LW        0x8C000000 // load word from offset to BASE and store it in RT
#define MIPS_TNE       0x00000036 // trap if RS not equal RT
#define MIPS_BASE_MASK 0x03E00000 // base register bits (bits 21 to 26)
#define MIPS_RS_MASK   0x03E00000 // RS register bits (bits 21 to 26) - same as BASE
#define MIPS_RT_MASK   0x001F0000 // RT register bits (bits 16 to 20)
#define MIPS_OFFS_MASK 0x0000FFFF // offset bits in the used instructions (16 bits value)
#define MIPS_BASE_SHFT 21         // base register bits shifted left
#define MIPS_RS_SHFT   21         // RS register bits shifted left
#define MIPS_RT_SHFT   16         // RT register bits shifted left
#define MIPS_REG_V0    2          // register v0
#define MIPS_REG_V1    3          // register v1
#define MIPS_REG_A0    4          // register a0
#define MIPS_TRAP_CODE 0x00000300 // trap code 12 (encoded in bits 6 to 15)
#define MIPS_AND_MASK  0xFFFFFFFF // all bits set for logical AND mask

#define YF_INFO(args...) pr_info("[%s] ",__this_module.name);pr_cont(args)

typedef struct patchEntry
{
	unsigned char   *fname;         // kernel symbol name, where to start with a search
	unsigned int    *startAddress;  // the result from kallsyms_lookup_name for the above symbol
	unsigned int    startOffset;    // number of instructions (32 bits per instruction) to skip prior to first comparision
	unsigned int    maxOffset;      // maximum number of instructions to process, while searching for this patch
	unsigned int    lookFor;        // the value to look for, the source value will be modified by AND and OR masks first (see below)
	unsigned int    andMask;        // the mask to use for a logical AND operation, may be used to mask out unwanted bits from value
	unsigned int    orMask;         // the mask to use for a logical OR operation, may be used as a mask to set some additional bits or to ensure, they're set already
	unsigned int    verifyOffset;   // the offset of another value to check, if the search from above was successful, if it's 0, no further check is performed
	unsigned int    verifyValue;    // the expected value from verification, after processing AND and OR operations with masks below
	unsigned int    verifyAndMask;  // the AND mask for verification
	unsigned int    verifyOrMask;   // the OR mask for verification
	unsigned int    patchOffset;    // the offset of instruction to patch, relative to the search result (not to verification offset)
	unsigned int    patchValue;     // the new value to store at patched location
	unsigned int    *patchAddress;  // the address, where the change was applied
	unsigned int    originalValue;  // the original value prior to patching
	int             isPatched;      // not zero, if this patch was applied successfully
} patchEntry_t;

static unsigned int yf_patchkernel_patch(patchEntry_t *);
static void yf_patchkernel_restore(patchEntry_t *);

// entries to patch for TUN device on 7490/75x0 devices, starting with FRITZ!OS version 07.0x

static patchEntry_t patchesForTunDevice[] = {
	{
		.fname = "ip_forward",
		.maxOffset = 10,
		.lookFor = MIPS_LW + (MIPS_REG_A0 << MIPS_BASE_SHFT) + offsetof(struct sk_buff, sk),
		.andMask = MIPS_AND_MASK - MIPS_RT_MASK,
		.patchValue = MIPS_ADDIU + (MIPS_REG_V0 << MIPS_RT_SHFT)
	},
	{
		.fname = "netif_receive_skb",
		.maxOffset = 10,
		.lookFor = MIPS_LW + (MIPS_REG_A0 << MIPS_BASE_SHFT) + offsetof(struct sk_buff, sk),
		.andMask = MIPS_AND_MASK - MIPS_RT_MASK,
		.verifyOffset = 1,
		.verifyValue = MIPS_TNE + MIPS_TRAP_CODE,
		.verifyAndMask = MIPS_AND_MASK - MIPS_RT_MASK,
		.patchOffset = 1,
		.patchValue = MIPS_NOP
	},
	{
		.fname = "__netif_receive_skb",
		.maxOffset = 8,
		.lookFor = MIPS_LW + (MIPS_REG_A0 << MIPS_BASE_SHFT) + offsetof(struct sk_buff, sk),
		.andMask = MIPS_AND_MASK - MIPS_RT_MASK,
		.verifyOffset = 1,
		.verifyValue = MIPS_TNE + MIPS_TRAP_CODE,
		.verifyAndMask = MIPS_AND_MASK - MIPS_RT_MASK,
		.patchOffset = 1,
		.patchValue = MIPS_NOP
	},
	{
		.fname = NULL		// last entry needed as 'end of list' marker
	}
};

static unsigned int	patches_applied = 0;	// number of patches applied successfully

static unsigned int yf_patchkernel_patch(patchEntry_t *patches)
{
	unsigned int	patches_applied = 0;
	patchEntry_t	*patch = patches;

	unsigned int	*ptr;
	unsigned int	offset;
	unsigned int	value;
	unsigned int	orgValue;
	unsigned int	verify;

	while (patch->fname)
	{
		ptr = (unsigned int *)kallsyms_lookup_name(patch->fname);

		if (!ptr)
		{
			YF_INFO("Unable to locate kernel symbol '%s', patch skipped.\n", patch->fname);
		}
		else
		{
			YF_INFO("Patching kernel function '%s' at address %#010x.\n", patch->fname, (unsigned int)ptr);

			for (offset = 0, patch->startAddress = ptr, ptr += patch->startOffset; offset < patch->maxOffset; offset++, ptr++)
			{
				value = (*ptr & patch->andMask) | patch->orMask;
				orgValue = *(ptr + patch->patchOffset);

				if (orgValue == patch->patchValue)
				{
					YF_INFO("Found patched instruction (%#010x) at address %#010x, looks like this patch was applied already or is not necessary.\n", orgValue, (unsigned int)(ptr + patch->patchOffset));
					break;
				}

				if (value == patch->lookFor)
				{
					if (patch->verifyOffset != 0)
					{
						verify = (*(ptr + patch->verifyOffset) & patch->verifyAndMask) | patch->verifyOrMask;
						if (verify != patch->verifyValue) continue;
					}

					patch->patchAddress = ptr + patch->patchOffset;
					patch->originalValue = *(patch->patchAddress);
					*(patch->patchAddress) = patch->patchValue;
					patch->isPatched = 1;
					patches_applied++;

					YF_INFO("Found instruction to patch (%#010x) at address %#010x, replaced it with %#010x.\n", patch->originalValue, (unsigned int)(patch->patchAddress), *(patch->patchAddress));

					break;
				}
			}

			if (!(patch->isPatched))
			{
				YF_INFO("No instruction to patch found in function '%s', patch skipped.\n", patch->fname);
			}
		}
		patch++;
	}

	return patches_applied;
}

static void yf_patchkernel_restore(patchEntry_t *patch)
{
	while (patch->fname)
	{
		if (patch->isPatched)
		{
			*(patch->patchAddress) = patch->originalValue;
			patch->isPatched = 0;

			YF_INFO("Reversed patch in '%s' at address %#010x to original value %#010x.\n", patch->fname, (unsigned int)(patch->patchAddress), patch->originalValue);
		}

		patch++;
	}
}

static int __init yf_patchkernel_init(void)
{
	YF_INFO("Initialization started\n");
	YF_INFO("Any preceding error messages regarding memory allocation are expected and may be ignored.\n");

	patches_applied = yf_patchkernel_patch(patchesForTunDevice);

	YF_INFO("%u patches applied.\n", patches_applied);

	return 0;
}

static void __exit yf_patchkernel_exit(void)
{
	YF_INFO("Module will be removed now.\n");

	yf_patchkernel_restore(patchesForTunDevice);

	YF_INFO("All applied patches have been reversed.\n");
}

module_init(yf_patchkernel_init);
module_exit(yf_patchkernel_exit);
