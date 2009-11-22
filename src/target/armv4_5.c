/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2008 by Oyvind Harboe                                   *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "armv4_5.h"
#include "arm_jtag.h"
#include "breakpoints.h"
#include "arm_disassembler.h"
#include "binarybuffer.h"
#include "algorithm.h"
#include "register.h"


/* offsets into armv4_5 core register cache */
enum {
//	ARMV4_5_CPSR = 31,
	ARMV4_5_SPSR_FIQ = 32,
	ARMV4_5_SPSR_IRQ = 33,
	ARMV4_5_SPSR_SVC = 34,
	ARMV4_5_SPSR_ABT = 35,
	ARMV4_5_SPSR_UND = 36,
	ARM_SPSR_MON = 39,
};

static const uint8_t arm_usr_indices[17] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, ARMV4_5_CPSR,
};

static const uint8_t arm_fiq_indices[8] = {
	16, 17, 18, 19, 20, 21, 22, ARMV4_5_SPSR_FIQ,
};

static const uint8_t arm_irq_indices[3] = {
	23, 24, ARMV4_5_SPSR_IRQ,
};

static const uint8_t arm_svc_indices[3] = {
	25, 26, ARMV4_5_SPSR_SVC,
};

static const uint8_t arm_abt_indices[3] = {
	27, 28, ARMV4_5_SPSR_ABT,
};

static const uint8_t arm_und_indices[3] = {
	29, 30, ARMV4_5_SPSR_UND,
};

static const uint8_t arm_mon_indices[3] = {
	37, 38, ARM_SPSR_MON,
};

static const struct {
	const char *name;
	unsigned short psr;
	/* For user and system modes, these list indices for all registers.
	 * otherwise they're just indices for the shadow registers and SPSR.
	 */
	unsigned short n_indices;
	const uint8_t *indices;
} arm_mode_data[] = {
	/* Seven modes are standard from ARM7 on. "System" and "User" share
	 * the same registers; other modes shadow from 3 to 8 registers.
	 */
	{
		.name = "User",
		.psr = ARMV4_5_MODE_USR,
		.n_indices = ARRAY_SIZE(arm_usr_indices),
		.indices = arm_usr_indices,
	},
	{
		.name = "FIQ",
		.psr = ARMV4_5_MODE_FIQ,
		.n_indices = ARRAY_SIZE(arm_fiq_indices),
		.indices = arm_fiq_indices,
	},
	{
		.name = "Supervisor",
		.psr = ARMV4_5_MODE_SVC,
		.n_indices = ARRAY_SIZE(arm_svc_indices),
		.indices = arm_svc_indices,
	},
	{
		.name = "Abort",
		.psr = ARMV4_5_MODE_ABT,
		.n_indices = ARRAY_SIZE(arm_abt_indices),
		.indices = arm_abt_indices,
	},
	{
		.name = "IRQ",
		.psr = ARMV4_5_MODE_IRQ,
		.n_indices = ARRAY_SIZE(arm_irq_indices),
		.indices = arm_irq_indices,
	},
	{
		.name = "Undefined instruction",
		.psr = ARMV4_5_MODE_UND,
		.n_indices = ARRAY_SIZE(arm_und_indices),
		.indices = arm_und_indices,
	},
	{
		.name = "System",
		.psr = ARMV4_5_MODE_SYS,
		.n_indices = ARRAY_SIZE(arm_usr_indices),
		.indices = arm_usr_indices,
	},
	/* TrustZone "Security Extensions" add a secure monitor mode.
	 * This is distinct from a "debug monitor" which can support
	 * non-halting debug, in conjunction with some debuggers.
	 */
	{
		.name = "Secure Monitor",
		.psr = ARM_MODE_MON,
		.n_indices = ARRAY_SIZE(arm_mon_indices),
		.indices = arm_mon_indices,
	},
};

/** Map PSR mode bits to the name of an ARM processor operating mode. */
const char *arm_mode_name(unsigned psr_mode)
{
	for (unsigned i = 0; i < ARRAY_SIZE(arm_mode_data); i++) {
		if (arm_mode_data[i].psr == psr_mode)
			return arm_mode_data[i].name;
	}
	LOG_ERROR("unrecognized psr mode: %#02x", psr_mode);
	return "UNRECOGNIZED";
}

/** Return true iff the parameter denotes a valid ARM processor mode. */
bool is_arm_mode(unsigned psr_mode)
{
	for (unsigned i = 0; i < ARRAY_SIZE(arm_mode_data); i++) {
		if (arm_mode_data[i].psr == psr_mode)
			return true;
	}
	return false;
}

/** Map PSR mode bits to linear number indexing armv4_5_core_reg_map */
int armv4_5_mode_to_number(enum armv4_5_mode mode)
{
	switch (mode) {
	case ARMV4_5_MODE_ANY:
		/* map MODE_ANY to user mode */
	case ARMV4_5_MODE_USR:
		return 0;
	case ARMV4_5_MODE_FIQ:
		return 1;
	case ARMV4_5_MODE_IRQ:
		return 2;
	case ARMV4_5_MODE_SVC:
		return 3;
	case ARMV4_5_MODE_ABT:
		return 4;
	case ARMV4_5_MODE_UND:
		return 5;
	case ARMV4_5_MODE_SYS:
		return 6;
	case ARM_MODE_MON:
		return 7;
	default:
		LOG_ERROR("invalid mode value encountered %d", mode);
		return -1;
	}
}

