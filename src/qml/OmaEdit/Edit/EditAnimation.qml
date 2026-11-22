import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

Column {
    id: root
    component NumberRow: PropertyRow {
        property real value: 0
        property real from: -999999
        property real to: 999999
        property int decimals: 0
        signal valueEdited(real value)
        animatable: false
        fields: [
            {
                value: value,
                decimals: decimals,
                from: from,
                to: to
            }
        ]
        onFieldEdited: function (i, v) {
            valueEdited(Math.max(from, Math.min(to, v)));
        }
    }
    property var clipData: null
    property int currentFrame: 0
    property var copiedKeys: null
    property var catalogue: []       // the video effects' definitions, for their number controls
    signal keysEdited(var keys)
    signal effectEdited(string uid, var values)
    signal opacityKeysEdited(var keys)
    signal frameRequested(int frame)
    signal propertyReset(string key, real value)
    spacing: Theme.s1
    // The number controls of the clip's video effects, after the clip's own.
    readonly property var effectParameters: {
        const out = [], stack = clipData && clipData.videoEffects ? clipData.videoEffects : []
        stack.forEach(function(state) {
            const spec = root.catalogue.find(function(e) { return e.id === state.id })
            if (!spec) return
            const same = stack.filter(function(s) { return s.id === state.id })
            const title = same.length > 1 ? qsTr("%1 %2").arg(spec.name).arg(same.indexOf(state) + 1) : spec.name
            ;(spec.params || []).forEach(function(p) {
                if (p.type !== "number") return
                out.push({key: "fx:" + state.uid + ":" + p.key, name: qsTr("%1 · %2").arg(title).arg(p.label),
                          value: p.default, effect: state.uid, param: p.key, min: p.min, max: p.max, whole: p.step === 1})
            })
            // Its mask's fields too (fractions of the frame; rotation in degrees).
            if (state.mask) {
                const fields = [{f: "x", n: qsTr("Mask centre X"), v: .5, lo: -1, hi: 2}, {f: "y", n: qsTr("Mask centre Y"), v: .5, lo: -1, hi: 2},
                                {f: "w", n: qsTr("Mask width"), v: .5, lo: .001, hi: 4}, {f: "h", n: qsTr("Mask height"), v: .5, lo: .001, hi: 4},
                                {f: "rotation", n: qsTr("Mask rotation"), v: 0, lo: -360, hi: 360}, {f: "feather", n: qsTr("Mask feather"), v: 0, lo: 0, hi: .5}]
                fields.forEach(function(m) {
                    if (state.mask.shape === "polygon" && m.f !== "feather") return
                    out.push({key: "mask:" + state.uid + ":" + m.f, name: qsTr("%1 · %2").arg(title).arg(m.n),
                              value: m.v, effect: state.uid, maskField: m.f, min: m.lo, max: m.hi, whole: false})
                })
            }
        })
        return out
    }
    readonly property var parameters: clipParameters.concat(effectParameters)
    // The list's names change only when the properties do, so an edit's echo
    // leaves the choice where it is; a changed list keeps the same property chosen.
    property var parameterNames: []
    property string chosenKey: "rectX"
    onParametersChanged: {
        const names = parameters.map(function(p) { return p.name })
        if (JSON.stringify(names) === JSON.stringify(parameterNames)) return
        const keep = chosenKey
        parameterNames = names
        const at = parameters.findIndex(function(p) { return p.key === keep })
        parameterBox.currentIndex = Math.max(0, at)
    }
    readonly property var effectState: parameter.effect && clipData && clipData.videoEffects
        ? clipData.videoEffects.find(function(s) { return s.uid === parameter.effect }) || null : null
    readonly property var clipParameters: [
        {
            key: "rectX",
            name: qsTr("Position X"),
            value: 0
        },
        {
            key: "rectY",
            name: qsTr("Position Y"),
            value: 0
        },
        {
            key: "rectW",
            name: qsTr("Width"),
            value: 1
        },
        {
            key: "rectH",
            name: qsTr("Height"),
            value: 1
        },
        {
            key: "rotation",
            name: qsTr("Rotation"),
            value: 0
        },
        {
            key: "anchorX",
            name: qsTr("Anchor X"),
            value: .5
        },
        {
            key: "anchorY",
            name: qsTr("Anchor Y"),
            value: .5
        },
        {
            key: "cropL",
            name: qsTr("Crop left"),
            value: 0
        },
        {
            key: "cropR",
            name: qsTr("Crop right"),
            value: 0
        },
        {
            key: "cropT",
            name: qsTr("Crop top"),
            value: 0
        },
        {
            key: "cropB",
            name: qsTr("Crop bottom"),
            value: 0
        },
        {
            key: "cropFeather",
            name: qsTr("Crop feather"),
            value: 0
        },
        {
            key: "colorB",
            name: qsTr("Brightness"),
            value: 0
        },
        {
            key: "colorC",
            name: qsTr("Contrast"),
            value: 1
        },
        {
            key: "colorS",
            name: qsTr("Saturation"),
            value: 1
        },
        { key: "opacity", name: qsTr("Opacity"), value: 1 }
    ]
    readonly property var parameter: parameters[parameterBox.currentIndex] || parameters[0]
    readonly property var allKeys: clipData && clipData.editKeys ? clipData.editKeys : ({})
    readonly property var keys: parameter.key === "opacity"
        ? (clipData && clipData.opacityKf ? clipData.opacityKf.map(function(k) {
            return {frame:k.frame, value:k.alpha, interpolation:k.interpolation || "linear"}
        }) : []) : parameter.maskField ? (effectState && effectState.maskKeys && effectState.maskKeys[parameter.maskField] || [])
        : parameter.effect ? (effectState && effectState.keys && effectState.keys[parameter.param] || [])
        : allKeys[parameter.key] || []
    readonly property real minimum: parameter.effect ? parameter.min : parameter.key === "rotation" ? -10000
        : parameter.key === "rectX" || parameter.key === "rectY" ? -10
        : parameter.key === "rectW" || parameter.key === "rectH" ? .001
        : parameter.key === "colorB" ? -1 : 0
    readonly property real maximum: parameter.effect ? parameter.max : parameter.key === "rotation" ? 10000
        : parameter.key.indexOf("rect") === 0 ? 20 : parameter.key === "colorC" ? 2
        : parameter.key === "colorS" ? 3 : parameter.key.indexOf("crop") === 0 && parameter.key !== "cropFeather" ? .95 : 1
    readonly property int frames: clipData && clipData.frames !== undefined ? clipData.frames : 1
    function currentValue(frame) {
        if (frame === undefined)
            frame = currentFrame;
        const base = parameter.maskField
            ? (effectState && effectState.mask && effectState.mask[parameter.maskField] !== undefined ? effectState.mask[parameter.maskField] : parameter.value)
            : parameter.effect
            ? (effectState && effectState.params && effectState.params[parameter.param] !== undefined ? effectState.params[parameter.param] : parameter.value)
            : clipData && clipData[parameter.key] !== undefined ? clipData[parameter.key] : parameter.value;
        if (!keys.length)
            return base;
        if (frame <= keys[0].frame)
            return keys[0].value;
        for (let i = 1; i < keys.length; ++i) {
            const a = keys[i - 1], b = keys[i];
            if (frame < b.frame) {
                if (a.interpolation === "hold")
                    return a.value;
                let u = (frame - a.frame) / (b.frame - a.frame);
                if (a.interpolation === "smooth")
                    u = u * u * (3 - 2 * u);
                return a.value + (b.value - a.value) * u;
            }
        }
        return keys[keys.length - 1].value;
    }
    // An effect's control: its keys replaced, or ended with a value left set.
    function effectCommit(list, value) {
        const keys = {}, values = {}
        list.sort(function (a, b) { return a.frame - b.frame })
        if (parameter.maskField) {
            // A mask's field: its keys, or with the last one gone the value left set in the mask.
            keys[parameter.maskField] = list
            values.maskKeys = keys
            if (!list.length && effectState && effectState.mask) {
                const mask = JSON.parse(JSON.stringify(effectState.mask))
                mask[parameter.maskField] = value
                values.mask = mask
            }
            effectEdited(parameter.effect, values)
            return
        }
        keys[parameter.param] = list
        values.keys = keys
        if (!list.length) {
            const params = {}
            params[parameter.param] = parameter.whole ? Math.round(value) : value
            values.params = params
        }
        effectEdited(parameter.effect, values)
    }
    function resetProperty() {
        if (parameter.effect) effectCommit([], parameter.value)
        else propertyReset(parameter.key, parameter.value)
    }
    function commit(list) {
        if (parameter.effect) {
            effectCommit(list, currentValue())
            return
        }
        if (!list.length) {
            propertyReset(parameter.key, currentValue());
            return;
        }
        const out = JSON.parse(JSON.stringify(allKeys));
        list.sort(function (a, b) {
            return a.frame - b.frame;
        });
        if (list.length)
            out[parameter.key] = list;
        else
            delete out[parameter.key];
        if (parameter.key === "opacity")
            opacityKeysEdited(list.map(function(k) {return {frame:k.frame,alpha:k.value,interpolation:k.interpolation || "linear"}}));
        else keysEdited(out);
    }
    function put(frame, value, mode) {
        const previous = keys.filter(function(k) { return k.frame <= frame; });
        const list = keys.filter(function (k) {
            return k.frame !== frame;
        });
        list.push({
            frame: frame,
            value: value,
            interpolation: mode || (previous.length ? previous[previous.length - 1].interpolation : "linear")
        });
        commit(list);
    }
    ComboField {
        id: parameterBox
        objectName: "animationParameter"
        tip: qsTr("Which clip property the keyframes below animate")
        width: parent.width
        model: root.parameterNames
        onCurrentIndexChanged: root.chosenKey = (root.parameters[currentIndex] || root.parameters[0]).key
    }
    Text {
        width: parent.width
        text: qsTr("Clip frame %1 · %2 keyframes. Position, size and crop use fractions of the frame.").arg(root.currentFrame).arg(root.keys.length)
        wrapMode: Text.WordWrap
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textMuted
    }
    NumberRow {
        objectName: "animationValue"
        width: parent.width
        label: root.parameter.name
        value: root.currentValue()
        from: root.minimum
        to: root.maximum
        decimals: 3
        onValueEdited: function (value) {
            root.put(root.currentFrame, value);
        }
    }
    Row {
        spacing: Theme.s1
        IconButton {
            objectName: "animationPrevious"
            iconName: "chevron-left"
            text: qsTr("Previous keyframe")
            onClicked: {
                const list = root.keys.filter(function (k) {
                    return k.frame < root.currentFrame && k.frame >= 0;
                });
                if (list.length)
                    root.frameRequested(list[list.length - 1].frame);
            }
        }
        IconButton {
            objectName: "animationAdd"
            iconName: "diamond"
            text: qsTr("Add keyframe at playhead")
            onClicked: root.put(root.currentFrame, root.currentValue())
        }
        IconButton {
            objectName: "animationNext"
            iconName: "chevron-right"
            text: qsTr("Next keyframe")
            onClicked: {
                const list = root.keys.filter(function (k) {
                    return k.frame > root.currentFrame && k.frame < root.frames;
                });
                if (list.length)
                    root.frameRequested(list[0].frame);
            }
        }
        IconButton {
            objectName: "animationCopy"
            iconName: "copy"
            text: qsTr("Copy this property's keyframes")
            enabled: root.keys.length > 0
            onClicked: root.copiedKeys = JSON.parse(JSON.stringify(root.keys))
        }
        IconButton {
            objectName: "animationPaste"
            iconName: "clipboard"
            text: qsTr("Paste keyframes at playhead")
            enabled: root.copiedKeys !== null
            // The pasted keys replace this property's keys over the stretch
            // they cover; its keys before and after stay, as Edit ▸
            // Keyframes ▸ Paste does. Keys past the clip's end are left off.
            onClicked: {
                const offset = root.currentFrame - root.copiedKeys[0].frame;
                const pasted = root.copiedKeys.map(function (k) {
                    return {
                        frame: k.frame + offset,
                        value: k.value,
                        interpolation: k.interpolation
                    };
                }).filter(function (k) { return k.frame < root.frames; });
                if (!pasted.length)
                    return;
                const from = pasted[0].frame, to = pasted[pasted.length - 1].frame;
                root.commit(root.keys.filter(function (k) {
                    return k.frame < from || k.frame > to;
                }).concat(pasted));
            }
        }
        IconButton {
            objectName: "animationResetProperty"
            iconName: "rotate-ccw"
            text: qsTr("Reset this property")
            onClicked: root.resetProperty()
        }
        IconButton {
            objectName: "animationClear"
            iconName: "x"
            text: qsTr("Clear this property's animation")
            enabled: root.keys.length > 0
            onClicked: root.commit([])
        }
    }
    Canvas {
        id: curve
        objectName: "editAnimationCurve"
        Connections { target: Theme; function onAccentChanged() { curve.requestPaint() } } // the desktop accent can change while this is on screen
        width: parent.width
        height: 64
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            ctx.fillStyle = Theme.windowBg;
            ctx.fillRect(0, 0, width, height);
            if (!root.keys.length)
                return;
            const values = root.keys.map(function (k) {
                return k.value;
            }), lo = Math.min.apply(null, values), hi = Math.max.apply(null, values), range = Math.max(.01, hi - lo);
            ctx.strokeStyle = Theme.accent;
            ctx.beginPath();
            for (let x = 0; x <= width; ++x) {
                const value = root.currentValue(x / Math.max(1, width) * (root.frames - 1)), y = height - 8 - (value - lo) / range * (height - 16);
                if (x === 0)
                    ctx.moveTo(x, y);
                else
                    ctx.lineTo(x, y);
            }
            ctx.stroke();
            ctx.fillStyle = Theme.accent;
            root.keys.forEach(function (k) {
                ctx.beginPath();
                ctx.arc(k.frame / Math.max(1, root.frames - 1) * width, height - 8 - (k.value - lo) / range * (height - 16), 3, 0, Math.PI * 2);
                ctx.fill();
            });
            ctx.strokeStyle = Theme.textPrimary;
            ctx.beginPath();
            const x = root.currentFrame / Math.max(1, root.frames - 1) * width;
            ctx.moveTo(x, 0);
            ctx.lineTo(x, height);
            ctx.stroke();
        }
        Connections {
            target: root
            function onKeysChanged() {
                curve.requestPaint();
            }
            function onCurrentFrameChanged() {
                curve.requestPaint();
            }
            function onFramesChanged() {
                curve.requestPaint();
            }
        }
    }
    Repeater {
        // Keep fields and pointer grabs while the worker echoes edited values.
        model: root.keys.length
        delegate: Column {
            id: keyRow
            required property int index
            readonly property var modelData: root.keys[index]
            width: root.width
            Row {
                width: parent.width
                spacing: Theme.s1
                ValueField {
                    id: frameValue
                    objectName: "animationKeyFrame:" + index
                    tip: qsTr("Frame within the clip where this keyframe sits")
                    width: 100
                    from: -100000000
                    to: 100000000
                    decimals: 0
                    label: qsTr("Keyframe frame")
                    suffix: qsTr("fr")
                    value: modelData.frame
                    Connections {
                        target: keyRow
                        function onModelDataChanged() { frameValue.value = keyRow.modelData.frame }
                    }
                    // A scrub moves the key once, at its end (one undo step);
                    // it is followed by its original frame, not its row, and
                    // keys it passes over on the way stay where they are.
                    property var origin: null
                    property var pending: undefined
                    function moveKey(key, frame) {
                        const list = JSON.parse(JSON.stringify(root.keys)).filter(function (k) {
                            return k.frame !== key.frame && k.frame !== frame;
                        });
                        list.push({ frame: frame, value: key.value, interpolation: key.interpolation });
                        root.commit(list);
                    }
                    onEditStarted: {
                        origin = { frame: modelData.frame, value: modelData.value, interpolation: modelData.interpolation };
                        pending = undefined;
                    }
                    onEdited: function (value) {
                        if (scrubbing) { pending = Math.round(value); return; }
                        moveKey(modelData, Math.round(value));
                    }
                    onEditFinished: {
                        if (pending !== undefined && origin && pending !== origin.frame) moveKey(origin, pending);
                        pending = undefined; origin = null;
                    }
                }
                ComboField {
                    objectName: "animationInterpolation:" + index
                    tip: qsTr("How the value travels out of this keyframe: even, stepped or eased")
                    width: Math.max(80, parent.width - 100 - Theme.hControl - 2 * Theme.s1)
                    model: [qsTr("Linear"), qsTr("Hold"), qsTr("Smooth")]
                    currentIndex: Math.max(0, ["linear", "hold", "smooth"].indexOf(modelData.interpolation))
                    onActivated: root.put(modelData.frame, modelData.value, ["linear", "hold", "smooth"][currentIndex])
                }
                IconButton {
                    iconName: "x"
                    text: qsTr("Delete keyframe")
                    onClicked: root.commit(root.keys.filter(function (k, i) {
                        return i !== index;
                    }))
                }
            }
            NumberRow {
                width: parent.width
                label: qsTr("Value")
                value: modelData.value
                from: root.minimum
                to: root.maximum
                decimals: 3
                onValueEdited: function (v) {
                    root.put(modelData.frame, v, modelData.interpolation);
                }
            }
        }
    }
}
