/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include "config/aom_config.h"
#include "config/av1_rtcd.h"
#include "config/aom_dsp_rtcd.h"

#include "aom_dsp/bitwriter.h"
#include "aom_dsp/quantize.h"
#include "aom_mem/aom_mem.h"
#include "aom_ports/mem.h"

#if CONFIG_BITSTREAM_DEBUG || CONFIG_MISMATCH_DEBUG
#include "aom_util/debug_util.h"
#endif  // CONFIG_BITSTREAM_DEBUG || CONFIG_MISMATCH_DEBUG

#include "av1/common/cfl.h"
#include "av1/common/idct.h"
#include "av1/common/reconinter.h"
#include "av1/common/reconintra.h"
#include "av1/common/scan.h"

#include "av1/encoder/av1_quantize.h"
#include "av1/encoder/encodemb.h"
#include "av1/encoder/encodetxb.h"
#include "av1/encoder/hybrid_fwd_txfm.h"
#include "av1/encoder/rd.h"
#include "av1/encoder/rdopt.h"

#if CONFIG_INTERINTRA_ML_DATA_COLLECT
#include "av1/encoder/interintra_ml_data_collect.h"
#endif

// Check if one needs to use c version subtraction.
static int check_subtract_block_size(int w, int h) { return w < 4 || h < 4; }

static void subtract_block(const MACROBLOCKD *xd, int rows, int cols,
                           int16_t *diff, ptrdiff_t diff_stride,
                           const uint8_t *src8, ptrdiff_t src_stride,
                           const uint8_t *pred8, ptrdiff_t pred_stride) {
  if (check_subtract_block_size(rows, cols)) {
    if (is_cur_buf_hbd(xd)) {
      aom_highbd_subtract_block_c(rows, cols, diff, diff_stride, src8,
                                  src_stride, pred8, pred_stride, xd->bd);
      return;
    }
    aom_subtract_block_c(rows, cols, diff, diff_stride, src8, src_stride, pred8,
                         pred_stride);

    return;
  }

  if (is_cur_buf_hbd(xd)) {
    aom_highbd_subtract_block(rows, cols, diff, diff_stride, src8, src_stride,
                              pred8, pred_stride, xd->bd);
    return;
  }
  aom_subtract_block(rows, cols, diff, diff_stride, src8, src_stride, pred8,
                     pred_stride);
}

void av1_subtract_txb(MACROBLOCK *x, int plane, BLOCK_SIZE plane_bsize,
                      int blk_col, int blk_row, TX_SIZE tx_size) {
  MACROBLOCKD *const xd = &x->e_mbd;
  struct macroblock_plane *const p = &x->plane[plane];
  const struct macroblockd_plane *const pd = &x->e_mbd.plane[plane];
  const int diff_stride = block_size_wide[plane_bsize];
  const int src_stride = p->src.stride;
  const int dst_stride = pd->dst.stride;
  const int tx1d_width = tx_size_wide[tx_size];
  const int tx1d_height = tx_size_high[tx_size];
  uint8_t *dst =
      &pd->dst.buf[(blk_row * dst_stride + blk_col) << tx_size_wide_log2[0]];
  uint8_t *src =
      &p->src.buf[(blk_row * src_stride + blk_col) << tx_size_wide_log2[0]];
  int16_t *src_diff =
      &p->src_diff[(blk_row * diff_stride + blk_col) << tx_size_wide_log2[0]];
  subtract_block(xd, tx1d_height, tx1d_width, src_diff, diff_stride, src,
                 src_stride, dst, dst_stride);
}

void av1_subtract_plane(MACROBLOCK *x, BLOCK_SIZE bsize, int plane) {
  struct macroblock_plane *const p = &x->plane[plane];
  const struct macroblockd_plane *const pd = &x->e_mbd.plane[plane];
  assert(bsize < BLOCK_SIZES_ALL);
  const MACROBLOCKD *xd = &x->e_mbd;
  const BLOCK_SIZE bsize_base =
      plane ? xd->mi[0]->chroma_ref_info.bsize_base : bsize;
  const BLOCK_SIZE plane_bsize =
      get_plane_block_size(bsize_base, pd->subsampling_x, pd->subsampling_y);
  assert(plane_bsize < BLOCK_SIZES_ALL);
  const int bw = block_size_wide[plane_bsize];
  const int bh = block_size_high[plane_bsize];

  subtract_block(xd, bh, bw, p->src_diff, bw, p->src.buf, p->src.stride,
                 pd->dst.buf, pd->dst.stride);
}

int av1_optimize_b(const struct AV1_COMP *cpi, MACROBLOCK *x, int plane,
                   int block, TX_SIZE tx_size, TX_TYPE tx_type,
                   const TXB_CTX *const txb_ctx, int fast_mode,
                   int *rate_cost) {
  MACROBLOCKD *const xd = &x->e_mbd;
  struct macroblock_plane *const p = &x->plane[plane];
  const int eob = p->eobs[block];
  const int segment_id = xd->mi[0]->segment_id;

  if (eob == 0 || !cpi->optimize_seg_arr[segment_id] ||
      xd->lossless[segment_id]) {
    *rate_cost = av1_cost_skip_txb(x, txb_ctx, plane, tx_size);
    return eob;
  }

  return av1_optimize_txb_new(cpi, x, plane, block, tx_size, tx_type, txb_ctx,
                              rate_cost, cpi->oxcf.sharpness, fast_mode);
}

