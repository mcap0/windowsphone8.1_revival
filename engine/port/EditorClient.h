// WebCore::EditorClient of a tab: the hooks that tell the app when an editable element got or lost the focus and when
// its text or selection changed (this WebKit tag has no EditorState, so the field is described by the port itself
// from the focused Element).
//
// Focus is announced through setInputMethodState(), the only hook FocusController calls for <input>, <textarea> and
// contenteditable alike (FocusController.cpp: setFocusedElement -> setInputMethodState). textFieldDidBeginEditing()
// fires for <input> only, so it is kept as a secondary trigger, not as the source of truth.
//
// Written standalone instead of deriving from EmptyEditorClient because that class is final and lives inside
// EmptyClients.cpp: moving it to EmptyClients.h would rebuild every WebCore translation unit that includes the header
// (the same 30-minute rebuild as RunLoop.h), while this file costs one minute.
#pragma once

#include <WebCore/EditorClient.h>
#include <WebCore/EventTarget.h>
#include <WebCore/Timer.h>
#include <lumiaweb.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WeakPtr.h>
#include <wtf/text/WTFString.h>

namespace WebCore {
class Document;
class Element;
class LocalFrame;
}

namespace WebKit {

class WebPage;

// A contenteditable's text and selection in the units the bar works in (EditorClient.cpp: the plain text of the
// editable root, the selection counted in it). Lays the page out first.
struct EditableText {
    String text;
    int selectionStart { -1 }; // -1: the selection is not inside the root
    int selectionEnd { -1 };
};
EditableText editableText(WebCore::Element& root);

class LumiaEditorClient final : public WebCore::EditorClient {
    WTF_MAKE_TZONE_ALLOCATED(LumiaEditorClient);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(LumiaEditorClient);
public:
    explicit LumiaEditorClient(WebPage&);

private:
    bool shouldDeleteRange(const std::optional<WebCore::SimpleRange>&) override { return true; }
    bool smartInsertDeleteEnabled() override { return false; }
    bool isSelectTrailingWhitespaceEnabled() const override { return false; }
    bool isContinuousSpellCheckingEnabled() override { return false; }
    void toggleContinuousSpellChecking() override { }
    bool isGrammarCheckingEnabled() override { return false; }
    void toggleGrammarChecking() override { }
    int spellCheckerDocumentTag() override { return -1; }

    bool shouldBeginEditing(const WebCore::SimpleRange&) override { return true; }
    bool shouldEndEditing(const WebCore::SimpleRange&) override { return true; }
    bool shouldInsertNode(WebCore::Node&, const std::optional<WebCore::SimpleRange>&, WebCore::EditorInsertAction) override { return true; }
    bool shouldInsertText(const String&, const std::optional<WebCore::SimpleRange>&, WebCore::EditorInsertAction) override { return true; }
    bool shouldChangeSelectedRange(const std::optional<WebCore::SimpleRange>&, const std::optional<WebCore::SimpleRange>&, WebCore::Affinity, bool) override { return true; }
    bool shouldApplyStyle(const WebCore::StyleProperties&, const std::optional<WebCore::SimpleRange>&) override { return true; }
    void didApplyStyle() override { }
    bool shouldMoveRangeAfterDelete(const WebCore::SimpleRange&, const WebCore::SimpleRange&) override { return true; }

    void didBeginEditing() override { }
    void respondToChangedContents() override;
    void respondToChangedSelection(WebCore::LocalFrame*) override;
    void didEndUserTriggeredSelectionChanges() override { }
    void updateEditorStateAfterLayoutIfEditabilityChanged() override { }
    void didEndEditing() override { }
    void willWriteSelectionToPasteboard(const std::optional<WebCore::SimpleRange>&) override { }
    void didWriteSelectionToPasteboard() override { }
    void getClientPasteboardData(const std::optional<WebCore::SimpleRange>&, Vector<std::pair<String, RefPtr<WebCore::SharedBuffer>>>&) override { }

    WebCore::DOMPasteAccessResponse requestDOMPasteAccess(WebCore::DOMPasteAccessCategory, WebCore::FrameIdentifier, const String&) override;
    void discardedComposition(const WebCore::Document&) override { }
    void canceledComposition() override { }
    void didUpdateComposition() override { }

    void registerUndoStep(WebCore::UndoStep&) override { }
    void registerRedoStep(WebCore::UndoStep&) override { }
    void clearUndoRedoOperations() override { }

    bool canCopyCut(WebCore::LocalFrame*, bool defaultValue) const override { return defaultValue; }
    bool canPaste(WebCore::LocalFrame*, bool defaultValue) const override { return defaultValue; }
    bool canUndo() const override { return false; }
    bool canRedo() const override { return false; }
    void undo() override { }
    void redo() override { }

    void handleKeyboardEvent(WebCore::KeyboardEvent&) override { }
    void handleInputMethodKeydown(WebCore::KeyboardEvent&) override { }

    void textFieldDidBeginEditing(WebCore::Element&) override;
    void textFieldDidEndEditing(WebCore::Element&) override;
    void textDidChangeInTextField(WebCore::Element&) override;
    bool doTextFieldCommandFromEvent(WebCore::Element&, WebCore::KeyboardEvent*) override { return false; }
    void textWillBeDeletedInTextField(WebCore::Element&) override { }
    void textDidChangeInTextArea(WebCore::Element&) override;
    void overflowScrollPositionChanged() override { }
    void subFrameScrollPositionChanged() override { }

    WebCore::TextCheckerClient* textChecker() override;
    void updateSpellingUIWithGrammarString(const String&, const WebCore::GrammarDetail&) override { }
    void updateSpellingUIWithMisspelledWord(const String&) override { }
    void showSpellingUI(bool) override { }
    bool spellingUIIsShowing() override { return false; }
    void setInputMethodState(WebCore::Element*) override;

    bool performTwoStepDrop(WebCore::DocumentFragment&, const WebCore::SimpleRange&, bool) override { return false; }

    // One LW_EVENT_FIELD_FOCUS (or UPDATE) out of the focused element, if it is still the one the app knows about.
    void postField(lw_event_type, WebCore::Element*);
    // LW_EVENT_FIELD_UPDATE when the text or the selection differ from the last one the app was told. A
    // contenteditable's state is measured with TextIterator, which lays the page out: never from inside WebCore's own
    // selection or editing updates, so for that kind this only arms m_editableUpdateTimer.
    void updateFocusedField();
    void editableUpdateTimerFired();

    WeakPtr<WebPage> m_page;
    // Element's weak pointer uses EventTarget's impl (EventTarget.h): the default one does not compile for it.
    WeakPtr<WebCore::Element, WebCore::WeakPtrImplWithEventTargetData> m_focusedElement;
    String m_lastText;
    int m_lastSelectionStart { -1 };
    int m_lastSelectionEnd { -1 };
    int m_lastKind { LW_FIELD_NONE };
    WebCore::Timer m_editableUpdateTimer { *this, &LumiaEditorClient::editableUpdateTimerFired };
};

} // namespace WebKit
