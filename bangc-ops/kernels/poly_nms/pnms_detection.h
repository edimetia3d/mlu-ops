/*************************************************************************
 * Copyright (C) [2019-2022] by Cambricon, Inc.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *************************************************************************/

#ifndef KERNELS_PNMS_PNMS_DETECTION_H_
#define KERNELS_PNMS_PNMS_DETECTION_H_

#include "float.h"

#define PNMS_MIN (-(float)FLT_MAX)

const int BOX_SHAPE = 9;

template <typename IN_DT>
__mlu_func__ void quickSort(IN_DT *arr, int low, int high) {
  if (high <= low) return;
  int i = low;
  int j = high;
  int key = arr[low];
  while (true) {
    while (arr[i] <= key) {
      i++;
      if (i == high) {
        break;
      }
    }
    while (arr[j] >= key) {
      j--;
      if (j == low) {
        break;
      }
    }

    if (i >= j) break;
    int temp = arr[i];
    arr[i] = arr[j];
    arr[j] = temp;
  }

  arr[low] = arr[j];
  arr[j] = key;
  quickSort(arr, low, j - 1);
  quickSort(arr, j + 1, high);
}

// calculate max_score_box_area
// output: max_score| index | coordinate | sign box area | box area
// box_area=1/2 * ((x1*y2 - y1*x2) + (x2*y3-y2*x3) + (x3*y4 - y3*x4) + (x4*y1 -
// y4*x1))
template <typename IN_DT>
__mlu_func__ void calculateMaxScoreBoxArea(IN_DT *max_box) {
  auto max_box_coordinate = max_box + 2;
  max_box_coordinate[8] = max_box_coordinate[0];
  max_box_coordinate[9] = max_box_coordinate[1];
  auto max_area = 0.0;
  for (int j = 0; j < 8; j = j + 2) {
    max_area += (max_box_coordinate[j] * max_box_coordinate[j + 3] -
                 max_box_coordinate[j + 1] * max_box_coordinate[j + 2]);
  }
  max_area = max_area / 2;
  max_box_coordinate[8] = max_area;
  max_area = max_area > 0 ? max_area : -max_area;
  max_box_coordinate[9] = max_area;

  // if (max_area < 0) reverse coordinate  ABCD-->DCBA
  max_area = max_box[10];  //  max_box[10] sign max score box area
  if (max_area < 0) {
    auto tmp_x = max_box[2];
    auto tmp_y = max_box[3];
    max_box[2] = max_box[8];
    max_box[3] = max_box[9];
    max_box[8] = tmp_x;
    max_box[9] = tmp_y;

    tmp_x = max_box[4];
    tmp_y = max_box[5];
    max_box[4] = max_box[6];
    max_box[5] = max_box[7];
    max_box[6] = tmp_x;
    max_box[7] = tmp_y;
  }
}


// const float eps = 1E-8;  return (d > eps) - (d < -eps);
template <typename IN_DT>
__mlu_func__ void sig(IN_DT *output, IN_DT *input, IN_DT *tmp1, IN_DT *tmp2,
                      const int actual_box_num) {
  int length = actual_box_num;
  __bang_write_value(output, length, float(1E-8));
  __bang_write_value(tmp1, length, float(-1E-8));
  __bang_write_value(tmp2, length, float(0.0));

  __bang_gt(tmp2, input, output, length);
  __bang_lt(output, input, tmp1, length);
  __bang_sub(output, tmp2, output, length);
}

template <typename IN_DT>
__mlu_func__ void crossP3(IN_DT *result, IN_DT *p1_x, IN_DT *p1_y, IN_DT *p2_x,
                          IN_DT *p2_y, IN_DT *p3_x, IN_DT *p3_y,
                          const int length, IN_DT *temp1_ram,
                          IN_DT *temp2_ram) {
  int calculate_num = length;
  // crossP3<T>(o, a, b) = (a.x - o.x) * (b.y - o.y) - (b.x - o.x) * (a.y -
  // o.y);
  // a.x - o.x
  __bang_sub((IN_DT *)result, (IN_DT *)p2_x, (IN_DT *)p1_x, calculate_num);
  // b.y - o.y
  __bang_sub((IN_DT *)temp2_ram, (IN_DT *)p3_y, (IN_DT *)p1_y, calculate_num);
  // A =  (a.x - o.x) * (b.y - o.y)
  __bang_mul((IN_DT *)result, (IN_DT *)result, (IN_DT *)temp2_ram,
             calculate_num);
  // (b.x - o.x)
  __bang_sub((IN_DT *)temp1_ram, (IN_DT *)p3_x, (IN_DT *)p1_x, calculate_num);
  // (a.y - o.y)
  __bang_sub((IN_DT *)temp2_ram, (IN_DT *)p2_y, (IN_DT *)p1_y, calculate_num);
  // B = (b.x - o.x) * (a.y - o.y)
  __bang_mul((IN_DT *)temp1_ram, (IN_DT *)temp1_ram, (IN_DT *)temp2_ram,
             calculate_num);
  // A-B
  __bang_sub((IN_DT *)result, (IN_DT *)result, (IN_DT *)temp1_ram,
             calculate_num);
}

