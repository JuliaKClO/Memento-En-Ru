import QtQuick
import QtQuick.Controls
import Ripose.Memento

MpvPlayer {
    id: root

    readonly property bool oscVisible:
        !root.ocrMode && (
            controls.visible ||
            (!Features.isMacos && menu.visible)
        )
    readonly property bool oscHovered:
        !root.ocrMode && (
            subtitleHover.hovered ||
            (!Features.isMacos && menu.anyHovered) ||
            controlsHover.hovered
        )

    readonly property bool pause: root.state.pause

    property point cursorPosition: Qt.point(0, 0)

    property bool ocrMode: false

    signal auxiliarySearchRequested(string text)

    property bool sentenceAssistantArmed: false
    property int sentenceAssistantCursorIndex: -1
    property string sentenceAssistantDisplayText: ""
    property string sentenceAssistantSourceText: ""
    property real sentenceAssistantCueStart: -1

    function rememberSentenceAssistantHover(index) {
        if (index < 0 || index >= subtitleText.text.length)
        {
            if (sentenceAssistantArmed)
            {
                sentenceAssistantButtonTimer.restart();
            }
            return;
        }

        const character = subtitleText.text.charAt(index);
        const isEnglishLetter = (character >= "a" && character <= "z") ||
            (character >= "A" && character <= "Z");
        if (!isEnglishLetter)
        {
            return;
        }

        sentenceAssistantCursorIndex = index;
        sentenceAssistantDisplayText = subtitleText.text;
        sentenceAssistantSourceText = root.state.subtitle.text;
        sentenceAssistantCueStart = root.state.subtitle.startTime;
        sentenceAssistantArmed = true;
        sentenceAssistantButtonTimer.restart();
    }

    function cleanSubtitleForAssistant(text) {
        let cleaned = text.replace(subtitleText.regexFilter, "");
        if (MementoSettings.searchReplaceNewlines)
        {
            cleaned = cleaned.replace(
                /\n/g, MementoSettings.searchReplaceNewlinesWith
            );
        }
        return cleaned;
    }

    function openSentenceAssistant() {
        if (!sentenceAssistantArmed ||
                sentenceAssistantCursorIndex < 0 ||
                sentenceAssistantDisplayText.length === 0)
        {
            return;
        }

        let subtitles = [];
        let currentIndex = 0;
        const model = SubtitleLists.primary;
        if (model)
        {
            const count = model.rowCount();
            let modelCurrentIndex = -1;
            let closestStartDelta = Number.MAX_VALUE;
            let closestTimestampIndex = -1;
            let closestTimestampDelta = Number.MAX_VALUE;
            const selectedRow = model.selectionModel?.currentIndex?.row ?? -1;
            if (selectedRow >= 0 && selectedRow < count)
            {
                const selectedModelIndex = model.index(selectedRow, 0);
                const selectedStart = Number(
                    model.data(selectedModelIndex, SubtitleListModel.StartRole)
                );
                const selectedText = String(
                    model.data(selectedModelIndex, SubtitleListModel.TextRole) ?? ""
                );
                if (selectedText === sentenceAssistantSourceText &&
                        Math.abs(selectedStart - sentenceAssistantCueStart) < 0.5)
                {
                    modelCurrentIndex = selectedRow;
                }
            }
            if (modelCurrentIndex < 0)
            {
                for (let i = 0; i < count; ++i)
                {
                    const modelIndex = model.index(i, 0);
                    const rowText = String(
                        model.data(modelIndex, SubtitleListModel.TextRole) ?? ""
                    );
                    const rowStart = Number(
                        model.data(modelIndex, SubtitleListModel.StartRole)
                    );
                    const delta = Math.abs(rowStart - sentenceAssistantCueStart);
                    if (delta < closestTimestampDelta)
                    {
                        closestTimestampDelta = delta;
                        closestTimestampIndex = i;
                    }
                    if (rowText === sentenceAssistantSourceText &&
                            delta < closestStartDelta)
                    {
                        closestStartDelta = delta;
                        modelCurrentIndex = i;
                    }
                }
            }
            if (modelCurrentIndex < 0 && closestTimestampDelta < 0.5)
            {
                modelCurrentIndex = closestTimestampIndex;
            }

            if (modelCurrentIndex >= 0)
            {
                const first = Math.max(0, modelCurrentIndex - 5);
                const last = Math.min(count - 1, modelCurrentIndex + 5);
                for (let i = first; i <= last; ++i)
                {
                    const rowText = String(model.data(
                        model.index(i, 0), SubtitleListModel.TextRole
                    ) ?? "");
                    subtitles.push(cleanSubtitleForAssistant(rowText));
                }
                currentIndex = modelCurrentIndex - first;
                subtitles[currentIndex] = sentenceAssistantDisplayText;
            }
        }

        if (subtitles.length === 0)
        {
            subtitles = [sentenceAssistantDisplayText];
            currentIndex = 0;
        }

        let sentence = SentenceAssistant.collectSentence(
            subtitles, currentIndex, sentenceAssistantCursorIndex
        );
        if (sentence.length === 0)
        {
            sentence = sentenceAssistantDisplayText.trim();
        }
        const prompt = SentenceAssistant.buildPrompt(
            sentence, MementoSettings.searchAiPrompt
        );
        if (prompt.length === 0)
        {
            root.controller.showText(qsTr("Не удалось определить текст предложения"));
            return;
        }

        sentenceAssistantClipboard.setText(prompt);
        let assistantUrl = "https://gemini.google.com/app";
        switch (MementoSettings.searchAiAssistant)
        {
            case MementoSetting.AiAssistantChatGPT:
                assistantUrl = "https://chatgpt.com/";
                break;
            case MementoSetting.AiAssistantAlice:
                assistantUrl = "https://alice.yandex.ru/chat/0/";
                break;
            case MementoSetting.AiAssistantClaude:
                assistantUrl = "https://claude.ai/";
                break;
        }
        const opened = Qt.openUrlExternally(assistantUrl);
        root.controller.showText(
            opened ?
                qsTr("Запрос скопирован. Вставьте его в открывшийся чат.") :
                qsTr("Запрос скопирован. Откройте выбранный чат и вставьте его вручную.")
        );
        sentenceAssistantArmed = false;
        sentenceAssistantButtonTimer.stop();
    }

    enum OscVisibility
    {
        OscAuto,
        OscVisible,
        OscHidden
    }

    property int oscVisibility: Player.OscAuto

    /**
     * Shows the OSC with fade in.
     */
    function showOsc() {
        if (root.ocrMode)
        {
            return;
        }

        if (root.oscVisibility === Player.OscHidden)
        {
            return;
        }

        if (!Features.isMacos)
        {
            menuFadeOut.stop();
            menuFadeIn.start();
        }
        controlsFadeOut.stop();
        controlsFadeIn.start();
    }

    /**
     * Hides the OSC with fade out.
     *
     * @param force Unconditionally hide the OSC if true.
     */
    function hideOsc(force) {
        if (!force && root.oscVisibility === Player.OscVisible)
        {
            return;
        }

        if (!Features.isMacos)
        {
            menuFadeIn.stop();
            menuFadeOut.start();
        }
        controlsFadeIn.stop();
        controlsFadeOut.start();
    }

    /**
     * Put the player into OCR mode.
     */
    function startOcrMode() {
        ocrOverlay.start();
    }

    /**
     * Get the player out of OCR mode.
     */
    function cancelOcrMode() {
        ocrOverlay.cancel();
    }

    /**
     * Handles mouse movement.
     */
    function mouseMoved(x, y) {
        root.controller.sendMouse(x, y);
        cursorTimer.restart();
    }

    onFileLoaded: {
        if (MementoSettings.searchHideMpvSubs)
        {
            root.controller.setSubtitleVisibility(false);
        }
    }

    Keys.onPressed: function(event) {
        if (root.ocrMode && event.key === Qt.Key_Escape)
        {
            root.cancelOcrMode();
            event.accepted = true;
            return;
        }
        root.controller.sendKeyPress(event.key, event.modifiers);
    }

    onCursorPositionChanged: root.mouseMoved(root.cursorPosition.x, root.cursorPosition.y)

    onOscVisibilityChanged: {
        switch (root.oscVisibility)
        {
        case Player.OscAuto:
            interactiveTimer.restart();
            break;

        case Player.OscVisible:
            root.showOsc();
            break;

        case Player.OscHidden:
            root.hideOsc();
            break;
        }
    }

    Action {
        id: cycleOscVisibilityAction
        shortcut: MementoSettings.keybinds.profile?.oscVisibility
        onTriggered: {
            const oscVisibilityText = qsTr("OSC Visibility: %1");

            switch (root.oscVisibility)
            {
            case Player.OscAuto:
                root.oscVisibility = Player.OscVisible;
                root.controller.showText(oscVisibilityText.arg(qsTr("Visible")));
                break;

            case Player.OscVisible:
                root.oscVisibility = Player.OscHidden;
                root.controller.showText(oscVisibilityText.arg(qsTr("Hidden")));
                break;

            case Player.OscHidden:
                root.oscVisibility = Player.OscAuto;
                root.controller.showText(oscVisibilityText.arg(qsTr("Auto")));
                break;
            }
        }
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        propagateComposedEvents: true
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        cursorShape: root.oscHovered ? Qt.ArrowCursor : cursorTimer.cursorShape
        enabled: !root.ocrMode

        onPositionChanged: function(event) {
            root.cursorPosition = Qt.point(event.x, event.y);
            event.accepted = false;
        }

        onClicked: function(event) {
            /* This hack makes it so keys are still handled and the player has focus */
            root.forceActiveFocus();
            root.focus = false;
            root.forceActiveFocus();

            definitionPopup.clearResults();
            root.controller.sendMouseButton(event.x, event.y, event.button, true);
            event.accepted = false;
        }

        onDoubleClicked: function(event) {
            root.controller.sendMouseButton(event.x, event.y, event.button, false);
            event.accepted = false;
        }
    }

    /* This handler is needed because the itemCursor Items block
     * onPositionChanged in MouseArea */
    HoverHandler {
        id: hoverHandler

        readonly property point position: hoverHandler.point.position

        onPositionChanged: root.cursorPosition = Qt.point(position.x, position.y)
    }

    WheelHandler {
        enabled: !root.ocrMode
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function(event) {
            root.controller.sendWheel(event.x, event.y, event.angleDelta);
        }
    }

    Timer {
        id: interactiveTimer
        interval: MementoSettings.behaviorOscDuration
        repeat: false
        running: false
        onTriggered: {
            if (!root.oscHovered && !definitionPopup.visible)
            {
                root.hideOsc();
            }
        }
    }

    MoveTimer {
        id: minMouseMoveTimer
        running: !root.ocrMode
        minMovement: MementoSettings.behaviorOscMinMove
        cursorPosition: root.cursorPosition
        onMouseMoved: {
            interactiveTimer.restart();
            if (!root.oscVisible)
            {
                root.showOsc();
            }
        }
    }

    CursorTimer {
        id: cursorTimer
        player: root
    }

    onOscHoveredChanged: {
        interactiveTimer.restart();
        cursorTimer.restart();
    }

    /* Inhibit the screensaver during playback */
    Screensaver {
        id: screensaver
    }
    onPauseChanged: {
        if (root.state.pause)
        {
            screensaver.uninhibit();
        }
        else
        {
            screensaver.inhibit(qsTr("Playing media"));
        }
    }

    /* Implements subtitle cursor show */
    Item {
        id: itemCursorSubtitleShow

        readonly property bool show: hoverCursorSubtitleShow.hovered ||
                                     subtitleHover.hovered ||
                                     subtitleText.hoverIndex !== -1 || // This prevents flashing on edges
                                     controlsHover.hovered

        anchors {
            left: root.left
            right: root.right
            bottom: root.bottom
        }
        height: root.height * 0.25
        focusPolicy: Qt.NoFocus
        visible: MementoSettings.behaviorSubtitleCursorShow

        HoverHandler {
            id: hoverCursorSubtitleShow
        }
    }

    /* Implements secondary subtitle cursor show */
    Item {
        id: itemCursorSecondarySubtitleShow

        readonly property bool show:
            hoverCursorSecondarySubtitleShow.hovered ||
            (!Features.isMacos && menu.anyHovered)

        anchors {
            left: root.left
            right: root.right
            top: root.top
        }
        height: root.height * 0.25
        focusPolicy: Qt.NoFocus
        visible: MementoSettings.behaviorSecondarySubtitleCursorShow

        HoverHandler {
            id: hoverCursorSecondarySubtitleShow
        }

        /**
         * Toggles secondary subtitle visibility.
         */
        function updateSeconarySubtitleVisibility() {
            if (itemCursorSecondarySubtitleShow.visible)
            {
                root.controller.setSecondarySubtitleVisibility(show);
            }
        }

        onShowChanged: updateSeconarySubtitleVisibility()
        onVisibleChanged: updateSeconarySubtitleVisibility()
    }

    Popup {
        id: definitionPopup

        property point cursorAtSearch: Qt.point(0, 0)
        property int lastHoverIndex: -1

        readonly property int playerWidth: root.width
        readonly property int playerHeight: root.height
        readonly property bool paused: root.state.pause
        readonly property real position: controls.position

        readonly property bool shouldOpen: dictionarySearch.terms.length > 0 || dictionarySearch.kanji

        x: {
            let idealPosition = cursorAtSearch.x - (width / 2);
            idealPosition = Math.max(idealPosition, 0);
            idealPosition = Math.min(idealPosition, root.width - width);
            return idealPosition;
        }
        y: {
            let idealPosition = subtitleText.y - height;
            if (idealPosition < 0)
            {
                idealPosition = subtitleText.y + subtitleText.height;
            }
            return idealPosition;
        }
        width: MementoSettings.interfacePopupWidth
        height: MementoSettings.interfacePopupHeight
        closePolicy: Popup.NoAutoClose

        onShouldOpenChanged: definitionPopup.shouldOpen ? definitionPopup.open() : definitionPopup.close()
        onOpened: {
            const popupFits = x >= 0 &&
                            y >= 0 &&
                            x + width <= parent.width &&
                            y + height <= parent.height;
            if (!popupFits)
            {
                definitionPopup.close();
            }
        }
        onClosed: {
            definitionPopup.clearResults();
            root.sentenceAssistantArmed = false;
            sentenceAssistantButtonTimer.stop();
            interactiveTimer.restart();
        }

        onPlayerWidthChanged: definitionPopup.clearResults()
        onPlayerHeightChanged: definitionPopup.clearResults()
        onPausedChanged: definitionPopup.clearResults()
        onPositionChanged: definitionPopup.clearResults()

        Rectangle {
            id: dictionaryBorderRectangle
            anchors.fill: parent
            color: "transparent"
            border.color: MementoPalette.border
            border.width: Features.isUnix ? 1 : 0

            DefinitionPage {
                id: definitionPage
                anchors.fill: parent
                anchors.margins: dictionaryBorderRectangle.border.width
                search: dictionarySearch
                sentenceAssistantAvailable: root.sentenceAssistantArmed
                onSentenceAssistantRequested: root.openSentenceAssistant()

                /**
                 * Selects the longest cloze match or clears the selection on clear.
                 */
                function updateSelection() {
                    if (dictionarySearch.terms.length === 0 &&
                        dictionarySearch.kanji === null)
                    {
                        subtitleText.clearSelection();
                        return;
                    }
                    let selectionLength = 0;
                    let selectionStart = subtitleText.hoverIndex;
                    if (dictionarySearch.terms.length > 0)
                    {
                        const term = dictionarySearch.terms[0];
                        selectionStart = term.clozePrefix.length;
                        selectionLength = term.clozeBody.length;
                    }
                    else if (dictionarySearch.kanji !== null)
                    {
                        selectionLength = 1;
                    }
                    subtitleText.select(
                                selectionStart,
                                selectionStart + selectionLength);
                }

                onClosePressed: definitionPopup.clearResults()

                Connections {
                    target: dictionarySearch

                    function onKanjiChanged() {
                        definitionPage.updateSelection();
                    }

                    function onTermsChanged() {
                        definitionPage.updateSelection();
                    }
                }
            }

        }

        DictionarySearch {
            id: dictionarySearch
        }

        Timer {
            id: definitionPopupTimer
            interval: MementoSettings.searchDelay
            running: false
            repeat: false
            onTriggered: {
                if (subtitleText.hoverIndex !== definitionPopup.lastHoverIndex)
                {
                    return;
                }
                else if (!root.state.pause)
                {
                    return;
                }

                Qt.callLater(definitionPopup.search, subtitleText.hoverIndex);
            }
        }

        /**
         * Starts the search timer and sets start state.
         */
        function startSearchTimer() {
            if (subtitleText.hoverIndex === definitionPopup.lastHoverIndex)
            {
                return;
            }
            definitionPopup.lastHoverIndex = subtitleText.hoverIndex;
            if (subtitleText.hoverIndex < 0)
            {
                return;
            }
            else if (!root.state.pause)
            {
                return;
            }
            definitionPopupTimer.restart();
        }

        /**
         * Executes a search using the hover index of the subtitle text.
         * @param index The index into the subtitle text to search.
         */
        function search(index) {
            if (index < 0 || index >= subtitleText.text.length)
            {
                return;
            }

            const text = subtitleText.text;
            let queryIndex = index;
            const hoveredCharacter = text.charAt(index);
            const isEnglishLetter = function(character) {
                return (character >= "a" && character <= "z") ||
                    (character >= "A" && character <= "Z");
            };
            const isEnglishTokenCharacter = function(character) {
                return isEnglishLetter(character) || character === "'" ||
                    character === "’";
            };
            if (MementoSettings.searchMatcherEnglish &&
                    (isEnglishLetter(hoveredCharacter) ||
                     hoveredCharacter === "'" || hoveredCharacter === "’"))
            {
                while (queryIndex > 0 &&
                        isEnglishTokenCharacter(text.charAt(queryIndex - 1)))
                {
                    --queryIndex;
                }
            }

            definitionPopup.cursorAtSearch = root.cursorPosition;
            definitionPage.resetStack();

            const query = text.substring(queryIndex);
            if (MementoSettings.searchDebugTrace)
            {
                const lineStart = text.lastIndexOf("\n", index - 1);
                console.info(
                    "[SearchTrace] original subtitle:",
                    JSON.stringify(root.state.subtitle.text),
                    "filtered subtitle:", JSON.stringify(text),
                    "cursor full index:", index,
                    "cursor line index:", index - lineStart - 1,
                    "query full index:", queryIndex,
                    "query:", JSON.stringify(query)
                );
            }
            dictionarySearch.searchTerms(query, text, queryIndex);
            dictionarySearch.searchKanji(text.charAt(index), text, index);
        }

        /**
         * Clears search results.
         */
        function clearResults() {
            definitionPopup.lastHoverIndex = -1;
            definitionPage.resetStack();
            dictionarySearch.clearResults();
        }
    }

    SubtitleText {
        id: subtitleText

        readonly property var regexFilter: Utils.safeRegex(MementoSettings.searchRemoveRegex, "g")

        /**
         * Toggles mpv subtitles if searchHideMpvSubs is enabled.
         */
        function toggleMpvSubs() {
            if (!MementoSettings.searchHideMpvSubs)
            {
                return;
            }
            root.controller.setSubtitleVisibility(!subtitleText.visible);
        }

        anchors {
            horizontalCenter: root.horizontalCenter
            bottom: root.bottom

            /* Makes sure UI elements don't obscure the subtitles or that they go offscreen */
            bottomMargin: {
                let minValue = controls.visible ? controls.height : 0;
                let maxValue = root.height - subtitleText.height;
                if (!Features.isMacos && menu.visible)
                {
                    maxValue -= menu.height;
                }
                let margin = root.height * MementoSettings.interfaceSubtitleOffset;
                margin = Math.max(margin, minValue);
                margin = Math.min(margin, maxValue);
                return margin;
            }
        }

        /* Prevents text from being wider than the window */
        transformOrigin: Item.Bottom
        scale: subtitleText.width > root.width ? (root.width / subtitleText.width) : 1.0

        font.family: MementoSettings.interfaceSubtitleFont.family
        font.bold: MementoSettings.interfaceSubtitleFont.bold
        font.italic: MementoSettings.interfaceSubtitleFont.italic
        font.underline: MementoSettings.interfaceSubtitleFont.underline
        font.pixelSize: root.height * MementoSettings.interfaceSubtitleScale
        font.weight: MementoSettings.interfaceSubtitleFont.weight
        font.overline: MementoSettings.interfaceSubtitleFont.overline
        font.strikeout: MementoSettings.interfaceSubtitleFont.strikeout
        font.letterSpacing: MementoSettings.interfaceSubtitleFont.letterSpacing
        font.wordSpacing: MementoSettings.interfaceSubtitleFont.wordSpacing
        font.kerning: MementoSettings.interfaceSubtitleFont.kerning
        font.preferShaping: MementoSettings.interfaceSubtitleFont.preferShaping
        font.hintingPreference: MementoSettings.interfaceSubtitleFont.hintingPreference
        font.styleName: MementoSettings.interfaceSubtitleFont.styleName

        color: MementoSettings.interfaceSubtitleColor
        background: MementoSettings.interfaceSubtitleBackground
        stroke: MementoSettings.interfaceSubtitleStrokeColor
        strokeSize: MementoSettings.interfaceSubtitleStroke
        lineSpacing: MementoSettings.interfaceSubtitleLineSpacing

        text: {
            /* Implements regex filtering */
            let text = root.state.subtitle.text.replace(subtitleText.regexFilter, "");
            /* Implements replace new lines */
            if (MementoSettings.searchReplaceNewlines)
            {
                text = text.replace(/\n/g, MementoSettings.searchReplaceNewlinesWith);
            }
            return text;
        }

        visible: {
            if (root.ocrMode)
            {
                return false;
            }

            /* Implement unconditional hiding of subtitles */
            if (!menu.showSubtitles)
            {
                return false;
            }

            /* Implement hide Memento subtitles while playing media */
            if (MementoSettings.searchHideSubs && !root.state.pause)
            {
                return false;
            }

            /* Implements subtitle cursor show */
            if (MementoSettings.behaviorSubtitleCursorShow)
            {
                return itemCursorSubtitleShow.show;
            }

            return true;
        }

        /* Implements hide mpv subtitles */
        onVisibleChanged: subtitleText.toggleMpvSubs()
        Connections {
            target: MementoSettings
            function onSearchHideMpvSubsChanged() {
                subtitleText.toggleMpvSubs();
            }
        }

        Clipboard {
            id: clipboard
        }
        onDoubleClicked: clipboard.setText(text)

        HoverHandler {
            id: subtitleHover

            /* Implements auto pause on hover */
            onHoveredChanged: {
                if (subtitleHover.hovered && MementoSettings.searchPauseOnHover)
                {
                    root.controller.pause()
                }
            }
        }

        /* Implement searching */
        onHoverIndexChanged: {
            root.rememberSentenceAssistantHover(subtitleText.hoverIndex);
            switch (MementoSettings.searchMethod)
            {
            case MementoSetting.SearchMethodHover:
                definitionPopup.startSearchTimer();
                break;

            case MementoSetting.SearchMethodModifier:
                if (KeyTracker.modifierHeld(MementoSettings.searchModifier))
                {
                    Qt.callLater(definitionPopup.search, subtitleText.hoverIndex);
                }
                break;
            }
        }
        onMiddleClicked: {
            if (MementoSettings.searchMiddleMouseScan)
            {
                Qt.callLater(definitionPopup.search, subtitleText.hoverIndex);
            }
        }

        Connections {
            target: KeyTracker
            function onModifiersChanged() {
                if (MementoSettings.searchMethod === MementoSetting.SearchMethodModifier &&
                        subtitleHover.hovered &&
                        KeyTracker.modifierHeld(MementoSettings.searchModifier))
                {
                    Qt.callLater(definitionPopup.search, subtitleText.hoverIndex);
                }
            }
        }
    }

    Timer {
        id: sentenceAssistantButtonTimer
        interval: 2500
        repeat: false
        onTriggered: {
            if (subtitleText.hoverIndex >= 0 || definitionPopup.visible)
            {
                restart();
            }
            else
            {
                root.sentenceAssistantArmed = false;
            }
        }
    }

    Clipboard {
        id: sentenceAssistantClipboard
    }

    PlayerMenu {
        id: menu
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
        }
        visible: Features.isMacos
        player: root

        OpacityAnimator on opacity {
            id: menuFadeIn
            to: 1
            duration: Math.max(MementoSettings.behaviorOscFadeDuration, 1)
            running: false
            onStarted: menu.visible = true
        }

        OpacityAnimator on opacity {
            id: menuFadeOut
            to: 0
            duration: Math.max(MementoSettings.behaviorOscFadeDuration, 1)
            running: false
            onFinished: menu.visible = false
        }

        onOcrModeRequested: root.startOcrMode()
    }

    PlayerControls {
        id: controls
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        visible: false
        player: root

        OpacityAnimator on opacity {
            id: controlsFadeIn
            to: 1
            duration: Math.max(MementoSettings.behaviorOscFadeDuration, 1)
            running: false
            onStarted: controls.visible = true
        }

        OpacityAnimator on opacity {
            id: controlsFadeOut
            to: 0
            duration: Math.max(MementoSettings.behaviorOscFadeDuration, 1)
            running: false
            onFinished: controls.visible = false
        }

        HoverHandler {
            id: controlsHover
        }
    }

    PlayerOcrOverlay {
        id: ocrOverlay
        player: root
        onHideOscRequested: {
            definitionPopup.clearResults();
            interactiveTimer.stop();
            root.hideOsc(true);
        }
        onModeChanged: (enabled) => root.ocrMode = enabled
        onRestoreOscRequested: {
            if (root.oscVisibility === Player.OscVisible)
            {
                root.showOsc();
            }
            else
            {
                interactiveTimer.restart();
            }
        }
        onShowTextRequested: (text) => root.controller.showText(text)
        onTextRecognized: (text) => root.auxiliarySearchRequested(text)
    }
}
