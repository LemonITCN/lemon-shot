import AppKit
import LemonShotCore

/// 截图层各部件的位置与显隐（spec 2.2）。屏幕上的 CaptureView 与自测共用这一份计算。
struct CaptureLayout {
    struct Flags {
        var crosshair = Settings.shared.bool("capture.showCrosshair")
        var magnifier = Settings.shared.bool("capture.showMagnifier")
        var tips = Settings.shared.bool("capture.showTips")
    }

    var crosshair: CGPoint?
    var magnifier: CGRect?
    var pixelInfo: CaptureRender.PixelInfo?
    var sizeTip: CGRect?
    var sizeTipText = ""
    var tips: CGRect?
    var tipsLines: [String] = []
    /// 工具栏本体（不含投影边距）。
    var toolbar: CGRect?

    static func compute(session: Session, state st: ls_session_state, mouse: CGPoint, mouseInView: Bool, mode: CaptureView.Mode,
                        flags: Flags, bounds: CGRect, toolbarSize: CGSize, editingSize: Bool = false) -> CaptureLayout {
        var l = CaptureLayout()
        let scale = session.scale
        let selected = st.has_selection != 0 && st.phase == Int32(LS_PHASE_SELECTED.rawValue)
        let inside = st.mouse_inside != 0 && mouseInView

        // 录屏模式（.record）与截图相同，只是没有放大镜、提示文字不同
        let capLike = mode == .capture || mode == .record
        if capLike && flags.crosshair && !selected && inside { l.crosshair = mouse }

        let magOn = (mode == .colorPicker && inside) || (mode == .capture && flags.magnifier && st.show_magnifier != 0 && inside)
        if magOn {
            l.pixelInfo = CaptureRender.pixelInfo(session: session, at: mouse)
            l.magnifier = session.placeMagnifier(CaptureRender.magnifierSize(showHint: flags.tips)).pixelAligned(scale)
        }

        if capLike, !editingSize, selected || st.dragging_selection != 0 || st.has_hover != 0,
           let text = CaptureRender.sizeTipText(session: session, state: st) {
            l.sizeTipText = text
            l.sizeTip = session.placeSizeTip(CaptureRender.sizeTipSize(text)).pixelAligned(scale)
        }

        if capLike && flags.tips && st.dragging_selection == 0 {
            l.tipsLines = mode == .record ? RecordUI.tipsLines(st) : CaptureRender.tipsLines(st)
            let size = CaptureRender.tipsSize(l.tipsLines)
            l.tips = CGRect(x: bounds.minX + 16, y: bounds.maxY - 16 - size.height, width: size.width, height: size.height)
        }

        if mode != .colorPicker && selected && st.toolbar_visible != 0 && st.dragging_selection == 0 {
            l.toolbar = session.placeToolbar(toolbarSize).pixelAligned(scale)
        }
        return l
    }

    /// 样式面板位置：锚点为工具按钮中心（本屏坐标）。
    static func popover(session: Session, toolbar: CGRect, anchorX: CGFloat, size: CGSize) -> CGRect {
        session.placePopover(toolbar: toolbar, anchorX: anchorX, size: size).pixelAligned(session.scale)
    }

    /// 提示气泡（spec 2.3）：按钮正下方居中，间距 6pt；工具栏在选区上方时放正上方；放不下换另一边，夹在屏幕内。
    static func tooltip(size: CGSize, toolbar: CGRect, button: CGRect, selection: CGRect, bounds: CGRect, scale: CGFloat) -> CGRect {
        let below = toolbar.maxY > selection.minY + 1
        var y = below ? toolbar.maxY + 6 : toolbar.minY - 6 - size.height
        if y + size.height > bounds.maxY { y = toolbar.minY - 6 - size.height }
        if y < bounds.minY { y = toolbar.maxY + 6 }
        let x = max(bounds.minX + 4, min(button.midX - size.width / 2, bounds.maxX - size.width - 4))
        return CGRect(x: x, y: y, width: size.width, height: size.height).pixelAligned(scale)
    }
}