template <typename IN_DT>
__mlu_func__ void updatePAndPCount(IN_DT *p_x[], IN_DT *p_y[], IN_DT *p_count,
                                   IN_DT *pp_x, IN_DT *pp_y, IN_DT *px_ram,
                                   IN_DT *py_ram, IN_DT *buffer,
                                   const int pp_count, const int max_seg_num,
                                   const int actual_box_num,
                                   const int align_actual_box_num) {
  // n = 0;
  // for (int i = 0; i < m; i++)
  //     if (!i || !(pp[i] == pp[i - 1]))
  //         p[n++] = pp[i];
  // while (n > 1 && p[n - 1] == p[0])
  //     n--;
  __bang_write_value(p_count, max_seg_num, float(0.0));
  __bang_write_value(px_ram, 30 * max_seg_num, float(0.0));
  __bang_write_value(py_ram, 30 * max_seg_num, float(0.0));
  bool pp_vaild = false;
  for (int i = 0; i < actual_box_num; ++i) {
    bool first_loop = true;
    for (int j = 0; j < pp_count; ++j) {
      float valid_pp0_x = 0.0;
      float valid_pp0_y = 0.0;
      for (int t = j; t < pp_count; t++) {
        if (pp_x[i + t * max_seg_num] == PNMS_MIN ||
            pp_y[i + t * max_seg_num] == PNMS_MIN) {
          continue;
        } else {
          valid_pp0_x = pp_x[i + t * max_seg_num];
          valid_pp0_y = pp_y[i + t * max_seg_num];
          j = t;
          pp_vaild = true;
          break;
        }
      }

      if (pp_vaild == false) {
        break;
      }

      if (first_loop) {
        first_loop = false;
        px_ram[uint32_t(p_count[i]) * max_seg_num + i] = valid_pp0_x;
        py_ram[uint32_t(p_count[i]) * max_seg_num + i] = valid_pp0_y;
        p_count[i] = p_count[i] + 1;
        j--;
        continue;
      }

      if (j == pp_count - 1) {
        break;
      }
      j++;
      pp_vaild = false;
      auto valid_pp1_x = 0.0;
      auto valid_pp1_y = 0.0;

      for (int t = j; t < pp_count; t++) {
        if (pp_x[i + t * max_seg_num] == PNMS_MIN ||
            pp_y[i + t * max_seg_num] == PNMS_MIN) {
          continue;
        } else {
          valid_pp1_x = pp_x[i + t * max_seg_num];
          valid_pp1_y = pp_y[i + t * max_seg_num];
          pp_vaild = true;
          j = t;
          break;
        }
      }

      if (pp_vaild == false) {
        break;
      }

      if (valid_pp0_x != valid_pp1_x || valid_pp0_y != valid_pp1_y) {
        px_ram[uint32_t(p_count[i]) * max_seg_num + i] = valid_pp1_x;
        py_ram[uint32_t(p_count[i]) * max_seg_num + i] = valid_pp1_y;
        p_count[i] = p_count[i] + 1;
      }
      j--;
    }
  }

  // while (n > 1 && p[n - 1] == p[0]) n--;
  for (int i = 0; i < actual_box_num; ++i) {
    int n = uint32_t(p_count[i]);
    while (n > 1 && px_ram[(n - 1) * max_seg_num + i] == px_ram[i] &&
           py_ram[(n - 1) * max_seg_num + i] == py_ram[i]) {
      p_count[i] = p_count[i] - 1;
      n--;
    }
  }
  for (int i = 0; i < actual_box_num; ++i) {
    int n = uint32_t(p_count[i]);
    px_ram[n * max_seg_num + i] = px_ram[i];
    py_ram[n * max_seg_num + i] = py_ram[i];
  }

  uint32_t p_count_max = 0;
  __bang_max(buffer, p_count, align_actual_box_num);
  p_count_max = uint32_t(((float *)buffer)[0]);
  for (int j = 0; j < p_count_max + 1; ++j) {
    p_x[j] = px_ram + j * max_seg_num;
    p_y[j] = py_ram + j * max_seg_num;
  }
}

template <typename IN_DT>
__mlu_func__ void points_swap(IN_DT *boxes_pts_x0, IN_DT *boxes_pts_y0,
                              IN_DT *boxes_pts_x1, IN_DT *boxes_pts_y1,
                              const int idx) {
  auto tmp = boxes_pts_x0[idx];
  boxes_pts_x0[idx] = boxes_pts_x1[idx];
  boxes_pts_x1[idx] = tmp;

  tmp = boxes_pts_y0[idx];
  boxes_pts_y0[idx] = boxes_pts_y1[idx];
  boxes_pts_y1[idx] = tmp;
}

template <typename IN_DT>
__mlu_func__ void points_reverse(IN_DT *boxes_pts_x, IN_DT *boxes_pts_y,
                                 const int idx, const int max_seg_num) {
  auto tmp_x = boxes_pts_x[idx];
  auto tmp_y = boxes_pts_y[idx];
  boxes_pts_x[idx] = boxes_pts_x[idx + 3 * max_seg_num];
  boxes_pts_y[idx] = boxes_pts_y[idx + 3 * max_seg_num];
  boxes_pts_x[idx + 3 * max_seg_num] = tmp_x;
  boxes_pts_y[idx + 3 * max_seg_num] = tmp_y;

  tmp_x = boxes_pts_x[idx + 1 * max_seg_num];
  tmp_y = boxes_pts_y[idx + 1 * max_seg_num];
  boxes_pts_x[idx + 1 * max_seg_num] = boxes_pts_x[idx + 2 * max_seg_num];
  boxes_pts_y[idx + 1 * max_seg_num] = boxes_pts_y[idx + 2 * max_seg_num];
  boxes_pts_x[idx + 2 * max_seg_num] = tmp_x;
  boxes_pts_y[idx + 2 * max_seg_num] = tmp_y;
}

template <typename IN_DT>
__mlu_func__ void calPolygonSignArea(IN_DT *ret, IN_DT *p_x[], IN_DT *p_y[],
                                     IN_DT *count, const int actual_box_num) {
  __bang_write_value(ret, actual_box_num, float(0.0));
  for (int j = 0; j < actual_box_num; j++) {
    uint32_t n = uint32_t(count[j]);
    p_x[n][j] = p_x[0][j];
    p_y[n][j] = p_y[0][j];

    for (int i = 0; i < n; i++) {
      ret[j] += p_x[i][j] * p_y[i + 1][j] - p_y[i][j] * p_x[i + 1][j];
    }
    ret[j] = 0.5 * ret[j];
  }
}

template <typename IN_DT>
__mlu_func__ void computeDiv(IN_DT *result, const IN_DT *melo,
                             const IN_DT *denom, IN_DT *denom_tmp, IN_DT *tmp1,
                             IN_DT *tmp2, const int actual_box_num) {
  __bang_write_value(result, actual_box_num, float(0.0));
#if (__BANG_ARCH__ >= 300) && (__BANG_ARCH__ != 372)
  __bang_div((float *)result, (float *)melo, (float *)denom, actual_box_num);
#else
  // Calculation error when denominator is large
  for (int i = 0; i < actual_box_num; i++) {
    result[i] = melo[i] / denom[i];
  }
//   __bang_active_sign((float *)tmp1, (float *)denom, actual_box_num);
//   __bang_active_abs((float *)tmp2, (float *)denom, actual_box_num);
// #if __BANG_ARCH__ == 372
//   __bang_recip((float *)denom_tmp, (float *)tmp2, actual_box_num);
// #else
//   __bang_active_reciphp((float *)denom_tmp, (float *)tmp2, actual_box_num);
// #endif
//   __bang_mul((float *)result, (float *)melo, (float *)denom_tmp,
//              actual_box_num);
//   __bang_mul((float *)result, (float *)result, (float *)tmp1, actual_box_num);
// #endif
}

