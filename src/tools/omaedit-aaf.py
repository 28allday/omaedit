#!/usr/bin/env python3
"""OmaEdit's AAF helper: the editor's timeline JSON <-> an AAF composition, on pyaaf2.

    omaedit-aaf.py write timeline.json out.aaf
    omaedit-aaf.py read in.aaf timeline.json

Carried: video/audio tracks, clips (by file URL), gaps, cross dissolves, clip
names, source start timecode, track names, sequence markers (as a comment).
Not carried: speed changes, per-clip markers, editor extras — the JSON keeps
`notes` saying so.
"""
import json, os, sys, urllib.parse
from fractions import Fraction

# AAF SDK AAFOperationDefs.h / AAFParameterDefs.h identifiers.
VIDEO_DISSOLVE = "0c3bea40-fc05-11d2-8a29-0050040ef7d2"
MONO_AUDIO_DISSOLVE = "0c3bea41-fc05-11d2-8a29-0050040ef7d2"
VIDEO_SPEED_CONTROL = "9d2ea890-0968-11d3-8a38-0050040ef7d2"
SPEED_RATIO = "72559a80-24d7-11d3-8a50-0050040ef7d2"

try:
    import aaf2
except ImportError:
    sys.stderr.write("pyaaf2 is not installed (python-aaf2)\n")
    sys.exit(3)


def file_url(path):
    return "file://" + urllib.parse.quote(os.path.abspath(path))