enum {
  QUANT_FUNC_LOWBD = 0,
  QUANT_FUNC_HIGHBD = 1,
  QUANT_FUNC_TYPES = 2
} UENUM1BYTE(QUANT_FUNC);

static AV1_QUANT_FACADE
    quant_func_list[AV1_XFORM_QUANT_TYPES][QUANT_FUNC_TYPES] = {
      { av1_quantize_fp_facade, av1_highbd_quantize_fp_facade },
      { av1_quantize_b_facade, av1_highbd_quantize_b_facade },
      { av1_quantize_dc_facade, av1_highbd_quantize_dc_facade },
      { NULL, NULL }
    };

#if CONFIG_DSPL_RESIDUAL
/*!
 * This functions transforms residuals after downsampling them. There are three
 * key steps: downsampling, transform and packing.
 *
 * Packing is done by scanning in (using scan_array()) transformed coefficients
 * into a buffer in the scan order of the smaller transform and then scanning
 * them back out (using iscan_array()) in the order of the original transform.
 * This ensures that the quantizer always sees the smaller transform
 * coefficients first in the coeff buffer followed by consecutive zeros which
 * are coded at almost no cost (using eob).
 */
void av1_dspl_xform(const int16_t *src_diff, const int diff_stride,
                    tran_low_t *const coeff, const TxfmParam *txfm_param,
                    const TX_SIZE tx_size, const TX_TYPE tx_type) {
  const uint8_t txw = tx_size_wide[tx_size], txh = tx_size_high[tx_size];
  const TX_SIZE new_tx_size = dspl_tx_size_map[tx_size];
  const uint8_t dspl_txw = txw >> 1;
  TxfmParam dspl_txfm_param = *txfm_param;
  dspl_txfm_param.tx_size = new_tx_size;

  // Buffers
  DECLARE_ALIGNED(32, int16_t, dspl_src_diff[MAX_TX_SQUARE]);
  DECLARE_ALIGNED(32, tran_low_t, scan_buf[MAX_TX_SQUARE]);

  // Downsample
  memset(dspl_src_diff, 0, MAX_TX_SQUARE * sizeof(int16_t));
  av1_signed_down2(src_diff, txh, txw, diff_stride, dspl_src_diff, dspl_txw, 1,
                   1, dspl_txfm_param.bd);

  // Transform
  memset(coeff, 0, txw * txh * sizeof(tran_low_t));
  av1_fwd_txfm(dspl_src_diff, coeff, dspl_txw, &dspl_txfm_param);

  // Pack coeffcients
  const int size = av1_get_max_eob(tx_size),
            dspl_size = av1_get_max_eob(new_tx_size);
  const SCAN_ORDER *const scan_order = get_scan(tx_size, tx_type);
  const SCAN_ORDER *const dspl_scan_order = get_scan(new_tx_size, tx_type);
  memset(scan_buf, 0, size * sizeof(tran_low_t));
  scan_array(coeff, scan_buf, dspl_size, dspl_scan_order);
  memset(coeff, 0, txw * txh * sizeof(tran_low_t));
  iscan_array(scan_buf, coeff, size, scan_order);
}
#endif  // CONFIG_DSPL_RESIDUAL

