#include "UiTimeline.h"
#include "app/UiKit.h"
#include "app/Icons.h"
#include "core/I18n.h"
#include "imgui_internal.h"
#include "ui_probe/UiProbe.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace mmdx::studio {

namespace {
using namespace ui;

// The graph lane's speed sub-lane reports units per second: MMD plays at 30 frames per second (mmdx::kMmdFps). Named
// here so the data-agnostic widget does not depend on the animation headers.
constexpr float kGraphLaneFps = 30.0f;

// A curve handle's position: the normalised curve point (x, y) 0..127 inside the segment's handle box, whose top-left
// corner is the previous key (xa, ya) and whose far vertical edge is `farY` (>= ya: the box going up from ya).
ImVec2 GraphHandlePoint(float xa, float xb, float ya, float farY, uint8_t x, uint8_t y) {
    return ImVec2(xa + (xb - xa) * ((float)x / 127.0f), ya + (farY - ya) * ((float)y / 127.0f));
}

int GetMinSelectedFrame(const std::vector<TimelineRow>& rows) {
    int minFrame = 2147483647;
    for (const auto& r : rows) {
        if (!r.keysEditable) continue;
        for (const auto& k : r.keys) {
            if (k.selected && k.frame < minFrame) {
                minFrame = k.frame;
            }
        }
    }
    return minFrame;
}
} // namespace

