#include "config.h"
#include "EditorClient.h"

#include "Engine.h"
#include "WebPage.h"
#include <WebCore/DOMPasteAccess.h>
#include <WebCore/Document.h>
#include <WebCore/Editor.h>
#include <WebCore/BoundaryPointInlines.h>
#include <WebCore/Element.h>
#include <WebCore/ElementInlines.h>
#include <WebCore/EnterKeyHint.h>
#include <WebCore/FrameDestructionObserverInlines.h>
#include <WebCore/FrameSelection.h>
#include <WebCore/HTMLFrameOwnerElement.h>
#include <WebCore/HTMLInputElement.h>
#include <WebCore/HTMLTextAreaElement.h>
#include <WebCore/HTMLTextFormControlElement.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/NodeDocument.h>
#include <WebCore/SimpleRange.h>
#include <WebCore/TextCheckerClient.h>
#include <WebCore/TextIterator.h>
#include <WebCore/VisibleSelection.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(LumiaEditorClient);

namespace {

// Nothing checks spelling in this port (isContinuousSpellCheckingEnabled is false), but Editor dereferences the
// pointer in a few paths, so it returns no-op answers instead of null.
class EmptyTextChecker final : public TextCheckerClient {
public:
    bool shouldEraseMarkersAfterChangeSelection(TextCheckingType) const final { return true; }
    void ignoreWordInSpellDocument(const String&) final { }
    void learnWord(const String&) final { }
    void checkSpellingOfString(StringView, int*, int*) final { }
    void checkGrammarOfString(StringView, Vector<GrammarDetail>&, int*, int*) final { }
#if USE(UNIFIED_TEXT_CHECKING)
    Vector<TextCheckingResult> checkTextOfParagraph(StringView, OptionSet<TextCheckingType>, const VisibleSelection&) final { return { }; }
#endif
    void getGuessesForWord(const String&, const String&, const VisibleSelection&, Vector<String>&) final { }
    void requestCheckingOfString(TextCheckingRequest&, const VisibleSelection&) final { }
    void requestExtendedCheckingOfString(TextCheckingRequest&, const VisibleSelection&) final { }
};

EmptyTextChecker& emptyTextChecker()
{
    static NeverDestroyed<EmptyTextChecker> checker;
    return checker;
}

int kindOf(const Element& element)
{
    if (auto* input = dynamicDowncast<HTMLInputElement>(element)) {
        if (input->isPasswordField())
            return LW_FIELD_PASSWORD;
        if (input->isSearchField())
            return LW_FIELD_SEARCH;
        if (input->isEmailField())
            return LW_FIELD_EMAIL;
        if (input->isNumberField())
            return LW_FIELD_NUMBER;
        if (input->isTelephoneField())
            return LW_FIELD_PHONE;
        if (input->isURLField())
            return LW_FIELD_URL;
        return LW_FIELD_TEXT;
    }
    if (is<HTMLTextAreaElement>(element) || element.isRootEditableElement())
        return LW_FIELD_MULTILINE;
    return LW_FIELD_NONE;
}

// Whether the app's bar edits this element. WebCore's own answer (shouldUseInputMethod) is no for passwords on this
// platform: PasswordInputType keeps desktop input methods away from them. Here the bar is the only keyboard there is,
// and for a password it is a PasswordBox; taking WebCore's word made a password field lose the bar in the same
// instant it got the focus (textFieldDidBeginEditing announced it, setInputMethodState took it back).
bool isEditedByBar(Element& element)
{
    if (auto* input = dynamicDowncast<HTMLInputElement>(element); input && input->isPasswordField())
        return true;
    return element.shouldUseInputMethod();
}

} // namespace

// A contenteditable as an input method sees it (WebKit2's EditingRange, relative to the editable root): the root's plain
// text, where a <br> and each block boundary are one "\n", with the selection counted in that same text. The bar's
// text, its caret and the ranges it sends back (lw_text_edit) are then positions in one string. Before, the bar got
// textContent (no line breaks) and a caret always at 0, and put every new character at the start (Gmail, 2026-10-28).
// TextIterator lays the page out first: callers must not be inside WebCore's own selection or editing updates.
EditableText editableText(Element& root)
{
    EditableText result;
    auto scope = makeRangeSelectingNodeContents(root);
    result.text = plainText(scope);
    RefPtr frame = root.document().frame();
    auto selection = frame ? frame->selection().selection().firstRange() : std::nullopt;
    if (selection && contains(ComposedTree, scope, *selection)) {
        auto range = characterRange(makeBoundaryPointBeforeNodeContents(root), *selection);
        result.selectionStart = static_cast<int>(range.location);
        result.selectionEnd = static_cast<int>(range.location + range.length);
    }
    return result;
}