template <typename IN_DT>
__mlu_func__ void lineCross(IN_DT *a_x, IN_DT *a_y, IN_DT *b_x, IN_DT *b_y,
                            IN_DT *p1_x, IN_DT *p1_y, IN_DT *p2_x, IN_DT *p2_y,
                            IN_DT *pp_x, IN_DT *pp_y, IN_DT *valid_pts,
                            uint32_t pp_count, IN_DT *cross_s1, IN_DT *cross_s2,
                            IN_DT *sig_cross_s1, IN_DT *sig_cross_s2,
                            IN_DT *nram_tmp, const int max_seg_num,
                            const int actual_box_num,
                            const int align_actual_box_num) {
  IN_DT *p_tmp1;
  IN_DT *p_tmp2;
  IN_DT *p_melo;
  IN_DT *p_denom;
  IN_DT *tmp_zero;
  IN_DT *p_denom_tmp;
  IN_DT *mask_sig_eq0;

  p_tmp1 = nram_tmp;
  p_tmp2 = p_tmp1 + 1 * max_seg_num;
  p_melo = p_tmp1 + 2 * max_seg_num;
  p_denom = p_tmp1 + 3 * max_seg_num;
  tmp_zero = p_tmp1 + 4 * max_seg_num;
  p_denom_tmp = p_tmp1 + 5 * max_seg_num;
  mask_sig_eq0 = p_tmp1 + 6 * max_seg_num;

  // if (sig(s1) == 0 && sig(s2) == 0) return
  __bang_write_value(tmp_zero, align_actual_box_num, float(0.0));
  __bang_ne(p_tmp1, sig_cross_s1, tmp_zero, align_actual_box_num);
  __bang_ne(p_tmp2, sig_cross_s2, tmp_zero, align_actual_box_num);
  __bang_or(mask_sig_eq0, p_tmp1, p_tmp2, align_actual_box_num);

  //  if (sig(s2 - s1) == 0) return
  __bang_sub(p_tmp1, cross_s2, cross_s1, align_actual_box_num);
  sig(p_tmp2, p_tmp1, p_melo, p_denom, align_actual_box_num);
  __bang_ne(valid_pts, p_tmp2, tmp_zero, align_actual_box_num);
  //  if (sig(s2 - s1) == 0) return  ||  (sig(s1) == 0 && sig(s2) == 0) return
  __bang_mul(valid_pts, valid_pts, mask_sig_eq0,
             align_actual_box_num);  // and->or

  // line cross a,b,p0,p1
  // p.x = (c.x * s2 - d.x * s1) / (s2 - s1);
  __bang_mul((float *)p_tmp1, (float *)p1_x, (float *)cross_s2,
             align_actual_box_num);
  __bang_mul(p_tmp2, p2_x, cross_s1, align_actual_box_num);
  __bang_sub(p_melo, p_tmp1, p_tmp2, align_actual_box_num);

  // s2 - s1
  __bang_sub(p_denom, cross_s2, cross_s1, align_actual_box_num);

  // set 1 with (s2 -s1 = 0)
  __bang_eq(p_denom_tmp, p_denom, tmp_zero, align_actual_box_num);
  __bang_add(p_denom, p_denom, p_denom_tmp, align_actual_box_num);

  // compute div
  computeDiv((float *)(pp_x + pp_count * max_seg_num), (float *)p_melo,
             (float *)p_denom, (float *)p_denom_tmp, (float *)p_tmp1,
             (float *)p_tmp2, align_actual_box_num);
  // p.y = (c.y * s2 - d.y * s1) / (s2 - s1);
  __bang_mul(p_tmp1, p1_y, cross_s2, align_actual_box_num);
  __bang_mul(p_tmp2, p2_y, cross_s1, align_actual_box_num);
  __bang_sub(p_melo, p_tmp1, p_tmp2, align_actual_box_num);
  // compute div
  computeDiv((float *)(pp_y + pp_count * max_seg_num), (float *)p_melo,
             (float *)p_denom, (float *)p_denom_tmp, (float *)p_tmp1,
             (float *)p_tmp2, align_actual_box_num);
}

template <typename IN_DT>
__mlu_func__ void polygon_cut(IN_DT *p_x[], IN_DT *p_y[], IN_DT *p_count,
                              IN_DT *a_x, IN_DT *a_y, IN_DT *b_x, IN_DT *b_y,
                              IN_DT *pp_x, IN_DT *pp_y, IN_DT *buffer,
                              const int max_seg_num, const int actual_box_num,
                              int align_actual_box_num) {
  IN_DT *cross_s1;      // 1
  IN_DT *cross_s2;      // 1
  IN_DT *sig_cross_s1;  // 1
  IN_DT *sig_cross_s2;  // 1
  IN_DT *tmp_zero;      // 1
  IN_DT *mask_sig_ne;   // 1
  IN_DT *valid_pts;     // 1
  IN_DT *px_ram;        // 10
  IN_DT *py_ram;        // 10
  IN_DT *s1_tmp1;       // 1
  IN_DT *s1_tmp2;       // 1
  IN_DT *tmp1;          // 1
  IN_DT *invalid_pts;   // 1
  IN_DT *nram_tmp;

  cross_s1 = buffer;
  cross_s2 = cross_s1 + 1 * max_seg_num;
  sig_cross_s1 = cross_s1 + 2 * max_seg_num;
  sig_cross_s2 = cross_s1 + 3 * max_seg_num;
  tmp_zero = cross_s1 + 4 * max_seg_num;
  mask_sig_ne = cross_s1 + 5 * max_seg_num;
  valid_pts = cross_s1 + 6 * max_seg_num;
  px_ram = cross_s1 + 7 * max_seg_num;   // 10
  py_ram = cross_s1 + 17 * max_seg_num;  // 10
  s1_tmp1 = cross_s1 + 27 * max_seg_num;
  s1_tmp2 = cross_s1 + 28 * max_seg_num;
  tmp1 = cross_s1 + 29 * max_seg_num;
  invalid_pts = cross_s1 + 30 * max_seg_num;
  nram_tmp = cross_s1 + 31 * max_seg_num;

  uint32_t p_count_max = 0;
  __bang_max(tmp1, p_count, align_actual_box_num);
  p_count_max = uint32_t(((float *)tmp1)[0]);
  int pp_count = 0;

  // Loop according to the maximum value of p_count.
  // Invalid pp value is set to PNMS_MIN.
  for (int n = 0; n < p_count_max; n++) {
    // cross(a,b,p[i])
    crossP3(cross_s1, a_x, a_y, b_x, b_y, p_x[n], p_y[n], align_actual_box_num,
            s1_tmp1, s1_tmp2);
    // cross(a,b,p[i+1])
    crossP3(cross_s2, a_x, a_y, b_x, b_y, p_x[n + 1], p_y[n + 1],
            align_actual_box_num, s1_tmp1, s1_tmp2);

    sig(sig_cross_s1, cross_s1, s1_tmp1, s1_tmp2, align_actual_box_num);
    sig(sig_cross_s2, cross_s2, s1_tmp1, s1_tmp2, align_actual_box_num);

    //  if (sig(cross(a, b, p[i])) > 0) pp[m++] = p[i]
    __bang_write_value(tmp_zero, align_actual_box_num, float(0.0));
    __bang_gt(valid_pts, sig_cross_s1, tmp_zero, align_actual_box_num);
    // pp = sig_gt_ret *  p[n];
    __bang_mul(pp_x + pp_count * max_seg_num, valid_pts, p_x[n],
               align_actual_box_num);
    __bang_mul(pp_y + pp_count * max_seg_num, valid_pts, p_y[n],
               align_actual_box_num);

    // s1 <= 0, set PNMS_MIN with unvailds pp_x, pp_y
    __bang_eq(invalid_pts, valid_pts, tmp_zero, align_actual_box_num);
    __bang_mul_const(invalid_pts, invalid_pts, float(PNMS_MIN),
                     align_actual_box_num);
    __bang_add(pp_x + pp_count * max_seg_num, pp_x + pp_count * max_seg_num,
               invalid_pts, align_actual_box_num);
    __bang_add(pp_y + pp_count * max_seg_num, pp_y + pp_count * max_seg_num,
               invalid_pts, align_actual_box_num);
    pp_count++;

    // if (sig(cross(a, b, p[i])) != sig(cross(a, b, p[i + 1]))):
    __bang_ne(mask_sig_ne, sig_cross_s1, sig_cross_s2, align_actual_box_num);

    lineCross(a_x, a_y, b_x, b_y, p_x[n], p_y[n], p_x[n + 1], p_y[n + 1], pp_x,
              pp_y, valid_pts, pp_count, cross_s1, cross_s2, sig_cross_s1,
              sig_cross_s2, nram_tmp, max_seg_num, actual_box_num,
              align_actual_box_num);

    // valid_pts = valid_pts || if (sig(cross(a, b, p[i])) != sig(cross(a, b,
    // p[i + 1])))
    __bang_mul(valid_pts, valid_pts, mask_sig_ne, align_actual_box_num);

    __bang_mul(pp_x + pp_count * max_seg_num, pp_x + pp_count * max_seg_num,
               valid_pts, align_actual_box_num);
    __bang_mul(pp_y + pp_count * max_seg_num, pp_y + pp_count * max_seg_num,
               valid_pts, align_actual_box_num);

    // set PNMS_MIN with unvailds pp_x, pp_y
    __bang_eq(invalid_pts, valid_pts, tmp_zero, align_actual_box_num);
    __bang_mul_const(invalid_pts, invalid_pts, float(PNMS_MIN),
                     align_actual_box_num);
    __bang_add(pp_x + pp_count * max_seg_num, pp_x + pp_count * max_seg_num,
               invalid_pts, align_actual_box_num);
    __bang_add(pp_y + pp_count * max_seg_num, pp_y + pp_count * max_seg_num,
               invalid_pts, align_actual_box_num);
    pp_count++;
  }  // for(int n = 0;n<p_count;n++)

  updatePAndPCount(p_x, p_y, p_count, pp_x, pp_y, px_ram, py_ram, nram_tmp,
                   pp_count, max_seg_num, actual_box_num, align_actual_box_num);
}