/** Map linear number indexing armv4_5_core_reg_map to PSR mode bits. */
enum armv4_5_mode armv4_5_number_to_mode(int number)
{
	switch (number) {
	case 0:
		return ARMV4_5_MODE_USR;
	case 1:
		return ARMV4_5_MODE_FIQ;
	case 2:
		return ARMV4_5_MODE_IRQ;
	case 3:
		return ARMV4_5_MODE_SVC;
	case 4:
		return ARMV4_5_MODE_ABT;
	case 5:
		return ARMV4_5_MODE_UND;
	case 6:
		return ARMV4_5_MODE_SYS;
	case 7:
		return ARM_MODE_MON;
	default:
		LOG_ERROR("mode index out of bounds %d", number);
		return ARMV4_5_MODE_ANY;
	}
}

char* armv4_5_state_strings[] =
{
	"ARM", "Thumb", "Jazelle", "ThumbEE",
};

/* Templates for ARM core registers.
 *
 * NOTE:  offsets in this table are coupled to the arm_mode_data
 * table above, the armv4_5_core_reg_map array below, and also to
 * the ARMV4_5_CPSR symbol (which should vanish after ARM11 updates).
 */
static const struct {
	/* The name is used for e.g. the "regs" command. */
	const char *name;

	/* The {cookie, mode} tuple uniquely identifies one register.
	 * In a given mode, cookies 0..15 map to registers R0..R15,
	 * with R13..R15 usually called SP, LR, PC.
	 *
	 * MODE_ANY is used as *input* to the mapping, and indicates
	 * various special cases (sigh) and errors.
	 *
	 * Cookie 16 is (currently) confusing, since it indicates
	 * CPSR -or- SPSR depending on whether 'mode' is MODE_ANY.
	 * (Exception modes have both CPSR and SPSR registers ...)
	 */
	unsigned cookie;
	enum armv4_5_mode mode;
} arm_core_regs[] = {
	{ .name = "r0", .cookie = 0, .mode = ARMV4_5_MODE_ANY, },
	{ .name = "r1", .cookie = 1, .mode = ARMV4_5_MODE_ANY, },
	{ .name = "r2", .cookie = 2, .mode = ARMV4_5_MODE_ANY, },
	{ .name = "r3", .cookie = 3, .mode = ARMV4_5_MODE_ANY, },
	{ .name = "r4", .cookie = 4, .mode = ARMV4_5_MODE_ANY, },
	{ .name = "r5", .cookie = 5, .mode = ARMV4_5_MODE_ANY, },
	{ .name = "r6", .cookie = 6, .mode = ARMV4_5_MODE_ANY, },
	{ .name = "r7", .cookie = 7, .mode = ARMV4_5_MODE_ANY, },

	/* NOTE: regs 8..12 might be shadowed by FIQ ... flagging
	 * them as MODE_ANY creates special cases.
	 */
	{ .name = "r8", .cookie = 8, .mode = ARMV4_5_MODE_ANY, },
	{ .name = "r9", .cookie = 9, .mode = ARMV4_5_MODE_ANY, },
	{ .name = "r10", .cookie = 10, .mode = ARMV4_5_MODE_ANY, },
	{ .name = "r11", .cookie = 11, .mode = ARMV4_5_MODE_ANY, },
	{ .name = "r12", .cookie = 12, .mode = ARMV4_5_MODE_ANY, },

	/* NOTE all MODE_USR registers are equivalent to MODE_SYS ones */
	{ .name = "sp_usr", .cookie = 13, .mode = ARMV4_5_MODE_USR, },
	{ .name = "lr_usr", .cookie = 14, .mode = ARMV4_5_MODE_USR, },

	{ .name = "pc", .cookie = 15, .mode = ARMV4_5_MODE_ANY, },

	{ .name = "r8_fiq", .cookie = 8, .mode = ARMV4_5_MODE_FIQ, },
	{ .name = "r9_fiq", .cookie = 9, .mode = ARMV4_5_MODE_FIQ, },
	{ .name = "r10_fiq", .cookie = 10, .mode = ARMV4_5_MODE_FIQ, },
	{ .name = "r11_fiq", .cookie = 11, .mode = ARMV4_5_MODE_FIQ, },
	{ .name = "r12_fiq", .cookie = 12, .mode = ARMV4_5_MODE_FIQ, },

	{ .name = "lr_fiq", .cookie = 13, .mode = ARMV4_5_MODE_FIQ, },
	{ .name = "sp_fiq", .cookie = 14, .mode = ARMV4_5_MODE_FIQ, },

	{ .name = "lr_irq", .cookie = 13, .mode = ARMV4_5_MODE_IRQ, },
	{ .name = "sp_irq", .cookie = 14, .mode = ARMV4_5_MODE_IRQ, },

	{ .name = "lr_svc", .cookie = 13, .mode = ARMV4_5_MODE_SVC, },
	{ .name = "sp_svc", .cookie = 14, .mode = ARMV4_5_MODE_SVC, },

	{ .name = "lr_abt", .cookie = 13, .mode = ARMV4_5_MODE_ABT, },
	{ .name = "sp_abt", .cookie = 14, .mode = ARMV4_5_MODE_ABT, },

	{ .name = "lr_und", .cookie = 13, .mode = ARMV4_5_MODE_UND, },
	{ .name = "sp_und", .cookie = 14, .mode = ARMV4_5_MODE_UND, },

	{ .name = "cpsr", .cookie = 16, .mode = ARMV4_5_MODE_ANY, },
	{ .name = "spsr_fiq", .cookie = 16, .mode = ARMV4_5_MODE_FIQ, },
	{ .name = "spsr_irq", .cookie = 16, .mode = ARMV4_5_MODE_IRQ, },
	{ .name = "spsr_svc", .cookie = 16, .mode = ARMV4_5_MODE_SVC, },
	{ .name = "spsr_abt", .cookie = 16, .mode = ARMV4_5_MODE_ABT, },
	{ .name = "spsr_und", .cookie = 16, .mode = ARMV4_5_MODE_UND, },

	{ .name = "lr_mon", .cookie = 13, .mode = ARM_MODE_MON, },
	{ .name = "sp_mon", .cookie = 14, .mode = ARM_MODE_MON, },
	{ .name = "spsr_mon", .cookie = 16, .mode = ARM_MODE_MON, },
};