namespace {

// The enterkeyhint attribute, as the app's keyboard expects it (0 = newline/default, 1 = go, 2 = send, 3 = search,
// 4 = next, 5 = done). "previous" has no slot and falls back to the default.
int enterActionOf(const Element& element)
{
    auto* htmlElement = dynamicDowncast<HTMLElement>(element);
    if (!htmlElement)
        return 0;
    switch (htmlElement->canonicalEnterKeyHint()) {
    case EnterKeyHint::Go: return 1;
    case EnterKeyHint::Send: return 2;
    case EnterKeyHint::Search: return 3;
    case EnterKeyHint::Next: return 4;
    case EnterKeyHint::Done: return 5;
    case EnterKeyHint::Unspecified:
    case EnterKeyHint::Enter:
    case EnterKeyHint::Previous:
        break;
    }
    return 0;
}

} // namespace

LumiaEditorClient::LumiaEditorClient(WebPage& page)
    : m_page(page)
{
}

TextCheckerClient* LumiaEditorClient::textChecker()
{
    return &emptyTextChecker();
}

// A script reads the clipboard (navigator.clipboard.read*, execCommand("paste")) during a tap: what is there may come
// from another site, so the user is asked, once per tap. The clipboard is the engine's own (shims/wp81shims.c).
DOMPasteAccessResponse LumiaEditorClient::requestDOMPasteAccess(DOMPasteAccessCategory, FrameIdentifier, const String&)
{
    if (!m_page)
        return DOMPasteAccessResponse::DeniedForGesture;
    auto answer = Lumia::runDialog(m_page->view(), LW_DIALOG_CONFIRM, "Questa pagina vuole leggere il testo copiato in Internet (gli appunti). Lo permetti?"_s, { });
    return answer ? DOMPasteAccessResponse::GrantedForGesture : DOMPasteAccessResponse::DeniedForGesture;
}

void LumiaEditorClient::setInputMethodState(Element* element)
{
    // FocusController calls this with the newly focused element (any element, not just editables) and with null when
    // the focus is cleared. Tapping plain text or a link also arrives here as "not editable", i.e. as a blur.
    RefPtr<Element> editable;
    if (element && isEditedByBar(*element))
        editable = element;
    else if (element && is<HTMLFrameOwnerElement>(*element)) {
        // The focus is moving into a child frame: the editable inside it calls us again right away. Reporting a blur
        // here would close and reopen the compose bar for nothing.
        return;
    }

    if (editable == m_focusedElement.get()) {
        // FocusController calls this again for an element that is already focused (focus()/blur() round trips):
        // only real transitions reach the app, or the bar would rebuild itself.
        return;
    }

    m_focusedElement = editable;
    postField(LW_EVENT_FIELD_FOCUS, editable.get());
}

void LumiaEditorClient::textFieldDidBeginEditing(Element& element)
{
    // <input> only (TextFieldInputType::handleFocusEvent); setInputMethodState has normally announced it already and
    // the pointer comparison makes this a no-op unless it did not.
    if (m_focusedElement.get() == &element)
        return;
    m_focusedElement = &element;
    postField(LW_EVENT_FIELD_FOCUS, &element);
}

void LumiaEditorClient::textFieldDidEndEditing(Element& element)
{
    if (m_focusedElement.get() != &element)
        return;
    m_focusedElement = nullptr;
    postField(LW_EVENT_FIELD_FOCUS, nullptr);
}

void LumiaEditorClient::textDidChangeInTextField(Element& element)
{
    if (m_focusedElement.get() == &element)
        updateFocusedField();
}

void LumiaEditorClient::textDidChangeInTextArea(Element& element)
{
    if (m_focusedElement.get() == &element)
        updateFocusedField();
}

void LumiaEditorClient::respondToChangedContents()
{
    updateFocusedField();
}

void LumiaEditorClient::respondToChangedSelection(LocalFrame*)
{
    updateFocusedField();
}