void av1_xform_quant(const AV1_COMMON *cm, MACROBLOCK *x, int plane, int block,
                     int blk_row, int blk_col, BLOCK_SIZE plane_bsize,
                     TX_SIZE tx_size, TX_TYPE tx_type,
                     AV1_XFORM_QUANT xform_quant_idx) {
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mbmi = xd->mi[0];
  const struct macroblock_plane *const p = &x->plane[plane];
  const struct macroblockd_plane *const pd = &xd->plane[plane];
  const SCAN_ORDER *const scan_order = get_scan(tx_size, tx_type);

  tran_low_t *const coeff = BLOCK_OFFSET(p->coeff, block);
  tran_low_t *const qcoeff = BLOCK_OFFSET(p->qcoeff, block);
  tran_low_t *const dqcoeff = BLOCK_OFFSET(pd->dqcoeff, block);
  uint16_t *const eob = &p->eobs[block];
  const int diff_stride = block_size_wide[plane_bsize];
  int seg_id = mbmi->segment_id;
  const TX_SIZE qm_tx_size = av1_get_adjusted_tx_size(tx_size);
  // Use a flat matrix (i.e. no weighting) for 1D and Identity transforms
  const qm_val_t *qmatrix =
      IS_2D_TRANSFORM(tx_type) ? pd->seg_qmatrix[seg_id][qm_tx_size]
                               : cm->gqmatrix[NUM_QM_LEVELS - 1][0][qm_tx_size];
  const qm_val_t *iqmatrix =
      IS_2D_TRANSFORM(tx_type)
          ? pd->seg_iqmatrix[seg_id][qm_tx_size]
          : cm->giqmatrix[NUM_QM_LEVELS - 1][0][qm_tx_size];

  const int src_offset = (blk_row * diff_stride + blk_col);
  const int16_t *src_diff = &p->src_diff[src_offset << tx_size_wide_log2[0]];
  QUANT_PARAM qparam;
  qparam.log_scale = av1_get_tx_scale(tx_size);
  qparam.tx_size = tx_size;
  qparam.qmatrix = qmatrix;
  qparam.iqmatrix = iqmatrix;
  qparam.use_quant_b_adapt = cm->use_quant_b_adapt;
  TxfmParam txfm_param;
  txfm_param.tx_type = tx_type;
  txfm_param.tx_size = tx_size;
  txfm_param.lossless = xd->lossless[mbmi->segment_id];
  txfm_param.tx_set_type = av1_get_ext_tx_set_type(
      txfm_param.tx_size, is_inter_block(mbmi), cm->reduced_tx_set_used);

  txfm_param.bd = xd->bd;
  txfm_param.is_hbd = is_cur_buf_hbd(xd);
  txfm_param.mode = get_mode_dep_txfm_mode(mbmi);

#if CONFIG_DSPL_RESIDUAL
  DSPL_TYPE dspl_type = xd->mi[0]->dspl_type;
  if (plane > 0 || dspl_type != DSPL_XY || xd->bd > 8) {
    av1_fwd_txfm(src_diff, coeff, diff_stride, &txfm_param);
  } else {
    av1_dspl_xform(src_diff, diff_stride, coeff, &txfm_param, tx_size, tx_type);
  }
#else
  av1_fwd_txfm(src_diff, coeff, diff_stride, &txfm_param);
#endif  // CONFIG_DSPL_RESIDUAL

  if (xform_quant_idx != AV1_XFORM_QUANT_SKIP_QUANT) {
    const int n_coeffs = av1_get_max_eob(tx_size);
    if (LIKELY(!x->skip_block)) {
      quant_func_list[xform_quant_idx][txfm_param.is_hbd](
          coeff, n_coeffs, p, qcoeff, dqcoeff, eob, scan_order, &qparam);
    } else {
      av1_quantize_skip(n_coeffs, qcoeff, dqcoeff, eob);
    }
  }
  // NOTE: optimize_b_following is true means av1_optimze_b will be called
  // When the condition of doing optimize_b is changed,
  // this flag need update simultaneously
  const int optimize_b_following =
      (xform_quant_idx != AV1_XFORM_QUANT_FP) || (txfm_param.lossless);
  if (optimize_b_following) {
    p->txb_entropy_ctx[block] =
        (uint8_t)av1_get_txb_entropy_context(qcoeff, scan_order, tx_size, *eob);
  } else {
    p->txb_entropy_ctx[block] = 0;
  }
  return;
}

