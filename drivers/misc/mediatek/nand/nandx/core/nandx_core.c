/*
 * Copyright (C) 2017 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include "nandx_util.h"
#include "nandx_errno.h"
#include "nandx_chip.h"
#include "nandx_device_info.h"
#include "nandx_core.h"
#include "nandx_ops.h"
#include "nandx_platform.h"

#define TLC_WL_PROGRAM_COUNT	(3)

enum DUMP_OPS_TYPE {
	DUMP_OPS_READ,
	DUMP_OPS_WRITE,
};

static struct nandx_core *ncore;

struct nandx_chip_info *get_chip_info(void)
{
	return ncore->info;
}

struct nandx_core *get_nandx_core(void)
{
	return ncore;
}

struct nandx_ops *alloc_ops_table(int count)
{
	struct nandx_ops *ops_table;

	ops_table = mem_alloc(count, sizeof(struct nandx_ops));
	if (!ops_table)
		return NULL;

	return ops_table;
}

void free_ops_table(struct nandx_ops *ops_table)
{
	mem_free(ops_table);
}

static void dump_ops(struct nandx_ops *ops_table, int count,
		     enum DUMP_OPS_TYPE type)
{
#if 0
	int i;
	struct nandx_ops *ops;
	char name[10];

	if (type == DUMP_OPS_READ)
		strcpy(name, "read");
	else if (type == DUMP_OPS_WRITE)
		strcpy(name, "write");

	ops = ops_table;
	for (i = 0; i < count; i++) {
		pr_debug("%s: ops[%s][%d] - row %u col %u len %u data %p oob %p\n",
			  __func__, name, i, ops->row, ops->col, ops->len,
			  ops->data, ops->oob);
		ops++;
	}
#endif
}

static inline void ops_arrange(struct nandx_ops **src, struct nandx_ops **dst,
			       int count)
{
	struct nandx_ops *tmp = *src;

	while (count--)
		*dst++ = tmp++;
}

static int do_chip_read_page(struct nandx_chip_dev *chip,
			struct nandx_ops **ops_list,
			int count, bool multi, bool cache)
{
	int i, ret;
	struct nandx_ops **ops;

	if (!multi) {
		if (!cache) {
			ops = ops_list;
			if (count == 1)
				return chip->read_page(chip, *ops);

			for (i = 0; i < count; i++) {
				ret = chip->read_page(chip, *ops++);
				if (ret < 0)
					return -ENOMEM;
			}
		} else {
			return chip->cache_read_page(chip, ops_list, count);
		}
	} else {
		if (!cache)
			return chip->multi_read_page(chip, ops_list, count);
		else
			return chip->multi_cache_read_page(chip, ops_list,
							   count);
	}

	return 0;
}

static int do_chip_write_page(struct nandx_chip_dev *chip,
			struct nandx_ops **ops_list,
			int count, bool multi, bool cache)
{
	if (!multi) {
		if (!cache)
			return chip->program_page(chip, ops_list, count);
		else
			return chip->cache_program_page(chip, ops_list, count);
	} else {
		if (!cache)
			return chip->multi_program_page(chip, ops_list, count);
		else
			return chip->multi_cache_program_page(chip, ops_list,
							      count);
	}
}

static int do_ops_read(struct nandx_chip_dev *chip,
			struct nandx_ops **ops_list,
			int count, bool multi, bool cache)
{
	int ret;
	u32 page_size, sec_size;
	struct nandx_chip_info *info;
	struct nandx_ops oops_h, oops_t, *pops_h, *pops_t;

	info = &chip->info;
	page_size = info->page_size;
	sec_size = info->sector_size;

	pops_h = *ops_list;
	pops_t = NULL;
	if (count > 1)
		pops_t = ops_list[count - 1];

	if ((pops_h->col > 0 && pops_h->col + pops_h->len <= page_size) ||
	    (pops_h->col == 0 && pops_h->len < page_size)) {
		oops_h.row = pops_h->row;
		oops_h.col = div_round_down(pops_h->col, sec_size);
		oops_h.len = div_round_up(pops_h->col + pops_h->len, sec_size) -
			     oops_h.col;
		oops_h.data = mem_alloc(1, oops_h.len);
		if (!oops_h.data)
			return -ENOMEM;
		oops_h.oob = pops_h->oob;
		ops_list[0] = &oops_h;
	} else {
		pops_h = NULL;
	}

	if (pops_t && pops_t->len < page_size) {
		oops_t.row = pops_t->row;
		oops_t.col = 0;
		oops_t.len = div_round_up(pops_t->len, sec_size);
		oops_t.data = mem_alloc(1, oops_t.len);
		if (!oops_t.data) {
			ret = -ENOMEM;
			goto err_h;
		}
		oops_t.oob = pops_t->oob;
		ops_list[count - 1] = &oops_t;
	} else {
		pops_t = NULL;
	}

	ret = do_chip_read_page(chip, ops_list, count, multi, cache);
	if (ret < 0 && ret != -ENANDFLIPS)
		goto err_t;

	if (pops_h) {
		memcpy(pops_h->data, oops_h.data + pops_h->col - oops_h.col,
		       pops_h->len);
		pops_h->status = oops_h.status;
	}
	if (pops_t) {
		memcpy(pops_t->data, oops_t.data, pops_t->len);
		pops_t->status = oops_t.status;
	}

err_t:
	if (pops_t)
		mem_free(oops_t.data);
err_h:
	if (pops_h)
		mem_free(oops_h.data);

	return ret;
}

int nandx_core_read(struct nandx_ops *ops_table,
			int count, u32 mode)
{
	int ret, ret_tmp, plane_num;
	bool do_cache, do_multi;
	struct nandx_chip_dev *chip;
	struct nandx_ops **ops_list;

	chip = ncore->chip;
	plane_num = ncore->info->plane_num;

	dump_ops(ops_table, count, DUMP_OPS_READ);

	do_multi = plane_num > 1;
	if (do_multi)
		do_multi = (mode & MODE_MULTI) == MODE_MULTI;

	do_cache = count > (do_multi ? plane_num : 1);

	ops_list = mem_alloc(count, sizeof(struct nandx_ops *));
	if (!ops_list) {
		NANDX_ASSERT(ops_list);
		return -ENOMEM;
	}

	if (mode & MODE_SLC) {
		ret = chip->change_mode(chip, OPS_MODE_SLC, true, NULL);
		if (ret < 0)
			goto err;
	}

	ops_arrange(&ops_table, ops_list, count);
	ret = do_ops_read(chip, ops_list, count, do_multi, do_cache);
	if (mode & MODE_SLC) {
		ret_tmp = chip->change_mode(chip, OPS_MODE_SLC, false, NULL);
		if (!ret)
			ret = ret_tmp;
	}

err:
	mem_free(ops_list);
	return ret;
}

static void ops_order_arrange(struct nandx_ops *from, struct nandx_ops **to,
			int cycle_count, int program_count,
			int page_count, bool revesal)
{
	int i, max_count;
	int dst_index;
	struct nandx_ops *src, **dst;

	src = from;
	dst = to;
	memset(*dst, 0, cycle_count * page_count);
	i = revesal ? cycle_count - program_count : 0;
	max_count = revesal ? cycle_count : program_count;
	while (i < max_count) {
		dst_index = revesal ? max_count - i : max_count - i - 1;
		src = from + page_count * i;
		dst = to + page_count * dst_index;
		ops_arrange(&src, dst, page_count);
		i++;
	}
}

static int tlc_order_program(struct nandx_chip_dev *chip,
			struct wl_order_program *program,
			struct nandx_ops *ops_table,
			int count, bool multi, bool cache)
{
	u32 plane_num, max_keep_page_num;
	u32 wl_page_num, multi_wl_page_num;
	u32 i, page_num, block_page_num, wl_count;
	struct nandx_ops *ops, *src, **dst;

	ops = ops_table;
	src = ops_table;

	plane_num = chip->info.plane_num;
	wl_page_num = chip->info.wl_page_num;

	multi_wl_page_num = plane_num * wl_page_num;
	max_keep_page_num = multi_wl_page_num * wl_page_num;

	block_page_num = chip->info.block_size / chip->info.page_size;
	page_num = ops->row % block_page_num;
	if (page_num != program->wl_num * multi_wl_page_num) {
		pr_err("%s: input %u, record %u\n",
			    __func__, page_num,
			    program->wl_num * multi_wl_page_num);
		return -EIO;
	}

	wl_count = count / multi_wl_page_num;
	dst = program->ops_list;
	while (wl_count--) {
		if (!program->wl_num) {
			for (i = 1; i <= wl_page_num; i++)
				ops_order_arrange(src, dst, wl_page_num, i,
						  multi_wl_page_num, false);
			program->wl_num += wl_page_num;
			src += max_keep_page_num;
			dst += wl_page_num * max_keep_page_num;
		} else if (program->wl_num == program->total_wl_num) {
			for (i = wl_page_num - 1; i > 0; i--)
				ops_order_arrange(src, dst, wl_page_num, i,
						  multi_wl_page_num, true);
			program->wl_num = 0;
		} else {
			ops_order_arrange(src, dst, wl_page_num, wl_page_num,
					  multi_wl_page_num, false);
			program->wl_num++;
			src += multi_wl_page_num;
			dst += max_keep_page_num;
		}
	}

	return do_chip_write_page(chip, program->ops_list, count * wl_page_num,
				  multi, cache);
}

static int
vtlc_toshiba_order_program(struct nandx_chip_dev *chip,
			struct wl_order_program *program,
			struct nandx_ops *ops_table, int count,
			bool multi, bool cache)
{
	return 0;
}

static int
vtlc_micron_order_program(struct nandx_chip_dev *chip,
			struct wl_order_program *program,
			struct nandx_ops *ops_table,
			int count, bool multi, bool cache)
{
	return 0;
}

static int do_order_program(struct nandx_chip_dev *chip,
			struct wl_order_program *program,
			struct nandx_ops *ops_table,
			int count, bool multi, bool cache)
{
	struct nandx_ops *ops;
	struct nandx_chip_info *info;
	int multi_wl_page_num, max_keep_page_num;
	u32 page_num, block_page_num;

	info = &chip->info;
	ops = ops_table;
	multi_wl_page_num = info->plane_num * info->wl_page_num;
	max_keep_page_num = multi_wl_page_num * TLC_WL_PROGRAM_COUNT;

	if (count % max_keep_page_num) {
		pr_err("%s: invalid ops count = %d\n",
				__func__, count);
		return -EINVAL;
	}

	if (ops->row % info->wl_page_num) {
		pr_err("%s: invalid ops row = %u\n",
				__func__, ops->row);
		return -EINVAL;
	}

	block_page_num = info->block_size / info->page_size;
	page_num = ops->row % block_page_num;
	if (!page_num) {
		program->wl_num = 0;
	} else if (page_num != info->wl_page_num * program->wl_num) {
		pr_err("%s: invalid input page_num = %u\n",
				__func__, page_num);
		return -EINVAL;
	}

	program->ops_list = mem_alloc(count * TLC_WL_PROGRAM_COUNT,
				      sizeof(struct nandx_ops *));
	if (!program->ops_list) {
		NANDX_ASSERT(program->ops_list);
		return -ENOMEM;
	}

	return program->order_program_func(chip, program, ops_table, count,
					   multi, cache);
}

static int non_order_program(struct nandx_chip_dev *chip,
			struct nandx_ops *ops_table,
			int count, int slc_mode,
			bool multi, bool cache)
{
	int ret = 0, ret_tmp, multi_wl_page_num, max_keep_page_num;
	struct nandx_chip_info *info;
	struct nandx_ops *ops, **ops_list;

	info = &chip->info;
	ops = ops_table;
	multi_wl_page_num = info->plane_num * info->wl_page_num;
	max_keep_page_num = multi_wl_page_num;

	if (slc_mode) {
		ret = chip->change_mode(chip, OPS_MODE_SLC, true, NULL);
		if (ret < 0)
			return ret;
		goto slcmode;
	}

	if (count % max_keep_page_num) {
		pr_err("%s: invalid ops count = %d\n",
				__func__, count);
		return -EINVAL;
	}

	if (ops->row % info->wl_page_num) {
		pr_err("%s: invalid ops row = %u\n",
				__func__, ops->row);
		return -EINVAL;
	}

slcmode:
	ops_list = mem_alloc(count, sizeof(struct nandx_ops *));
	if (!ops_list)
		goto err;

	ops_arrange(&ops_table, ops_list, count);
	ret = do_chip_write_page(chip, ops_list, count, multi, cache);
	mem_free(ops_list);

err:
	if (slc_mode) {
		ret_tmp = chip->change_mode(chip, OPS_MODE_SLC, false, NULL);
		if (!ret)
			ret = ret_tmp;
	}

	return ret;
}

int nandx_core_write(struct nandx_ops *ops_table,
			int count, u32 mode)
{
	int ret;
	bool slc_mode, do_cache, do_multi;
	int plane_num;
	struct nandx_chip_dev *chip;
	struct nandx_chip_info *info;

	chip = ncore->chip;
	info = &chip->info;
	plane_num = info->plane_num;

	dump_ops(ops_table, count, DUMP_OPS_WRITE);

	do_multi = plane_num > 1;
	if (do_multi)
		do_multi = (mode & MODE_MULTI) == MODE_MULTI;

	do_cache = count > (do_multi ? plane_num : 1);

	slc_mode = (mode & MODE_SLC) == MODE_SLC;
	if (chip->program_order_type == PROGRAM_ORDER_NONE) {
		ret = non_order_program(chip, ops_table, count, slc_mode,
					do_multi, do_cache);
		return ret;
	}

	ret = do_order_program(chip, &ncore->program, ops_table, count,
			       do_multi, do_cache);
	return ret;
}

static void dump_erase_rows(u32 *rows, int count)
{
#if 0
	int i;

	for (i = 0; i < count; i++)
		pr_debug("%s: row[%d] %u\n",
			    __func__, i, rows[i]);
#endif
}

int nandx_core_erase(u32 *rows, int count, u32 mode)
{
	u32 *row;
	bool do_multi = false;
	int plane_num, erase_count, ret;
	struct nandx_chip_dev *chip;
	struct nandx_chip_info *info;

	chip = ncore->chip;
	info = &chip->info;
	plane_num = info->plane_num;

	dump_erase_rows(rows, count);

	do_multi = plane_num > 1;
	if (do_multi)
		do_multi = (mode & MODE_MULTI) == MODE_MULTI;

	row = rows;
	erase_count = count;
	if (do_multi)
		erase_count = count / plane_num;

	if (mode & MODE_SLC) {
		ret = chip->change_mode(chip, OPS_MODE_SLC, true, NULL);
		if (ret < 0)
			goto err;
	}

	while (erase_count--) {
		if (do_multi) {
			ret = chip->multi_erase(chip, row);
			row += plane_num;
		} else {
			ret = chip->erase(chip, *row);
			row++;
		}
		if (ret < 0)
			goto err;
	}

	if (mode & MODE_SLC) {
		ret = chip->change_mode(chip, OPS_MODE_SLC, false, NULL);
		if (ret < 0)
			goto err;
	}

err:
	return 0;
}

bool nandx_core_is_bad(u32 row)
{
	return ncore->chip->block_is_bad(ncore->chip, row);
}

int nandx_core_mark_bad(u32 row)
{
	/* TODO: 1 page for slc; 2 page for mlc; 1 block for tlc; */
	return 0;
}

