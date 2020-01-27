#include <torch/script.h>

#include "compat.h"

#define CHECK_CPU(x) AT_ASSERTM(!x.type().is_cuda(), #x " must be CPU tensor")

enum ReductionType { SUM, MEAN, MIN, MAX };

const std::map<std::string, ReductionType> reduce2REDUCE = {
    {"sum", SUM}, {"add", SUM}, {"mean", MEAN}, {"min", MIN}, {"max", MAX},
};

#define AT_DISPATCH_REDUCTION_TYPES(reduce, ...)                               \
  [&] {                                                                        \
    switch (reduce2REDUCE.at(reduce)) {                                        \
    case SUM: {                                                                \
      const ReductionType REDUCE = SUM;                                        \
      return __VA_ARGS__();                                                    \
    }                                                                          \
    case MEAN: {                                                               \
      const ReductionType REDUCE = MEAN;                                       \
      return __VA_ARGS__();                                                    \
    }                                                                          \
    case MIN: {                                                                \
      const ReductionType REDUCE = MIN;                                        \
      return __VA_ARGS__();                                                    \
    }                                                                          \
    case MAX: {                                                                \
      const ReductionType REDUCE = MAX;                                        \
      return __VA_ARGS__();                                                    \
    }                                                                          \
    }                                                                          \
  }()

#define AT_DISPATCH_HAS_VAL(value_opt, ...)                                    \
  [&] {                                                                        \
    switch (value_opt.has_value()) {                                           \
    case true: {                                                               \
      const bool HAS_VAL = true;                                               \
      return __VA_ARGS__();                                                    \
    }                                                                          \
    case false: {                                                              \
      const bool HAS_VAL = false;                                              \
      return __VA_ARGS__();                                                    \
    }                                                                          \
    }                                                                          \
  }()

template <typename scalar_t, ReductionType REDUCE> struct Reducer {
  static inline scalar_t init() {
    if (REDUCE == MIN) {
      return std::numeric_limits<scalar_t>::max();
    } else if (REDUCE == MAX) {
      return std::numeric_limits<scalar_t>::lowest();
    } else {
      return (scalar_t)0;
    }
  }

  static inline void update(scalar_t *val, scalar_t new_val, int64_t *arg,
                            int64_t new_arg) {
    if (REDUCE == SUM || REDUCE == MEAN) {
      *val = *val + new_val;
    } else if ((REDUCE == MIN && new_val < *val) ||
               (REDUCE == MAX && new_val > *val)) {
      *val = new_val;
      *arg = new_arg;
    }
  }

  static inline void write(scalar_t *address, scalar_t val,
                           int64_t *arg_address, int64_t arg, int count) {
    if (REDUCE == SUM) {
      *address = val;
    } else if (REDUCE == MEAN) {
      *address = val / (count > 0 ? count : (scalar_t)1);
    } else if (REDUCE == MIN || REDUCE == MAX) {
      if (count > 0) {
        *address = val;
        *arg_address = arg;
      } else {
        *address = (scalar_t)0;
      }
    }
  }
};

