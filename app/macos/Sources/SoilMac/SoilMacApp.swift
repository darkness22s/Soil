import AppKit
import Darwin
import SwiftUI

@MainActor
final class VMController: ObservableObject {
    @Published private(set) var isRunning = false
    @Published private(set) var isIdle = false
    @Published private(set) var monitorReady = false
    @Published private(set) var status = "Stopped"
    @Published private(set) var display = VNCClient()

    private var process: Process?
    private var vncSocketDirectory: String?
    private var monitor: QMPClient?
    private var snapshotMarker: URL?

    func start() {
        guard process == nil else { return }
        do {
            let images = try resolveImages()
            let qemu = try resolveQemu()
            let disk = try prepareDisk(images: images, qemu: qemu)
            let machineDirectory = try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask,
                                                               appropriateFor: nil, create: true)
                .appendingPathComponent("Soil/machines/default", isDirectory: true)
            try FileManager.default.createDirectory(at: machineDirectory, withIntermediateDirectories: true)
            let marker = machineDirectory.appendingPathComponent("snapshot.marker")
            snapshotMarker = marker
            let vm = Process()
            let socketDirectory = "/tmp/soil-vnc-\(UUID().uuidString)"
            try FileManager.default.createDirectory(atPath: socketDirectory, withIntermediateDirectories: false,
                                                    attributes: [.posixPermissions: 0o700])
            let socketPath = "\(socketDirectory)/display.sock"
            let monitorPath = "\(socketDirectory)/monitor.sock"
            vncSocketDirectory = socketDirectory
            vm.executableURL = qemu
            vm.arguments = [
                "-machine", "q35",
                "-accel", "tcg,thread=multi",
                "-cpu", "max",
                "-smp", "1",
                "-m", "1024",
                "-kernel", images.appendingPathComponent("vmlinuz64").path,
                "-initrd", images.appendingPathComponent("core2.gz").path,
                "-append", "console=ttyS0 quiet loglevel=3 acpi=off noapic nolapic mitigations=off nozswap",
                "-drive", "file=\(disk.path),format=qcow2,if=virtio",
                "-display", "none",
                "-vnc", "unix:\(socketPath)",
                "-qmp", "unix:\(monitorPath),server=on,wait=off",
                "-serial", "null",
                "-monitor", "none",
                "-no-reboot"
            ]
            if ProcessInfo.processInfo.arguments.contains("--smoke-test") {
                vm.standardOutput = FileHandle.standardOutput
                vm.standardError = FileHandle.standardError
            } else {
                vm.standardOutput = FileHandle.nullDevice
                vm.standardError = FileHandle.nullDevice
            }
            vm.terminationHandler = { [weak self] ended in
                Task { @MainActor in
                    self?.process = nil
                    self?.monitor?.close()
                    self?.monitor = nil
                    self?.monitorReady = false
                    self?.display.disconnect()
                    if let directory = self?.vncSocketDirectory {
                        try? FileManager.default.removeItem(atPath: directory)
                    }
                    self?.vncSocketDirectory = nil
                    self?.isRunning = false
                    self?.isIdle = false
                    self?.status = ended.terminationStatus == 0 ? "Stopped" : "QEMU exited (\(ended.terminationStatus))"
                }
            }
            try vm.run()
            process = vm
            isRunning = true
            status = "Starting headless VM…"
            display.connect(socketPath: socketPath)
            let qmp = QMPClient()
            monitor = qmp
            DispatchQueue.global(qos: .userInitiated).async { [weak self] in
                do {
                    try qmp.connect(path: monitorPath)
                    Task { @MainActor in self?.monitorReady = true }
                    if FileManager.default.fileExists(atPath: marker.path) {
                        try qmp.loadSnapshot(named: "Soil")
                        Task { @MainActor in
                            if self?.process != nil { self?.status = "Restored saved snapshot" }
                        }
                    } else {
                        Task { @MainActor in
                            if self?.process != nil { self?.status = "Running" }
                        }
                    }
                } catch {
                    Task { @MainActor in
                        if self?.process != nil { self?.status = "QEMU monitor unavailable: \(error.localizedDescription)" }
                    }
                }
            }
        } catch {
            if let directory = vncSocketDirectory {
                try? FileManager.default.removeItem(atPath: directory)
            }
            vncSocketDirectory = nil
            status = "Could not start: \(error.localizedDescription)"
        }
    }

    func stop() {
        guard let process else { return }
        guard let monitor, let marker = snapshotMarker else {
            status = "Stopping without snapshot…"
            process.terminate()
            return
        }
        status = "Saving snapshot…"
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            do {
                try monitor.saveSnapshot(named: "Soil")
                try Data("Soil".utf8).write(to: marker, options: .atomic)
                monitor.quit()
            } catch {
                monitor.quit()
                Task { @MainActor in
                    self?.status = "Snapshot failed: \(error.localizedDescription)"
                    process.terminate()
                }
            }
        }
    }

    func setIdle(_ idle: Bool) {
        guard isRunning, isIdle != idle, let monitor else { return }
        DispatchQueue.global(qos: .utility).async { [weak self] in
            do {
                if idle { try monitor.pause() } else { try monitor.resume() }
                Task { @MainActor in
                    self?.isIdle = idle
                    self?.status = idle ? "Paused while inactive" : "Running"
                }
            } catch {
                Task { @MainActor in self?.status = "Could not change idle state: \(error.localizedDescription)" }
            }
        }
    }

    func runSmokeTest() async {
        start()
        let booted = await waitFor(timeout: 180, {
            (self.display.image != nil && self.monitorReady)
                || self.status.hasPrefix("Could not start:")
                || self.status.hasPrefix("QEMU monitor unavailable:")
        })
        guard booted, display.image != nil, monitorReady else {
            smokeTestFailed("guest did not boot and produce a framebuffer")
        }
        print("[smoke] Guest booted and framebuffer received")

        setIdle(true)
        guard await waitFor(timeout: 10, { self.isIdle }) else {
            smokeTestFailed("QEMU did not pause")
        }
        setIdle(false)
        guard await waitFor(timeout: 10, { !self.isIdle }) else {
            smokeTestFailed("QEMU did not resume")
        }
        print("[smoke] QEMU pause/resume passed")

        stop()
        guard await waitFor(timeout: 60, { !self.isRunning }) else {
            smokeTestFailed("QEMU did not stop after saving its snapshot")
        }
        print("[smoke] Snapshot saved")

        start()
        guard await waitFor(timeout: 180, {
            self.display.image != nil && self.status == "Restored saved snapshot"
        }) else {
            smokeTestFailed("saved snapshot was not restored")
        }
        print("[smoke] Snapshot restored")

        stop()
        guard await waitFor(timeout: 60, { !self.isRunning }) else {
            smokeTestFailed("QEMU did not stop after the restore test")
        }
        print("[smoke] PASS")
        Darwin.exit(EXIT_SUCCESS)
    }

    private func waitFor(timeout: TimeInterval, _ condition: () -> Bool) async -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if condition() { return true }
            try? await Task.sleep(nanoseconds: 250_000_000)
        }
        return condition()
    }

    private func smokeTestFailed(_ message: String) -> Never {
        fputs("[smoke] FAIL: \(message) (status: \(status))\n", stderr)
        process?.terminate()
        Darwin.exit(EXIT_FAILURE)
    }

    private func prepareDisk(images: URL, qemu: URL) throws -> URL {
        let manager = FileManager.default
        let machineDirectory = try manager.url(for: .applicationSupportDirectory, in: .userDomainMask,
                                               appropriateFor: nil, create: true)
            .appendingPathComponent("Soil/machines/default", isDirectory: true)
        try manager.createDirectory(at: machineDirectory, withIntermediateDirectories: true)
        let overlay = machineDirectory.appendingPathComponent("disk.qcow2")
        guard !manager.fileExists(atPath: overlay.path) else { return overlay }
        let qemuImg = qemu.deletingLastPathComponent().appendingPathComponent("qemu-img")
        guard manager.isExecutableFile(atPath: qemuImg.path) else {
            throw NSError(domain: "SoilMac", code: 3, userInfo: [NSLocalizedDescriptionKey:
                "qemu-img was not found next to QEMU. Reinstall it with Homebrew: brew install qemu"])
        }
        let create = Process()
        create.executableURL = qemuImg
        create.arguments = ["create", "-f", "qcow2", "-F", "raw", "-b",
                            images.appendingPathComponent("soil.squashfs").path, overlay.path]
        create.standardOutput = FileHandle.nullDevice
        create.standardError = FileHandle.nullDevice
        try create.run()
        create.waitUntilExit()
        guard create.terminationStatus == 0 else {
            throw NSError(domain: "SoilMac", code: 4, userInfo: [NSLocalizedDescriptionKey:
                "Could not create the persistent QEMU disk overlay"])
        }
        return overlay
    }

    private func resolveImages() throws -> URL {
        let manager = FileManager.default
        let support = try manager.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
            .appendingPathComponent("Soil", isDirectory: true)
            .appendingPathComponent("images", isDirectory: true)
        try manager.createDirectory(at: support, withIntermediateDirectories: true)
        let required = ["vmlinuz64", "core2.gz", "soil.squashfs"]
        if required.allSatisfy({ manager.fileExists(atPath: support.appendingPathComponent($0).path) }) {
            return support
        }
        if let bundled = Bundle.main.resourceURL?.appendingPathComponent("images", isDirectory: true),
           required.allSatisfy({ manager.fileExists(atPath: bundled.appendingPathComponent($0).path) }) {
            for name in required where !manager.fileExists(atPath: support.appendingPathComponent(name).path) {
                try manager.copyItem(at: bundled.appendingPathComponent(name), to: support.appendingPathComponent(name))
            }
            return support
        }
        throw NSError(domain: "SoilMac", code: 1, userInfo: [NSLocalizedDescriptionKey:
            "Guest images are missing. Put vmlinuz64, core2.gz, and soil.squashfs in ~/Library/Application Support/Soil/images."])
    }

    private func resolveQemu() throws -> URL {
        let candidates = ["/opt/homebrew/bin/qemu-system-x86_64", "/usr/local/bin/qemu-system-x86_64"]
        if let path = candidates.first(where: { FileManager.default.isExecutableFile(atPath: $0) }) {
            return URL(fileURLWithPath: path)
        }
        throw NSError(domain: "SoilMac", code: 2, userInfo: [NSLocalizedDescriptionKey:
            "QEMU was not found. Install it with Homebrew: brew install qemu"])
    }
}

