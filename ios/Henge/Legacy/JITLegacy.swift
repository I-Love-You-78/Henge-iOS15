// SPDX-License-Identifier: GPL-2.0-or-later
//
// The JIT flow for the iOS 15/A9 build.
//
// The stock app's JIT screen is written around what arrived with later iOS:
// StikDebug (iOS 17.4 and later), the built-in StikJIT helper app extension
// (iOS 26, which needs a debugger to hand out executable memory at all because
// of TXM), and on-device pairing (iOS 27). None of that exists on iOS 14–16,
// where a jailbroken or TrollStore-installed app can be given executable memory
// directly: `dynamic-codesigning`, TrollStore's "enable JIT", or a jailbreak's
// "Allow JIT in Apps" setting. So this build has its own, much shorter, screen,
// and JITBootstrap.prewarm() does the work — it already had the pre-iOS-26
// self-grant path, which is the only one that applies here.
#if HENGE_LEGACY
import SwiftUI
import UIKit

enum JITMethod: String, CaseIterable, Identifiable {
    case automatic
    case trollStore
    case jailbreak

    var id: String { rawValue }

    var title: String {
        switch self {
        case .automatic: return "Automatic"
        case .trollStore: return "TrollStore"
        case .jailbreak: return "Jailbreak"
        }
    }

    var detail: String {
        switch self {
        case .automatic: return "Henge tries whatever this device offers."
        case .trollStore: return "TrollStore's enable JIT gives Henge executable memory."
        case .jailbreak: return "A jailbreak with \"Allow JIT in Apps\" on, or a debugger attached."
        }
    }
}

/// What the app asks of the JIT, for this build: the two members the rest of the
/// app binds to, and a way to turn JIT on now.
final class JITCoordinator: ObservableObject {
    static let shared = JITCoordinator()
    static let autoEnableKey = "henge.jit.autoEnable"
    private static let methodKey = "henge.jitMethod"

    @Published var showSetup = false
    @Published var attachGeneration = 0
    @Published var status: String?
    @Published var busy = false
    @Published var method: JITMethod {
        didSet { UserDefaults.standard.set(method.rawValue, forKey: Self.methodKey) }
    }

    private init() {
        let saved = UserDefaults.standard.string(forKey: Self.methodKey) ?? JITMethod.automatic.rawValue
        method = JITMethod(rawValue: saved) ?? .automatic
    }

    var automaticDescription: String {
        if JITBootstrap.ready { return "JIT is on. Tap Turn On JIT on the Macs tab if a Mac still says it is off." }
        guard JITBootstrap.canGrantOwnJIT else {
            return "This iOS only gives executable memory to an app with a debugger attached."
        }
        if JITBootstrap.isInstalledWithTrollStore {
            return "Installed with TrollStore: tap Turn On JIT, or use TrollStore's Enable JIT on Henge."
        }
        if JITBootstrap.isTrollStoreInstalled {
            return "TrollStore is on this device: tap Turn On JIT and let TrollStore enable it for Henge."
        }
        if JITBootstrap.isJailbroken {
            return "Jailbroken: turn on \"Allow JIT in Apps\" in your jailbreak's settings, then tap Turn On JIT."
        }
        return "Henge needs executable memory. Install it with TrollStore, from a jailbreak, or with a sideloader that can hand out JIT (SideStore with its JIT option)."
    }

    /// Turn JIT on now: hand off to TrollStore when that is the only route, else
    /// make the region here (jailbreak, dynamic-codesigning, TrollStore's attach).
    @MainActor
    func enable() {
        guard !busy else { return }
        if JITBootstrap.ready {
            status = "JIT is on."
            return
        }
        busy = true
        status = "Turning on JIT…"
        if method != .jailbreak, !JITBootstrap.mapJITWorks, JITBootstrap.isTrollStoreInstalled || JITBootstrap.isInstalledWithTrollStore {
            if JITBootstrap.requestTrollStoreAttach() {
                status = "TrollStore is enabling JIT for Henge. Come back when it has."
                busy = false
                attachGeneration += 1
                return
            }
        }
        if JITBootstrap.prewarm() {
            status = "JIT is on."
        } else {
            status = JITBootstrap.lastFailure ?? "No executable memory yet. Try the route for this device above."
        }
        busy = false
    }

    func enableAtLaunchIfAsked() {
        guard UserDefaults.standard.bool(forKey: Self.autoEnableKey) else { return }
        Task { @MainActor in enable() }
    }
}

/// The sheet behind "Set up JIT": what this iOS needs, and one button.
struct JITSetupFlow: View {
    @ObservedObject private var jit = JITCoordinator.shared
    @Environment(\.presentationMode) private var presentationMode
    @State private var ready = JITBootstrap.ready

    var body: some View {
        CompatNavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    Text(ready ? "JIT is on" : "Turn on JIT")
                        .font(.system(size: 26, weight: .semibold))
                    Text(jit.automaticDescription)
                        .font(.system(size: 15))
                        .foregroundColor(.secondary)

                    VStack(alignment: .leading, spacing: 10) {
                        step(1, "Install Henge with TrollStore, or from a jailbreak. A plain sideload gives an app no way to make executable memory on these iOS versions.")
                        step(2, "Turn JIT on for Henge: TrollStore › Henge › Enable JIT, or your jailbreak's \"Allow JIT in Apps\".", ready)
                        step(3, "Open Henge and start a Mac.", ready)
                    }
                    .padding(14)
                    .background(RoundedRectangle(cornerRadius: 16, style: .continuous).fill(Desk.card))

                    if let status = jit.status {
                        Text(status).font(.system(size: 13)).foregroundColor(Theme.textDim)
                    }
                    if let failure = JITBootstrap.lastFailure, !ready {
                        Text(failure).font(.system(size: 13)).foregroundColor(.orange)
                    }

                    Button(ready ? "Done" : "Turn On JIT") {
                        if ready {
                            presentationMode.wrappedValue.dismiss()
                        } else {
                            jit.enable()
                        }
                    }
                    .buttonStyle(PrimaryButtonStyle(enabled: !jit.busy))
                    .padding(.top, 4)
                }
                .padding(18)
            }
            .background(Desk.page)
            .navigationTitle("JIT")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { presentationMode.wrappedValue.dismiss() }
                }
            }
            .compatTask {
                while !Task.isCancelled {
                    ready = JITBootstrap.ready
                    try? await Task.sleep(nanoseconds: 1_000_000_000)
                }
            }
        }
    }

    private func step(_ number: Int, _ text: String, _ done: Bool = false) -> some View {
        HStack(alignment: .top, spacing: 10) {
            ZStack {
                Circle().fill(done ? Color.green.opacity(0.2) : Desk.card)
                    .frame(width: 22, height: 22)
                if done {
                    Image(systemName: "checkmark").font(.system(size: 11, weight: .bold)).foregroundColor(.green)
                } else {
                    Text("\(number)").font(.system(size: 12, weight: .bold)).foregroundColor(Theme.accent)
                }
            }
            Text(text).font(.system(size: 14)).foregroundColor(Theme.text)
            Spacer(minLength: 0)
        }
    }
}
#endif
