import Darwin
import Foundation

final class QMPClient {
    private var socketFD: Int32 = -1
    private let lock = NSLock()
    private var nextID = 1

    func connect(path: String) throws {
        var lastError: Error = QMPError.connection("QEMU monitor did not start")
        for _ in 0..<40 {
            do {
                let fd = try openSocket(path: path)
                socketFD = fd
                _ = try readJSONLine(fd) // QMP greeting
                _ = try execute("qmp_capabilities")
                return
            } catch {
                lastError = error
                close()
                Thread.sleep(forTimeInterval: 0.25)
            }
        }
        throw lastError
    }

    func pause() throws { _ = try execute("stop") }
    func resume() throws { _ = try execute("cont") }

    func saveSnapshot(named name: String) throws {
        try monitor("savevm \(name)")
    }

    func loadSnapshot(named name: String) throws {
        try monitor("loadvm \(name)")
    }

    func quit() {
        _ = try? execute("quit")
        close()
    }

    func close() {
        lock.lock()
        let fd = socketFD
        socketFD = -1
        lock.unlock()
        if fd >= 0 {
            _ = Darwin.shutdown(fd, SHUT_RDWR)
            Darwin.close(fd)
        }
    }

    private func monitor(_ command: String) throws {
        let response = try execute("human-monitor-command", arguments: ["command-line": command])
        if let text = response as? String, text.localizedCaseInsensitiveContains("error:") {
            throw QMPError.command(text.trimmingCharacters(in: .whitespacesAndNewlines))
        }
    }

    private func execute(_ command: String, arguments: [String: Any] = [:]) throws -> Any? {
        lock.lock()
        defer { lock.unlock() }
        guard socketFD >= 0 else { throw QMPError.connection("QEMU monitor is disconnected") }
        let id = nextID
        nextID += 1
        var request: [String: Any] = ["execute": command, "id": id]
        if !arguments.isEmpty { request["arguments"] = arguments }
        var payload = try JSONSerialization.data(withJSONObject: request)
        payload.append(0x0a)
        try writeAll(socketFD, payload)
        while true {
            let response = try readJSONLine(socketFD)
            if let responseID = response["id"] as? Int, responseID == id {
                if let error = response["error"] as? [String: Any] {
                    throw QMPError.command(error["desc"] as? String ?? "QEMU monitor command failed")
                }
                return response["return"]
            }
        }
    }

    private func openSocket(path: String) throws -> Int32 {
        let fd = Darwin.socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw QMPError.connection("Could not create QEMU monitor socket") }
        var address = sockaddr_un()
        address.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        address.sun_family = sa_family_t(AF_UNIX)
        let bytes = Array(path.utf8CString)
        guard bytes.count <= MemoryLayout.size(ofValue: address.sun_path) else {
            Darwin.close(fd)
            throw QMPError.connection("QEMU monitor socket path is too long")
        }
        withUnsafeMutableBytes(of: &address.sun_path) { destination in
            bytes.withUnsafeBytes { source in destination.copyBytes(from: source) }
        }
        let result = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard result == 0 else {
            Darwin.close(fd)
            throw QMPError.connection("Could not connect to QEMU monitor")
        }
        return fd
    }

    private func readJSONLine(_ fd: Int32) throws -> [String: Any] {
        var bytes = [UInt8]()
        while bytes.count < 1_048_576 {
            var byte: UInt8 = 0
            let count = Darwin.recv(fd, &byte, 1, 0)
            guard count == 1 else { throw QMPError.connection("QEMU monitor connection closed") }
            if byte == 0x0a { break }
            if byte != 0x0d { bytes.append(byte) }
        }
        guard let value = try JSONSerialization.jsonObject(with: Data(bytes)) as? [String: Any] else {
            throw QMPError.connection("Invalid response from QEMU monitor")
        }
        return value
    }

    private func writeAll(_ fd: Int32, _ data: Data) throws {
        var offset = 0
        while offset < data.count {
            let count = data.withUnsafeBytes { bytes in
                Darwin.send(fd, bytes.baseAddress!.advanced(by: offset), data.count - offset, 0)
            }
            guard count > 0 else { throw QMPError.connection("Could not write to QEMU monitor") }
            offset += count
        }
    }
}

private enum QMPError: LocalizedError {
    case connection(String)
    case command(String)
    var errorDescription: String? {
        switch self {
        case .connection(let text), .command(let text): text
        }
    }
}
