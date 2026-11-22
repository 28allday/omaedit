pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Dialogs as D
import OmaEdit.Ui
import "../Ui/Keyframes.js" as Keyframes

Column {
    id: root
    property var effects: []
    property var catalogue: []
    property var expandedIds: ({})
    property var effectIds: []
    property string pickingUid: ""   // the keyer whose backdrop the viewer's next click picks
    property bool reorderable: true  // up and down buttons (the effects window has its own list)
    property bool maskViewable: false   // whether a mask can be shown on the viewer for placing
    property string maskViewUid: ""     // the effect whose mask is shown there
    property int animationFrame: -1  // the clip's frame under the playhead; below 0, no keyframe buttons
    signal editRequested(string action, string uid, var values)
    signal gestureStarted()
    signal gestureFinished()
    spacing: Theme.s2

    // Keep delegates while parameter values change; committing a field must
    // not destroy keyboard focus or collapse the controls being edited.
    onEffectsChanged: {
        const ids = effects.map(function(e) { return e.uid })
        if (JSON.stringify(ids) !== JSON.stringify(effectIds)) effectIds = ids
    }
    function effectState(uid) { return effects.find(function(e) { return e.uid === uid }) || {} }
    function definition(id) { return catalogue.find(function(e) { return e.id === id }) || {} }
    function toggle(uid) {
        const next = Object.assign({}, expandedIds)
        next[uid] = next[uid] === undefined ? false : !next[uid]
        expandedIds = next
    }
    // An effect's mask: the shape it shows within (see VideoEffects::validateMask).
    readonly property var maskShapes: ["rectangle", "ellipse", "polygon"]
    // As it stands under the playhead: its keyed fields from their keys.
    function maskOf(uid) { return Keyframes.maskAt(effectState(uid), animationFrame) }
    function setMask(uid, patch) {
        const mask = Object.assign({x: .5, y: .5, w: .5, h: .5, rotation: 0, feather: 0, invert: false}, maskOf(uid) || {}, patch)
        if (mask.shape === "polygon") {
            if (!mask.points || mask.points.length < 3) mask.points = [[.25, .25], [.75, .25], [.75, .75], [.25, .75]]
        } else delete mask.points
        const state = effectState(uid)
        editRequested("update", uid, state.mask ? Keyframes.maskUpdate(state, mask, animationFrame) : {mask: mask})
    }
    function maskKeysOf(uid, field) {
        const keys = effectState(uid).maskKeys
        return keys && keys[field] ? keys[field] : []
    }
    // A mask field's diamond: a key under the playhead with the value shown, or that key taken away.
    function toggleMaskKey(uid, field) {
        const state = effectState(uid), shown = maskOf(uid)
        if (animationFrame < 0 || !shown) return
        const keys = {}, values = {}
        if (Keyframes.keyedAt(maskKeysOf(uid, field), animationFrame)) {
            keys[field] = maskKeysOf(uid, field).filter(function(k) { return k.frame !== animationFrame })
            if (!keys[field].length) { const mask = JSON.parse(JSON.stringify(state.mask)); mask[field] = shown[field]; values.mask = mask }
        } else keys[field] = Keyframes.withKey(maskKeysOf(uid, field), animationFrame, shown[field])
        values.maskKeys = keys
        editRequested("update", uid, values)
    }
    function chooseMask(uid, index) {
        if (index <= 0) editRequested("update", uid, {mask: {}})
        else {
            setMask(uid, {shape: maskShapes[index - 1]})
            if (maskViewable) editRequested("maskView", uid, {on: true})  // a new shape is put where it can be placed
        }
    }
    component MaskSlider: SliderField {
        property string uid: ""
        property string maskField: ""   // the mask field it sets, for its keyframe diamond
        signal valueChosen(real value)
        animatable: root.animationFrame >= 0 && maskField !== "" && uid !== ""
        keyed: animatable && Keyframes.keyedAt(root.maskKeysOf(uid, maskField), root.animationFrame)
        hasAnimation: maskField !== "" && root.maskKeysOf(uid, maskField).length > 0
        keyObjectName: "videoEffectMaskKey:" + uid + ":" + maskField
        onKeyToggled: root.toggleMaskKey(uid, maskField)
        labelWidth: 90  // fixed: one that followed the width would feed the card's own width back to it
        tip: label
        liveEdit: true
        onEditingStarted: root.gestureStarted()
        onEditingEnded: root.gestureFinished()
        onEdited: function(v) { if (dragging) valueChosen(v) }
        onEditingFinished: function(v) { valueChosen(v) }
    }
    // The keyframe diamond for a control that is not a slider: a key under the
    // playhead holding what the control shows, or that key taken away.
    component KeyDiamond: IconButton {
        property string uid: ""
        property string control: ""    // the effect's control, or with `mask` the mask's field
        property string label: ""
        property var shown             // what the control shows now
        property bool mask: false
        readonly property var keys: mask ? root.maskKeysOf(uid, control) : root.keysOf(uid, control)
        readonly property bool keyed: Keyframes.keyedAt(keys, root.animationFrame)
        objectName: (mask ? "videoEffectMaskKey:" : "videoEffectKey:") + uid + ":" + control
        visible: root.animationFrame >= 0
        width: Theme.szIcon + Theme.s1
        height: Theme.hControl
        iconName: "diamond"
        checked: keyed
        iconColor: keyed ? Theme.accent : keys.length ? Theme.textPrimary : Theme.textMuted
        text: keyed ? qsTr("Remove %1 keyframe at playhead").arg(label) : qsTr("Add %1 keyframe at playhead").arg(label)
        onClicked: mask ? root.toggleMaskKey(uid, control) : root.toggleKey(uid, control, shown, false)
    }
    // A control's keys, [{frame, value, interpolation}] in the clip's frames.
    function keysOf(uid, key) {
        const keys = effectState(uid).keys
        return keys && keys[key] ? keys[key] : []
    }
    function keyedAt(uid, key) {
        return keysOf(uid, key).some(function(k) { return k.frame === animationFrame })
    }
    // What the control reads under the playhead: its keys' value there, or as set.
    function shownValue(uid, key, fallback) {
        const keys = keysOf(uid, key)
        if (!keys.length || animationFrame < 0) return fallback
        if (animationFrame <= keys[0].frame) return keys[0].value
        for (let i = 1; i < keys.length; ++i) {
            const a = keys[i - 1], b = keys[i]
            if (animationFrame < b.frame) {
                if (a.interpolation === "hold") return a.value
                let u = (animationFrame - a.frame) / (b.frame - a.frame)
                if (a.interpolation === "smooth") u = u * u * (3 - 2 * u)
                return a.value + u * (b.value - a.value)
            }
        }
        return keys[keys.length - 1].value
    }
    function withKey(uid, key, value) {
        const before = keysOf(uid, key)
        const earlier = before.filter(function(k) { return k.frame < animationFrame })
        const exact = before.find(function(k) { return k.frame === animationFrame })
        const list = before.filter(function(k) { return k.frame !== animationFrame })
        list.push({frame: animationFrame, value: value,
                   interpolation: exact ? exact.interpolation || "linear"
                                : earlier.length ? earlier[earlier.length - 1].interpolation || "linear" : "linear"})
        list.sort(function(a, b) { return a.frame - b.frame })
        return list
    }
    // A control with keys takes a new value as a key under the playhead.
    function parameter(uid, key, value) {
        if (animationFrame >= 0 && keysOf(uid, key).length) {
            const keys = {}; keys[key] = withKey(uid, key, value)
            editRequested("update", uid, {keys: keys})
            return
        }
        const params = {}; params[key] = value
        editRequested("update", uid, {params: params})
    }
    // The diamond: a key under the playhead with the value shown there, or
    // that key taken away. The last one gone leaves the value as it stood.
    function toggleKey(uid, key, value, whole) {
        if (animationFrame < 0) return
        if (whole) value = Math.round(value)
        const keys = {}, values = {}
        if (keyedAt(uid, key)) {
            keys[key] = keysOf(uid, key).filter(function(k) { return k.frame !== animationFrame })
            if (!keys[key].length) { const params = {}; params[key] = value; values.params = params }
        } else keys[key] = withKey(uid, key, value)
        values.keys = keys
        editRequested("update", uid, values)
    }

    Repeater {
        model: root.effectIds
        delegate: Rectangle {
            id: card
            required property string modelData
            required property int index
            readonly property var effect: root.effectState(modelData)
            readonly property var spec: root.definition(effect.id || "")
            readonly property bool expanded: root.expandedIds[modelData] !== false
            width: root.width
            height: contents.implicitHeight + Theme.s2 * 2
            color: Theme.panelRaised
            radius: Theme.rControl
            border.width: Theme.hairline
            border.color: Theme.border
            objectName: "videoEffect:" + modelData

            Column {
                id: contents
                x: Theme.s2; y: Theme.s2
                width: parent.width - Theme.s2 * 2
                spacing: Theme.s1
                Row {
                    width: parent.width
                    IconButton {
                        iconName: card.expanded ? "chevron-down" : "chevron-right"
                        text: card.expanded ? qsTr("Collapse %1").arg(card.spec.name || card.effect.id)
                                            : qsTr("Expand %1").arg(card.spec.name || card.effect.id)
                        onClicked: root.toggle(card.modelData)
                    }
                    Text {
                        width: Math.max(0, parent.width - Theme.szIconHit * 2)
                        height: Theme.szIconHit
                        verticalAlignment: Text.AlignVCenter
                        text: card.spec.name || card.effect.id || ""
                        color: card.effect.bypass ? Theme.textMuted : Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsControl
                        font.weight: Theme.wHeading
                        elide: Text.ElideRight
                    }
                    IconButton {
                        objectName: "videoEffectBypass:" + card.modelData
                        iconName: card.effect.bypass ? "eye-off" : "eye"
                        text: card.effect.bypass ? qsTr("Enable effect") : qsTr("Bypass effect")
                        checkable: true
                        checked: !card.effect.bypass
                        onClicked: root.editRequested("update", card.modelData, {bypass: !card.effect.bypass})
                    }
                }
                Text {
                    visible: card.expanded && !root.reorderable && !!card.spec.description
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: card.spec.description || ""
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                }
                Row {
                    spacing: Theme.s1
                    IconButton {
                        objectName: "videoEffectUp:" + card.modelData
                        visible: root.reorderable
                        iconName: "chevron-up"; text: qsTr("Move effect earlier")
                        enabled: card.index > 0
                        onClicked: root.editRequested("move", card.modelData, {direction: -1})
                    }
                    IconButton {
                        objectName: "videoEffectDown:" + card.modelData
                        visible: root.reorderable
                        iconName: "chevron-down"; text: qsTr("Move effect later")
                        enabled: card.index + 1 < root.effectIds.length
                        onClicked: root.editRequested("move", card.modelData, {direction: 1})
                    }
                    IconButton {
                        iconName: "rotate-ccw"; text: qsTr("Reset effect parameters")
                        enabled: card.spec.available === true
                        onClicked: root.editRequested("reset", card.modelData, {})
                    }
                    IconButton {
                        objectName: "videoEffectRemove:" + card.modelData
                        iconName: "trash-2"; text: qsTr("Remove effect")
                        onClicked: root.editRequested("remove", card.modelData, {})
                    }
                    Text {
                        visible: card.effect.bypass === true
                        height: Theme.szIconHit
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("Bypassed")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsLabel
                    }
                }
                Text {
                    visible: card.spec.available !== true || !!card.effect.problem
                    width: parent.width
                    text: card.effect.problem || qsTr("Unavailable plug-in. Enable or install it in Preferences, or bypass this effect.")
                    wrapMode: Text.WordWrap
                    color: Theme.warning
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                }
                // The keyer's backdrop: found in the frame under the playhead,
                // or picked from the viewer.
                Row {
                    visible: card.expanded && card.effect.id === "chroma-key" && card.spec.available === true
                    spacing: Theme.s1
                    ToolButton {
                        objectName: "videoEffectFindBackdrop:" + card.modelData
                        iconName: "scan-line"; showLabel: true
                        text: qsTr("Find backdrop")
                        onClicked: root.editRequested("findBackdrop", card.modelData, {})
                    }
                    ToolButton {
                        objectName: "videoEffectPickBackdrop:" + card.modelData
                        iconName: "mouse-pointer-2"; showLabel: true
                        text: root.reorderable ? qsTr("Pick from viewer") : qsTr("Pick from preview")
                        checkable: true
                        checked: root.pickingUid === card.modelData
                        onClicked: root.editRequested("pickBackdrop", card.modelData, {})
                    }
                }
                Text {
                    visible: card.expanded && card.effect.id === "chroma-key"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: root.pickingUid === card.modelData ? (root.reorderable ? qsTr("Click the screen in the viewer.") : qsTr("Click the screen in the preview."))
                        : qsTr("Raise Similarity if grey specks remain; lower it if the subject has holes. Raise Edge softness if hair looks cut out. Temporal smoothing steadies edges that flicker on compressed footage (it renders the frames either side too). Show matte: white is kept, black removed.")
                    color: root.pickingUid === card.modelData ? Theme.accent : Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                }
                Loader {
                    width: parent.width
                    active: card.expanded && card.spec.available === true
                    sourceComponent: Column {
                        width: parent.width
                        spacing: Theme.s2
                        Repeater {
                            model: card.spec.params || []
                            delegate: Loader {
                                id: field
                                required property var modelData
                                readonly property var setValue: card.effect.params && card.effect.params[modelData.key] !== undefined
                                                                ? card.effect.params[modelData.key] : modelData.default
                                // As it stands under the playhead: a number or a colour on its way
                                // between its keys, a switch or a choice as its last key left it.
                                readonly property var value: {
                                    const keys = root.keysOf(card.modelData, modelData.key)
                                    if (!keys.length || root.animationFrame < 0) return setValue
                                    return modelData.type === "number" ? root.shownValue(card.modelData, modelData.key, setValue)
                                         : modelData.type === "color" ? Keyframes.colourAt(keys, root.animationFrame, setValue)
                                         : Keyframes.stepAt(keys, root.animationFrame, setValue)
                                }
                                // The diamond beside a switch, a choice or a colour (a slider has its own).
                                readonly property real keyRoom: root.animationFrame >= 0 ? Theme.szIcon + Theme.s1 * 2 : 0
                                width: parent.width
                                // A control that matters only for one setting of another.
                                visible: {
                                    if (!modelData.whenKey) return true
                                    const params = card.effect.params || {}
                                    const other = (card.spec.params || []).find(function(p) { return p.key === modelData.whenKey })
                                    const now = params[modelData.whenKey] !== undefined ? params[modelData.whenKey] : (other ? other.default : "")
                                    return String(now) === modelData.whenValue
                                }
                                sourceComponent: modelData.type === "number" ? numberField
                                               : modelData.type === "bool" ? boolField
                                               : modelData.type === "choice" ? choiceField : colorField
                                Component {
                                    id: numberField
                                    SliderField {
                                        objectName: "videoEffectParam:" + card.modelData + ":" + field.modelData.key
                                        width: field.width
                                        labelWidth: Math.min(90, width * .35)
                                        label: field.modelData.label
                                        tip: field.modelData.label
                                        from: field.modelData.min; to: field.modelData.max
                                        origin: field.modelData.default
                                        stepSize: field.modelData.step
                                        decimals: stepSize >= 1 ? 0 : stepSize >= .01 ? 2 : 3
                                        value: field.value
                                        animatable: root.animationFrame >= 0
                                        keyed: animatable && root.keyedAt(card.modelData, field.modelData.key)
                                        hasAnimation: root.keysOf(card.modelData, field.modelData.key).length > 0
                                        keyObjectName: "videoEffectKey:" + card.modelData + ":" + field.modelData.key
                                        onKeyToggled: root.toggleKey(card.modelData, field.modelData.key, field.value, field.modelData.step === 1)
                                        liveEdit: true
                                        onEditingStarted: root.gestureStarted()
                                        onEditingEnded: root.gestureFinished()
                                        onEdited: function(v) { if (dragging) root.parameter(card.modelData, field.modelData.key, v) }
                                        onEditingFinished: function(v) { root.parameter(card.modelData, field.modelData.key, v) }
                                    }
                                }
                                Component {
                                    id: boolField
                                    Row {
                                        width: field.width
                                        spacing: Theme.s1
                                        Toggle {
                                            objectName: "videoEffectParam:" + card.modelData + ":" + field.modelData.key
                                            width: parent.width - field.keyRoom
                                            label: field.modelData.label
                                            checked: field.value === true
                                            tip: field.modelData.label
                                            onClicked: root.parameter(card.modelData, field.modelData.key, !field.value)
                                        }
                                        KeyDiamond { uid: card.modelData; control: field.modelData.key; label: field.modelData.label; shown: field.value }
                                    }
                                }
                                Component {
                                    id: choiceField
                                    Row {
                                        width: field.width
                                        spacing: Theme.s1
                                        ComboField {
                                            objectName: "videoEffectParam:" + card.modelData + ":" + field.modelData.key
                                            width: parent.width - field.keyRoom
                                            caption: field.modelData.label
                                            tip: field.modelData.label
                                            model: field.modelData.labels
                                            currentIndex: Math.max(0, field.modelData.values.indexOf(field.value))
                                            onActivated: function(i) { root.parameter(card.modelData, field.modelData.key, field.modelData.values[i]) }
                                        }
                                        KeyDiamond { uid: card.modelData; control: field.modelData.key; label: field.modelData.label; shown: field.value }
                                    }
                                }
                                Component {
                                    id: colorField
                                    Row {
                                        width: field.width
                                        spacing: Theme.s1
                                        ToolButton {
                                            objectName: "videoEffectColour:" + card.modelData + ":" + field.modelData.key
                                            width: parent.width - field.keyRoom
                                            iconName: "palette"
                                            text: field.modelData.label + "…"
                                            showLabel: true
                                            Rectangle {
                                                width: Theme.szIcon; height: Theme.szIcon
                                                anchors.right: parent.right; anchors.rightMargin: Theme.s2
                                                anchors.verticalCenter: parent.verticalCenter
                                                color: field.value; radius: Theme.rControl
                                                border.width: Theme.hairline; border.color: Theme.border
                                            }
                                            onClicked: { picker.selectedColor = field.value; picker.open() }
                                            D.ColorDialog {
                                                id: picker
                                                title: field.modelData.label
                                                options: D.ColorDialog.DontUseNativeDialog | D.ColorDialog.ShowAlphaChannel
                                                onAccepted: root.parameter(card.modelData, field.modelData.key, String(selectedColor))
                                            }
                                        }
                                        KeyDiamond { uid: card.modelData; control: field.modelData.key; label: field.modelData.label; shown: field.value }
                                    }
                                }
                            }
                        }
                    }
                }
                // Where the effect shows: everywhere, or within a shape.
                Column {
                    id: maskBox
                    visible: card.expanded && card.spec.available === true && card.spec.maskable === true
                    width: parent.width
                    spacing: Theme.s2
                    readonly property var mask: card.effect.mask ? root.maskOf(card.modelData) : null
                    readonly property string shape: mask ? mask.shape : ""
                    readonly property var points: mask && mask.points ? mask.points : []
                    property int corner: 0
                    readonly property int cornerNow: Math.max(0, Math.min(corner, points.length - 1))
                    function field(key, fallback) { return mask && mask[key] !== undefined ? mask[key] : fallback }
                    function moveCorner(axis, value) {
                        const next = JSON.parse(JSON.stringify(points))
                        next[cornerNow][axis] = value
                        root.setMask(card.modelData, {points: next})
                    }
                    ComboField {
                        objectName: "videoEffectMask:" + card.modelData
                        width: parent.width
                        caption: qsTr("Mask")
                        tip: qsTr("Where the effect shows: everywhere, or only within a shape")
                        model: [qsTr("None"), qsTr("Rectangle"), qsTr("Ellipse"), qsTr("Polygon")]
                        currentIndex: root.maskShapes.indexOf(maskBox.shape) + 1
                        onActivated: function(i) { root.chooseMask(card.modelData, i) }
                    }
                    ToolButton {
                        objectName: "videoEffectMaskView:" + card.modelData
                        visible: root.maskViewable && maskBox.shape !== ""
                        iconName: "move"; showLabel: true
                        text: qsTr("Place on viewer")
                        checkable: true
                        checked: root.maskViewUid === card.modelData
                        onClicked: root.editRequested("maskView", card.modelData, {})
                    }
                    Column {
                        visible: maskBox.shape === "rectangle" || maskBox.shape === "ellipse"
                        width: parent.width
                        spacing: Theme.s2
                        MaskSlider {
                            objectName: "videoEffectMaskX:" + card.modelData
                            width: parent.width; uid: card.modelData; maskField: "x"; label: qsTr("Centre X"); suffix: "%"
                            from: -50; to: 150; origin: 50; stepSize: .1; decimals: 1
                            value: maskBox.field("x", .5) * 100
                            onValueChosen: function(v) { root.setMask(card.modelData, {x: v / 100}) }
                        }
                        MaskSlider {
                            width: parent.width; uid: card.modelData; maskField: "y"; label: qsTr("Centre Y"); suffix: "%"
                            from: -50; to: 150; origin: 50; stepSize: .1; decimals: 1
                            value: maskBox.field("y", .5) * 100
                            onValueChosen: function(v) { root.setMask(card.modelData, {y: v / 100}) }
                        }
                        MaskSlider {
                            objectName: "videoEffectMaskW:" + card.modelData
                            width: parent.width; uid: card.modelData; maskField: "w"; label: qsTr("Width"); suffix: "%"
                            from: 1; to: 200; stepSize: .1; decimals: 1
                            value: maskBox.field("w", .5) * 100
                            onValueChosen: function(v) { root.setMask(card.modelData, {w: v / 100}) }
                        }
                        MaskSlider {
                            width: parent.width; uid: card.modelData; maskField: "h"; label: qsTr("Height"); suffix: "%"
                            from: 1; to: 200; stepSize: .1; decimals: 1
                            value: maskBox.field("h", .5) * 100
                            onValueChosen: function(v) { root.setMask(card.modelData, {h: v / 100}) }
                        }
                        MaskSlider {
                            width: parent.width; uid: card.modelData; maskField: "rotation"; label: qsTr("Rotation"); suffix: "°"
                            from: -180; to: 180; origin: 0; stepSize: .1; decimals: 1
                            value: maskBox.field("rotation", 0)
                            onValueChosen: function(v) { root.setMask(card.modelData, {rotation: v}) }
                        }
                    }
                    Column {
                        visible: maskBox.shape === "polygon"
                        width: parent.width
                        spacing: Theme.s2
                        Row {
                            spacing: Theme.s1
                            ComboField {
                                objectName: "videoEffectMaskCorner:" + card.modelData
                                width: 120
                                tip: qsTr("Which corner of the polygon the X and Y sliders move")
                                model: maskBox.points.map(function(p, i) { return qsTr("Corner %1").arg(i + 1) })
                                currentIndex: maskBox.cornerNow
                                onActivated: function(i) { maskBox.corner = i }
                            }
                            IconButton {
                                objectName: "videoEffectMaskAddCorner:" + card.modelData
                                iconName: "plus"; text: qsTr("Add a corner after this one")
                                enabled: maskBox.points.length >= 3 && maskBox.points.length < 8
                                onClicked: {
                                    const next = JSON.parse(JSON.stringify(maskBox.points)), i = maskBox.cornerNow, b = next[(i + 1) % next.length]
                                    next.splice(i + 1, 0, [(next[i][0] + b[0]) / 2, (next[i][1] + b[1]) / 2])
                                    maskBox.corner = i + 1
                                    root.setMask(card.modelData, {points: next})
                                }
                            }
                            IconButton {
                                objectName: "videoEffectMaskRemoveCorner:" + card.modelData
                                iconName: "minus"; text: qsTr("Remove this corner")
                                enabled: maskBox.points.length > 3
                                onClicked: {
                                    const next = JSON.parse(JSON.stringify(maskBox.points))
                                    next.splice(maskBox.cornerNow, 1)
                                    maskBox.corner = Math.max(0, maskBox.cornerNow - 1)
                                    root.setMask(card.modelData, {points: next})
                                }
                            }
                            KeyDiamond { uid: card.modelData; control: "points"; label: qsTr("Corners"); mask: true }
                        }
                        MaskSlider {
                            objectName: "videoEffectMaskCornerX:" + card.modelData
                            width: parent.width; label: qsTr("Corner X"); suffix: "%"
                            from: -50; to: 150; origin: 50; stepSize: .1; decimals: 1
                            value: maskBox.points.length ? maskBox.points[maskBox.cornerNow][0] * 100 : 50
                            onValueChosen: function(v) { maskBox.moveCorner(0, v / 100) }
                        }
                        MaskSlider {
                            width: parent.width; label: qsTr("Corner Y"); suffix: "%"
                            from: -50; to: 150; origin: 50; stepSize: .1; decimals: 1
                            value: maskBox.points.length ? maskBox.points[maskBox.cornerNow][1] * 100 : 50
                            onValueChosen: function(v) { maskBox.moveCorner(1, v / 100) }
                        }
                    }
                    MaskSlider {
                        objectName: "videoEffectMaskFeather:" + card.modelData
                        visible: maskBox.shape !== ""
                        width: parent.width; uid: card.modelData; maskField: "feather"; label: qsTr("Feather"); suffix: "%"
                        from: 0; to: 50; stepSize: .1; decimals: 1
                        value: maskBox.field("feather", 0) * 100
                        onValueChosen: function(v) { root.setMask(card.modelData, {feather: v / 100}) }
                    }
                    Toggle {
                        objectName: "videoEffectMaskInvert:" + card.modelData
                        visible: maskBox.shape !== ""
                        width: parent.width
                        label: qsTr("Invert mask")
                        tip: qsTr("Show the effect outside the shape instead of inside it")
                        checked: maskBox.field("invert", false) === true
                        onClicked: root.setMask(card.modelData, {invert: !maskBox.field("invert", false)})
                    }
                }
            }
        }
    }
    Text {
        width: parent.width
        visible: root.effects.length === 0
        text: qsTr("Add an effect from the Effects browser. Double-click a tile or drag it onto a clip.")
        wrapMode: Text.WordWrap
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textMuted
    }
}
