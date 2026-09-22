import Combine
import Foundation
import MaiChatCore

/// UITextView owns live editing; surrounding SwiftUI controls observe only
/// presentation changes. External replacements still invalidate the editor.
@MainActor
final class RemoteIMDraftState: @preconcurrency ObservableObject {
    let objectWillChange = ObservableObjectPublisher()
    private var storedQuote: RemoteIMQuote?
    private var storedText = ""
    private var presentation = Presentation(text: "")

    var text: String {
        get { storedText }
        set {
            guard newValue != storedText else { return }
            objectWillChange.send()
            storedText = newValue
            presentation = Presentation(text: newValue)
        }
    }

    var quote: RemoteIMQuote? {
        get { storedQuote }
        set {
            guard newValue != storedQuote else { return }
            objectWillChange.send()
            storedQuote = newValue
        }
    }

    func updateFromEditor(_ value: String) {
        guard value != storedText else { return }
        let next = Presentation(text: value)
        if next != presentation { objectWillChange.send() }
        storedText = value
        presentation = next
    }

    private struct Presentation: Equatable {
        let isEmpty: Bool
        let canSubmitText: Bool
        let commandQuery: String?

        init(text: String) {
            isEmpty = text.isEmpty
            canSubmitText = !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            let query = text.trimmingCharacters(in: .whitespaces)
            commandQuery = query.hasPrefix("/") ? query : nil
        }
    }
}