/* map core mode (USR, FIQ, ...) and register number to
 * indices into the register cache
 */
const int armv4_5_core_reg_map[8][17] =
{
	{	/* USR */
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 31
	},
	{	/* FIQ (8 shadows of USR, vs normal 3) */
		0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 15, 32
	},
	{	/* IRQ */
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 23, 24, 15, 33
	},
	{	/* SVC */
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 25, 26, 15, 34
	},
	{	/* ABT */
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 27, 28, 15, 35
	},
	{	/* UND */
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 29, 30, 15, 36
	},
	{	/* SYS (same registers as USR) */
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 31
	},
	{	/* MON */
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 37, 38, 15, 39,
	}
};

static const uint8_t arm_gdb_dummy_fp_value[12];

/**
 * Dummy FPA registers are required to support GDB on ARM.
 * Register packets require eight obsolete FPA register values.
 * Modern ARM cores use Vector Floating Point (VFP), if they
 * have any floating point support.  VFP is not FPA-compatible.
 */
struct reg arm_gdb_dummy_fp_reg =
{
	.name = "GDB dummy FPA register",
	.value = (uint8_t *) arm_gdb_dummy_fp_value,
	.valid = 1,
	.size = 96,
};

static const uint8_t arm_gdb_dummy_fps_value[4];

/**
 * Dummy FPA status registers are required to support GDB on ARM.
 * Register packets require an obsolete FPA status register.
 */
struct reg arm_gdb_dummy_fps_reg =
{
	.name = "GDB dummy FPA status register",
	.value = (uint8_t *) arm_gdb_dummy_fps_value,
	.valid = 1,
	.size = 32,
};

static void arm_gdb_dummy_init(void) __attribute__ ((constructor));

static void arm_gdb_dummy_init(void)
{
	register_init_dummy(&arm_gdb_dummy_fp_reg);
	register_init_dummy(&arm_gdb_dummy_fps_reg);
}

