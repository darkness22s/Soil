import CoreGraphics
import Combine
import Foundation
import Darwin

final class VNCClient: ObservableObject {
    @Published private(set) var image: CGImage?
    @Published private(set) var isConnected = false
    @Published private(set) var error: String?

    private var socketFD: Int32 = -1
    private let sendLock = NSLock()

    func connect(socketPath: String) {
        disconnect()
        error = nil
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else { return }
            var connectionFD: Int32 = -1
            do {
                var fd: Int32 = -1
                var lastError: Error = VNCError.socket("QEMU's VNC display did not start")
                for _ in 0..<40 {
                    do {
                        fd = try Self.openSocket(path: socketPath)
                        break
                    } catch {
                        lastError = error
                        Thread.sleep(forTimeInterval: 0.25)
                    }
                }
                guard fd >= 0 else { throw lastError }
                connectionFD = fd
                self.sendLock.lock()
                self.socketFD = fd
                self.sendLock.unlock()
                let (width, height) = try self.handshake(fd)
                Task { @MainActor in self.isConnected = true }
                self.readUpdates(fd, width: width, height: height)
            } catch {
                Task { @MainActor in
                    self.isConnected = false
                    self.error = error.localizedDescription
                }
            }
            self.closeSocket(fd: connectionFD)
            Task { @MainActor in self.isConnected = false }
        }
    }

    func disconnect() {
        closeSocket()
        isConnected = false
        image = nil
    }

    func sendKey(keySym: UInt32, down: Bool) {
        var msg = Data([4, down ? 1 : 0, 0, 0])
        msg.appendBE(keySym)
        send(msg)
    }

    func sendPointer(x: UInt16, y: UInt16, buttons: UInt8) {
        var msg = Data([5, buttons])
        msg.appendBE(x)
        msg.appendBE(y)
        send(msg)
    }

    private func handshake(_ fd: Int32) throws -> (width: Int, height: Int) {
        _ = try readExact(fd, count: 12) // server protocol version
        try writeAll(fd, data: Data("RFB 003.008\n".utf8))
        let securityCount = Int(try readExact(fd, count: 1)[0])
        guard securityCount > 0 else { throw VNCError.protocolError("QEMU rejected the VNC connection") }
        let securityTypes = try readExact(fd, count: securityCount)
        guard securityTypes.contains(1) else { throw VNCError.protocolError("QEMU offered no unauthenticated loopback VNC mode") }
        try writeAll(fd, data: Data([1]))
        let securityResult = try readExact(fd, count: 4).readBE32(at: 0)
        guard securityResult == 0 else { throw VNCError.protocolError("VNC security negotiation failed") }
        try writeAll(fd, data: Data([1])) // shared session

        let dimensions = try readExact(fd, count: 4)
        let width = Int(dimensions.readBE16(at: 0))
        let height = Int(dimensions.readBE16(at: 2))
        guard width > 0, height > 0, width <= 4096, height <= 4096 else {
            throw VNCError.protocolError("QEMU returned invalid framebuffer dimensions")
        }
        _ = try readExact(fd, count: 16) // server pixel format
        let nameLength = Int(try readExact(fd, count: 4).readBE32(at: 0))
        guard nameLength <= 4096 else { throw VNCError.protocolError("Invalid VNC desktop name") }
        _ = try readExact(fd, count: nameLength)

        // 32-bit little-endian true-colour pixels, with RGB in the low 24 bits.
        var pixelFormat = Data([32, 24, 0, 1])
        pixelFormat.appendBE(UInt16(255))
        pixelFormat.appendBE(UInt16(255))
        pixelFormat.appendBE(UInt16(255))
        pixelFormat.append(contentsOf: [16, 8, 0, 0, 0, 0])
        try writeAll(fd, data: Data([0, 0, 0, 0]) + pixelFormat) // SetPixelFormat
        try writeAll(fd, data: Data([2, 0, 0, 1, 0, 0, 0, 0])) // SetEncodings: Raw
        try writeAll(fd, data: framebufferRequest(width: width, height: height, incremental: false))
        return (width, height)
    }

    private func readUpdates(_ fd: Int32, width screenWidth: Int, height screenHeight: Int) {
        var framebuffer = Data(count: screenWidth * screenHeight * 4)
        while true {
            do {
                let type = try readExact(fd, count: 1)[0]
                switch type {
                case 0:
                    _ = try readExact(fd, count: 1)
                    let count = Int(try readExact(fd, count: 2).readBE16(at: 0))
                    for _ in 0..<count {
                        let header = try readExact(fd, count: 12)
                        let x = Int(header.readBE16(at: 0))
                        let y = Int(header.readBE16(at: 2))
                        let width = Int(header.readBE16(at: 4))
                        let height = Int(header.readBE16(at: 6))
                        let encoding = Int32(bitPattern: header.readBE32(at: 8))
                        guard encoding == 0 else { throw VNCError.protocolError("Unsupported QEMU framebuffer encoding: \(encoding)") }
                        guard width > 0, height > 0, width * height <= 16_777_216,
                              x >= 0, y >= 0, x + width <= screenWidth, y + height <= screenHeight else {
                            throw VNCError.protocolError("Invalid framebuffer rectangle")
                        }
                        let raw = try readExact(fd, count: width * height * 4)
                        raw.withUnsafeBytes { source in
                            framebuffer.withUnsafeMutableBytes { destination in
                                guard let src = source.baseAddress?.assumingMemoryBound(to: UInt8.self),
                                      let dst = destination.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
                                for row in 0..<height {
                                    for col in 0..<width {
                                        let srcOffset = (row * width + col) * 4
                                        let dstOffset = ((y + row) * screenWidth + x + col) * 4
                                        dst[dstOffset] = src[srcOffset + 2]
                                        dst[dstOffset + 1] = src[srcOffset + 1]
                                        dst[dstOffset + 2] = src[srcOffset]
                                        dst[dstOffset + 3] = 255
                                    }
                                }
                            }
                        }
                        publish(framebuffer, width: screenWidth, height: screenHeight)
                    }
                    send(framebufferRequest(width: screenWidth, height: screenHeight, incremental: true))
                case 2:
                    break // bell
                case 3:
                    _ = try readExact(fd, count: 3)
                    let length = Int(try readExact(fd, count: 4).readBE32(at: 0))
                    guard length <= 1_048_576 else { throw VNCError.protocolError("Oversized VNC clipboard payload") }
                    _ = try readExact(fd, count: length)
                default:
                    throw VNCError.protocolError("Unknown VNC message type \(type)")
                }
            } catch {
                Task { @MainActor in self.error = error.localizedDescription }
                return
            }
        }
    }

    private func publish(_ rgba: Data, width: Int, height: Int) {
        guard let provider = CGDataProvider(data: rgba as CFData),
              let cgImage = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                                    bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(),
                                    bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                                    provider: provider, decode: nil, shouldInterpolate: false,
                                    intent: .defaultIntent) else { return }
        Task { @MainActor in self.image = cgImage }
    }

    private func framebufferRequest(width: Int, height: Int, incremental: Bool) -> Data {
        var data = Data([3, incremental ? 1 : 0])
        data.appendBE(UInt16(0))
        data.appendBE(UInt16(0))
        data.appendBE(UInt16(min(width, 65535)))
        data.appendBE(UInt16(min(height, 65535)))
        return data
    }

    private func send(_ data: Data) {
        sendLock.lock()
        defer { sendLock.unlock() }
        guard socketFD >= 0 else { return }
        try? writeAll(socketFD, data: data)
    }

    private func closeSocket(fd expectedFD: Int32) {
        guard expectedFD >= 0 else { return }
        sendLock.lock()
        guard socketFD == expectedFD else {
            sendLock.unlock()
            return
        }
        socketFD = -1
        sendLock.unlock()
        _ = Darwin.shutdown(expectedFD, SHUT_RDWR)
        Darwin.close(expectedFD)
    }

    private func closeSocket() {
        sendLock.lock()
        let fd = socketFD
        socketFD = -1
        sendLock.unlock()
        if fd >= 0 {
            _ = Darwin.shutdown(fd, SHUT_RDWR)
            Darwin.close(fd)
        }
    }

    private static func openSocket(path: String) throws -> Int32 {
        let fd = Darwin.socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw VNCError.socket("Could not create local VNC socket") }
        var address = sockaddr_un()
        address.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        address.sun_family = sa_family_t(AF_UNIX)
        let pathBytes = Array(path.utf8CString)
        let pathCapacity = MemoryLayout.size(ofValue: address.sun_path)
        guard pathBytes.count <= pathCapacity else {
            Darwin.close(fd)
            throw VNCError.socket("The temporary VNC socket path is too long")
        }
        withUnsafeMutableBytes(of: &address.sun_path) { destination in
            pathBytes.withUnsafeBytes { source in destination.copyBytes(from: source) }
        }
        let result = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard result == 0 else {
            Darwin.close(fd)
            throw VNCError.socket("Could not connect to QEMU's private display socket")
        }
        return fd
    }

    private func readExact(_ fd: Int32, count: Int) throws -> Data {
        var result = Data(count: count)
        var offset = 0
        while offset < count {
            let received = result.withUnsafeMutableBytes { bytes in
                Darwin.recv(fd, bytes.baseAddress!.advanced(by: offset), count - offset, 0)
            }
            guard received > 0 else { throw VNCError.socket("QEMU's VNC connection closed") }
            offset += received
        }
        return result
    }

    private func writeAll(_ fd: Int32, data: Data) throws {
        var offset = 0
        while offset < data.count {
            let sent = data.withUnsafeBytes { bytes in
                Darwin.send(fd, bytes.baseAddress!.advanced(by: offset), data.count - offset, 0)
            }
            guard sent > 0 else { throw VNCError.socket("Could not write to QEMU's VNC connection") }
            offset += sent
        }
    }
}

private enum VNCError: LocalizedError {
    case socket(String)
    case protocolError(String)
    var errorDescription: String? {
        switch self {
        case .socket(let text), .protocolError(let text): text
        }
    }
}

private extension Data {
    mutating func appendBE(_ value: UInt16) {
        append(UInt8((value >> 8) & 0xff))
        append(UInt8(value & 0xff))
    }

    mutating func appendBE(_ value: UInt32) {
        append(UInt8((value >> 24) & 0xff))
        append(UInt8((value >> 16) & 0xff))
        append(UInt8((value >> 8) & 0xff))
        append(UInt8(value & 0xff))
    }

    func readBE16(at offset: Int) -> UInt16 {
        (UInt16(self[offset]) << 8) | UInt16(self[offset + 1])
    }

    func readBE32(at offset: Int) -> UInt32 {
        (UInt32(self[offset]) << 24) | (UInt32(self[offset + 1]) << 16) |
        (UInt32(self[offset + 2]) << 8) | UInt32(self[offset + 3])
    }
}
