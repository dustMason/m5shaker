#include "analog_dsp.hpp"

namespace shaker::analog {
const float tanh_table[513] = {
#include "five_tanh.inc"
};
} // namespace shaker::analog