static int armv4_5_get_core_reg(struct reg *reg)
{
	int retval;
	struct arm_reg *armv4_5 = reg->arch_info;
	struct target *target = armv4_5->target;

	if (target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = armv4_5->armv4_5_common->read_core_reg(target, reg, armv4_5->num, armv4_5->mode);
	if (retval == ERROR_OK) {
		reg->valid = 1;
		reg->dirty = 0;
	}

	return retval;
}

static int armv4_5_set_core_reg(struct reg *reg, uint8_t *buf)
{
	struct arm_reg *armv4_5 = reg->arch_info;
	struct target *target = armv4_5->target;
	struct armv4_5_common_s *armv4_5_target = target_to_armv4_5(target);
	uint32_t value = buf_get_u32(buf, 0, 32);

	if (target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* Except for CPSR, the "reg" command exposes a writeback model
	 * for the register cache.
	 */
	buf_set_u32(reg->value, 0, 32, value);
	reg->dirty = 1;
	reg->valid = 1;

	if (reg == armv4_5_target->cpsr)
	{
		/* FIXME handle J bit too; mostly for ThumbEE, also Jazelle */
		if (value & 0x20)
		{
			/* T bit should be set */
			if (armv4_5_target->core_state == ARMV4_5_STATE_ARM)
			{
				/* change state to Thumb */
				LOG_DEBUG("changing to Thumb state");
				armv4_5_target->core_state = ARMV4_5_STATE_THUMB;
			}
		}
		else
		{
			/* T bit should be cleared */
			if (armv4_5_target->core_state == ARMV4_5_STATE_THUMB)
			{
				/* change state to ARM */
				LOG_DEBUG("changing to ARM state");
				armv4_5_target->core_state = ARMV4_5_STATE_ARM;
			}
		}

		/* REVISIT Why only update core for mode change, not also
		 * for state changes?  Possibly older cores need to stay
		 * in ARM mode during halt mode debug, not execute Thumb;
		 * v6/v7a/v7r seem to do that automatically...
		 */

		if (armv4_5_target->core_mode != (enum armv4_5_mode)(value & 0x1f))
		{
			LOG_DEBUG("changing ARM core mode to '%s'",
					arm_mode_name(value & 0x1f));
			armv4_5_target->core_mode = value & 0x1f;
			armv4_5_target->write_core_reg(target, reg,
					16, ARMV4_5_MODE_ANY, value);
			reg->dirty = 0;
		}
	}

	return ERROR_OK;
}

static const struct reg_arch_type arm_reg_type = {
	.get = armv4_5_get_core_reg,
	.set = armv4_5_set_core_reg,
};

struct reg_cache* armv4_5_build_reg_cache(struct target *target, struct arm *armv4_5_common)
{
	int num_regs = ARRAY_SIZE(arm_core_regs);
	struct reg_cache *cache = malloc(sizeof(struct reg_cache));
	struct reg *reg_list = calloc(num_regs, sizeof(struct reg));
	struct arm_reg *arch_info = calloc(num_regs, sizeof(struct arm_reg));
	int i;

	if (!cache || !reg_list || !arch_info) {
		free(cache);
		free(reg_list);
		free(arch_info);
		return NULL;
	}

	cache->name = "ARM registers";
	cache->next = NULL;
	cache->reg_list = reg_list;
	cache->num_regs = 0;

	for (i = 0; i < num_regs; i++)
	{
		/* Skip registers this core doesn't expose */
		if (arm_core_regs[i].mode == ARM_MODE_MON
				&& armv4_5_common->core_type != ARM_MODE_MON)
			continue;

		/* REVISIT handle Cortex-M, which only shadows R13/SP */

		arch_info[i].num = arm_core_regs[i].cookie;
		arch_info[i].mode = arm_core_regs[i].mode;
		arch_info[i].target = target;
		arch_info[i].armv4_5_common = armv4_5_common;

		reg_list[i].name = (char *) arm_core_regs[i].name;
		reg_list[i].size = 32;
		reg_list[i].value = &arch_info[i].value;
		reg_list[i].type = &arm_reg_type;
		reg_list[i].arch_info = &arch_info[i];

		cache->num_regs++;
	}

	armv4_5_common->cpsr = reg_list + ARMV4_5_CPSR;
	armv4_5_common->core_cache = cache;
	return cache;
}

int armv4_5_arch_state(struct target *target)
{
	struct armv4_5_common_s *armv4_5 = target_to_armv4_5(target);

	if (armv4_5->common_magic != ARMV4_5_COMMON_MAGIC)
	{
		LOG_ERROR("BUG: called for a non-ARMv4/5 target");
		return ERROR_FAIL;
	}

	LOG_USER("target halted in %s state due to %s, current mode: %s\ncpsr: 0x%8.8" PRIx32 " pc: 0x%8.8" PRIx32 "",
			 armv4_5_state_strings[armv4_5->core_state],
			 Jim_Nvp_value2name_simple(nvp_target_debug_reason, target->debug_reason)->name,
			 arm_mode_name(armv4_5->core_mode),
			 buf_get_u32(armv4_5->cpsr->value, 0, 32),
			 buf_get_u32(armv4_5->core_cache->reg_list[15].value, 0, 32));

	return ERROR_OK;
}

#define ARMV4_5_CORE_REG_MODENUM(cache, mode, num) \
		cache->reg_list[armv4_5_core_reg_map[mode][num]]

COMMAND_HANDLER(handle_armv4_5_reg_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct armv4_5_common_s *armv4_5 = target_to_armv4_5(target);
	unsigned num_regs;
	struct reg *regs;

	if (!is_arm(armv4_5))
	{
		command_print(CMD_CTX, "current target isn't an ARM");
		return ERROR_FAIL;
	}

	if (target->state != TARGET_HALTED)
	{
		command_print(CMD_CTX, "error: target must be halted for register accesses");
		return ERROR_FAIL;
	}

	if (!is_arm_mode(armv4_5->core_mode))
		return ERROR_FAIL;

	if (!armv4_5->full_context) {
		command_print(CMD_CTX, "error: target doesn't support %s",
				CMD_NAME);
		return ERROR_FAIL;
	}

	num_regs = armv4_5->core_cache->num_regs;
	regs = armv4_5->core_cache->reg_list;

	for (unsigned mode = 0; mode < ARRAY_SIZE(arm_mode_data); mode++) {
		const char *name;
		char *sep = "\n";
		char *shadow = "";

		/* label this bank of registers (or shadows) */
		switch (arm_mode_data[mode].psr) {
		case ARMV4_5_MODE_SYS:
			continue;
		case ARMV4_5_MODE_USR:
			name = "System and User";
			sep = "";
			break;
		case ARM_MODE_MON:
			if (armv4_5->core_type != ARM_MODE_MON)
				continue;
			/* FALLTHROUGH */
		default:
			name = arm_mode_data[mode].name;
			shadow = "shadow ";
			break;
		}
		command_print(CMD_CTX, "%s%s mode %sregisters",
				sep, name, shadow);

		/* display N rows of up to 4 registers each */
		for (unsigned i = 0; i < arm_mode_data[mode].n_indices;) {
			char output[80];
			int output_len = 0;

			for (unsigned j = 0; j < 4; j++, i++) {
				uint32_t value;
				struct reg *reg = regs;

				if (i >= arm_mode_data[mode].n_indices)
					break;

				reg += arm_mode_data[mode].indices[i];

				/* REVISIT be smarter about faults... */
				if (!reg->valid)
					armv4_5->full_context(target);

				value = buf_get_u32(reg->value, 0, 32);
				output_len += snprintf(output + output_len,
						sizeof(output) - output_len,
					       "%8s: %8.8" PRIx32 " ",
					       reg->name, value);
			}
			command_print(CMD_CTX, "%s", output);
		}
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_armv4_5_core_state_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct armv4_5_common_s *armv4_5 = target_to_armv4_5(target);

	if (!is_arm(armv4_5))
	{
		command_print(CMD_CTX, "current target isn't an ARM");
		return ERROR_FAIL;
	}

	if (CMD_ARGC > 0)
	{
		if (strcmp(CMD_ARGV[0], "arm") == 0)
		{
			armv4_5->core_state = ARMV4_5_STATE_ARM;
		}
		if (strcmp(CMD_ARGV[0], "thumb") == 0)
		{
			armv4_5->core_state = ARMV4_5_STATE_THUMB;
		}
	}

	command_print(CMD_CTX, "core state: %s", armv4_5_state_strings[armv4_5->core_state]);

	return ERROR_OK;
}

COMMAND_HANDLER(handle_armv4_5_disassemble_command)
{
	int retval = ERROR_OK;
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target ? target_to_arm(target) : NULL;
	uint32_t address;
	int count = 1;
	int thumb = 0;

	if (!is_arm(arm)) {
		command_print(CMD_CTX, "current target isn't an ARM");
		return ERROR_FAIL;
	}

	switch (CMD_ARGC) {
	case 3:
		if (strcmp(CMD_ARGV[2], "thumb") != 0)
			goto usage;
		thumb = 1;
		/* FALL THROUGH */
	case 2:
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[1], count);
		/* FALL THROUGH */
	case 1:
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], address);
		if (address & 0x01) {
			if (!thumb) {
				command_print(CMD_CTX, "Disassemble as Thumb");
				thumb = 1;
			}
			address &= ~1;
		}
		break;
	default:
usage:
		command_print(CMD_CTX,
			"usage: arm disassemble <address> [<count> ['thumb']]");
		count = 0;
		retval = ERROR_FAIL;
	}

	while (count-- > 0) {
		struct arm_instruction cur_instruction;

		if (thumb) {
			/* Always use Thumb2 disassembly for best handling
			 * of 32-bit BL/BLX, and to work with newer cores
			 * (some ARMv6, all ARMv7) that use Thumb2.
			 */
			retval = thumb2_opcode(target, address,
					&cur_instruction);
			if (retval != ERROR_OK)
				break;
		} else {
			uint32_t opcode;

			retval = target_read_u32(target, address, &opcode);
			if (retval != ERROR_OK)
				break;
			retval = arm_evaluate_opcode(opcode, address,
					&cur_instruction) != ERROR_OK;
			if (retval != ERROR_OK)
				break;
		}
		command_print(CMD_CTX, "%s", cur_instruction.text);
		address += cur_instruction.instruction_size;
	}

	return retval;
}

