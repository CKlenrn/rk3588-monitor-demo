#ifndef VIDEOTYPES_H
#define VIDEOTYPES_H

#include <QString>
#include <QMetaType>

struct VideoFormat
{
    int width = 0;
    int height = 0;
    int fpsNumerator = 0;
    int fpsDenominator = 1;
    QString gstFormat;
    QString driverFormat;
    QString colorimetry;

    bool isValid() const
    {
        return width > 0 && height > 0 && fpsNumerator > 0
                && fpsDenominator > 0 && !gstFormat.isEmpty();
    }

    bool operator==(const VideoFormat &other) const
    {
        return width == other.width
                && height == other.height
                && fpsNumerator == other.fpsNumerator
                && fpsDenominator == other.fpsDenominator
                && gstFormat == other.gstFormat
                && colorimetry == other.colorimetry;
    }

    bool operator!=(const VideoFormat &other) const
    {
        return !(*this == other);
    }
};

Q_DECLARE_METATYPE(VideoFormat)

#endif