bool nandx_get_mode(enum OPS_MODE_TYPE mode)
{
	struct nandx_chip_dev *chip;

	chip = ncore->chip;
	return chip->get_mode(chip, mode);
}

static order_program_cb order_program_table[] = {
	NULL,
	tlc_order_program,
	vtlc_toshiba_order_program,
	vtlc_micron_order_program
};

static struct nandx_core *
nandx_core_alloc(struct nandx_chip_dev *chip)
{
	struct nandx_chip_info *info;
	struct wl_order_program *program;
	enum PROGRAM_ORDER type;

	if (!chip) {
		NANDX_ASSERT(chip);
		return NULL;
	}

	ncore = mem_alloc(1, sizeof(struct nandx_core));
	if (!ncore) {
		NANDX_ASSERT(ncore);
		return NULL;
	}

	ncore->chip = chip;
	info = &chip->info;
	ncore->info = info;
	program = &ncore->program;
	type = chip->program_order_type;
	program->total_wl_num = (info->block_size / info->page_size) *
				info->wl_page_num;
	program->order_program_func = order_program_table[type];

	return ncore;
}

void nandx_core_free(void)
{
	if (!ncore)
		return;

	if (ncore->program.ops_list)
		mem_free(ncore->program.ops_list);

	mem_free(ncore);
}