static void encode_block(int plane, int block, int blk_row, int blk_col,
                         BLOCK_SIZE plane_bsize, TX_SIZE tx_size, void *arg,
                         int mi_row, int mi_col, RUN_TYPE dry_run) {
  (void)mi_row;
  (void)mi_col;
  (void)dry_run;
  struct encode_b_args *const args = arg;
  const AV1_COMMON *const cm = &args->cpi->common;
  MACROBLOCK *const x = args->x;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *mbmi = xd->mi[0];
  struct macroblock_plane *const p = &x->plane[plane];
  struct macroblockd_plane *const pd = &xd->plane[plane];
  tran_low_t *const dqcoeff = BLOCK_OFFSET(pd->dqcoeff, block);
  uint8_t *dst;
  ENTROPY_CONTEXT *a, *l;
  int dummy_rate_cost = 0;

  const int bw = block_size_wide[plane_bsize] >> tx_size_wide_log2[0];
  dst = &pd->dst
             .buf[(blk_row * pd->dst.stride + blk_col) << tx_size_wide_log2[0]];

  a = &args->ta[blk_col];
  l = &args->tl[blk_row];

  if (!is_blk_skip(x, plane, blk_row * bw + blk_col) && !mbmi->skip_mode) {
    TX_TYPE tx_type = av1_get_tx_type(pd->plane_type, xd, blk_row, blk_col,
                                      tx_size, cm->reduced_tx_set_used);
    const int use_trellis = is_trellis_used(args->enable_optimize_b, dry_run);
    int quant_idx;
    if (use_trellis)
      quant_idx = AV1_XFORM_QUANT_FP;
    else
      quant_idx =
          USE_B_QUANT_NO_TRELLIS ? AV1_XFORM_QUANT_B : AV1_XFORM_QUANT_FP;
    if (use_trellis) {
      av1_xform_quant(cm, x, plane, block, blk_row, blk_col, plane_bsize,
                      tx_size, tx_type, quant_idx);
      TXB_CTX txb_ctx;
      get_txb_ctx(plane_bsize, tx_size, plane, a, l, &txb_ctx);
      av1_optimize_b(args->cpi, x, plane, block, tx_size, tx_type, &txb_ctx,
                     args->cpi->sf.trellis_eob_fast, &dummy_rate_cost);
    } else {
      av1_xform_quant(cm, x, plane, block, blk_row, blk_col, plane_bsize,
                      tx_size, tx_type, quant_idx);
    }
  } else {
    p->eobs[block] = 0;
    p->txb_entropy_ctx[block] = 0;
  }

  av1_set_txb_context(x, plane, block, tx_size, a, l);

#if CONFIG_SUPERRES_TX64
#define GET_SUPERRES_TX64_TRAINING_DATA 0
#endif  // CONFIG_SUPERRES_TX64

#if CONFIG_INTERINTRA_ML_DATA_COLLECT
  if (p->eobs[block] == 0 && dry_run == OUTPUT_ENABLED) {
    // This turned out to be a skip block. Ignore it.
    av1_interintra_ml_data_collect_abandon();
  }
#endif

  if (p->eobs[block]) {
    *(args->skip) = 0;

    TX_TYPE tx_type = av1_get_tx_type(pd->plane_type, xd, blk_row, blk_col,
                                      tx_size, cm->reduced_tx_set_used);
#if CONFIG_SUPERRES_TX64 && GET_SUPERRES_TX64_TRAINING_DATA
    uint8_t prd[64 * 64];
    if (dry_run == OUTPUT_ENABLED && txsize_sqr_up_map[tx_size] == TX_64X64) {
      const int dst_stride = pd->dst.stride;
      // Prediction
      for (int ii = 0; ii < tx_size_high[tx_size]; ++ii) {
        for (int jj = 0; jj < tx_size_wide[tx_size]; ++jj) {
          const uint8_t prdval = dst[ii * dst_stride + jj];
          prd[ii * tx_size_wide[tx_size] + jj] = prdval;
        }
      }
    }
#endif  // CONFIG_SUPERRES_TX64 && GET_SUPERRES_TX64_TRAINING_DATA
    av1_inverse_transform_block(xd, dqcoeff, plane, tx_type, tx_size, dst,
                                pd->dst.stride, p->eobs[block],
                                cm->reduced_tx_set_used);

#if CONFIG_INTERINTRA_ML_DATA_COLLECT
    if (dry_run == OUTPUT_ENABLED) {
      // This is not a skip block. Record it.
      av1_interintra_ml_data_collect_finalize();
    }
#endif

#if CONFIG_SUPERRES_TX64 && GET_SUPERRES_TX64_TRAINING_DATA
    if (dry_run == OUTPUT_ENABLED && txsize_sqr_up_map[tx_size] == TX_64X64 &&
        p->eobs[block] > 1) {
      printf("Gotcha %d [%d]\n", xd->current_qindex, cm->base_qindex);
      char fname[256];
      sprintf(fname, "stx_%dx%d.dat", tx_size_wide[tx_size],
              tx_size_high[tx_size]);
      FILE *fp = fopen(fname, "ab");
      // Source
      const int src_stride = p->src.stride;
      const int src_offset = (blk_row * src_stride + blk_col);
      const uint8_t *src = &p->src.buf[src_offset << tx_size_wide_log2[0]];
      for (int ii = 0; ii < tx_size_high[tx_size]; ++ii) {
        for (int jj = 0; jj < tx_size_wide[tx_size]; ++jj) {
          const uint8_t srcval = src[ii * src_stride + jj];
          fwrite(&srcval, 1, 1, fp);
        }
      }
      // Prediction
      for (int ii = 0; ii < tx_size_high[tx_size]; ++ii) {
        for (int jj = 0; jj < tx_size_wide[tx_size]; ++jj) {
          const uint8_t prdval = prd[ii * tx_size_wide[tx_size] + jj];
          fwrite(&prdval, 1, 1, fp);
        }
      }
      // Reconstruction
      const int dst_stride = pd->dst.stride;
      for (int ii = 0; ii < tx_size_high[tx_size]; ++ii) {
        for (int jj = 0; jj < tx_size_wide[tx_size]; ++jj) {
          const uint8_t recval = dst[ii * dst_stride + jj];
          fwrite(&recval, 1, 1, fp);
        }
      }
      fclose(fp);
    }
#endif  // CONFIG_SUPERRES_TX64 && GET_SUPERRES_TX64_TRAINING_DATA
  }

  // TODO(debargha, jingning): Temporarily disable txk_type check for eob=0
  // case. It is possible that certain collision in hash index would cause
  // the assertion failure. To further optimize the rate-distortion
  // performance, we need to re-visit this part and enable this assert
  // again.
  if (p->eobs[block] == 0 && plane == 0) {
#if 0
    if (args->cpi->oxcf.aq_mode == NO_AQ &&
        args->cpi->oxcf.deltaq_mode == NO_DELTA_Q) {
      // TODO(jingning,angiebird,huisu@google.com): enable txk_check when
      // enable_optimize_b is true to detect potential RD bug.
      const uint8_t disable_txk_check = args->enable_optimize_b;
      if (!disable_txk_check) {
        assert(mbmi->txk_type[av1_get_txk_type_index(plane_bsize, blk_row,
                                                     blk_col)] == DCT_DCT);
      }
    }
#endif
    update_txk_array(mbmi->txk_type, plane_bsize, blk_row, blk_col, tx_size,
                     DCT_DCT);
  }

  if (p->eobs[block] == 0 && dry_run == OUTPUT_ENABLED) {
    av1_update_txk_skip_array(cm, mi_row, mi_col, plane, blk_row, blk_col,
                              tx_size, cm->fEncTxSkipLog);
  }

#if CONFIG_MISMATCH_DEBUG
  if (dry_run == OUTPUT_ENABLED) {
    int pixel_c, pixel_r;
    BLOCK_SIZE bsize = txsize_to_bsize[tx_size];
    int blk_w = block_size_wide[bsize];
    int blk_h = block_size_high[bsize];
    mi_to_pixel_loc(&pixel_c, &pixel_r, mi_col, mi_row, blk_col, blk_row,
                    pd->subsampling_x, pd->subsampling_y);
    mismatch_record_block_tx(dst, pd->dst.stride, cm->current_frame.order_hint,
                             plane, pixel_c, pixel_r, blk_w, blk_h,
                             xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH);
  }
#endif
}