def write(json_path, aaf_path):
    doc = json.load(open(json_path))
    rate = Fraction(int(doc.get("fpsNum", 25)), int(doc.get("fpsDen", 1)))
    notes = list(doc.get("notes", []))
    with aaf2.open(aaf_path, "w") as f:
        # A fresh pyaaf2 file has no operation definitions: register the Edit
        # Protocol dissolves so other tools recognise the transitions.
        def dissolve_def(media_kind):
            name, auid = ("VideoDissolve", VIDEO_DISSOLVE) if media_kind == "picture" else ("MonoAudioDissolve", MONO_AUDIO_DISSOLVE)
            try:
                return f.dictionary.lookup_operationdef(name)
            except Exception:
                d = f.create.OperationDef(aaf2.auid.AUID(auid), name, "Cross dissolve")
                d.media_kind = media_kind
                d["NumberInputs"].value = 2
                d["IsTimeWarp"].value = False
                d["Bypass"].value = 0
                f.dictionary.register_def(d)
                return d

        def speed_defs():
            try:
                return f.dictionary.lookup_operationdef("VideoSpeedControl"), f.dictionary.lookup_parameterdef("SpeedRatio")
            except Exception:
                d = f.create.OperationDef(aaf2.auid.AUID(VIDEO_SPEED_CONTROL), "VideoSpeedControl", "Constant speed change")
                d.media_kind = "picture"
                d["NumberInputs"].value = 1
                d["IsTimeWarp"].value = True
                d["Bypass"].value = 1
                f.dictionary.register_def(d)
                p = f.create.ParameterDef(aaf2.auid.AUID(SPEED_RATIO), "SpeedRatio", "Output speed as a ratio of input speed", "Rational")
                f.dictionary.register_def(p)
                return d, p

        comp = f.create.CompositionMob(doc.get("name") or "Sequence")
        comp.usage = "Usage_TopLevel"
        f.content.mobs.append(comp)
        masters = {}

        def master_for(item, media_kind):
            key = (item["path"], media_kind)
            if key in masters:
                return masters[key]
            src = f.create.SourceMob()
            f.content.mobs.append(src)
            descriptor = f.create.ImportDescriptor()
            loc = f.create.NetworkLocator()
            loc["URLString"].value = file_url(item["path"])
            descriptor["Locator"].append(loc)
            src.descriptor = descriptor
            length = int(item.get("sourceLength") or 0) or int(item["sourceIn"]) + int(item["duration"]) + 1
            tc = int(item.get("sourceTimecodeFrames", -1))
            src_slot = src.create_timeline_slot(rate)
            src_slot.segment = f.create.SourceClip(media_kind=media_kind, length=length)
            if tc >= 0:
                tc_slot = src.create_timeline_slot(rate)
                timecode = f.create.Timecode(int(round(float(rate))), drop=False, length=length)
                timecode.start = tc
                tc_slot.segment = timecode
            master = f.create.MasterMob(item.get("name") or os.path.basename(item["path"]))
            f.content.mobs.append(master)
            master_slot = master.create_timeline_slot(rate)
            master_slot.segment = src.create_source_clip(src_slot.slot_id, media_kind=media_kind)
            master_slot.segment.length = length
            masters[key] = (master, master_slot.slot_id)
            return masters[key]

        for track in doc.get("tracks", []):
            media_kind = "sound" if track.get("kind") == "audio" else "picture"
            seq = f.create.Sequence(media_kind=media_kind)
            slot = comp.create_timeline_slot(rate)
            slot.name = track.get("name") or ""
            slot.segment = seq
            cursor = 0
            pending = 0
            for item in track.get("items", []):
                start, duration = int(item["start"]), int(item["duration"])
                if start > cursor:
                    seq.components.append(f.create.Filler(media_kind, start - cursor))
                overlap = int(item.get("transitionOut") or 0)
                if item.get("kind") == "gap" or not item.get("path"):
                    seq.components.append(f.create.Filler(media_kind, duration))
                    if item.get("generator"):
                        notes.append("%s: %s generator written as filler" % (item.get("name", "?"), item["generator"]))
                else:
                    master, slot_id = master_for(item, media_kind)
                    speed = float(item.get("speed", 1.0)) or 1.0
                    if abs(speed - 1.0) > 1e-6:
                        # Edit Protocol speed change: the group runs `duration` on the timeline over
                        # `duration * |speed|` frames of source. Reverse is carried as the sign.
                        ratio = Fraction(speed).limit_denominator(100000)
                        consumed = max(1, int(round((duration + pending) * abs(speed))))
                        clip = master.create_source_clip(slot_id, start=int(round((int(item["sourceIn"]) - pending) * abs(speed))), length=consumed, media_kind=media_kind)
                        op_def, param_def = speed_defs()
                        group = f.create.OperationGroup(op_def, length=duration + pending, media_kind=media_kind)
                        group.segments.append(clip)
                        group.parameters.append(f.create.ConstantValue(param_def, ratio))
                        seq.components.append(group)
                    else:
                        clip = master.create_source_clip(slot_id, start=int(item["sourceIn"]) - pending, length=duration + pending, media_kind=media_kind)
                        seq.components.append(clip)
                    if item.get("markers"):
                        notes.append("%s: clip markers not written" % item.get("name", "?"))
                if overlap > 0:
                    try:
                        op_def = dissolve_def(media_kind)  # before f.create.*: the helper remembers the last name it was asked for
                        trans = f.create.Transition(media_kind, overlap)
                        op = f.create.OperationGroup(op_def, length=overlap)
                        trans["OperationGroup"].value = op
                        trans.cutpoint = overlap
                        seq.components.append(trans)
                    except Exception as exc:  # the definition is missing in this pyaaf2
                        if os.environ.get("OMAEDIT_AAF_DEBUG"):
                            raise
                        notes.append("%s: dissolve of %d frames written as a cut (%s)" % (item.get("name", "?"), overlap, exc))
                        overlap = 0
                cursor = start + duration
                pending = overlap
        if doc.get("markers"):
            comp.comments["OmaEditMarkers"] = json.dumps(doc["markers"])
    json.dump({"notes": notes}, open(json_path + ".notes", "w"))


