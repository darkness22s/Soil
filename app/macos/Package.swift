// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "SoilMac",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "SoilMac", targets: ["SoilMac"])
    ],
    targets: [
        .executableTarget(name: "SoilMac")
    ]
)
