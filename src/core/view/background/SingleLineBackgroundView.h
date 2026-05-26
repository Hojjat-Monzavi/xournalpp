/*
 * Xournal++
 *
 * Class for single horizontal line backgrounds
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cairo.h>  // for cairo_t

#include "util/Color.h"  // for Color

#include "OneColorBackgroundView.h"  // for OneColorBackgroundView

class BackgroundConfig;

namespace xoj::view {
class SingleLineBackgroundView: public OneColorBackgroundView {
public:
    SingleLineBackgroundView(double pageWidth, double pageHeight, Color backgroundColor, const BackgroundConfig& config);
    virtual ~SingleLineBackgroundView() = default;

    virtual void draw(cairo_t* cr) const override;

protected:
    double linePosition = -1;  // Y position of the line from config (using CFG_RASTER), negative means use default (middle of page)

    constexpr static Color DEFAULT_LINE_COLOR = Colors::xopp_dodgerblue;
    constexpr static Color ALT_DEFAULT_LINE_COLOR = Colors::xopp_darkslategray;
    constexpr static double DEFAULT_LINE_WIDTH = 0.5;
};
};  // namespace xoj::view