import SwiftUI
import UniformTypeIdentifiers

/// Download macOS (or use a copy already on this device) and install it on a new Mac.
struct InstallView: View {
    @ObservedObject private var installer = Installer.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @State private var jitReady = JITBootstrap.ready
    @StateObject private var display = DisplayRouter()
    @State private var release = MacOSRelease.all.first(where: Installer.haveImage) ?? MacOSRelease.all[0]
    @State private var name = "Mac"
    @State private var ramMB = Device.suggestedRAMMB
    @State private var deleteAfter = false
    @State private var refresh = 0 // redraws the list when a download is deleted
    @State private var details = false
    @State private var stageStarted = Date()
    @State private var stageSeen = -1
    @State private var confirmDeleteDownload: MacOSRelease?
    /* Where the restore image comes from. Reusing one already on the device is
       what makes reinstalling a build cheap: the image is 16 GB. */
    @State private var source: Installer.ImageSource = Installer.haveLocalImage ? .files : .download
    @State private var pickingImage = false
    @State private var localImage = Installer.localImage()
    @State private var imageInfo: Installer.ImageInfo?
    @AppStorage("henge.cpus") private var cpus = 4

    var body: some View {
        CompatNavigationStack {
            ScrollView {
                VStack(spacing: 0) {
                    switch installer.phase {
                    case .idle: chooser
                    case .downloading, .preparing, .installing: progress
                    case .done(let text): finished(text)
                    case .failed(let text): failed(text)
                    }
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 24)
                .deskColumn()
            }
            .background(Desk.page)
            .navigationTitle(installer.busy ? "Installing" : "Install macOS")
            .compatTask {
                while !Task.isCancelled { // the region may be claimed after a hand-off to StikDebug
                    jitReady = JITBootstrap.ready
                    try? await Task.sleep(nanoseconds: 1_000_000_000)
                }
            }
        }
    }

    // MARK: Choosing

    @ViewBuilder private var chooser: some View {
        DeskHeading("Version")
        VStack(spacing: 0) {
            ForEach(MacOSRelease.all) { r in
                Button { release = r } label: { versionRow(r) }
                    .buttonStyle(.plain)
                    .contextMenu {
                        if Installer.haveImage(r) {
                            DestructiveButton("Delete Download") { confirmDeleteDownload = r }
                        }
                    }
                if r.id != MacOSRelease.all.last?.id { Rectangle().fill(Desk.hairline).frame(height: 0.5) }
            }
        }
        .deskCard()
        .id(refresh)
        .compatConfirm("Delete this download?", isPresented: Binding(get: { confirmDeleteDownload != nil }, set: { if !$0 { confirmDeleteDownload = nil } }),
                       confirm: "Delete Download") {
            if let r = confirmDeleteDownload { Installer.deleteImage(r) }
            refresh += 1
        }

        DeskHeading("Image")
        VStack(spacing: 0) {
            Picker("Image", selection: $source) {
                ForEach(Installer.ImageSource.allCases) { s in
                    Text(s.title).tag(s)
                }
            }
            .pickerStyle(.segmented)
            .padding(.horizontal, 17)
            .padding(.top, 12)
            Text(source.detail)
                .font(.system(size: 13)).foregroundColor(Desk.dim)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 17)
                .padding(.top, 8)
                .padding(.bottom, 10)
            if source != .download {
                Rectangle().fill(Desk.hairline).frame(height: 0.5)
                HStack(spacing: 10) {
                    VStack(alignment: .leading, spacing: 2) {
                        if let image = localImage {
                            Text(image.lastPathComponent)
                                .font(.system(size: 15, weight: .medium)).foregroundColor(Desk.text).lineLimit(1)
                            HStack(spacing: 8) {
                                Text(ByteText.string(UInt64((try? image.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0)))
                                    .font(.system(size: 13)).foregroundColor(Desk.good)
                                if let info = imageInfo {
                                    Text(info.buildsVirtualMac ? "macOS \(info.version) · virtual Mac ok"
                                                               : "macOS \(info.version) · no Virtual Mac")
                                        .font(.system(size: 13))
                                        .foregroundColor(info.buildsVirtualMac ? Desk.good : Desk.warn)
                                }
                            }
                        } else {
                            Text(source == .builtIn ? "No image inside this build" : "No image on this device")
                                .font(.system(size: 15, weight: .medium)).foregroundColor(Desk.warn)
                            Text(source == .builtIn ? "The stock ipa carries none: the image is 16 GB."
                                                    : "Copy an .ipsw into Files › Henge › Restore images.")
                                .font(.system(size: 13)).foregroundColor(Desk.dim)
                        }
                    }
                    Spacer(minLength: 8)
                    Button("Choose…") { pickingImage = true }
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundColor(Desk.accentLight)
                }
                .padding(.horizontal, 17)
                .padding(.vertical, 10)
            }
        }
        .deskCard()
        .fileImporter(isPresented: $pickingImage, allowedContentTypes: [UTType(filenameExtension: "ipsw") ?? .data]) { result in
            if case let .success(url) = result { importImage(url) }
        }
        .onChange(of: source) { chosen in
            /* Follow the image: its file name carries the build id, so the version
               row cannot claim a release the file is not. */
            guard chosen != .download else { return }
            localImage = Installer.localImage()
            imageInfo = localImage.flatMap { Installer.inspect($0) }
            if let image = localImage, let match = Installer.release(forImage: image.lastPathComponent) { release = match }
        }

        DeskHeading("New Mac")
        VStack(spacing: 0) {
            TextField("Name", text: $name)                .font(.system(size: 16))
                .padding(.horizontal, 17)
                .padding(.vertical, 14)
            Rectangle().fill(Desk.hairline).frame(height: 0.5)
            Stepper(value: $ramMB, in: (Device.ramChoicesMB.first ?? 1_024)...(Device.ramChoicesMB.last ?? 16_384),
                    step: Device.ramStepMB) {
                HStack {
                    Text("Memory").font(.system(size: 16))
                    Spacer()
                    Text(Device.ramText(ramMB)).font(.system(size: 16, weight: .semibold))
                }
            }
            .padding(.horizontal, 17)
            .padding(.vertical, 10)
            if let note = Device.ramNote(ramMB) {
                Text(note)
                    .font(.system(size: 13))
                    .foregroundColor(Desk.dim)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 17)
                    .padding(.bottom, 10)
            }
            Rectangle().fill(Desk.hairline).frame(height: 0.5)
            Toggle("Delete the download after", isOn: $deleteAfter)
                .font(.system(size: 16))
                .padding(.horizontal, 17)
                .padding(.vertical, 10)
        }
        .deskCard()