int armv4_5_register_commands(struct command_context *cmd_ctx)
{
	struct command *armv4_5_cmd;

	armv4_5_cmd = register_command(cmd_ctx, NULL, "arm",
			NULL, COMMAND_ANY,
			"generic ARM commands");

	register_command(cmd_ctx, armv4_5_cmd, "reg",
			handle_armv4_5_reg_command, COMMAND_EXEC,
			"display ARM core registers");
	register_command(cmd_ctx, armv4_5_cmd, "core_state",
			handle_armv4_5_core_state_command, COMMAND_EXEC,
			"display/change ARM core state <arm | thumb>");
	register_command(cmd_ctx, armv4_5_cmd, "disassemble",
			handle_armv4_5_disassemble_command, COMMAND_EXEC,
			"disassemble instructions "
				"<address> [<count> ['thumb']]");

	return ERROR_OK;
}

int armv4_5_get_gdb_reg_list(struct target *target, struct reg **reg_list[], int *reg_list_size)
{
	struct armv4_5_common_s *armv4_5 = target_to_armv4_5(target);
	int i;

	if (!is_arm_mode(armv4_5->core_mode))
		return ERROR_FAIL;

	*reg_list_size = 26;
	*reg_list = malloc(sizeof(struct reg*) * (*reg_list_size));

	for (i = 0; i < 16; i++)
	{
		(*reg_list)[i] = &ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5->core_mode, i);
	}

	for (i = 16; i < 24; i++)
	{
		(*reg_list)[i] = &arm_gdb_dummy_fp_reg;
	}

	(*reg_list)[24] = &arm_gdb_dummy_fps_reg;
	(*reg_list)[25] = armv4_5->cpsr;

	return ERROR_OK;
}

