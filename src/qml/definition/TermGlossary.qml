import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Ripose.Memento

ColumnLayout {
    id: root

    required property Term term
    required property TermDefinition definition
    required property color canvasColor
    property bool lingvoUkAudioVisible: false
    property bool lingvoUsAudioVisible: false
    property bool lingvoUkAudioEnabled: false
    property bool lingvoUsAudioEnabled: false

    function glossaryValueText(value) {
        if (typeof value === "string")
        {
            return value;
        }
        if (Array.isArray(value))
        {
            let text = "";
            for (const child of value)
            {
                text += root.glossaryValueText(child);
            }
            return text;
        }
        if (value && typeof value === "object")
        {
            if (value.content !== undefined)
            {
                return root.glossaryValueText(value.content);
            }
            if (value.text !== undefined)
            {
                return root.glossaryValueText(value.text);
            }
        }
        return "";
    }

    function extractDefinitionGlossaryText() {
        const entries = root.definition?.glossary ?? [];
        const text = [];
        for (const entry of entries)
        {
            text.push(root.glossaryValueText(entry));
        }
        return text.join("\n");
    }

    readonly property string definitionGlossaryText:
        root.extractDefinitionGlossaryText()

    function extractLingvoGlossaryText() {
        if ((root.definition?.dictionaryInfo?.name ?? "")
                .toLowerCase().indexOf("lingvouniversal") < 0)
        {
            return "";
        }
        return root.definitionGlossaryText;
    }

    readonly property string lingvoGlossaryText: root.extractLingvoGlossaryText()

    function extractPronunciation() {
        const entries = root.definition?.glossary ?? [];
        const pronunciationPattern = /^\s*(?:[•·]\s*)?(?:(?:VIII|VII|VI|IV|V|III|II|IX|X|I)\)?[ \t]+)?(\[[^\]\r\n]+\](?:[ \t]*(?:,|\/)[ \t]*\[[^\]\r\n]+\])*)/i;
        const romanSectionPattern = /^\s*(?:VIII|VII|VI|IV|V|III|II|IX|X|I)\)?\s*$/i;
        for (const entry of entries)
        {
            const lines = root.glossaryValueText(entry).split("\n");
            for (const line of lines)
            {
                if (!line.trim().length)
                {
                    continue;
                }
                // Lingvo may put the Roman part-of-speech number on its own
                // line before the pronunciation.
                if (romanSectionPattern.test(line))
                {
                    continue;
                }
                const match = line.match(pronunciationPattern);
                if (match)
                {
                    if (match[1].toLowerCase() === "[ex]" ||
                            match[1].toLowerCase() === "[/ex]")
                    {
                        break;
                    }
                    return match[1].replace(/[ \t]*\/[ \t]*/g, " / ");
                }
                break;
            }
        }
        return "";
    }

    readonly property string pronunciationText: root.extractPronunciation()

    function extractIrregularVerbForms() {
        if ((root.definition?.dictionaryInfo?.name ?? "")
                .toLowerCase().indexOf("lingvouniversal") < 0)
        {
            return [];
        }

        const pattern = /прош\.\s*вр\.\s*([A-Za-z][A-Za-z'’.-]*(?:[ \t]+[A-Za-z][A-Za-z'’.-]*)*(?:[ \t]*(?:,|\/)[ \t]*[A-Za-z][A-Za-z'’.-]*(?:[ \t]+[A-Za-z][A-Za-z'’.-]*)*)*)\s*[;,]?\s*прич\.\s*прош\.\s*вр\.\s*([A-Za-z][A-Za-z'’.-]*(?:[ \t]+[A-Za-z][A-Za-z'’.-]*)*(?:[ \t]*(?:,|\/)[ \t]*[A-Za-z][A-Za-z'’.-]*(?:[ \t]+[A-Za-z][A-Za-z'’.-]*)*)*)/i;
        const match = root.lingvoGlossaryText.match(pattern);
        const base = (root.term?.expression ?? "").trim();
        const sharedForms = root.term?.irregularVerbForms ?? [];
        if (!base || !sharedForms.length)
        {
            return [];
        }
        if (match && match[1].trim() && match[2].trim())
        {
            return [base,
                    match[1].trim().replace(/\s*,\s*/g, " / "),
                    match[2].trim().replace(/\s*,\s*/g, " / ")];
        }

        // The dictionary sometimes omits the form metadata. Use the shared
        // built-in irregular-verb list in that case.
        return sharedForms;
    }

    readonly property var irregularVerbForms: root.extractIrregularVerbForms()

    readonly property string widestIrregularVerbForm: {
        let widest = "";
        for (const form of root.irregularVerbForms)
        {
            if (String(form).length > widest.length)
            {
                widest = String(form);
            }
        }
        return widest;
    }

    readonly property string selectedText: glossaryText.ankiSelectedText

    signal searchRequested(query: string)
    signal lingvoUkAudioRequested()
    signal lingvoUsAudioRequested()

    Item {
        id: definitionHeader
        Layout.fillWidth: true
        implicitHeight: Math.max(
                            dictionaryTags.implicitHeight +
                                (pronunciationRow.visible ?
                                     pronunciationRow.implicitHeight : 0),
                            irregularVerbTable.visible ?
                                irregularVerbTable.height : 0
                        )

        CenteredFlow {
            id: dictionaryTags
            anchors.left: parent.left
            anchors.right: irregularVerbTable.visible ?
                               irregularVerbTable.left :
                               parent.right
            anchors.rightMargin: irregularVerbTable.visible ? 6 : 0
            height: implicitHeight
            spacing: 5

            CheckBox {
                id: checkBox

                Component.onCompleted: {
                    if (Features.isUnix)
                    {
                        return;
                    }

                    checkBox.leftPadding = 0;
                    checkBox.rightPadding = 0;
                    checkBox.topPadding = 0;
                    checkBox.bottomPadding = 0;
                    checkBox.leftInset = 0;
                    checkBox.rightInset = 0;
                    checkBox.topInset = 0;
                    checkBox.bottomInset = 0;
                    checkBox.spacing = 0;
                    checkBox.implicitWidth = Qt.binding(function() {
                        return checkBox.contentItem.implicitWidth +
                                checkBox.indicator.implicitWidth +
                                checkBox.spacing;
                    });
                }

                visible: AnkiConfig.enabled &&
                         MementoSettings.showAnkiGlossaryCheckbox &&
                         root.term &&
                         root.term.ankiChecked &&
                         (root.term.addableExpression || root.term.addableReading)
                enabled: root.term && !root.term.ankiAdding
                checked: root.definition?.selected ?? false
                onClicked: root.definition.selected = checked
            }

            Repeater {
                model: root.definition?.tags ?? []
                delegate: TagLabel {
                    required property int index
                    tag: root.definition?.tags[index]
                }
            }

            DictionaryLabel {
                info: root.definition?.dictionaryInfo
            }
        }

        Row {
            id: pronunciationRow
            readonly property real flagHeight: Math.max(
                pronunciationBracketMetrics.boundingRect.height,
                pronunciationLabel.contentHeight * 0.9
            )
            x: AnkiConfig.enabled &&
               MementoSettings.showAnkiGlossaryCheckbox &&
               root.term && root.term.ankiChecked &&
               (root.term.addableExpression || root.term.addableReading) ?
                   11 : 0
            y: dictionaryTags.implicitHeight
            height: pronunciationLabel.contentHeight
            spacing: 4
            visible: root.pronunciationText.length > 0

            Label {
                id: pronunciationLabel
                text: root.pronunciationText
                font: {
                    var transcriptionFont = glossaryText.font;
                    transcriptionFont.pointSize += 1;
                    return transcriptionFont;
                }
                color: MementoPalette.text
                anchors.verticalCenter: parent.verticalCenter
            }

            Item {
                width: pronunciationSpaceMetrics.advanceWidth
                height: 1
            }

            TextMetrics {
                id: pronunciationSpaceMetrics
                font: pronunciationLabel.font
                text: "  "
            }

            TextMetrics {
                id: pronunciationBracketMetrics
                font: pronunciationLabel.font
                text: "[]"
            }

            Item {
                visible: root.lingvoUsAudioVisible
                width: pronunciationRow.flagHeight * 40 / 26
                height: pronunciationLabel.contentHeight

                Image {
                    anchors.centerIn: parent
                    width: parent.width
                    height: pronunciationRow.flagHeight
                    source: "qrc:/images/flag-us.svg"
                    sourceSize.width: Math.ceil(width * 2)
                    sourceSize.height: Math.ceil(height * 2)
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    opacity: root.lingvoUsAudioEnabled ? 1 : 0.45
                    Accessible.name: qsTr("Play American pronunciation")
                    ToolTip.visible: flagUsMouseArea.containsMouse
                    ToolTip.text: qsTr("American pronunciation")

                    MouseArea {
                        id: flagUsMouseArea
                        anchors.fill: parent
                        enabled: root.lingvoUsAudioEnabled
                        hoverEnabled: true
                        cursorShape: containsMouse ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: root.lingvoUsAudioRequested()
                    }
                }
            }

            Item {
                visible: root.lingvoUsAudioVisible &&
                         root.lingvoUkAudioVisible
                width: Math.max(
                           0,
                           pronunciationSpaceMetrics.advanceWidth -
                               pronunciationRow.spacing * 2
                       )
                height: 1
            }

            Item {
                visible: root.lingvoUkAudioVisible
                width: pronunciationRow.flagHeight * 40 / 26
                height: pronunciationLabel.contentHeight

                Image {
                    anchors.centerIn: parent
                    width: parent.width
                    height: pronunciationRow.flagHeight
                    source: "qrc:/images/flag-uk.svg"
                    sourceSize.width: Math.ceil(width * 2)
                    sourceSize.height: Math.ceil(height * 2)
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    opacity: root.lingvoUkAudioEnabled ? 1 : 0.45
                    Accessible.name: qsTr("Play British pronunciation")
                    ToolTip.visible: flagUkMouseArea.containsMouse
                    ToolTip.text: qsTr("British pronunciation")

                    MouseArea {
                        id: flagUkMouseArea
                        anchors.fill: parent
                        enabled: root.lingvoUkAudioEnabled
                        hoverEnabled: true
                        cursorShape: containsMouse ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: root.lingvoUkAudioRequested()
                    }
                }
            }
        }

        Rectangle {
            id: irregularVerbTable
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.rightMargin: 0
            z: 2
            visible: root.irregularVerbForms.length === 3
            width: Math.min(
                       Math.max(72, formTextMetrics.advanceWidth + 16),
                       Math.max(
                           1,
                           parent.width
                       )
                   )
            height: formRows.implicitHeight + 2
            radius: 3
            color: "#292929"
            border.width: 1
            border.color: "#3B5962"

            TextMetrics {
                id: formTextMetrics
                font.pixelSize: 12
                font.bold: true
                text: root.widestIrregularVerbForm
            }

            Column {
                id: formRows
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 1
                spacing: 0

                Repeater {
                    model: root.irregularVerbForms

                    delegate: Item {
                        required property int index
                        required property string modelData
                        width: formRows.width
                        height: formLabel.implicitHeight + 4

                        Rectangle {
                            visible: index > 0
                            anchors.top: parent.top
                            width: parent.width
                            height: 1
                            color: "#3B5962"
                        }

                        Text {
                            id: formLabel
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: 5
                            anchors.rightMargin: 5
                            anchors.verticalCenter: parent.verticalCenter
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            text: modelData
                            color: "#E0E0E0"
                            font.pixelSize: 12
                            font.bold: true
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }
    }

    GlossaryText {
        id: glossaryText
        Layout.fillWidth: true
        Layout.minimumHeight: contentHeight
        Layout.preferredHeight: contentHeight
        persistentSelection: true
        definition: root.definition
        canvasColor: root.canvasColor

        onSearchRequested: (query) => root.searchRequested(query)
    }
}