static void encode_block_inter(int plane, int block, int blk_row, int blk_col,
                               BLOCK_SIZE plane_bsize, TX_SIZE tx_size,
                               void *arg, int mi_row, int mi_col,
                               RUN_TYPE dry_run) {
  (void)mi_row;
  (void)mi_col;
  struct encode_b_args *const args = arg;
  MACROBLOCK *const x = args->x;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mbmi = xd->mi[0];
  const struct macroblockd_plane *const pd = &xd->plane[plane];
  const int max_blocks_high = max_block_high(xd, plane_bsize, plane);
  const int max_blocks_wide = max_block_wide(xd, plane_bsize, plane);

  if (blk_row >= max_blocks_high || blk_col >= max_blocks_wide) return;

  const BLOCK_SIZE uv_bsize_base = mbmi->chroma_ref_info.bsize_base;
  const TX_SIZE plane_tx_size =
      plane ? av1_get_max_uv_txsize(uv_bsize_base, pd->subsampling_x,
                                    pd->subsampling_y)
            : mbmi->inter_tx_size[av1_get_txb_size_index(plane_bsize, blk_row,
                                                         blk_col)];
  if (!plane) {
    assert(tx_size_wide[tx_size] >= tx_size_wide[plane_tx_size] &&
           tx_size_high[tx_size] >= tx_size_high[plane_tx_size]);
  }

  if (tx_size == plane_tx_size || plane) {
    encode_block(plane, block, blk_row, blk_col, plane_bsize, tx_size, arg,
                 mi_row, mi_col, dry_run);
  } else {
#if CONFIG_NEW_TX_PARTITION
    TX_SIZE sub_txs[MAX_TX_PARTITIONS] = { 0 };
    const int index = av1_get_txb_size_index(plane_bsize, blk_row, blk_col);
    get_tx_partition_sizes(mbmi->partition_type[index], tx_size, sub_txs);
    int cur_partition = 0;
    int bsw = 0, bsh = 0;
    for (int r = 0; r < tx_size_high_unit[tx_size]; r += bsh) {
      for (int c = 0; c < tx_size_wide_unit[tx_size]; c += bsw) {
        const TX_SIZE sub_tx = sub_txs[cur_partition];
        bsw = tx_size_wide_unit[sub_tx];
        bsh = tx_size_high_unit[sub_tx];
        const int sub_step = bsw * bsh;
        const int offsetr = blk_row + r;
        const int offsetc = blk_col + c;
        if (offsetr >= max_blocks_high || offsetc >= max_blocks_wide) continue;
        encode_block(plane, block, offsetr, offsetc, plane_bsize, sub_tx, arg,
                     mi_row, mi_col, dry_run);
        block += sub_step;
        cur_partition++;
      }
    }
#else
    assert(tx_size < TX_SIZES_ALL);
    const TX_SIZE sub_txs = sub_tx_size_map[tx_size];
    assert(IMPLIES(tx_size <= TX_4X4, sub_txs == tx_size));
    assert(IMPLIES(tx_size > TX_4X4, sub_txs < tx_size));
    // This is the square transform block partition entry point.
    const int bsw = tx_size_wide_unit[sub_txs];
    const int bsh = tx_size_high_unit[sub_txs];
    const int step = bsh * bsw;
    assert(bsw > 0 && bsh > 0);

    for (int row = 0; row < tx_size_high_unit[tx_size]; row += bsh) {
      for (int col = 0; col < tx_size_wide_unit[tx_size]; col += bsw) {
        const int offsetr = blk_row + row;
        const int offsetc = blk_col + col;

        if (offsetr >= max_blocks_high || offsetc >= max_blocks_wide) continue;

        encode_block_inter(plane, block, offsetr, offsetc, plane_bsize, sub_txs,
                           arg, mi_row, mi_col, dry_run);
        block += step;
      }
    }
#endif  // CONFIG_NEW_TX_PARTITION
  }
}

