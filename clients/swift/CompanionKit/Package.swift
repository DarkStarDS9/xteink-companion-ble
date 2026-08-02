// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "CompanionKit",
    platforms: [
        .iOS(.v15),
        .macOS(.v12)
    ],
    products: [
        .library(name: "CompanionKit", targets: ["CompanionKit"]),
        .executable(name: "companion-bench", targets: ["companion-bench"])
    ],
    targets: [
        .target(name: "CompanionKit"),
        .executableTarget(name: "companion-bench", dependencies: ["CompanionKit"]),
        .testTarget(name: "CompanionKitTests", dependencies: ["CompanionKit"])
    ]
)
