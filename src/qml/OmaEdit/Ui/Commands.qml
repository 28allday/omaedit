import QtQuick
pragma Singleton

QtObject {
    property var registry: null
    property var openPopups: []
    readonly property bool popupOpen: openPopups.length > 0
    // A window open beside the picture, where Space still plays (the effects window).
    property bool transportInPopup: false
    function popupVisibility(menu, shown) {
        const menus = openPopups.filter(function(item) { return item !== menu })
        if (shown) menus.push(menu)
        openPopups = menus
    }

    function command(id) {
        const rev = registry ? registry.revision : 0;
        return registry ? registry.command(id) : null;
    }

    function trigger(id) {
        return registry ? registry.trigger(id) : false;
    }

    function hint(id) {
        const rev = registry ? registry.bindingRevision : 0;
        return registry ? registry.hint(id) : "";
    }

    function hintForDefault(key) {
        const rev = registry ? registry.bindingRevision : 0;
        return registry ? registry.hintForDefault(key) : key;
    }

    function title(id, fallback) {
        const c = command(id);
        return c ? c.text : fallback;
    }

}