void av1_foreach_transformed_block_in_plane(
    const MACROBLOCKD *const xd, BLOCK_SIZE bsize, int plane,
    foreach_transformed_block_visitor visit, void *arg) {
  const struct macroblockd_plane *const pd = &xd->plane[plane];
  // block and transform sizes, in number of 4x4 blocks log 2 ("*_b")
  // 4x4=0, 8x8=2, 16x16=4, 32x32=6, 64x64=8
  // transform size varies per plane, look it up in a common way.
  const TX_SIZE tx_size = av1_get_tx_size(plane, xd);
  const BLOCK_SIZE bsize_base =
      plane ? xd->mi[0]->chroma_ref_info.bsize_base : bsize;
  const BLOCK_SIZE plane_bsize =
      get_plane_block_size(bsize_base, pd->subsampling_x, pd->subsampling_y);
  const uint8_t txw_unit = tx_size_wide_unit[tx_size];
  const uint8_t txh_unit = tx_size_high_unit[tx_size];
  const int step = txw_unit * txh_unit;
  int i = 0, r, c;

  // If mb_to_right_edge is < 0 we are in a situation in which
  // the current block size extends into the UMV and we won't
  // visit the sub blocks that are wholly within the UMV.
  const int max_blocks_wide = max_block_wide(xd, plane_bsize, plane);
  const int max_blocks_high = max_block_high(xd, plane_bsize, plane);

  int blk_row, blk_col;

  const BLOCK_SIZE max_unit_bsize =
      get_plane_block_size(BLOCK_64X64, pd->subsampling_x, pd->subsampling_y);
  int mu_blocks_wide = block_size_wide[max_unit_bsize] >> tx_size_wide_log2[0];
  int mu_blocks_high = block_size_high[max_unit_bsize] >> tx_size_high_log2[0];
  mu_blocks_wide = AOMMIN(max_blocks_wide, mu_blocks_wide);
  mu_blocks_high = AOMMIN(max_blocks_high, mu_blocks_high);

  // Keep track of the row and column of the blocks we use so that we know
  // if we are in the unrestricted motion border.
  for (r = 0; r < max_blocks_high; r += mu_blocks_high) {
    const int unit_height = AOMMIN(mu_blocks_high + r, max_blocks_high);
    // Skip visiting the sub blocks that are wholly within the UMV.
    for (c = 0; c < max_blocks_wide; c += mu_blocks_wide) {
      const int unit_width = AOMMIN(mu_blocks_wide + c, max_blocks_wide);
      for (blk_row = r; blk_row < unit_height; blk_row += txh_unit) {
        for (blk_col = c; blk_col < unit_width; blk_col += txw_unit) {
          visit(plane, i, blk_row, blk_col, plane_bsize, tx_size, arg);
          i += step;
        }
      }
    }
  }
}

void av1_foreach_transformed_block(const MACROBLOCKD *const xd,
                                   BLOCK_SIZE bsize,
                                   foreach_transformed_block_visitor visit,
                                   void *arg, const int num_planes) {
  for (int plane = 0; plane < num_planes; ++plane) {
    if (plane && !xd->mi[0]->chroma_ref_info.is_chroma_ref) continue;
    av1_foreach_transformed_block_in_plane(xd, bsize, plane, visit, arg);
  }
}

typedef struct encode_block_pass1_args {
  AV1_COMMON *cm;
  MACROBLOCK *x;
} encode_block_pass1_args;

static void encode_block_pass1(int plane, int block, int blk_row, int blk_col,
                               BLOCK_SIZE plane_bsize, TX_SIZE tx_size,
                               void *arg) {
  encode_block_pass1_args *args = (encode_block_pass1_args *)arg;
  AV1_COMMON *cm = args->cm;
  MACROBLOCK *const x = args->x;
  MACROBLOCKD *const xd = &x->e_mbd;
  struct macroblock_plane *const p = &x->plane[plane];
  struct macroblockd_plane *const pd = &xd->plane[plane];
  tran_low_t *const dqcoeff = BLOCK_OFFSET(pd->dqcoeff, block);
  TxfmParam txfm_param;
  uint8_t *dst;
  dst = &pd->dst
             .buf[(blk_row * pd->dst.stride + blk_col) << tx_size_wide_log2[0]];
  av1_xform_quant(cm, x, plane, block, blk_row, blk_col, plane_bsize, tx_size,
                  DCT_DCT, AV1_XFORM_QUANT_B);

  if (p->eobs[block] > 0) {
    txfm_param.bd = xd->bd;
    txfm_param.is_hbd = is_cur_buf_hbd(xd);
    txfm_param.tx_type = DCT_DCT;
    txfm_param.tx_size = tx_size;
    txfm_param.eob = p->eobs[block];
    txfm_param.lossless = xd->lossless[xd->mi[0]->segment_id];
    txfm_param.tx_set_type = av1_get_ext_tx_set_type(
        txfm_param.tx_size, is_inter_block(xd->mi[0]), cm->reduced_tx_set_used);
    if (txfm_param.is_hbd) {
      av1_highbd_inv_txfm_add(dqcoeff, dst, pd->dst.stride, &txfm_param);
      return;
    }
    av1_inv_txfm_add(dqcoeff, dst, pd->dst.stride, &txfm_param);
  }
}

void av1_encode_sby_pass1(AV1_COMMON *cm, MACROBLOCK *x, BLOCK_SIZE bsize) {
  encode_block_pass1_args args = { cm, x };
  av1_subtract_plane(x, bsize, 0);
  av1_foreach_transformed_block_in_plane(&x->e_mbd, bsize, 0,
                                         encode_block_pass1, &args);
}

