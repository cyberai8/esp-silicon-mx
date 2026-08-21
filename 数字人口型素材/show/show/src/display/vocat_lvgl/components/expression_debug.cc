#include "expression_debug.h"

namespace vocat {
namespace {

ExpressionScale g_scale;

}  // namespace

ExpressionState ExpressionStateFromIndex(uint8_t index)
{
    if (index >= kExpressionStateCount) {
        index = 0;
    }
    return static_cast<ExpressionState>(index);
}

uint8_t ExpressionStateToIndex(ExpressionState state)
{
    return static_cast<uint8_t>(state);
}

void ExpressionScale::Configure(uint16_t width, uint16_t height)
{
    const uint16_t m = width < height ? width : height;
    q10_ = (static_cast<int32_t>(m) * 1024) / 360;
    if (q10_ < 1) {
        q10_ = 1;
    }
}

ExpressionScale& MutableDefaultExpressionScale()
{
    return g_scale;
}

const ExpressionScale& DefaultExpressionScale()
{
    return g_scale;
}

}  // namespace vocat