template <typename IN_DT>
__mlu_func__ void intersectArea(
    IN_DT *area, const IN_DT *const_max_box_pts_x0,
    const IN_DT *const_max_box_pts_y0, const IN_DT *const_max_box_pts_x1,
    const IN_DT *const_max_box_pts_y1, const IN_DT *const_box_pts_x0,
    const IN_DT *const_box_pts_y0, const IN_DT *const_box_pts_x1,
    const IN_DT *const_box_pts_y1, IN_DT *buffer, const int max_seg_num,
    const int actual_box_num, const int align_actual_box_num) {
  // 14 + 60 + buffer
  IN_DT *o_x;             // 1
  IN_DT *o_y;             // 1
  IN_DT *box_pts_x0;      // 1
  IN_DT *box_pts_y0;      // 1
  IN_DT *box_pts_x1;      // 1
  IN_DT *box_pts_y1;      // 1
  IN_DT *max_box_pts_x0;  // 1
  IN_DT *max_box_pts_y0;  // 1
  IN_DT *max_box_pts_x1;
  IN_DT *max_box_pts_y1;
  IN_DT *s1;
  IN_DT *s2;
  IN_DT *p_count;
  IN_DT *mask_s1_s2_eqf1;
  IN_DT *pp_x;  // 30
  IN_DT *pp_y;  // 30
  IN_DT *p_x[10];
  IN_DT *p_y[10];
  IN_DT *nram_tmp;

  IN_DT *s_c1;
  IN_DT *s_c2;
  IN_DT *s_tmp1;
  IN_DT *s_tmp2;
  IN_DT *temp3_ram;
  IN_DT *mask_vaild_pts;
  IN_DT *mask_s1_eq0;  // = s_tmp1;
  IN_DT *mask_s2_eq0;  // = s_tmp2;

  o_x = (IN_DT *)buffer;
  o_y = o_x + max_seg_num;
  box_pts_x0 = o_y + max_seg_num;                 // 1
  box_pts_y0 = box_pts_x0 + max_seg_num;          // 1
  box_pts_x1 = box_pts_y0 + max_seg_num;          // 1
  box_pts_y1 = box_pts_x1 + max_seg_num;          // 1
  max_box_pts_x0 = box_pts_y1 + max_seg_num;      // 1
  max_box_pts_y0 = max_box_pts_x0 + max_seg_num;  // 1
  max_box_pts_x1 = max_box_pts_y0 + max_seg_num;  // 1
  max_box_pts_y1 = max_box_pts_x1 + max_seg_num;  // 1

  s1 = max_box_pts_y1 + max_seg_num;
  s2 = s1 + max_seg_num;
  p_count = s2 + max_seg_num;
  mask_s1_s2_eqf1 = p_count + max_seg_num;
  mask_vaild_pts = mask_s1_s2_eqf1 + max_seg_num;
  pp_x = mask_vaild_pts + max_seg_num;
  pp_y = pp_x + 30 * max_seg_num;
  nram_tmp = pp_y + 30 * max_seg_num;

  s_c1 = nram_tmp;
  s_c2 = s_c1 + max_seg_num;
  s_tmp1 = s_c2 + max_seg_num;
  s_tmp2 = s_tmp1 + max_seg_num;
  temp3_ram = s_tmp2 + max_seg_num;
  mask_s1_eq0 = temp3_ram + max_seg_num;
  mask_s2_eq0 = mask_s1_eq0 + max_seg_num;

  __bang_write_value(o_x, 74 * max_seg_num, float(0.0));

  __memcpy(box_pts_x0, const_box_pts_x0, max_seg_num * 4, NRAM2NRAM);
  __memcpy(box_pts_y0, const_box_pts_y0, max_seg_num * 4, NRAM2NRAM);
  __memcpy(box_pts_x1, const_box_pts_x1, max_seg_num * 4, NRAM2NRAM);
  __memcpy(box_pts_y1, const_box_pts_y1, max_seg_num * 4, NRAM2NRAM);

  __memcpy(max_box_pts_x0, const_max_box_pts_x0, max_seg_num * 4, NRAM2NRAM);
  __memcpy(max_box_pts_y0, const_max_box_pts_y0, max_seg_num * 4, NRAM2NRAM);
  __memcpy(max_box_pts_x1, const_max_box_pts_x1, max_seg_num * 4, NRAM2NRAM);
  __memcpy(max_box_pts_y1, const_max_box_pts_y1, max_seg_num * 4, NRAM2NRAM);

  // a b max_box_pts, cd  box_pts
  // corss_oab
  crossP3(s_c1, o_x, o_y, max_box_pts_x0, max_box_pts_y0, max_box_pts_x1,
          max_box_pts_y1, align_actual_box_num, s_tmp1, s_tmp2);
  // corss_ocd
  crossP3(s_c2, o_x, o_y, box_pts_x0, box_pts_y0, box_pts_x1, box_pts_y1,
          align_actual_box_num, s_tmp1, s_tmp2);

  sig(s1, s_c1, s_tmp1, s_tmp2, align_actual_box_num);
  sig(s2, s_c2, s_tmp1, s_tmp2, align_actual_box_num);

  // if (s1 == 0 || s2 == 0) return valid pts mask
  __bang_write_value(temp3_ram, align_actual_box_num, float(0.0));
  __bang_ne((float *)mask_s1_eq0, (float *)s1, (float *)temp3_ram,
            align_actual_box_num);
  __bang_ne((float *)mask_s2_eq0, (float *)s2, (float *)temp3_ram,
            align_actual_box_num);
  __bang_and((float *)mask_vaild_pts, (float *)mask_s1_eq0,
             (float *)mask_s2_eq0, align_actual_box_num);

  // swap boxes_points with s1=-1 s2=-1
  for (int j = 0; j < actual_box_num; j++) {
    if (s1[j] < 0) {
      points_swap(max_box_pts_x0, max_box_pts_y0, max_box_pts_x1,
                  max_box_pts_y1, j);
    }
    if (s2[j] < 0) {
      points_swap(box_pts_x0, box_pts_y0, box_pts_x1, box_pts_y1, j);
    }
  }

  // // polygon cut
  // // p(o,a,b) 3 o c
  p_x[0] = o_x;
  p_x[1] = max_box_pts_x0;  // box_pts_x0;
  p_x[2] = max_box_pts_x1;  // box_pts_x1;
  p_x[3] = o_x;

  p_y[0] = o_y;
  p_y[1] = max_box_pts_y0;  // box_pts_y0;
  p_y[2] = max_box_pts_y1;  // box_pts_y1;
  p_y[3] = o_y;

  IN_DT *a_x;
  IN_DT *a_y;
  IN_DT *b_x;
  IN_DT *b_y;

  a_x = o_x;
  a_y = o_y;
  b_x = box_pts_x0;
  b_y = box_pts_y0;
  __bang_write_value(p_count, max_seg_num, float(3.0));
  __bang_write_value(pp_x, 30 * max_seg_num, float(0.0));
  __bang_write_value(pp_y, 30 * max_seg_num, float(0.0));

  polygon_cut(p_x, p_y, p_count, a_x, a_y, b_x, b_y, pp_x, pp_y, nram_tmp,
              max_seg_num, actual_box_num, align_actual_box_num);

  a_x = box_pts_x0;
  a_y = box_pts_y0;
  b_x = box_pts_x1;
  b_y = box_pts_y1;
  polygon_cut(p_x, p_y, p_count, a_x, a_y, b_x, b_y, pp_x, pp_y, nram_tmp,
              max_seg_num, actual_box_num, align_actual_box_num);

  a_x = box_pts_x1;
  a_y = box_pts_y1;
  b_x = o_x;
  b_y = o_y;
  polygon_cut(p_x, p_y, p_count, a_x, a_y, b_x, b_y, pp_x, pp_y, nram_tmp,
              max_seg_num, actual_box_num, align_actual_box_num);

  calPolygonSignArea<float>((float *)area, p_x, p_y, p_count,
                            align_actual_box_num);
  __bang_active_abs((float *)area, (float *)area, align_actual_box_num);

  __bang_mul((float *)area, (float *)mask_vaild_pts, (float *)area,
             align_actual_box_num);

  // f (s1 * s2 == -1) res = -res;
  __bang_write_value((void *)temp3_ram, align_actual_box_num, float(-1.0));
  __bang_mul(mask_s1_s2_eqf1, s1, s2, align_actual_box_num);
  __bang_eq(s1, mask_s1_s2_eqf1, (float *)temp3_ram, align_actual_box_num);
  __bang_ne(s2, mask_s1_s2_eqf1, (float *)temp3_ram, align_actual_box_num);

  __bang_mul(mask_s1_s2_eqf1, s1, (float *)temp3_ram, align_actual_box_num);
  __bang_add(mask_s1_s2_eqf1, mask_s1_s2_eqf1, s2, align_actual_box_num);

  __bang_mul(area, area, mask_s1_s2_eqf1, align_actual_box_num);
}
template <typename IN_DT, typename OUT_DT>
__mlu_func__ void calculateBoxesArea(const IN_DT *x1, const IN_DT *y1,
                                     const IN_DT *x2, const IN_DT *y2,
                                     const IN_DT *x3, const IN_DT *y3,
                                     const IN_DT *x4, const IN_DT *y4,
                                     OUT_DT *area, OUT_DT *tmp,
                                     const int input_stride) {
  // calculate polygon area
  // polygon vertexs:(x1,y1),(x2,y2),(x3,y3),(x4,y4)
  // polygon_area= abs(1/2 * ((x1*y2 - y1*x2) + (x2*y3-y2*x3) + (x3*y4 - y3*x4)
  // + (x4*y1 - y4*x1)))
  int calculate_num = input_stride;
  // x1*y2
  __bang_mul((IN_DT *)area, (IN_DT *)x1, (IN_DT *)y2,
             calculate_num * sizeof(IN_DT));
  __bang_mul((IN_DT *)tmp, (IN_DT *)y1, (IN_DT *)x2,
             calculate_num * sizeof(IN_DT));
  // x1*y2 - y1*x2
  __bang_sub((IN_DT *)area, (IN_DT *)area, (IN_DT *)tmp,
             calculate_num * sizeof(IN_DT));
  // x1*y2 - y1*x2 + x2*y3
  __bang_mul((IN_DT *)tmp, (IN_DT *)x2, (IN_DT *)y3,
             calculate_num * sizeof(IN_DT));
  __bang_add((IN_DT *)area, (IN_DT *)area, (IN_DT *)tmp,
             calculate_num * sizeof(IN_DT));
  // x1*y2 - y1*x2 + x2*y3-y2*x3
  __bang_mul((IN_DT *)tmp, (IN_DT *)y2, (IN_DT *)x3,
             calculate_num * sizeof(IN_DT));
  __bang_sub((IN_DT *)area, (IN_DT *)area, (IN_DT *)tmp,
             calculate_num * sizeof(IN_DT));
  // x1*y2 - y1*x2 + x2*y3-y2*x3 + x3*y4
  __bang_mul((IN_DT *)tmp, (IN_DT *)x3, (IN_DT *)y4,
             calculate_num * sizeof(IN_DT));
  __bang_add((IN_DT *)area, (IN_DT *)area, (IN_DT *)tmp,
             calculate_num * sizeof(IN_DT));
  // x1*y2 - y1*x2 + x2*y3-y2*x3) + x3*y4 - y3*x4
  __bang_mul((IN_DT *)tmp, (IN_DT *)y3, (IN_DT *)x4,
             calculate_num * sizeof(IN_DT));
  __bang_sub((IN_DT *)area, (IN_DT *)area, (IN_DT *)tmp,
             calculate_num * sizeof(IN_DT));
  // x1*y2 - y1*x2 + x2*y3-y2*x3) + x3*y4 - y3*x4 + x4*y1
  __bang_mul((IN_DT *)tmp, (IN_DT *)x4, (IN_DT *)y1,
             calculate_num * sizeof(IN_DT));
  __bang_add((IN_DT *)area, (IN_DT *)area, (IN_DT *)tmp,
             calculate_num * sizeof(IN_DT));
  // x1*y2 - y1*x2 + x2*y3-y2*x3) + x3*y4 - y3*x4 + x4*y1 - y4*x1
  __bang_mul((IN_DT *)tmp, (IN_DT *)y4, (IN_DT *)x1,
             calculate_num * sizeof(IN_DT));
  __bang_sub((IN_DT *)area, (IN_DT *)area, (IN_DT *)tmp,
             calculate_num * sizeof(IN_DT));
  // (x1*y2 - y1*x2 + x2*y3-y2*x3) + x3*y4 - y3*x4 + x4*y1 - y4*x1)*0.5
  __bang_mul_scalar((IN_DT *)area, (IN_DT *)area, (IN_DT)0.5,
                    calculate_num * sizeof(IN_DT));
}

