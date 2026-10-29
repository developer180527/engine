// Study 008 spike: RUN cull.slang's Metal output and check it against the CPU.
// 10 000 spheres scattered around a box-shaped "frustum" (six planes, |x|,|y|,|z| <= 1).
// The shader appends every sphere that is not fully outside a plane, counting
// with an atomic into the indirect-args buffer. Exit 0 only if the GPU's count
// and its SET of visible ids equal the CPU's.
//
//   swift run_cull.swift <cull.metallib>
import Metal
import Foundation

let args = CommandLine.arguments
guard args.count == 2, let dev = MTLCreateSystemDefaultDevice() else { print("usage: run_cull <cull.metallib>"); exit(2) }
let lib = try! dev.makeLibrary(URL: URL(fileURLWithPath: args[1]))
guard let fn = lib.makeFunction(name: "csMain") else { print("FAIL no csMain in \(lib.functionNames)"); exit(1) }
let pso = try! dev.makeComputePipelineState(function: fn)

let N = 10_000
var rng = SystemRandomNumberGenerator()
var spheres = [SIMD4<Float>]()
for _ in 0..<N {
    spheres.append(SIMD4(Float.random(in: -3...3, using: &rng), Float.random(in: -3...3, using: &rng),
                         Float.random(in: -3...3, using: &rng), Float.random(in: 0.05...0.5, using: &rng)))
}
// Planes: inward normals, dot(n, p) + d >= 0 inside.  x <= 1 is (-1,0,0, 1), and so on.
let planes: [SIMD4<Float>] = [SIMD4(1,0,0,1), SIMD4(-1,0,0,1), SIMD4(0,1,0,1), SIMD4(0,-1,0,1), SIMD4(0,0,1,1), SIMD4(0,0,-1,1)]
var cpu = Set<UInt32>()
for (i, s) in spheres.enumerated() {
    var out = false
    for p in planes where p.x*s.x + p.y*s.y + p.z*s.z + p.w < -s.w { out = true; break }
    if !out { cpu.insert(UInt32(i)) }
}

// Cull_0: float4 planes[6]; uint objectCount; uint indexCount; uint2 pad  (112 bytes)
var cull = [UInt8](repeating: 0, count: 112)
cull.withUnsafeMutableBytes { raw in
    for (k, p) in planes.enumerated() { raw.storeBytes(of: p, toByteOffset: k * 16, as: SIMD4<Float>.self) }
    raw.storeBytes(of: UInt32(N), toByteOffset: 96, as: UInt32.self)
    raw.storeBytes(of: UInt32(36), toByteOffset: 100, as: UInt32.self)
}
let bSpheres = dev.makeBuffer(bytes: spheres, length: N * 16, options: .storageModeShared)!
let bVisible = dev.makeBuffer(length: N * 4, options: .storageModeShared)!
let bArgs    = dev.makeBuffer(length: 20, options: .storageModeShared)!   // DrawArgs_0, zeroed
memset(bArgs.contents(), 0, 20)
let bCull    = dev.makeBuffer(bytes: cull, length: cull.count, options: .storageModeShared)!

let q = dev.makeCommandQueue()!, cb = q.makeCommandBuffer()!, enc = cb.makeComputeCommandEncoder()!
enc.setComputePipelineState(pso)
enc.setBuffer(bSpheres, offset: 0, index: 0)   // indices from the generated MSL signature
enc.setBuffer(bVisible, offset: 0, index: 1)
enc.setBuffer(bArgs,    offset: 0, index: 2)
enc.setBuffer(bCull,    offset: 0, index: 3)
enc.dispatchThreadgroups(MTLSize(width: (N + 63) / 64, height: 1, depth: 1),
                         threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1))
enc.endEncoding(); cb.commit(); cb.waitUntilCompleted()
if let e = cb.error { print("FAIL GPU: \(e)"); exit(1) }

let count = Int(bArgs.contents().load(fromByteOffset: 4, as: UInt32.self))   // instanceCount
let ids = bVisible.contents().bindMemory(to: UInt32.self, capacity: N)
let gpu = Set((0..<count).map { ids[$0] })
var bad = 0
func check(_ what: String, _ ok: Bool) { print("\(ok ? "ok  " : "FAIL") \(what)"); if !ok { bad += 1 } }
check("GPU count \(count) equals CPU count \(cpu.count) (of \(N))", count == cpu.count)
check("no id appended twice (the atomic gave each survivor its own slot)", gpu.count == count)
check("the same SET of visible ids as the CPU", gpu == cpu)
exit(bad == 0 ? 0 : 1)
