// Holds the left mouse button at a point of the main display, for the click mark E2E:
// mouse.swift X Y DELAY HOLD, with X and Y as fractions of the display and the times in
// seconds. Prints the display's size in points first, and "untrusted" instead of pressing
// when this process may not post events (Accessibility permission).
import ApplicationServices
import Foundation

let arguments = CommandLine.arguments
guard arguments.count == 5, let x = Double(arguments[1]), let y = Double(arguments[2]),
      let delay = Double(arguments[3]), let hold = Double(arguments[4]) else {
    FileHandle.standardError.write("usage: mouse X Y DELAY HOLD\n".data(using: .utf8)!)
    exit(2)
}
let bounds = CGDisplayBounds(CGMainDisplayID())
print("size=\(Int(bounds.width))x\(Int(bounds.height))")
guard AXIsProcessTrusted() else {
    print("untrusted")
    exit(0)
}
fflush(stdout)
let point = CGPoint(x: bounds.minX + bounds.width * x, y: bounds.minY + bounds.height * y)
Thread.sleep(forTimeInterval: delay)
CGWarpMouseCursorPosition(point)
CGEvent(mouseEventSource: nil, mouseType: .leftMouseDown, mouseCursorPosition: point, mouseButton: .left)?.post(tap: .cghidEventTap)
Thread.sleep(forTimeInterval: hold)
CGEvent(mouseEventSource: nil, mouseType: .leftMouseUp, mouseCursorPosition: point, mouseButton: .left)?.post(tap: .cghidEventTap)
print("pressed")