/* wait for execution to complete and check exit point */
static int armv4_5_run_algorithm_completion(struct target *target, uint32_t exit_point, int timeout_ms, void *arch_info)
{
	int retval;
	struct armv4_5_common_s *armv4_5 = target_to_armv4_5(target);

	if ((retval = target_wait_state(target, TARGET_HALTED, timeout_ms)) != ERROR_OK)
	{
		return retval;
	}
	if (target->state != TARGET_HALTED)
	{
		if ((retval = target_halt(target)) != ERROR_OK)
			return retval;
		if ((retval = target_wait_state(target, TARGET_HALTED, 500)) != ERROR_OK)
		{
			return retval;
		}
		return ERROR_TARGET_TIMEOUT;
	}

	/* fast exit: ARMv5+ code can use BKPT */
	if (exit_point && buf_get_u32(armv4_5->core_cache->reg_list[15].value,
				0, 32) != exit_point)
	{
		LOG_WARNING("target reentered debug state, but not at the desired exit point: 0x%4.4" PRIx32 "",
			buf_get_u32(armv4_5->core_cache->reg_list[15].value, 0, 32));
		return ERROR_TARGET_TIMEOUT;
	}

	return ERROR_OK;
}

int armv4_5_run_algorithm_inner(struct target *target, int num_mem_params, struct mem_param *mem_params, int num_reg_params, struct reg_param *reg_params, uint32_t entry_point, uint32_t exit_point, int timeout_ms, void *arch_info, int (*run_it)(struct target *target, uint32_t exit_point, int timeout_ms, void *arch_info))
{
	struct armv4_5_common_s *armv4_5 = target_to_armv4_5(target);
	struct armv4_5_algorithm *armv4_5_algorithm_info = arch_info;
	enum armv4_5_state core_state = armv4_5->core_state;
	enum armv4_5_mode core_mode = armv4_5->core_mode;
	uint32_t context[17];
	uint32_t cpsr;
	int exit_breakpoint_size = 0;
	int i;
	int retval = ERROR_OK;
	LOG_DEBUG("Running algorithm");

	if (armv4_5_algorithm_info->common_magic != ARMV4_5_COMMON_MAGIC)
	{
		LOG_ERROR("current target isn't an ARMV4/5 target");
		return ERROR_TARGET_INVALID;
	}

	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!is_arm_mode(armv4_5->core_mode))
		return ERROR_FAIL;

	/* armv5 and later can terminate with BKPT instruction; less overhead */
	if (!exit_point && armv4_5->is_armv4)
	{
		LOG_ERROR("ARMv4 target needs HW breakpoint location");
		return ERROR_FAIL;
	}

	for (i = 0; i <= 16; i++)
	{
		struct reg *r;

		r = &ARMV4_5_CORE_REG_MODE(armv4_5->core_cache,
				armv4_5_algorithm_info->core_mode, i);
		if (!r->valid)
			armv4_5->read_core_reg(target, r, i,
					armv4_5_algorithm_info->core_mode);
		context[i] = buf_get_u32(r->value, 0, 32);
	}
	cpsr = buf_get_u32(armv4_5->cpsr->value, 0, 32);

	for (i = 0; i < num_mem_params; i++)
	{
		if ((retval = target_write_buffer(target, mem_params[i].address, mem_params[i].size, mem_params[i].value)) != ERROR_OK)
		{
			return retval;
		}
	}

	for (i = 0; i < num_reg_params; i++)
	{
		struct reg *reg = register_get_by_name(armv4_5->core_cache, reg_params[i].reg_name, 0);
		if (!reg)
		{
			LOG_ERROR("BUG: register '%s' not found", reg_params[i].reg_name);
			return ERROR_INVALID_ARGUMENTS;
		}

		if (reg->size != reg_params[i].size)
		{
			LOG_ERROR("BUG: register '%s' size doesn't match reg_params[i].size", reg_params[i].reg_name);
			return ERROR_INVALID_ARGUMENTS;
		}

		if ((retval = armv4_5_set_core_reg(reg, reg_params[i].value)) != ERROR_OK)
		{
			return retval;
		}
	}

	armv4_5->core_state = armv4_5_algorithm_info->core_state;
	if (armv4_5->core_state == ARMV4_5_STATE_ARM)
		exit_breakpoint_size = 4;
	else if (armv4_5->core_state == ARMV4_5_STATE_THUMB)
		exit_breakpoint_size = 2;
	else
	{
		LOG_ERROR("BUG: can't execute algorithms when not in ARM or Thumb state");
		return ERROR_INVALID_ARGUMENTS;
	}

	if (armv4_5_algorithm_info->core_mode != ARMV4_5_MODE_ANY)
	{
		LOG_DEBUG("setting core_mode: 0x%2.2x",
				armv4_5_algorithm_info->core_mode);
		buf_set_u32(armv4_5->cpsr->value, 0, 5,
				armv4_5_algorithm_info->core_mode);
		armv4_5->cpsr->dirty = 1;
		armv4_5->cpsr->valid = 1;
	}

	/* terminate using a hardware or (ARMv5+) software breakpoint */
	if (exit_point && (retval = breakpoint_add(target, exit_point,
				exit_breakpoint_size, BKPT_HARD)) != ERROR_OK)
	{
		LOG_ERROR("can't add HW breakpoint to terminate algorithm");
		return ERROR_TARGET_FAILURE;
	}

	if ((retval = target_resume(target, 0, entry_point, 1, 1)) != ERROR_OK)
	{
		return retval;
	}
	int retvaltemp;
	retval = run_it(target, exit_point, timeout_ms, arch_info);

	if (exit_point)
		breakpoint_remove(target, exit_point);

	if (retval != ERROR_OK)
		return retval;

	for (i = 0; i < num_mem_params; i++)
	{
		if (mem_params[i].direction != PARAM_OUT)
			if ((retvaltemp = target_read_buffer(target, mem_params[i].address, mem_params[i].size, mem_params[i].value)) != ERROR_OK)
			{
					retval = retvaltemp;
			}
	}

	for (i = 0; i < num_reg_params; i++)
	{
		if (reg_params[i].direction != PARAM_OUT)
		{

			struct reg *reg = register_get_by_name(armv4_5->core_cache, reg_params[i].reg_name, 0);
			if (!reg)
			{
				LOG_ERROR("BUG: register '%s' not found", reg_params[i].reg_name);
				retval = ERROR_INVALID_ARGUMENTS;
				continue;
			}

			if (reg->size != reg_params[i].size)
			{
				LOG_ERROR("BUG: register '%s' size doesn't match reg_params[i].size", reg_params[i].reg_name);
				retval = ERROR_INVALID_ARGUMENTS;
				continue;
			}

			buf_set_u32(reg_params[i].value, 0, 32, buf_get_u32(reg->value, 0, 32));
		}
	}

	for (i = 0; i <= 16; i++)
	{
		uint32_t regvalue;
		regvalue = buf_get_u32(ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_algorithm_info->core_mode, i).value, 0, 32);
		if (regvalue != context[i])
		{
			LOG_DEBUG("restoring register %s with value 0x%8.8" PRIx32 "", ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_algorithm_info->core_mode, i).name, context[i]);
			buf_set_u32(ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_algorithm_info->core_mode, i).value, 0, 32, context[i]);
			ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_algorithm_info->core_mode, i).valid = 1;
			ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, armv4_5_algorithm_info->core_mode, i).dirty = 1;
		}
	}
	buf_set_u32(armv4_5->cpsr->value, 0, 32, cpsr);
	armv4_5->cpsr->valid = 1;
	armv4_5->cpsr->dirty = 1;

	armv4_5->core_state = core_state;
	armv4_5->core_mode = core_mode;

	return retval;
}

