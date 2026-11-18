import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The counter objects as the identity's AGX driver makes them: counterSets of Metal's own MTLCounterSetInternal and
/// MTLCounterInternal (equal and of one hash from call to call), and a sample buffer of
/// newCounterSampleBufferWithLength:label:error:, the driver's own form, refused for 0, 4, 12 and 32776 bytes with
/// MTLCounterErrorDomain code 1 and made for 64: 8 samples under its label, which a compute encoder's boundaries write and
/// the CPU resolves, start at or before end, the unsampled ones 0.
struct NativeCounterObjectsTest: NativeObjectTest {
    let id = "metal.native.counter_objects"
    let name = "Counter sets of Metal's classes and the length form of a sample buffer, on the raw path"

    typealias LengthForm =
        @convention(c) (AnyObject, Selector, UInt, NSString?, UnsafeMutablePointer<Unmanaged<NSError>?>?) ->
        Unmanaged<AnyObject>?

    static func classes(_ device: MTLDevice) -> [String] {
        var failures: [String] = []
        let first = device.counterSets ?? [], again = device.counterSets ?? []
        guard let set = first.first as? NSObject, let other = again.first as? NSObject,
            let counter = (set as? MTLCounterSet)?.counters.first as? NSObject
        else { return ["counterSets answered no set"] }
        let names = [NSStringFromClass(type(of: set)), NSStringFromClass(type(of: counter))]
        if names != ["MTLCounterSetInternal", "MTLCounterInternal"] { failures.append("classes \(names)") }
        if !set.isEqual(other) || set.hash != other.hash { failures.append("two calls' sets are not equal") }
        if set.description != "A counter set containing only a GPU timestamp."
            || counter.description != "A timestamp in nanoseconds on the GPU."
        {
            failures.append("descriptions \(set.description) / \(counter.description)")
        }
        return failures
    }

    static func make(_ device: MTLDevice, _ length: UInt, _ label: String) -> (MTLCounterSampleBuffer?, NSError?) {
        let selector = NSSelectorFromString("newCounterSampleBufferWithLength:label:error:")
        guard let object = device as? NSObject, object.responds(to: selector) else { return (nil, nil) }
        let form = unsafeBitCast(object.method(for: selector), to: LengthForm.self)
        var error: Unmanaged<NSError>?
        let made = form(object, selector, length, label as NSString, &error)?.takeRetainedValue()
        return (made as? MTLCounterSampleBuffer, error?.takeUnretainedValue())
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        work.failures += Self.classes(device)
        guard
            (device as? NSObject)?.responds(to: NSSelectorFromString("newCounterSampleBufferWithLength:label:error:"))
                == true
        else { throw TestFailure("the device has no newCounterSampleBufferWithLength:label:error:") }
        let refused = [0, 4, 12, 0x8008].map { length -> String in
            let (made, error) = Self.make(device, UInt(length), "refused")
            if made != nil { return "made" }
            return error.map { "\($0.domain == MTLCounterErrorDomain ? "counter" : $0.domain)/\($0.code)" } ?? "nil"
        }
        metrics.set("errors", refused.joined(separator: ","))
        if refused != ["counter/1", "counter/1", "counter/1", "counter/1"] {
            work.failures.append("length errors \(refused)")
        }
        let (made, error) = Self.make(device, 64, "length form")
        guard let counters = made else {
            throw TestFailure("64 bytes made no sample buffer: \(String(describing: error))")
        }
        work.objects.append(counters)
        if counters.sampleCount != 8 || counters.label != "length form" || counters.device !== device {
            work.failures.append("sampleCount \(counters.sampleCount) label \(counters.label)")
        }
        let kernel = try gpu.compute("fma_loop")
        let data = try gpu.buffer([SIMD4<Float>](repeating: SIMD4(1, 2, 3, 4), count: 256))
        let sampling = try gpu.commit { commands in
            let pass = MTLComputePassDescriptor()
            pass.sampleBufferAttachments[0].sampleBuffer = counters
            pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = 0
            pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = 1
            let compute = commands.makeComputeCommandEncoder(descriptor: pass)!
            compute.setComputePipelineState(kernel)
            compute.setBuffer(data, offset: 0, index: 0)
            var iterations: UInt32 = 256
            compute.setBytes(&iterations, length: 4, index: 1)
            compute.dispatchThreadgroups(
                MTLSize(width: 4, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1))
            compute.endEncoding()
        }
        try gpu.finish([sampling])
        work.paths.append(RawFacade.path(of: sampling))
        let samples = NativeCountersTest.words(try counters.resolveCounterRange(0..<8))
        if samples.count != 8 || samples[0] == 0 || samples[0] > samples[1]
            || samples[2...].contains(where: { $0 != 0 })
        {
            work.failures.append("samples \(samples)")
        } else {
            metrics.set("compute_us", (Double(samples[1]) - Double(samples[0])) / 1000, digits: 1)
        }
        return work
    }
}
