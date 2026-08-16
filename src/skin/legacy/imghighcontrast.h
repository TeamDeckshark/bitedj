#pragma once

#include <QImage>

#include "skin/highcontrast.h"
#include "skin/legacy/imgsource.h"

/// Bite DJ: the daylight-mode filter in the skin's image/colour pipeline.
///
/// `ColorSchemeParser` installs this on `WPixmapStore`, `WImageStore` and
/// `WSkinColor` while the mode is on, which puts one hook in front of both
/// halves of the skin that a stylesheet cannot reach:
///   - every colour read out of skin XML, because they all land in
///     `WSkinColor::getCorrectColor()` — the waveform's background, signal,
///     beat and mark colours, `WOverview`, `WLabel`, knob and slider colours;
///   - every image the skin loads, i.e. knob.svg and the effect meters, which
///     are named from XML rather than from QSS and so are invisible to the
///     stylesheet transform.
///
/// Distinct from the stock `ImgInvert` colour-scheme filter, which inverts RGB
/// outright. This flips HSL lightness only, so a dark waveform on a light page
/// keeps its signal hues and the state colours keep mirroring the LEDs.
class ImgHighContrast : public ImgColorProcessor {
  public:
    inline ImgHighContrast(ImgSource* parent)
            : ImgColorProcessor(parent) {
    }

    QColor doColorCorrection(const QColor& c) const override {
        return HighContrast::invertColor(c);
    }

    void correctImageColors(QImage* p) const override {
        if (p == nullptr || p->isNull()) {
            return;
        }
        // ImgColorProcessor reads each pixel straight into a QColor, so a
        // premultiplied source would have its channels inverted while still
        // scaled by alpha — which smears the antialiased edges of an icon into
        // a halo. Convert to straight alpha first; the base class skips
        // anything narrower than 32bpp anyway.
        if (p->format() != QImage::Format_ARGB32) {
            *p = p->convertToFormat(QImage::Format_ARGB32);
        }
        ImgColorProcessor::correctImageColors(p);
    }
};
