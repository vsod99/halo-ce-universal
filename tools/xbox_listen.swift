// The Xbox's sound, live, on the Mac's speakers (tools/xbox_dev.py run
// --listen): xemu's AC'97 controller, which the Xbox port plays through,
// reaches only QEMU's audio backends, and of those xemu's SDL one crashes
// for it; the WAV one works. This follows the WAV file xemu writes (16-bit
// PCM after a 44-byte header) and plays what it adds as it comes.
//
//     swiftc -O tools/xbox_listen.swift -o build/xbox/tools/xbox_listen
//     build/xbox/tools/xbox_listen sound.wav

import AVFoundation
import Foundation

let path = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "sound.wav"

// the file, once xemu has made it and written its header
var file: FileHandle? = nil
var header = Data()
while header.count < 44 {
    if file == nil {
        file = FileHandle(forReadingAtPath: path)
    }
    if let file = file {
        header.append(file.readData(ofLength: 44 - header.count))
    }
    if header.count < 44 {
        usleep(50_000)
    }
}
func little(_ offset: Int, _ size: Int) -> Int {
    var value = 0
    for index in 0..<size {
        value |= Int(header[offset + index]) << (8 * index)
    }
    return value
}
let channels = max(1, little(22, 2))
let rate = Double(little(24, 4))
let frameBytes = 2 * channels

let engine = AVAudioEngine()
let player = AVAudioPlayerNode()
let format = AVAudioFormat(standardFormatWithSampleRate: rate, channels: AVAudioChannelCount(channels))!
engine.attach(player)
engine.connect(player, to: engine.mainMixerNode, format: format)
do {
    try engine.start()
} catch {
    FileHandle.standardError.write("xbox_listen: no sound output: \(error)\n".data(using: .utf8)!)
    exit(1)
}
player.play()

// what is scheduled and not yet played, in frames: kept under half a second
// (if the Mac falls behind, the oldest is skipped rather than lagging on)
let maximumAhead = Int(rate / 2)
var ahead = 0
let lock = NSLock()
var pending = Data()
while true {
    let data = file!.readData(ofLength: 16384)
    if data.isEmpty {
        usleep(10_000)
        continue
    }
    pending.append(data)
    let frames = pending.count / frameBytes
    if frames == 0 {
        continue
    }
    lock.lock()
    let skip = ahead + frames > maximumAhead * 2
    lock.unlock()
    if skip {
        pending.removeFirst(frames * frameBytes)
        continue
    }
    let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(frames))!
    buffer.frameLength = AVAudioFrameCount(frames)
    pending.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
        let samples = raw.bindMemory(to: Int16.self)
        for channel in 0..<channels {
            let output = buffer.floatChannelData![channel]
            for frame in 0..<frames {
                output[frame] = Float(Int16(littleEndian: samples[frame * channels + channel])) / 32768
            }
        }
    }
    pending.removeFirst(frames * frameBytes)
    lock.lock()
    ahead += frames
    lock.unlock()
    player.scheduleBuffer(buffer) {
        lock.lock()
        ahead -= frames
        lock.unlock()
    }
}
