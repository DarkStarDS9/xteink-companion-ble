// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "CompanionKit",
    platforms: [
        .iOS(.v15),
        .macOS(.v12)
    ],
    products: [
        .library(name: "CompanionKit", targets: ["CompanionKit"])
    ],
    targets: [
        .target(name: "CompanionKit"),
        .testTarget(name: "CompanionKitTests", dependencies: ["CompanionKit"])
    ]
)
