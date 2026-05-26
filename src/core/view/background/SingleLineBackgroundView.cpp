#include "SingleLineBackgroundView.h"

#include <memory>  // for allocator

#include "model/BackgroundConfig.h"                  // for BackgroundConfig
#include "view/background/BackgroundView.h"          // for view
#include "view/background/OneColorBackgroundView.h"  // for OneColorBackgroundView
#include "view/background/PlainBackgroundView.h"     // for PlainBackgroundView

using namespace background_config_strings;
using namespace xoj::view;

SingleLineBackgroundView::SingleLineBackgroundView(double pageWidth, double pageHeight, Color backgroundColor,
                                         const BackgroundConfig& config):
        OneColorBackgroundView(pageWidth, pageHeight, backgroundColor, config, DEFAULT_LINE_WIDTH, DEFAULT_LINE_COLOR,
                               ALT_DEFAULT_LINE_COLOR) {
    config.loadValue(CFG_RASTER, linePosition);
}

void SingleLineBackgroundView::draw(cairo_t* cr) const {
    // Paint the background color
    PlainBackgroundView::draw(cr);

    // Draw a single horizontal line at the configured position
    double yPos = (linePosition >= 0) ? linePosition : (pageHeight / 2.0);  // Default to middle if no config value
    cairo_move_to(cr, 0, yPos);
    cairo_line_to(cr, pageWidth, yPos);

    cairo_save(cr);
    Util::cairo_set_source_rgbi(cr, foregroundColor);
    cairo_set_line_width(cr, lineWidth);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_stroke(cr);
    cairo_restore(cr);
}