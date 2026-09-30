import Combine
import Foundation
import MaiChatCore

enum AIComposerSubmissionPolicy {
    static func submittedText(
        currentText: String,
        replacing range: NSRange,
        with replacementText: String
    ) -> String? {
        guard replacementText.last?.isNewline == true else { return nil }
        let current = currentText as NSString
        guard range.location <= current.length,
              range.length <= current.length - range.location
        else { return nil }
        var submitted = current.replacingCharacters(in: range, with: replacementText)
        while submitted.last?.isNewline == true { submitted.removeLast() }
        return submitted
    }
}

/// UITextView owns live editing; surrounding SwiftUI controls observe only
/// presentation changes. External replacements still invalidate the editor.
@MainActor
final class RemoteIMDraftState: @preconcurrency ObservableObject {
    let objectWillChange = ObservableObjectPublisher()
    private var storedQuote: RemoteIMQuote?
    private var storedText = ""
    private var presentation = Presentation(text: "")
    private var selectedConversation: String?
    private var draftsByConversation: [String: ConversationDraft] = [:]

    private struct ConversationDraft {
        let text: String
        let quote: RemoteIMQuote?
    }

    func selectConversation(ownerUserID: String, peerUserID: String) {
        let key = ownerUserID + "\u{0}" + peerUserID
        guard selectedConversation != key else { return }
        guard let previous = selectedConversation else {
            selectedConversation = key
            return
        }
        if storedText.isEmpty && storedQuote == nil {
            draftsByConversation.removeValue(forKey: previous)
        } else {
            draftsByConversation[previous] = ConversationDraft(text: storedText, quote: storedQuote)
        }
        selectedConversation = key
        let restored = draftsByConversation.removeValue(forKey: key)
        text = restored?.text ?? ""
        quote = restored?.quote
    }

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