        Button(source == .download && !Installer.haveImage(release) ? "Download and Install" : "Install") {
            installer.start(release, name: name.isEmpty ? "Mac" : name, ramMB: ramMB, cpus: cpus, keepImage: !deleteAfter,
                            source: source)
        }
        .buttonStyle(DeskButtonStyle(enabled: jitReady && !Device.chipTooOld))
        .disabled(!jitReady || Device.chipTooOld)
        .padding(.top, 20)
        Text(Device.chipTooOld ? Device.chipTooOldText : jitReady ? "Keep Henge open and the \(Device.name) plugged in. Needs about \(ByteText.string(UInt64(installBytes))) free."
                                : JITBootstrap.lastFailure ?? "Turn on JIT first. \(jit.automaticDescription)")
            .font(.system(size: 13))
            .foregroundColor(Desk.dim)
            .multilineTextAlignment(.center)
            .padding(.top, 10)
        if !jitReady && !Device.chipTooOld {
            // Before any Mac exists, this is the only place to ask for JIT (#23).
            Button("Turn On JIT") { jit.enable() }
                .buttonStyle(DeskButtonStyle())
                .padding(.top, 12)
        }
    }

    /// Free space this install takes: the new Mac's disk, plus the download when
    /// the image is not already here.
    private var installBytes: Int64 {
        let image = (source == .download && !Installer.haveImage(release)) ? release.bytes : 0
        return image + (24 << 30)
    }

    /// Copies a picked .ipsw into Files › Henge › Restore images, so it is reused
    /// by every later install and visible in Files.
    private func importImage(_ url: URL) {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let fm = FileManager.default
        try? fm.createDirectory(at: Installer.documentsImageDirectory, withIntermediateDirectories: true)
        let dest = Installer.documentsImageDirectory.appendingPathComponent(url.lastPathComponent)
        if url.standardizedFileURL != dest.standardizedFileURL {
            try? fm.removeItem(at: dest)
            try? fm.copyItem(at: url, to: dest)
            HengeLog.log("install", "image brought in from Files: \(dest.lastPathComponent)")
        }
        localImage = Installer.localImage()
        imageInfo = localImage.flatMap { Installer.inspect($0) }
        source = .files
        if let image = localImage, let match = Installer.release(forImage: image.lastPathComponent) { release = match }
    }

    private func versionRow(_ r: MacOSRelease) -> some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(r.name).font(.system(size: 16, weight: .medium)).foregroundColor(Desk.text)
                if let size = Installer.imageSize(r) {
                    Text("Downloaded, \(ByteText.string(UInt64(size)))").font(.system(size: 13)).foregroundColor(Desk.good)
                } else {
                    Text("\(ByteText.string(UInt64(r.bytes))) download").font(.system(size: 13)).foregroundColor(Desk.dim)
                }
                Text(r.note).font(.system(size: 13)).foregroundColor(Desk.dim)
            }
            Spacer()
            if release == r { Image(systemName: "checkmark").font(.system(size: 15, weight: .bold)).foregroundColor(Desk.accentLight) }
        }
        .padding(.horizontal, 17)
        .padding(.vertical, 12)
        .contentShape(Rectangle())
    }

    // MARK: Going

    private static let stages = ["Download", "Start installer", "Write macOS", "Check macOS", "Finish"]

    private var stage: Int {
        switch installer.phase {
        case .downloading: return 0
        case .preparing: return 1
        default:
            switch installer.step {
            case 0, 1: return 1
            case 2: return 2
            case 3: return 3
            default: return 4
            }
        }
    }

    private var fraction: Double {
        if case .downloading = installer.phase {
            return installer.expected > 0 ? Double(installer.received) / Double(installer.expected) : 0
        }
        return installer.stepProgress
    }

    /// From how this step has gone so far; nothing until there is enough to go on.
    private var timeLeft: String? {
        let elapsed = Date().timeIntervalSince(stageStarted)
        guard stageSeen == stage, fraction > 0.03, fraction < 1, elapsed > 10 else { return nil }
        let minutes = Int((elapsed / fraction * (1 - fraction) / 60).rounded(.up))
        return minutes <= 1 ? "About a minute left in this step" : "About \(minutes) minutes left in this step"
    }

    @ViewBuilder private var progress: some View {
        Text("\(release.name) on this \(Device.name). Keep Henge open.")
            .font(.system(size: 16))
            .foregroundColor(Desk.dim)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 4)
            .padding(.bottom, 16)

        SecondTicker { _ in
            VStack(alignment: .leading, spacing: 0) {
                Text("\(Int(fraction * 100))%")
                    .font(.system(size: 66, weight: .bold))
                    
                Text(timeLeft ?? "Step \(stage + 1) of \(Self.stages.count)")
                    .font(.system(size: 15))
                    .foregroundColor(Desk.dim)
                    .padding(.top, 2)
                    .padding(.bottom, 16)
                DeskBar(fraction: fraction, height: 11)
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 24)
            .frame(maxWidth: .infinity, alignment: .leading)
            .deskCard(radius: 30)
        }
        .onChange(of: stage) { value in
            stageSeen = value
            stageStarted = Date()
        }
        .onAppear {
            stageSeen = stage
            stageStarted = Date()
        }

        DeskHeading("Steps")
        VStack(spacing: 0) {
            ForEach(Array(Self.stages.enumerated()), id: \.offset) { i, title in
                HStack {
                    Text(title)
                        .font(.system(size: 16, weight: i == stage ? .semibold : .regular))
                        .foregroundColor(i > stage ? Desk.faint : Desk.text)
                    Spacer()
                    if i < stage {
                        Image(systemName: "checkmark").font(.system(size: 15, weight: .bold)).foregroundColor(Desk.good)
                    } else if i == stage {
                        ProgressView().scaleEffect(0.8)
                    }
                }
                .padding(.horizontal, 17)
                .padding(.vertical, 13)
                if i < Self.stages.count - 1 { Rectangle().fill(Desk.hairline).frame(height: 0.5) }
            }
        }
        .deskCard()

        Button(details ? "Hide details" : "Show details") { withAnimation { details.toggle() } }
            .font(.system(size: 15, weight: .semibold))
            .foregroundColor(Desk.accentLight)
            .padding(.top, 14)
        if details {
            VStack(spacing: 12) {
                if case .installing = installer.phase {
                    BootDisplay(display: display, role: .device)
                        .aspectRatio(16.0 / 10.0, contentMode: .fit)
                        .clipShape(RoundedRectangle(cornerRadius: 15, style: .continuous))
                }
                LogTail()
            }
            .padding(14)
            .deskCard()
            .padding(.top, 10)
        }
    }

    private func finished(_ text: String) -> some View {
        VStack(spacing: 14) {
            Image(systemName: "checkmark.circle.fill").font(.system(size: 54)).foregroundColor(Desk.good).padding(.top, 8)
            Text("Installed").font(.system(size: 24, weight: .bold))
            Text(text).font(.system(size: 15)).foregroundColor(Desk.dim).multilineTextAlignment(.center)
        }
        .padding(20)
        .frame(maxWidth: .infinity)
        .deskCard(radius: 30)
        .padding(.top, 8)
    }

    @ViewBuilder private func failed(_ text: String) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("It did not work").font(.system(size: 22, weight: .bold))
            Text(text).font(.system(size: 15))
            Text("Quit Henge (swipe it away) and open it again before trying again: only one Mac can start per launch.")
                .font(.system(size: 13)).foregroundColor(Desk.dim)
            Button("Back") { installer.reset() }.buttonStyle(DeskButtonStyle()).padding(.top, 6)
        }
        .padding(18)
        .deskCard(radius: 30)
        .padding(.top, 8)
        DeskHeading("Log")
        LogTail().padding(14).frame(maxWidth: .infinity, alignment: .leading).deskCard()
    }
}

/// The last lines of Henge's log.
private struct LogTail: View {
    @State private var lines: [String] = []

    var body: some View {
        VStack(alignment: .leading, spacing: 1) {
            ForEach(Array(lines.enumerated()), id: \.offset) { _, line in
                Text(line).font(.system(size: 9, design: .monospaced)).foregroundColor(.secondary).lineLimit(2)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .compatTask {
            while !Task.isCancelled {
                lines = Array(HengeLog.recentLines(14))
                try? await Task.sleep(nanoseconds: 1_000_000_000)
            }
        }
    }
}
