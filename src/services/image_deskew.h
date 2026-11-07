#pragma once

#include <QImage>

// Skew correction for OCR input. A page tilted by a few degrees makes the engine
// merge text lines across columns and eventually stop reading the page at all,
// so the angle is measured and the image is rotated back before recognition.

// Returns the rotation in degrees that straightens the image, or 0 when the
// content is too sparse to measure. Positive values rotate clockwise.
double estimateSkewAngle(const QImage& image);

// Rotates the image by the given angle on a white canvas large enough to keep
// the corners. Returns the source image unchanged for negligible angles.
QImage deskewImage(const QImage& image, double degrees);

// Same rotation, but the canvas keeps the original size. Used when the result is
// both the OCR input and the page background, so word boxes stay aligned with
// the bitmap that is drawn back into the document.
QImage deskewImageCropped(const QImage& image, double degrees);
