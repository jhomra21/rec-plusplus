// Shows black and white vertical stripes for the scaling E2E: stripes.swift X Y WIDTH HEIGHT,
// in pixels of the main display from its top-left corner. The left half has 1-pixel stripes,
// the right half 8-pixel ones. Prints the display's size in pixels, then "small" and exits if
// the rectangle does not fit, else "shown" once the window is up. Runs until killed.
import AppKit

let arguments = CommandLine.arguments
guard arguments.count == 5, let x = Int(arguments[1]), let y = Int(arguments[2]),
      let width = Int(arguments[3]), let height = Int(arguments[4]), width >= 2, height >= 1 else {
    FileHandle.standardError.write("usage: stripes X Y WIDTH HEIGHT\n".data(using: .utf8)!)
    exit(2)
}
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
guard let screen = NSScreen.screens.first else { exit(1) }
let scale = screen.backingScaleFactor
let pixelsWide = Int(screen.frame.width * scale), pixelsHigh = Int(screen.frame.height * scale)
print("pixels=\(pixelsWide)x\(pixelsHigh)")
guard x >= 0, y >= 0, x + width <= pixelsWide, y + height <= pixelsHigh else {
    print("small")
    exit(0)
}

var pixels = [UInt8](repeating: 0, count: width * height)
for row in 0..<height {
    for column in 0..<width {
        let stripe = column < width / 2 ? 1 : 8
        pixels[row * width + column] = (column / stripe) % 2 == 0 ? 0 : 255
    }
}
let provider = CGDataProvider(data: Data(pixels) as CFData)!
let image = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 8, bytesPerRow: width,
                    space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGBitmapInfo(rawValue: 0), provider: provider,
                    decode: nil, shouldInterpolate: false, intent: .defaultIntent)!

let size = NSSize(width: CGFloat(width) / scale, height: CGFloat(height) / scale)
let origin = NSPoint(x: screen.frame.minX + CGFloat(x) / scale,
                     y: screen.frame.maxY - CGFloat(y) / scale - size.height)
let window = NSWindow(contentRect: NSRect(origin: origin, size: size), styleMask: .borderless, backing: .buffered, defer: false)
window.level = .floating
window.hasShadow = false
window.ignoresMouseEvents = true
let view = NSView(frame: NSRect(origin: .zero, size: size))
view.wantsLayer = true
view.layer?.contents = image
view.layer?.contentsScale = scale
view.layer?.contentsGravity = .topLeft
view.layer?.magnificationFilter = .nearest
view.layer?.minificationFilter = .nearest
window.contentView = view
window.setFrameOrigin(origin)
window.orderFrontRegardless()
DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
    print("shown")
    fflush(stdout)
}
app.run()