template <typename IN_DT>
__mlu_func__ void calculateOverlapArea(
    IN_DT *box_pts_x, IN_DT *box_pts_y, const IN_DT *scores, IN_DT *max_box,
    IN_DT *max_box_pts_x, IN_DT *max_box_pts_y, IN_DT *boxes_area,
    IN_DT *intersection_area, IN_DT *nram_tmp, const int max_seg_num,
    const int actual_box_num, const int align_actual_box_num) {

  // nram_tmp
  // |area_tmp| buffer
  // |  1      |
  IN_DT *area_tmp;
  IN_DT *buffer;

  area_tmp = nram_tmp;
  buffer = area_tmp + max_seg_num;

  // reverse boxes_points with area < 0
  for (int j = 0; j < actual_box_num; j++) {
    if (boxes_area[j] < 0) {
      points_reverse(box_pts_x, box_pts_y, j, max_seg_num);
    }
  }

  // after reverse points coord, update box_area
  calculateBoxesArea(box_pts_x, box_pts_y, box_pts_x + 1 * max_seg_num,
                     box_pts_y + 1 * max_seg_num, box_pts_x + 2 * max_seg_num,
                     box_pts_y + 2 * max_seg_num, box_pts_x + 3 * max_seg_num,
                     box_pts_y + 3 * max_seg_num, boxes_area, area_tmp,
                     align_actual_box_num);

  __bang_write_value(intersection_area, align_actual_box_num, float(0.0));
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      int m = i == 3 ? 0 : (i + 1);
      int n = j == 3 ? 0 : (j + 1);
      intersectArea<float>(
          area_tmp, max_box_pts_x + i * max_seg_num,
          max_box_pts_y + i * max_seg_num, max_box_pts_x + m * max_seg_num,
          max_box_pts_y + m * max_seg_num, box_pts_x + j * max_seg_num,
          box_pts_y + j * max_seg_num, box_pts_x + n * max_seg_num,
          box_pts_y + n * max_seg_num, buffer, max_seg_num, actual_box_num,
          align_actual_box_num);
      __bang_add(intersection_area, intersection_area, area_tmp,
                 align_actual_box_num);
    }
  }
}

