// Study 008 spike: RUN Slang's Metal output, not just compile it.
// Loads a metallib, draws triangle.slang's vsMain/fsMain into a 64x64 offscreen
// target, and reads back three pixels: the centre (inside the triangle, a blend
// of the three vertex colours), a corner (outside, the clear colour), and one
// near each vertex. Exits 0 only if all of them are what the shader says.
//
//   swift run_metal.swift <triangle.metallib> <vertexFn> <fragmentFn>
import Metal
import Foundation

let args = CommandLine.arguments
guard args.count == 4 else { print("usage: run_metal <lib.metallib> <vs> <fs>"); exit(2) }
guard let dev = MTLCreateSystemDefaultDevice() else { print("FAIL no Metal device"); exit(1) }
let lib: MTLLibrary
do { lib = try dev.makeLibrary(URL: URL(fileURLWithPath: args[1])) }
catch { print("FAIL loading \(args[1]): \(error)"); exit(1) }
guard let vs = lib.makeFunction(name: args[2]), let fs = lib.makeFunction(name: args[3]) else {
    print("FAIL functions not found; library has: \(lib.functionNames)"); exit(1)
}
let W = 64, H = 64
let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: W, height: H, mipmapped: false)
td.usage = [.renderTarget]; td.storageMode = .shared
let target = dev.makeTexture(descriptor: td)!
let pd = MTLRenderPipelineDescriptor()
pd.vertexFunction = vs; pd.fragmentFunction = fs
pd.colorAttachments[0].pixelFormat = .rgba8Unorm
let pso: MTLRenderPipelineState
do { pso = try dev.makeRenderPipelineState(descriptor: pd) }
catch { print("FAIL pipeline: \(error)"); exit(1) }
let rp = MTLRenderPassDescriptor()
rp.colorAttachments[0].texture = target
rp.colorAttachments[0].loadAction = .clear
rp.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
rp.colorAttachments[0].storeAction = .store
let q = dev.makeCommandQueue()!, cb = q.makeCommandBuffer()!
let enc = cb.makeRenderCommandEncoder(descriptor: rp)!
enc.setRenderPipelineState(pso)
enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
enc.endEncoding(); cb.commit(); cb.waitUntilCompleted()
if let e = cb.error { print("FAIL GPU: \(e)"); exit(1) }

var px = [UInt8](repeating: 0, count: W * H * 4)
target.getBytes(&px, bytesPerRow: W * 4, from: MTLRegionMake2D(0, 0, W, H), mipmapLevel: 0)
func at(_ x: Int, _ y: Int) -> (Int, Int, Int) { let i = (y * W + x) * 4; return (Int(px[i]), Int(px[i+1]), Int(px[i+2])) }

var bad = 0
func check(_ what: String, _ p: (Int, Int, Int), _ ok: Bool) {
    print("\(ok ? "ok  " : "FAIL") \(what): rgb\(p)"); if !ok { bad += 1 }
}
let c = at(W/2, H/2)
check("centre is inside, a blend of all three colours", c, c.0 > 20 && c.1 > 20 && c.2 > 20)
check("corner is outside, the clear colour", at(1, 1), at(1, 1) == (0, 0, 0))
// Vertices (NDC): top (0,0.5) red, bottom-right (0.5,-0.5) green, bottom-left (-0.5,-0.5) blue.
// Pixel rows run top-down, so NDC y=+0.5 is row H/4.
let top = at(W/2, H/4 + 2), br = at(3*W/4 - 3, 3*H/4 - 2), bl = at(W/4 + 3, 3*H/4 - 2)
check("near the top vertex, mostly red", top, top.0 > top.1 && top.0 > top.2)
check("near bottom-right, mostly green", br, br.1 > br.0 && br.1 > br.2)
check("near bottom-left, mostly blue", bl, bl.2 > bl.0 && bl.2 > bl.1)
exit(bad == 0 ? 0 : 1)
