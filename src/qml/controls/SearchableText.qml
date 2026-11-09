import QtQuick
import QtQuick.Controls
import Ripose.Memento

TextEdit {
    id: root

    /* Shape of the cursor when hovering over text */
    property alias cursorShape: mouseArea.cursorShape

    /* The index into plainText that is currently hovered */
    property int hoverIndex: -1

    /* Start of the token used for the latest dictionary search */
    property int searchStartIndex: -1

    /* The current mouse position */
    property point mousePosition: Qt.point(0, 0)

    /* The unformatted text string */
    readonly property string plainText: root.getText(0, root.length)

    /**
     * Returns the index into plain text of the coordinates.
     * @param x The x-coordinate of the TextEdit.
     * @param y The y-coordinate of the TextEdit.
     * @return The index of the character.
     */
    function getIndexAt(x, y) {
        const index = root.positionAt(x, y);
        const rect = root.positionToRectangle(index);
        return (x < rect.x) ? index - 1 : index;
    }

    /**
     * Returns whether a character is part of a searchable word token.
     * Cased Unicode letters include Latin and Cyrillic; Japanese scripts stay
     * character-indexed so their dictionary/deconjugation handling is kept.
     */
    function isWordCharacter(character) {
        if (!character)
        {
            return false;
        }
        const lower = character.toLowerCase();
        const upper = character.toUpperCase();
        return lower !== upper ||
               (character >= "0" && character <= "9") ||
               character === "'" || character === "’" ||
               character === "-";
    }

    /** Return the beginning of the word under index, or index for punctuation. */
    function getWordStart(index) {
        const text = root.plainText;
        if (!root.isWordCharacter(text.charAt(index)))
        {
            return index;
        }
        let start = index;
        while (start > 0 && root.isWordCharacter(text.charAt(start - 1)))
        {
            --start;
        }
        return start;
    }

    /**
     * Executes a search at a given plain text index.
     * @param index The index into plain text to search.
     */
    function searchIndex(index) {
        if (index < 0 || index >= root.length)
        {
            return;
        }

        switch (popupLoader.status)
        {
        case Loader.Null:
            popupLoader.active = true;
            return;

        case Loader.Ready:
            break;

        default:
        case Loader.Loading:
            return;
        }

        const MAX_SEARCH_LENGTH = 40;
        const text = root.plainText;
        const searchStart = root.getWordStart(index);
        root.searchStartIndex = searchStart;

        popupLoader.item.search.searchTerms(
                    text.substring(searchStart, searchStart + MAX_SEARCH_LENGTH),
                    text,
                    searchStart);
        popupLoader.item.search.searchKanji(
                    text.charAt(index),
                    text,
                    index);

        popupLoader.item.parent = Overlay.overlay;

        const rect = root.mapToItem(null, root.positionToRectangle(searchStart));
        let x = rect.x - (popupLoader.item.width / 2);
        x = Math.max(x, 0);
        x = Math.min(x, Overlay.overlay.width - popupLoader.item.width);
        let y = rect.y - popupLoader.item.height;
        if (y < 0)
        {
            y = rect.y + rect.height;
        }
        popupLoader.item.x = x;
        popupLoader.item.y = y;
    }

    readOnly: true
    selectByMouse: false
    selectByKeyboard: false
    textFormat: TextEdit.PlainText
    color: MementoPalette.text
    selectionColor: MementoPalette.highlight

    onHoverIndexChanged: {
        if (!KeyTracker.modifierHeld(MementoSettings.searchModifier))
        {
            return;
        }
        Qt.callLater(root.searchIndex, root.hoverIndex);
    }

    Connections {
        target: KeyTracker
        function onModifiersChanged() {
            if (hoverHandler.hovered &&
                    KeyTracker.modifierHeld(MementoSettings.searchModifier))
            {
                Qt.callLater(root.searchIndex, root.hoverIndex);
            }
        }
    }

    Connections {
        target: popupLoader.item
        function onClosed() {
            root.deselect();
        }
    }

    Connections {
        target: popupLoader.item?.search ?? null

        function onTermsChanged() {
            const start = root.searchStartIndex >= 0 ?
                              root.searchStartIndex : root.hoverIndex;
            root.select(start, start + popupLoader.item.matchLength);
        }

        function onKanjiChanged() {
            root.select(root.hoverIndex,
                        root.hoverIndex + popupLoader.item.matchLength);
        }
    }

    HoverHandler {
        id: hoverHandler
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        propagateComposedEvents: true
        cursorShape: Qt.ArrowCursor
        acceptedButtons: Qt.MiddleButton

        onPositionChanged: function(event) {
            root.mousePosition = Qt.point(event.x, event.y);
            root.hoverIndex = root.getIndexAt(event.x, event.y);
            event.accepted = false;
        }

        onClicked: function(event) {
            if (!MementoSettings.searchMiddleMouseScan)
            {
                event.accepted = false;
                return;
            }
            Qt.callLater(root.searchIndex, root.hoverIndex);
        }

        onExited: root.hoverIndex = -1
    }

    Loader {
        id: popupLoader
        active: false
        source: "qrc:/qt/qml/Ripose/Memento/qml/definition/DefinitionPopup.qml"
        onLoaded: Qt.callLater(root.searchIndex, root.hoverIndex)
    }
}
