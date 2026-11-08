// SPDX-License-Identifier: GPL-2.0-or-later
//
// One source tree, two deployment targets.
//
// The iOS 15/A9 build (docs/ios15-a9-support.md) has to compile against iOS 14
// APIs, so everything the app uses that arrived later lives here as a shim with
// an `#available` branch: iOS 16 and later get the real API, iOS 15 gets
// the nearest equivalent. The iOS 17/26 build uses the same shims and gets the
// real thing, so the two builds cannot drift apart.
import SwiftUI
import UIKit

// MARK: - Navigation (NavigationStack is iOS 16)

/// Pushes a destination for a value. iOS 16 and later go through the stack's
/// value-based navigation; before that the destination travels in the link.
struct CompatLink<Value: Hashable, Destination: View, Label: View>: View {
    let value: Value
    @ViewBuilder let destination: () -> Destination
    @ViewBuilder let label: () -> Label

    init(value: Value, @ViewBuilder destination: @escaping () -> Destination,
         @ViewBuilder label: @escaping () -> Label) {
        self.value = value
        self.destination = destination
        self.label = label
    }

    var body: some View {
        if #available(iOS 16.0, *) {
            NavigationLink(value: value, label: label)
        } else {
            NavigationLink(destination: destination(), label: label)
        }
    }
}

extension View {
    /// `navigationDestination(for:)` where it exists; below iOS 16 the link
    /// carries its destination itself (`CompatLink`), so there is nothing to
    /// register and this does nothing.
    @ViewBuilder func compatDestination<D: Hashable, C: View>(for data: D.Type,
                                                             @ViewBuilder destination: @escaping (D) -> C) -> some View {
        if #available(iOS 16.0, *) {
            navigationDestination(for: data, destination: destination)
        } else {
            self
        }
    }
}

struct CompatNavigationStack<Content: View>: View {
    private let content: Content

    init(@ViewBuilder content: () -> Content) {
        self.content = content()
    }

    var body: some View {
        if #available(iOS 16.0, *) {
            NavigationStack { content }
        } else {
            NavigationView { content }
                .navigationViewStyle(StackNavigationViewStyle())
        }
    }
}

// MARK: - Rows (LabeledContent is iOS 16)

/// A label on the left and its value on the right, in a List or a Form.
struct LCRow: View {
    let title: String
    let value: String

    init(_ title: String, _ value: String) {
        self.title = title
        self.value = value
    }

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 12) {
            Text(title)
            Spacer(minLength: 8)
            Text(value)
                .foregroundColor(.secondary)
                .multilineTextAlignment(.trailing)
        }
    }
}

// MARK: - Asynchronous work on appear (.task is iOS 15)

extension View {
    /// `.task`, spelled with the API every supported version has: the work
    /// starts when the view appears. (iOS 14's SwiftUI has no `task`.)
    func compatTask(_ action: @escaping () async -> Void) -> some View {
        onAppear { Task { await action() } }
    }

    /// Keeps the status bar and the home indicator as they are where the API exists.
    @ViewBuilder func compatHideSystemOverlays() -> some View {
        if #available(iOS 16.0, *) {
            persistentSystemOverlays(.hidden)
        } else {
            self
        }
    }

    /// A List's own background can only be dropped on iOS 16 and later.
    @ViewBuilder func compatHideScrollBackground() -> some View {
        if #available(iOS 16.0, *) {
            scrollContentBackground(.hidden)
        } else {
            self
        }
    }
}

// MARK: - One-tick timers (TimelineView is iOS 15)

/// Calls its content again every second, for the elapsed time on the install screen.
struct SecondTicker<Content: View>: View {
    private let content: (Date) -> Content
    @State private var now = Date()
    private let timer = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    init(@ViewBuilder content: @escaping (Date) -> Content) {
        self.content = content
    }

    var body: some View {
        content(now).onReceive(timer) { now = $0 }
    }
}

// MARK: - Sharing (ShareLink is iOS 16)

/// The system share sheet, from a URL.
struct ShareButton<Content: View>: View {
    let url: URL
    @ViewBuilder let content: () -> Content
    @State private var showing = false

    init(url: URL, @ViewBuilder content: @escaping () -> Content) {
        self.url = url
        self.content = content
    }

    var body: some View {
        Button { showing = true } label: { content() }
            .sheet(isPresented: $showing) { ActivityView(items: [url]) }
    }
}

struct ActivityView: UIViewControllerRepresentable {
    let items: [Any]

    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: items, applicationActivities: nil)
    }

    func updateUIViewController(_ controller: UIActivityViewController, context: Context) {}
}

// MARK: - Confirmation dialogs (confirmationDialog is iOS 15)

extension View {
    /// A destructive confirmation, with a Cancel button.
    func compatConfirm(_ title: String, message: String? = nil, isPresented: Binding<Bool>,
                       confirm: String, onConfirm: @escaping () -> Void) -> some View {
        Group {
            if #available(iOS 15.0, *) {
                confirmationDialog(title, isPresented: isPresented, titleVisibility: .visible) {
                    Button(confirm, role: .destructive, action: onConfirm)
                    Button("Cancel", role: .cancel) {}
                } message: {
                    if let message { Text(message) }
                }
            } else {
                alert(isPresented: isPresented) {
                    Alert(title: Text(title), message: message.map(Text.init),
                          primaryButton: .destructive(Text(confirm), action: onConfirm),
                          secondaryButton: .cancel(Text("Cancel")))
                }
            }
        }
    }
}

// MARK: - Buttons (Button(_:role:) is iOS 15)

/// A red button, for a row that deletes something.
struct DestructiveButton: View {
    let title: String
    let action: () -> Void

    init(_ title: String, action: @escaping () -> Void) {
        self.title = title
        self.action = action
    }

    var body: some View {
        Button(title, action: action).foregroundColor(.red)
    }
}

// MARK: - Dates (FormatStyle is iOS 15)

enum DateText {
    private static let formatter: DateFormatter = {
        let f = DateFormatter()
        f.locale = .current
        f.setLocalizedDateFormatFromTemplate("MMM d")
        return f
    }()

    static func monthDay(_ date: Date) -> String { formatter.string(from: date) }
}
