#pragma once

#include "expression_types.h"

#include <cstdint>

namespace vocat {

/** Number of ExpressionState values (Neutral … Speaking). */
constexpr uint8_t kExpressionStateCount = 21;

ExpressionState ExpressionStateFromIndex(uint8_t index);
uint8_t ExpressionStateToIndex(ExpressionState state);

/** Scheme §十: 360 logical → device px (Q10 fixed-point). */
class ExpressionScale {
public:
    void Configure(uint16_t width, uint16_t height);
    int32_t Scale(int32_t v) const { return (v * q10_) / 1024; }
    int32_t Q10() const { return q10_; }

private:
    int32_t q10_ = 1024;
};

const ExpressionScale& DefaultExpressionScale();
ExpressionScale& MutableDefaultExpressionScale();

}  // namespace vocat
