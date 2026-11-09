import Foundation
import UIKit

/// A macOS restore image Apple publishes for virtual Macs (VirtualMac2,1).
struct MacOSRelease: Identifiable, Hashable {
    let id: String        // build
    let name: String
    let url: URL
    let bytes: Int64
    let note: String

    /// The builds offered, newest last. Apple's catalog (mesu.apple.com) only lists the newest;
    /// these are the same images from Apple's CDN, and Apple still signs them for virtual Macs.
    static let all: [MacOSRelease] = [
        MacOSRelease(id: "24G90", name: "macOS Sequoia 15.6.1",
                     url: URL(string: "https://updates.cdn-apple.com/2025SummerFCS/fullrestores/093-10809/CFD6DD38-DAF0-40DA-854F-31AAD1294C6F/UniversalMac_15.6.1_24G90_Restore.ipsw")!,
                     bytes: 16_035 << 20, note: "Recommended: the version Henge is tested with."),
        MacOSRelease(id: "25G83", name: "macOS Tahoe 26.6.2",
                     url: URL(string: "https://updates.cdn-apple.com/2026SummerFCS/fullrestores/140-75212/A2A24B94-1FC1-45A3-93F7-C51B02AF1F4D/UniversalMac_26.6.2_25G83_Restore.ipsw")!,
                     bytes: 18_856 << 20, note: "Not tested with Henge yet."),
        MacOSRelease(id: "26B123", name: "macOS 27.2",
                     url: URL(string: "https://updates.cdn-apple.com/2026FallFCS/c9671975-52db-4fb5-9af2-00217169ba8d/UniversalMac_27.2_26B123_Restore.ipsw")!,
                     bytes: 24_409 << 20, note: "The newest. Not tested with Henge yet."),
    ]
}

/// Downloads a restore image and installs it on a new virtual Mac, on the
/// iPhone: the machine boots its ROM in DFU mode and the restore runs over its
/// USB port, as Virtualization.framework's installer does on a Mac.
@MainActor
final class Installer: NSObject, ObservableObject {
    static let shared = Installer()

    enum Phase: Equatable {
        case idle
        case downloading
        case preparing
        case installing
        case done(String)
        case failed(String)
    }

    @Published private(set) var phase: Phase = .idle
    @Published private(set) var received: Int64 = 0
    @Published private(set) var expected: Int64 = 0
    @Published private(set) var step: Int = 0
    @Published private(set) var stepProgress: Double = 0
    @Published private(set) var machineName = ""

    private var session: URLSession?
    private var downloadContinuation: CheckedContinuation<URL, Error>?
    nonisolated(unsafe) private var downloadTarget: URL? // set before the task starts, read by the delegate
    private var lastResumeData: Data?

    var busy: Bool {
        switch phase {
        case .downloading, .preparing, .installing: return true
        default: return false
        }
    }

    nonisolated static var imageDirectory: URL { Machines.root.appendingPathComponent("Restore images", isDirectory: true) }
    nonisolated static func imagePath(_ r: MacOSRelease) -> URL { imageDirectory.appendingPathComponent("\(r.id).ipsw") }

    /// Where a restore image can come from.
    enum ImageSource: String, CaseIterable, Identifiable {
        case download
        case files
        case builtIn

        var id: String { rawValue }

        var title: String {
            switch self {
            case .download: return "Download"
            case .files: return "On this device"
            case .builtIn: return "Inside Henge"
            }
        }

        var detail: String {
            switch self {
            case .download: return "Henge downloads the image from Apple when it installs."
            case .files: return "An .ipsw in Files › Henge › Restore images, or one you pick. Download it once and every reinstall reuses it."
            case .builtIn: return "An image packaged inside this copy of Henge. The stock build carries none: the image is 16 GB."
            }
        }
    }