int armv4_5_run_algorithm(struct target *target, int num_mem_params, struct mem_param *mem_params, int num_reg_params, struct reg_param *reg_params, uint32_t entry_point, uint32_t exit_point, int timeout_ms, void *arch_info)
{
	return armv4_5_run_algorithm_inner(target, num_mem_params, mem_params, num_reg_params, reg_params, entry_point, exit_point, timeout_ms, arch_info, armv4_5_run_algorithm_completion);
}

/**
 * Runs ARM code in the target to calculate a CRC32 checksum.
 *
 * \todo On ARMv5+, rely on BKPT termination for reduced overhead.
 */
int arm_checksum_memory(struct target *target,
		uint32_t address, uint32_t count, uint32_t *checksum)
{
	struct working_area *crc_algorithm;
	struct armv4_5_algorithm armv4_5_info;
	struct reg_param reg_params[2];
	int retval;
	uint32_t i;

	static const uint32_t arm_crc_code[] = {
		0xE1A02000,		/* mov		r2, r0 */
		0xE3E00000,		/* mov		r0, #0xffffffff */
		0xE1A03001,		/* mov		r3, r1 */
		0xE3A04000,		/* mov		r4, #0 */
		0xEA00000B,		/* b		ncomp */
		/* nbyte: */
		0xE7D21004,		/* ldrb	r1, [r2, r4] */
		0xE59F7030,		/* ldr		r7, CRC32XOR */
		0xE0200C01,		/* eor		r0, r0, r1, asl 24 */
		0xE3A05000,		/* mov		r5, #0 */
		/* loop: */
		0xE3500000,		/* cmp		r0, #0 */
		0xE1A06080,		/* mov		r6, r0, asl #1 */
		0xE2855001,		/* add		r5, r5, #1 */
		0xE1A00006,		/* mov		r0, r6 */
		0xB0260007,		/* eorlt	r0, r6, r7 */
		0xE3550008,		/* cmp		r5, #8 */
		0x1AFFFFF8,		/* bne		loop */
		0xE2844001,		/* add		r4, r4, #1 */
		/* ncomp: */
		0xE1540003,		/* cmp		r4, r3 */
		0x1AFFFFF1,		/* bne		nbyte */
		/* end: */
		0xEAFFFFFE,		/* b		end */
		/* CRC32XOR: */
		0x04C11DB7		/* .word 0x04C11DB7 */
	};

	retval = target_alloc_working_area(target,
			sizeof(arm_crc_code), &crc_algorithm);
	if (retval != ERROR_OK)
		return retval;

	/* convert code into a buffer in target endianness */
	for (i = 0; i < ARRAY_SIZE(arm_crc_code); i++) {
		retval = target_write_u32(target,
				crc_algorithm->address + i * sizeof(uint32_t),
				arm_crc_code[i]);
		if (retval != ERROR_OK)
			return retval;
	}

	armv4_5_info.common_magic = ARMV4_5_COMMON_MAGIC;
	armv4_5_info.core_mode = ARMV4_5_MODE_SVC;
	armv4_5_info.core_state = ARMV4_5_STATE_ARM;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT);
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);

	buf_set_u32(reg_params[0].value, 0, 32, address);
	buf_set_u32(reg_params[1].value, 0, 32, count);

	/* 20 second timeout/megabyte */
	int timeout = 20000 * (1 + (count / (1024 * 1024)));

	retval = target_run_algorithm(target, 0, NULL, 2, reg_params,
			crc_algorithm->address,
			crc_algorithm->address + sizeof(arm_crc_code) - 8,
			timeout, &armv4_5_info);
	if (retval != ERROR_OK) {
		LOG_ERROR("error executing ARM crc algorithm");
		destroy_reg_param(&reg_params[0]);
		destroy_reg_param(&reg_params[1]);
		target_free_working_area(target, crc_algorithm);
		return retval;
	}

	*checksum = buf_get_u32(reg_params[0].value, 0, 32);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);

	target_free_working_area(target, crc_algorithm);

	return ERROR_OK;
}

