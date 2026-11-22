#pragma once

#include <bit>
#include <hdf5.h>

namespace anndatapp_h5ad_detail {

inline constexpr auto native_order = std::endian::native == std::endian::little ? H5T_ORDER_LE : H5T_ORDER_BE;

inline bool is_binary16(hid_t type) {
    const auto order = H5Tget_order(type);
    size_t sign, exponent, exponent_bits, mantissa, mantissa_bits;
    return (order == H5T_ORDER_LE || order == H5T_ORDER_BE) &&
        H5Tget_precision(type) == 16 && H5Tget_offset(type) == 0 &&
        H5Tget_fields(type, &sign, &exponent, &exponent_bits, &mantissa, &mantissa_bits) >= 0 &&
        sign == 15 && exponent == 10 && exponent_bits == 5 && mantissa == 0 && mantissa_bits == 10 &&
        H5Tget_ebias(type) == 15 && H5Tget_norm(type) == H5T_NORM_IMPLIED;
}

// Construct the file type using the floating datatype API available in HDF5 1.10.
inline hid_t binary16_file_type(H5T_order_t order) {
    const auto type = H5Tcopy(H5T_IEEE_F32LE);
    if (type < 0) return type;
    if (H5Tset_fields(type, 15, 10, 5, 0, 10) < 0 ||
        H5Tset_ebias(type, 15) < 0 || H5Tset_precision(type, 16) < 0 ||
        H5Tset_size(type, 2) < 0 || H5Tset_order(type, order) < 0) {
        H5Tclose(type);
        return -1;
    }
    return type;
}

// Keep all datatype properties except byte order so HDF5 preserves the payload bits.
inline hid_t binary16_native_type(hid_t file_type) {
    const auto type = H5Tcopy(file_type);
    if (type < 0) return type;
    if (H5Tset_order(type, native_order) < 0) {
        H5Tclose(type);
        return -1;
    }
    return type;
}

} // namespace anndatapp_h5ad_detail