void av1_encode_inter_txfm_block(const struct AV1_COMP *cpi, MACROBLOCK *x,
                                 int mi_row, int mi_col, RUN_TYPE dry_run) {
  (void)dry_run;
  const AV1_COMMON *const cm = &cpi->common;
  const int num_planes = av1_num_planes(cm);
  MACROBLOCKD *const xd = &x->e_mbd;
  struct optimize_ctx ctx;
  MB_MODE_INFO *mbmi = xd->mi[0];
  struct encode_b_args arg = {
    cpi,  x,    &ctx,    &mbmi->skip,
    NULL, NULL, dry_run, cpi->optimize_seg_arr[mbmi->segment_id]
  };
  int plane;

  // first set mbmi->skip = 1. inside encode_block_inter function, if certain
  // blocks' eob != 0, mbmi->skip will be set to 0. After all blocks are coded
  // if mbmi->skip is still 1, then this block should be coded as skip.
  mbmi->skip = 1;

  if (x->skip && dry_run == OUTPUT_ENABLED) {
    av1_init_txk_skip_array(cm, mbmi, mi_row, mi_col, mbmi->sb_type, 1,
                            cm->fEncTxSkipLog);
  }

  if (x->skip) return;

  for (plane = 0; plane < num_planes; ++plane) {
    if (plane && !mbmi->chroma_ref_info.is_chroma_ref) continue;

    const BLOCK_SIZE bsizec =
        plane ? mbmi->chroma_ref_info.bsize_base : mbmi->sb_type;

    // TODO(jingning): Clean this up.
    const struct macroblockd_plane *const pd = &xd->plane[plane];
    const BLOCK_SIZE plane_bsize =
        get_plane_block_size(bsizec, pd->subsampling_x, pd->subsampling_y);
    assert(plane_bsize < BLOCK_SIZES_ALL);
    const int mi_width = block_size_wide[plane_bsize] >> tx_size_wide_log2[0];
    const int mi_height = block_size_high[plane_bsize] >> tx_size_high_log2[0];
    const TX_SIZE max_tx_size = get_vartx_max_txsize(xd, plane_bsize, plane);

    const BLOCK_SIZE txb_size = txsize_to_bsize[max_tx_size];
    const int bw = block_size_wide[txb_size] >> tx_size_wide_log2[0];
    const int bh = block_size_high[txb_size] >> tx_size_high_log2[0];
    int idx, idy;
    int block = 0;
    int step = tx_size_wide_unit[max_tx_size] * tx_size_high_unit[max_tx_size];
    av1_get_entropy_contexts(bsizec, pd, ctx.ta[plane], ctx.tl[plane]);

    av1_subtract_plane(x, bsizec, plane);

    arg.ta = ctx.ta[plane];
    arg.tl = ctx.tl[plane];

    const BLOCK_SIZE max_unit_bsize =
        get_plane_block_size(BLOCK_64X64, pd->subsampling_x, pd->subsampling_y);
    int mu_blocks_wide =
        block_size_wide[max_unit_bsize] >> tx_size_wide_log2[0];
    int mu_blocks_high =
        block_size_high[max_unit_bsize] >> tx_size_high_log2[0];

    mu_blocks_wide = AOMMIN(mi_width, mu_blocks_wide);
    mu_blocks_high = AOMMIN(mi_height, mu_blocks_high);

    for (idy = 0; idy < mi_height; idy += mu_blocks_high) {
      for (idx = 0; idx < mi_width; idx += mu_blocks_wide) {
        int blk_row, blk_col;
        const int unit_height = AOMMIN(mu_blocks_high + idy, mi_height);
        const int unit_width = AOMMIN(mu_blocks_wide + idx, mi_width);
        for (blk_row = idy; blk_row < unit_height; blk_row += bh) {
          for (blk_col = idx; blk_col < unit_width; blk_col += bw) {
            encode_block_inter(plane, block, blk_row, blk_col, plane_bsize,
                               max_tx_size, &arg, mi_row, mi_col, dry_run);
            block += step;
          }
        }
      }
    }
  }
}

static void encode_block_intra_and_set_context(int plane, int block,
                                               int blk_row, int blk_col,
                                               BLOCK_SIZE plane_bsize,
                                               TX_SIZE tx_size, void *arg) {
  av1_encode_block_intra(plane, block, blk_row, blk_col, plane_bsize, tx_size,
                         arg);

  struct encode_b_args *const args = arg;
  MACROBLOCK *x = args->x;
  ENTROPY_CONTEXT *a = &args->ta[blk_col];
  ENTROPY_CONTEXT *l = &args->tl[blk_row];
  av1_set_txb_context(x, plane, block, tx_size, a, l);
}

