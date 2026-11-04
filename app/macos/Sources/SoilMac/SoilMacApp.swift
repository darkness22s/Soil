import AppKit
import SwiftUI

@MainActor
final class VMController: ObservableObject {
    @Published private(set) var isRunning = false
    @Published private(set) var status = "Stopped"
    @Published var log = ""

    private var process: Process?
    private var outputPipe: Pipe?

    func start() {
        guard process == nil else { return }
        do {
            let images = try resolveImages()
            let qemu = try resolveQemu()
            let vm = Process()
            let output = Pipe()
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
                "-drive", "file=\(images.appendingPathComponent("soil.squashfs").path),format=raw,if=virtio,readonly=on",
                "-display", "cocoa",
                "-serial", "stdio",
                "-monitor", "none",
                "-no-reboot"
            ]
            vm.standardOutput = output
            vm.standardError = output
            vm.terminationHandler = { [weak self] ended in
                Task { @MainActor in
                    self?.process = nil
                    self?.outputPipe = nil
                    self?.isRunning = false
                    self?.status = ended.terminationStatus == 0 ? "Stopped" : "QEMU exited (\(ended.terminationStatus))"
                }
            }
            output.fileHandleForReading.readabilityHandler = { [weak self] handle in
                let data = handle.availableData
                guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
                Task { @MainActor in
                    self?.log = String((self?.log ?? "") + text.suffix(12000))
                }
            }
            try vm.run()
            process = vm
            outputPipe = output
            isRunning = true
            status = "Running in QEMU (software emulation)"
            log = ""
        } catch {
            status = "Could not start"
            log = error.localizedDescription
        }
    }

    func stop() {
        guard let process else { return }
        status = "Stopping…"
        process.terminate()
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
        }
        .commands {
            CommandGroup(replacing: .newItem) { }
        }
    }
}

struct ContentView: View {
    @ObservedObject var vm: VMController

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
            Label(vm.status, systemImage: vm.isRunning ? "play.circle.fill" : "stop.circle")
                .foregroundStyle(vm.isRunning ? .green : .secondary)
            Text("This preview launcher starts one guest using QEMU. It does not yet include the Windows app’s machine manager, Files view, snapshots, SSH integration, or Soil app API.")
                .font(.callout)
                .foregroundStyle(.secondary)
            TextEditor(text: $vm.log)
                .font(.system(.caption, design: .monospaced))
                .scrollContentBackground(.hidden)
                .background(Color(nsColor: .textBackgroundColor).opacity(0.5))
                .clipShape(RoundedRectangle(cornerRadius: 8))
        }
        .padding(24)
    }
}
