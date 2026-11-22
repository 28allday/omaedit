import QtQml
import OmaEdit.Ui as Ui

// Where the tracks sit on screen: each track's height, the y of a lane and
// the lane under a y, and which row a video track is drawn on. The window
// holds one as `win.lanes`.
QtObject {
    id: root
    // The window: selection, entries and the other commands.
    required property var win

    function setTrackHeight(h) {
        win.hLaneRow = Math.max(win.hLaneRowMin, Math.min(win.hLaneRowMax, Math.round(h)))
        // Sizing them all brings back any track sized on its own.
        for (let t=0;t<backend.tracks.length;++t)
            if (backend.tracks[t].mix && backend.tracks[t].mix.editHeight)
                backend.setTrackMix(t,"editHeight",0)
    }
    function trackHeightFor(track) {
        const info=backend.tracks[track],mix=info && info.mix ? info.mix : {}
        return mix.editHeight || win.hLaneRow
    }
    function setOneTrackHeight(track,h) {backend.setTrackMix(track,"editHeight",Math.max(win.hLaneRowMin,Math.min(200,Math.round(h))))}
    function laneY(track,audio) {
        const list=audio ? win.laneKinds.audio : win.laneKinds.video.slice().reverse()
        let y=0;for(let i=0;i<list.length;++i){if(list[i]===track)return y;y+=trackHeightFor(list[i])}return y
    }
    function laneHeight(audio) {return laneY(-1,audio)}
    function laneTrackAt(y,audio) {
        const list=audio ? win.laneKinds.audio : win.laneKinds.video.slice().reverse()
        let top=0;for(let i=0;i<list.length;++i){top+=trackHeightFor(list[i]);if(y>=0 && y<top)return list[i]}return -1
    }
    function laneDragOffset(track,audio,rows) {
        const list=audio?win.laneKinds.audio:win.laneKinds.video,at=list.indexOf(track),wanted=at+(audio?rows:-rows)
        // Below the lowest video lane is V1: the storyline row, past the
        // strip's bottom and the column's spacing.
        if (!audio && wanted < 0)
            return laneHeight(false) + 4 - laneY(track, false)
        const target=Math.max(0,Math.min(list.length-1,wanted))
        return laneY(list[target],audio)-laneY(track,audio)
    }
    // Video rows counted up from V1: V1 is row 0, the lowest video lane 1;
    // one past the top is a new track.
    function videoRowOf(track) { return track === 0 ? 0 : win.laneKinds.video.indexOf(track) + 1 }
    function videoTrackAtRow(row) {
        return row <= 0 ? 0 : row <= win.laneKinds.video.length ? win.laneKinds.video[row - 1] : backend.tracks.length
    }
    function tallerTracks() { setTrackHeight(win.hLaneRow + 8) }
    function shorterTracks() { setTrackHeight(win.hLaneRow - 8) }
}