void av1_encode_block_intra(int plane, int block, int blk_row, int blk_col,
                            BLOCK_SIZE plane_bsize, TX_SIZE tx_size,
                            void *arg) {
  struct encode_b_args *const args = arg;
  const AV1_COMMON *const cm = &args->cpi->common;
  MACROBLOCK *const x = args->x;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *mbmi = xd->mi[0];
  struct macroblock_plane *const p = &x->plane[plane];
  struct macroblockd_plane *const pd = &xd->plane[plane];
  tran_low_t *dqcoeff = BLOCK_OFFSET(pd->dqcoeff, block);
  PLANE_TYPE plane_type = get_plane_type(plane);
  const TX_TYPE tx_type = av1_get_tx_type(plane_type, xd, blk_row, blk_col,
                                          tx_size, cm->reduced_tx_set_used);
  uint16_t *eob = &p->eobs[block];
  const int dst_stride = pd->dst.stride;
  uint8_t *dst =
      &pd->dst.buf[(blk_row * dst_stride + blk_col) << tx_size_wide_log2[0]];
  int dummy_rate_cost = 0;

  av1_predict_intra_block_facade(cm, xd, plane, blk_col, blk_row, tx_size);

  const int bw = block_size_wide[plane_bsize] >> tx_size_wide_log2[0];
  if (plane == 0 && is_blk_skip(x, plane, blk_row * bw + blk_col)) {
    *eob = 0;
    p->txb_entropy_ctx[block] = 0;
  } else {
    av1_subtract_txb(x, plane, plane_bsize, blk_col, blk_row, tx_size);

    const ENTROPY_CONTEXT *a = &args->ta[blk_col];
    const ENTROPY_CONTEXT *l = &args->tl[blk_row];
    const int use_trellis =
        is_trellis_used(args->enable_optimize_b, args->dry_run);
    int quant_idx;
    if (use_trellis)
      quant_idx = AV1_XFORM_QUANT_FP;
    else
      quant_idx =
          USE_B_QUANT_NO_TRELLIS ? AV1_XFORM_QUANT_B : AV1_XFORM_QUANT_FP;
    if (use_trellis) {
      av1_xform_quant(cm, x, plane, block, blk_row, blk_col, plane_bsize,
                      tx_size, tx_type, quant_idx);
      TXB_CTX txb_ctx;
      get_txb_ctx(plane_bsize, tx_size, plane, a, l, &txb_ctx);
      av1_optimize_b(args->cpi, x, plane, block, tx_size, tx_type, &txb_ctx,
                     args->cpi->sf.trellis_eob_fast, &dummy_rate_cost);
    } else {
      av1_xform_quant(cm, x, plane, block, blk_row, blk_col, plane_bsize,
                      tx_size, tx_type, quant_idx);
    }
  }

  if (*eob) {
    av1_inverse_transform_block(xd, dqcoeff, plane, tx_type, tx_size, dst,
                                dst_stride, *eob, cm->reduced_tx_set_used);
  }

  // TODO(jingning): Temporarily disable txk_type check for eob=0 case.
  // It is possible that certain collision in hash index would cause
  // the assertion failure. To further optimize the rate-distortion
  // performance, we need to re-visit this part and enable this assert
  // again.
  if (*eob == 0 && plane == 0) {
#if 0
    if (args->cpi->oxcf.aq_mode == NO_AQ
        && args->cpi->oxcf.deltaq_mode == NO_DELTA_Q) {
      assert(mbmi->txk_type[av1_get_txk_type_index(plane_bsize, blk_row,
                                                   blk_col)] == DCT_DCT);
    }
#endif
    update_txk_array(mbmi->txk_type, plane_bsize, blk_row, blk_col, tx_size,
                     DCT_DCT);
  }

  if (*eob == 0 && args->dry_run == OUTPUT_ENABLED) {
    av1_update_txk_skip_array(cm, xd->mi_row, xd->mi_col, plane, blk_row,
                              blk_col, tx_size, cm->fEncTxSkipLog);
  }

  // For intra mode, skipped blocks are so rare that transmitting skip=1 is
  // very expensive.
  *(args->skip) = 0;

  if (plane == AOM_PLANE_Y && xd->cfl.store_y) {
    cfl_store_tx(xd, blk_row, blk_col, tx_size);
  }
}

void av1_encode_intra_block_plane(const struct AV1_COMP *cpi, MACROBLOCK *x,
                                  BLOCK_SIZE bsize, int plane, RUN_TYPE dry_run,
                                  TRELLIS_OPT_TYPE enable_optimize_b) {
  assert(bsize < BLOCK_SIZES_ALL);
  const MACROBLOCKD *const xd = &x->e_mbd;
  ENTROPY_CONTEXT ta[MAX_MIB_SIZE] = { 0 };
  ENTROPY_CONTEXT tl[MAX_MIB_SIZE] = { 0 };
  struct encode_b_args arg = { cpi, x,  NULL,    &(xd->mi[0]->skip),
                               ta,  tl, dry_run, enable_optimize_b };

  if (plane && !xd->mi[0]->chroma_ref_info.is_chroma_ref) return;

  if (enable_optimize_b) {
    const struct macroblockd_plane *const pd = &xd->plane[plane];
    const BLOCK_SIZE bsize_base =
        plane ? xd->mi[0]->chroma_ref_info.bsize_base : bsize;
    av1_get_entropy_contexts(bsize_base, pd, ta, tl);
  }
  av1_foreach_transformed_block_in_plane(
      xd, bsize, plane, encode_block_intra_and_set_context, &arg);
}