    /// Files › Henge › Restore images: the app's Documents folder, visible with
    /// UIFileSharingEnabled, so an image can be copied in from a computer once.
    nonisolated static var documentsImageDirectory: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Restore images", isDirectory: true)
    }

    /// A restore image shipped inside the app bundle, if this build carries one.
    nonisolated static var bundledImage: URL? {
        if let u = Bundle.main.url(forResource: "macos", withExtension: "ipsw") { return u }
        let root = Bundle.main.bundleURL
        let items = (try? FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: [.fileSizeKey])) ?? []
        return items.first { $0.pathExtension.lowercased() == "ipsw" && isWholeImage($0) }
    }

    private nonisolated static func isWholeImage(_ url: URL) -> Bool {
        let size = (try? url.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0
        return size > 1 << 30  /* a restore image is at least a gigabyte; a stub is not */
    }

    /// The image to install from when not downloading: the newest .ipsw in
    /// Files › Henge › Restore images, then one Henge downloaded earlier, then
    /// one inside the bundle.
    nonisolated static func localImage() -> URL? {
        var candidates: [URL] = []
        for dir in [documentsImageDirectory, imageDirectory] {
            let items = (try? FileManager.default.contentsOfDirectory(
                at: dir, includingPropertiesForKeys: [.fileSizeKey, .contentModificationDateKey])) ?? []
            candidates += items.filter { $0.pathExtension.lowercased() == "ipsw" && isWholeImage($0) }
        }
        let newestFirst = candidates.sorted {
            let a = (try? $0.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate ?? .distantPast
            let b = (try? $1.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate ?? .distantPast
            return a > b
        }
        return newestFirst.first ?? bundledImage
    }

    /// True when this build or this device already has an image to install from.
    nonisolated static var haveLocalImage: Bool { localImage() != nil }

    /// What an image says it can install, read from its BuildManifest.
    struct ImageInfo: Equatable {
        var version: String
        var products: [String]
        var buildsVirtualMac: Bool
    }

    /// Read the image's BuildManifest out of the IPSW (one member of a 16-20 GB
    /// zip; libzip and libplist are already linked for the restore itself).
    nonisolated static func inspect(_ ipsw: URL) -> ImageInfo? {
        var version = [CChar](repeating: 0, count: 64)
        var products = [CChar](repeating: 0, count: 2048)
        var virtual: Int32 = 0
        let rc = version.withUnsafeMutableBufferPointer { v in
            products.withUnsafeMutableBufferPointer { p in
                ipsw.path.withCString { path in
                    hv_install_check_ipsw(path, v.baseAddress, v.count, p.baseAddress, p.count, &virtual)
                }
            }
        }
        guard rc == 0 else { return nil }
        let list = String(cString: products).split(separator: ",").map(String.init)
        return ImageInfo(version: String(cString: version), products: list, buildsVirtualMac: virtual != 0)
    }

    /// Which listed release an image file belongs to, from the build id in its
    /// name: `UniversalMac_15.6.1_24G90_Restore.ipsw` is the 24G90 entry. nil
    /// when the name carries no id we know, which is worth showing: picking a
    /// macOS 27 image while the row says 15.6.1 makes the restore refuse with
    /// "Could not make sure this firmware is suitable for the current device".
    nonisolated static func release(forImage name: String) -> MacOSRelease? {
        MacOSRelease.all.first { name.contains($0.id) }
    }

    /// The size of the image on this iPhone, if a whole one is there (the listed sizes are rounded).
    static func imageSize(_ r: MacOSRelease) -> Int64? {
        let values = try? imagePath(r).resourceValues(forKeys: [.fileSizeKey])
        guard let size = values?.fileSize, Int64(size) > r.bytes / 2 else { return nil }
        return Int64(size)
    }

    static func haveImage(_ r: MacOSRelease) -> Bool { imageSize(r) != nil }

    static func deleteImage(_ r: MacOSRelease) { try? FileManager.default.removeItem(at: imagePath(r)) }

    /// Free space an install takes: the new Mac's disk, and the download if it is not here yet.
    static func spaceNeeded(_ r: MacOSRelease) -> Int64 { (haveImage(r) ? 0 : r.bytes) + newMacBytes }
    private static let newMacBytes: Int64 = 24 << 30

    func reset() { if !busy { phase = .idle } }

    func start(_ release: MacOSRelease, name: String, ramMB: Int, cpus: Int, keepImage: Bool,
               source: ImageSource = .download) {
        guard !busy else { return }
        machineName = name
        UIApplication.shared.isIdleTimerDisabled = true // the screen locking would suspend the download and the restore
        Task {
            defer { UIApplication.shared.isIdleTimerDisabled = false }
            var made: VirtualMac?
            do {
                try checkSpace(release, source: source)
                let rom = try await BootROM.file() // before the long download, so a problem shows up at once
                HengeLog.log("install", "boot ROM ready")
                let ipsw: URL
                switch source {
                case .download:
                    ipsw = try await download(release)
                case .files:
                    guard let local = Self.localImage() else {
                        throw message("No restore image on this \(Device.name). Copy an .ipsw into Files › Henge › Restore images, or download one here first.")
                    }
                    HengeLog.log("install", "using the image on this device: \(local.lastPathComponent)")
                    ipsw = local
                case .builtIn:
                    guard let built = Self.bundledImage else {
                        throw message("This copy of Henge has no macOS image inside it. Use Download, or an image in Files.")
                    }
                    HengeLog.log("install", "using the image inside Henge: \(built.lastPathComponent)")
                    ipsw = built
                }
                if let info = Self.inspect(ipsw) {
                    HengeLog.log("install", "image \(ipsw.lastPathComponent): macOS \(info.version), "
                                 + "builds a virtual Mac: \(info.buildsVirtualMac ? "yes" : "no"), "
                                 + "product types: \(info.products.joined(separator: " "))")
                    if !info.buildsVirtualMac {
                        /* Apple's macOS 27.2 image lists only Mac17,10 to Mac17,13: no
                           Virtual Mac, no AVPBooter, no vmapple component, so the restore
                           has nothing to boot and refuses. Say that here rather than
                           after the machine has started. */
                        throw message("This image (macOS \(info.version)) cannot be installed on a virtual Mac. "
                                    + "It lists \(info.products.prefix(4).joined(separator: ", ")) and no Virtual Mac entry, "
                                    + "so the restore has nothing to boot. Use an image whose manifest names VirtualMac2,1 "
                                    + "(macOS 15.6.1 does), or update macOS from inside the Mac once one runs.")
                    }
                }
                phase = .preparing
                var mac = try prepare(name: name, ramMB: ramMB, rom: rom)
                /* The image decides the version, not the row that was highlighted:
                   a local or bundled image can be any release. */
                mac.osName = Self.release(forImage: ipsw.lastPathComponent)?.name ?? release.name
                made = mac
                phase = .installing
                try await install(mac, ipsw: ipsw, cpus: cpus)
                Machines.shared.add(mac)
                if !keepImage { try? FileManager.default.removeItem(at: ipsw) }
                HengeLog.log("install", "\(name): installed \(release.name)")
                phase = .done("\(release.name) is installed on \(name). Quit Henge (swipe it away) and open it again, then tap Start.")
            } catch {
                HengeLog.log("install", "failed: \(error.localizedDescription)")
                // A half-installed Mac is no use, and its disk can be many gigabytes. The download stays.
                if let made, henge_vm_state() != 1 { try? FileManager.default.removeItem(at: made.directory) }
                phase = .failed(error.localizedDescription)
            }
        }
    }

    private func checkSpace(_ r: MacOSRelease, source: ImageSource) throws {
        let free = (try? URL(fileURLWithPath: NSHomeDirectory()).resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?
            .volumeAvailableCapacityForImportantUsage ?? 0
        /* Only a download adds the image's size: a local or bundled one is already here. */
        let imageBytes = source == .download ? (Self.haveImage(r) ? 0 : r.bytes) : 0
        let needed = imageBytes + Self.newMacBytes
        if free > 0 && free < needed {
            throw message("This install needs about \(ByteText.string(UInt64(needed))) free; this \(Device.name) has \(ByteText.string(UInt64(free))).")
        }
    }

    // MARK: Download

    private func download(_ r: MacOSRelease) async throws -> URL {
        let dest = Self.imagePath(r)
        try FileManager.default.createDirectory(at: Self.imageDirectory, withIntermediateDirectories: true)
        Machines.excludeFromBackup(Self.imageDirectory)
        if Self.haveImage(r) {
            HengeLog.log("install", "using the downloaded \(r.name)")
            return dest
        }
        phase = .downloading
        received = 0
        expected = r.bytes
        downloadTarget = dest
        HengeLog.log("install", "downloading \(r.url.absoluteString)")
        for attempt in 1...20 {
            do {
                return try await downloadOnce(r.url)
            } catch let error as URLError where error.code != .cancelled && attempt < 20 {
                lastResumeData = error.downloadTaskResumeData ?? lastResumeData
                HengeLog.log("install", "download interrupted (\(error.localizedDescription)); resuming")
                try await Task.sleep(nanoseconds: 3_000_000_000)
            }
        }
        throw message("The download kept failing.")
    }

    private func downloadOnce(_ url: URL) async throws -> URL {
        let cfg = URLSessionConfiguration.default
        cfg.timeoutIntervalForRequest = 120
        cfg.timeoutIntervalForResource = 48 * 3600
        let s = URLSession(configuration: cfg, delegate: self, delegateQueue: nil)
        session = s
        return try await withCheckedThrowingContinuation { (c: CheckedContinuation<URL, Error>) in
            downloadContinuation = c
            let task = lastResumeData.map { s.downloadTask(withResumeData: $0) } ?? s.downloadTask(with: url)
            lastResumeData = nil
            task.resume()
        }
    }

    // MARK: The new machine

    private func prepare(name: String, ramMB: Int, rom: URL) throws -> VirtualMac {
        // A new machine identity, as VZMacMachineIdentifier makes one: a random ECID.
        let ecid = UInt64.random(in: 1 << 40 ..< 1 << 62)
        var mac = VirtualMac(name: name, ecid: ecid, seed: 0, ramMB: ramMB, diskBytes: 64 << 30)
        (mac.screenWidth, mac.screenHeight) = Device.fittingScreen
        let fm = FileManager.default
        try fm.createDirectory(at: mac.directory, withIntermediateDirectories: true)
        Machines.excludeFromBackup(Machines.root)
        try? fm.removeItem(at: mac.rom)
        try fm.copyItem(at: rom, to: mac.rom)
        guard fm.createFile(atPath: mac.disk.path, contents: nil) else { throw message("Cannot create the disk.") }
        let h = try FileHandle(forWritingTo: mac.disk)
        try h.truncate(atOffset: UInt64(64) << 30) // sparse: blocks are only used as macOS writes them
        try h.close()
        guard henge_make_aux(mac.aux.path) == 0 else { throw message("Cannot create the auxiliary storage.") }
        let info: [String: Any] = ["name": name, "ecid": ecid, "seed": 0, "ramMB": ramMB]
        try JSONSerialization.data(withJSONObject: info).write(to: mac.directory.appendingPathComponent("machine.json"))
        mac.diskBytes = 64 << 30
        HengeLog.log("install", "new machine \(name): ECID \(ecid), 64 GB disk")
        return mac
    }

    // MARK: The restore

    /// What the Mac's own restore system is given, in MB. Apple's restore expects
    /// 2 GB; a 2 GB iPhone is given 1.5 GB because that is all it can spare.
    static var installRAMFloorMB: Int { Device.lowMemoryDevice ? 1_536 : 2_048 }

    private func install(_ mac: VirtualMac, ipsw: URL, cpus: Int) async throws {
        guard JITBootstrap.ready else { throw message("Turn on JIT first: the installer runs the Mac's own restore system, which needs it.") }
        // The restore needs 2 GB, whatever the Mac is set to use afterwards. A 2 GB
        // iPhone cannot leave 2 GB free, so it is offered 1.5 GB instead
        // (docs/ios15-a9-support.md); the restore system may refuse to run there.
        let floor = Self.installRAMFloorMB
        var restoring = mac
        restoring.ramMB = max(mac.ramMB, floor)
        let ram = GuestMemory.prepare(restoring)
        guard ram >= floor else { throw message("Installing needs about \(Device.ramText(floor)) for the Mac's memory, and this \(Device.name) cannot spare it.") }
        let work = mac.directory.appendingPathComponent("restore", isDirectory: true)
        try FileManager.default.createDirectory(at: work, withIntermediateDirectories: true)
        var buffer = [CChar](repeating: 0, count: 256)
        let rc = henge_vm_install(mac.rom.path, mac.disk.path, mac.aux.path, mac.ecid, ipsw.path, work.path, ram, Int32(cpus),
                                  &buffer, buffer.count)
        if rc != 0 { throw message(String(cString: buffer)) }
        while true {
            try await Task.sleep(nanoseconds: 1_000_000_000)
            step = Int(henge_install_step())
            stepProgress = henge_install_progress()
            if step == 100 { break }
            if step == 101 { throw message("The restore failed. The log (Settings, or Documents/henge.log) says why.") }
            if henge_vm_state() == 2 && step < 100 {
                // The restore ends with the machine restarting, and idevicerestore finishes a
                // little after; a stop with no result after that is a failure.
                for _ in 0..<90 where henge_install_step() < 100 { try await Task.sleep(nanoseconds: 1_000_000_000) }
                if henge_install_step() == 100 { break }
                throw message("The machine stopped during the restore (\(String(cString: henge_vm_exit_reason()))).")
            }
        }
        if henge_vm_state() == 1 { henge_vm_stop() }
        try? FileManager.default.removeItem(at: work)
    }

    static func stepName(_ step: Int) -> String {
        switch step {
        case 0: return "Finding the Mac"
        case 1: return "Starting the installer"
        case 2: return "Writing macOS"
        case 3: return "Checking macOS"
        case 4: return "Installing firmware"
        case 6, 7: return "Installing system files"
        default: return "Installing"
        }
    }

    private func message(_ text: String) -> NSError {
        NSError(domain: "Henge", code: 2, userInfo: [NSLocalizedDescriptionKey: text])
    }
}

extension Installer: URLSessionDownloadDelegate {
    nonisolated func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                                totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        Task { @MainActor in
            self.received = totalBytesWritten
            if totalBytesExpectedToWrite > 0 { self.expected = totalBytesExpectedToWrite }
        }
    }

    nonisolated func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didResumeAtOffset fileOffset: Int64,
                                expectedTotalBytes: Int64) {
        Task { @MainActor in self.received = fileOffset }
    }

    nonisolated func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        // The file is gone when this returns: move it now, on this thread.
        let target = self.downloadTarget
        let status = (downloadTask.response as? HTTPURLResponse)?.statusCode ?? 0
        do {
            guard let target, status == 200 || status == 206 else {
                throw NSError(domain: "Henge", code: 3, userInfo: [NSLocalizedDescriptionKey: "Apple's server answered \(status)."])
            }
            try? FileManager.default.removeItem(at: target)
            try FileManager.default.moveItem(at: location, to: target)
            Task { @MainActor in
                self.downloadContinuation?.resume(returning: target)
                self.downloadContinuation = nil
            }
        } catch {
            Task { @MainActor in
                self.downloadContinuation?.resume(throwing: error)
                self.downloadContinuation = nil
            }
        }
    }

    nonisolated func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let error else { return }
        Task { @MainActor in
            self.downloadContinuation?.resume(throwing: error)
            self.downloadContinuation = nil
        }
    }
}
