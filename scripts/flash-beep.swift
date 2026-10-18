// Full-screen white flash and a simultaneous 1 kHz beep once a second, for A/V sync checks.
// Usage: flash-beep [COUNT [TITLE]]. With a title the window is a normal titled window at the
// normal level, so it can be picked for window capture.
import AppKit
import AVFoundation

let count = CommandLine.arguments.count > 1 ? Int(CommandLine.arguments[1]) ?? 5 : 5
let title = CommandLine.arguments.count > 2 ? CommandLine.arguments[2] : ""

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
guard let screen = NSScreen.main else { fatalError("no screen") }
let window = NSWindow(contentRect: screen.frame, styleMask: .borderless, backing: .buffered, defer: false)
window.level = title.isEmpty ? .screenSaver : .normal
window.title = title
window.backgroundColor = .black
window.isOpaque = true
window.ignoresMouseEvents = true
window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
window.setFrame(screen.frame, display: true)
window.orderFrontRegardless()

let rate = 48000.0
let beepFrames = AVAudioFrameCount(rate / 10)
guard let format = AVAudioFormat(standardFormatWithSampleRate: rate, channels: 2),
      let beep = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: beepFrames),
      let channels = beep.floatChannelData else { fatalError("could not create the beep") }
beep.frameLength = beepFrames
for channel in 0..<2 {
    for i in 0..<Int(beepFrames) {
        channels[channel][i] = 0.4 * sinf(2 * Float.pi * 1000 * Float(i) / Float(rate))
    }
}
let engine = AVAudioEngine()
let player = AVAudioPlayerNode()
engine.attach(player)
engine.connect(player, to: engine.mainMixerNode, format: format)
do { try engine.start() } catch { fatalError("audio engine: \(error)") }
player.play()

var remaining = count
func flash() {
    guard remaining > 0 else { NSApp.terminate(nil); return }
    remaining -= 1
    player.scheduleBuffer(beep, at: nil, options: [], completionHandler: nil)
    window.backgroundColor = .white
    window.display()
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { window.backgroundColor = .black }
    DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { flash() }
}
DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { flash() }
app.run()