def read(aaf_path, json_path):
    doc = {"name": "", "fpsNum": 25, "fpsDen": 1, "width": 1920, "height": 1080, "startTimecodeFrames": 0, "tracks": [], "markers": [], "notes": []}
    with aaf2.open(aaf_path, "r") as f:
        comps = list(f.content.toplevel()) or [m for m in f.content.compositionmobs()]
        if not comps:
            sys.stderr.write("no composition in the AAF\n"); sys.exit(4)
        comp = comps[0]
        doc["name"] = comp.name or ""
        try:
            markers = comp.comments.get("OmaEditMarkers")
            if markers:
                doc["markers"] = json.loads(markers.value if hasattr(markers, "value") else markers)
        except Exception:
            pass

        def locator_path(mob):
            try:
                for loc in mob.descriptor["Locator"].value:
                    url = loc["URLString"].value
                    if url.startswith("file://"):
                        return urllib.parse.unquote(url[7:])
                    return url
            except Exception:
                return ""
            return ""

        def resolve(clip):
            """Follow a source clip down to the file and its start timecode; the name is the master mob's."""
            mob = clip.mob
            offset = clip.start
            depth = 0
            top_name = mob.name if mob is not None else ""
            while mob is not None and depth < 8:
                depth += 1
                if isinstance(mob, aaf2.mobs.SourceMob):
                    path = locator_path(mob)
                    tc = -1
                    for s in mob.slots:
                        seg = s.segment
                        if isinstance(seg, aaf2.components.Timecode):
                            tc = int(seg.start)
                    return path, offset, tc, top_name or mob.name
                nxt = None
                try:
                    slot = mob.slot_at(clip.slot_id)
                    seg = slot.segment
                    if isinstance(seg, aaf2.components.SourceClip):
                        nxt = seg
                    elif isinstance(seg, aaf2.components.Sequence):
                        for c in seg.components:
                            if isinstance(c, aaf2.components.SourceClip):
                                nxt = c; break
                except Exception:
                    nxt = None
                if nxt is None:
                    return "", offset, -1, top_name or mob.name
                offset += int(nxt.start)
                clip, mob = nxt, nxt.mob
            return "", offset, -1, ""

        seen_rate = False
        for slot in comp.slots:
            seg = slot.segment
            if not isinstance(seg, aaf2.components.Sequence):
                continue
            rate = Fraction(slot.edit_rate)
            if not seen_rate:
                doc["fpsNum"], doc["fpsDen"] = rate.numerator, rate.denominator; seen_rate = True
            kind = "audio" if str(seg.media_kind).lower() == "sound" else "video"
            track = {"kind": kind, "name": slot.name or "", "locked": False, "muted": False, "disabled": False, "items": []}
            cursor = 0
            pending_in = 0
            for comp_item in seg.components:
                if isinstance(comp_item, aaf2.components.Transition):
                    # An AAF transition overlaps the last `length` frames of the outgoing
                    # segment and the first `length` of the incoming one. The editor keeps
                    # the whole overlap on the outgoing side, so the incoming clip's visible
                    # range starts `length` frames in; the cut point does not change that.
                    length = int(comp_item.length)
                    if track["items"]:
                        track["items"][-1]["transitionOut"] = length
                    pending_in = length
                    cursor -= length
                    continue
                length = int(comp_item.length)
                item = {"kind": "gap", "name": "", "path": "", "start": cursor + pending_in, "duration": length - pending_in, "sourceIn": 0,
                        "sourceLength": 0, "speed": 1.0, "transitionOut": 0, "enabled": True, "audioOnly": kind == "audio", "videoOnly": False,
                        "sourceTimecodeFrames": -1, "generator": "", "markers": [], "extras": {}}
                target = comp_item
                speed = 1.0
                if isinstance(comp_item, aaf2.components.OperationGroup):
                    inputs = list(comp_item.segments) if hasattr(comp_item, "segments") else []
                    target = inputs[0] if inputs else comp_item
                    try:
                        op_auid = str(comp_item["Operation"].value.auid).lower()
                    except Exception:
                        op_auid = ""
                    if op_auid == VIDEO_SPEED_CONTROL:
                        for p in comp_item.parameters:
                            try:
                                if str(p.parameterdef.auid).lower() == SPEED_RATIO:
                                    speed = float(Fraction(p.value)) or 1.0
                            except Exception:
                                pass
                    else:
                        doc["notes"].append("effect on %s read as its first input" % (slot.name or "track"))
                if isinstance(target, aaf2.components.SourceClip):
                    path, offset, tc, name = resolve(target)
                    if path:
                        source_in = int(round(int(offset) / abs(speed))) if abs(speed - 1.0) > 1e-6 else int(offset)
                        item.update(kind="clip", path=path, name=name or os.path.basename(path), sourceIn=source_in + pending_in, sourceTimecodeFrames=tc, speed=speed)
                        try:
                            item["sourceLength"] = int(target.mob.slot_at(target.slot_id).segment.length)
                        except Exception:
                            pass
                if item["duration"] > 0 and item["kind"] == "clip":  # filler is implied by the next start
                    track["items"].append(item)
                cursor += length
                pending_in = 0
            doc["tracks"].append(track)
    doc["tracks"] = [t for t in doc["tracks"] if t["kind"] == "video"] + [t for t in doc["tracks"] if t["kind"] == "audio"]
    json.dump(doc, open(json_path, "w"))


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] not in ("write", "read"):
        sys.stderr.write(__doc__); sys.exit(2)
    (write if sys.argv[1] == "write" else read)(sys.argv[2], sys.argv[3])