bool Timeline(const char* id, ImVec2 size, const std::vector<TimelineRow>& rows, int currentFrame, int maxFrame,
              TimelineView& view, TimelineEvents& ev) {
    ev = TimelineEvents{};
    bool changed = false;

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    const ImGuiID imguiId = window->GetID(id);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (size.x <= 0.0f) size.x = avail.x;
    if (size.y <= 0.0f) size.y = avail.y;

    const ImRect bb(window->DC.CursorPos, ImVec2(window->DC.CursorPos.x + size.x, window->DC.CursorPos.y + size.y));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, imguiId)) return false;

    const Palette& p = P();
    ImDrawList* dl = window->DrawList;
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mousePos = io.MousePos;

    float colWidth = Dp(220.0f);
    float rowHeight = Dp(24.0f);
    float rulerHeight = Dp(28.0f);
    const float sbH = Dp(10.0f);

    float keyAreaX = bb.Min.x + colWidth;
    float keyAreaW = std::max(0.0f, bb.Max.x - keyAreaX);
    const float originX = keyAreaX + Dp(12.0f);  // x of frame `scrollFrame` (inset keeps frame-0 keys whole)
    float contentY = bb.Min.y + rulerHeight;
    float contentH = std::max(0.0f, bb.Max.y - sbH - contentY);

    float visibleFrames = (keyAreaW - Dp(12.0f)) / (view.pxPerFrame * Dpi());
    float maxScrollFrame = std::max(0.0f, (float)(maxFrame + 120) - visibleFrames);
    float contentHeightAll = rows.size() * rowHeight;
    float maxScrollY = std::max(0.0f, contentHeightAll - contentH);

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, imguiId, &hovered, &held, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight |
                                                                     ImGuiButtonFlags_PressedOnClick);
    // The wheel scrolls/zooms the timeline, not the host window.
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);

    // Middle pan
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(2, 0.0f)) {
        view.scrollFrame -= io.MouseDelta.x / (view.pxPerFrame * Dpi());
        view.scrollY -= io.MouseDelta.y;
    }

    // Wheel
    if (hovered && view.graphDrag == 0 && io.MouseWheel != 0.0f) {
        if (mousePos.x > keyAreaX) {
            if (io.KeyCtrl || mousePos.y < contentY) {
                float oldPx = view.pxPerFrame;
                view.pxPerFrame = std::clamp(view.pxPerFrame * (io.MouseWheel > 0 ? 1.15f : (1.0f / 1.15f)), 0.2f, 40.0f);
                float mouseFrame = view.scrollFrame + (mousePos.x - originX) / (oldPx * Dpi());
                view.scrollFrame = mouseFrame - (mousePos.x - originX) / (view.pxPerFrame * Dpi());
            } else if (io.KeyShift) {
                view.scrollFrame -= io.MouseWheel * 20.0f * (6.0f / view.pxPerFrame);
            } else {
                view.scrollY -= io.MouseWheel * rowHeight;
            }
        } else {
            view.scrollY -= io.MouseWheel * rowHeight;
        }
    }

    // The key of row `rowIndex` under the mouse (7 px pick radius), or nullptr.
    const auto keyUnderMouse = [&](int rowIndex) -> const TimelineKey* {
        const TimelineRow& r = rows[rowIndex];
        const float rowY = contentY - view.scrollY + rowIndex * rowHeight + rowHeight * 0.5f;
        float bestDistSq = Dp(7.0f) * Dp(7.0f);
        const TimelineKey* bestKey = nullptr;
        const float f0 = view.scrollFrame + (mousePos.x - Dp(7.0f) - originX) / (view.pxPerFrame * Dpi());
        const float f1 = view.scrollFrame + (mousePos.x + Dp(7.0f) - originX) / (view.pxPerFrame * Dpi());
        auto it = std::lower_bound(r.keys.begin(), r.keys.end(), (int)std::floor(f0),
                                   [](const TimelineKey& a, int b) { return a.frame < b; });
        for (; it != r.keys.end() && it->frame <= std::ceil(f1); ++it) {
            const float kx = originX + (it->frame - view.scrollFrame) * view.pxPerFrame * Dpi();
            const float dx = mousePos.x - kx, dy = mousePos.y - rowY;
            const float distSq = dx * dx + dy * dy;
            if (distSq <= bestDistSq) {
                bestDistSq = distSq;
                bestKey = &(*it);
            }
        }
        return bestKey;
    };

    struct RowControlRects {
        ImRect sw{0, 0, 0, 0};
        ImRect prev{0, 0, 0, 0};
        ImRect keyHere{0, 0, 0, 0};
        ImRect next{0, 0, 0, 0};
        ImRect graph{0, 0, 0, 0};
        bool hasStopwatch = false;
        bool animated = false;
        bool hasPrev = false;
        bool hasNext = false;
        bool keyedHere = false;
        bool hasGraph = false;
        float leftBound = 0.0f; // left edge of all controls
    };

    const auto controlRects = [&](int rowIndex) -> RowControlRects {
        RowControlRects res;
        if (rowIndex < 0 || rowIndex >= (int)rows.size()) return res;
        const auto& r = rows[rowIndex];

        const float rowY = contentY - view.scrollY + rowIndex * rowHeight;
        const float swSize = Dp(18.0f);
        const float navSize = Dp(16.0f);
        float curRight = keyAreaX - Dp(2.0f);

        if (r.hasStopwatch && !r.isGroup) {
            res.hasStopwatch = true;
            res.animated = r.animated;

            res.sw = ImRect(ImVec2(curRight - swSize, rowY + (rowHeight - swSize) * 0.5f),
                            ImVec2(curRight, rowY + (rowHeight + swSize) * 0.5f));
            curRight -= swSize + Dp(2.0f);

            if (r.animated) {
                for (const auto& k : r.keys) {
                    if (k.frame < currentFrame) res.hasPrev = true;
                    if (k.frame == currentFrame) res.keyedHere = true;
                    if (k.frame > currentFrame) res.hasNext = true;
                }
                res.next = ImRect(ImVec2(curRight - navSize, rowY + (rowHeight - navSize) * 0.5f),
                                  ImVec2(curRight, rowY + (rowHeight + navSize) * 0.5f));
                curRight -= navSize;
                res.keyHere = ImRect(ImVec2(curRight - navSize, rowY + (rowHeight - navSize) * 0.5f),
                                     ImVec2(curRight, rowY + (rowHeight + navSize) * 0.5f));
                curRight -= navSize;
                res.prev = ImRect(ImVec2(curRight - navSize, rowY + (rowHeight - navSize) * 0.5f),
                                  ImVec2(curRight, rowY + (rowHeight + navSize) * 0.5f));
                curRight -= navSize;
            }
        }
        // graph toggle: left of the stopwatch / navigation cluster
        if (!r.graphChannels.empty() && r.graphOf == 0) {
            const float gSize = Dp(18.0f);
            res.graph = ImRect(ImVec2(curRight - gSize, rowY + (rowHeight - gSize) * 0.5f),
                               ImVec2(curRight, rowY + (rowHeight + gSize) * 0.5f));
            res.hasGraph = true;
            curRight -= gSize + Dp(2.0f);
        }
        res.leftBound = curRight;
        return res;
    };

    bool leftDown = ImGui::IsMouseDown(0);
    bool leftReleased = ImGui::IsMouseReleased(0);

    // --- graph lanes: geometry shared by the hit tests and the drawing. A lane row carries its parent's id in
    // `graphOf` and its position (0..kTimelineGraphRows-1) in `graphIndex`; the caller places the block right after
    // the parent row, so the parent is `rowIndex - graphIndex - 1`.
    const auto laneParentOf = [&](int rowIndex) -> int {
        if (rowIndex < 0 || rowIndex >= (int)rows.size()) return -1;
        const TimelineRow& r = rows[rowIndex];
        if (r.graphOf == 0 || r.graphIndex < 0 || r.graphIndex >= kTimelineGraphRows) return -1;
        const int parent = rowIndex - r.graphIndex - 1;
        if (parent < 0 || parent >= (int)rows.size() || rows[parent].id != r.graphOf) return -1;
        return parent;
    };
    const auto laneTopOf = [&](int parentIndex) -> float {
        return contentY - view.scrollY + (parentIndex + 1) * rowHeight;
    };
    // Chip rects of a lane block: one per channel, wrapped, kept in the label column's left half (the value sub-lane's
    // centre holds "값" and its right edge the min / max numbers). Width 0 = the chip did not fit.
    const auto chipRectsOf = [&](int parentIndex) -> std::vector<ImRect> {
        std::vector<ImRect> out;
        if (parentIndex < 0 || parentIndex >= (int)rows.size()) return out;
        const auto& chans = rows[parentIndex].graphChannels;
        out.resize(chans.size(), ImRect(0, 0, 0, 0));
        if (chans.empty()) return out;
        const float valueTop = laneTopOf(parentIndex), valueBottom = valueTop + 3.0f * rowHeight;
        const float chipH = Dp(13.0f), gap = Dp(3.0f);
        const float xStart = bb.Min.x + Dp(6.0f), xMax = bb.Min.x + colWidth * 0.5f - Dp(18.0f);
        float x = xStart, y = valueTop + Dp(3.0f);
        for (size_t i = 0; i < chans.size(); ++i) {
            const float textW = TextSize(Font::Regular, size::Caption - 1.0f, chans[i].label.c_str()).x;
            const float w = Dp(9.0f) + textW + Dp(8.0f);
            if (x > xStart && x + w > xMax) { x = xStart; y += chipH + gap; }
            if (y + chipH > valueBottom - Dp(2.0f)) break;
            out[i] = ImRect(ImVec2(x, y), ImVec2(x + w, y + chipH));
            x += w + Dp(3.0f);
        }
        return out;
    };

    // --- a lane block's plot, sampled at most once per frame and shared by the handle hit tests and the drawing. The
    // pointer stays valid for the rest of the call (the cache is reserved up front).
    struct LanePlot {
        bool ok = false;
        float scrollFrame = 0.0f, pxPerFrame = 0.0f, scrollY = 0.0f;   // the view these geometry + samples were taken in
        float blockTop = 0.0f, valueTop = 0.0f, valueBottom = 0.0f, speedTop = 0.0f, speedBottom = 0.0f;
        float x0 = 0.0f, x1 = 0.0f, plotX0 = 0.0f;
        float vmin = 0.0f, vmax = 1.0f, dataMin = 0.0f, dataMax = 0.0f, speedMax = 1.0f, speedPeak = 0.0f;
        int sampleCount = 0;
        std::vector<float> frames, activeVals;
    };
    std::vector<LanePlot> lanePlots(rows.size());  // one per row index (stable addresses); re-sampled when the view moved
    // A value -> y inside a block's value sub-lane (the single source of the lane curve and the handle mapping).
    const auto laneValueY = [](const LanePlot& plot, float v) {
        return plot.valueBottom - Dp(2.0f) -
               (v - plot.vmin) / (plot.vmax - plot.vmin) * (plot.valueBottom - plot.valueTop - Dp(4.0f));
    };
    const auto lanePlotOf = [&](int parentIndex) -> const LanePlot* {
        if (parentIndex < 0 || parentIndex >= (int)rows.size()) return nullptr;
        LanePlot& out = lanePlots[(size_t)parentIndex];
        if (out.ok && out.scrollFrame == view.scrollFrame && out.pxPerFrame == view.pxPerFrame && out.scrollY == view.scrollY)
            return &out;
        out = LanePlot{};
        if (!view.graphSample) return &out;
        const TimelineRow& parent = rows[parentIndex];
        if (parent.graphChannels.empty()) return &out;
        const int active = std::clamp(parent.graphActive, 0, (int)parent.graphChannels.size() - 1);
        out.scrollFrame = view.scrollFrame;
        out.pxPerFrame = view.pxPerFrame;
        out.scrollY = view.scrollY;
        out.blockTop = laneTopOf(parentIndex);
        out.valueTop = out.blockTop;
        out.valueBottom = out.blockTop + 3.0f * rowHeight;
        out.speedTop = out.valueBottom;
        out.speedBottom = out.blockTop + (float)kTimelineGraphRows * rowHeight;
        const float framesPerPx = 1.0f / std::max(view.pxPerFrame * Dpi(), 0.0001f);
        const float fA = view.scrollFrame;
        const float fB = view.scrollFrame + std::max(keyAreaW - Dp(12.0f), 1.0f) * framesPerPx;
        out.sampleCount = std::clamp((int)std::ceil(keyAreaW / Dp(2.0f)), 2, 2048);
        out.frames.resize((size_t)out.sampleCount);
        out.activeVals.resize((size_t)out.sampleCount);
        for (int i = 0; i < out.sampleCount; ++i) {
            const float f = fA + (fB - fA) * (float)i / (float)(out.sampleCount - 1);
            out.frames[(size_t)i] = f;
            out.activeVals[(size_t)i] = view.graphSample(parent.id, active, f);
        }
        float vmin = out.activeVals[0], vmax = out.activeVals[0], speedMax = 0.0f;
        for (float v : out.activeVals) { vmin = std::min(vmin, v); vmax = std::max(vmax, v); }
        out.dataMin = vmin;
        out.dataMax = vmax;  // printed next to the sub-lane (the plot range is padded)
        for (int i = 1; i < out.sampleCount; ++i) {
            const float df = out.frames[(size_t)i] - out.frames[(size_t)(i - 1)];
            if (df > 0.0f) speedMax = std::max(speedMax, std::abs(out.activeVals[(size_t)i] - out.activeVals[(size_t)(i - 1)]) / df);
        }
        speedMax *= kGraphLaneFps;  // frames -> per second
        out.speedPeak = speedMax;   // printed at the speed lane's left edge (before the min span guard)
        if (vmax - vmin < 1e-4f) {  // flat: pad around the value
            const float mid = (vmin + vmax) * 0.5f, pad = std::max(std::abs(mid) * 0.1f, 0.5f);
            vmin = mid - pad;
            vmax = mid + pad;
        } else {
            const float pad = (vmax - vmin) * 0.1f;
            vmin -= pad;
            vmax += pad;
        }
        out.vmin = vmin;
        out.vmax = vmax;
        if (speedMax < 1e-3f) speedMax = 1.0f;  // min span guard
        out.speedMax = speedMax;
        out.x0 = std::max(keyAreaX, bb.Min.x);
        out.x1 = bb.Max.x;
        out.plotX0 = originX - view.scrollFrame * view.pxPerFrame * Dpi();  // x of frame 0
        out.ok = true;
        return &out;
    };

    // The editable segment's handle box of a lane block: P1 / P2 map the curve bytes 0..127 through the box from the
    // previous key (xa, ya) to the key that owns the bytes (xb, yb). A flat segment (< Dp(36) tall) uses a box of that
    // height going UP from ya, so its handles stay draggable.
    struct LaneHandles {
        bool ok = false;
        float xa = 0.0f, xb = 0.0f, ya = 0.0f, yb = 0.0f, boxY1 = 0.0f;
        ImVec2 p1{0, 0}, p2{0, 0};
    };
    const auto laneHandlesOf = [&](const LanePlot& plot, const TimelineRow& parent) -> LaneHandles {
        LaneHandles res;
        if (!plot.ok || !view.graphSample || !parent.graphOpen) return res;  // handles only exist while the lane is open
        if (parent.graphSegFrom < 0 || parent.graphSegTo <= parent.graphSegFrom) return res;
        const int active = std::clamp(parent.graphActive, 0, (int)parent.graphChannels.size() - 1);
        res.xa = plot.plotX0 + (float)parent.graphSegFrom * view.pxPerFrame * Dpi();
        res.xb = plot.plotX0 + (float)parent.graphSegTo * view.pxPerFrame * Dpi();
        res.ya = laneValueY(plot, view.graphSample(parent.id, active, (float)parent.graphSegFrom));
        res.yb = laneValueY(plot, view.graphSample(parent.id, active, (float)parent.graphSegTo));
        res.boxY1 = std::abs(res.yb - res.ya) < Dp(36.0f) ? res.ya - Dp(36.0f) : res.yb;
        res.p1 = GraphHandlePoint(res.xa, res.xb, res.ya, res.boxY1, parent.graphCurve[0], parent.graphCurve[1]);
        res.p2 = GraphHandlePoint(res.xa, res.xb, res.ya, res.boxY1, parent.graphCurve[2], parent.graphCurve[3]);
        res.ok = true;
        return res;
    };
    // Mouse position -> the dragged handle's bytes (the other handle keeps its bytes).
    const auto laneDragBytes = [&](const TimelineRow& parent, const LaneHandles& h, ImVec2 mouse, uint8_t out[4]) {
        for (int i = 0; i < 4; ++i) out[i] = parent.graphCurve[i];
        const float u = h.xb != h.xa ? (mouse.x - h.xa) / (h.xb - h.xa) : 0.0f;
        const float v = h.boxY1 != h.ya ? (mouse.y - h.ya) / (h.boxY1 - h.ya) : 0.0f;
        const int i = view.graphDrag == 1 ? 0 : 2;
        out[i] = (uint8_t)std::clamp((int)std::lround(u * 127.0f), 0, 127);
        out[i + 1] = (uint8_t)std::clamp((int)std::lround(v * 127.0f), 0, 127);
    };
    const auto laneIndexOfRow = [&](uint64_t rowId) -> int {
        for (int ri = 0; ri < (int)rows.size(); ++ri)
            if (rows[ri].id == rowId) return ri;
        return -1;
    };
    // The handle under the mouse of the lane block at `parentIndex` (1 = P1, 2 = P2, 0 = none), inside the clip only.
    const auto laneHandleUnderMouse = [&](int parentIndex, const LanePlot*& plotOut) -> int {
        plotOut = nullptr;
        const LanePlot* plot = lanePlotOf(parentIndex);
        if (!plot || !plot->ok) return 0;
        const LaneHandles h = laneHandlesOf(*plot, rows[parentIndex]);
        if (!h.ok) return 0;
        if (mousePos.x < plot->x0 || mousePos.x > plot->x1) return 0;
        if (mousePos.y < std::max(contentY, plot->valueTop) || mousePos.y > std::min(bb.Max.y - sbH, plot->valueBottom)) return 0;
        const float r2 = Dp(8.0f) * Dp(8.0f);
        const auto d2 = [&](ImVec2 q) {
            const float dx = mousePos.x - q.x, dy = mousePos.y - q.y;
            return dx * dx + dy * dy;
        };
        const float d1 = d2(h.p1), d2p = d2(h.p2);
        plotOut = plot;
        if (d1 <= r2 && d1 <= d2p) return 1;
        if (d2p <= r2) return 2;
        return 0;
    };
    // Emits the current drag state as a curve event (`end` also ends the gesture). A drag whose segment disappeared
    // mid-gesture still reports the end, so the host commits its undo step. Nothing is reported until the pointer
    // moved: a plain click on a handle must not rewrite the curve (nor start an undo step).
    const auto laneEmitDrag = [&](bool end) {
        if (view.graphDrag == 0) return;
        const int parentIndex = laneIndexOfRow(view.graphDragRow);
        const LanePlot* plot = parentIndex >= 0 ? lanePlotOf(parentIndex) : nullptr;
        if (view.graphDragMoved && plot) {
            const LaneHandles h = laneHandlesOf(*plot, rows[parentIndex]);
            if (h.ok) {
                uint8_t bytes[4];
                laneDragBytes(rows[parentIndex], h, mousePos, bytes);
                ev.graphCurve = true;
                ev.graphCurveRow = rows[parentIndex].id;
                ev.graphCurveFrame = rows[parentIndex].graphSegTo;
                for (int i = 0; i < 4; ++i) ev.graphCurveBytes[i] = bytes[i];
            }
        }
        ev.graphCurveEnd = end && view.graphDragMoved;
        if (ev.graphCurve || ev.graphCurveEnd) changed = true;
    };

    if (pressed && io.MouseClicked[0]) {
        if (mousePos.x >= keyAreaX && mousePos.y >= bb.Max.y - sbH) {
            int T = maxFrame + 120;
            float V = visibleFrames;
            if (T > V) {
                float tw = std::max(Dp(24.0f), keyAreaW * V / T);
                float tx = keyAreaX + (view.scrollFrame / maxScrollFrame) * (keyAreaW - tw);
                if (mousePos.x >= tx && mousePos.x <= tx + tw) {
                    view.dragMode = 5;
                    view.hscrollGrab = mousePos.x - tx;
                } else {
                    view.scrollFrame = std::clamp(((mousePos.x - tw * 0.5f - keyAreaX) / (keyAreaW - tw)) * maxScrollFrame, 0.0f, maxScrollFrame);
                    view.dragMode = 5;
                    view.hscrollGrab = tw * 0.5f;
                }
            }
        } else if (mousePos.y >= bb.Min.y && mousePos.y < contentY && mousePos.x >= keyAreaX) {
            if (io.KeyShift) {
                view.dragMode = 4;
                view.rangeAnchor = std::max(0, (int)std::round((mousePos.x - originX) / (view.pxPerFrame * Dpi()) + view.scrollFrame));
                view.rangeStart = view.rangeEnd = view.rangeAnchor;
                ev.rangeChanged = true;
                changed = true;
            } else {
                view.dragMode = 1;
            }
        } else if (mousePos.x < keyAreaX && mousePos.y >= contentY && mousePos.y < bb.Max.y - sbH) {
            int rowIndex = (int)((mousePos.y - contentY + view.scrollY) / rowHeight);
            if (rowIndex >= 0 && rowIndex < (int)rows.size()) {
                const auto& row = rows[rowIndex];
                RowControlRects cr = controlRects(rowIndex);
                // a graph lane row's label does nothing (lane rows are not selectable)
                const int laneParent = laneParentOf(rowIndex);
                bool chipHit = false;
                if (laneParent >= 0) {
                    const std::vector<ImRect> chips = chipRectsOf(laneParent);
                    for (size_t i = 0; i < chips.size(); ++i) {
                        if (chips[i].GetWidth() > 0.0f && chips[i].Contains(mousePos)) {
                            ev.graphChannel = true;
                            ev.graphChannelRow = rows[laneParent].id;
                            ev.graphChannelIndex = (int)i;
                            changed = true;
                            chipHit = true;
                            break;
                        }
                    }
                }
                if (chipHit) {
                    // the chip consumed the click
                } else if (cr.hasGraph && cr.graph.Contains(mousePos)) {
                    ev.toggleGraph = true;
                    ev.toggleGraphRow = row.id;
                    changed = true;
                } else if (cr.hasStopwatch && cr.sw.Contains(mousePos)) {
                    ev.stopwatch = true;
                    ev.stopwatchRow = row.id;
                    changed = true;
                } else if (cr.hasStopwatch && cr.animated && cr.prev.Contains(mousePos)) {
                    if (cr.hasPrev) {
                        ev.keyNav = true;
                        ev.keyNavRow = row.id;
                        ev.keyNavDir = -1;
                        changed = true;
                    }
                } else if (cr.hasStopwatch && cr.animated && cr.keyHere.Contains(mousePos)) {
                    ev.keyNav = true;
                    ev.keyNavRow = row.id;
                    ev.keyNavDir = 2;
                    changed = true;
                } else if (cr.hasStopwatch && cr.animated && cr.next.Contains(mousePos)) {
                    if (cr.hasNext) {
                        ev.keyNav = true;
                        ev.keyNavRow = row.id;
                        ev.keyNavDir = 1;
                        changed = true;
                    }
                } else if (row.isGroup && mousePos.x < bb.Min.x + Dp(4.0f) + row.depth * Dp(14.0f) + Dp(18.0f)) {
                    ev.toggleGroup = true;
                    ev.toggledRow = row.id;
                    changed = true;
                } else if (laneParent >= 0) {
                    // a lane row's label is not a row: no selection change
                } else {
                    ev.rowClick = true;
                    ev.rowClickId = row.id;
                    ev.rowClickMode = io.KeyShift ? RowClickMode::Range : (io.KeyCtrl ? RowClickMode::Toggle : RowClickMode::Replace);
                    changed = true;
                    if (ImGui::IsMouseDoubleClicked(0) && row.isGroup) {
                        ev.toggleGroup = true;
                        ev.toggledRow = row.id;
                    }
                }
            }
        } else if (mousePos.x >= keyAreaX && mousePos.y >= contentY && mousePos.y < bb.Max.y - sbH) {
            int rowIndex = (int)((mousePos.y - contentY + view.scrollY) / rowHeight);
            // A lane handle (P1 / P2) starts a curve drag before any key / box / playhead drag is considered.
            int handleParent = rowIndex;
            if (rowIndex >= 0 && rowIndex < (int)rows.size()) {
                const int laneParent = laneParentOf(rowIndex);
                if (laneParent >= 0) handleParent = laneParent;
            }
            const LanePlot* handlePlot = nullptr;
            const int handleHit = handleParent >= 0 && handleParent < (int)rows.size() ? laneHandleUnderMouse(handleParent, handlePlot) : 0;
            if (handleHit != 0) {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    // reset the segment to linear and end the gesture at once (any drag started by the first click ends
                    // here too, so the release does not re-emit the drag's bytes)
                    static const uint8_t linear[4] = {20, 20, 107, 107};
                    view.graphDrag = 0;
                    view.graphDragRow = 0;
                    view.graphDragMoved = false;
                    ev.graphCurve = true;
                    ev.graphCurveRow = rows[handleParent].id;
                    ev.graphCurveFrame = rows[handleParent].graphSegTo;
                    for (int i = 0; i < 4; ++i) ev.graphCurveBytes[i] = linear[i];
                    ev.graphCurveEnd = true;
                    changed = true;
                } else {
                    view.graphDrag = handleHit;
                    view.graphDragRow = rows[handleParent].id;
                    view.graphDragMoved = false;
                    view.dragMode = 0;
                    laneEmitDrag(false);
                }
            }
            bool hitKey = false;
            if (handleHit != 0) {
                // a press on a handle starts no other drag (box select, playhead, row click)
            } else if (rowIndex >= 0 && rowIndex < (int)rows.size()) {
                const auto& r = rows[rowIndex];
                if (r.keysEditable) {
                    const TimelineKey* bestKey = keyUnderMouse(rowIndex);
                    if (bestKey) {
                        hitKey = true;
                        view.anchorRow = r.id;
                        view.anchorFrame = bestKey->frame;
                        bool isAlreadySelected = bestKey->selected;
                        ImGui::GetStateStorage()->SetBool(imguiId, isAlreadySelected);
                        bool emitOnPress = !(isAlreadySelected && !io.KeyCtrl && !io.KeyShift);
                        if (emitOnPress) {
                            ev.select = true;
                            ev.selectMode = io.KeyCtrl ? SelectMode::Toggle : (io.KeyShift ? SelectMode::Add : SelectMode::Replace);
                            ev.selectKeys.push_back({r.id, bestKey->frame});
                            changed = true;
                            view.dragMinFrame = std::min(GetMinSelectedFrame(rows), bestKey->frame);
                        } else {
                            view.dragMinFrame = GetMinSelectedFrame(rows);
                        }
                        if (view.dragMinFrame == 2147483647) view.dragMinFrame = 0;
                        view.dragMode = 2;
                        view.dragDelta = 0;
                        view.dragGrabFrame = view.scrollFrame + (mousePos.x - originX) / (view.pxPerFrame * Dpi());
                    }
                }
            }
            if (!hitKey && handleHit == 0) {
                const bool laneRow = laneParentOf(rowIndex) >= 0;
                if (laneRow) {
                    // graph lanes are read-only: no box select, no key insert
                } else if (ImGui::IsMouseDoubleClicked(0)) {
                    if (rowIndex >= 0 && rowIndex < (int)rows.size()) {
                        const auto& r = rows[rowIndex];
                        if (r.keysEditable && !r.isGroup) {
                            ev.addKeyAt = true;
                            ev.addKeyRow = r.id;
                            ev.addKeyFrame = (int)std::round((mousePos.x - originX) / (view.pxPerFrame * Dpi()) + view.scrollFrame);
                            changed = true;
                        }
                    }
                } else {
                    view.dragMode = 3;
                    view.boxAnchorFrame = view.scrollFrame + (mousePos.x - originX) / (view.pxPerFrame * Dpi());
                    view.boxAnchorY = (mousePos.y - contentY) + view.scrollY;  // content px below the rows area's top
                }
            }
        }
    }

    if (pressed && io.MouseClicked[1]) {
        if (mousePos.x >= keyAreaX && mousePos.y >= contentY && mousePos.y < bb.Max.y - sbH &&
            laneParentOf((int)((mousePos.y - contentY + view.scrollY) / rowHeight)) < 0) {
            // key area: the host's key menu; a key under the mouse that is not selected becomes the selection
            // (graph lane rows are read-only: no menu)
            const int rowIndex = (int)((mousePos.y - contentY + view.scrollY) / rowHeight);
            ev.contextMenu = true;
            ev.contextFrame = std::max(0, (int)std::round((mousePos.x - originX) / (view.pxPerFrame * Dpi()) + view.scrollFrame));
            if (rowIndex >= 0 && rowIndex < (int)rows.size()) {
                ev.contextRow = rows[rowIndex].id;
                if (rows[rowIndex].keysEditable)
                    if (const TimelineKey* k = keyUnderMouse(rowIndex); k && !k->selected) {
                        ev.select = true;
                        ev.selectMode = SelectMode::Replace;
                        ev.selectKeys.push_back({rows[rowIndex].id, k->frame});
                    }
            }
            changed = true;
        } else if (mousePos.y >= bb.Min.y && mousePos.y < contentY && mousePos.x >= keyAreaX) {
            // ruler: the host's range menu (range start here / range end here / clear range); the range itself is untouched
            ev.contextMenu = true;
            ev.contextOnRuler = true;
            ev.contextFrame = std::max(0, (int)std::round((mousePos.x - originX) / (view.pxPerFrame * Dpi()) + view.scrollFrame));
            ev.contextRow = 0;
            changed = true;
        }
    }

    // ButtonBehavior clears the active id in the release frame, so drags are tracked by view.dragMode.
    if (view.graphDrag != 0 && leftDown) {
        if (!view.graphDragMoved && ImGui::IsMouseDragging(ImGuiMouseButton_Left, Dp(2.0f))) view.graphDragMoved = true;
        laneEmitDrag(false);
    }
    if ((held || ImGui::IsItemActive()) && leftDown) {
        // Edge auto-scroll (modes 2/3/4): scroll when the mouse is near/past the key area's edges, faster the
        // further past the edge zone it is; frame-rate independent via io.DeltaTime, up to ~2 screens/second.
        if (view.dragMode >= 2 && view.dragMode <= 4 && keyAreaW > 0.0f && ImGui::IsMouseDragging(ImGuiMouseButton_Left, Dp(3.0f))) {
            const float edgeZone = Dp(24.0f);
            const float mouseInArea = mousePos.x - keyAreaX;  // px from the key area's left edge
            float dir = 0.0f;   // -1 scroll left, +1 scroll right
            float dist = 0.0f;  // px past the inner edge of the zone (0 inside the zone)
            if (mouseInArea < edgeZone) {
                dir = -1.0f;
                dist = edgeZone - mouseInArea;  // grows inside the zone and keeps growing when outside
            } else if (mouseInArea > keyAreaW - edgeZone) {
                dir = 1.0f;
                dist = mouseInArea - (keyAreaW - edgeZone);
            }
            if (io.DeltaTime > 0.0f) {
                if (dir != 0.0f) {
                    float t = std::min(dist / edgeZone, 1.0f);  // 0 at the zone's inner edge, 1 past it
                    const float speed = (0.25f + 1.75f * t) * keyAreaW;  // px per second: 0.25..2 screens/second
                    view.scrollFrame += dir * speed * io.DeltaTime / (view.pxPerFrame * Dpi());
                    view.scrollFrame = std::clamp(view.scrollFrame, 0.0f, maxScrollFrame);
                }
                if (view.dragMode == 2 || view.dragMode == 3) {
                    // The same vertically against the rows area (independent of the horizontal edges).
                    const float rowsTop = contentY;
                    const float rowsBottom = bb.Max.y - sbH;
                    const float mouseFromTop = mousePos.y - rowsTop;
                    const float rowsH = std::max(0.0f, rowsBottom - rowsTop);
                    float vdir = 0.0f, vdist = 0.0f;
                    if (mouseFromTop < edgeZone && mouseFromTop < rowsH) { vdir = -1.0f; vdist = edgeZone - mouseFromTop; }
                    else if (rowsH > edgeZone && mousePos.y > rowsBottom - edgeZone) { vdir = 1.0f; vdist = mousePos.y - (rowsBottom - edgeZone); }
                    if (vdir != 0.0f) {
                        const float speed = (0.25f + 0.75f * std::min(vdist / edgeZone, 1.0f)) * rowsH;  // up to 1 screen height per second
                        view.scrollY += vdir * speed * io.DeltaTime;
                        view.scrollY = std::clamp(view.scrollY, 0.0f, maxScrollY);
                    }
                }
            }
        }
        if (view.dragMode == 1) {
            ev.seek = true;
            ev.seekFrame = std::max(0, (int)std::round((mousePos.x - originX) / (view.pxPerFrame * Dpi()) + view.scrollFrame));
            changed = true;
        } else if (view.dragMode == 2) {
            // dragDelta from the frame under the mouse vs the grab point in CONTENT coordinates, so the keys
            // follow the mouse even while the view auto-scrolls.
            float mouseFrame = view.scrollFrame + (mousePos.x - originX) / (view.pxPerFrame * Dpi());
            float dx = (mouseFrame - view.dragGrabFrame) * view.pxPerFrame * Dpi();
            if (std::abs(dx) >= Dp(3.0f)) {
                view.dragDelta = (int)std::round(dx / (view.pxPerFrame * Dpi()));
                if (view.dragMinFrame + view.dragDelta < 0) {
                    view.dragDelta = -view.dragMinFrame;
                }
            } else {
                view.dragDelta = 0;
            }
        } else if (view.dragMode == 4) {
            int f = std::max(0, (int)std::round((mousePos.x - originX) / (view.pxPerFrame * Dpi()) + view.scrollFrame));
            int newStart = std::min(view.rangeAnchor, f);
            int newEnd = std::max(view.rangeAnchor, f);
            if (newStart != view.rangeStart || newEnd != view.rangeEnd) {
                view.rangeStart = newStart;
                view.rangeEnd = newEnd;
                ev.rangeChanged = true;
                changed = true;
            }
        } else if (view.dragMode == 5) {
            int T = maxFrame + 120;
            float V = visibleFrames;
            float tw = std::max(Dp(24.0f), keyAreaW * V / T);
            view.scrollFrame = std::clamp(((mousePos.x - view.hscrollGrab - keyAreaX) / (keyAreaW - tw)) * maxScrollFrame, 0.0f, maxScrollFrame);
        }
    }

    if (leftReleased && view.graphDrag != 0) {
        laneEmitDrag(true);  // the final bytes + one undo step
        view.graphDrag = 0;
        view.graphDragRow = 0;
        view.graphDragMoved = false;
    }
    if (leftReleased && view.dragMode != 0) {
        if (view.dragMode == 1) {
            // done
        } else if (view.dragMode == 2) {
            if (view.dragDelta != 0) {
                ev.moveKeys = true;
                ev.moveDelta = view.dragDelta;
                changed = true;
            } else {
                bool wasAlreadySelected = ImGui::GetStateStorage()->GetBool(imguiId);
                if (wasAlreadySelected && !io.KeyCtrl && !io.KeyShift) {
                    ev.select = true;
                    ev.selectMode = SelectMode::Replace;
                    ev.selectKeys.push_back({view.anchorRow, view.anchorFrame});
                    changed = true;
                }
            }
        } else if (view.dragMode == 3) {
            ev.select = true;
            ev.selectMode = (io.KeyShift || io.KeyCtrl) ? SelectMode::Add : SelectMode::Replace;
            // The box anchor is in content coordinates (frame + px below the rows area's top), so the selection
            // still resolves correctly after the view auto-scrolled (anchor possibly off screen).
            float anchorX = originX + (view.boxAnchorFrame - view.scrollFrame) * view.pxPerFrame * Dpi();
            float anchorY = contentY + view.boxAnchorY - view.scrollY;
            float bx0 = std::min(anchorX, mousePos.x);
            float bx1 = std::max(anchorX, mousePos.x);
            float by0 = std::min(anchorY, mousePos.y);
            float by1 = std::max(anchorY, mousePos.y);

            float f0f = view.scrollFrame + (bx0 - originX) / (view.pxPerFrame * Dpi());
            float f1f = view.scrollFrame + (bx1 - originX) / (view.pxPerFrame * Dpi());
            // Row range in content px: independent of the current scroll.
            float row0f = by0 - contentY + view.scrollY;
            float row1f = by1 - contentY + view.scrollY;
            int firstRow = std::max(0, (int)std::floor(row0f / rowHeight));
            int lastRow = std::min((int)rows.size() - 1, (int)std::floor(row1f / rowHeight));

            for (int ri = firstRow; ri <= lastRow; ++ri) {
                const auto& r = rows[ri];
                if (!r.keysEditable) continue;
                float rowCenter = ri * rowHeight + rowHeight * 0.5f;  // content px
                if (rowCenter >= row0f && rowCenter <= row1f) {
                    auto it = std::lower_bound(r.keys.begin(), r.keys.end(), (int)std::floor(f0f),
                        [](const TimelineKey& a, int b) { return a.frame < b; });
                    for (; it != r.keys.end() && it->frame <= std::ceil(f1f); ++it) {
                        if ((float)it->frame >= f0f && (float)it->frame <= f1f) {
                            ev.selectKeys.push_back({r.id, it->frame});
                        }
                    }
                }
            }
            changed = true;
        }
        view.dragMode = 0;
    }
    
    if (!leftDown && view.graphDrag != 0) {
        laneEmitDrag(true);  // the release happened outside the item: still end the gesture
        view.graphDrag = 0;
        view.graphDragRow = 0;
        view.graphDragMoved = false;
    }
    if (!leftDown) {
        if (view.dragMode >= 1 && view.dragMode <= 5) view.dragMode = 0;
    }

    // Caller request: scroll a row into view (~1 row of margin), then clear the request (unknown id = just clear).
    if (view.scrollToRow != 0) {
        for (int ri = 0; ri < (int)rows.size(); ++ri) {
            if (rows[ri].id == view.scrollToRow) {
                float rowTop = ri * rowHeight;             // content px
                float rowBottom = rowTop + rowHeight;
                float visTop = view.scrollY;
                float visBottom = view.scrollY + contentH;
                if (rowTop < visTop || rowBottom > visBottom) {  // partly visible counts as not visible
                    if (rowTop < visTop) {
                        view.scrollY = rowTop - rowHeight;
                    } else {
                        view.scrollY = rowBottom - contentH + rowHeight;
                    }
                }
                break;
            }
        }
        view.scrollToRow = 0;
    }

    view.scrollFrame = std::clamp(view.scrollFrame, 0.0f, maxScrollFrame);
    view.scrollY = std::clamp(view.scrollY, 0.0f, maxScrollY);

    // Hover over a lane handle: the hand cursor.
    if (hovered && view.graphDrag == 0 && !leftDown) {
        const int rowIndex = (int)((mousePos.y - contentY + view.scrollY) / rowHeight);
        int parentIndex = rowIndex;
        if (rowIndex >= 0 && rowIndex < (int)rows.size()) {
            const int laneParent = laneParentOf(rowIndex);
            if (laneParent >= 0) parentIndex = laneParent;
        }
        const LanePlot* hoverPlot = nullptr;
        if (parentIndex >= 0 && parentIndex < (int)rows.size() && laneHandleUnderMouse(parentIndex, hoverPlot) != 0)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    dl->PushClipRect(bb.Min, bb.Max, true);
    
    int firstRow = std::max(0, (int)std::floor(view.scrollY / rowHeight));
    int lastRow = std::min((int)rows.size() - 1, (int)((view.scrollY + contentH) / rowHeight) + 1);
    
    // Draw rows
    for (int ri = firstRow; ri <= lastRow; ++ri) {
        const auto& r = rows[ri];
        float rowY = contentY - view.scrollY + ri * rowHeight;

        ImVec2 rmin(bb.Min.x, rowY);
        ImVec2 rmax(bb.Max.x, rowY + rowHeight);

        ImU32 rowBg = (ri % 2 == 0) ? 0 : WithAlpha(p.sunken, 0.5f);
        if (r.isGroup) rowBg = p.sunken;
        if (rowBg) dl->AddRectFilled(rmin, rmax, rowBg);
        
        if (r.selected) {
            dl->AddRectFilled(rmin, ImVec2(keyAreaX, rmax.y), p.accentSoft);
        }

        if (r.tint) dl->AddRectFilled(ImVec2(rmin.x, rmin.y + Dp(4.0f)), ImVec2(rmin.x + Dp(3.0f), rmax.y - Dp(4.0f)), r.tint, Dp(1.5f));
        float labelX = bb.Min.x + Dp(4.0f) + r.depth * Dp(14.0f);
        if (r.isGroup) {
            Icon(dl, r.expanded ? icon::CaretDown : icon::CaretRight, 13.0f, ImVec2(labelX + Dp(8.0f), rowY + rowHeight * 0.5f), p.ink);
            labelX += Dp(16.0f);
        }
        RowControlRects cr = controlRects(ri);
        float textRight = keyAreaX - Dp(4.0f);
        if (cr.hasStopwatch || cr.hasGraph) {
            textRight = cr.leftBound - Dp(4.0f);
        }
        ImU32 textCol = r.selected ? p.accentInk : p.ink;
        TextEllipsis(dl, r.isGroup ? Font::Semibold : Font::Regular, size::Small,
                     ImVec2(labelX, rowY + (rowHeight - Dp(size::Small * 1.25f)) * 0.5f), textRight, textCol, r.label.c_str());

        if (cr.hasGraph) {
            // graph toggle: accent while the lane is open, quiet otherwise
            const bool graphHovered = cr.graph.Contains(mousePos);
            if (graphHovered) dl->AddRectFilled(cr.graph.Min, cr.graph.Max, WithAlpha(p.ink, 0.06f), Dp(4.0f));
            ImVec2 gCenter((cr.graph.Min.x + cr.graph.Max.x) * 0.5f, (cr.graph.Min.y + cr.graph.Max.y) * 0.5f);
            Icon(dl, icon::Waveform, 14.0f, gCenter, r.graphOpen ? p.accent : p.ink3);
            if (uiprobe::Enabled()) uiprobe::Add("##tlgraph", r.label.c_str(), cr.graph.Min, cr.graph.Max);
        }

        if (cr.hasStopwatch) {
            // Stopwatch button
            bool swHovered = cr.sw.Contains(mousePos);
            if (swHovered) {
                dl->AddRectFilled(cr.sw.Min, cr.sw.Max, WithAlpha(p.ink, 0.06f), Dp(4.0f));
            }
            ImVec2 swCenter((cr.sw.Min.x + cr.sw.Max.x) * 0.5f, (cr.sw.Min.y + cr.sw.Max.y) * 0.5f);
            Icon(dl, icon::Timer, 14.0f, swCenter, cr.animated ? p.accent : p.ink3);
            if (uiprobe::Enabled()) uiprobe::Add("##tlsw", r.label.c_str(), cr.sw.Min, cr.sw.Max);

            if (cr.animated) {
                // Prev key button
                bool prevHovered = cr.prev.Contains(mousePos);
                if (prevHovered && cr.hasPrev) {
                    dl->AddRectFilled(cr.prev.Min, cr.prev.Max, WithAlpha(p.ink, 0.06f), Dp(4.0f));
                }
                ImVec2 prevCenter((cr.prev.Min.x + cr.prev.Max.x) * 0.5f, (cr.prev.Min.y + cr.prev.Max.y) * 0.5f);
                Icon(dl, icon::CaretLeft, 13.0f, prevCenter, cr.hasPrev ? p.ink3 : WithAlpha(p.ink3, 0.35f));
                if (uiprobe::Enabled()) uiprobe::Add("##tlnav", r.label.c_str(), cr.prev.Min, cr.prev.Max);

                // Key at playhead diamond
                bool keyHovered = cr.keyHere.Contains(mousePos);
                if (keyHovered) {
                    dl->AddRectFilled(cr.keyHere.Min, cr.keyHere.Max, WithAlpha(p.ink, 0.06f), Dp(4.0f));
                }
                ImVec2 keyCenter((cr.keyHere.Min.x + cr.keyHere.Max.x) * 0.5f, (cr.keyHere.Min.y + cr.keyHere.Max.y) * 0.5f);
                const float dr = cr.keyHere.GetWidth() * 0.22f;
                const ImVec2 pts[4] = {
                    {keyCenter.x, keyCenter.y - dr},
                    {keyCenter.x + dr, keyCenter.y},
                    {keyCenter.x, keyCenter.y + dr},
                    {keyCenter.x - dr, keyCenter.y}
                };
                const ImU32 kc = cr.keyedHere ? p.accent : p.ink3;
                if (cr.keyedHere) dl->AddConvexPolyFilled(pts, 4, kc);
                else dl->AddPolyline(pts, 4, kc, ImDrawFlags_Closed, 1.5f);
                if (uiprobe::Enabled()) uiprobe::Add("##tlnav", r.label.c_str(), cr.keyHere.Min, cr.keyHere.Max);

                // Next key button
                bool nextHovered = cr.next.Contains(mousePos);
                if (nextHovered && cr.hasNext) {
                    dl->AddRectFilled(cr.next.Min, cr.next.Max, WithAlpha(p.ink, 0.06f), Dp(4.0f));
                }
                ImVec2 nextCenter((cr.next.Min.x + cr.next.Max.x) * 0.5f, (cr.next.Min.y + cr.next.Max.y) * 0.5f);
                Icon(dl, icon::CaretRight, 13.0f, nextCenter, cr.hasNext ? p.ink3 : WithAlpha(p.ink3, 0.35f));
                if (uiprobe::Enabled()) uiprobe::Add("##tlnav", r.label.c_str(), cr.next.Min, cr.next.Max);
            }
        }

        dl->AddLine(ImVec2(rmin.x, rmax.y), ImVec2(rmax.x, rmax.y), p.line);
        // label-column controls: a tooltip with the same strings the shortcuts use (after the Timeline item, which is
        // hovered while the mouse rests on one of these rects)
        if (cr.hasGraph && cr.graph.Contains(mousePos)) Tooltip(Tr("값 / 속력 그래프"));
        if (cr.hasStopwatch && cr.sw.Contains(mousePos))
            Tooltip(cr.animated ? Tr("애니메이션 끄기 (키프레임 모두 삭제)") : Tr("애니메이션 켜기"));
        if (cr.hasStopwatch && cr.animated && cr.prev.Contains(mousePos)) Tooltip(Tr("이전 키"));
        if (cr.hasStopwatch && cr.animated && cr.keyHere.Contains(mousePos)) Tooltip(Tr("이 프레임 키 추가/삭제"));
        if (cr.hasStopwatch && cr.animated && cr.next.Contains(mousePos)) Tooltip(Tr("다음 키"));
        if (uiprobe::Enabled() && r.graphOf == 0) uiprobe::Add("##tlrow", r.label.c_str(), rmin, ImVec2(keyAreaX, rmax.y));   // MCP ui_click
    }
    
    // --- graph lanes (drawn after the rows so a lane covers its block; clipped under the ruler and above the
    // horizontal scrollbar). Only graphIndex 0 draws; the other lane rows of the block are plain backgrounds.
    if (view.graphSample && !rows.empty()) {
        const float clipTop = contentY, clipBottom = bb.Max.y - sbH;
        // A block draws when ANY of its kTimelineGraphRows rows is visible: start below the first visible row by up to
        // a whole block so a scrolled-out first lane row still resolves its block.
        const int laneFirst = std::max(0, firstRow - (kTimelineGraphRows - 1));
        uint64_t lastBlock = 0;
        for (int ri = laneFirst; ri <= lastRow; ++ri) {
            const TimelineRow& lane = rows[ri];
            if (lane.graphOf == 0) continue;
            if (lane.graphIndex < 0 || lane.graphIndex >= kTimelineGraphRows) continue;
            const int parentIndex = laneParentOf(ri);
            if (parentIndex < 0) continue;
            const TimelineRow& parent = rows[parentIndex];
            if (parent.id == lastBlock) continue;  // one block spans kTimelineGraphRows consecutive rows
            lastBlock = parent.id;
            const auto& chans = parent.graphChannels;
            if (chans.empty()) continue;
            const int active = std::clamp(parent.graphActive, 0, (int)chans.size() - 1);

            const float blockTop = laneTopOf(parentIndex);
            const float valueTop = blockTop, valueBottom = blockTop + 3.0f * rowHeight;
            const float speedTop = valueBottom, speedBottom = blockTop + (float)kTimelineGraphRows * rowHeight;
            if (speedBottom < clipTop || valueTop > clipBottom) continue;  // scrolled out entirely

            const LanePlot* lanePlot = lanePlotOf(parentIndex);  // resamples when the view moved since the hit tests
            if (!lanePlot || !lanePlot->ok) continue;
            const int sampleCount = lanePlot->sampleCount;
            const std::vector<float>& frames = lanePlot->frames;
            const std::vector<float>& activeVals = lanePlot->activeVals;
            const float dataMin = lanePlot->dataMin, dataMax = lanePlot->dataMax;  // printed next to the sub-lane
            const float speedMax = lanePlot->speedMax, speedPeak = lanePlot->speedPeak;

            const float x0 = lanePlot->x0, x1 = lanePlot->x1;
            const float plotX0 = lanePlot->plotX0;  // x of frame 0
            const auto xOf = [&](float frame) { return plotX0 + frame * view.pxPerFrame * Dpi(); };
            const auto yOf = [&](float v) { return laneValueY(*lanePlot, v); };
            const auto ySpeed = [&](float s) {
                return speedBottom - Dp(2.0f) - s / speedMax * (speedBottom - speedTop - Dp(4.0f));
            };

            dl->PushClipRect(ImVec2(bb.Min.x, clipTop), ImVec2(bb.Max.x, clipBottom), true);
            // lane background (label column + key area), a thin separator between the sub-lanes
            dl->AddRectFilled(ImVec2(bb.Min.x, blockTop), ImVec2(bb.Max.x, speedBottom), WithAlpha(p.sunken, 0.85f));
            dl->AddLine(ImVec2(bb.Min.x, valueBottom), ImVec2(bb.Max.x, valueBottom), WithAlpha(p.line, 0.9f));
            dl->PopClipRect();

            // value curves stay inside the value sub-lane (and the key area: never over the ruler / scrollbar)
            dl->PushClipRect(ImVec2(x0, std::max(clipTop, valueTop)), ImVec2(x1, std::min(clipBottom, valueBottom)), true);
            // the other channels, dimmed: each normalised to its own min / max over the visible frames (shape only, full
            // lane height) so a flat active channel does not push them off the lane
            for (int ch = 0; ch < (int)chans.size(); ++ch) {
                if (ch == active || chans[(size_t)ch].color == 0) continue;
                std::vector<float> vals((size_t)sampleCount);
                float cmin = 0.0f, cmax = 0.0f;
                for (int i = 0; i < sampleCount; ++i) {
                    vals[(size_t)i] = view.graphSample(parent.id, ch, frames[(size_t)i]);
                    if (i == 0) { cmin = cmax = vals[0]; }
                    else { cmin = std::min(cmin, vals[(size_t)i]); cmax = std::max(cmax, vals[(size_t)i]); }
                }
                if (cmax - cmin < 1e-4f) { cmin -= 0.5f; cmax += 0.5f; }  // flat channel: a straight line mid-lane
                const float ghostTop = valueTop + Dp(2.0f), ghostBottom = valueBottom - Dp(2.0f);
                const auto yGhost = [&](float v) {
                    return ghostBottom - (v - cmin) / (cmax - cmin) * (ghostBottom - ghostTop);
                };
                std::vector<ImVec2> pts;
                pts.reserve((size_t)sampleCount);
                for (int i = 0; i < sampleCount; ++i) pts.push_back(ImVec2(xOf(frames[(size_t)i]), yGhost(vals[(size_t)i])));
                dl->AddPolyline(pts.data(), (int)pts.size(), WithAlpha(chans[(size_t)ch].color, 0.35f), 0, Dp(1.5f));
            }
            // the active channel's value curve
            {
                std::vector<ImVec2> pts;
                pts.reserve((size_t)sampleCount);
                for (int i = 0; i < sampleCount; ++i) pts.push_back(ImVec2(xOf(frames[(size_t)i]), yOf(activeVals[(size_t)i])));
                dl->AddPolyline(pts.data(), (int)pts.size(), chans[(size_t)active].color, 0, Dp(1.5f));
                // a small filled dot on the active curve at each of the parent row's key frames
                for (const TimelineKey& k : parent.keys) {
                    const float kx = xOf((float)k.frame);
                    if (kx < x0 - Dp(2.0f) || kx > x1 + Dp(2.0f)) continue;
                    dl->AddCircleFilled(ImVec2(kx, yOf(view.graphSample(parent.id, active, (float)k.frame))), Dp(2.5f),
                                        chans[(size_t)active].color);
                }
            }
            // the editable segment's Bezier handles, on top of the curves
            {
                const LaneHandles h = laneHandlesOf(*lanePlot, parent);
                if (h.ok) {
                    const ImU32 col = chans[(size_t)active].color;
                    const bool drag1 = view.graphDrag == 1 && view.graphDragRow == parent.id;
                    const bool drag2 = view.graphDrag == 2 && view.graphDragRow == parent.id;
                    ImVec2 p1 = h.p1, p2 = h.p2;
                    if (drag1 || drag2) {
                        // while dragging, the handle sits on the bytes the drag reports (not the row's previous frame)
                        uint8_t bytes[4];
                        laneDragBytes(parent, h, mousePos, bytes);
                        const ImVec2 q = GraphHandlePoint(h.xa, h.xb, h.ya, h.boxY1, bytes[drag1 ? 0 : 2], bytes[drag1 ? 1 : 3]);
                        (drag1 ? p1 : p2) = q;
                    }
                    dl->AddRect(ImVec2(std::min(h.xa, h.xb), std::min(h.ya, h.boxY1)),
                                ImVec2(std::max(h.xa, h.xb), std::max(h.ya, h.boxY1)), WithAlpha(p.line, 0.6f));
                    dl->AddLine(ImVec2(h.xa, h.ya), p1, WithAlpha(col, 0.6f), Dp(1.0f));
                    dl->AddLine(ImVec2(h.xb, h.yb), p2, WithAlpha(col, 0.6f), Dp(1.0f));
                    const LanePlot* hoverPlot = nullptr;
                    const int hoverHandle = view.graphDrag == 0 ? laneHandleUnderMouse(parentIndex, hoverPlot) : 0;
                    const float r = Dp(4.5f), rHover = Dp(6.0f);
                    const float r1 = drag1 || hoverHandle == 1 ? rHover : r;
                    const float r2 = drag2 || hoverHandle == 2 ? rHover : r;
                    dl->AddCircleFilled(p1, r1, col);
                    dl->AddCircle(p1, r1, p.surface);
                    dl->AddCircleFilled(p2, r2, col);
                    dl->AddCircle(p2, r2, p.surface);
                }
            }
            dl->PopClipRect();
            // speed curve (|d value / d frame| in units per second), auto-scaled from 0, inside its own sub-lane
            dl->PushClipRect(ImVec2(x0, std::max(clipTop, speedTop)), ImVec2(x1, std::min(clipBottom, speedBottom)), true);
            {
                std::vector<ImVec2> pts;
                pts.reserve((size_t)sampleCount);
                for (int i = 0; i < sampleCount; ++i) {
                    float s = 0.0f;
                    if (i == 0) {
                        const float df = frames[1] - frames[0];
                        s = df > 0.0f ? std::abs(activeVals[1] - activeVals[0]) / df : 0.0f;
                    } else {
                        const float df = frames[(size_t)i] - frames[(size_t)(i - 1)];
                        s = df > 0.0f ? std::abs(activeVals[(size_t)i] - activeVals[(size_t)(i - 1)]) / df : 0.0f;
                    }
                    pts.push_back(ImVec2(xOf(frames[(size_t)i]), ySpeed(s * kGraphLaneFps)));
                }
                dl->AddPolyline(pts.data(), (int)pts.size(), p.warn, 0, Dp(1.5f));
            }
            dl->PopClipRect();

            // label column: sub-lane names, min / max numbers, channel chips
            dl->PushClipRect(ImVec2(bb.Min.x, std::max(clipTop, blockTop)), ImVec2(keyAreaX, std::min(clipBottom, speedBottom)), true);
            const float captionH = Dp(size::Caption * 1.25f);
            const float colMid = bb.Min.x + colWidth * 0.5f;
            char vminBuf[32], vmaxBuf[32], smaxBuf[32];
            std::snprintf(vminBuf, sizeof(vminBuf), "%.1f", dataMin);
            std::snprintf(vmaxBuf, sizeof(vmaxBuf), "%.1f", dataMax);
            std::snprintf(smaxBuf, sizeof(smaxBuf), "%.1f/s", speedPeak);
            const float numRight = keyAreaX - Dp(4.0f);
            Text(dl, Font::Regular, size::Caption, ImVec2(numRight - TextSize(Font::Regular, size::Caption, vmaxBuf).x, valueTop + Dp(1.0f)),
                 p.ink3, vmaxBuf);
            Text(dl, Font::Regular, size::Caption, ImVec2(numRight - TextSize(Font::Regular, size::Caption, vminBuf).x, valueBottom - captionH - Dp(1.0f)),
                 p.ink3, vminBuf);
            Text(dl, Font::Regular, size::Caption, ImVec2(numRight - TextSize(Font::Regular, size::Caption, smaxBuf).x, speedTop + Dp(1.0f)),
                 p.ink3, smaxBuf);
            const std::vector<ImRect> chips = chipRectsOf(parentIndex);
            float chipsRight = bb.Min.x + Dp(6.0f);
            for (size_t i = 0; i < chips.size(); ++i) {
                const ImRect& chip = chips[i];
                if (chip.GetWidth() <= 0.0f) continue;
                chipsRight = std::max(chipsRight, chip.Max.x);
                const bool sel = (int)i == active;
                dl->AddRectFilled(chip.Min, chip.Max, sel ? p.accentSoft : WithAlpha(p.ink, 0.05f), Dp(3.0f));
                if (sel) dl->AddRect(chip.Min, chip.Max, WithAlpha(p.accent, 0.7f), Dp(3.0f));
                dl->AddCircleFilled(ImVec2(chip.Min.x + Dp(5.5f), (chip.Min.y + chip.Max.y) * 0.5f), Dp(2.5f),
                                    chans[i].color ? chans[i].color : p.ink3);
                Text(dl, Font::Regular, size::Caption - 1.0f, ImVec2(chip.Min.x + Dp(9.0f), chip.Min.y + Dp(1.0f)),
                     sel ? p.accentInk : p.ink2, chans[i].label.c_str());
            }
            {   // "값" centred in the label column between the chips and the numbers, "속력" in the speed sub-lane
                const char* valueLabel = Tr("값");
                const float vw = TextSize(Font::Regular, size::Caption, valueLabel).x;
                const float left = chipsRight + Dp(4.0f), right = numRight - Dp(34.0f);
                const float vx = std::clamp(colMid - vw * 0.5f, left, std::max(left, right - vw));
                Text(dl, Font::Regular, size::Caption, ImVec2(vx, (valueTop + valueBottom - captionH) * 0.5f), p.ink3, valueLabel);
                const char* speedLabel = Tr("속력");
                const float sw = TextSize(Font::Regular, size::Caption, speedLabel).x;
                Text(dl, Font::Regular, size::Caption, ImVec2(std::max(bb.Min.x + Dp(4.0f), colMid - sw * 0.5f),
                                                             (speedTop + speedBottom - captionH) * 0.5f),
                     p.ink3, speedLabel);
            }
            dl->PopClipRect();
        }
    }

    // Ruler
    dl->AddRectFilled(bb.Min, ImVec2(bb.Max.x, contentY), p.surface);
    dl->AddLine(ImVec2(bb.Min.x, contentY), ImVec2(bb.Max.x, contentY), p.lineStrong);

    if (view.rangeStart >= 0 && view.rangeEnd >= view.rangeStart) {
        float xa = originX + (view.rangeStart - view.scrollFrame) * view.pxPerFrame * Dpi() - 0.5f * view.pxPerFrame * Dpi();
        float xb = originX + (view.rangeEnd - view.scrollFrame) * view.pxPerFrame * Dpi() + 0.5f * view.pxPerFrame * Dpi();
        float cxa = std::max(keyAreaX, xa);
        float cxb = std::min(bb.Max.x, xb);
        if (cxa <= cxb) {
            dl->AddRectFilled(ImVec2(cxa, bb.Min.y), ImVec2(cxb, contentY), WithAlpha(p.accent, 0.22f));
            dl->AddRectFilled(ImVec2(cxa, contentY), ImVec2(cxb, bb.Max.y - sbH), WithAlpha(p.accent, 0.07f));
        }
        if (xa >= keyAreaX && xa <= bb.Max.x) {
            dl->AddLine(ImVec2(xa, bb.Min.y), ImVec2(xa, bb.Max.y - sbH), WithAlpha(p.accent, 0.6f));
        }
        if (xb >= keyAreaX && xb <= bb.Max.x) {
            dl->AddLine(ImVec2(xb, bb.Min.y), ImVec2(xb, bb.Max.y - sbH), WithAlpha(p.accent, 0.6f));
        }
    }

    float minTickPx = Dp(60.0f);
    int steps[] = {1, 2, 5, 10, 15, 30, 60, 150, 300, 900, 1800, 3600, 9000, 18000};
    int tickStep = 18000;
    for (int step : steps) {
        if (step * view.pxPerFrame * Dpi() >= minTickPx) {
            tickStep = step;
            break;
        }
    }
    
    int startFrame = std::max(0, (int)std::floor(view.scrollFrame));
    int endFrame = startFrame + (int)std::ceil(keyAreaW / (view.pxPerFrame * Dpi())) + 1;
    
    dl->PushClipRect(ImVec2(keyAreaX, bb.Min.y), bb.Max, true);
    for (int f = (startFrame / tickStep) * tickStep; f <= endFrame; f += tickStep) {
        float x = originX + (f - view.scrollFrame) * view.pxPerFrame * Dpi();
        if (x < keyAreaX || x > bb.Max.x) continue;
        
        dl->AddLine(ImVec2(x, contentY - Dp(6.0f)), ImVec2(x, contentY), p.ink3);
        
        char fstr[32]; snprintf(fstr, sizeof(fstr), "%d", f);
        char sstr[32]; 
        int secs = f / 30;
        int frames = f % 30;
        if (secs >= 60) snprintf(sstr, sizeof(sstr), "%d:%02d.%02d", secs / 60, secs % 60, (frames * 100) / 30);
        else snprintf(sstr, sizeof(sstr), "%d.%02ds", secs, (frames * 100) / 30);
        
        Text(dl, Font::Regular, size::Caption, ImVec2(std::round(x + Dp(4.0f)), std::round(bb.Min.y + Dp(2.0f))), p.ink, fstr);
        Text(dl, Font::Regular, size::Caption - 2.0f, ImVec2(std::round(x + Dp(4.0f)), std::round(bb.Min.y + Dp(14.0f))), p.ink3, sstr);
    }
    
    dl->PopClipRect();
    dl->PushClipRect(ImVec2(keyAreaX, contentY), bb.Max, true);
    auto drawDiamond = [&](float x, float y, ImU32 col, float radius) {
        ImVec2 pts[4] = {
            ImVec2(x, y - radius), ImVec2(x + radius, y),
            ImVec2(x, y + radius), ImVec2(x - radius, y)
        };
        dl->AddConvexPolyFilled(pts, 4, col);
    };
    
    float f0 = view.scrollFrame - 12.0f / std::max(view.pxPerFrame, 0.01f);
    float f1 = view.scrollFrame + keyAreaW / (view.pxPerFrame * Dpi()) + 2.0f;
    for (int ri = firstRow; ri <= lastRow; ++ri) {
        const auto& r = rows[ri];
        float rowY = contentY - view.scrollY + ri * rowHeight + rowHeight * 0.5f;
        
        auto it = std::lower_bound(r.keys.begin(), r.keys.end(), (int)std::floor(f0),
            [](const TimelineKey& a, int b) { return a.frame < b; });
        for (; it != r.keys.end() && it->frame <= std::ceil(f1); ++it) {
            const auto& k = *it;
            float kx = originX + (k.frame - view.scrollFrame) * view.pxPerFrame * Dpi();
            
            float radius = r.isGroup ? std::min(Dp(4.0f), std::max(Dp(2.0f), view.pxPerFrame * Dpi() * 0.7f)) 
                                     : std::min(Dp(6.5f), std::max(Dp(2.5f), view.pxPerFrame * Dpi() * 1.1f));
            ImU32 col = p.ink2;
            if (!r.keysEditable) col = WithAlpha(p.ink3, 0.8f);
            else if (k.selected) col = p.accent;
            
            const bool dragged = k.selected && r.keysEditable && view.dragMode == 2 && view.dragDelta != 0;
            if (!dragged) drawDiamond(std::round(kx), std::round(rowY), col, radius);
            if (uiprobe::Enabled() && !r.isGroup) {   // MCP: "##tlkey:<row>:<frame>"
                const std::string kid = "##tlkey:" + r.label + ":" + std::to_string(k.frame);
                uiprobe::Add(kid.c_str(), r.label.c_str(), ImVec2(kx - radius, rowY - radius), ImVec2(kx + radius, rowY + radius));
            }

            if (dragged) {
                float gx = originX + (k.frame + view.dragDelta - view.scrollFrame) * view.pxPerFrame * Dpi();
                drawDiamond(std::round(gx), std::round(rowY), p.accent, radius);
                ImVec2 pts[4] = {
                    ImVec2(std::round(kx), std::round(rowY - radius)), ImVec2(std::round(kx + radius), std::round(rowY)),
                    ImVec2(std::round(kx), std::round(rowY + radius)), ImVec2(std::round(kx - radius), std::round(rowY))
                };
                dl->AddPolyline(pts, 4, WithAlpha(p.accent, 0.5f), ImDrawFlags_Closed, Dp(1.0f));
            }
        }
    }
    
    dl->PopClipRect();
    dl->PushClipRect(ImVec2(keyAreaX, bb.Min.y), bb.Max, true);
    float phx = std::round(originX + (currentFrame - view.scrollFrame) * view.pxPerFrame * Dpi());
    if (phx >= keyAreaX && phx <= bb.Max.x) {
        dl->AddLine(ImVec2(phx, bb.Min.y), ImVec2(phx, bb.Max.y), p.accent, std::max(1.0f, std::round(Dp(1.5f))));
        ImVec2 hpts[3] = {
            ImVec2(phx - Dp(5.0f), bb.Min.y), ImVec2(phx + Dp(5.0f), bb.Min.y),
            ImVec2(phx, bb.Min.y + Dp(6.0f))
        };
        dl->AddConvexPolyFilled(hpts, 3, p.accent);
    }
    
    if (view.dragMode == 3) {
        // Box drawn from the content-coordinate anchor: its origin corner stays on the frame/row where the
        // drag began even while the view auto-scrolls under the mouse.
        float anchorX = originX + (view.boxAnchorFrame - view.scrollFrame) * view.pxPerFrame * Dpi();
        float anchorY = contentY + view.boxAnchorY - view.scrollY;
        ImVec2 b0(std::min(anchorX, mousePos.x), std::min(anchorY, mousePos.y));
        ImVec2 b1(std::max(anchorX, mousePos.x), std::max(anchorY, mousePos.y));
        dl->AddRectFilled(b0, b1, WithAlpha(p.accent, 0.2f));
        dl->AddRect(b0, b1, p.accent);
    }
    
    dl->PopClipRect();
    
    dl->AddLine(ImVec2(keyAreaX, bb.Min.y), ImVec2(keyAreaX, bb.Max.y), p.lineStrong);
    
    if (maxScrollY > 0) {
        float indH = std::max(Dp(20.0f), contentH * (contentH / contentHeightAll));
        float indY = contentY + (view.scrollY / maxScrollY) * (contentH - indH);
        dl->AddRectFilled(ImVec2(bb.Max.x - Dp(4.0f), indY), ImVec2(bb.Max.x, indY + indH), WithAlpha(p.ink3, 0.5f), Dp(2.0f));
    }
    
    int T = maxFrame + 120;
    float V = visibleFrames;
    dl->AddRectFilled(ImVec2(keyAreaX, bb.Max.y - sbH), bb.Max, WithAlpha(p.sunken, 1.0f));
    if (T > V) {
        float tw = std::max(Dp(24.0f), keyAreaW * V / T);
        float tx = keyAreaX + (view.scrollFrame / maxScrollFrame) * (keyAreaW - tw);
        bool isThumbHovered = hovered && mousePos.x >= tx && mousePos.x <= tx + tw && mousePos.y >= bb.Max.y - sbH && mousePos.y <= bb.Max.y;
        dl->AddRectFilled(ImVec2(tx, bb.Max.y - sbH + Dp(2.0f)), ImVec2(tx + tw, bb.Max.y - Dp(2.0f)), 
                          WithAlpha(p.ink3, (view.dragMode == 5 || isThumbHovered) ? 0.85f : 0.55f), Dp(4.0f));
    }
    
    dl->PopClipRect();
    
    return changed;
}

} // namespace mmdx::studio