int nandx_core_suspend(void)
{
	if (ncore->chip->suspend)
		return ncore->chip->suspend(ncore->chip);

	return 0;
}

int nandx_core_resume(void)
{
	struct nandx_chip_dev *chip = ncore->chip;
	struct platform_data *pdata = ncore->pdata;
	struct nfc_frequency *freq = &pdata->freq;
	bool is_ddr = nandx_get_mode(OPS_MODE_DDR);
	int ret = 0;

	if (chip->resume) {
		ret = chip->resume(chip);

		if (!ret)
			ret = chip->change_mode(chip, OPS_MODE_DDR, is_ddr,
						freq);
	}

	return ret;
}

struct nandx_core *
nandx_core_init(struct platform_data *pdata, u32 mode)
{
	int ret;
	struct nandx_chip_dev *chip;
	bool high_speed_en, ecc_clk_en;

	nandx_platform_prepare_clock(pdata, false, false);
	nandx_platform_enable_clock(pdata, false, false);

	chip = nandx_chip_alloc(pdata->res);
	if (!chip) {
		NANDX_ASSERT(chip);
		goto disable_clk;
	}

	chip->change_mode(chip, OPS_MODE_DMA,
			(mode & MODE_DMA) == MODE_DMA, NULL);
	chip->change_mode(chip, OPS_MODE_IRQ,
			(mode & MODE_IRQ) == MODE_IRQ, NULL);
	chip->change_mode(chip, OPS_MODE_ECC,
			(mode & MODE_ECC) == MODE_ECC, NULL);
	chip->change_mode(chip, OPS_MODE_RANDOMIZE,
			(mode & MODE_RANDOMIZE) == MODE_RANDOMIZE, NULL);
	chip->change_mode(chip, OPS_MODE_CALIBRATION,
			(mode & MODE_CALIBRATION) == MODE_CALIBRATION, NULL);

	/* ret EOPNOTSUPP, EIO, OK */
	ret = -EOPNOTSUPP;
	if ((mode & MODE_DDR) == MODE_DDR)
		ret = chip->change_mode(chip, OPS_MODE_DDR, true, &pdata->freq);
	if (ret == -EOPNOTSUPP)
		ret = chip->change_mode(chip, OPS_MODE_DDR, false,
					&pdata->freq);
	if (ret < 0)
		goto freechip;

	ncore = nandx_core_alloc(chip);
	if (!ncore)
		goto freechip;

	ncore->pdata = pdata;

	nandx_platform_disable_clock(pdata, false, false);
	nandx_platform_unprepare_clock(pdata, false, false);

	high_speed_en = pdata->freq.sel_2x_idx >= 0;
	ecc_clk_en = pdata->freq.sel_ecc_idx >= 0;
	nandx_platform_prepare_clock(ncore->pdata, high_speed_en, ecc_clk_en);

	return ncore;

freechip:
	nandx_chip_free(chip);
disable_clk:
	nandx_platform_disable_clock(pdata, false, false);
	nandx_platform_unprepare_clock(pdata, false, false);

	return NULL;
}

int nandx_core_exit(void)
{
	nandx_chip_free(ncore->chip);

	mem_free(ncore->pdata);
	nandx_core_free();

	return 0;
}
