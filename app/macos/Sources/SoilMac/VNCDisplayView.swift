import AppKit
import SwiftUI

struct VNCDisplayView: NSViewRepresentable {
    @ObservedObject var client: VNCClient

    func makeNSView(context: Context) -> VNCFramebufferView {
        let view = VNCFramebufferView()
        view.client = client
        return view
    }

    func updateNSView(_ nsView: VNCFramebufferView, context: Context) {
        nsView.client = client
        nsView.image = client.image
    }
}

final class VNCFramebufferView: NSView {
    weak var client: VNCClient?
    var image: CGImage? { didSet { needsDisplay = true } }
    private var buttons: UInt8 = 0

    override var acceptsFirstResponder: Bool { true }

    override func draw(_ dirtyRect: NSRect) {
        NSColor.black.setFill()
        NSBezierPath(rect: bounds).fill()
        guard let image else { return }
        let imageSize = NSSize(width: image.width, height: image.height)
        let scale = min(bounds.width / imageSize.width, bounds.height / imageSize.height)
        let size = NSSize(width: imageSize.width * scale, height: imageSize.height * scale)
        let rect = NSRect(x: (bounds.width - size.width) / 2, y: (bounds.height - size.height) / 2,
                          width: size.width, height: size.height)
        NSGraphicsContext.current?.imageInterpolation = .none
        NSImage(cgImage: image, size: imageSize).draw(in: rect)
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        buttons |= 1
        sendPointer(event)
    }

    override func mouseUp(with event: NSEvent) {
        buttons &= ~1
        sendPointer(event)
    }

    override func rightMouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        buttons |= 4
        sendPointer(event)
    }

    override func rightMouseUp(with event: NSEvent) {
        buttons &= ~4
        sendPointer(event)
    }

    override func otherMouseDown(with event: NSEvent) {
        buttons |= 2
        sendPointer(event)
    }

    override func otherMouseUp(with event: NSEvent) {
        buttons &= ~2
        sendPointer(event)
    }

    override func mouseDragged(with event: NSEvent) { sendPointer(event) }
    override func rightMouseDragged(with event: NSEvent) { sendPointer(event) }
    override func otherMouseDragged(with event: NSEvent) { sendPointer(event) }

    override func keyDown(with event: NSEvent) { sendKey(event, down: true) }
    override func keyUp(with event: NSEvent) { sendKey(event, down: false) }

    private func sendPointer(_ event: NSEvent) {
        guard let image else { return }
        let size = NSSize(width: image.width, height: image.height)
        let scale = min(bounds.width / size.width, bounds.height / size.height)
        let drawnWidth = size.width * scale
        let drawnHeight = size.height * scale
        let originX = (bounds.width - drawnWidth) / 2
        let originY = (bounds.height - drawnHeight) / 2
        let point = convert(event.locationInWindow, from: nil)
        let x = min(max(Int((point.x - originX) / scale), 0), image.width - 1)
        let y = min(max(Int((drawnHeight - (point.y - originY)) / scale), 0), image.height - 1)
        client?.sendPointer(x: UInt16(x), y: UInt16(y), buttons: buttons)
    }

    private func sendKey(_ event: NSEvent, down: Bool) {
        if let special = Self.keysyms[event.keyCode] {
            client?.sendKey(keySym: special, down: down)
            return
        }
        guard let scalar = event.charactersIgnoringModifiers?.unicodeScalars.first else { return }
        let value = scalar.value
        let keySym = value <= 0xff ? value : (0x0100_0000 | value)
        client?.sendKey(keySym: keySym, down: down)
    }

    private static let keysyms: [UInt16: UInt32] = [
        36: 0xff0d, 48: 0xff09, 49: 0x20, 51: 0xff08, 53: 0xff1b,
        123: 0xff51, 124: 0xff53, 125: 0xff54, 126: 0xff52,
        115: 0xff50, 119: 0xff57, 116: 0xff55, 121: 0xff56,
        122: 0xffbe, 120: 0xffbf, 99: 0xffc0, 118: 0xffc1,
        96: 0xffc2, 97: 0xffc3, 98: 0xffc4, 100: 0xffc5,
        101: 0xffc6, 109: 0xffc7, 103: 0xffc8, 111: 0xffc9,
        114: 0xffe5, // Caps Lock
        56: 0xffe1, 60: 0xffe2, 59: 0xffe3, 62: 0xffe4,
        58: 0xffe9, 61: 0xffea, 55: 0xffeb, 54: 0xffec
    ]
}
