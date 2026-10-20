// SPDX-License-Identifier: Apache-2.0
// Activates or deactivates the embedded xrt_user driver extension.
// Usage: XrtLoader.app/Contents/MacOS/XrtLoader [activate|deactivate]
import Foundation
import SystemExtensions

let dextID = "org.xrtmac.XrtUser"

final class Delegate: NSObject, OSSystemExtensionRequestDelegate {
  func request(_ request: OSSystemExtensionRequest,
               actionForReplacingExtension existing: OSSystemExtensionProperties,
               withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
    print("replacing \(existing.bundleShortVersion) (\(existing.bundleVersion)) with \(ext.bundleShortVersion) (\(ext.bundleVersion))")
    return .replace
  }
  func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
    print("NEEDS USER APPROVAL: System Settings > General > Login Items & Extensions > Driver Extensions")
  }
  func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
    print("finished: \(result == .completed ? "completed" : "will complete after reboot")")
    exit(0)
  }
  func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
    let e = error as NSError
    print("FAILED: \(e.domain) \(e.code): \(e.localizedDescription)")
    exit(1)
  }
}

let action = CommandLine.arguments.dropFirst().first ?? "activate"
let delegate = Delegate()
let req = action == "deactivate"
  ? OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: dextID, queue: .main)
  : OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: dextID, queue: .main)
req.delegate = delegate
OSSystemExtensionManager.shared.submitRequest(req)
print("\(action) request submitted for \(dextID)")
DispatchQueue.main.asyncAfter(deadline: .now() + 120) { print("timed out waiting for sysextd"); exit(2) }
dispatchMain()
