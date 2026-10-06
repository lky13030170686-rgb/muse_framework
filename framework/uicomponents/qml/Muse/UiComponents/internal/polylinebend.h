/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QPointF>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

//! NOTE: The geometry of one *bent* segment of a polyline plot.
//!
//! A segment is bent by a single point, exactly the way an automation curve is: the segment is split
//! into two quadratic Bezier arcs that meet at the bend and share a tangent there (see
//! `muse::mpe::AutomationPoint::evaluateAt`, which is what a synthesiser plays). Keeping the drawing
//! and the playback of that shape in *one* formula is the whole point: a straight chord through the
//! same endpoints looks perfectly plausible and sounds wrong, and "the line I see is not the line I
//! hear" is the hardest kind of bug to notice.
//!
//! Coordinates are the caller's: normalized [0, 1] plot values or plot pixels, as long as the same
//! space is used for all three inputs. The formula is affine in x and in y, so both work.
namespace muse::uicomponents::polyline {
struct BendGeometry {
    //! where the two arcs meet - also where a drag handle for this bend belongs
    QPointF bend;
    //! control point of the arc from `from` to `bend`
    QPointF control1;
    //! control point of the arc from `bend` to `to`
    QPointF control2;
};

inline QPointF quadraticBezierAt(qreal s, const QPointF& p0, const QPointF& p1, const QPointF& p2)
{
    const qreal u = 1.0 - s;
    return QPointF(u * u * p0.x() + 2.0 * u * s * p1.x() + s * s * p2.x(),
                   u * u * p0.y() + 2.0 * u * s * p1.y() + s * s * p2.y());
}

//! The two arcs of the segment `from` -> `to` bent at `bend`.
//!
//! A bend that cannot bend this segment (a segment with no horizontal extent, or a bend sitting at
//! or beyond either end - which `evaluateAt` treats as "no bend" and plays as a straight line)
//! collapses onto the chord: every returned point, including `bend`, is then on the straight line.
//! Callers therefore never need a "straight or curved" branch of their own.
inline BendGeometry bendGeometry(const QPointF& from, const QPointF& to, const QPointF& bend)
{
    constexpr qreal EPS = 1e-12;

    const QPointF chordMid((from.x() + to.x()) * 0.5, (from.y() + to.y()) * 0.5);

    const qreal dx = to.x() - from.x();
    if (std::abs(dx) <= EPS) {
        return { chordMid, chordMid, chordMid };
    }

    const qreal t = (bend.x() - from.x()) / dx;
    if (t <= EPS || t >= 1.0 - EPS) {
        return { chordMid, chordMid, chordMid };
    }

    // The value axis is y, and `evaluateAt` clamps each control point into the segment's own value
    // range - with y increasing downwards the bounds simply swap, which min/max already handles.
    const qreal halfSlope = 0.5 * (to.y() - from.y());
    const qreal lo = std::min(from.y(), to.y());
    const qreal hi = std::max(from.y(), to.y());

    const qreal control1Y = std::clamp(bend.y() - t * halfSlope, lo, hi);
    const qreal control2Y = std::clamp(bend.y() + (1.0 - t) * halfSlope, lo, hi);

    // The arcs are parameterized by the *value* Bezier, so x has to stay linear in that parameter:
    // a quadratic Bezier is linear in its parameter exactly when its control point sits halfway
    // between the endpoints. That is what makes the drawn shape the played shape and not merely
    // something that passes through the same bend.
    return {
        bend,
        QPointF((from.x() + bend.x()) * 0.5, control1Y),
        QPointF((bend.x() + to.x()) * 0.5, control2Y),
    };
}

//! The point at parameter `s` in [0, 1] across the bent segment.
inline QPointF bendPointAt(const QPointF& from, const QPointF& to, const QPointF& bend, qreal s)
{
    const BendGeometry g = bendGeometry(from, to, bend);

    const qreal dx = to.x() - from.x();
    const qreal bendS = std::abs(dx) <= 1e-12 ? 0.5 : std::clamp((g.bend.x() - from.x()) / dx, 0.0, 1.0);
    if (bendS <= 1e-12 || bendS >= 1.0 - 1e-12) {
        return quadraticBezierAt(std::clamp(s, 0.0, 1.0), from, g.control1, to);
    }

    const qreal t = std::clamp(s, 0.0, 1.0);
    if (t <= bendS) {
        return quadraticBezierAt(t / bendS, from, g.control1, g.bend);
    }

    return quadraticBezierAt((t - bendS) / (1.0 - bendS), g.bend, g.control2, to);
}

//! Samples the bent segment into a polygon, for callers that need points rather than a curve
//! (hit-testing, distance-to-line tests). `stepsPerArc` >= 1.
inline QVector<QPointF> sampleBend(const QPointF& from, const QPointF& to, const QPointF& bend, int stepsPerArc)
{
    const int steps = std::max(1, stepsPerArc);
    const BendGeometry g = bendGeometry(from, to, bend);

    QVector<QPointF> out;
    out.reserve(2 * steps + 1);

    for (int i = 0; i <= steps; ++i) {
        out.push_back(quadraticBezierAt(qreal(i) / steps, from, g.control1, g.bend));
    }
    for (int i = 1; i <= steps; ++i) {
        out.push_back(quadraticBezierAt(qreal(i) / steps, g.bend, g.control2, to));
    }

    return out;
}
}