/**
 * Runs ARM code in the target to check whether a memory block holds
 * all ones.  NOR flash which has been erased, and thus may be written,
 * holds all ones.
 *
 * \todo On ARMv5+, rely on BKPT termination for reduced overhead.
 */
int arm_blank_check_memory(struct target *target,
		uint32_t address, uint32_t count, uint32_t *blank)
{
	struct working_area *check_algorithm;
	struct reg_param reg_params[3];
	struct armv4_5_algorithm armv4_5_info;
	int retval;
	uint32_t i;

	static const uint32_t check_code[] = {
		/* loop: */
		0xe4d03001,		/* ldrb r3, [r0], #1 */
		0xe0022003,		/* and r2, r2, r3    */
		0xe2511001,		/* subs r1, r1, #1   */
		0x1afffffb,		/* bne loop          */
		/* end: */
		0xeafffffe		/* b end             */
	};

	/* make sure we have a working area */
	retval = target_alloc_working_area(target,
			sizeof(check_code), &check_algorithm);
	if (retval != ERROR_OK)
		return retval;

	/* convert code into a buffer in target endianness */
	for (i = 0; i < ARRAY_SIZE(check_code); i++) {
		retval = target_write_u32(target,
				check_algorithm->address
						+ i * sizeof(uint32_t),
				check_code[i]);
		if (retval != ERROR_OK)
			return retval;
	}

	armv4_5_info.common_magic = ARMV4_5_COMMON_MAGIC;
	armv4_5_info.core_mode = ARMV4_5_MODE_SVC;
	armv4_5_info.core_state = ARMV4_5_STATE_ARM;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_OUT);
	buf_set_u32(reg_params[0].value, 0, 32, address);

	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);
	buf_set_u32(reg_params[1].value, 0, 32, count);

	init_reg_param(&reg_params[2], "r2", 32, PARAM_IN_OUT);
	buf_set_u32(reg_params[2].value, 0, 32, 0xff);

	retval = target_run_algorithm(target, 0, NULL, 3, reg_params,
			check_algorithm->address,
			check_algorithm->address + sizeof(check_code) - 4,
			10000, &armv4_5_info);
	if (retval != ERROR_OK) {
		destroy_reg_param(&reg_params[0]);
		destroy_reg_param(&reg_params[1]);
		destroy_reg_param(&reg_params[2]);
		target_free_working_area(target, check_algorithm);
		return retval;
	}

	*blank = buf_get_u32(reg_params[2].value, 0, 32);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);

	target_free_working_area(target, check_algorithm);

	return ERROR_OK;
}

static int arm_full_context(struct target *target)
{
	struct armv4_5_common_s *armv4_5 = target_to_armv4_5(target);
	unsigned num_regs = armv4_5->core_cache->num_regs;
	struct reg *reg = armv4_5->core_cache->reg_list;
	int retval = ERROR_OK;

	for (; num_regs && retval == ERROR_OK; num_regs--, reg++) {
		if (reg->valid)
			continue;
		retval = armv4_5_get_core_reg(reg);
	}
	return retval;
}

int armv4_5_init_arch_info(struct target *target, struct arm *armv4_5)
{
	target->arch_info = armv4_5;

	armv4_5->common_magic = ARMV4_5_COMMON_MAGIC;
	armv4_5->core_state = ARMV4_5_STATE_ARM;
	armv4_5->core_mode = ARMV4_5_MODE_USR;

	/* core_type may be overridden by subtype logic */
	armv4_5->core_type = ARMV4_5_MODE_ANY;

	/* default full_context() has no core-specific optimizations */
	if (!armv4_5->full_context && armv4_5->read_core_reg)
		armv4_5->full_context = arm_full_context;

	return ERROR_OK;
}
