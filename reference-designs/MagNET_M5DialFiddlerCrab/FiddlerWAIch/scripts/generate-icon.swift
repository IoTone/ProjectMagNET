#!/usr/bin/env swift
import Foundation
import AppKit
import CoreGraphics

// Renders the fiddler crab pixel art from CrawdadView into watchOS icon PNGs.
// Outputs into: FiddlerWAIch Watch App/Assets.xcassets/AppIcon.appiconset/

let scriptDir = URL(fileURLWithPath: #file).deletingLastPathComponent()
let projectDir = scriptDir.deletingLastPathComponent()
let iconsetDir = projectDir
    .appendingPathComponent("FiddlerWAIch Watch App/Assets.xcassets/AppIcon.appiconset", isDirectory: true)
try? FileManager.default.createDirectory(at: iconsetDir, withIntermediateDirectories: true)

// 48x48 logical grid — same layout as CrawdadView.
// bg, magenta, hotpink, cyan, neonGreen, yellow, sessionColor, shadow
typealias RGB = (UInt8, UInt8, UInt8)
let BG:      RGB = (10, 3, 22)       // #0A0316
let MAGENTA: RGB = (255, 0, 255)
let HOTPINK: RGB = (255, 74, 152)
let CYAN:    RGB = (0, 255, 255)
let GREEN:   RGB = (57, 255, 20)
let GRID:    RGB = (102, 31, 153)
let YELLOW:  RGB = (255, 229, 0)

enum Pix { case bg, magenta, hotpink, cyan, green, yellow, grid, black }
let lookup: [Pix: RGB] = [
    .bg: BG, .magenta: MAGENTA, .hotpink: HOTPINK,
    .cyan: CYAN, .green: GREEN, .yellow: YELLOW, .grid: GRID,
    .black: (0, 0, 0)
]

// Build a 48x48 grid, filling background first.
var grid: [[Pix]] = Array(repeating: Array(repeating: .bg, count: 48), count: 48)

func fill(_ x: Int, _ y: Int, _ w: Int, _ h: Int, _ color: Pix) {
    for i in 0..<w {
        for j in 0..<h {
            let gx = x + i, gy = y + j
            if gx >= 0, gx < 48, gy >= 0, gy < 48 {
                grid[gy][gx] = color
            }
        }
    }
}

// Shadow
fill(10, 44, 28, 1, .grid)
// Antennae
fill(18, 10, 1, 8, .hotpink)
fill(29, 10, 1, 8, .hotpink)
// Antenna tips (cyan)
grid[9][18] = .cyan
grid[9][29] = .cyan
// Body outline
fill(12, 17, 24, 1, .hotpink)
fill(12, 30, 24, 1, .hotpink)
fill(11, 19, 1, 10, .hotpink)
fill(36, 19, 1, 10, .hotpink)
// Body core
fill(12, 18, 24, 12, .magenta)
fill(14, 20, 20, 1, .hotpink) // stripe
// Session color highlight
fill(14, 18, 4, 1, .cyan)
fill(30, 18, 4, 1, .cyan)
// Big right claw
fill(36, 24, 4, 4, .cyan)
fill(40, 22, 5, 3, .cyan)
fill(40, 27, 5, 3, .cyan)
fill(45, 22, 1, 3, .green)
fill(45, 27, 1, 3, .green)
// Small left claw
fill(8, 22, 4, 3, .cyan)
fill(5, 20, 3, 2, .cyan)
fill(5, 25, 3, 2, .cyan)
// Eyes (closed for icon — calm look)
fill(16, 22, 4, 1, .hotpink)
fill(27, 22, 4, 1, .hotpink)
// Legs
for i in 0..<3 {
    let lx = 15 + i * 6
    fill(lx, 31, 1, 5, .magenta)
    fill(lx + 3, 31, 1, 5, .magenta)
}

func renderPNG(size: Int) -> Data? {
    let colorSpace = CGColorSpaceCreateDeviceRGB()
    let bitsPerComponent = 8
    let bytesPerRow = size * 4
    guard let ctx = CGContext(
        data: nil, width: size, height: size,
        bitsPerComponent: bitsPerComponent, bytesPerRow: bytesPerRow,
        space: colorSpace, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
    ) else { return nil }

    let scale = Double(size) / 48.0
    // Flip y so pixel (0,0) is top-left.
    ctx.translateBy(x: 0, y: CGFloat(size))
    ctx.scaleBy(x: 1, y: -1)

    for y in 0..<48 {
        for x in 0..<48 {
            let pix = grid[y][x]
            let c = lookup[pix] ?? BG
            ctx.setFillColor(CGColor(
                srgbRed: CGFloat(c.0) / 255,
                green: CGFloat(c.1) / 255,
                blue:  CGFloat(c.2) / 255,
                alpha: 1
            ))
            ctx.fill(CGRect(
                x: Double(x) * scale,
                y: Double(y) * scale,
                width: scale + 1, // overdraw slightly to avoid seams
                height: scale + 1
            ))
        }
    }

    guard let cgImg = ctx.makeImage() else { return nil }
    let nsImg = NSImage(cgImage: cgImg, size: NSSize(width: size, height: size))
    guard let tiff = nsImg.tiffRepresentation,
          let rep = NSBitmapImageRep(data: tiff),
          let png = rep.representation(using: .png, properties: [:]) else { return nil }
    return png
}

struct IconSpec {
    let size: Int
    let filename: String
    let idiom: String
    let scale: String
    let role: String?
    let subtype: String?
}

// Apple Watch icon sizes per Xcode's AppIcon catalog for watchOS.
let specs: [IconSpec] = [
    IconSpec(size: 48,  filename: "icon_48.png",  idiom: "watch", scale: "2x", role: "notificationCenter", subtype: "38mm"),
    IconSpec(size: 55,  filename: "icon_55.png",  idiom: "watch", scale: "2x", role: "notificationCenter", subtype: "42mm"),
    IconSpec(size: 58,  filename: "icon_58.png",  idiom: "watch", scale: "2x", role: "companionSettings", subtype: nil),
    IconSpec(size: 87,  filename: "icon_87.png",  idiom: "watch", scale: "3x", role: "companionSettings", subtype: nil),
    IconSpec(size: 80,  filename: "icon_80.png",  idiom: "watch", scale: "2x", role: "appLauncher", subtype: "38mm"),
    IconSpec(size: 88,  filename: "icon_88.png",  idiom: "watch", scale: "2x", role: "appLauncher", subtype: "40mm"),
    IconSpec(size: 92,  filename: "icon_92.png",  idiom: "watch", scale: "2x", role: "appLauncher", subtype: "41mm"),
    IconSpec(size: 100, filename: "icon_100.png", idiom: "watch", scale: "2x", role: "appLauncher", subtype: "44mm"),
    IconSpec(size: 102, filename: "icon_102.png", idiom: "watch", scale: "2x", role: "appLauncher", subtype: "45mm"),
    IconSpec(size: 108, filename: "icon_108.png", idiom: "watch", scale: "2x", role: "appLauncher", subtype: "49mm"),
    IconSpec(size: 1024, filename: "icon_1024.png", idiom: "watch-marketing", scale: "1x", role: nil, subtype: nil),
]

// Dedup sizes for generation (many roles reuse same pixel size)
var written: Set<Int> = []
for s in specs {
    if written.contains(s.size) { continue }
    if let data = renderPNG(size: s.size) {
        let url = iconsetDir.appendingPathComponent(s.filename)
        try? data.write(to: url)
        written.insert(s.size)
        print("wrote \(s.filename) (\(s.size)x\(s.size))")
    }
}

// Write Contents.json
struct ImageEntry: Codable {
    let size: String?
    let idiom: String
    let filename: String
    let scale: String?
    let role: String?
    let subtype: String?
}
struct Info: Codable { let version: Int; let author: String }
struct Catalog: Codable { let images: [ImageEntry]; let info: Info }

let entries = specs.map { s -> ImageEntry in
    let unit = Double(s.size) / (s.scale == "3x" ? 3 : 2)
    return ImageEntry(
        size: "\(unit)x\(unit)",
        idiom: s.idiom,
        filename: s.filename,
        scale: s.scale,
        role: s.role,
        subtype: s.subtype
    )
}

let catalog = Catalog(
    images: entries,
    info: Info(version: 1, author: "FiddlerWAIch")
)
let enc = JSONEncoder()
enc.outputFormatting = [.prettyPrinted, .sortedKeys]
if let data = try? enc.encode(catalog) {
    let url = iconsetDir.appendingPathComponent("Contents.json")
    try? data.write(to: url)
    print("wrote Contents.json")
}

print("done → \(iconsetDir.path)")