template <typename IN_DT>
__mlu_func__ void getMaxScoreIndex(IN_DT *input_box_ptr, IN_DT *input_score_ptr,
                                   IN_DT *max_box, IN_DT *buffer,
                                   IN_DT *sram_buffer, const int buffer_size,
                                   const int scores_num, const int input_offset,
                                   const int input_stride,
                                   const int core_limit) {
  //  | nram_save| max_box|  max_box_tmp     |  scores |
  //  | 1| BANG_MAX_ALIGN |  BANG_MAX_ALIGN  |  N      |
  const int INPUT_TYPE_SIZE = sizeof(IN_DT);
  const int BANG_MAX_ALIGN = NFU_ALIGN_SIZE;
  int align_num = BANG_MAX_ALIGN / INPUT_TYPE_SIZE;

  IN_DT *max_box_tmp;
  IN_DT *scores;

  int32_t max_buffer_num = (buffer_size - NFU_ALIGN_SIZE) / INPUT_TYPE_SIZE;
  int32_t max_seg_num = FLOOR_ALIGN(max_buffer_num, align_num);
  int32_t max_seg_pad = max_seg_num * INPUT_TYPE_SIZE;
  int32_t repeat = scores_num / max_seg_num;
  int32_t remain = scores_num % max_seg_num;
  int32_t remain_pad = CEIL_ALIGN(remain * INPUT_TYPE_SIZE, BANG_MAX_ALIGN);

  // | max_box_tmp|scores|
  max_box_tmp = buffer;
  scores = max_box_tmp + align_num;

  int32_t max_index = 0;
  max_box[0] = (float)PNMS_MIN;

  for (int i = 0; i <= repeat; i++) {
    if (i == repeat && remain == 0) {
      break;
    }
    int actual_scores_num = (i == repeat) ? remain : max_seg_num;
    int actual_scores_pad = (i == repeat) ? remain_pad : max_seg_pad;

    __bang_write_value((float *)scores, actual_scores_pad / 4, (float)PNMS_MIN);
    __memcpy(scores, input_score_ptr + input_offset + i * max_seg_num,
             actual_scores_num * INPUT_TYPE_SIZE, GDRAM2NRAM);

    __bang_max(max_box_tmp, scores, actual_scores_pad / 4);
    if (max_box_tmp[0] > max_box[0]) {
      max_box[0] = max_box_tmp[0];
      max_index = ((uint32_t *)max_box_tmp)[1] + input_offset + i * max_seg_num;
      max_box[1] = max_index;
    }
  }  // for repeat

  if (core_limit == 1) {
    // get max_score_box coordinate
    // max_box
    // | max_score| max_score_index | max_box_coordinate | max_score_box_area|
    // | 1        |       1         |          8         |     1 |
    // input_box_ptr:x1---, y1---, x2---, y2---, x3---, y3---, x4---,
    // y4---,scores---
    __memcpy(max_box + 2, input_box_ptr + max_index, 1 * INPUT_TYPE_SIZE,
             GDRAM2NRAM, 1 * sizeof(uint32_t), input_stride * sizeof(uint32_t),
             8);

    // calculate max_score_box_area
    // output: max_score| index | coordinate | sign box area | box area
    calculateMaxScoreBoxArea(max_box);
    input_score_ptr[uint32_t(max_box[1])] = PNMS_MIN;
  } else {
    // sram_buffer: max_score1 | index1 | max_score2 | index2 ...
    __memcpy(sram_buffer + 2 * taskId, max_box, 2 * INPUT_TYPE_SIZE, NRAM2SRAM);
    __sync_cluster();

    __bang_write_value(scores, BANG_MAX_ALIGN / 4, PNMS_MIN);
    for (int j = 0; j < core_limit; j++) {
      scores[j] = sram_buffer[j * 2];
    }

    __bang_max(max_box_tmp, scores, BANG_MAX_ALIGN / 4);
    max_box[0] = max_box_tmp[0];
    max_index = ((uint32_t *)max_box_tmp)[1];
    max_box[1] = (uint32_t)(sram_buffer[max_index * 2 + 1]);

    max_index = uint32_t(max_box[1]);
    input_score_ptr[max_index] = PNMS_MIN;

    __memcpy(max_box + 2, input_box_ptr + max_index, 1 * INPUT_TYPE_SIZE,
             GDRAM2NRAM, 1 * sizeof(uint32_t), input_stride * sizeof(uint32_t),
             8);
    calculateMaxScoreBoxArea(max_box);
  }  // if (core_limit == 4)
}