void LumiaEditorClient::postField(lw_event_type type, Element* element)
{
    if (!m_page)
        return;

    lw_field field { };
    String text;
    if (element) {
        field.kind = kindOf(*element);
        if (field.kind == LW_FIELD_NONE)
            element = nullptr;
    }

    if (element) {
        // boundingBoxInRootViewCoordinates(), not boundsInRootViewSpace(): the latter calls updateLayout(), and this
        // runs from respondToChangedSelection, i.e. potentially in the middle of WebCore's own selection update.
        // Forcing a layout from there is both expensive and re-entrant; the rect is still the one the page has.
        // Same space as lw_input_tap: root view coordinates, i.e. CSS pixels of the view the app is showing.
        auto rect = element->boundingBoxInRootViewCoordinates();
        field.x = rect.x();
        field.y = rect.y();
        field.width = rect.width();
        field.height = rect.height();
        field.enter_action = enterActionOf(*element);

        if (auto* control = dynamicDowncast<HTMLTextFormControlElement>(*element)) {
            text = control->value();
            field.selection_start = static_cast<int>(control->selectionStart());
            field.selection_end = static_cast<int>(control->selectionEnd());
            field.max_length = control->maxLength();
        } else {
            // contenteditable. A focus arrives from inside FocusController, where laying the page out is not safe: the
            // bar opens empty and the text with its caret follows from the timer, a moment later.
            if (type == LW_EVENT_FIELD_FOCUS)
                m_editableUpdateTimer.startOneShot(0_s);
            else {
                auto state = editableText(*element);
                text = WTF::move(state.text);
                field.selection_start = state.selectionStart;
                field.selection_end = state.selectionEnd;
            }
            field.max_length = -1;
        }
        field.sequence = m_page->lastTextSequence();
    } else
        m_editableUpdateTimer.stop();

    if (type == LW_EVENT_FIELD_FOCUS)
        Lumia::log("campo: focus kind=%d testo=%d sel=%d..%d rect=%dx%d+%d+%d entra=%d max=%d seq=%u",
            field.kind, static_cast<int>(text.length()), field.selection_start, field.selection_end,
            field.width, field.height, field.x, field.y, field.enter_action, field.max_length, field.sequence);
    else if (Lumia::traceEnabled()) {
        // One line per keystroke on the Web thread: only when the tracer is on (same switch as the input hops).
        Lumia::log("campo: update testo=%d sel=%d..%d seq=%u",
            static_cast<int>(text.length()), field.selection_start, field.selection_end, field.sequence);
    }

    m_lastText = text;
    m_lastSelectionStart = field.selection_start;
    m_lastSelectionEnd = field.selection_end;
    m_lastKind = field.kind;

    Lumia::postFieldEvent(m_page->view(), type, field, text);
}

void LumiaEditorClient::updateFocusedField()
{
    RefPtr element = m_focusedElement.get();
    if (!element || !m_page)
        return;
    // One lw_text_edit sets the range, inserts and sets the caret again: the intermediate states are not worth an
    // event (and pay for a string copy each). lw_text_edit lifts this and asks for one update at the end.
    if (m_page->textEditInProgress())
        return;

    // The page can move the focus while it is inside an edit: report what is focused now, not what was. No
    // shouldUseInputMethod() here on purpose: it calls computeEditability(..., Update) and can force a style update
    // from inside a selection change. The focus hook already guarantees that this element was editable.
    if (RefPtr document = element->document())
        if (RefPtr focused = document->focusedElement())
            element = focused;
    if (element != m_focusedElement.get())
        return;
    // After a blur WebCore keeps reporting selection changes (the selection moves to the body, or to whatever the page
    // focuses next): an update for an element that no longer has the focus is worthless — it was the sel=0..0 lines
    // arriving right after the bar had already been closed.
    if (!element->focused())
        return;

    auto* control = dynamicDowncast<HTMLTextFormControlElement>(*element);
    if (!control) {
        // contenteditable: measured after WebCore has finished what it is doing; a burst of selection changes (a
        // page script rewriting its editor) costs one measurement.
        m_editableUpdateTimer.startOneShot(0_s);
        return;
    }

    // Called on every caret move and every character: only changes the app has not seen yet are worth an event.
    int selectionStart = static_cast<int>(control->selectionStart());
    int selectionEnd = static_cast<int>(control->selectionEnd());
    if (control->value() == m_lastText && selectionStart == m_lastSelectionStart && selectionEnd == m_lastSelectionEnd)
        return;

    postField(LW_EVENT_FIELD_UPDATE, element.get());
}

void LumiaEditorClient::editableUpdateTimerFired()
{
    RefPtr element = m_focusedElement.get();
    if (!element || !m_page || !element->focused() || is<HTMLTextFormControlElement>(*element))
        return;
    // An edit of the bar's is being applied right now (a page handler spun the run loop): its own update follows.
    if (m_page->textEditInProgress())
        return;
    auto state = editableText(*element);
    if (state.text == m_lastText && state.selectionStart == m_lastSelectionStart && state.selectionEnd == m_lastSelectionEnd)
        return;
    postField(LW_EVENT_FIELD_UPDATE, element.get());
}

} // namespace WebKit