std::tuple<torch::Tensor, torch::optional<torch::Tensor>>
spmm(torch::Tensor rowptr, torch::Tensor col,
     torch::optional<torch::Tensor> value_opt, torch::Tensor mat,
     std::string reduce) {

  CHECK_CPU(rowptr);
  CHECK_CPU(col);
  if (value_opt.has_value())
    CHECK_CPU(value_opt.value());
  CHECK_CPU(mat);

  AT_ASSERTM(rowptr.dim() == 1, "Input mismatch");
  AT_ASSERTM(col.dim() == 1, "Input mismatch");
  if (value_opt.has_value())
    AT_ASSERTM(value_opt.value().dim() == 1);
  AT_ASSERTM(mat.dim() >= 2, "Input mismatch");

  mat = mat.contiguous();

  auto sizes = mat.sizes().vec();
  sizes[mat.dim() - 2] = rowptr.numel() - 1;
  auto out = torch::empty(sizes, mat.options());

  torch::optional<torch::Tensor> arg_out = torch::nullopt;
  int64_t *arg_out_data = nullptr;
  if (reduce2REDUCE.at(reduce) == MIN || reduce2REDUCE.at(reduce) == MAX) {
    arg_out = torch::full_like(out, col.numel(), rowptr.options());
    arg_out_data = arg_out.value().DATA_PTR<int64_t>();
  }

  auto rowptr_data = rowptr.DATA_PTR<int64_t>();
  auto col_data = col.DATA_PTR<int64_t>();

  auto M = rowptr.numel() - 1;
  auto N = mat.size(-2);
  auto K = mat.size(-1);
  auto B = mat.numel() / (N * K);

  AT_DISPATCH_ALL_TYPES(mat.scalar_type(), "spmm", [&] {
    scalar_t *value_data = nullptr;
    auto mat_data = mat.DATA_PTR<scalar_t>();
    auto out_data = out.DATA_PTR<scalar_t>();

    scalar_t val;
    std::vector<scalar_t> vals(K);
    int64_t row_start, row_end, c;
    std::vector<int64_t> args(K);

    AT_DISPATCH_REDUCTION_TYPES(reduce, [&] {
      AT_DISPATCH_HAS_VAL(value_opt, [&] {
        if (HAS_VAL) {
          value_data = value_opt.value().DATA_PTR<scalar_t>();
        }

        for (int b = 0; b < B; b++) {
          for (int m = 0; m < M; m++) {
            row_start = rowptr_data[m], row_end = rowptr_data[m + 1];

            for (int k = 0; k < K; k++)
              vals[k] = Reducer<scalar_t, REDUCE>::init();

            int offset = b * N * K;
            for (int e = row_start; e < row_end; e++) {
              c = col_data[e];
              if (HAS_VAL)
                val = value_data[e];
              for (int k = 0; k < K; k++) {
                if (HAS_VAL)
                  Reducer<scalar_t, REDUCE>::update(
                      &vals[k], val * mat_data[offset + c * K + k], &args[k],
                      e);
                else
                  Reducer<scalar_t, REDUCE>::update(
                      &vals[k], mat_data[offset + c * K + k], &args[k], e);
              }
            }
            offset = b * M * K + m * K;
            for (int k = 0; k < K; k++)
              Reducer<scalar_t, REDUCE>::write(out_data + offset + k, vals[k],
                                               arg_out_data + offset + k,
                                               args[k], row_end - row_start);
          }
        }
      });
    });
  });

  return std::make_tuple(out, arg_out);
}

torch::Tensor spmm_val_bw(torch::Tensor row, torch::Tensor rowptr,
                          torch::Tensor col, torch::Tensor mat,
                          torch::Tensor grad, std::string reduce) {
  CHECK_CPU(row);
  CHECK_CPU(rowptr);
  CHECK_CPU(col);
  CHECK_CPU(mat);
  CHECK_CPU(grad);

  mat = mat.contiguous();
  grad = grad.contiguous();

  auto M = grad.size(-2);
  auto N = mat.size(-2);
  auto E = row.numel();
  auto K = mat.size(-1);
  auto B = mat.numel() / (N * K);

  auto out = torch::zeros(row.numel(), grad.options());

  auto row_data = row.DATA_PTR<int64_t>();
  auto rowptr_data = rowptr.DATA_PTR<int64_t>();
  auto col_data = col.DATA_PTR<int64_t>();
  AT_DISPATCH_ALL_TYPES(mat.scalar_type(), "spmm_val_bw", [&] {
    auto mat_data = mat.DATA_PTR<scalar_t>();
    auto grad_data = grad.DATA_PTR<scalar_t>();
    auto out_data = out.DATA_PTR<scalar_t>();

    scalar_t val;
    int64_t row, col;
    AT_DISPATCH_REDUCTION_TYPES(reduce, [&] {
      for (int b = 0; b < B; b++) {
        for (int e = 0; e < E; e++) {
          row = row_data[e], col = col_data[e], val = (scalar_t)0;
          for (int k = 0; k < K; k++) {
            val += mat_data[b * N * K + col * K + k] *
                   grad_data[b * M * K + row * K + k];
          }
          if (REDUCE == MEAN) {
            int row_start = rowptr_data[row], row_end = rowptr_data[row + 1];
            val /= (scalar_t)std::max(row_end - row_start, 1);
          }
          out_data[e] += val;
        }
      }
    });
  });

  return out;
}

static auto registry = torch::RegisterOperators("torch_sparse_cpu::spmm", &spmm)
                           .op("torch_sparse_cpu::spmm_val_bw", &spmm_val_bw);