template <typename IN_DT, typename OUT_DT>
__mlu_func__ void pnms_detection(OUT_DT *result_num, OUT_DT *output_data,
                                 IN_DT *input_data_box, IN_DT *buffer,
                                 const int buffer_size, IN_DT *sram_buffer,
                                 const int core_limit, const int input_data_num,
                                 const int input_stride,
                                 const float thresh_iou) {
  uint32_t output_box_num = 0;
  // NRAM N=max_seg_pad
  // max_box(max_score, max_box_coordinate, max_index,+-max_area, max_area)

  // |nram_save | max_box |max_box_pts_x|max_box_pts_y| box_pts_x |
  // |  N |NFU_ALIGN_SIZE |    N*4      |  N*4        |   N*4     |

  //  box_pts_y | scores | box_area |intersection_area| nram_save |
  //  N*4     | N      |   N      |     N           | overlap   |

  const int INPUT_TYPE_SIZE = sizeof(IN_DT);

  int input_boxes_num = input_stride;
  int input_offset_num = 0;
  if (core_limit == 1) {
    input_boxes_num = input_stride;
    input_offset_num = 0;
  } else {
    int avg_core = input_boxes_num / core_limit;
    int rem = input_boxes_num % core_limit;
    input_boxes_num = avg_core + (taskId < rem ? 1 : 0);
    input_offset_num = avg_core * taskId + (taskId <= rem ? taskId : rem);
  }

  const int memory_block = 137;
  int limit = (buffer_size - NFU_ALIGN_SIZE) / memory_block;
  int max_seg_pad = FLOOR_ALIGN(limit, NFU_ALIGN_SIZE * BOX_SHAPE);
  int max_seg_num = max_seg_pad / INPUT_TYPE_SIZE;

  int repeat = input_boxes_num / max_seg_num;
  int remain_num = input_boxes_num % max_seg_num;
  int remain_align_num = CEIL_ALIGN(remain_num, NFU_ALIGN_SIZE / 4);

  // gdram
  IN_DT *input_box_ptr;
  IN_DT *input_score_ptr;

  input_box_ptr = input_data_box;
  input_score_ptr = input_box_ptr + 8 * input_stride;

  // sram space
  int32_t *loop_end_flag = (int32_t *)((char *)sram_buffer);
  IN_DT *sram_space = (IN_DT *)((char *)loop_end_flag + sizeof(int32_t));
  loop_end_flag[0] = 0;

  // init nram ptr
  IN_DT *box_pts_x;
  IN_DT *box_pts_y;
  IN_DT *scores;
  IN_DT *max_box;
  OUT_DT *nram_save;
  IN_DT *nram_tmp;
  IN_DT *box_area_tmp;
  IN_DT *box_area;
  IN_DT *max_box_pts_x;
  IN_DT *max_box_pts_y;
  IN_DT *intersection_area;
  IN_DT *get_max_score_buffer;

  nram_save = (OUT_DT *)((char *)buffer);
  max_box = (IN_DT *)((char *)nram_save + max_seg_pad);
  max_box_pts_x = max_box + NFU_ALIGN_SIZE / INPUT_TYPE_SIZE;
  get_max_score_buffer = max_box_pts_x;

  max_box_pts_y = max_box_pts_x + 4 * max_seg_num;
  box_pts_x = max_box_pts_y + 4 * max_seg_num;
  box_pts_y = box_pts_x + 4 * max_seg_num;
  scores = box_pts_y + 4 * max_seg_num;
  box_area = scores + max_seg_num;
  intersection_area = box_area + max_seg_num;
  nram_tmp = intersection_area + max_seg_pad;
  box_area_tmp = nram_tmp;

  int nram_save_count = 0;
  const int nram_save_limit_count = max_seg_num;
  int max_output_size = input_stride;
  int output_save_count = 0;

  // max_output_size   = 2; //debug
  for (int loop = 0; loop < max_output_size; loop++) {
    if (core_limit != 1) {
      __sync_cluster();  // sync before current loop
    }

    // look for max_score
    // 1 get_max_box_index();
    // output: max_box (max_score, max_index, max_box_coordinate, sign box_area,
    // max_score_box_area)
    uint32_t scoreIndexBufSize =
        buffer_size - max_seg_pad -
        NFU_ALIGN_SIZE;  // nram size - nram_save - max_box
    getMaxScoreIndex(input_box_ptr, input_score_ptr, max_box,
                     get_max_score_buffer, sram_space, scoreIndexBufSize,
                     input_boxes_num, input_offset_num, input_stride,
                     core_limit);
    // store max_score_index to nram_save, and store nram_save to
    // output_data(gdram).
    if (coreId == 0) {
      if (float(max_box[0]) > (float)PNMS_MIN) {
        nram_save[nram_save_count] = (uint32_t)(max_box[1]);
        nram_save_count++;
        output_box_num++;

        if (nram_save_count == nram_save_limit_count) {
          __memcpy(output_data + output_save_count * nram_save_limit_count,
                   nram_save, nram_save_count * sizeof(uint32_t), NRAM2GDRAM);
          output_save_count++;
          nram_save_count = 0;
        }
      }  // if (float(max_box[0]) >= (float)PNMS_MIN)
    }    // if (coreId == 0)

    // if the max score <= 0, end
    if (core_limit == 1) {
      if (float(max_box[0]) <= PNMS_MIN || (loop == max_output_size - 1)) {
        __memcpy(output_data + output_save_count * nram_save_limit_count,
                 nram_save, nram_save_count * sizeof(uint32_t), NRAM2GDRAM);
        break;
      }
    } else {
      if (float(max_box[0]) <= PNMS_MIN || (loop == max_output_size - 1)) {
        if (coreId == 0) {
          __memcpy(output_data + output_save_count * nram_save_limit_count,
                   nram_save, nram_save_count * sizeof(uint32_t), NRAM2GDRAM);
          loop_end_flag[0] = 1;
        }
      }
      __sync_cluster();
      if (loop_end_flag[0] == 1) {
        break;
      }
    }

    // max_score_box -> max_box_pts_x,y,  1*9->9*N
    // max_box: max_score, max_score_index, max_score_box coordinate(x1, y1, x2,
    // y2, x3, y3, x4,
    // y4), sign max_score_box_area, unsign max_score_box_area
    // max_box_pts_x: x1---, x2---, x3---, x4---
    // max_box_pts_y: y1---, y2---, y3---, y4---
    __bang_write_value(max_box_pts_x, max_seg_num * 8, float(0.0));
    int max_box_count = repeat == 0 ? remain_align_num : max_seg_num;

    for (int j = 0; j < 4; j++) {
      __bang_write_value(max_box_pts_x + j * max_seg_num, max_box_count,
                         float(max_box[2 + j * 2]));
      __bang_write_value(max_box_pts_y + j * max_seg_num, max_box_count,
                         float(max_box[3 + j * 2]));
    }

    for (int i = 0; i <= repeat; i++) {
      if (i == repeat && remain_num == 0) {
        break;
      }

      int actual_box_num = 0;
      int align_actual_box_num = 0;

      actual_box_num = (i == repeat) ? remain_num : max_seg_num;
      align_actual_box_num = CEIL_ALIGN(actual_box_num, NFU_ALIGN_SIZE / 4);

      // input_box_ptr: x1---, y1---, x2---, y2---, x3---, y3---, x4---, y4---,
      // scores---
      // box_pts_x: x1---, x2---, x3---, x4---
      // box_pts_y: y1---, y2---, y3---, y4---
      __bang_write_value(box_pts_x, 11 * max_seg_num, (float)0.0);
      __memcpy((IN_DT *)box_pts_x,
               input_box_ptr + input_offset_num + i * max_seg_num,
               align_actual_box_num * INPUT_TYPE_SIZE, GDRAM2NRAM,
               max_seg_num * INPUT_TYPE_SIZE,
               2 * input_stride * INPUT_TYPE_SIZE, 4);

      __memcpy(
          (IN_DT *)box_pts_y,
          input_box_ptr + input_offset_num + i * max_seg_num + input_stride,
          align_actual_box_num * INPUT_TYPE_SIZE, GDRAM2NRAM,
          max_seg_num * INPUT_TYPE_SIZE, 2 * input_stride * INPUT_TYPE_SIZE, 4);

      // scores
      __memcpy(scores, input_score_ptr + input_offset_num + i * max_seg_num,
               align_actual_box_num * INPUT_TYPE_SIZE, GDRAM2NRAM);

      calculateBoxesArea(
          box_pts_x, box_pts_y, box_pts_x + 1 * max_seg_num,
          box_pts_y + 1 * max_seg_num, box_pts_x + 2 * max_seg_num,
          box_pts_y + 2 * max_seg_num, box_pts_x + 3 * max_seg_num,
          box_pts_y + 3 * max_seg_num, box_area, box_area_tmp,
          align_actual_box_num);

      calculateOverlapArea<float>(box_pts_x, box_pts_y, scores, max_box,
                                  max_box_pts_x, max_box_pts_y, box_area,
                                  intersection_area, nram_tmp, max_seg_num,
                                  actual_box_num, align_actual_box_num);
      __bang_active_abs((float *)box_area, (float *)box_area,
                        align_actual_box_num);

      // 4 compare iou with thresh_iou(); iou>thresh_iou, score set pnms_min；
      // area_U = box_area + max_area - area_I
      __bang_add_const((float *)box_area, (float *)box_area, (float)max_box[11],
                       align_actual_box_num);
      __bang_sub((float *)box_area, (float *)box_area,
                 (float *)intersection_area, align_actual_box_num);
      // if (union_area == 0)  iou = (inter_area + 1) / (union_area + 1);
      __bang_write_value(nram_tmp, align_actual_box_num, (float)0.0);
      __bang_eq(nram_tmp, box_area, nram_tmp, align_actual_box_num);
      __bang_add(box_area, box_area, nram_tmp, align_actual_box_num);
      __bang_add(nram_tmp, intersection_area, nram_tmp, align_actual_box_num);

      // compute div  iou = intersection_area
      computeDiv((float *)intersection_area, (float *)nram_tmp,
                 (float *)box_area, nram_tmp + align_actual_box_num,
                 nram_tmp + 2 * align_actual_box_num,
                 nram_tmp + 3 * align_actual_box_num, align_actual_box_num);

      // masked = intersection_area = iou <= thresh_iou
      __bang_write_value(nram_tmp, align_actual_box_num, (float)thresh_iou);
      __bang_le((float *)intersection_area, (float *)intersection_area,
                (float *)nram_tmp, align_actual_box_num);

      // scores = scores * intersection_area; iou>iou_thresh, score set pnms_min
      __bang_mul((float *)scores, (float *)scores, (float *)intersection_area,
                 align_actual_box_num);

      // compare scores with float 0 -> intersection_area
      __bang_write_value(nram_tmp, align_actual_box_num,
                         (float)0.0);
      __bang_eq((float *)intersection_area, (float *)scores, (float *)nram_tmp,
                align_actual_box_num);

      // intersection_area = intersection_area*FLT_MIN  (masked *FLT_MIN )
      __bang_mul_const((float *)intersection_area, (float *)intersection_area,
                       (float)PNMS_MIN, align_actual_box_num);

      // scores  = scores + intersection_area
      __bang_add((float *)scores, (float *)scores, (float *)intersection_area,
                 align_actual_box_num);
      __memcpy((float *)input_score_ptr + input_offset_num + i * max_seg_num,
               (float *)scores, actual_box_num * INPUT_TYPE_SIZE, NRAM2GDRAM);
    }  // for repeat
  }    // for loop : max_output_size

  if (coreId == 0) {
    ((uint32_t *)result_num)[0] = output_box_num;
    __memcpy(buffer, output_data, output_box_num * 4, GDRAM2NRAM);
    quickSort((uint32_t *)buffer, 0, output_box_num - 1);
    __memcpy(output_data, buffer, output_box_num * 4, NRAM2GDRAM);
  }
}
#endif  // KERNELS_NMS_NMS_DETECTION_H_