@main
struct SoilMacApp: App {
    @StateObject private var vm = VMController()

    var body: some Scene {
        WindowGroup {
            ContentView(vm: vm)
                .frame(minWidth: 700, minHeight: 480)
                .task {
                    if ProcessInfo.processInfo.arguments.contains("--smoke-test") {
                        await vm.runSmokeTest()
                    }
                }
        }
        .commands {
            CommandGroup(replacing: .newItem) { }
        }
    }
}

struct ContentView: View {
    @ObservedObject var vm: VMController
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                VStack(alignment: .leading, spacing: 5) {
                    Text("Soil").font(.largeTitle.bold())
                    Text("Apple Silicon · x86_64 guest software emulation")
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Button(vm.isRunning ? "Stop" : "Start") {
                    vm.isRunning ? vm.stop() : vm.start()
                }
                .keyboardShortcut(.defaultAction)
                .controlSize(.large)
            }
            Label(vm.display.isConnected ? (vm.isIdle ? "Paused · display connected" : "Running · display connected") : vm.status,
                  systemImage: vm.display.isConnected ? (vm.isIdle ? "pause.circle.fill" : "play.circle.fill") : "stop.circle")
                .foregroundStyle(vm.display.isConnected && !vm.isIdle ? .green : .secondary)
            Group {
                if vm.display.image != nil {
                    VNCDisplayView(client: vm.display)
                } else {
                    VStack(spacing: 12) {
                        Image(systemName: "display")
                            .font(.system(size: 40))
                            .foregroundStyle(.secondary)
                        Text(vm.display.error ?? "Waiting for the guest display…")
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(Color.black.opacity(0.9))
                    .clipShape(RoundedRectangle(cornerRadius: 8))
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .padding(24)
        .onChange(of: scenePhase) { phase in
            vm.setIdle(phase != .active)
        }
    }
}
