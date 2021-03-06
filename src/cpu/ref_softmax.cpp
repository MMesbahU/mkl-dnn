/*******************************************************************************
* Copyright 2016-2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <assert.h>
#include <float.h>
#include <math.h>

#include "c_types_map.hpp"
#include "type_helpers.hpp"
#include "mkldnn_thread.hpp"

#include "ref_softmax.hpp"
#include "gemm/os_blas.hpp"

#ifdef USE_MKL
#include "mkl_vml_functions.h"
#endif

namespace mkldnn {
namespace impl {
namespace cpu {

template <impl::data_type_t data_type>
void ref_softmax_fwd_t<data_type>::execute_forward_dense() const {
    auto src = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto dst = reinterpret_cast<data_t *>(this->memory(0));

    parallel_nd(outer_size_, [&](int ou) {
        const data_t *src_data = src + ou * channels_;
        data_t *dst_data = dst + ou * channels_;
        data_t scalar = 0;

        _max(channels_, src_data, &scalar);
        _sub(channels_, scalar, src_data, dst_data);
        _exp(channels_, dst_data, dst_data);
        _sum(channels_, dst_data, &scalar);
        _scal(channels_, data_t(1)/scalar, dst_data);
    });
}

template <impl::data_type_t data_type>
void ref_softmax_fwd_t<data_type>::execute_forward_generic() const {
    auto src = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto dst = reinterpret_cast<data_t *>(this->memory(0));

    data_t space_max_val = 0, space_denom_val = 0;
    data_t *space_max = &space_max_val, *space_denom = &space_denom_val;
    if (inner_size_ > 1) {
        using namespace memory_tracking::names;
        space_max = scratchpad().template get<data_t>(key_softmax_reduction);
        space_denom = space_max + inner_size_;
    }

    const memory_desc_wrapper data_d(pd()->src_pd());
    const size_t dim = channels_ * inner_size_;

    for (int ou = 0; ou < outer_size_; ou++) {
        utils::array_set(space_max, -FLT_MAX, inner_size_);
        utils::array_set(space_denom, 0, inner_size_);

        for (int c = 0; c < channels_; c++) {
            for(int in = 0; in < inner_size_; in++) {
                size_t off = data_d.off_l(ou * dim + c * inner_size_ + in);
                space_max[in] = nstl::max(space_max[in], src[off]);
            }
        }

        for (int c = 0; c < channels_; c++) {
            for(int in = 0; in < inner_size_; in++) {
                size_t off = data_d.off_l(ou * dim + c * inner_size_ + in);
                space_denom[in] += dst[off] = exp(src[off] - space_max[in]);
            }
        }

        for (int c = 0; c < channels_; c++) {
            for (int in = 0; in < inner_size_; in++) {
                size_t off = data_d.off_l(ou * dim + c * inner_size_ + in);
                dst[off] /= space_denom[in];
            }
        }
    }
}

template <impl::data_type_t data_type>
void ref_softmax_fwd_t<data_type>::_max(int n, const data_t *x,
        data_t *max_data) const {
#ifdef USE_CBLAS
    if (data_type == data_type::f32) {
        max_data[0] = x[cblas_isamax(n, x, 1)];
        return;
    }
#endif
    max_data[0] = x[0];
    for (int c = 1; c < n; ++c)
        max_data[0] = nstl::max(max_data[0], x[c]);
}

template <impl::data_type_t data_type>
void ref_softmax_fwd_t<data_type>::_sub(int n, data_t alpha, const data_t *x,
        data_t *y) const {
    constexpr int unroll_factor = 32;
    int tail = n % unroll_factor;
    for (int i = 0; i < n - tail; i += unroll_factor) {
        PRAGMA_OMP_SIMD()
        for (int j = 0; j < unroll_factor; j++) {
            y[i + j] = x[i + j] - alpha;
        }
    }
    PRAGMA_OMP_SIMD()
    for (int i = n - tail; i < n; i++) {
        y[i] = x[i] - alpha;
    }
}

template <impl::data_type_t data_type>
void ref_softmax_fwd_t<data_type>::_exp(int n, const data_t *a,
        data_t *r) const {
#ifdef USE_MKL
    if (data_type == data_type::f32) {
        vsExp(n, a, r);
        return;
    }
#endif
    parallel_nd(n, [&](int c) { r[c] = expf(a[c]); });
}

template <impl::data_type_t data_type>
void ref_softmax_fwd_t<data_type>::_sum(int n, const data_t *x,
        data_t *sum_data) const {
#ifdef USE_CBLAS
    // Here we are summing x's eg. e^z , which are positives
    // so we can use BLAS ASUM
    if (data_type == data_type::f32) {
        sum_data[0] = cblas_sasum(n, x, 1);
        return;
    }
#endif
    data_t tsum = static_cast<data_t>(0);
    PRAGMA_OMP_SIMD(reduction(+ : tsum))
    for (int c = 0; c < n; ++c)
        tsum += x[c];
    sum_data[0] = tsum;
}

template <impl::data_type_t data_type>
void ref_softmax_fwd_t<data_type>::_scal(int n, data_t alpha, data_t *x) const {
#ifdef USE_CBLAS
    if (data_type == data_type::f32) {
        cblas_sscal(n, alpha, x, 1);
        return;
    }
#endif
    parallel_nd(n, [&](int c) { x[c] *= alpha; });
}

template struct ref_softmax_fwd_t<data_type::f32>;


// NC/NCHW softmax for along final axe (1 for NC, 3 for NCHW)
template <impl::data_type_t data_type>
void ref_softmax_bwd_t<data_type>::execute_backward_dense() const {
    auto data = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto diff_dst = reinterpret_cast<const data_t *>(this->input_memory(1));
    auto diff_src = reinterpret_cast<data_t *>(this->memory(0));

    parallel_nd(outer_size_, [&](int ou) {
        data_t sbr = 0;
        size_t off = channels_*ou;
        for (int c = 0; c < channels_; c++) {
            size_t loff = off + c;
            data_t ldata = data[loff];
            sbr += diff_dst[loff]*ldata;
            diff_src[loff] = ldata;
        }

        for(int c=0; c < channels_ ; ++c) {
          size_t loff = off + c;
          diff_src[loff] *= (diff_dst[loff] - sbr);
        }
    });
}

template <impl::data_type_t data_type>
void ref_softmax_bwd_t<data_type>::execute_backward_generic() const {
    const size_t dim = channels_ * inner_size_;
    auto data = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto diff_dst = reinterpret_cast<const data_t *>(this->input_memory(1));
    auto diff_src = reinterpret_cast<data_t *>(this->memory(0));
    const memory_desc_wrapper diff_d(pd()->diff_src_pd());
    const memory_desc_wrapper data_d(pd()->dst_pd());

    parallel_nd(outer_size_, [&](int ou) {
        for (int in = 0; in < inner_size_; in++) {
            data_t sbr = 0;
            for (int c = 0; c < channels_; c++) {
                size_t off_diff = diff_d.off_l(ou * dim + c * inner_size_ + in);
                size_t off_data = diff_d.off_l(ou * dim + c * inner_size_ + in);
                sbr += diff_dst[off_diff]*data[off_data];
            }

            for(int c=0; c < channels_ ; ++c) {
              size_t off_diff = diff_d.off_l(ou * dim + c * inner_size_ + in);
              size_t off_data = data_d.off_l(ou * dim + c * inner_size_ + in);
              diff_src[off_diff] = data[off_data]*(diff_dst[off_diff] - sbr);
            }
        }
    });
}

template struct ref_softmax_bwd_t<data_type::f32>;

}
}
}

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
